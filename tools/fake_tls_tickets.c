/*
 * P4OS - a TLS 1.2 server that hands out session tickets of the size asked
 * for, to test the HAL's session cache (components/aos_hal/aos_tls.c)
 * without anybody's real server (2026-10-26).
 *
 * Why C and mbedtls and not Python: a ticket's size is the server's choice,
 * and Python's ssl (OpenSSL) gives ~200 bytes with no way to change it.
 * Here the ticket callbacks are ours: the ticket is the session serialized
 * by mbedtls, then padding up to --ticket bytes, so it resumes like a real
 * one. A server that gave a 5.5 KB ticket broke the board's next
 * ClientHello (4 KB output buffer, -0x6a00) for good.
 *
 * It also logs each ClientHello's size, with or without a ticket (the
 * measurement the cache's limit rests on), and with --max-hello N closes
 * the connection on a ClientHello over N bytes: what a board with a 4 KB
 * output buffer does to itself, reproduced on the other side, since the
 * simulator's mbedtls has 16 KB. Each connection is read up to the end of
 * its request's headers and answered with an HTTP 200 whose body is one
 * line ("hello N, resumed yes/no"), then closed: any HTTPS client works
 * against it (aos_hal_http_*, Cartero).
 *
 * Build (macOS, Homebrew's mbedtls, the simulator's):
 *   M=/opt/homebrew/opt/mbedtls
 *   cc -I$M/include tools/fake_tls_tickets.c -L$M/lib -lmbedtls -lmbedx509 -lmbedcrypto -o fake_tls_tickets
 * Run:
 *   ./fake_tls_tickets --port 8443 --cert srv.pem --key srv.key --ticket 5520 [--max-hello 4096]
 * The certificate is a throwaway one (see tools/fake_ha.py --tls); the
 * simulator trusts its CA with P4_SIM_EXTRA_CA.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define MAGIC 0x50345453u               /* "P4TS" */

static size_t s_ticket = 5520;
static bool   s_parsed;                 /* this connection's ticket was taken */

/* The ticket: magic, the serialized session's length, the session, padding */
static int ticket_write(void *p, const mbedtls_ssl_session *session, unsigned char *start,
                        const unsigned char *end, size_t *tlen, uint32_t *lifetime)
{
    (void)p;
    size_t room = (size_t)(end - start), slen = 0;
    if (room < 8) return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    int rc = mbedtls_ssl_session_save(session, start + 8, room - 8, &slen);
    if (rc != 0) return rc;
    uint32_t m = MAGIC, l = (uint32_t)slen;
    memcpy(start, &m, 4);
    memcpy(start + 4, &l, 4);
    size_t want = s_ticket > 8 + slen ? s_ticket : 8 + slen;
    if (want > room) want = room;
    memset(start + 8 + slen, 0xA5, want - 8 - slen);
    *tlen = want;
    *lifetime = 3600;
    return 0;
}

static int ticket_parse(void *p, mbedtls_ssl_session *session, unsigned char *buf, size_t len)
{
    (void)p;
    uint32_t m, l;
    if (len < 8) return MBEDTLS_ERR_SSL_INVALID_MAC;
    memcpy(&m, buf, 4);
    memcpy(&l, buf + 4, 4);
    if (m != MAGIC || 8 + (size_t)l > len) return MBEDTLS_ERR_SSL_INVALID_MAC;
    int rc = mbedtls_ssl_session_load(session, buf + 8, l);
    s_parsed = rc == 0;
    return rc;
}

/* The ClientHello's record, peeked before mbedtls reads it: its length, and
 * whether it carries a session ticket (extension 35) with data in it */
