/*
 * P4OS simulator - emulated light sensors for the sensor service (sensors_emu.h).
 *
 * Each model integrates the bench's light (sim_light) the way its chip
 * does: nothing until it is powered and one integration has passed, then
 * the counts of the last finished integration at the gain and time set,
 * clipped where the chip's ADC clips. The counts come from inverting each
 * datasheet's own formula (TSL2561's floating-point table, ams DN40 for
 * the colour chips, Lite-On's for the LTR parts, Vishay's for the VEMLs),
 * so the drivers' integer or ranging arithmetic is checked against an
 * independent implementation.
 *
 * The bench by a window: the UV index follows the light (about 1.5 to 4),
 * the colour temperature drifts between 3400 and 5200 K, and when a hand
 * covers the sensors (sim_light drops, every 45 s) the APDS-9960 sees it
 * close.
 */
#include "sensors_emu.h"

#include <math.h>
#include <string.h>

#define TAU 6.283185307179586

static double uvi_at(double t) { return sim_light(t) / 150.0; }
static double cct_at(double t) { return 4300.0 + 900.0 * sin(TAU * t / 97.0); }
static bool hand_at(double t) { return fmod(t, 45.0) < 3.0; }
static uint32_t now32(void) { return (uint32_t)sim_ms(); }
static uint32_t clipu(double v, uint32_t max) { return v <= 0 ? 0 : v >= max ? max : (uint32_t)lrint(v); }

/* A white source split into red, green, blue and an IR share, so that ams
 * DN40 gives back the lux and CCT asked for: G'' = 0.136 R' + G' - 0.444 B'
 * = lux x cpl, B'/R' = (CCT - 1391) / 3810, G' = 1.1 R' */
static void rgb_split(double lux, double cct, double cpl, double *r, double *g, double *b, double *c)
{
    double k = (cct - 1391.0) / 3810.0;
    double rp = lux * cpl / (0.136 + 1.1 - 0.444 * k);
    double gp = 1.1 * rp, bp = k * rp, ir = 0.08 * (rp + gp + bp);
    *r = rp + ir; *g = gp + ir; *b = bp + ir;
    *c = rp + gp + bp + ir;             /* (R+G+B-C)/2 = IR */
}

/* -------------------------------------------------------------------------- */
/* VEML7700 / VEML6030 and VEML6075: 16-bit little-endian registers           */
/* -------------------------------------------------------------------------- */

/* u[0] when the ALS was powered or reconfigured */

static void veml_reset(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[0x00] = 0x0001;              /* ALS_SD: shut down until told */
    d->r16[0x07] = d->addr == 0x48 ? 0xD481 : 0xC481;
}

/* The polynomial Vishay gives to correct the response above 1000 lx; the
 * chip reports x where the light is p(x) */
static double veml_poly(double x) { return (((6.0135e-13 * x - 9.3924e-9) * x + 8.1488e-5) * x + 1.0023) * x; }

static void veml_update(sim_emu_t *d)
{
    uint16_t c = d->r16[0];
    if (c & 1) return;                  /* shut down: the registers keep the last */
    static const double GAIN[4] = { 1.0, 2.0, 0.125, 0.25 };
    int itc = (c >> 6) & 0x0F;
    double it = itc == 0x0C ? 25 : itc == 0x08 ? 50 : itc <= 3 ? 100 << itc : 100;
    if (now32() - d->u[0] < it + 3) return;
    double lx = sim_light(sim_now_s());
    double x = lx;
    if (lx > veml_poly(1000.0)) {
        double lo = 1000, hi = 200000;
        for (int i = 0; i < 50; i++) { double m = (lo + hi) / 2; if (veml_poly(m) < lx) lo = m; else hi = m; }
        x = lo;
    }
    double res = 0.0042 * (800.0 / it) * (2.0 / GAIN[(c >> 11) & 3]);
    d->r16[0x04] = (uint16_t)clipu(x / res, 65535);
    d->r16[0x05] = (uint16_t)clipu(1.25 * x / res, 65535);
}

static bool veml_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) d->ptr = w[0];
    if (wn >= 3 && d->ptr <= 0x03) {
        uint16_t v = (uint16_t)(w[1] | w[2] << 8);
        if (d->ptr == 0 && v != d->r16[0]) d->u[0] = now32();   /* a new integration */
        d->r16[d->ptr] = v;
    }
    if (!rn) return true;
    veml_update(d);
    uint16_t v = d->r16[d->ptr];
    for (size_t i = 0; i < rn; i++) r[i] = i == 0 ? (uint8_t)v : i == 1 ? (uint8_t)(v >> 8) : 0xFF;
    return true;
}

