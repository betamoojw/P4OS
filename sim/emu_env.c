/*
 * P4OS simulator - emulated env sensors for the sensor service (sensors_emu.h):
 * the barometers of components/aos_io/sensors/drv_env.c.
 *
 * Each model keeps its chip's register map, power-on values, calibration
 * block in the chip's own encoding, conversion times and data format. The
 * raw values come from inverting each datasheet's (or vendor library's)
 * floating-point formula by bisection, in double, so a driver's fixed-point
 * or float arithmetic is checked against an independent implementation. The
 * calibration values are made up, in the ranges real parts have.
 *
 * Each reads the room a little differently (two sensors never agree):
 *   bme680/688  +0.40 °C   -2.0 %RH  +0.10 hPa   plus a gas resistance
 *   bmp180      -0.20 °C             -0.30 hPa
 *   bmp388/390  +0.10 °C             +0.05 hPa
 *   bmp581/585  +0.25 °C             -0.08 hPa
 *   dps310      +0.30 °C             +0.12 hPa   (spl06 the same)
 *   lps22/25    -0.35 °C             +0.20 hPa
 *   ms5611/5607 +0.50 °C             -0.15 hPa
 *   qmp6988     -0.10 °C             +0.25 hPa
 */
#include "sensors_emu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.283185307179586

/* x where f(x) = target, f monotonic on [lo, hi] either way */
typedef double (*fn_t)(const void *c, double x);
static double invert(fn_t f, const void *c, double target, double lo, double hi)
{
    bool up = f(c, hi) > f(c, lo);
    for (int i = 0; i < 64; i++) {
        double m = 0.5 * (lo + hi);
        if ((f(c, m) < target) == up) lo = m;
        else hi = m;
    }
    return 0.5 * (lo + hi);
}

static double room_t(double off) { return sim_room_t(sim_now_s()) + off; }
static double room_h(double off) { return sim_room_h(sim_now_s()) + off; }
static double room_pa(double off) { return (sim_room_p(sim_now_s()) + off) * 100.0; }

static uint32_t now32(void) { return (uint32_t)sim_ms(); }

/* ========================================================================== */
/* BME680 / BME688 (Bosch BME68x_SensorAPI, float compensation)               */
/* ========================================================================== */

static const struct {
    uint16_t t1; int16_t t2; int8_t t3;
    uint16_t p1; int16_t p2; int8_t p3; int16_t p4, p5; int8_t p6, p7; int16_t p8, p9; uint8_t p10;
    uint16_t h1, h2; int8_t h3, h4, h5; uint8_t h6; int8_t h7;
    int8_t gh1; int16_t gh2; int8_t gh3;
    uint8_t heat_range; int8_t heat_val, sw_err;
} B68 = {
    25942, 26445, 3,
    35791, -10327, 88, 6943, -77, 30, 40, -2483, -2805, 30,
    761, 1017, 0, 45, 20, 120, -100,
    -30, -12422, 18,
    1, 44, 2,
};

static double b68_tfine(const void *c, double adc)
{
    (void)c;
    double v1 = (adc / 16384.0 - B68.t1 / 1024.0) * B68.t2;
    double d = adc / 131072.0 - B68.t1 / 8192.0;
    return v1 + d * d * (B68.t3 * 16.0);
}
static double b68_temp(const void *c, double adc) { return b68_tfine(c, adc) / 5120.0; }

