/*
 * P4OS - 3D printers: the authorities that sign Bambu Lab's printers.
 *
 * A Bambu printer's certificate is issued to its serial number by Bambu's
 * own authority, not by one of the bundle's, so its MQTT (8883) and its
 * camera (6000) are reached with aos_tls_open_trust(): the chain verified
 * against THESE certificates only, and the name against the serial. They
 * are Bambu's public CA certificates, the same ones Bambu Studio and
 * ha-bambulab (MIT, custom_components/bambu_lab/pybambu/certs) trust: the
 * root "BBL CA", the "BBL CA2" ones (RSA and ECC, on their own and
 * cross-signed by BBL CA), and the device authorities of the H2C, P2S and
 * X2C under BBL CA2.
 *
 * An authority that comes later can be added without a new app: whatever
 * certificates /data/impresoras_ca.pem has are trusted too (also how the
 * simulator trusts tools/fake_printers.py's).
 */
#include "p3d.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>

#define EXTRA_FILE "impresoras_ca.pem"
#define EXTRA_MAX  (32 * 1024)

static const char BAMBU_CA[] =
    /* BBL CA (by BBL CA), until Apr  1 03:42:11 2032 GMT
     * SHA-256 03:0B:CA:81:CE:CE:18:B7:EF:F3:CF:D2:B7:5D:09:D3
     *         EF:CA:89:3B:C0:69:60:9E:37:FA:04:25:7F:E4:D8:40 */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDZTCCAk2gAwIBAgIUV1FckwXElyek1onFnQ9kL7Bk4N8wDQYJKoZIhvcNAQEL\n"
    "BQAwQjELMAkGA1UEBhMCQ04xIjAgBgNVBAoMGUJCTCBUZWNobm9sb2dpZXMgQ28u\n"
    "LCBMdGQxDzANBgNVBAMMBkJCTCBDQTAeFw0yMjA0MDQwMzQyMTFaFw0zMjA0MDEw\n"
    "MzQyMTFaMEIxCzAJBgNVBAYTAkNOMSIwIAYDVQQKDBlCQkwgVGVjaG5vbG9naWVz\n"
    "IENvLiwgTHRkMQ8wDQYDVQQDDAZCQkwgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IB\n"
    "DwAwggEKAoIBAQDL3pnDdxGOk5Z6vugiT4dpM0ju+3Xatxz09UY7mbj4tkIdby4H\n"
    "oeEdiYSZjc5LJngJuCHwtEbBJt1BriRdSVrF6M9D2UaBDyamEo0dxwSaVxZiDVWC\n"
    "eeCPdELpFZdEhSNTaT4O7zgvcnFsfHMa/0vMAkvE7i0qp3mjEzYLfz60axcDoJLk\n"
    "p7n6xKXI+cJbA4IlToFjpSldPmC+ynOo7YAOsXt7AYKY6Glz0BwUVzSJxU+/+VFy\n"
    "/QrmYGNwlrQtdREHeRi0SNK32x1+bOndfJP0sojuIrDjKsdCLye5CSZIvqnbowwW\n"
    "1jRwZgTBR29Zp2nzCoxJYcU9TSQp/4KZuWNVAgMBAAGjUzBRMB0GA1UdDgQWBBSP\n"
    "NEJo3GdOj8QinsV8SeWr3US+HjAfBgNVHSMEGDAWgBSPNEJo3GdOj8QinsV8SeWr\n"
    "3US+HjAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQABlBIT5ZeG\n"
    "fgcK1LOh1CN9sTzxMCLbtTPFF1NGGA13mApu6j1h5YELbSKcUqfXzMnVeAb06Htu\n"
    "3CoCoe+wj7LONTFO++vBm2/if6Jt/DUw1CAEcNyqeh6ES0NX8LJRVSe0qdTxPJuA\n"
    "BdOoo96iX89rRPoxeed1cpq5hZwbeka3+CJGV76itWp35Up5rmmUqrlyQOr/Wax6\n"
    "itosIzG0MfhgUzU51A2P/hSnD3NDMXv+wUY/AvqgIL7u7fbDKnku1GzEKIkfH8hm\n"
    "Rs6d8SCU89xyrwzQ0PR853irHas3WrHVqab3P+qNwR0YirL0Qk7Xt/q3O1griNg2\n"
    "Blbjg3obpHo9\n"
    "-----END CERTIFICATE-----\n"
    /* BBL CA2 RSA (by BBL CA2 RSA), until Jun 17 01:38:08 2050 GMT
     * SHA-256 E9:8F:19:57:8B:3F:12:4A:CE:6B:8A:24:7F:FE:DA:52
     *         DC:99:C8:9F:D4:E7:D2:0C:82:82:99:77:B7:F3:35:02 */
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "haGNe/KFNvJBMOIuIUzknRRmiQ==\n"
    "-----END CERTIFICATE-----\n"
    /* BBL CA2 ECC (by BBL CA2 ECC), until Jun 17 01:38:35 2050 GMT
     * SHA-256 1F:99:D8:46:71:8C:8D:33:3A:C3:19:CB:8E:70:91:9A
     *         B6:32:36:09:0F:BA:DD:E9:09:BA:4A:6F:08:67:A6:83 */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIB8zCCAZmgAwIBAgIUe61jGQ4RzIC8k+sNuqbI/CaNqPIwCgYIKoZIzj0EAwIw\n"
    "RjELMAkGA1UEBhMCQ04xITAfBgNVBAoMGEJCTCBUZWNobm9sb2dpZXMgQ28uIEx0\n"
    "ZDEUMBIGA1UEAwwLQkJMIENBMiBFQ0MwIBcNMjUwNjE3MDEzODM1WhgPMjA1MDA2\n"
    "MTcwMTM4MzVaMEYxCzAJBgNVBAYTAkNOMSEwHwYDVQQKDBhCQkwgVGVjaG5vbG9n\n"
    "aWVzIENvLiBMdGQxFDASBgNVBAMMC0JCTCBDQTIgRUNDMFkwEwYHKoZIzj0CAQYI\n"
    "KoZIzj0DAQcDQgAEpKTF7wRSty4DXpGJzgCPwRh8ghLlxUC3qJbyEgLqTvJgbiwY\n"
    "APPHK7kVbVmerkqhHOT4QeWRlTG3dOQGLA2VpaNjMGEwHQYDVR0OBBYEFKuRpsjY\n"
    "REOyIKH7HwOE6jhGBd6NMB8GA1UdIwQYMBaAFKuRpsjYREOyIKH7HwOE6jhGBd6N\n"
    "MA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMAoGCCqGSM49BAMCA0gA\n"
    "MEUCIErBiUm3VdtP3rz4kb8aLpI5p+BzL7M9vElBGWWJxpHMAiEA3r5tJWVGwuxi\n"
    "YCrB1c40KYFRFyahGrhOJZAj/YhRdnU=\n"
    "-----END CERTIFICATE-----\n"
    /* BBL CA2 RSA (by BBL CA), until Jun 15 02:01:27 2035 GMT
     * SHA-256 3D:13:01:9B:45:A4:FA:8A:6C:B8:DC:0F:D7:BE:CB:B1
     *         9B:B2:BF:0C:2F:CB:94:34:A9:34:5F:A5:86:80:DA:0B */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIEeTCCA2GgAwIBAgIUOq+lNIaC2xsswkFqj5JPyVBl45cwDQYJKoZIhvcNAQEL\n"
    "BQAwQjELMAkGA1UEBhMCQ04xIjAgBgNVBAoMGUJCTCBUZWNobm9sb2dpZXMgQ28u\n"
    "LCBMdGQxDzANBgNVBAMMBkJCTCBDQTAeFw0yNTA2MTcwMjAxMjdaFw0zNTA2MTUw\n"
    "MjAxMjdaMEYxCzAJBgNVBAYTAkNOMSEwHwYDVQQKDBhCQkwgVGVjaG5vbG9naWVz\n"
    "IENvLiBMdGQxFDASBgNVBAMMC0JCTCBDQTIgUlNBMIICIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAg8AMIICCgKCAgEAo4550G4c42gTKzQqixwKT089RizIdZpyOcGA679rPaOd\n"
    "WsMqVwnYPP2FpMqXKkjFbedE+SpGloi2NKCuiPNVRbq9PHOOZwTs7YLobOwf53FJ\n"
    "uO6vRFpzFfX1tlc9zlFqJvZnYO9NgHpMysidocWcgrDN/SIDywgPB5CVbYg3Vvzu\n"
    "a9fwZx9e5KT9xd5IpTqdTrWS47jQOVKLhdQCbJFIlMrblOwLBAx+fHokwqh6tkI6\n"
    "Ktuyyjw8Dysebi1ndWjKtZ2mW47r8xZ/J+z3EZqcyJMY6MRtx/zb1jBFuHtkjrb5\n"
    "Kv1DMzSKlkaNJIbvC+Mk+hI97W+SjLSRuIdC7+oJUzWaSzgu9cjXCVfmq8t4IL/3\n"
    "5hP69PK95LgLectIrP96CYAT/aVMG19FrFW0QWEyfT+kzG4jkumfPbHqY2nNkEN0\n"
    "+tjj3h4WdzrWgQEojK/lhfcRFVkts74+aZoMpQP+vmL17CKmSzXk5o/eK21xgxJd\n"
    "zMbdztfTpibiXk0abfOpN+1VR+3NYa+bROAKNyGaReEGsyW2bjcjNx515Vqzj3SV\n"
    "xhMSp5vfF9E4A1jE99M/l9jQDM6RzkT0lMccGAd5tUSdNvDlrqtQaQiKv/ZsXPgX\n"
    "LTWfOpvaLNEgwdMgZMuhjpkwvAZyoYfeF9kyydjDh7bvrX//cz/VopAUlxUtQtMC\n"
    "AwEAAaNjMGEwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0O\n"
    "BBYEFNVJgQad1sNTN0jxVkwbJ/XM1an1MB8GA1UdIwQYMBaAFI80QmjcZ06PxCKe\n"
    "xXxJ5avdRL4eMA0GCSqGSIb3DQEBCwUAA4IBAQAvS8tyfagaGsFf9YncA2ko/Na5\n"
    "9BVF+8TlUo+32oznwIVpS1AhSgLP6rNVekXNFKbuP5htudLQ17ZRBJI/UMVyYEDq\n"
    "IN7xv7Zj+zJwF6W6haYrjb2Vk8igw1XvNULZfvVNNKIkvJUiVqEslWrC+k74crk/\n"
    "Wv8ChVf+zqvfIN6LV3esaGRL02J3AprQGb7DDhR1EefQMScDkNpGJMUmvCmfknrl\n"
    "iK8qgvQN1SWO7JRf6fNKHsN1ZQvyP0pgLWxpT3V0/0/WttqX3cMGuJF+jVUzm/Nh\n"
    "xYhFewG8vc3KzTjnwQApMA6CW554FOJWFyOD2jn5yJLT3Vue+aYDQRp4bKMx\n"
    "-----END CERTIFICATE-----\n"
    /* BBL CA2 ECC (by BBL CA), until Jun 15 02:01:47 2035 GMT
     * SHA-256 E8:98:48:32:24:F9:09:2F:57:5D:62:74:33:BB:21:ED
     *         9D:12:5A:77:E3:63:5B:0D:A6:FC:18:FE:85:0A:02:D7 */
    "-----BEGIN CERTIFICATE-----\n"
    "MIICrjCCAZagAwIBAgIUOq+lNIaC2xsswkFqj5JPyVBl45gwDQYJKoZIhvcNAQEL\n"
    "BQAwQjELMAkGA1UEBhMCQ04xIjAgBgNVBAoMGUJCTCBUZWNobm9sb2dpZXMgQ28u\n"
    "LCBMdGQxDzANBgNVBAMMBkJCTCBDQTAeFw0yNTA2MTcwMjAxNDdaFw0zNTA2MTUw\n"
    "MjAxNDdaMEYxCzAJBgNVBAYTAkNOMSEwHwYDVQQKDBhCQkwgVGVjaG5vbG9naWVz\n"
    "IENvLiBMdGQxFDASBgNVBAMMC0JCTCBDQTIgRUNDMFkwEwYHKoZIzj0CAQYIKoZI\n"
    "zj0DAQcDQgAEpKTF7wRSty4DXpGJzgCPwRh8ghLlxUC3qJbyEgLqTvJgbiwYAPPH\n"
    "K7kVbVmerkqhHOT4QeWRlTG3dOQGLA2VpaNjMGEwDwYDVR0TAQH/BAUwAwEB/zAO\n"
    "BgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFKuRpsjYREOyIKH7HwOE6jhGBd6NMB8G\n"
    "A1UdIwQYMBaAFI80QmjcZ06PxCKexXxJ5avdRL4eMA0GCSqGSIb3DQEBCwUAA4IB\n"
    "AQCg6PjUSSZV+4bvejcVMvgXmKzfD95osWn0ctnoMBxPDa+m+Gg+BcLT2IlFAe3E\n"
    "KYMvu4T295WQc92rjKYqW6cirFppng9uEFW2mZLimxaSmutsTftE3sbMVMJ/SLYN\n"
    "PV7TFv6mcBSIFWXwmBOIpbh4BUcVfONTvdSfIqfyAVxsq4xzc2nc6hPBpAm21Ayj\n"
    "ToC1ev/TbDJ8VllFZiEVmWWlIP3aNzAm8S2mOpxPB2WnanaZHSrvXLFhstyzwrjD\n"
    "yO1/isOZ7wtr7rcuTJdEvvvCimOZlkfRhaDoTew9tQ0E2FVpzzSinw02qmQ1xIE9\n"
    "5/H5ZzJSPkpeAHWEPnKkxg0v\n"
    "-----END CERTIFICATE-----\n"
    /* BBL Device CA O1C2-V2 (by BBL CA2 RSA), until Jul 15 06:10:09 2040 GMT
     * SHA-256 CE:66:7B:6C:00:1D:26:C3:4E:D9:73:33:FB:60:99:69
     *         94:09:FC:3D:4E:12:42:9C:37:BA:EB:B0:5C:46:C6:0A */
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    /* BBL Device CA N7-V2 (by BBL CA2 RSA), until Jun 26 08:30:54 2040 GMT
     * SHA-256 6A:85:4D:54:6E:D6:E3:CE:7A:21:12:65:7E:53:F4:55
     *         53:74:41:3E:6F:5C:B5:5F:99:5D:1D:DD:7C:06:DC:69 */
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "v09+U4m1fOSvF0We4MPCYt+z/E/br+uZ1pIBcg==\n"
    "-----END CERTIFICATE-----\n"
    /* BBL Device CA N6-V2 (by BBL CA2 RSA), until Jun 26 08:29:09 2040 GMT
     * SHA-256 68:BF:31:82:BC:32:D5:A5:45:4F:86:49:28:AB:AA:29
     *         19:41:F6:D5:AC:6A:86:A0:E5:AD:6E:CF:E2:D5:47:7B */
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "kvkHW8vmTffkFHMWC/YoXRXklNlv79Z4+Rywrw==\n"
    "-----END CERTIFICATE-----\n"
    ;

