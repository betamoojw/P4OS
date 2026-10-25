/*
 * P4OS - 3D printers: HTTP/1.1 and raw sockets.
 *
 * aos_hal_http_* is not used, for three reasons measured against what the
 * drivers need: it keeps three slots for the whole system (a list of five
 * machines polled every two seconds would starve the radio and Home
 * Assistant), it opens a connection per request (OctoPrint and Moonraker
 * keep one open happily, and the poll then costs one round trip), and it
 * does not hand back the response's headers, which PrusaLink's Digest
 * authentication cannot do without (the nonce comes in WWW-Authenticate).
 *
 * So this is a small client over the HAL's own connection, aos_tls.c - the
 * same one Python's socket uses -, in the machine's thread: keep-alive,
 * Content-Length, chunked or read-to-close bodies into PSRAM, Basic and
 * Digest (RFC 7616 with MD5, with and without qop), and one retry on a
 * fresh connection when a kept one turns out to have been closed by the
 * other side.
 *
 * TLS is the HAL's, with its trust store and its rule that a certificate
 * that cannot be verified is refused: a printer with a self-signed
 * certificate is reached over plain http on the LAN.
 */
#include "p3d.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

/* from the HAL's aos_tls.h, which is not included: it pulls in mbedtls's
 * headers. The connection is opaque here, as in apps/python. */
typedef struct p3d_tls p3d_tls_t;
p3d_tls_t *aos_tls_new(void);
void aos_tls_free(p3d_tls_t *c);
int  aos_tls_open(p3d_tls_t *c, const char *host, int port, unsigned flags, int timeout_ms, char *why,
                  size_t why_n);
bool aos_tls_send(p3d_tls_t *c, const void *buf, int len, int timeout_ms);
int  aos_tls_recv(p3d_tls_t *c, void *buf, int max, int timeout_ms);
#define TLS_ENCRYPT  0x01u
#define TLS_NODELAY  0x02u
#define TLS_TIMEOUT  (-2)

#define HDR_MAX      6144
#define SLICE_MS     200
#define KEEP_MS      20000          /* a kept connection older than this is opened again */

void *p3d_psram(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
#else
    return malloc(n ? n : 1);
#endif
}

/* ---- sockets ------------------------------------------------------------ */

void *p3d_sock_open(const char *host, int port, bool tls, int timeout_ms, char *why, size_t n)
{
    p3d_tls_t *c = aos_tls_new();
    if (!c) {
        if (why) p3d_copy(why, n, "no memory");
        return NULL;
    }
    char w[128] = "";
    int rc = aos_tls_open(c, host, port, (tls ? TLS_ENCRYPT : 0) | TLS_NODELAY, timeout_ms, w, sizeof w);
    if (rc != 0) {
        aos_tls_free(c);
        if (why) {
            if (w[0]) p3d_copy(why, n, w);
            else snprintf(why, n, "error %d", rc);
        }
        return NULL;
    }
    return c;
}

bool p3d_sock_send(void *s, const void *b, int n, int timeout_ms)
{
    return s && aos_tls_send((p3d_tls_t *)s, b, n, timeout_ms);
}

int p3d_sock_recv(void *s, void *b, int max, int timeout_ms)
{
    if (!s) return -1;
    int r = aos_tls_recv((p3d_tls_t *)s, b, max, timeout_ms);
    if (r > 0) return r;
    if (r == TLS_TIMEOUT) return 0;
    return -1;
}

void p3d_sock_close(void *s)
{
    if (s) aos_tls_free((p3d_tls_t *)s);
}

/* ---- small text helpers -------------------------------------------------- */

