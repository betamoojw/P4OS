/*
 * P4OS - 3D printers: the camera of the machine on screen.
 *
 * Three sources, all JPEG, decoded by the HAL (the JPEG block on the board):
 *
 *  - a snapshot URL (OctoPrint's "?action=snapshot", Moonraker's webcams,
 *    Creality's mjpg-streamer): asked again as soon as one is shown;
 *  - an MJPEG stream (multipart/x-mixed-replace, the "?action=stream"
 *    ones, FlashForge's, Elegoo's port 3031): the newest whole frame of what
 *    has arrived, found by its SOI and EOI markers, as Cámaras does;
 *  - "bambu:", the P1 and A1's own stream on port 6000 (TLS): an 80-byte
 *    login with the access code, then each frame behind a 16-byte header
 *    that gives its size. The X1 streams RTSP with H.264 at a profile the
 *    board does not decode.
 *
 * Frames are scaled down by a whole factor to fit the box on screen and
 * kept in PSRAM; the UI copies the newest under the app's lock.
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>

#define STACK       (12 * 1024)
#define BUF_CAP     (768 * 1024)
#define CONNECT_MS  3000
#define SILENCE_MS  8000
#define SNAP_GAP_MS 250

struct p3d_cam {
    char          url[200];
    char          user[32], pass[64];
    char          code[64];         /* Bambu's access code */
    char          serial[48];       /* and its serial: the certificate's name */
    char          host[96];
    int           max_w, max_h;
    volatile bool stop, done;
    uint16_t     *px;
    int           w, h;
    uint32_t      gen;
    char          status[96];
    uint32_t      frames;
    uint64_t      t0;
};

static volatile int s_threads;

int p3d_cam_threads(void)
{
    return s_threads;
}

void p3d_cam_url(const p3d_cfg_t *cfg, const p3d_status_t *st, char *out, size_t n)
{
    if (!strcmp(cfg->camera, "off")) out[0] = '\0';
    else if (cfg->camera[0]) p3d_copy(out, n, cfg->camera);
    else p3d_copy(out, n, st->cam_url);
}

static void set_status(p3d_cam_t *c, const char *s)
{
    p3d_lock();
    p3d_copy(c->status, sizeof c->status, s);
    p3d_unlock();
}

/* decodes one JPEG and puts it, scaled, where the UI takes it */
static void show(p3d_cam_t *c, aos_jpeg_t *dec, const uint8_t *jpg, int len)
{
    int w, h, stride;
    const uint16_t *px = aos_hal_jpeg_decode(dec, jpg, (size_t)len, &w, &h, &stride);
    if (!px || w <= 0 || h <= 0) return;
    int k = 1;
    while ((w / k > c->max_w || h / k > c->max_h) && k < 8) k++;
    int ow = w / k, oh = h / k;
    uint16_t *out = p3d_psram((size_t)ow * oh * 2);
    if (!out) return;
    for (int y = 0; y < oh; y++) {
        const uint16_t *src = px + (size_t)(y * k) * stride;
        uint16_t *dst = out + (size_t)y * ow;
        if (k == 1) memcpy(dst, src, (size_t)ow * 2);
        else for (int x = 0; x < ow; x++) dst[x] = src[x * k];
    }
    p3d_lock();
    free(c->px);
    c->px = out;
    c->w = ow;
    c->h = oh;
    c->gen++;
    c->frames++;
    c->status[0] = '\0';
    p3d_unlock();
}

/* the newest whole JPEG in buf[0..*len): decoded, and what came before it dropped */
static void newest(p3d_cam_t *c, aos_jpeg_t *dec, uint8_t *buf, int *len)
{
    int last_soi = -1, last_eoi = -1;
    for (int i = 0; i + 1 < *len; i++) {
        if (buf[i] != 0xFF) continue;
        if (buf[i + 1] == 0xD8) last_soi = i;
        else if (buf[i + 1] == 0xD9 && last_soi >= 0) last_eoi = i + 2;
    }
    if (last_eoi > 0) {
        /* the last frame that both starts and ends in the buffer */
        int soi = -1;
        for (int i = last_eoi - 2; i >= 0; i--) {
            if (buf[i] == 0xFF && buf[i + 1] == 0xD8) {
                soi = i;
                break;
            }
        }
        if (soi >= 0) show(c, dec, buf + soi, last_eoi - soi);
        memmove(buf, buf + last_eoi, (size_t)(*len - last_eoi));
        *len -= last_eoi;
    } else if (*len > BUF_CAP - 65536) {
        *len = 0;                       /* nothing whole in all of it */
    }
}