static const sim_model_t M_VEML7700 = { "veml7700", 0x10, veml_reset, veml_xfer };
static const sim_model_t M_VEML6030 = { "veml6030", 0x48, veml_reset, veml_xfer };

static void veml75_reset(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[0x00] = 0x0001;
    d->r16[0x0C] = 0x0026;
}

static void veml75_measure(sim_emu_t *d)
{
    uint16_t c = d->r16[0];
    static const double IT[8] = { 50, 100, 200, 400, 800, 800, 800, 800 };
    double sc = IT[(c >> 4) & 7] / 100.0 * ((c & 0x08) ? 0.5 : 1.0);
    double t = sim_now_s(), lx = sim_light(t), uvi = uvi_at(t);
    /* at 100 ms: the two compensation channels, then UVA and UVB so that
     * Vishay's formula gives UVIA = UVIB = the bench's index */
    double c1 = 0.8 * lx, c2 = 0.35 * lx;
    double uva = uvi / 0.001461 + 2.22 * c1 + 1.33 * c2;
    double uvb = uvi / 0.002591 + 2.95 * c1 + 1.74 * c2;
    d->r16[0x07] = (uint16_t)clipu(uva * sc, 65535);
    d->r16[0x09] = (uint16_t)clipu(uvb * sc, 65535);
    d->r16[0x0A] = (uint16_t)clipu(c1 * sc, 65535);
    d->r16[0x0B] = (uint16_t)clipu(c2 * sc, 65535);
}

static bool veml75_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) d->ptr = w[0];
    if (wn >= 3 && d->ptr == 0) {
        uint16_t v = (uint16_t)(w[1] | w[2] << 8);
        if ((v & 0x06) == 0x06) { d->u[1] = 1; v &= (uint16_t)~0x04; }  /* active force + trigger: one */
        if (v != d->r16[0] || d->u[1]) d->u[0] = now32();
        d->r16[0] = v;
    }
    if (!rn) return true;
    uint16_t c = d->r16[0];
    static const uint32_t IT[8] = { 50, 100, 200, 400, 800, 800, 800, 800 };
    if (!(c & 1) && now32() - d->u[0] >= IT[(c >> 4) & 7] + 3 && (!(c & 0x02) || d->u[1])) {
        veml75_measure(d);
        d->u[1] = 0;
    }
    uint16_t v = d->r16[d->ptr];
    for (size_t i = 0; i < rn; i++) r[i] = i == 0 ? (uint8_t)v : i == 1 ? (uint8_t)(v >> 8) : 0xFF;
    return true;
}

static const sim_model_t M_VEML6075 = { "veml6075", 0x10, veml75_reset, veml75_xfer };

/* -------------------------------------------------------------------------- */
/* TSL2561: command byte, byte or word protocol                                */
/* -------------------------------------------------------------------------- */

/* regs[0x0..0xF]; u[0] the WORD/BLOCK bits of the last command, u[1] when
 * the ADC started (power up, timing change), u[2]/u[3] the latched high
 * bytes (bit 8 set while one waits to be read) */

static void tsl61_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x1] = 0x02;                /* 402 ms, 1x */
    d->regs[0xA] = 0x50;                /* TSL2561T, revision 0 */
    d->ptr = 0;
}

static void tsl61_update(sim_emu_t *d)
{
    if ((d->regs[0] & 3) != 3) return;  /* powered down: the ADC stops */
    int integ = d->regs[1] & 3;
    if (integ == 3) return;             /* manual integration: not modelled */
    static const double MS[3] = { 13.7, 101.0, 402.0 };
    static const double CYC[3] = { 11.0 / 322.0, 81.0 / 322.0, 1.0 };
    static const uint32_t MAX[3] = { 5047, 37177, 65535 };
    if (now32() - d->u[1] < MS[integ]) return;
    double t = sim_now_s(), lx = sim_light(t);
    /* the datasheet's T-package formula, 0 < CH1/CH0 <= 0.50, at 402 ms
     * and 16x: lux = 0.0304 CH0 - 0.062 CH0 (CH1/CH0)^1.4 */
    double ratio = 0.32 + 0.06 * sin(TAU * t / 70.0);
    double ch0 = lx / (0.0304 - 0.062 * pow(ratio, 1.4)), ch1 = ratio * ch0;
    double k = CYC[integ] * ((d->regs[1] & 0x10) ? 1.0 : 1.0 / 16.0);
    uint32_t c0 = clipu(ch0 * k, MAX[integ]), c1 = clipu(ch1 * k, MAX[integ]);
    d->regs[0xC] = (uint8_t)c0; d->regs[0xD] = (uint8_t)(c0 >> 8);
    d->regs[0xE] = (uint8_t)c1; d->regs[0xF] = (uint8_t)(c1 >> 8);
}