bool p3d_url_split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn, bool *tls)
{
    const char *p = url;
    *tls = false;
    if (!strncmp(p, "https://", 8)) { *tls = true; p += 8; }
    else if (!strncmp(p, "http://", 7)) p += 7;
    else return false;
    /* user:pass@ is dropped here; the camera takes it apart itself */
    const char *slash = strchr(p, '/');
    const char *at = strchr(p, '@');
    if (at && (!slash || at < slash)) p = at + 1;
    const char *end = strchr(p, '/');
    if (!end) end = p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(end - p));
    size_t hl = (size_t)((colon ? colon : end) - p);
    if (!hl || hl >= hn) return false;
    memcpy(host, p, hl);
    host[hl] = '\0';
    *port = colon ? atoi(colon + 1) : (*tls ? 443 : 80);
    p3d_copy(path, pn, *end ? end : "/");
    return *port > 0 && *port < 65536;
}

size_t p3d_url_enc(char *out, size_t n, const char *in)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
            *p == '_' || *p == '.' || *p == '~' || *p == '/') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 15];
        }
    }
    if (n) out[o] = '\0';
    return o;
}

size_t p3d_json_esc(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < n; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (*p < 0x20) {
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    if (n) out[o] = '\0';
    return o;
}

size_t p3d_b64(char *out, size_t n, const void *in, size_t len)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *s = in;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 5 < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16;
        if (i + 1 < len) v |= (uint32_t)s[i + 1] << 8;
        if (i + 2 < len) v |= s[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < len ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? T[v & 63] : '=';
    }
    if (n) out[o] = '\0';
    return o;
}

