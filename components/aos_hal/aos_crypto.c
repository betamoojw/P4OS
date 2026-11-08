/*
 * The HAL's crypto (aos_hal.h, "Crypto"): random bytes, SHA-256, HMAC,
 * HKDF and P-256, for the apps that speak a pairing or key-exchange
 * protocol (the BLE app's Xiaomi activation first).
 *
 * mbedTLS underneath, the one the firmware already carries for HTTPS
 * (3.6 in the IDF) and the simulator links (Homebrew's, 3.4 or later):
 * only calls both have. The same file builds for both; the random bytes
 * come from the chip's generator on the board and from arc4random in the
 * simulator. Nothing stays between calls: each one sets up and frees its
 * own contexts, and wipes what held a key.
 */
#include "aos_hal.h"

#include <string.h>

#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_random.h"
void aos_hal_random(void *buf, size_t len) { esp_fill_random(buf, len); }
/* a context is ~110 bytes: plain malloc would put it in internal RAM */
#define CTX_ALLOC(n) heap_caps_malloc(n, MALLOC_CAP_SPIRAM)
#else
void aos_hal_random(void *buf, size_t len) { arc4random_buf(buf, len); }
#define CTX_ALLOC(n) malloc(n)
#endif

static int rng(void *ctx, unsigned char *out, size_t n)
{
    (void)ctx;
    aos_hal_random(out, n);
    return 0;
}

void aos_hal_sha256(const void *data, size_t len, uint8_t out[32])
{
    mbedtls_sha256(data, len, out, 0);
}

struct aos_hal_sha256_ctx { mbedtls_sha256_context c; };

aos_hal_sha256_ctx_t *aos_hal_sha256_begin(void)
{
    aos_hal_sha256_ctx_t *h = CTX_ALLOC(sizeof *h);
    if (!h) return NULL;
    mbedtls_sha256_init(&h->c);
    mbedtls_sha256_starts(&h->c, 0);
    return h;
}

void aos_hal_sha256_update(aos_hal_sha256_ctx_t *h, const void *data, size_t len)
{
    if (h && len) mbedtls_sha256_update(&h->c, data, len);
}

void aos_hal_sha256_end(aos_hal_sha256_ctx_t *h, uint8_t out[32])
{
    if (!h) return;
    if (out) mbedtls_sha256_finish(&h->c, out);
    mbedtls_sha256_free(&h->c);
    free(h);
}

void aos_hal_hmac_sha256(const void *key, size_t klen, const void *data, size_t len, uint8_t out[32])
{
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, klen, data, len, out);
}

bool aos_hal_hkdf_sha256(const void *salt, size_t slen, const void *ikm, size_t ilen,
                         const void *info, size_t infolen, uint8_t *out, size_t outlen)
{
    if (outlen > 255 * 32 || (infolen && !info)) return false;
    static const uint8_t ZERO[32];
    uint8_t prk[32], t[32];
    if (!salt) { salt = ZERO; slen = sizeof ZERO; }
    aos_hal_hmac_sha256(salt, slen, ikm, ilen, prk);
    /* T(i) = HMAC(PRK, T(i-1) | info | i) */
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t c;
    mbedtls_md_init(&c);
    bool ok = !mbedtls_md_setup(&c, md, 1);
    size_t done = 0;
    for (uint8_t i = 1; ok && done < outlen; i++) {
        ok = !mbedtls_md_hmac_starts(&c, prk, sizeof prk) &&
             (i == 1 || !mbedtls_md_hmac_update(&c, t, sizeof t)) &&
             (!infolen || !mbedtls_md_hmac_update(&c, info, infolen)) &&
             !mbedtls_md_hmac_update(&c, &i, 1) &&
             !mbedtls_md_hmac_finish(&c, t);
        size_t n = outlen - done < sizeof t ? outlen - done : sizeof t;
        if (ok) memcpy(out + done, t, n);
        done += n;
    }
    mbedtls_md_free(&c);
    memset(prk, 0, sizeof prk);
    memset(t, 0, sizeof t);
    return ok;
}

bool aos_hal_p256_keypair(uint8_t priv[32], uint8_t pub[64])
{
    mbedtls_ecp_group g;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&g);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    uint8_t b[65];
    size_t n = 0;
    bool ok = !mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) &&
              !mbedtls_ecp_gen_keypair(&g, &d, &q, rng, NULL) &&
              !mbedtls_mpi_write_binary(&d, priv, 32) &&
              !mbedtls_ecp_point_write_binary(&g, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, b, sizeof b) && n == 65;
    if (ok) memcpy(pub, b + 1, 64);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&g);
    return ok;
}

bool aos_hal_p256_ecdh(const uint8_t priv[32], const uint8_t peer_pub[64], uint8_t secret[32])
{
    mbedtls_ecp_group g;
    mbedtls_mpi d;
    mbedtls_ecp_point q, r;
    mbedtls_ecp_group_init(&g);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    mbedtls_ecp_point_init(&r);
    uint8_t b[65];
    size_t n = 0;
    b[0] = 0x04;
    memcpy(b + 1, peer_pub, 64);
    /* the peer's point is checked: a point off the curve leaks the key */
    bool ok = !mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) &&
              !mbedtls_ecp_point_read_binary(&g, &q, b, sizeof b) &&
              !mbedtls_ecp_check_pubkey(&g, &q) &&
              !mbedtls_mpi_read_binary(&d, priv, 32) &&
              !mbedtls_ecp_check_privkey(&g, &d) &&
              !mbedtls_ecp_mul(&g, &r, &d, &q, rng, NULL) &&
              !mbedtls_ecp_point_write_binary(&g, &r, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, b, sizeof b) && n == 65;
    if (ok) memcpy(secret, b + 1, 32);
    memset(b, 0, sizeof b);
    mbedtls_ecp_point_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&g);
    return ok;
}