static int hello_peek(int fd, size_t *with_ticket)
{
    unsigned char b[16384 + 5];
    ssize_t n = 0;
    for (int tries = 0; tries < 50; tries++) {
        n = recv(fd, b, sizeof b, MSG_PEEK);
        if (n >= 5 && n >= 5 + ((b[3] << 8) | b[4])) break;
        usleep(20000);
    }
    if (n < 5 || b[0] != 22) return -1;
    int rec = (b[3] << 8) | b[4];
    *with_ticket = 0;
    /* record(5) hs(4) version(2) random(32) */
    size_t i = 5 + 4 + 2 + 32;
    if ((size_t)n < i + 1) return rec;
    i += 1 + b[i];                                          /* session id */
    if ((size_t)n < i + 2) return rec;
    i += 2 + ((b[i] << 8) | b[i + 1]);                      /* cipher suites */
    if ((size_t)n < i + 1) return rec;
    i += 1 + b[i];                                          /* compression */
    if ((size_t)n < i + 2) return rec;
    size_t end = i + 2 + ((b[i] << 8) | b[i + 1]);
    i += 2;
    while (i + 4 <= end && i + 4 <= (size_t)n) {
        int type = (b[i] << 8) | b[i + 1], len = (b[i + 2] << 8) | b[i + 3];
        if (type == 35) *with_ticket = (size_t)len;
        i += 4 + (size_t)len;
    }
    return rec;
}

int main(int argc, char **argv)
{
    int port = 8443;
    size_t max_hello = 0;
    const char *cert = NULL, *key = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--cert")) cert = argv[i + 1];
        else if (!strcmp(argv[i], "--key")) key = argv[i + 1];
        else if (!strcmp(argv[i], "--ticket")) s_ticket = (size_t)atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--max-hello")) max_hello = (size_t)atol(argv[i + 1]);
    }
    if (!cert || !key) {
        fprintf(stderr, "usage: %s --port P --cert C --key K [--ticket N] [--max-hello N]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    mbedtls_ssl_config conf;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&pk);
    mbedtls_ssl_config_init(&conf);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"p4ts", 4) ||
        mbedtls_x509_crt_parse_file(&crt, cert) ||
        mbedtls_pk_parse_keyfile(&pk, key, NULL, mbedtls_ctr_drbg_random, &drbg) ||
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) ||
        mbedtls_ssl_conf_own_cert(&conf, &crt, &pk)) {
        fprintf(stderr, "setup failed (certificate, key)\n");
        return 1;
    }
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_session_tickets_cb(&conf, ticket_write, ticket_parse, NULL);

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) {
        perror("bind");
        return 1;
    }
    printf("tls tickets on 127.0.0.1:%d, ticket %zu B%s\n", port, s_ticket,
           max_hello ? ", closing on a bigger ClientHello" : "");

    for (int conn = 1;; conn++) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        size_t tk = 0;
        int rec = hello_peek(fd, &tk);
        printf("#%d ClientHello %d B (ticket %zu B)", conn, rec, tk);
        if (max_hello && rec > (int)max_hello) {
            printf(" -> over %zu B: closed, as the board's buffer would fail it\n", max_hello);
            close(fd);
            continue;
        }
        mbedtls_ssl_context ssl;
        s_parsed = false;
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_setup(&ssl, &conf);
        mbedtls_net_context nc = { .fd = fd };
        mbedtls_ssl_set_bio(&ssl, &nc, mbedtls_net_send, mbedtls_net_recv, NULL);
        int rc;
        while ((rc = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        }
        if (rc != 0) {
            printf(" -> handshake failed -0x%04x\n", (unsigned)-rc);
        } else {
            /* a ticket presented and taken: an abbreviated handshake */
            bool resumed = s_parsed;
            char req[4096];
            size_t got = 0;
            while (got < sizeof req - 1) {          /* the request, up to its blank line */
                int r = mbedtls_ssl_read(&ssl, (unsigned char *)req + got, sizeof req - 1 - got);
                if (r <= 0) break;
                got += (size_t)r;
                req[got] = 0;
                if (strstr(req, "\r\n\r\n")) break;
            }
            char line[64], resp[256];
            int l = snprintf(line, sizeof line, "hello %d, resumed %s\n", rec, resumed ? "yes" : "no");
            int rl = snprintf(resp, sizeof resp,
                              "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                              "Connection: close\r\n\r\n%s", l, line);
            mbedtls_ssl_write(&ssl, (const unsigned char *)resp, (size_t)rl);
            printf(" -> %s", line);
            mbedtls_ssl_close_notify(&ssl);
        }
        mbedtls_ssl_free(&ssl);
        close(fd);
    }
}