static uint8_t tsl61_get(sim_emu_t *d, int reg)
{
    reg &= 0x0F;
    /* reading a low data byte latches its high byte */
    if (reg == 0xC) { d->u[2] = 0x100 | d->regs[0xD]; return d->regs[0xC]; }
    if (reg == 0xE) { d->u[3] = 0x100 | d->regs[0xF]; return d->regs[0xE]; }
    if (reg == 0xD && (d->u[2] & 0x100)) { uint8_t v = (uint8_t)d->u[2]; d->u[2] = 0; return v; }
    if (reg == 0xF && (d->u[3] & 0x100)) { uint8_t v = (uint8_t)d->u[3]; d->u[3] = 0; return v; }
    if (reg == 0x0) return d->regs[0] & 0x03;
    return d->regs[reg];
}

static bool tsl61_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        uint8_t c = w[0];
        if (!(c & 0x80)) return true;  /* not a command: ignored */
        d->ptr = c & 0x0F;
        d->u[0] = c & 0x30;
        for (size_t i = 1; i < wn; i++) {
            int reg = (d->ptr + (d->u[0] ? (int)i - 1 : 0)) & 0x0F;
            uint8_t v = w[i];
            if (reg == 0x0) {
                if ((v & 3) == 3 && (d->regs[0] & 3) != 3) d->u[1] = now32();
                d->regs[0] = v & 3;
            } else if (reg == 0x1) {
                if (v != d->regs[1]) d->u[1] = now32();
                d->regs[1] = v & 0x1B;
            } else if (reg >= 0x2 && reg <= 0x6) d->regs[reg] = v;
        }
    }
    if (!rn) return true;
    tsl61_update(d);
    for (size_t i = 0; i < rn; i++) r[i] = tsl61_get(d, d->ptr + (d->u[0] ? (int)i : 0));
    return true;
}

static const sim_model_t M_TSL2561 = { "tsl2561", 0x39, tsl61_reset, tsl61_xfer };

/* -------------------------------------------------------------------------- */
/* TSL2591: command byte, normal transactions auto-increment                  */
/* -------------------------------------------------------------------------- */

/* u[1] when the ALS started; resp[0..3] the four data bytes latched */

static void tsl91_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x12] = 0x50;
    d->ptr = 0;
}

static bool tsl91_on(const sim_emu_t *d) { return (d->regs[0] & 0x03) == 0x03; }
static uint32_t tsl91_ms(const sim_emu_t *d) { return 100u * (uint32_t)((d->regs[1] & 7) + 1); }

static void tsl91_latch(sim_emu_t *d)
{
    memset(d->resp, 0, 4);
    if (!tsl91_on(d) || now32() - d->u[1] < tsl91_ms(d)) return;
    static const double GAIN[4] = { 1.0, 25.0, 428.0, 9876.0 };
    double ms = tsl91_ms(d), cpl = ms * GAIN[(d->regs[1] >> 4) & 3] / 408.0;
    /* Adafruit's lux = (C0 - C1)(1 - C1/C0) / cpl = C0 (1 - q)^2 / cpl */
    double q = 0.18, ch0 = sim_light(sim_now_s()) * cpl / ((1 - q) * (1 - q));
    uint32_t max = ms == 100 ? 37887 : 65535;
    uint32_t c0 = clipu(ch0, max), c1 = clipu(q * ch0, max);
    d->resp[0] = (uint8_t)c0; d->resp[1] = (uint8_t)(c0 >> 8);
    d->resp[2] = (uint8_t)c1; d->resp[3] = (uint8_t)(c1 >> 8);
}

static uint8_t tsl91_get(sim_emu_t *d, int reg)
{
    if (reg == 0x13) return tsl91_on(d) && now32() - d->u[1] >= tsl91_ms(d) ? 0x01 : 0x00;
    if (reg == 0x14) tsl91_latch(d);
    if (reg >= 0x14 && reg <= 0x17) return d->resp[reg - 0x14];
    return reg < 0x20 ? d->regs[reg] : 0;
}