static double b68_press(const void *c, double adc)
{
    double tf = *(const double *)c;
    double v1 = tf / 2.0 - 64000.0;
    double v2 = v1 * v1 * (B68.p6 / 131072.0);
    v2 = v2 + v1 * B68.p5 * 2.0;
    v2 = v2 / 4.0 + B68.p4 * 65536.0;
    v1 = (B68.p3 * v1 * v1 / 16384.0 + B68.p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * B68.p1;
    double p = 1048576.0 - adc;
    p = (p - v2 / 4096.0) * 6250.0 / v1;
    v1 = B68.p9 * p * p / 2147483648.0;
    v2 = p * (B68.p8 / 32768.0);
    double v3 = (p / 256.0) * (p / 256.0) * (p / 256.0) * (B68.p10 / 131072.0);
    return p + (v1 + v2 + v3 + B68.p7 * 128.0) / 16.0;
}

static double b68_hum(const void *c, double adc)
{
    double tc = *(const double *)c / 5120.0;
    double v1 = adc - (B68.h1 * 16.0 + B68.h3 / 2.0 * tc);
    double v2 = v1 * (B68.h2 / 262144.0 * (1.0 + B68.h4 / 16384.0 * tc + B68.h5 / 1048576.0 * tc * tc));
    return v2 + (B68.h6 / 16384.0 + B68.h7 / 2097152.0 * tc) * v2 * v2;
}

/* the plate: around 95 kΩ, slowly breathing, and a pot on the stove every
 * two and a half minutes (VOCs pull it down) */
static double b68_gas_ohm(void)
{
    double t = sim_now_s();
    double r = 95000.0 * (1.0 + 0.25 * sin(TAU * t / 420.0)) * (1.0 + sim_noise(0.01));
    if (fmod(t, 150.0) > 120.0) r *= 0.35;
    return r;
}

enum { B68_BUSY, B68_DUR, B68_HIGH };   /* u[] */

static void b68_reset(sim_emu_t *d)
{
    bool high = !strcmp(d->m->name, "bme688");
    memset(d->regs, 0, sizeof d->regs);
    d->u[B68_BUSY] = 0;
    d->u[B68_HIGH] = high;
    uint8_t c[42] = { 0 };
    c[0] = (uint8_t)B68.t2; c[1] = (uint8_t)(B68.t2 >> 8); c[2] = (uint8_t)B68.t3;
    c[4] = (uint8_t)B68.p1; c[5] = (uint8_t)(B68.p1 >> 8);
    c[6] = (uint8_t)B68.p2; c[7] = (uint8_t)(B68.p2 >> 8); c[8] = (uint8_t)B68.p3;
    c[10] = (uint8_t)B68.p4; c[11] = (uint8_t)(B68.p4 >> 8);
    c[12] = (uint8_t)B68.p5; c[13] = (uint8_t)(B68.p5 >> 8);
    c[14] = (uint8_t)B68.p7; c[15] = (uint8_t)B68.p6;
    c[18] = (uint8_t)B68.p8; c[19] = (uint8_t)(B68.p8 >> 8);
    c[20] = (uint8_t)B68.p9; c[21] = (uint8_t)(B68.p9 >> 8); c[22] = B68.p10;
    c[23] = (uint8_t)(B68.h2 >> 4);
    c[24] = (uint8_t)((B68.h2 & 0x0F) << 4 | (B68.h1 & 0x0F));
    c[25] = (uint8_t)(B68.h1 >> 4);
    c[26] = (uint8_t)B68.h3; c[27] = (uint8_t)B68.h4; c[28] = (uint8_t)B68.h5; c[29] = B68.h6; c[30] = (uint8_t)B68.h7;
    c[31] = (uint8_t)B68.t1; c[32] = (uint8_t)(B68.t1 >> 8);
    c[33] = (uint8_t)B68.gh2; c[34] = (uint8_t)(B68.gh2 >> 8); c[35] = (uint8_t)B68.gh1; c[36] = (uint8_t)B68.gh3;
    c[37] = (uint8_t)B68.heat_val; c[38] = 0x5A;
    c[39] = (uint8_t)(B68.heat_range << 4 | 0x0A);
    c[41] = (uint8_t)(B68.sw_err << 4 | 0x03);
    memcpy(d->regs + 0x8A, c, 23);
    memcpy(d->regs + 0xE1, c + 23, 14);
    memcpy(d->regs + 0x00, c + 37, 5);
    d->regs[0xD0] = 0x61;
    d->regs[0xF0] = high ? 0x01 : 0x00;
    d->regs[0x1F] = 0x80; d->regs[0x22] = 0x80; d->regs[0x25] = 0x80;     /* "skipped" */
}

static bool b68_ro(uint8_t r)
{
    return r <= 0x04 || (r >= 0x1D && r <= 0x2D) || (r >= 0x8A && r <= 0xA0) || (r >= 0xE1 && r <= 0xEE) ||
           r == 0xD0 || r == 0xF0;
}

static int b68_wait_ms(uint8_t v) { return (v & 0x3F) << (2 * (v >> 6)); }

static void b68_start(sim_emu_t *d)
{
    static const int CYC[8] = { 0, 1, 2, 4, 8, 16, 16, 16 };
    uint8_t m = d->regs[0x74];
    int cyc = CYC[m >> 5] + CYC[(m >> 2) & 7] + CYC[d->regs[0x72] & 7];
    int us = cyc * 1963 + 477 * 9 + 1000;
    bool high = d->u[B68_HIGH];
    bool gas = (d->regs[0x71] & 0x30) == (high ? 0x20 : 0x10);
    int nb = d->regs[0x71] & 0x0F;
    if (nb > 9) nb = 9;
    d->u[B68_DUR] = (uint32_t)(us / 1000 + 1 + (gas ? b68_wait_ms(d->regs[0x64 + nb]) : 0));
    d->u[B68_BUSY] = 1;
    d->t0 = sim_ms();
    d->regs[0x1D] = (uint8_t)(0x20 | (gas ? 0x40 : 0));
}

static void b68_finish(sim_emu_t *d)
{
    uint8_t *r = d->regs;
    uint8_t m = r[0x74];
    double tc = room_t(0.40), pa = room_pa(0.10), rh = room_h(-2.0);
    uint32_t at = (uint32_t)lrint(invert(b68_temp, NULL, tc, 0, 1048575));
    double tf = b68_tfine(NULL, at);
    uint32_t ap = (uint32_t)lrint(invert(b68_press, &tf, pa, 0, 1048575));
    uint32_t ah = (uint32_t)lrint(invert(b68_hum, &tf, rh, 0, 65535));
    if (!(m >> 5)) at = 0x80000;
    if (!((m >> 2) & 7)) ap = 0x80000;
    if (!(r[0x72] & 7)) ah = 0x8000;
    r[0x1F] = (uint8_t)(ap >> 12); r[0x20] = (uint8_t)(ap >> 4); r[0x21] = (uint8_t)(ap << 4);
    r[0x22] = (uint8_t)(at >> 12); r[0x23] = (uint8_t)(at >> 4); r[0x24] = (uint8_t)(at << 4);
    r[0x25] = (uint8_t)(ah >> 8);  r[0x26] = (uint8_t)ah;

    bool high = d->u[B68_HIGH];
    int nb = r[0x71] & 0x0F;
    if (nb > 9) nb = 9;
    bool gas = (r[0x71] & 0x30) == (high ? 0x20 : 0x10);
    memset(r + 0x2A, 0, 4);
    if (gas) {
        double ohm = b68_gas_ohm();
        int best = -1, best_adc = 0;
        for (int g = 0; g < 16; g++) {
            double adc;
            if (high) {
                adc = 512.0 + (1000000.0 * (double)(262144u >> g) / ohm - 4096.0) / 3.0;
            } else {
                static const double K1[16] = { 0, 0, 0, 0, 0, -1.0, 0, -0.8, 0, 0, -0.2, -0.5, 0, -1.0, 0, 0 };
                static const double K2[16] = { 0, 0, 0, 0, 0.1, 0.7, 0, -0.8, -0.1, 0, 0, 0, 0, 0, 0, 0 };
                double v2 = (1340.0 + 5.0 * B68.sw_err) * (1.0 + K1[g] / 100.0);
                double v3 = 1.0 + K2[g] / 100.0;
                adc = 512.0 + v2 * (1.0 / (v3 * 0.000000125 * (double)(1u << g) * ohm) - 1.0);
            }
            long a = lrint(adc);
            if (a < 0 || a > 1023) continue;
            if (best < 0 || labs(a - 512) < labs(best_adc - 512)) { best = g; best_adc = (int)a; }
        }
        /* the plate reached its temperature if it was told one and given 20 ms */
        bool stab = r[0x5A + nb] != 0 && b68_wait_ms(r[0x64 + nb]) >= 20;
        if (best >= 0) {
            uint8_t *gr = r + (high ? 0x2C : 0x2A);
            gr[0] = (uint8_t)(best_adc >> 2);
            gr[1] = (uint8_t)((best_adc & 3) << 6 | 0x20 | (stab ? 0x10 : 0) | best);
        }
    }
    r[0x1D] = (uint8_t)(0x80 | nb);
    r[0x74] = m & 0xFC;                            /* forced: back to sleep */
    d->u[B68_BUSY] = 0;
}

static bool b68_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    /* writes are (register, value) pairs, as the API sends them */
    for (size_t i = 0; i + 1 < wn; i += 2) {
        uint8_t reg = w[i], v = w[i + 1];
        if (reg == 0xE0) { if (v == 0xB6) b68_reset(d); continue; }
        if (b68_ro(reg)) continue;
        d->regs[reg] = v;
        if (reg == 0x74 && (v & 3) == 1) b68_start(d);
    }
    if (wn == 1) d->ptr = w[0];
    if (!rn) return true;
    if (wn >= 2) return false;
    if (d->u[B68_BUSY] && sim_ms() - d->t0 >= d->u[B68_DUR]) b68_finish(d);
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(d->ptr + i)];
    return true;
}

