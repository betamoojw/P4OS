/*
 * P4OS - Cartero: HTTP/1.1 over the HAL's connection.
 *
 * aos_hal_http_* does not hand back the response's headers, and an API
 * client is about the headers as much as the body; nor does it say how long
 * each part took. So this is the app's own client over aos_tls.c (the
 * connection Impresoras 3D and Python's socket use), in the job's thread:
 * Content-Length, chunked and read-to-close bodies into PSRAM, redirects
 * followed by hand (so they are counted and the final address is known),
 * the connection kept for the next request to the same host, and the
 * times split as Postman does: connecting (DNS, TCP and TLS), sending,
 * waiting for the first byte, downloading.
 *
 * TLS is the HAL's, with its trust store and its rule that a certificate
 * that cannot be verified is refused.
 */
#include "ct.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct ct_tls ct_tls_t;
ct_tls_t *aos_tls_new(void);
void aos_tls_free(ct_tls_t *c);
int  aos_tls_open(ct_tls_t *c, const char *host, int port, unsigned flags, int timeout_ms, char *why, size_t why_n);
bool aos_tls_send(ct_tls_t *c, const void *buf, int len, int timeout_ms);
int  aos_tls_recv(ct_tls_t *c, void *buf, int max, int timeout_ms);
#define TLS_ENCRYPT  0x01u
#define TLS_NODELAY  0x02u
#define TLS_TIMEOUT  (-2)

#define HDR_MAX      (16 * 1024)
#define SLICE_MS     200
#define KEEP_MS      15000
#define MAX_REDIRECTS 10

/* ---- small text helpers --------------------------------------------------------------------- */

bool ct_url_split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn, bool *tls)
{
    const char *p = url;
    while (*p == ' ') p++;
    *tls = false;
    if (!strncasecmp(p, "https://", 8)) {
        *tls = true;
        p += 8;
    } else if (!strncasecmp(p, "http://", 7)) {
        p += 7;
    } else {
        return false;
    }
    const char *end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) p = at + 1;                 /* user:pass@ is not sent in the URL */
    const char *colon = NULL;
    if (*p == '[') {                    /* [::1]:8080 */
        const char *rb = memchr(p, ']', (size_t)(end - p));
        if (!rb) return false;
        if (rb + 1 < end && rb[1] == ':') colon = rb + 1;
        size_t hl = (size_t)(rb - p - 1);
        if (!hl || hl >= hn) return false;
        memcpy(host, p + 1, hl);
        host[hl] = '\0';
    } else {
        colon = memchr(p, ':', (size_t)(end - p));
        size_t hl = (size_t)((colon ? colon : end) - p);
        if (!hl || hl >= hn) return false;
        memcpy(host, p, hl);
        host[hl] = '\0';
    }
    *port = colon ? atoi(colon + 1) : (*tls ? 443 : 80);
    const char *rest = end;
    size_t k = 0;
    if (*rest != '/') path[k++] = '/';
    /* the fragment never goes to the server; spaces are sent escaped */
    for (; *rest && *rest != '#' && k + 4 < pn; rest++) {
        if (*rest == ' ') {
            path[k++] = '%';
            path[k++] = '2';
            path[k++] = '0';
        } else {
            path[k++] = *rest;
        }
    }
    path[k] = '\0';
    return *port > 0 && *port < 65536;
}

size_t ct_url_enc(char *out, size_t n, const char *in, bool keep_slash)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
            *p == '_' || *p == '.' || *p == '~' || (keep_slash && *p == '/')) {
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

size_t ct_b64(char *out, size_t n, const void *in, size_t len)
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

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int ct_b64_dec(uint8_t *out, size_t n, const char *in)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') continue;
        int v = b64v(*p);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < n) out[o] = (uint8_t)(acc >> bits);
            o++;
        }
    }
    if (o < n) out[o] = '\0';
    return (int)(o < n ? o : n);
}