static char  *s_pem;            /* the above and the card's, built once */
static size_t s_len;

static void build(void)
{
    char path[180];
    snprintf(path, sizeof path, "%s/%s", aos_hal_path_data(), EXTRA_FILE);
    size_t base = sizeof BAMBU_CA - 1;
    char *extra = NULL;
    long n = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n <= EXTRA_MAX && (extra = p3d_psram((size_t)n + 1))) {
            n = (long)fread(extra, 1, (size_t)n, f);
            extra[n] = '\0';
            if (!strstr(extra, "-----BEGIN CERTIFICATE-----")) n = 0;
        } else {
            n = 0;
        }
        fclose(f);
    }
    s_pem = p3d_psram(base + (size_t)n + 2);
    if (s_pem) {
        memcpy(s_pem, BAMBU_CA, base);
        s_len = base;
        if (n > 0) {
            s_pem[s_len++] = '\n';
            memcpy(s_pem + s_len, extra, (size_t)n);
            s_len += (size_t)n;
            aos_hal_log(P3D_TAG, "Bambu: %s trusted too", path);
        }
        s_pem[s_len] = '\0';
    }
    free(extra);
}

bool p3d_bambu_trust(const char *serial, p3d_trust_t *out)
{
    p3d_lock();
    if (!s_pem) build();
    out->pem = s_pem;
    out->len = s_len;
    out->name = serial;
    p3d_unlock();
    return s_pem != NULL;
}

void p3d_trust_free(void)
{
    p3d_lock();
    free(s_pem);
    s_pem = NULL;
    s_len = 0;
    p3d_unlock();
}