static const sim_model_t SIM_BME680 = { "bme680", 0x77, b68_reset, b68_xfer };
static const sim_model_t SIM_BME688 = { "bme688", 0x77, b68_reset, b68_xfer };

/* ========================================================================== */
/* BMP180 / BMP085 (datasheet BST-BMP180-DS000 §3.5, its example calibration) */
/* ========================================================================== */

static const int16_t B180_AC1 = 408, B180_AC2 = -72, B180_AC3 = -14383;
static const uint16_t B180_AC4 = 32741, B180_AC5 = 32757, B180_AC6 = 23153;
static const int16_t B180_B1 = 6190, B180_B2 = 4, B180_MB = -32768, B180_MC = -8711, B180_MD = 2868;

static double b180_b5(double ut)
{
    double x1 = (ut - B180_AC6) * B180_AC5 / 32768.0;
    return x1 + B180_MC * 2048.0 / (x1 + B180_MD);
}
static double b180_temp(const void *c, double ut) { (void)c; return (b180_b5(ut) + 8.0) / 16.0 / 10.0; }

typedef struct { double b5; int oss; } b180_ctx_t;
static double b180_press(const void *c, double up)
{
    const b180_ctx_t *k = c;
    double b6 = k->b5 - 4000.0, s = (double)(1 << k->oss);
    double x1 = B180_B2 * (b6 * b6 / 4096.0) / 2048.0, x2 = B180_AC2 * b6 / 2048.0;
    double b3 = ((B180_AC1 * 4.0 + x1 + x2) * s + 2.0) / 4.0;
    x1 = B180_AC3 * b6 / 8192.0;
    x2 = B180_B1 * (b6 * b6 / 4096.0) / 65536.0;
    double b4 = B180_AC4 * ((x1 + x2 + 2.0) / 4.0 + 32768.0) / 32768.0;
    double p = (up - b3) * (50000.0 / s) * 2.0 / b4;
    x1 = (p / 256.0) * (p / 256.0) * 3038.0 / 65536.0;
    x2 = -7357.0 * p / 65536.0;
    return p + (x1 + x2 + 3791.0) / 16.0;
}

enum { B180_BUSY, B180_MS, B180_KIND, B180_OSS };

static void b180_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->u, 0, sizeof d->u);
    const uint16_t c[11] = { (uint16_t)B180_AC1, (uint16_t)B180_AC2, (uint16_t)B180_AC3, B180_AC4, B180_AC5, B180_AC6,
                             (uint16_t)B180_B1, (uint16_t)B180_B2, (uint16_t)B180_MB, (uint16_t)B180_MC, (uint16_t)B180_MD };
    for (int i = 0; i < 11; i++) { d->regs[0xAA + 2 * i] = (uint8_t)(c[i] >> 8); d->regs[0xAB + 2 * i] = (uint8_t)c[i]; }
    d->regs[0xD0] = 0x55;
    d->regs[0xD1] = 0x02;
}

static void b180_finish(sim_emu_t *d)
{
    uint8_t *r = d->regs;
    double tc = room_t(-0.20);
    double ut = invert(b180_temp, NULL, tc, 0, 65535);
    if (d->u[B180_KIND] == 0) {
        uint32_t v = (uint32_t)lrint(ut);
        r[0xF6] = (uint8_t)(v >> 8); r[0xF7] = (uint8_t)v;
    } else {
        int oss = (int)d->u[B180_OSS];
        b180_ctx_t k = { b180_b5(ut), oss };
        double up = invert(b180_press, &k, room_pa(-0.30), 0, (double)((1 << (16 + oss)) - 1));
        uint32_t v = (uint32_t)lrint(up) << (8 - oss);
        r[0xF6] = (uint8_t)(v >> 16); r[0xF7] = (uint8_t)(v >> 8); r[0xF8] = (uint8_t)v;
    }
    r[0xF4] &= (uint8_t)~0x20;                     /* sco: done */
    d->u[B180_BUSY] = 0;
}

static bool b180_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) d->ptr = w[0];
    for (size_t i = 1; i < wn; i++) {
        uint8_t reg = (uint8_t)(w[0] + i - 1), v = w[i];
        if (reg == 0xE0) { if (v == 0xB6) b180_reset(d); continue; }
        if (reg != 0xF4) continue;                 /* the only other writable one */
        int oss = v >> 6;
        if (v == 0x2E) { d->u[B180_KIND] = 0; d->u[B180_MS] = 5; }
        else if ((v & 0x3F) == 0x34) {
            static const int MS[4] = { 5, 8, 14, 26 };
            d->u[B180_KIND] = 1; d->u[B180_MS] = (uint32_t)MS[oss]; d->u[B180_OSS] = (uint32_t)oss;
        } else { d->regs[0xF4] = v; continue; }
        d->regs[0xF4] = v | 0x20;
        d->u[B180_BUSY] = 1;
        d->t0 = sim_ms();
    }
    if (!rn) return true;
    if (d->u[B180_BUSY] && sim_ms() - d->t0 >= d->u[B180_MS]) b180_finish(d);
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(d->ptr + i)];
    return true;
}