const char *ct_resp_header(const ct_resp_t *rs, const char *name)
{
    if (!rs) return NULL;
    for (int i = 0; i < rs->nhdr; i++) if (!strcasecmp(rs->hdr[i].k, name)) return rs->hdr[i].v;
    return NULL;
}

/* text if the type says so, or, without a type, if the bytes look like it */
bool ct_body_is_text(const ct_resp_t *rs)
{
    const char *ct = ct_resp_header(rs, "Content-Type");
    if (ct) {
        if (!strncasecmp(ct, "text/", 5) || strstr(ct, "json") || strstr(ct, "xml") || strstr(ct, "javascript") ||
            strstr(ct, "x-www-form-urlencoded") || strstr(ct, "graphql") || strstr(ct, "yaml") || strstr(ct, "csv")) {
            return true;
        }
        if (!strncasecmp(ct, "image/", 6) || !strncasecmp(ct, "audio/", 6) || !strncasecmp(ct, "video/", 6) ||
            strstr(ct, "octet-stream") || strstr(ct, "zip") || strstr(ct, "pdf")) {
            return false;
        }
    }
    int n = rs->len < 2048 ? rs->len : 2048;
    for (int i = 0; i < n; i++) {
        uint8_t c = rs->body[i];
        if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) return false;
    }
    return true;
}

