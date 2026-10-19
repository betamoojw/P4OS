/*
 * P4OS - The drivers' helpers: values, the module line's arguments, CRCs and
 * the I2C transactions they all use (sens_int.h).
 */
#include "sens_int.h"

void sens_scpy(char *d, size_t n, const char *s)
{
    size_t i = 0;
    for (; i + 1 < n && s && s[i]; i++) d[i] = s[i];
    if (n) d[i] = 0;
}

void sens_setv(sens_slot_t *s, int k, const char *key, int q, float v)
{
    if (k < 0 || k >= AOS_SENSOR_XVALUES) return;
    sens_scpy(s->xv[k].key, sizeof s->xv[k].key, key);
    s->xv[k].q = (uint8_t)q;
    s->xv[k].v = v;
    if (s->xn < k + 1) s->xn = k + 1;
}

void sens_err(sens_slot_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->pub.err, sizeof s->pub.err, fmt, ap);
    va_end(ap);
}

uint32_t sens_now(void) { return (uint32_t)aos_hal_uptime_ms(); }

/* "key=value" in the module line's arguments */
static const char *arg_at(const char *args, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = args; p && *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') return p + kl + 1;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return NULL;
}

bool sens_arg_str(const sens_slot_t *s, const char *key, char *out, size_t n)
{
    const char *v = arg_at(s->args, key);
    if (!v) { if (n) out[0] = 0; return false; }
    size_t l = strcspn(v, " \t#");
    if (l >= n) l = n - 1;
    memcpy(out, v, l);
    out[l] = 0;
    return true;
}

int32_t sens_arg_int(const sens_slot_t *s, const char *key, int32_t def)
{
    const char *v = arg_at(s->args, key);
    return v ? (int32_t)strtol(v, NULL, 0) : def;
}

float sens_arg_f(const sens_slot_t *s, const char *key, float def)
{
    const char *v = arg_at(s->args, key);
    return v ? strtof(v, NULL) : def;
}

uint16_t sens_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint16_t sens_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t sens_be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
uint32_t sens_le24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

uint8_t sens_crc8(const uint8_t *d, int n, uint8_t poly, uint8_t init)
{
    uint8_t c = init;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ poly : c << 1);
    }
    return c;
}

uint16_t sens_crc16_modbus(const uint8_t *d, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (uint16_t)(c & 1 ? (c >> 1) ^ 0xA001 : c >> 1);
    }
    return c;
}

/* ---- I2C ---- */

bool si2c_wr(aos_io_i2c_t *b, uint8_t a, const void *w, size_t n) { return aos_io_i2c_xfer(b, a, w, n, NULL, 0, SENS_XFER_MS); }
bool si2c_rd(aos_io_i2c_t *b, uint8_t a, void *r, size_t n) { return aos_io_i2c_xfer(b, a, NULL, 0, r, n, SENS_XFER_MS); }

bool si2c_wrrd(aos_io_i2c_t *b, uint8_t a, const void *w, size_t wn, void *r, size_t rn)
{
    return aos_io_i2c_xfer(b, a, w, wn, r, rn, SENS_XFER_MS);
}

bool si2c_rreg(aos_io_i2c_t *b, uint8_t a, uint8_t reg, void *r, size_t n) { return si2c_wrrd(b, a, &reg, 1, r, n); }
bool si2c_rreg8(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t *v) { return si2c_rreg(b, a, reg, v, 1); }

bool si2c_wreg8(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t v)
{
    uint8_t w[2] = { reg, v };
    return si2c_wr(b, a, w, 2);
}

bool si2c_rreg16be(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t *v)
{
    uint8_t r[2];
    if (!si2c_rreg(b, a, reg, r, 2)) return false;
    *v = sens_be16(r);
    return true;
}

bool si2c_rreg16le(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t *v)
{
    uint8_t r[2];
    if (!si2c_rreg(b, a, reg, r, 2)) return false;
    *v = sens_le16(r);
    return true;
}

bool si2c_wreg16be(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t v)
{
    uint8_t w[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
    return si2c_wr(b, a, w, 3);
}

bool si2c_wreg16le(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t v)
{
    uint8_t w[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    return si2c_wr(b, a, w, 3);
}

bool si2c_rreg_a16(aos_io_i2c_t *b, uint8_t a, uint16_t reg, void *r, size_t n)
{
    uint8_t w[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return si2c_wrrd(b, a, w, 2, r, n);
}

bool si2c_wreg_a16(aos_io_i2c_t *b, uint8_t a, uint16_t reg, const void *v, size_t n)
{
    uint8_t w[34];
    if (n > sizeof w - 2) return false;
    w[0] = (uint8_t)(reg >> 8);
    w[1] = (uint8_t)reg;
    memcpy(w + 2, v, n);
    return si2c_wr(b, a, w, n + 2);
}

bool si2c_cmd16(aos_io_i2c_t *b, uint8_t a, uint16_t cmd)
{
    uint8_t w[2] = { (uint8_t)(cmd >> 8), (uint8_t)cmd };
    return si2c_wr(b, a, w, 2);
}

bool si2c_cmd16_w(aos_io_i2c_t *b, uint8_t a, uint16_t cmd, const uint16_t *words, int n)
{
    uint8_t w[2 + 3 * 4];
    if (n > 4) return false;
    w[0] = (uint8_t)(cmd >> 8);
    w[1] = (uint8_t)cmd;
    for (int i = 0; i < n; i++) {
        uint8_t *p = w + 2 + 3 * i;
        p[0] = (uint8_t)(words[i] >> 8);
        p[1] = (uint8_t)words[i];
        p[2] = sens_crc8_si(p, 2);
    }
    return si2c_wr(b, a, w, (size_t)(2 + 3 * n));
}

bool si2c_words(aos_io_i2c_t *b, uint8_t a, uint16_t *words, int n, bool *crc_ok)
{
    uint8_t r[3 * 16];
    if (n > 16) return false;
    if (!si2c_rd(b, a, r, (size_t)(3 * n))) return false;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = r + 3 * i;
        if (sens_crc8_si(p, 2) != p[2]) ok = false;
        words[i] = sens_be16(p);
    }
    if (crc_ok) *crc_ok = ok;
    return true;
}