static int http_open(p3d_cam_t *c, void **s, char *ctype, size_t cn, uint8_t *buf, int *have, long *clen)
{
    char host[96], path[300];
    int port;
    bool tls;
    if (!p3d_url_split(c->url, host, sizeof host, &port, path, sizeof path, &tls)) return -1;
    char why[96];
    *s = p3d_sock_open(host, port, tls, CONNECT_MS, why, sizeof why);
    if (!*s) return -2;
    char req[700], auth[200] = "";
    if (c->user[0]) {
        char up[100], b64[140];
        snprintf(up, sizeof up, "%s:%s", c->user, c->pass);
        p3d_b64(b64, sizeof b64, up, strlen(up));
        snprintf(auth, sizeof auth, "Authorization: Basic %s\r\n", b64);
    }
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: P4OS\r\nAccept: */*\r\n%sConnection: close\r\n\r\n",
                     path, host, port, auth);
    if (!p3d_sock_send(*s, req, n, 3000)) return -2;
    *have = 0;
    uint64_t until = aos_hal_uptime_ms() + 5000;
    for (;;) {
        if (c->stop || aos_hal_uptime_ms() > until) return -2;
        int k = p3d_sock_recv(*s, buf + *have, 8192 - *have, 200);
        if (k < 0) return -2;
        *have += k;
        buf[*have] = 0;
        char *e = strstr((char *)buf, "\r\n\r\n");
        if (!e) {
            if (*have >= 8192) return -2;
            continue;
        }
        int hl = (int)(e - (char *)buf) + 4;
        int status = atoi((char *)buf + 9);
        ctype[0] = '\0';
        *clen = -1;
        for (char *l = strstr((char *)buf, "\r\n"); l && l < e; l = strstr(l + 2, "\r\n")) {
            if (!strncasecmp(l + 2, "Content-Type:", 13)) {
                const char *v = l + 15;
                while (*v == ' ') v++;
                size_t k2 = strcspn(v, "\r\n;");
                if (k2 >= cn) k2 = cn - 1;
                memcpy(ctype, v, k2);
                ctype[k2] = '\0';
            } else if (!strncasecmp(l + 2, "Content-Length:", 15)) {
                *clen = atol(l + 17);
            }
        }
        memmove(buf, buf + hl, (size_t)(*have - hl));
        *have -= hl;
        return status;
    }
}

static void http_loop(p3d_cam_t *c, aos_jpeg_t *dec, uint8_t *buf)
{
    int fails = 0;
    while (!c->stop) {
        void *s = NULL;
        char ctype[64];
        int len = 0;
        long clen;
        int st = http_open(c, &s, ctype, sizeof ctype, buf, &len, &clen);
        if (st != 200) {
            if (s) p3d_sock_close(s);
            fails++;
            char m[96];
            if (st == -1) p3d_copy(m, sizeof m, _("La dirección de la cámara no sirve"));
            else if (st == 401) p3d_copy(m, sizeof m, _("La cámara pide usuario y contraseña"));
            else if (st < 0) p3d_copy(m, sizeof m, _("La cámara no responde"));
            else snprintf(m, sizeof m, _("La cámara contestó %d"), st);
            set_status(c, m);
            for (int i = 0; i < 20 * (fails > 3 ? 3 : fails) && !c->stop; i++) aos_hal_sleep_ms(100);
            continue;
        }
        fails = 0;
        bool stream = strstr(ctype, "multipart") != NULL;
        uint64_t last = aos_hal_uptime_ms();
        while (!c->stop) {
            int k = p3d_sock_recv(s, buf + len, BUF_CAP - len, 200);
            if (k < 0) break;
            if (k > 0) {
                len += k;
                last = aos_hal_uptime_ms();
                if (stream) newest(c, dec, buf, &len);
                else if (clen >= 0 && len >= clen) break;
            } else if (aos_hal_uptime_ms() - last > SILENCE_MS) {
                break;
            }
            if (len >= BUF_CAP) {
                if (!stream) break;
                len = 0;
            }
        }
        p3d_sock_close(s);
        if (!stream && len > 0) {
            show(c, dec, buf, len);
            for (int i = 0; i < SNAP_GAP_MS / 50 && !c->stop; i++) aos_hal_sleep_ms(50);
        }
    }
}

static void bambu_loop(p3d_cam_t *c, aos_jpeg_t *dec, uint8_t *buf)
{
    int fails = 0;
    while (!c->stop) {
        char why[128] = "";
        p3d_trust_t trust;
        void *s = p3d_bambu_trust(c->serial, &trust) ? p3d_sock_open_trust(c->host, 6000, &trust, 6000, why, sizeof why)
                                                     : NULL;
        if (!s) {
            fails++;
            set_status(c, strstr(why, "certif") || strstr(why, "authority")
                              ? _("La cámara no tiene un certificado de Bambu para este número de serie")
                              : _("La cámara no responde (las X1 usan RTSP, que no se puede ver acá)"));
            for (int i = 0; i < 30 * (fails > 3 ? 3 : fails) && !c->stop; i++) aos_hal_sleep_ms(100);
            continue;
        }
        uint8_t auth[80] = { 0x40, 0, 0, 0, 0, 0x30, 0, 0 };
        memcpy(auth + 16, "bblp", 4);
        size_t cl = strlen(c->code);
        memcpy(auth + 48, c->code, cl > 32 ? 32 : cl);
        if (!p3d_sock_send(s, auth, sizeof auth, 3000)) {
            p3d_sock_close(s);
            continue;
        }
        fails = 0;
        int len = 0;
        uint64_t last = aos_hal_uptime_ms();
        while (!c->stop) {
            int k = p3d_sock_recv(s, buf + len, BUF_CAP - len, 200);
            if (k < 0) break;
            if (k == 0) {
                if (aos_hal_uptime_ms() - last > SILENCE_MS) break;
                continue;
            }
            last = aos_hal_uptime_ms();
            len += k;
            /* whole frames: 16 bytes of header, then that many of JPEG */
            int at = 0;
            int show_at = -1, show_len = 0;
            while (len - at >= 16) {
                uint32_t size = buf[at] | (uint32_t)buf[at + 1] << 8 | (uint32_t)buf[at + 2] << 16 | (uint32_t)buf[at + 3] << 24;
                if (size == 0 || size > BUF_CAP - 16) {
                    at = len;           /* lost the thread: start again */
                    break;
                }
                if (len - at < 16 + (int)size) break;
                show_at = at + 16;
                show_len = (int)size;
                at += 16 + (int)size;
            }
            if (show_at >= 0) show(c, dec, buf + show_at, show_len);
            memmove(buf, buf + at, (size_t)(len - at));
            len -= at;
        }
        p3d_sock_close(s);
    }
}

