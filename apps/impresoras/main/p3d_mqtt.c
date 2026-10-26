/*
 * P4OS - 3D printers: a small MQTT 3.1.1 client, for Bambu Lab's LAN mode.
 *
 * The system's MQTT (aos_mqtt) is the user's broker and there is one of it;
 * a Bambu printer is a broker of its own, one per machine, so the driver
 * keeps a connection of its own over the HAL's TLS. QoS 0 only, which is
 * what the printer uses: CONNECT, SUBSCRIBE, PUBLISH both ways, PINGREQ.
 *
 * A report from an X1 is a whole status of 10-20 KB; the buffer grows up
 * to 128 KB (PSRAM) and a message bigger than that is skipped.
 */
#include "p3d.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>

#define KEEPALIVE_S  60
#define PING_MS      30000
#define BUF_START    (16 * 1024)
#define BUF_MAX      (128 * 1024)

struct p3d_mqtt {
    void    *s;
    uint8_t *buf;
    int      cap, len;          /* bytes in buf */
    int      skip;              /* bytes of a too-big message still to drop */
    uint64_t last_tx;
    uint16_t pid;
    char    *msg;               /* the last message handed out: topic\0payload\0 */
};

static int put_len(uint8_t *p, int n)
{
    int k = 0;
    do {
        uint8_t b = n % 128;
        n /= 128;
        if (n) b |= 0x80;
        p[k++] = b;
    } while (n && k < 4);
    return k;
}

static int put_str(uint8_t *p, const char *s)
{
    int n = (int)strlen(s);
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
    memcpy(p + 2, s, (size_t)n);
    return n + 2;
}

static bool send_pkt(p3d_mqtt_t *q, uint8_t type, const uint8_t *body, int n)
{
    uint8_t *pkt = p3d_psram((size_t)n + 5);
    if (!pkt) return false;
    pkt[0] = type;
    int h = 1 + put_len(pkt + 1, n);
    memcpy(pkt + h, body, (size_t)n);
    bool ok = p3d_sock_send(q->s, pkt, h + n, 5000);
    free(pkt);
    if (ok) q->last_tx = aos_hal_uptime_ms();
    return ok;
}

/* a whole packet in buf: its type, body offset and length; false if not yet */
static bool have_pkt(p3d_mqtt_t *q, uint8_t *type, int *off, int *len)
{
    if (q->len < 2) return false;
    int n = 0, mul = 1, i = 1;
    for (; i < 5 && i < q->len; i++) {
        n += (q->buf[i] & 0x7F) * mul;
        mul *= 128;
        if (!(q->buf[i] & 0x80)) break;
    }
    if (i >= 5) return false;
    if (i >= q->len) return false;
    *type = q->buf[0];
    *off = i + 1;
    *len = n;
    return q->len >= *off + n;
}

static void drop(p3d_mqtt_t *q, int n)
{
    if (n >= q->len) {
        q->len = 0;
        return;
    }
    memmove(q->buf, q->buf + n, (size_t)(q->len - n));
    q->len -= n;
}

/* reads what is there into buf; false: the connection is gone */
static bool fill(p3d_mqtt_t *q, int timeout_ms)
{
    if (q->len >= q->cap) {
        /* the header says how big: grow, or drop the message as it arrives */
        uint8_t t;
        int off = 0, len = 0;
        have_pkt(q, &t, &off, &len);
        int need = q->len + 4096;
        if (q->cap < BUF_MAX) {
            int nc = q->cap * 2 > BUF_MAX ? BUF_MAX : q->cap * 2;
            if (nc < need && need <= BUF_MAX) nc = need;
            uint8_t *nb = p3d_psram((size_t)nc);
            if (nb) {
                memcpy(nb, q->buf, (size_t)q->len);
                free(q->buf);
                q->buf = nb;
                q->cap = nc;
            }
        }
        if (q->len >= q->cap) {
            aos_hal_log(P3D_TAG, "mqtt: a message of %d bytes skipped", len);
            q->skip = off + len - q->len;
            q->len = 0;
        }
    }
    if (q->skip > 0) {
        uint8_t tmp[1024];
        int n = p3d_sock_recv(q->s, tmp, q->skip < (int)sizeof tmp ? q->skip : (int)sizeof tmp, timeout_ms);
        if (n < 0) return false;
        q->skip -= n;
        return true;
    }
    int n = p3d_sock_recv(q->s, q->buf + q->len, q->cap - q->len, timeout_ms);
    if (n < 0) return false;
    q->len += n;
    return true;
}