static const sim_model_t SIM_BMP180 = { "bmp180", 0x77, b180_reset, b180_xfer };

/* ========================================================================== */
/* BMP388 / BMP390 (Bosch BMP3_SensorAPI, float compensation)                 */
/* ========================================================================== */

static const struct {
    uint16_t t1, t2; int8_t t3; int16_t p1, p2; int8_t p3, p4; uint16_t p5, p6; int8_t p7, p8; int16_t p9; int8_t p10, p11;
} B3 = { 27448, 19317, -7, 3268, 1767, 33, 0, 25220, 30652, 3, -6, 16120, 4, -55 };

typedef struct { double t1, t2, t3, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11; } b3q_t;
static b3q_t b3q(void)
{
    b3q_t q = {
        B3.t1 * 256.0, B3.t2 / 1073741824.0, B3.t3 / 281474976710656.0,
        (B3.p1 - 16384.0) / 1048576.0, (B3.p2 - 16384.0) / 536870912.0, B3.p3 / 4294967296.0,
        B3.p4 / 137438953472.0, B3.p5 * 8.0, B3.p6 / 64.0, B3.p7 / 256.0, B3.p8 / 32768.0,
        B3.p9 / 281474976710656.0, B3.p10 / 281474976710656.0, B3.p11 / 36893488147419103232.0,
    };
    return q;
}
static double b3_tlin(const void *c, double ut)
{
    const b3q_t *q = c;
    double d = ut - q->t1;
    return d * q->t2 + d * d * q->t3;
}
typedef struct { b3q_t q; double tl; } b3_ctx_t;
static double b3_press(const void *c, double up)
{
    const b3_ctx_t *k = c;
    const b3q_t *q = &k->q;
    double t = k->tl;
    double o1 = q->p5 + q->p6 * t + q->p7 * t * t + q->p8 * t * t * t;
    double o2 = up * (q->p1 + q->p2 * t + q->p3 * t * t + q->p4 * t * t * t);
    return o1 + o2 + up * up * (q->p9 + q->p10 * t) + up * up * up * q->p11;
}

enum { B3_FORCED, B3_LAST };

static void b3_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->u, 0, sizeof d->u);
    uint8_t *r = d->regs;
    r[0x00] = strcmp(d->m->name, "bmp390") ? 0x50 : 0x60;
    r[0x01] = 0x01;
    r[0x03] = 0x10;                                /* cmd_rdy */
    r[0x1C] = 0x02;
    uint8_t *n = r + 0x31;
    n[0] = (uint8_t)B3.t1; n[1] = (uint8_t)(B3.t1 >> 8); n[2] = (uint8_t)B3.t2; n[3] = (uint8_t)(B3.t2 >> 8);
    n[4] = (uint8_t)B3.t3;
    n[5] = (uint8_t)B3.p1; n[6] = (uint8_t)(B3.p1 >> 8); n[7] = (uint8_t)B3.p2; n[8] = (uint8_t)(B3.p2 >> 8);
    n[9] = (uint8_t)B3.p3; n[10] = (uint8_t)B3.p4;
    n[11] = (uint8_t)B3.p5; n[12] = (uint8_t)(B3.p5 >> 8); n[13] = (uint8_t)B3.p6; n[14] = (uint8_t)(B3.p6 >> 8);
    n[15] = (uint8_t)B3.p7; n[16] = (uint8_t)B3.p8; n[17] = (uint8_t)B3.p9; n[18] = (uint8_t)(B3.p9 >> 8);
    n[19] = (uint8_t)B3.p10; n[20] = (uint8_t)B3.p11;
}

static void b3_measure(sim_emu_t *d)
{
    uint8_t *r = d->regs;
    b3_ctx_t k = { b3q(), 0 };
    double ut = invert(b3_tlin, &k.q, room_t(0.10), 0, 16777215);
    k.tl = b3_tlin(&k.q, floor(ut + 0.5));
    double up = invert(b3_press, &k, room_pa(0.05), 0, 16777215);
    uint32_t p = (uint32_t)lrint(up), t = (uint32_t)lrint(ut);
    bool pe = r[0x1B] & 0x01, te = r[0x1B] & 0x02;
    if (pe) { r[0x04] = (uint8_t)p; r[0x05] = (uint8_t)(p >> 8); r[0x06] = (uint8_t)(p >> 16); }
    if (te) { r[0x07] = (uint8_t)t; r[0x08] = (uint8_t)(t >> 8); r[0x09] = (uint8_t)(t >> 16); }
    r[0x03] |= (uint8_t)((pe ? 0x20 : 0) | (te ? 0x40 : 0));
}

static void b3_update(sim_emu_t *d)
{
    uint8_t *r = d->regs;
    uint32_t now = now32();
    uint32_t st = (uint32_t)(sim_ms() * 1000 / 39);    /* SENSORTIME, 39.0625 µs */
    r[0x0C] = (uint8_t)st; r[0x0D] = (uint8_t)(st >> 8); r[0x0E] = (uint8_t)(st >> 16);
    int mode = (r[0x1B] >> 4) & 3;
    if (mode == 3) {
        uint32_t period = 5u << (r[0x1D] & 0x1F);
        if (now - (uint32_t)d->t0 >= 25 && (d->u[B3_LAST] == 0 || now - d->u[B3_LAST] >= period)) {
            b3_measure(d);
            d->u[B3_LAST] = now | 1;
        }
    } else if (d->u[B3_FORCED] && now - (uint32_t)d->t0 >= 25) {
        b3_measure(d);
        d->u[B3_FORCED] = 0;
        r[0x1B] &= 0xCF;
    }
}