static void cam_thread(void *arg)
{
    p3d_cam_t *c = arg;
    uint8_t *buf = p3d_psram(BUF_CAP + 1);
    aos_jpeg_t *dec = aos_hal_jpeg_open();
    if (buf && dec) {
        if (!strncmp(c->url, "bambu:", 6)) bambu_loop(c, dec, buf);
        else http_loop(c, dec, buf);
    }
    if (dec) aos_hal_jpeg_close(dec);
    free(buf);
    __sync_synchronize();
    c->done = true;
    __sync_fetch_and_sub(&s_threads, 1);
}

p3d_cam_t *p3d_cam_start(const char *url, const p3d_cfg_t *cfg, int max_w, int max_h)
{
    if (!url || !url[0]) return NULL;
    p3d_cam_t *c = p3d_psram(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, sizeof *c);
    p3d_copy(c->url, sizeof c->url, url);
    p3d_copy(c->host, sizeof c->host, cfg->host);
    p3d_copy(c->code, sizeof c->code, cfg->key);
    p3d_copy(c->serial, sizeof c->serial, cfg->serial);
    /* user:pass@ in the URL */
    const char *p = strstr(url, "://");
    if (p) {
        p += 3;
        const char *at = strchr(p, '@'), *slash = strchr(p, '/');
        if (at && (!slash || at < slash)) {
            const char *colon = memchr(p, ':', (size_t)(at - p));
            size_t ul = (size_t)((colon ? colon : at) - p);
            if (ul < sizeof c->user) {
                memcpy(c->user, p, ul);
                c->user[ul] = '\0';
            }
            if (colon && (size_t)(at - colon - 1) < sizeof c->pass) {
                memcpy(c->pass, colon + 1, (size_t)(at - colon - 1));
                c->pass[at - colon - 1] = '\0';
            }
        }
    }
    c->max_w = max_w;
    c->max_h = max_h;
    c->t0 = aos_hal_uptime_ms();
    p3d_copy(c->status, sizeof c->status, _("Conectando con la cámara..."));
    __sync_fetch_and_add(&s_threads, 1);
    if (!aos_hal_thread_start("p3dcam", cam_thread, c, STACK, 3)) {
        __sync_fetch_and_sub(&s_threads, 1);
        free(c);
        return NULL;
    }
    return c;
}

void p3d_cam_stop(p3d_cam_t *c)
{
    if (!c) return;
    c->stop = true;
    __sync_synchronize();
    for (int i = 0; i < 300 && !c->done; i++) aos_hal_sleep_ms(20);
    if (!c->done) {
        /* still in a connect: it frees nothing of ours from here on but itself */
        aos_hal_log(P3D_TAG, "camera thread slow to stop");
        return;
    }
    free(c->px);
    free(c);
}

bool p3d_cam_take(p3d_cam_t *c, uint16_t **px, int *w, int *h, uint32_t *gen)
{
    if (!c) return false;
    bool got = false;
    p3d_lock();
    if (c->px && c->gen != *gen) {
        size_t bytes = (size_t)c->w * c->h * 2;
        if (!*px || *w != c->w || *h != c->h) {
            free(*px);
            *px = p3d_psram(bytes);
        }
        if (*px) {
            memcpy(*px, c->px, bytes);
            *w = c->w;
            *h = c->h;
            *gen = c->gen;
            got = true;
        }
    }
    p3d_unlock();
    return got;
}

void p3d_cam_status(p3d_cam_t *c, char *out, size_t n, float *fps)
{
    if (!c) {
        out[0] = '\0';
        return;
    }
    p3d_lock();
    p3d_copy(out, n, c->status);
    uint64_t dt = aos_hal_uptime_ms() - c->t0;
    if (fps) *fps = dt > 1000 ? c->frames * 1000.0f / (float)(uint32_t)dt : 0;
    p3d_unlock();
}