static bool tsl91_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        uint8_t c = w[0];
        if (!(c & 0x80)) return true;
        int tr = (c >> 5) & 3;
        if (tr == 3) return true;       /* special function: clears interrupts */
        if (tr != 1) return true;       /* reserved transaction */
        d->ptr = c & 0x1F;
        for (size_t i = 1; i < wn; i++) {
            int reg = (d->ptr + (int)i - 1) & 0x1F;
            uint8_t v = w[i];
            if (reg == 0x00) {
                if ((v & 3) == 3 && !tsl91_on(d)) d->u[1] = now32();
                d->regs[0] = v & 0xD3;
            } else if (reg == 0x01) {
                if (v & 0x80) { tsl91_reset(d); continue; }
                if (v != d->regs[1]) d->u[1] = now32();
                d->regs[1] = v & 0x37;
            } else if (reg >= 0x04 && reg <= 0x0C) d->regs[reg] = v;
        }
    }
    for (size_t i = 0; i < rn; i++) r[i] = tsl91_get(d, (d->ptr + (int)i) & 0x1F);
    return true;
}

static const sim_model_t M_TSL2591 = { "tsl2591", 0x29, tsl91_reset, tsl91_xfer };

/* -------------------------------------------------------------------------- */
/* TCS34725: command byte, repeated-byte or auto-increment                     */
/* -------------------------------------------------------------------------- */

/* u[0] auto-increment, u[1] when the RGBC started */

static void tcs_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x01] = 0xFF;
    d->regs[0x03] = 0xFF;
    d->regs[0x12] = 0x44;
    d->ptr = 0;
}

static bool tcs_on(const sim_emu_t *d) { return (d->regs[0] & 0x03) == 0x03; }
static double tcs_ms(const sim_emu_t *d) { return 2.4 * (256 - d->regs[1]); }

static void tcs_data(sim_emu_t *d)
{
    if (!tcs_on(d) || now32() - d->u[1] < tcs_ms(d) + 2.4) return;
    static const double GAIN[4] = { 1, 4, 16, 60 };
    int cycles = 256 - d->regs[1];
    double t = sim_now_s(), r, g, b, c;
    rgb_split(sim_light(t), cct_at(t), tcs_ms(d) * GAIN[d->regs[0x0F] & 3] / 310.0, &r, &g, &b, &c);
    uint32_t max = cycles >= 64 ? 65535 : 1024u * (uint32_t)cycles;
    uint32_t v[4] = { clipu(c, max), clipu(r, max), clipu(g, max), clipu(b, max) };
    for (int i = 0; i < 4; i++) { d->regs[0x14 + 2 * i] = (uint8_t)v[i]; d->regs[0x15 + 2 * i] = (uint8_t)(v[i] >> 8); }
}

static bool tcs_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        uint8_t c = w[0];
        if (!(c & 0x80)) return true;
        int type = (c >> 5) & 3;
        if (type == 3) return true;     /* special function: clear interrupt */
        d->ptr = c & 0x1F;
        d->u[0] = type == 1;
        for (size_t i = 1; i < wn; i++) {
            int reg = (d->ptr + (d->u[0] ? (int)i - 1 : 0)) & 0x1F;
            uint8_t v = w[i];
            if (reg == 0x00) {
                if ((v & 3) == 3 && !tcs_on(d)) d->u[1] = now32();
                d->regs[0] = v & 0x1B;
            } else if (reg == 0x01 || reg == 0x0F) {
                if (v != d->regs[reg]) d->u[1] = now32();
                d->regs[reg] = reg == 0x0F ? (v & 3) : v;
            } else if (reg <= 0x0D && reg != 0x02) d->regs[reg] = v;
        }
    }
    if (!rn) return true;
    if (d->ptr >= 0x14) tcs_data(d);    /* the data, latched for this read */
    for (size_t i = 0; i < rn; i++) {
        int reg = (d->ptr + (d->u[0] ? (int)i : 0)) & 0x1F;
        if (reg == 0x13) r[i] = tcs_on(d) && now32() - d->u[1] >= tcs_ms(d) + 2.4 ? 0x01 : 0x00;
        else r[i] = d->regs[reg];
    }
    return true;
}

static const sim_model_t M_TCS34725 = { "tcs34725", 0x29, tcs_reset, tcs_xfer };

/* -------------------------------------------------------------------------- */
/* APDS-9960: 8-bit registers with auto-increment                              */
/* -------------------------------------------------------------------------- */

/* u[1] when the ALS started, u[2] when proximity started */

static void apds_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x81] = 0xFF;
    d->regs[0x83] = 0xFF;
    d->regs[0x8E] = 0x40;
    d->regs[0x90] = 0x01;
    d->regs[0x92] = 0xAB;
    d->ptr = 0;
}