static bool b3_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    for (size_t i = 0; i + 1 < wn; i += 2) {      /* (register, value) pairs */
        uint8_t reg = w[i], v = w[i + 1];
        if (reg == 0x7E) { if (v == 0xB6) b3_reset(d); continue; }
        if (reg <= 0x0E || (reg >= 0x31 && reg <= 0x45)) continue;
        d->regs[reg] = v;
        if (reg == 0x1B) {
            int mode = (v >> 4) & 3;
            d->t0 = sim_ms();
            d->u[B3_LAST] = 0;
            d->u[B3_FORCED] = mode == 1 || mode == 2;
        }
    }
    if (wn == 1) d->ptr = w[0];
    if (!rn) return true;
    if (wn >= 2) return false;
    b3_update(d);
    uint8_t p = d->ptr;
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(p + i)];
    if (p <= 0x09 && p + rn > 0x04) d->regs[0x03] &= (uint8_t)~0x60;   /* drdy clears on a data read */
    return true;
}

static const sim_model_t SIM_BMP388 = { "bmp388", 0x77, b3_reset, b3_xfer };
static const sim_model_t SIM_BMP390 = { "bmp390", 0x77, b3_reset, b3_xfer };

/* ========================================================================== */
/* BMP581 / BMP585: compensated inside; T s24/2^16 °C, P u24/2^6 Pa           */
/* ========================================================================== */

static void b5_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->u, 0, sizeof d->u);
    uint8_t *r = d->regs;
    r[0x01] = strcmp(d->m->name, "bmp585") ? 0x50 : 0x51;
    r[0x02] = 0x32;
    r[0x27] = 0x10;                                /* POR / soft reset complete */
    r[0x28] = 0x02;                                /* nvm_rdy */
    r[0x37] = 0x70;
}

static void b5_measure(sim_emu_t *d)
{
    uint8_t *r = d->regs;
    int32_t t = (int32_t)lrint(room_t(0.25) * 65536.0);
    uint32_t p = r[0x36] & 0x40 ? (uint32_t)lrint(room_pa(-0.08) * 64.0) : 0;
    r[0x1D] = (uint8_t)t; r[0x1E] = (uint8_t)(t >> 8); r[0x1F] = (uint8_t)(t >> 16);
    r[0x20] = (uint8_t)p; r[0x21] = (uint8_t)(p >> 8); r[0x22] = (uint8_t)(p >> 16);
}

static bool b5_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) d->ptr = w[0];
    for (size_t i = 1; i < wn; i++) {
        uint8_t reg = (uint8_t)(w[0] + i - 1), v = w[i];
        if (reg == 0x7E) { if (v == 0xB6) b5_reset(d); continue; }
        if (reg < 0x13 || (reg >= 0x1D && reg <= 0x29)) continue;
        d->regs[reg] = v;
        if (reg == 0x37) { d->t0 = sim_ms(); d->u[0] = (v & 3) == 2; }
    }
    if (!rn) return true;
    int mode = d->regs[0x37] & 3;
    bool due = sim_ms() - d->t0 >= 40;
    if ((mode == 1 || mode == 3) && due) b5_measure(d);
    else if (mode == 2 && d->u[0] && due) { b5_measure(d); d->u[0] = 0; d->regs[0x37] &= 0xFC; }
    uint8_t p = d->ptr;
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(p + i)];
    if (p <= 0x27 && p + rn > 0x27) d->regs[0x27] = 0;    /* INT_STATUS clears on read */
    return true;
}

static const sim_model_t SIM_BMP581 = { "bmp581", 0x47, b5_reset, b5_xfer };
static const sim_model_t SIM_BMP585 = { "bmp585", 0x47, b5_reset, b5_xfer };

/* ========================================================================== */
/* DPS310 / SPL06-001 (Infineon datasheet §4.9; coefficients made with the    */
/* MEMS element's temperature, so TMP_EXT must be set)                        */
/* ========================================================================== */

static const struct { int16_t c0, c1; int32_t c00, c10; int16_t c01, c11, c20, c21, c30; } DPS = {
    203, -261, 80211, -54636, -2232, 1336, -10869, 175, -1530,
};
static const double DPS_K[8] = { 524288, 1572864, 3670016, 7864320, 253952, 516096, 1040384, 2088960 };

static double dps_press(const void *c, double psc)
{
    double tsc = *(const double *)c;
    return DPS.c00 + psc * (DPS.c10 + psc * (DPS.c20 + psc * DPS.c30)) + tsc * DPS.c01 +
           tsc * psc * (DPS.c11 + psc * DPS.c21);
}

enum { DPS_RST, DPS_MODE_T };

static void dps_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    uint8_t *r = d->regs;
    bool spl = !strcmp(d->m->name, "spl06");
    r[0x0D] = 0x10;
    uint32_t c0 = (uint32_t)DPS.c0 & 0xFFF, c1 = (uint32_t)DPS.c1 & 0xFFF;
    uint32_t c00 = (uint32_t)DPS.c00 & 0xFFFFF, c10 = (uint32_t)DPS.c10 & 0xFFFFF;
    uint8_t *c = r + 0x10;
    c[0] = (uint8_t)(c0 >> 4); c[1] = (uint8_t)((c0 & 0x0F) << 4 | c1 >> 8); c[2] = (uint8_t)c1;
    c[3] = (uint8_t)(c00 >> 12); c[4] = (uint8_t)(c00 >> 4); c[5] = (uint8_t)((c00 & 0x0F) << 4 | c10 >> 16);
    c[6] = (uint8_t)(c10 >> 8); c[7] = (uint8_t)c10;
    const int16_t w[5] = { DPS.c01, DPS.c11, DPS.c20, DPS.c21, DPS.c30 };
    for (int i = 0; i < 5; i++) { c[8 + 2 * i] = (uint8_t)((uint16_t)w[i] >> 8); c[9 + 2 * i] = (uint8_t)w[i]; }
    r[0x28] = spl ? 0x00 : 0x80;                   /* COEF_SRCE: only the DPS310 has it */
    r[0x32] = spl ? 0x00 : 0x02;                   /* a DPS310 that needs no temperature fix */
    d->u[DPS_RST] = now32() - 1000;                /* powered up a while ago */
    d->u[DPS_MODE_T] = 0;
}