p3d_mqtt_t *p3d_mqtt_open(const char *host, int port, bool tls, const p3d_trust_t *trust, const char *client,
                          const char *user, const char *pass, int timeout_ms, char *why, size_t n)
{
    p3d_mqtt_t *q = p3d_psram(sizeof *q);
    if (!q) return NULL;
    memset(q, 0, sizeof *q);
    q->buf = p3d_psram(BUF_START);
    q->cap = BUF_START;
    if (!q->buf) {
        free(q);
        return NULL;
    }
    q->s = trust ? p3d_sock_open_trust(host, port, trust, timeout_ms, why, n)
                 : p3d_sock_open(host, port, tls, timeout_ms, why, n);
    if (!q->s) {
        p3d_mqtt_close(q);
        return NULL;
    }
    uint8_t b[400];
    int k = put_str(b, "MQTT");
    b[k++] = 4;                                       /* 3.1.1 */
    b[k++] = 0x02 | (user && user[0] ? 0x80 : 0) | (pass && pass[0] ? 0x40 : 0);  /* clean session */
    b[k++] = KEEPALIVE_S >> 8;
    b[k++] = KEEPALIVE_S & 0xFF;
    k += put_str(b + k, client);
    if (user && user[0]) k += put_str(b + k, user);
    if (pass && pass[0]) k += put_str(b + k, pass);
    if (!send_pkt(q, 0x10, b, k)) {
        p3d_copy(why, n, "send failed");
        p3d_mqtt_close(q);
        return NULL;
    }
    uint64_t until = aos_hal_uptime_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint8_t t;
        int off, len;
        if (have_pkt(q, &t, &off, &len)) {
            if ((t & 0xF0) == 0x20 && len >= 2) {
                int rc = q->buf[off + 1];
                drop(q, off + len);
                if (rc == 0) return q;
                p3d_copy(why, n, rc == 4 || rc == 5 ? "not authorized" : "refused");
                p3d_mqtt_close(q);
                return NULL;
            }
            drop(q, off + len);
            continue;
        }
        if (aos_hal_uptime_ms() > until || !fill(q, 200)) {
            p3d_copy(why, n, "no CONNACK");
            p3d_mqtt_close(q);
            return NULL;
        }
    }
}

bool p3d_mqtt_sub(p3d_mqtt_t *q, const char *topic)
{
    uint8_t b[200];
    q->pid++;
    b[0] = (uint8_t)(q->pid >> 8);
    b[1] = (uint8_t)q->pid;
    int k = 2 + put_str(b + 2, topic);
    b[k++] = 0;                                       /* QoS 0 */
    return send_pkt(q, 0x82, b, k);
}

bool p3d_mqtt_pub(p3d_mqtt_t *q, const char *topic, const char *payload, int len)
{
    if (len < 0) len = (int)strlen(payload);
    int tl = (int)strlen(topic);
    uint8_t *b = p3d_psram((size_t)(tl + 2 + len));
    if (!b) return false;
    int k = put_str(b, topic);
    memcpy(b + k, payload, (size_t)len);
    bool ok = send_pkt(q, 0x30, b, k + len);
    free(b);
    return ok;
}

int p3d_mqtt_poll(p3d_mqtt_t *q, const char **topic, const char **payload, int timeout_ms)
{
    uint64_t until = aos_hal_uptime_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (aos_hal_uptime_ms() - q->last_tx > PING_MS) {
            if (!send_pkt(q, 0xC0, NULL, 0)) return -1;
        }
        uint8_t t;
        int off, len;
        if (have_pkt(q, &t, &off, &len)) {
            if ((t & 0xF0) == 0x30) {
                const uint8_t *p = q->buf + off;
                int tl = (p[0] << 8) | p[1];
                int head = 2 + tl + ((t & 0x06) ? 2 : 0);    /* a packet id with QoS > 0 */
                if (head > len) {
                    drop(q, off + len);
                    continue;
                }
                int pl = len - head;
                free(q->msg);
                q->msg = p3d_psram((size_t)(tl + pl + 2));
                if (!q->msg) {
                    drop(q, off + len);
                    continue;
                }
                memcpy(q->msg, p + 2, (size_t)tl);
                q->msg[tl] = '\0';
                memcpy(q->msg + tl + 1, p + head, (size_t)pl);
                q->msg[tl + 1 + pl] = '\0';
                drop(q, off + len);
                *topic = q->msg;
                *payload = q->msg + tl + 1;
                return pl > 0 ? pl : 1;
            }
            drop(q, off + len);         /* SUBACK, PINGRESP */
            continue;
        }
        int left = (int)(until - aos_hal_uptime_ms());
        if (left <= 0) return 0;
        if (!fill(q, left < 200 ? left : 200)) return -1;
    }
}

void p3d_mqtt_close(p3d_mqtt_t *q)
{
    if (!q) return;
    if (q->s) {
        static const uint8_t DISC[2] = { 0xE0, 0 };
        p3d_sock_send(q->s, DISC, 2, 500);
        p3d_sock_close(q->s);
    }
    free(q->buf);
    free(q->msg);
    free(q);
}