static double apds_ms(const sim_emu_t *d) { return 2.78 * (256 - d->regs[0x81]); }
static bool apds_als(const sim_emu_t *d) { return (d->regs[0x80] & 0x03) == 0x03 && now32() - d->u[1] >= apds_ms(d) + 6; }
static bool apds_prox(const sim_emu_t *d) { return (d->regs[0x80] & 0x05) == 0x05 && now32() - d->u[2] >= 10; }

static void apds_data(sim_emu_t *d)
{
    double t = sim_now_s();
    if (apds_als(d)) {
        static const double GAIN[4] = { 1, 4, 16, 64 };
        int cycles = 256 - d->regs[0x81];
        double r, g, b, c;
        rgb_split(sim_light(t), cct_at(t), apds_ms(d) * GAIN[d->regs[0x8F] & 3] / 310.0, &r, &g, &b, &c);
        uint32_t max = 1025u * (uint32_t)cycles > 65535 ? 65535 : 1025u * (uint32_t)cycles;
        uint32_t v[4] = { clipu(c, max), clipu(r, max), clipu(g, max), clipu(b, max) };
        for (int i = 0; i < 4; i++) { d->regs[0x94 + 2 * i] = (uint8_t)v[i]; d->regs[0x95 + 2 * i] = (uint8_t)(v[i] >> 8); }
    }
    if (apds_prox(d)) {
        /* a hand at a few centimetres, or the cover glass's crosstalk */
        static const double PG[4] = { 1, 2, 4, 8 }, LD[4] = { 1, 0.5, 0.25, 0.125 };
        double near = hand_at(t) ? 210.0 : 5.0;
        double v = near * PG[(d->regs[0x8F] >> 2) & 3] / 4.0 * LD[d->regs[0x8F] >> 6] * (((d->regs[0x8E] & 0x3F) + 1) / 8.0) + sim_noise(1.5);
        d->regs[0x9C] = (uint8_t)clipu(v, 255);
    }
}

static bool apds_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = (uint8_t)(d->ptr + i - 1), v = w[i];
            if (reg == 0x92 || reg == 0x93 || (reg >= 0x94 && reg <= 0x9C)) continue;   /* read-only */
            if (reg == 0x80) {
                if ((v & 3) == 3 && (d->regs[0x80] & 3) != 3) d->u[1] = now32();
                if ((v & 5) == 5 && (d->regs[0x80] & 5) != 5) d->u[2] = now32();
            }
            if ((reg == 0x81 || reg == 0x8F) && v != d->regs[reg]) d->u[1] = now32();
            d->regs[reg] = v;
        }
    }
    if (!rn) return true;
    apds_data(d);
    for (size_t i = 0; i < rn; i++) {
        uint8_t reg = (uint8_t)(d->ptr + i);
        r[i] = reg == 0x93 ? (uint8_t)((apds_als(d) ? 0x01 : 0) | (apds_prox(d) ? 0x02 : 0)) : d->regs[reg];
    }
    return true;
}

static const sim_model_t M_APDS9960 = { "apds9960", 0x39, apds_reset, apds_xfer };

/* -------------------------------------------------------------------------- */
/* LTR390-UV: 8-bit registers, one mode at a time                              */
/* -------------------------------------------------------------------------- */

/* u[1] when the measurements started, u[2] how many have finished */

static void ltr390_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x04] = 0x22;
    d->regs[0x05] = 0x01;
    d->regs[0x06] = 0xB2;
    d->regs[0x07] = 0x20;               /* power-on status */
    d->regs[0x19] = 0x10;
    d->regs[0x21] = d->regs[0x22] = d->regs[0x23] = 0xFF;
    d->ptr = 0;
}

static void ltr390_update(sim_emu_t *d)
{
    if (!(d->regs[0] & 0x02)) return;
    static const double RES_MS[8] = { 400, 200, 100, 50, 25, 12.5, 100, 100 };
    static const int BITS[8] = { 20, 19, 18, 17, 16, 13, 18, 18 };
    static const double RATE[8] = { 25, 50, 100, 200, 500, 1000, 2000, 2000 };
    static const double GAIN[8] = { 1, 3, 6, 9, 18, 3, 3, 3 };
    int res = (d->regs[0x04] >> 4) & 7;
    double period = RATE[d->regs[0x04] & 7] > RES_MS[res] ? RATE[d->regs[0x04] & 7] : RES_MS[res];
    uint32_t n = (uint32_t)((now32() - d->u[1]) / period);
    if (n <= d->u[2]) return;
    d->u[2] = n;
    double t = sim_now_s(), g = GAIN[d->regs[0x05] & 7], it = RES_MS[res] / 100.0;
    uint32_t max = (1u << BITS[res]) - 1, v;
    int at;
    if (d->regs[0] & 0x08) {            /* UVS: 2300 counts a UVI at 18x and 400 ms */
        v = clipu(uvi_at(t) * 2300.0 * g / 18.0 * it / 4.0, max);
        at = 0x10;
    } else {                            /* ALS: lux = 0.6 ALS / (gain x int) */
        v = clipu(sim_light(t) * g * it / 0.6, max);
        at = 0x0D;
    }
    d->regs[at] = (uint8_t)v; d->regs[at + 1] = (uint8_t)(v >> 8); d->regs[at + 2] = (uint8_t)(v >> 16 & 0x0F);
    d->regs[0x07] |= 0x08;
}