static void dps_measure(sim_emu_t *d)
{
    uint8_t *r = d->regs;
    int mode = r[0x08] & 7;
    double tc = room_t(0.30);
    double tsc = (tc - DPS.c0 * 0.5) / DPS.c1;
    double psc = invert(dps_press, &tsc, room_pa(0.12), -1.5, 1.5);
    /* the ASIC's sensor, against coefficients made with the MEMS one, reads warm */
    double tsc_seen = r[0x07] & 0x80 ? tsc : (tc + 2.5 - DPS.c0 * 0.5) / DPS.c1;
    int pprc = r[0x06] & 0x0F, tprc = r[0x07] & 0x0F;
    int32_t praw = (int32_t)lrint(psc * DPS_K[pprc > 7 ? 7 : pprc]);
    int32_t traw = (int32_t)lrint(tsc_seen * DPS_K[tprc > 7 ? 7 : tprc]);
    /* above x8 the result needs the shift, or it overflows its 24 bits */
    if (pprc > 3 && !(r[0x09] & 0x04)) praw *= 2;
    if (tprc > 3 && !(r[0x09] & 0x08)) traw *= 2;
    if (mode & 1) { r[0x00] = (uint8_t)(praw >> 16); r[0x01] = (uint8_t)(praw >> 8); r[0x02] = (uint8_t)praw; r[0x08] |= 0x10; }
    if (mode & 2) { r[0x03] = (uint8_t)(traw >> 16); r[0x04] = (uint8_t)(traw >> 8); r[0x05] = (uint8_t)traw; r[0x08] |= 0x20; }
}

static bool dps_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) d->ptr = w[0];
    for (size_t i = 1; i < wn; i++) {
        uint8_t reg = (uint8_t)(w[0] + i - 1), v = w[i];
        if (reg == 0x0C) {
            if ((v & 0x0F) == 0x09) { dps_reset(d); d->u[DPS_RST] = now32(); return !rn; }
            continue;
        }
        if (reg <= 0x05 || reg == 0x0D || (reg >= 0x10 && reg <= 0x21) || reg == 0x28) continue;
        if (reg == 0x08) {
            d->regs[0x08] = (uint8_t)((d->regs[0x08] & 0xF0) | (v & 0x07));
            d->u[DPS_MODE_T] = now32();
            d->u[DPS_MODE_T] += !d->u[DPS_MODE_T];
            continue;
        }
        d->regs[reg] = v;
    }
    if (!rn) return true;
    uint32_t now = now32();
    uint8_t *m = &d->regs[0x08];
    *m &= 0x3F;
    if (now - d->u[DPS_RST] >= 12) *m |= 0x40;     /* SENSOR_RDY */
    if (now - d->u[DPS_RST] >= 40) *m |= 0x80;     /* COEF_RDY */
    if ((*m & 7) >= 5 && d->u[DPS_MODE_T] && now - d->u[DPS_MODE_T] >= 60) dps_measure(d);
    uint8_t p = d->ptr;
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(p + i)];
    if (p <= 0x02 && p + rn > 0x00) *m &= (uint8_t)~0x10;
    if (p <= 0x05 && p + rn > 0x03) *m &= (uint8_t)~0x20;
    return true;
}

static const sim_model_t SIM_DPS310 = { "dps310", 0x77, dps_reset, dps_xfer };
static const sim_model_t SIM_SPL06 = { "spl06", 0x76, dps_reset, dps_xfer };

/* ========================================================================== */
/* ST LPS22HB / LPS22HH / LPS22DF / LPS25HB                                   */
/* ========================================================================== */

enum { LPS_WHO, LPS_LAST };

static bool lps_is25(const sim_emu_t *d) { return d->u[LPS_WHO] == 0xBD; }

static void lps_reset(sim_emu_t *d)
{
    uint8_t who = !strcmp(d->m->name, "lps25hb") ? 0xBD : !strcmp(d->m->name, "lps22df") ? 0xB4 :
                  !strcmp(d->m->name, "lps22hh") ? 0xB3 : 0xB1;
    memset(d->regs, 0, sizeof d->regs);
    d->u[LPS_WHO] = who;
    d->u[LPS_LAST] = 0;
    d->regs[0x0F] = who;
    if (who == 0xB1 || who == 0xB3) d->regs[0x11] = 0x10;     /* IF_ADD_INC */
    if (who == 0xB4) d->regs[0x12] = 0x01;
    if (who == 0xBD) d->regs[0x10] = 0x05;
}

static int lps_odr(const sim_emu_t *d)
{
    const uint8_t *r = d->regs;
    switch (d->u[LPS_WHO]) {
    case 0xBD: return r[0x20] & 0x80 ? (r[0x20] >> 4) & 7 : 0;
    case 0xB4: return (r[0x10] >> 3) & 0x0F;
    default: return (r[0x10] >> 4) & 7;
    }
}

static void lps_measure(sim_emu_t *d)
{
    uint8_t *r = d->regs;
    double tc = room_t(-0.35);
    int32_t p = (int32_t)lrint((room_pa(0.20) / 100.0) * 4096.0);
    int32_t t = (int32_t)lrint(lps_is25(d) ? (tc - 42.5) * 480.0 : tc * 100.0);
    r[0x28] = (uint8_t)p; r[0x29] = (uint8_t)(p >> 8); r[0x2A] = (uint8_t)(p >> 16);
    r[0x2B] = (uint8_t)t; r[0x2C] = (uint8_t)(t >> 8);
    r[0x27] |= 0x03;
    d->u[LPS_LAST] = now32() | 1;
}

static bool lps_inc(const sim_emu_t *d)
{
    if (d->u[LPS_WHO] == 0xB4) return d->regs[0x12] & 0x01;
    return d->regs[0x11] & 0x10;
}