const char *ct_reason_text(int s)
{
    switch (s) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 418: return "I'm a teapot";
    case 422: return "Unprocessable Entity";
    case 425: return "Too Early";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

/* ---- the connection ---------------------------------------------------------------------------- */

void ct_http_close(ct_conn_t *cc)
{
    if (cc->c) aos_tls_free((ct_tls_t *)cc->c);
    cc->c = NULL;
}

static int conn_open(ct_conn_t *cc, const char *host, int port, bool tls, int timeout_ms, char *why, size_t wn)
{
    ct_http_close(cc);
    ct_tls_t *c = aos_tls_new();
    if (!c) {
        ct_copy(why, wn, "no memory for the connection");
        return AOS_HTTP_ERR_MEM;
    }
    char w[160] = "";
    int rc = aos_tls_open(c, host, port, (tls ? TLS_ENCRYPT : 0) | TLS_NODELAY, timeout_ms, w, sizeof w);
    if (rc != 0) {
        aos_tls_free(c);
        ct_copy(why, wn, w);
        return rc;
    }
    cc->c = c;
    ct_copy(cc->host, sizeof cc->host, host);
    cc->port = port;
    cc->tls = tls;
    cc->at = aos_hal_uptime_ms();
    return 0;
}

static bool conn_fresh(const ct_conn_t *cc, const char *host, int port, bool tls)
{
    return cc->c && cc->port == port && cc->tls == tls && !strcasecmp(cc->host, host) &&
           aos_hal_uptime_ms() - cc->at < KEEP_MS;
}

/* ---- one exchange ------------------------------------------------------------------------------ */

typedef struct {
    bool on;
    int  state;             /* 0 the size line, 1 data, 2 the CRLF after it, 3 done */
    long left;
    char line[24];
    int  line_len;
} chunked_t;

typedef struct {
    uint8_t *p;
    int      len, cap, max;
    bool     cut;
} body_t;

static void body_put(body_t *b, const uint8_t *s, int n)
{
    if (n <= 0) return;
    int room = b->max - b->len;
    if (n > room) {
        b->cut = true;
        n = room;
        if (n <= 0) return;
    }
    if (b->len + n + 1 > b->cap) {
        int cap = b->cap ? b->cap : 8192;
        while (cap < b->len + n + 1) cap *= 2;
        if (cap > b->max + 1) cap = b->max + 1;
        uint8_t *p = ct_psram_realloc(b->p, (size_t)cap);
        if (!p) {
            b->cut = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
}

/* the payload of src[0..n), the chunk framing taken off; false once the last
 * chunk went by */
static bool body_add(body_t *b, chunked_t *c, const uint8_t *src, int n)
{
    if (!c->on) {
        body_put(b, src, n);
        return true;
    }
    for (int i = 0; i < n;) {
        if (c->state == 1) {
            int k = n - i;
            if (k > c->left) k = (int)c->left;
            body_put(b, src + i, k);
            i += k;
            c->left -= k;
            if (c->left == 0) c->state = 2;
        } else if (c->state == 2) {
            if (src[i++] == '\n') c->state = 0;
        } else if (c->state == 0) {
            char ch = (char)src[i++];
            if (ch == '\n') {
                c->line[c->line_len] = '\0';
                c->left = strtol(c->line, NULL, 16);
                c->line_len = 0;
                if (c->left <= 0) {
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

static bool has_header(const ct_http_req_t *rq, const char *name)
{
    for (int i = 0; i < rq->nhdr; i++) if (rq->hdr[i].k && !strcasecmp(rq->hdr[i].k, name)) return true;
    return false;
}

static void parse_headers(ct_resp_t *rs, char *hdr)
{
    /* the status line: HTTP/1.1 200 OK */
    char *eol = strstr(hdr, "\r\n");
    if (eol) *eol = '\0';
    char *sp = strchr(hdr, ' ');
    size_t vl = sp ? (size_t)(sp - hdr) : 0;
    if (vl >= sizeof rs->version) vl = sizeof rs->version - 1;
    memcpy(rs->version, hdr, vl);
    rs->version[vl] = '\0';
    rs->status = sp ? atoi(sp + 1) : 0;
    char *reason = sp ? strchr(sp + 1, ' ') : NULL;
    ct_copy(rs->reason, sizeof rs->reason, reason ? reason + 1 : "");
    if (!rs->reason[0]) ct_copy(rs->reason, sizeof rs->reason, ct_reason_text(rs->status));
    rs->nhdr = 0;
    char *p = eol ? eol + 2 : NULL;
    while (p && *p && rs->nhdr < CT_HDR_MAX) {
        char *e = strstr(p, "\r\n");
        if (e) *e = '\0';
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            char *t = v + strlen(v);
            while (t > v && (t[-1] == ' ' || t[-1] == '\t')) *--t = '\0';
            rs->hdr[rs->nhdr].k = p;
            rs->hdr[rs->nhdr].v = v;
            rs->nhdr++;
        }
        p = e ? e + 2 : NULL;
    }
}

/* 1 done (rs->status set), 0 a kept connection turned out dead before
 * anything came (try a fresh one), -1 failed (rs->code and err) */
static int exchange(ct_conn_t *cc, const char *method, const char *host, int port, bool tls, const char *path,
                    const ct_http_req_t *rq, const char *body, int blen, ct_resp_t *rs, volatile bool *stop,
                    uint64_t deadline, bool reused)
{
    /* the request */
    ct_sb_t q = { 0 };
    char hostport[180];
    if ((tls && port == 443) || (!tls && port == 80)) ct_copy(hostport, sizeof hostport, host);
    else if (strchr(host, ':')) snprintf(hostport, sizeof hostport, "[%.150s]:%d", host, port);
    else snprintf(hostport, sizeof hostport, "%.150s:%d", host, port);
    ct_sb_addf(&q, "%s ", method);
    ct_sb_adds(&q, path);
    ct_sb_adds(&q, " HTTP/1.1\r\n");
    if (!has_header(rq, "Host")) ct_sb_addf(&q, "Host: %s\r\n", hostport);
    for (int i = 0; i < rq->nhdr; i++) {
        const ct_kv_t *h = &rq->hdr[i];
        if (!h->k || !h->k[0] || !strcasecmp(h->k, "Content-Length") || !strcasecmp(h->k, "Connection")) continue;
        /* a line break inside a header would make a second header */
        ct_sb_adds(&q, h->k);
        ct_sb_adds(&q, ": ");
        for (const char *v = h->v ? h->v : ""; *v; v++) if (*v != '\r' && *v != '\n') ct_sb_addc(&q, *v);
        ct_sb_adds(&q, "\r\n");
    }
    if (!has_header(rq, "User-Agent")) ct_sb_adds(&q, "User-Agent: Cartero/1.0 (P4OS)\r\n");
    if (!has_header(rq, "Accept")) ct_sb_adds(&q, "Accept: */*\r\n");
    if (!has_header(rq, "Accept-Encoding")) ct_sb_adds(&q, "Accept-Encoding: identity\r\n");
    ct_sb_adds(&q, "Connection: keep-alive\r\n");
    bool sends_body = blen > 0 || !strcasecmp(method, "POST") || !strcasecmp(method, "PUT") ||
                      !strcasecmp(method, "PATCH");
    if (sends_body) ct_sb_addf(&q, "Content-Length: %d\r\n", blen);
    ct_sb_adds(&q, "\r\n");
    if (q.oom) {
        ct_sb_free(&q);
        rs->code = AOS_HTTP_ERR_MEM;
        ct_copy(rs->err, sizeof rs->err, "no memory for the request");
        return -1;
    }
    uint64_t t_send = aos_hal_uptime_ms();
    int to = (int)(deadline > t_send ? deadline - t_send : 1);
    bool sent = aos_tls_send((ct_tls_t *)cc->c, q.p, (int)q.n, to);
    ct_sb_free(&q);
    if (sent && blen > 0) sent = aos_tls_send((ct_tls_t *)cc->c, body, blen, to);
    if (!sent) {
        if (reused) return 0;
        rs->code = AOS_HTTP_ERR_SEND;
        ct_copy(rs->err, sizeof rs->err, "the request could not be sent");
        return -1;
    }
    uint64_t t_sent = aos_hal_uptime_ms();
    rs->t_send += (int)(t_sent - t_send);

    /* the headers: 1xx answers are skipped */
    char *hdr = ct_psram(HDR_MAX + 1);
    if (!hdr) {
        rs->code = AOS_HTTP_ERR_MEM;
        return -1;
    }
    int hl = 0, he = -1;
    uint64_t t_first = 0;
    for (;;) {
        while (he < 0) {
            if (stop && *stop) {
                free(hdr);
                rs->code = CT_ERR_STOPPED;
                return -1;
            }
            if (aos_hal_uptime_ms() > deadline) {
                free(hdr);
                rs->code = CT_ERR_TIMEOUT;
                return -1;
            }
            int n = aos_tls_recv((ct_tls_t *)cc->c, hdr + hl, HDR_MAX - hl, SLICE_MS);
            if (n == TLS_TIMEOUT) continue;
            if (n <= 0) {
                free(hdr);
                if (hl == 0 && reused) return 0;
                rs->code = AOS_HTTP_ERR_RECV;
                ct_copy(rs->err, sizeof rs->err, hl ? "the answer was cut short" : "the server closed the connection");
                return -1;
            }
            if (!t_first) t_first = aos_hal_uptime_ms();
            hl += n;
            hdr[hl] = '\0';
            char *e = strstr(hdr, "\r\n\r\n");
            if (e) he = (int)(e - hdr) + 4;
            else if (hl >= HDR_MAX) {
                free(hdr);
                rs->code = AOS_HTTP_ERR_PROTO;
                ct_copy(rs->err, sizeof rs->err, "the headers are longer than 16 KB");
                return -1;
            }
        }
        if (strncmp(hdr, "HTTP/", 5)) {
            free(hdr);
            rs->code = AOS_HTTP_ERR_PROTO;
            ct_copy(rs->err, sizeof rs->err, "the answer is not HTTP");
            return -1;
        }
        int st = atoi(hdr + 9);
        if (st >= 100 && st < 200 && st != 101) {
            memmove(hdr, hdr + he, (size_t)(hl - he));
            hl -= he;
            hdr[hl] = '\0';
            he = -1;
            char *e = strstr(hdr, "\r\n\r\n");
            if (e) he = (int)(e - hdr) + 4;
            continue;
        }
        break;
    }
    rs->t_wait += (int)(t_first - t_sent);

    /* keep the header block, cut into pairs */
    free(rs->hdr_buf);
    rs->hdr_buf = ct_psram((size_t)he + 1);
    if (!rs->hdr_buf) {
        free(hdr);
        rs->code = AOS_HTTP_ERR_MEM;
        return -1;
    }
    memcpy(rs->hdr_buf, hdr, (size_t)he);
    rs->hdr_buf[he - 2] = '\0';
    parse_headers(rs, rs->hdr_buf);

    long clen = -1;
    const char *v = ct_resp_header(rs, "Content-Length");
    if (v) clen = atol(v);
    chunked_t ck = { 0 };
    v = ct_resp_header(rs, "Transfer-Encoding");
    if (v && strstr(v, "chunked")) ck.on = true;
    v = ct_resp_header(rs, "Connection");
    bool close_after = (v && !strncasecmp(v, "close", 5)) ||
                       (!strncmp(rs->version, "HTTP/1.0", 8) && !(v && !strncasecmp(v, "keep", 4)));
    bool no_body = !strcasecmp(method, "HEAD") || rs->status == 204 || rs->status == 304;
    if (no_body) {
        clen = 0;
        ck.on = false;
    }

    body_t b = { .max = rq->max_body > 0 ? rq->max_body : 4 * 1024 * 1024 };
    long got = 0;
    bool more = true;
    if (hl > he && !no_body) {
        got += hl - he;
        more = body_add(&b, &ck, (const uint8_t *)hdr + he, hl - he);
    }
    free(hdr);
    uint8_t *tmp = ct_psram(8192);
    if (!tmp) {
        free(b.p);
        rs->code = AOS_HTTP_ERR_MEM;
        return -1;
    }
    while (more && !(clen >= 0 && !ck.on && got >= clen)) {
        if (stop && *stop) {
            rs->code = CT_ERR_STOPPED;
            free(tmp);
            free(b.p);
            ct_http_close(cc);
            return -1;
        }
        if (aos_hal_uptime_ms() > deadline) {
            b.cut = true;
            close_after = true;
            ct_copy(rs->err, sizeof rs->err, "timed out while downloading");
            break;
        }
        int n = aos_tls_recv((ct_tls_t *)cc->c, tmp, 8192, SLICE_MS);
        if (n == TLS_TIMEOUT) continue;
        if (n <= 0) {
            if ((clen >= 0 && got < clen) || (ck.on && ck.state != 3)) b.cut = true;
            close_after = true;
            break;
        }
        got += n;
        more = body_add(&b, &ck, tmp, n);
        /* past the ceiling: what is left is not wanted, and the connection
         * cannot be kept with it unread */
        if (b.cut) {
            close_after = true;
            break;
        }
    }
    free(tmp);
    if (!b.p) b.p = ct_psram(1);
    if (b.p) b.p[b.len] = '\0';
    free(rs->body);
    rs->body = b.p;
    rs->len = b.len;
    rs->cut = b.cut;
    rs->t_download += (int)(aos_hal_uptime_ms() - t_first);
    if (clen < 0 && !ck.on && !no_body) close_after = true;
    if (close_after) ct_http_close(cc);
    else cc->at = aos_hal_uptime_ms();
    return 1;
}

/* a Location against the URL it came from */
static void resolve_location(const char *base, const char *loc, char *out, size_t n)
{
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) {
        ct_copy(out, n, loc);
        return;
    }
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) {
        ct_copy(out, n, loc);
        return;
    }
    if (loc[0] == '/' && loc[1] == '/') {
        snprintf(out, n, "%.*s:%s", (int)(scheme_end - base), base, loc);
        return;
    }
    const char *host_end = scheme_end + 3 + strcspn(scheme_end + 3, "/?#");
    if (loc[0] == '/') {
        snprintf(out, n, "%.*s%s", (int)(host_end - base), base, loc);
        return;
    }
    /* relative to the base's folder */
    const char *q = host_end + strcspn(host_end, "?#");
    const char *slash = q;
    while (slash > host_end && slash[-1] != '/') slash--;
    if (slash == host_end) snprintf(out, n, "%.*s/%s", (int)(host_end - base), base, loc);
    else snprintf(out, n, "%.*s%s", (int)(slash - base), base, loc);
}

void ct_http_do(ct_conn_t *cc, const ct_http_req_t *rq, ct_resp_t *rs, volatile bool *stop)
{
    memset(rs, 0, sizeof *rs);
    uint64_t t0 = aos_hal_uptime_ms();
    int timeout = rq->timeout_ms > 0 ? rq->timeout_ms : 30000;
    uint64_t deadline = t0 + (uint64_t)timeout;
    char url[1024];
    ct_copy(url, sizeof url, rq->url);
    const char *method = rq->method && rq->method[0] ? rq->method : "GET";
    char mbuf[16];
    const char *body = rq->body;
    int blen = rq->blen;

    for (int hop = 0;; hop++) {
        char host[160], *path = ct_psram(2048);
        int port;
        bool tls;
        if (!path) {
            rs->code = AOS_HTTP_ERR_MEM;
            break;
        }
        if (!ct_url_split(url, host, sizeof host, &port, path, 2048, &tls)) {
            free(path);
            rs->code = CT_ERR_URL;
            ct_copy(rs->err, sizeof rs->err, url);
            break;
        }
        int r = -1;
        for (int attempt = 0; attempt < 2; attempt++) {
            bool reused = conn_fresh(cc, host, port, tls);
            if (!reused) {
                uint64_t tc = aos_hal_uptime_ms();
                int left = (int)(deadline > tc ? deadline - tc : 1);
                int rc = conn_open(cc, host, port, tls, left < 10000 ? left : 10000, rs->err, sizeof rs->err);
                rs->t_connect += (int)(aos_hal_uptime_ms() - tc);
                if (rc != 0) {
                    rs->code = rc;
                    break;
                }
            }
            r = exchange(cc, method, host, port, tls, path, rq, body, blen, rs, stop, deadline, reused);
            if (r != 0) break;
            ct_http_close(cc);       /* the kept one was dead: once more, fresh */
        }
        free(path);
        if (r != 1) {
            if (!rs->code) rs->code = AOS_HTTP_ERR_CONNECT;
            break;
        }
        rs->ok = true;
        ct_copy(rs->final_url, sizeof rs->final_url, url);
        const char *loc = ct_resp_header(rs, "Location");
        bool redirect = rs->status == 301 || rs->status == 302 || rs->status == 303 || rs->status == 307 ||
                        rs->status == 308;
        if (!rq->follow || !redirect || !loc || !loc[0]) break;
        if (hop >= MAX_REDIRECTS) {
            rs->ok = false;
            rs->code = CT_ERR_TOOMANY;
            break;
        }
        char next[1024];
        resolve_location(url, loc, next, sizeof next);
        ct_copy(url, sizeof url, next);
        /* 303 always, and 301/302 after a POST, go on as a GET without the
         * body: what browsers and Postman do */
        if (rs->status == 303 || ((rs->status == 301 || rs->status == 302) && strcasecmp(method, "GET") &&
                                  strcasecmp(method, "HEAD"))) {
            ct_copy(mbuf, sizeof mbuf, rs->status == 303 && !strcasecmp(method, "HEAD") ? "HEAD" : "GET");
            method = mbuf;
            body = NULL;
            blen = 0;
        }
        rs->redirects++;
        rs->ok = false;
    }
    rs->t_total = (int)(aos_hal_uptime_ms() - t0);
}

void ct_resp_free(ct_resp_t *rs)
{
    free(rs->body);
    free(rs->hdr_buf);
    rs->body = NULL;
    rs->hdr_buf = NULL;
    rs->len = 0;
    rs->nhdr = 0;
}