static bool ltr390_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = (uint8_t)(d->ptr + i - 1), v = w[i];
            if (reg == 0x00) {
                if (v & 0x10) { ltr390_reset(d); continue; }
                if ((v & 0x0A) != (d->regs[0] & 0x0A)) { d->u[1] = now32(); d->u[2] = 0; }
                d->regs[0] = v & 0x0A;
            } else if (reg == 0x04 || reg == 0x05) {
                if (v != d->regs[reg]) { d->u[1] = now32(); d->u[2] = 0; }
                d->regs[reg] = v;
            } else if (reg == 0x19 || reg == 0x1A || (reg >= 0x21 && reg <= 0x26)) d->regs[reg] = v;
        }
    }
    if (!rn) return true;
    ltr390_update(d);
    for (size_t i = 0; i < rn; i++) {
        uint8_t reg = (uint8_t)(d->ptr + i);
        r[i] = d->regs[reg];
        if (reg == 0x07) d->regs[0x07] &= (uint8_t)~0x38;   /* read clears the status bits */
    }
    return true;
}

static const sim_model_t M_LTR390 = { "ltr390", 0x53, ltr390_reset, ltr390_xfer };

/* -------------------------------------------------------------------------- */
/* OPT3001: 16-bit big-endian registers                                         */
/* -------------------------------------------------------------------------- */

/* u[1] when conversions started, u[2] how many have finished */

static void opt_reset(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[0x01] = 0xC810;
    d->r16[0x02] = 0xC000;
    d->r16[0x03] = 0xBFFF;
    d->r16[0x7E] = 0x5449;
    d->r16[0x7F] = 0x3001;
    d->ptr = 0;
}

static void opt_update(sim_emu_t *d)
{
    uint16_t c = d->r16[1];
    if (!(c & 0x0600)) return;
    uint32_t ct = (c & 0x0800) ? 800 : 100;
    uint32_t n = (now32() - d->u[1]) / ct;
    if (n <= d->u[2]) return;
    d->u[2] = n;
    double lx = sim_light(sim_now_s());
    int rn = c >> 12, e;
    double m;
    if (rn == 0x0C) {                   /* automatic full scale */
        for (e = 0; e < 11 && lx / (0.01 * (1 << e)) > 4095; e++) {}
    } else e = rn > 11 ? 11 : rn;
    m = lx / (0.01 * (1 << e));
    c &= (uint16_t)~0x0100;
    if (m > 4095) { m = 4095; c |= 0x0100; }
    d->r16[0] = (uint16_t)(e << 12 | clipu(m, 4095));
    c |= 0x0080;                        /* CRF */
    if ((c & 0x0600) == 0x0200) c &= (uint16_t)~0x0600;   /* single shot: back to shutdown */
    d->r16[1] = c;
}

static bool opt_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) d->ptr = w[0];
    if (wn >= 3) {
        uint16_t v = (uint16_t)(w[1] << 8 | w[2]);
        if (d->ptr == 0x01) {
            uint16_t old = d->r16[1];
            d->r16[1] = (uint16_t)((v & 0xFE1F) | (old & 0x0160));
            d->r16[1] &= (uint16_t)~0x0080;
            if ((v & 0x0600) && ((old & 0x0600) != (v & 0x0600) || (old & 0xF800) != (v & 0xF800))) { d->u[1] = now32(); d->u[2] = 0; }
        } else if (d->ptr == 0x02 || d->ptr == 0x03) d->r16[d->ptr] = v;
    }
    if (!rn) return true;
    opt_update(d);
    uint8_t p = d->ptr;
    uint16_t v = p <= 0x03 || p == 0x7E || p == 0x7F ? d->r16[p] : 0x0000;
    if (p == 0x01) d->r16[1] &= (uint16_t)~0x0080;   /* reading the configuration clears CRF */
    for (size_t i = 0; i < rn; i++) r[i] = i == 0 ? (uint8_t)(v >> 8) : i == 1 ? (uint8_t)v : 0xFF;
    return true;
}