static bool lps_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    bool is25 = lps_is25(d), inc = false;
    if (wn) {
        /* the LPS25HB increments only with bit 7 of the sub-address set */
        inc = is25 ? (w[0] & 0x80) != 0 : lps_inc(d);
        d->ptr = w[0] & 0x7F;
        d->u[2] = inc;
    } else inc = d->u[2];
    uint8_t c1 = is25 ? 0x20 : 0x10, c2 = is25 ? 0x21 : 0x11;
    uint8_t p = d->ptr;
    for (size_t i = 1; i < wn; i++) {
        uint8_t reg = p, v = w[i];
        if (inc) p++;
        if (reg == 0x0F || (reg >= 0x27 && reg <= 0x2C)) continue;
        if (reg == c2 && (v & 0x04)) { lps_reset(d); return !rn; }     /* SWRESET */
        if (reg == c2 && (v & 0x01)) { d->regs[reg] = v & 0xFE; lps_measure(d); continue; }   /* ONE_SHOT */
        d->regs[reg] = v;
        if (reg == c1) d->t0 = sim_ms();
    }
    if (!rn) return true;
    if (lps_odr(d) && sim_ms() - d->t0 >= 100 && (!d->u[LPS_LAST] || now32() - d->u[LPS_LAST] >= 50)) lps_measure(d);
    p = d->ptr;
    for (size_t i = 0; i < rn; i++) {
        r[i] = d->regs[p];
        /* P_DA clears with PRESS_OUT_H, T_DA with TEMP_OUT_H (swapped bits on the 25) */
        if (p == 0x2A) d->regs[0x27] &= (uint8_t)~(is25 ? 0x02 : 0x01);
        if (p == 0x2C) d->regs[0x27] &= (uint8_t)~(is25 ? 0x01 : 0x02);
        if (inc) p++;
    }
    return true;
}

static const sim_model_t SIM_LPS22HB = { "lps22hb", 0x5D, lps_reset, lps_xfer };
static const sim_model_t SIM_LPS22HH = { "lps22hh", 0x5D, lps_reset, lps_xfer };
static const sim_model_t SIM_LPS22DF = { "lps22df", 0x5D, lps_reset, lps_xfer };
static const sim_model_t SIM_LPS25HB = { "lps25hb", 0x5D, lps_reset, lps_xfer };

/* ========================================================================== */
/* MS5611 / MS5607: commands, PROM with CRC-4 (TE AN520), 24-bit ADC          */
/* ========================================================================== */

static const uint16_t MS5611_C[7] = { 0x0076, 40127, 36924, 23317, 23282, 33464, 28312 };
static const uint16_t MS5607_C[7] = { 0x0076, 46372, 43981, 29059, 27842, 31553, 28165 };

static uint16_t ms_crc4(const uint16_t *prom)
{
    uint16_t n[8], rem = 0;
    memcpy(n, prom, sizeof n);
    n[7] &= 0xFF00;
    for (int i = 0; i < 16; i++) {
        rem ^= (uint16_t)(i % 2 ? n[i / 2] & 0xFF : n[i / 2] >> 8);
        for (int b = 8; b > 0; b--) rem = (uint16_t)(rem & 0x8000 ? (rem << 1) ^ 0x3000 : rem << 1);
    }
    return (rem >> 12) & 0x0F;
}

typedef struct { const uint16_t *c; bool m07; double d2; } ms_ctx_t;

/* TEMP and the corrected OFF and SENS, double, datasheet second order included */
static double ms_temp(const ms_ctx_t *k, double d2, double *off, double *sens)
{
    const uint16_t *c = k->c;
    double dt = d2 - c[5] * 256.0;
    double t = 2000.0 + dt * c[6] / 8388608.0;
    double o = k->m07 ? c[2] * 131072.0 + c[4] * dt / 64.0 : c[2] * 65536.0 + c[4] * dt / 128.0;
    double s = k->m07 ? c[1] * 65536.0 + c[3] * dt / 128.0 : c[1] * 32768.0 + c[3] * dt / 256.0;
    if (t < 2000.0) {
        double q = (t - 2000.0) * (t - 2000.0), o2, s2;
        if (k->m07) { o2 = 61.0 * q / 16.0; s2 = 2.0 * q; }
        else { o2 = 5.0 * q / 2.0; s2 = 5.0 * q / 4.0; }
        if (t < -1500.0) {
            double q2 = (t + 1500.0) * (t + 1500.0);
            if (k->m07) { o2 += 15.0 * q2; s2 += 8.0 * q2; }
            else { o2 += 7.0 * q2; s2 += 5.5 * q2; }
        }
        t -= dt * dt / 2147483648.0;
        o -= o2;
        s -= s2;
    }
    if (off) *off = o;
    if (sens) *sens = s;
    return t;
}
static double ms_tf(const void *c, double d2) { return ms_temp(c, d2, NULL, NULL); }

enum { MS_CONV, MS_MS, MS_RES, MS_HAS, MS_M07 };

static void ms_reset(sim_emu_t *d)
{
    memset(d->u, 0, sizeof d->u);
    bool m07 = !strcmp(d->m->name, "ms5607");
    d->u[MS_M07] = m07;
    const uint16_t *c = m07 ? MS5607_C : MS5611_C;
    uint16_t prom[8];
    memcpy(prom, c, 7 * sizeof *c);
    prom[7] = 0x5A30;
    prom[7] = (uint16_t)((prom[7] & 0xFFF0) | ms_crc4(prom));
    for (int i = 0; i < 8; i++) d->r16[i] = prom[i];
    d->ptr = 0xFF;
}

static uint32_t ms_convert(sim_emu_t *d, bool pressure)
{
    ms_ctx_t k = { d->u[MS_M07] ? MS5607_C : MS5611_C, d->u[MS_M07], 0 };
    double d2 = invert(ms_tf, &k, room_t(0.50) * 100.0, 0, 16777215);
    if (!pressure) return (uint32_t)lrint(d2);
    double off, sens;
    ms_temp(&k, floor(d2 + 0.5), &off, &sens);
    double d1 = (room_pa(-0.15) * 32768.0 + off) * 2097152.0 / sens;
    return d1 < 0 ? 0 : d1 > 16777215 ? 16777215 : (uint32_t)lrint(d1);
}