bool p3d_save_file(const char *path, const void *data, int len)
{
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%.190s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

/* ---- HTTP ------------------------------------------------------------------ */

void p3d_http_init(p3d_http_t *h, const char *host, int port, bool tls)
{
    memset(h, 0, sizeof *h);
    p3d_copy(h->host, sizeof h->host, host);
    h->port = port;
    h->tls = tls;
    h->conn_ms = 4000;
}

void p3d_http_close(p3d_http_t *h)
{
    if (h->c) p3d_sock_close(h->c);
    h->c = NULL;
}

void p3d_resp_free(p3d_resp_t *r)
{
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

const char *p3d_http_why(const p3d_resp_t *r)
{
    if (r->status == 401 || r->status == 403) return "auth";
    return r->err[0] ? r->err : "";
}

static bool stopped(const p3d_http_t *h)
{
    return h->stop && *h->stop;
}

/* a header's value (case-insensitive name), copied */
static bool header(const char *hdr, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (const char *p = strstr(hdr, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        if (!strncasecmp(l, name, nl) && l[nl] == ':') {
            const char *v = l + nl + 1;
            while (*v == ' ') v++;
            const char *e = strstr(v, "\r\n");
            size_t k = e ? (size_t)(e - v) : strlen(v);
            if (k >= n) k = n - 1;
            memcpy(out, v, k);
            out[k] = '\0';
            return true;
        }
    }
    return false;
}

/* key="value" or key=value out of a Digest challenge */
static void dparam(const char *ch, const char *key, char *out, size_t n)
{
    out[0] = '\0';
    size_t kl = strlen(key);
    for (const char *p = ch; (p = strstr(p, key)); p += kl) {
        if (p > ch && p[-1] != ' ' && p[-1] != ',') continue;
        if (p[kl] != '=') continue;
        const char *v = p + kl + 1;
        const char *e;
        if (*v == '"') {
            v++;
            e = strchr(v, '"');
        } else {
            e = v + strcspn(v, ", ");
        }
        if (!e) e = v + strlen(v);
        size_t k = (size_t)(e - v);
        if (k >= n) k = n - 1;
        memcpy(out, v, k);
        out[k] = '\0';
        return;
    }
}

static void digest_learn(p3d_http_t *h, const char *ch)
{
    dparam(ch, "realm", h->realm, sizeof h->realm);
    dparam(ch, "nonce", h->nonce, sizeof h->nonce);
    dparam(ch, "opaque", h->opaque, sizeof h->opaque);
    char qop[32];
    dparam(ch, "qop", qop, sizeof qop);
    h->qop = strstr(qop, "auth") != NULL;
    h->have_digest = h->nonce[0] != '\0';
    h->nc = 0;
}

static int auth_line(p3d_http_t *h, const char *method, const char *uri, char *out, size_t n)
{
    if (!h->user[0]) return 0;
    if (h->have_digest) {
        char buf[400], ha1[33], ha2[33], resp[33], cnonce[17];
        snprintf(buf, sizeof buf, "%s:%s:%s", h->user, h->realm, h->pass);
        aos_hal_md5_hex(buf, strlen(buf), ha1);
        snprintf(buf, sizeof buf, "%s:%.300s", method, uri);
        aos_hal_md5_hex(buf, strlen(buf), ha2);
        h->nc++;
        snprintf(cnonce, sizeof cnonce, "%08x%08x", (unsigned)aos_hal_uptime_ms(), (unsigned)h->nc * 2654435761u);
        if (h->qop) {
            snprintf(buf, sizeof buf, "%s:%s:%08x:%s:auth:%s", ha1, h->nonce, (unsigned)h->nc, cnonce, ha2);
        } else {
            snprintf(buf, sizeof buf, "%s:%s:%s", ha1, h->nonce, ha2);
        }
        aos_hal_md5_hex(buf, strlen(buf), resp);
        int k = snprintf(out, n, "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                                 "algorithm=MD5, response=\"%s\"",
                         h->user, h->realm, h->nonce, uri, resp);
        if (h->opaque[0]) k += snprintf(out + k, n - (size_t)k, ", opaque=\"%s\"", h->opaque);
        if (h->qop) k += snprintf(out + k, n - (size_t)k, ", qop=auth, nc=%08x, cnonce=\"%s\"", (unsigned)h->nc, cnonce);
        k += snprintf(out + k, n - (size_t)k, "\r\n");
        return k;
    }
    if (h->digest_only) return 0;
    char up[180], b64[256];
    snprintf(up, sizeof up, "%s:%s", h->user, h->pass);
    p3d_b64(b64, sizeof b64, up, strlen(up));
    return snprintf(out, n, "Authorization: Basic %s\r\n", b64);
}

typedef struct {
    bool on;
    int  state;             /* 0 size line, 1 data, 2 the CRLF after the data, 3 done */
    int  left;
    char line[20];
    int  line_len;
} chunked_t;

/* the payload of src[0..n) appended to r->body (cap bytes), dropping what
 * does not fit; returns false once the last chunk went by */
static bool body_add(p3d_resp_t *r, chunked_t *c, const uint8_t *src, int n, int cap)
{
    for (int i = 0; i < n;) {
        int k;
        if (!c->on || c->state == 1) {
            k = n - i;
            if (c->on && k > c->left) k = c->left;
            int room = cap - r->len;
            int put = k < room ? k : room;
            if (put > 0) {
                memcpy(r->body + r->len, src + i, (size_t)put);
                r->len += put;
            }
            if (put < k) r->cut = true;
            i += k;
            if (c->on) {
                c->left -= k;
                if (c->left == 0) c->state = 2;
            }
        } else if (c->state == 2) {
            if (src[i++] == '\n') c->state = 0;
        } else if (c->state == 0) {
            char ch = (char)src[i++];
            if (ch == '\n') {
                c->line[c->line_len] = '\0';
                c->left = (int)strtol(c->line, NULL, 16);
                c->line_len = 0;
                if (c->left == 0) {
                    c->state = 3;
                    return false;
                }
                c->state = 1;
            } else if (ch != '\r' && c->line_len < (int)sizeof c->line - 1) {
                c->line[c->line_len++] = ch;
            }
        } else {
            return false;
        }
    }
    return true;
}

/* one attempt on h's connection: 1 done (r->status set), 0 the connection
 * was dead before anything came (try a fresh one), -1 failed */
static int attempt(p3d_http_t *h, const char *method, const char *path, const char *body, int blen,
                   const char *ctype, p3d_resp_t *r, int max, int timeout_ms)
{
    char *req = p3d_psram(1024 + (size_t)blen);
    if (!req) {
        p3d_copy(r->err, sizeof r->err, "no memory");
        return -1;
    }
    char hostport[112];
    if ((h->tls && h->port == 443) || (!h->tls && h->port == 80)) p3d_copy(hostport, sizeof hostport, h->host);
    else snprintf(hostport, sizeof hostport, "%.96s:%d", h->host, h->port);
    int k = snprintf(req, 1024, "%s %.400s HTTP/1.1\r\nHost: %s\r\nUser-Agent: P4OS\r\nAccept: */*\r\n"
                                "Connection: keep-alive\r\n",
                     method, path, hostport);
    k += auth_line(h, method, path, req + k, (size_t)(1024 - k));
    if (h->extra[0]) k += snprintf(req + k, (size_t)(1024 - k), "%s", h->extra);
    if (body || !strcmp(method, "POST") || !strcmp(method, "PUT")) {
        k += snprintf(req + k, (size_t)(1024 - k), "Content-Type: %s\r\nContent-Length: %d\r\n",
                      ctype ? ctype : "application/json", blen);
    }
    k += snprintf(req + k, (size_t)(1024 - k), "\r\n");
    if (k >= 1024) k = 1023;
    if (blen > 0) {
        memcpy(req + k, body, (size_t)blen);
        k += blen;
    }
    bool sent = p3d_sock_send(h->c, req, k, timeout_ms);
    free(req);
    if (!sent) return 0;

    char *hdr = p3d_psram(HDR_MAX + 1);
    if (!hdr) return -1;
    int hl = 0, he = -1;
    uint64_t deadline = aos_hal_uptime_ms() + (uint64_t)timeout_ms;
    while (he < 0) {
        if (stopped(h) || aos_hal_uptime_ms() > deadline) {
            free(hdr);
            p3d_copy(r->err, sizeof r->err, "timeout");
            return -1;
        }
        int n = p3d_sock_recv(h->c, hdr + hl, HDR_MAX - hl, SLICE_MS);
        if (n < 0) {
            free(hdr);
            if (hl == 0) return 0;
            p3d_copy(r->err, sizeof r->err, "cut");
            return -1;
        }
        hl += n;
        hdr[hl] = '\0';
        char *e = strstr(hdr, "\r\n\r\n");
        if (e) he = (int)(e - hdr) + 4;
        else if (hl >= HDR_MAX) {
            free(hdr);
            p3d_copy(r->err, sizeof r->err, "headers too long");
            return -1;
        }
    }
    hdr[he - 2] = '\0';
    if (strncmp(hdr, "HTTP/1.", 7) || !hdr[8]) {
        free(hdr);
        p3d_copy(r->err, sizeof r->err, "not HTTP");
        return -1;
    }
    r->status = atoi(hdr + 9);
    char v[300];
    long clen = -1;
    if (header(hdr, "Content-Length", v, sizeof v)) clen = atol(v);
    chunked_t ck = { 0 };
    if (header(hdr, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked")) ck.on = true;
    bool close_after = header(hdr, "Connection", v, sizeof v) && !strncasecmp(v, "close", 5);
    if (!strncmp(hdr, "HTTP/1.0", 8) && !(header(hdr, "Connection", v, sizeof v) && !strncasecmp(v, "keep", 4))) {
        close_after = true;
    }
    header(hdr, "Content-Type", r->ctype, sizeof r->ctype);
    if (r->status == 401 && header(hdr, "WWW-Authenticate", v, sizeof v) && !strncasecmp(v, "Digest", 6)) {
        digest_learn(h, v + 6);
    }
    bool no_body = !strcmp(method, "HEAD") || r->status == 204 || r->status == 304 || (r->status >= 100 && r->status < 200);
    if (no_body) clen = 0;

    int cap = max;
    if (clen >= 0 && clen < cap) cap = (int)clen;
    r->body = p3d_psram((size_t)cap + 1);
    if (!r->body) {
        free(hdr);
        p3d_copy(r->err, sizeof r->err, "no memory");
        return -1;
    }
    r->len = 0;
    long got = 0;
    bool more = true;
    if (hl > he) {
        got += hl - he;
        more = body_add(r, &ck, (const uint8_t *)hdr + he, hl - he, cap);
    }
    free(hdr);
    uint8_t *tmp = p3d_psram(4096);
    if (!tmp) return -1;
    while (more && !(clen >= 0 && !ck.on && got >= clen)) {
        if (stopped(h) || aos_hal_uptime_ms() > deadline) {
            p3d_copy(r->err, sizeof r->err, "timeout");
            close_after = true;
            r->cut = true;
            break;
        }
        int n = p3d_sock_recv(h->c, tmp, 4096, SLICE_MS);
        if (n < 0) {
            if (clen >= 0 || ck.on) r->cut = true;
            close_after = true;
            break;
        }
        got += n;
        more = body_add(r, &ck, tmp, n, cap);
    }
    free(tmp);
    r->body[r->len] = '\0';
    if (clen < 0 && !ck.on) close_after = true;
    if (close_after || r->cut) p3d_http_close(h);
    return 1;
}

static int request(p3d_http_t *h, const char *method, const char *path, const char *body, int blen,
                   const char *ctype, p3d_resp_t *r, int max, int timeout_ms)
{
    for (int round = 0; round < 3; round++) {
        bool fresh = false;
        if (h->c && aos_hal_uptime_ms() - h->used_ms > KEEP_MS) p3d_http_close(h);
        if (!h->c) {
            if (stopped(h)) return r->status = P3D_ERR_CONNECT;
            char why[96] = "";
            h->c = p3d_sock_open(h->host, h->port, h->tls, h->conn_ms, why, sizeof why);
            if (!h->c) {
                p3d_copy(r->err, sizeof r->err, why);
                return r->status = strstr(why, "certif") || strstr(why, "TLS") ? P3D_ERR_TLS : P3D_ERR_CONNECT;
            }
            fresh = true;
        }
        p3d_resp_free(r);
        r->status = 0;
        r->cut = false;
        r->err[0] = '\0';
        int rc = attempt(h, method, path, body, blen, ctype, r, max, timeout_ms);
        h->used_ms = aos_hal_uptime_ms();
        if (rc == 0) {
            p3d_http_close(h);
            if (fresh) {
                p3d_copy(r->err, sizeof r->err, "closed");
                return r->status = P3D_ERR_CONNECT;
            }
            continue;
        }
        if (rc < 0) {
            p3d_http_close(h);
            return r->status = P3D_ERR_CONNECT;
        }
        /* a 401 that taught a new nonce is asked again, once */
        if (r->status == 401 && h->have_digest && h->user[0] && round == 0 && h->nc == 0) continue;
        return r->status;
    }
    return r->status;
}

int p3d_http(p3d_http_t *h, const char *method, const char *path, const char *body, int body_len,
             const char *ctype, p3d_resp_t *r, int max, int timeout_ms)
{
    memset(r, 0, sizeof *r);
    if (body && body_len < 0) body_len = (int)strlen(body);
    if (!body) body_len = 0;
    if (!strncmp(path, "http://", 7) || !strncmp(path, "https://", 8)) {
        p3d_http_t o;
        char p[300];
        bool tls;
        int port;
        char host[96];
        if (!p3d_url_split(path, host, sizeof host, &port, p, sizeof p, &tls)) {
            p3d_copy(r->err, sizeof r->err, "bad URL");
            return r->status = P3D_ERR_CONNECT;
        }
        if (!strcmp(host, h->host) && port == h->port && tls == h->tls) {
            return request(h, method, p, body, body_len, ctype, r, max, timeout_ms);
        }
        p3d_http_init(&o, host, port, tls);
        o.stop = h->stop;
        o.conn_ms = h->conn_ms;
        int st = request(&o, method, p, body, body_len, ctype, r, max, timeout_ms);
        p3d_http_close(&o);
        return st;
    }
    return request(h, method, path, body, body_len, ctype, r, max, timeout_ms);
}