static const sim_model_t M_OPT3001 = { "opt3001", 0x44, opt_reset, opt_xfer };

/* -------------------------------------------------------------------------- */
/* Si1145: registers, a command register and a parameter RAM                   */
/* -------------------------------------------------------------------------- */

/* resp[0..31] the parameter RAM; u[0] autonomous ALS on, u[1] since when */

static void si_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->resp, 0, sizeof d->resp);
    d->regs[0x00] = 0x45;
    d->regs[0x02] = 0x08;
    d->regs[0x30] = 0x01;               /* sleeping */
    d->resp[0x10] = 0x70;               /* ALS_VIS_ADC_COUNTER */
    d->resp[0x1D] = 0x70;               /* ALS_IR_ADC_COUNTER */
    d->resp[0x0E] = 0x00;
    d->u[0] = 0;
    d->ptr = 0;
}

static void si_measure(sim_emu_t *d)
{
    double t = sim_now_s(), lx = sim_light(t);
    uint8_t ch = d->resp[0x01];
    if (ch & 0x10) { uint32_t v = clipu(256 + 0.5 * lx, 65535); d->regs[0x22] = (uint8_t)v; d->regs[0x23] = (uint8_t)(v >> 8); }
    if (ch & 0x20) { uint32_t v = clipu(253 + 0.22 * lx, 65535); d->regs[0x24] = (uint8_t)v; d->regs[0x25] = (uint8_t)(v >> 8); }
    if (ch & 0x80) {
        /* the index x 100 only with the coefficients the datasheet gives */
        bool coef = d->regs[0x13] == 0x29 && d->regs[0x14] == 0x89 && d->regs[0x15] == 0x02 && d->regs[0x16] == 0x00;
        uint32_t v = coef ? clipu(uvi_at(t) * 100.0, 65535) : 0;
        d->regs[0x2C] = (uint8_t)v; d->regs[0x2D] = (uint8_t)(v >> 8);
    }
}

static void si_command(sim_emu_t *d, uint8_t c)
{
    if (c == 0x01) { si_reset(d); return; }
    if (d->regs[0x07] != 0x17) return;  /* no HW_KEY: it does not run */
    uint8_t *resp = &d->regs[0x20];
    if (c == 0x00) { *resp = 0; return; }
    if ((c & 0xE0) == 0xA0) { d->resp[c & 0x1F] = d->regs[0x17]; d->regs[0x2E] = d->regs[0x17]; }
    else if ((c & 0xE0) == 0x80) d->regs[0x2E] = d->resp[c & 0x1F];
    else if (c == 0x06 || c == 0x07) si_measure(d);
    else if (c == 0x0E || c == 0x0F) { d->u[0] = 1; d->u[1] = now32(); }
    else if (c == 0x0A || c == 0x0B) d->u[0] = 0;
    *resp = (uint8_t)((*resp + 1) & 0x0F);
}

static bool si_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = (uint8_t)(d->ptr + i - 1);
            if (reg == 0x18) si_command(d, w[i]);
            else if (reg == 0x21) d->regs[0x21] &= (uint8_t)~w[i];   /* write 1 to clear */
            else if (reg >= 0x03 && reg <= 0x17) d->regs[reg] = w[i];
        }
    }
    if (!rn) return true;
    uint32_t rate = (uint32_t)(d->regs[0x08] | d->regs[0x09] << 8);
    if (d->u[0] && rate && (now32() - d->u[1]) * 1000u >= rate * 31u) si_measure(d);
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(d->ptr + i)];
    return true;
}

static const sim_model_t M_SI1145 = { "si1145", 0x60, si_reset, si_xfer };

/* -------------------------------------------------------------------------- */
/* MAX44009: 8-bit registers, no auto-increment                                */
/* -------------------------------------------------------------------------- */

static void max44_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x02] = 0x03;
    d->regs[0x05] = 0xFF;
    d->regs[0x06] = 0x00;
    d->regs[0x07] = 0xFF;
    d->ptr = 0;
    d->u[2] = 0xFFFFFFFF;
}

/* A conversion every 800 ms (100 ms with CONT), auto-ranged so the
 * mantissa fits 8 bits */