static bool ms_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        uint8_t c = w[0];
        d->ptr = c;
        if (c == 0x1E) ms_reset(d);
        else if ((c & 0xE0) == 0x40 && (c & 0x0F) <= 8 && !(c & 1)) {
            static const uint32_t MS[5] = { 1, 2, 3, 5, 10 };
            d->u[MS_CONV] = (c & 0x10) ? 2 : 1;
            d->u[MS_MS] = MS[(c & 0x0F) >> 1];
            d->u[MS_HAS] = 0;
            d->t0 = sim_ms();
        }
    }
    if (!rn) return true;
    uint8_t c = d->ptr;
    memset(r, 0, rn);
    if (c == 0x00) {
        /* the result once; a read before the conversion ends, or a second
         * read, gives 0 */
        if (d->u[MS_CONV] && sim_ms() - d->t0 >= d->u[MS_MS]) {
            uint32_t v = ms_convert(d, d->u[MS_CONV] == 1);
            uint8_t b[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
            for (size_t i = 0; i < rn && i < 3; i++) r[i] = b[i];
        }
        d->u[MS_CONV] = 0;
    } else if ((c & 0xF0) == 0xA0 && !(c & 1)) {
        uint16_t v = d->r16[(c & 0x0E) >> 1];
        if (rn > 0) r[0] = (uint8_t)(v >> 8);
        if (rn > 1) r[1] = (uint8_t)v;
    }
    return true;
}

static const sim_model_t SIM_MS5611 = { "ms5611", 0x77, ms_reset, ms_xfer };
static const sim_model_t SIM_MS5607 = { "ms5607", 0x77, ms_reset, ms_xfer };

/* ========================================================================== */
/* QMP6988: OTP coefficients, the datasheet's conversion (A + S * k / 32767)  */
/* ========================================================================== */

/* raw OTP words */
static const int32_t Q_A0 = -20000, Q_B00 = 480000;        /* 20-bit */
static const int16_t Q_K[10] = { 2345, -1617, -3456, 456, -789, 1011, -1213, 1415, -1234, 1819 };
/*                               bt1   bt2    bp1    b11  bp2   b12   b21    bp3   a1     a2   */
static const double Q_A[10] = { 1.00e-01, 1.20e-08, 3.30e-02, 2.10e-07, -6.30e-10, 2.90e-13, 2.10e-15, 1.30e-16, -6.30e-03, -1.90e-11 };
static const double Q_S[10] = { 9.10e-02, 1.20e-06, 1.90e-02, 1.40e-07, 3.50e-10, 7.60e-13, 1.20e-14, 7.90e-17, 4.30e-04, 1.20e-10 };

static double q_k(int i) { return Q_A[i] + Q_S[i] * Q_K[i] / 32767.0; }
static double q_tr(const void *c, double dt) { (void)c; return Q_A0 / 16.0 + q_k(8) * dt + q_k(9) * dt * dt; }
static double q_pr(const void *c, double dp)
{
    double tr = *(const double *)c;
    return Q_B00 / 16.0 + q_k(0) * tr + q_k(2) * dp + q_k(3) * tr * dp + q_k(1) * tr * tr + q_k(4) * dp * dp +
           q_k(5) * dp * tr * tr + q_k(6) * dp * dp * tr + q_k(7) * dp * dp * dp;
}

static void q_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->u, 0, sizeof d->u);
    uint8_t *o = d->regs + 0xA0;
    uint32_t b00 = (uint32_t)Q_B00 & 0xFFFFF, a0 = (uint32_t)Q_A0 & 0xFFFFF;
    o[0] = (uint8_t)(b00 >> 12); o[1] = (uint8_t)(b00 >> 4);
    for (int i = 0; i < 8; i++) { o[2 + 2 * i] = (uint8_t)((uint16_t)Q_K[i] >> 8); o[3 + 2 * i] = (uint8_t)Q_K[i]; }
    o[18] = (uint8_t)(a0 >> 12); o[19] = (uint8_t)(a0 >> 4);
    o[20] = (uint8_t)((uint16_t)Q_K[8] >> 8); o[21] = (uint8_t)Q_K[8];
    o[22] = (uint8_t)((uint16_t)Q_K[9] >> 8); o[23] = (uint8_t)Q_K[9];
    o[24] = (uint8_t)((b00 & 0x0F) << 4 | (a0 & 0x0F));
    d->regs[0xD1] = 0x5C;
}

static void q_measure(sim_emu_t *d)
{
    uint8_t *r = d->regs;
    double tr = room_t(-0.10) * 256.0;
    double dt = invert(q_tr, NULL, tr, -8388608.0, 8388607.0);
    double dp = invert(q_pr, &tr, room_pa(0.25), -8388608.0, 8388607.0);
    uint32_t rt = (uint32_t)(lrint(dt) + 8388608), rp = (uint32_t)(lrint(dp) + 8388608);
    r[0xF7] = (uint8_t)(rp >> 16); r[0xF8] = (uint8_t)(rp >> 8); r[0xF9] = (uint8_t)rp;
    r[0xFA] = (uint8_t)(rt >> 16); r[0xFB] = (uint8_t)(rt >> 8); r[0xFC] = (uint8_t)rt;
}

static bool q_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) d->ptr = w[0];
    for (size_t i = 1; i < wn; i++) {
        uint8_t reg = (uint8_t)(w[0] + i - 1), v = w[i];
        if (reg == 0xE0) { if (v == 0xE6) { q_reset(d); return !rn; } continue; }
        if (reg == 0xD1 || (reg >= 0xA0 && reg <= 0xB8) || reg >= 0xF7) continue;
        d->regs[reg] = v;
        if (reg == 0xF4) { d->t0 = sim_ms(); d->u[0] = (v & 3) == 1 || (v & 3) == 2; }
    }
    if (!rn) return true;
    int mode = d->regs[0xF4] & 3;
    bool due = sim_ms() - d->t0 >= 30;
    if (mode == 3 && due) q_measure(d);
    else if (d->u[0] && due) { q_measure(d); d->u[0] = 0; d->regs[0xF4] &= 0xFC; }
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(d->ptr + i)];
    return true;
}

static const sim_model_t SIM_QMP6988 = { "qmp6988", 0x70, q_reset, q_xfer };

const sim_model_t *const SIM_M_ENV[] = {
    &SIM_BME680, &SIM_BME688, &SIM_BMP180, &SIM_BMP388, &SIM_BMP390, &SIM_BMP581, &SIM_BMP585,
    &SIM_DPS310, &SIM_SPL06, &SIM_LPS22HB, &SIM_LPS22HH, &SIM_LPS22DF, &SIM_LPS25HB,
    &SIM_MS5611, &SIM_MS5607, &SIM_QMP6988,
    NULL,
};