static void max44_update(sim_emu_t *d)
{
    uint32_t period = (d->regs[0x02] & 0x80) ? 100 : 800;
    uint32_t n = now32() / period;
    if (n == d->u[2]) return;
    d->u[2] = n;
    double lx = sim_light(sim_now_s());
    int e = 0;
    while (e < 14 && lx / (0.045 * (1 << e)) > 255) e++;
    uint32_t m = clipu(lx / (0.045 * (1 << e)), 255);
    d->regs[0x03] = (uint8_t)(e << 4 | m >> 4);
    d->regs[0x04] = (uint8_t)(m & 0x0F);
}

static bool max44_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) d->ptr = w[0];
    if (wn >= 2) {
        uint8_t reg = d->ptr;
        if (reg == 0x01 || reg == 0x02 || (reg >= 0x05 && reg <= 0x07)) d->regs[reg] = w[1];
    }
    if (!rn) return true;
    max44_update(d);
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[d->ptr];
    if (d->ptr == 0x00) d->regs[0x00] = 0;   /* interrupt status: cleared by reading */
    return true;
}

static const sim_model_t M_MAX44009 = { "max44009", 0x4A, max44_reset, max44_xfer };

/* -------------------------------------------------------------------------- */
/* LTR-329ALS: 8-bit registers with auto-increment                              */
/* -------------------------------------------------------------------------- */

/* u[1] when measuring started, u[2] how many measurements have finished */

static void ltr329_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x85] = 0x03;
    d->regs[0x86] = 0xA0;
    d->regs[0x87] = 0x05;
    d->ptr = 0;
}

static void ltr329_update(sim_emu_t *d)
{
    if (!(d->regs[0x80] & 0x01)) return;
    static const double INT_MS[8] = { 100, 50, 200, 400, 150, 250, 300, 350 };
    static const double RATE[8] = { 50, 100, 200, 500, 1000, 2000, 2000, 2000 };
    static const double GAIN[8] = { 1, 2, 4, 8, 1, 1, 48, 96 };
    uint8_t mr = d->regs[0x85];
    double it = INT_MS[(mr >> 3) & 7], per = RATE[mr & 7] > it ? RATE[mr & 7] : it;
    if (now32() - d->u[1] < 10) return;                  /* wake-up */
    uint32_t n = (uint32_t)((now32() - d->u[1] - 10) / per);
    if (n < 1 || n <= d->u[2]) return;
    d->u[2] = n;
    int gc = (d->regs[0x80] >> 2) & 7;
    /* the datasheet's lux, ratio CH1/(CH0+CH1) < 0.45:
     * lux = (1.7743 CH0 + 1.1059 CH1) / gain / int; CH1 = 0.3 CH0 */
    double q = 0.3, ch0 = sim_light(sim_now_s()) * GAIN[gc] * (it / 100.0) / (1.7743 + 1.1059 * q);
    uint32_t c0 = clipu(ch0, 65535), c1 = clipu(q * ch0, 65535);
    d->regs[0x88] = (uint8_t)c1; d->regs[0x89] = (uint8_t)(c1 >> 8);
    d->regs[0x8A] = (uint8_t)c0; d->regs[0x8B] = (uint8_t)(c0 >> 8);
    d->regs[0x8C] = (uint8_t)((c0 >= 65535 || c1 >= 65535 ? 0x80 : 0) | gc << 4 | 0x04);
}

static bool ltr329_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = (uint8_t)(d->ptr + i - 1), v = w[i];
            if (reg == 0x80) {
                if (v & 0x02) { ltr329_reset(d); continue; }
                if (v != d->regs[0x80]) { d->u[1] = now32(); d->u[2] = 0; }
                d->regs[0x80] = v & 0x1D;
            } else if (reg == 0x85) {
                if (v != d->regs[0x85]) { d->u[1] = now32(); d->u[2] = 0; }
                d->regs[0x85] = v & 0x3F;
            }
        }
    }
    if (!rn) return true;
    ltr329_update(d);
    for (size_t i = 0; i < rn; i++) {
        uint8_t reg = (uint8_t)(d->ptr + i);
        r[i] = d->regs[reg];
        if (reg == 0x8B) d->regs[0x8C] &= (uint8_t)~0x04;   /* the data read: not new any more */
    }
    return true;
}

static const sim_model_t M_LTR329 = { "ltr329", 0x29, ltr329_reset, ltr329_xfer };

const sim_model_t *const SIM_M_LIGHT[] = {
    &M_VEML7700, &M_VEML6030, &M_VEML6075, &M_TSL2561, &M_TSL2591, &M_TCS34725, &M_APDS9960,
    &M_LTR390, &M_OPT3001, &M_SI1145, &M_MAX44009, &M_LTR329,
    NULL,
};
