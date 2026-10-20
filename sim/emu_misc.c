/*
 * P4OS simulator - emulated misc sensors for the sensor service (sensors_emu.h).
 *
 * MAX30102 / MAX30100: a finger on the sensor, taken away for 12 s every
 *   90 s. The pulse is 72 ± 8 beats a minute (two minutes a cycle), a
 *   systolic dip and a smaller dicrotic one, the IR swinging 1.2 % and the
 *   red 0.8 %, plus a slow breathing wave. The FIFO fills at the configured
 *   rate (sample rate over averaging), 32 deep (16 on the 30100), rolls
 *   over counting what it lost, and FIFO_DATA does not move the register
 *   pointer. The level follows the LED current. The die temperature takes
 *   29 ms, and its enable bit clears when done.
 * MPR121: its power-on values (0x5C = 0x10, 0x5D = 0x24), soft reset
 *   0x80 = 0x63, registers 0x2B..0x7F written only in stop mode (ECR 0),
 *   and a finger walking along the electrodes that ECR enables.
 * Seesaw (soil): module and function bytes, then a read after a stop and a
 *   wait (1 ms; 3 ms for touch). A read too early, or joined to the write
 *   by a repeated start, gets 0xFF. Soil watered every 10 minutes and
 *   drying between.
 * AS5600: its register file, a knob turned back and forth, the magnet
 *   taken away 5 s a minute; a write to 0xFF burns the OTP, and the log
 *   says so.
 * DS3231: time running from 12:00:00, temperature converted every 64 s in
 *   quarters of a degree, the register pointer wrapping after 0x12.
 * PCF8574: a latch per pin; a read is the latch AND what the outside
 *   drives: a button on P0 (pressed half a second every 4 s), a switch on
 *   P1 (10 s each way), a sensor's open-collector output on P5.
 */
#include "sensors_emu.h"
#include "aos_hal.h"

#include <math.h>
#include <string.h>

#define TAU 6.283185307179586

static uint32_t now32(void) { return (uint32_t)sim_ms(); }

/* -------------------------------------------------------------------------- */
/* MAX30102 / MAX30100                                                         */
/* -------------------------------------------------------------------------- */

/* u[]: written samples, read samples, byte in the sample, overflow,
 * temperature done at, the current sample's values (two LEDs), the model */
enum { MX_WR, MX_RD, MX_BYTE, MX_OVF, MX_TEMP, MX_V0, MX_V1, MX_OLD };

static bool mx_old(const sim_emu_t *d) { return d->u[MX_OLD] != 0; }

static void mx_defaults(sim_emu_t *d)
{
    bool old = mx_old(d);
    memset(d->regs, 0, sizeof d->regs);
    for (int i = 0; i < MX_OLD; i++) d->u[i] = 0;
    d->regs[0xFF] = old ? 0x11 : 0x15;
    d->regs[0xFE] = old ? 0x03 : 0x02;
    d->regs[0x00] = 0x01;                           /* PWR_RDY */
    if (old) d->regs[0x07] = 0x00;
    d->t0 = sim_ms();
}

static void mx102_reset(sim_emu_t *d) { d->u[MX_OLD] = 0; mx_defaults(d); }
static void mx100_reset(sim_emu_t *d) { d->u[MX_OLD] = 1; mx_defaults(d); }

static double mx_rate(const sim_emu_t *d)
{
    if (mx_old(d)) {
        static const double SR[8] = { 50, 100, 167, 200, 400, 600, 800, 1000 };
        return SR[(d->regs[0x07] >> 2) & 7];
    }
    static const double SR[8] = { 50, 100, 200, 400, 800, 1000, 1600, 3200 };
    static const double AVG[8] = { 1, 2, 4, 8, 16, 32, 32, 32 };
    return SR[(d->regs[0x0A] >> 2) & 7] / AVG[d->regs[0x08] >> 5];
}

static int mx_mode(const sim_emu_t *d) { return mx_old(d) ? d->regs[0x06] & 7 : d->regs[0x09] & 7; }
static bool mx_running(const sim_emu_t *d) { int m = mx_mode(d); return m == 2 || m == 3 || (!mx_old(d) && m == 7); }
static int mx_leds(const sim_emu_t *d) { return mx_mode(d) == 2 ? 1 : 2; }
static int mx_depth(const sim_emu_t *d) { return mx_old(d) ? 16 : 32; }

static void mx_fifo(sim_emu_t *d)
{
    if (!mx_running(d)) return;
    uint32_t total = (uint32_t)((double)(sim_ms() - d->t0) * mx_rate(d) / 1000.0);
    d->u[MX_WR] = total;
    uint32_t pending = d->u[MX_WR] - d->u[MX_RD];
    uint32_t depth = (uint32_t)mx_depth(d);
    if (pending > depth) {
        uint32_t lost = pending - depth;
        d->u[MX_RD] += lost;
        d->u[MX_BYTE] = 0;
        d->u[MX_OVF] = d->u[MX_OVF] + lost > 31 ? 31 : d->u[MX_OVF] + lost;
    }
    if (d->u[MX_TEMP] && now32() >= d->u[MX_TEMP]) {
        d->u[MX_TEMP] = 0;
        double tc = sim_room_t(sim_now_s()) + 7.0;  /* the LEDs warm it */
        double ti = floor(tc);
        if (mx_old(d)) {
            d->regs[0x16] = (uint8_t)(int8_t)ti;
            d->regs[0x17] = (uint8_t)((tc - ti) * 16.0);
            d->regs[0x06] &= (uint8_t)~0x08;
            d->regs[0x00] |= 0x40;                  /* TEMP_RDY */
        } else {
            d->regs[0x1F] = (uint8_t)(int8_t)ti;
            d->regs[0x20] = (uint8_t)((tc - ti) * 16.0);
            d->regs[0x21] &= (uint8_t)~0x01;
            d->regs[0x01] |= 0x02;                  /* DIE_TEMP_RDY */
        }
    }
}

/* The finger's light at sample i, LED led (0 the first in the FIFO) */
static uint32_t mx_sample(sim_emu_t *d, uint32_t i, int led)
{
    double ts = ((double)d->t0 + (double)i * 1000.0 / mx_rate(d)) / 1000.0;
    bool ir = mx_old(d) ? led == 0 : led == 1;      /* the 30100 gives IR first */
    double ma;
    if (mx_old(d)) {
        static const double MA[16] = { 0, 4.4, 7.6, 11, 14.2, 17.4, 20.8, 24, 27.1, 30.6, 33.8, 37, 40.2, 43.6, 46.8, 50 };
        ma = MA[ir ? d->regs[0x09] & 15 : d->regs[0x09] >> 4];
    } else ma = (ir ? d->regs[0x0D] : d->regs[0x0C]) * 0.2;
    double full = mx_old(d) ? 65535.0 : 262143.0;
    double v;
    if (fmod(ts, 90.0) >= 78.0 || ma == 0) v = 900.0 + sim_noise(40);    /* no finger: the room's light */
    else {
        double per_ma = mx_old(d) ? (ir ? 1050.0 : 1000.0) : (ir ? 16500.0 : 12800.0);
        double dc = ma * per_ma;
        /* the beats: phase is the integral of 72 + 8 sin(2 pi t / 120) a minute */
        double ph = 1.2 * ts - 8.0 * 120.0 / (60.0 * TAU) * cos(TAU * ts / 120.0);
        double f = ph - floor(ph);
        double p = exp(-pow((f - 0.20) / 0.07, 2)) + 0.35 * exp(-pow((f - 0.50) / 0.09, 2));
        v = dc * (1.0 - (ir ? 0.012 : 0.008) * p) * (1.0 + 0.003 * sin(TAU * ts * 0.25)) + sim_noise(25);
    }
    if (v > full) v = full;
    if (v < 0) v = 0;
    return (uint32_t)lrint(v);
}

static uint8_t mx_fifo_byte(sim_emu_t *d)
{
    if (d->u[MX_RD] == d->u[MX_WR]) return 0;       /* empty */
    int bpl = mx_old(d) ? 2 : 3, leds = mx_leds(d);
    int led = (int)d->u[MX_BYTE] / bpl, b = (int)d->u[MX_BYTE] % bpl;
    if (b == 0) d->u[MX_V0 + led] = mx_sample(d, d->u[MX_RD], led);
    uint32_t v = d->u[MX_V0 + led];
    uint8_t out = (uint8_t)(v >> (8 * (bpl - 1 - b)));
    if (++d->u[MX_BYTE] == (uint32_t)(bpl * leds)) { d->u[MX_BYTE] = 0; d->u[MX_RD]++; }
    return out;
}

static void mx_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    bool old = mx_old(d);
    uint8_t mode = old ? 0x06 : 0x09;
    if (reg == mode) {
        if (v & 0x40) { mx_defaults(d); return; }   /* reset */
        bool was = mx_running(d);
        d->regs[reg] = v;
        if (!was && mx_running(d)) { d->t0 = sim_ms(); d->u[MX_WR] = d->u[MX_RD] = d->u[MX_BYTE] = d->u[MX_OVF] = 0; }
        if (old && (v & 0x08)) d->u[MX_TEMP] = now32() + 29;
        return;
    }
    if (old ? (reg >= 0x02 && reg <= 0x04) : (reg >= 0x04 && reg <= 0x06)) {   /* the FIFO's pointers */
        d->u[MX_RD] = d->u[MX_WR];
        d->u[MX_BYTE] = 0;
        d->u[MX_OVF] = 0;
        return;
    }
    if (!old && reg == 0x21) {
        d->regs[0x21] = v & 0x01;
        if (v & 0x01) d->u[MX_TEMP] = now32() + 29;
        return;
    }
    if (reg >= 0xFE || reg == 0x00 || (!old && reg == 0x01)) return;    /* ids and interrupt status: read only */
    d->regs[reg] = v;
}

static uint8_t mx_rd(sim_emu_t *d, uint8_t reg)
{
    bool old = mx_old(d);
    uint32_t mask = (uint32_t)mx_depth(d) - 1;
    if (old) {
        switch (reg) {
        case 0x02: return (uint8_t)(d->u[MX_WR] & mask);
        case 0x03: return (uint8_t)(d->u[MX_OVF] > 15 ? 15 : d->u[MX_OVF]);
        case 0x04: return (uint8_t)(d->u[MX_RD] & mask);
        case 0x00: { uint8_t v = d->regs[0]; d->regs[0] = 0; return v; }   /* read clears */
        default: return d->regs[reg];
        }
    }
    switch (reg) {
    case 0x04: return (uint8_t)(d->u[MX_WR] & mask);
    case 0x05: return (uint8_t)d->u[MX_OVF];
    case 0x06: return (uint8_t)(d->u[MX_RD] & mask);
    case 0x00: case 0x01: { uint8_t v = d->regs[reg]; d->regs[reg] = 0; return v; }
    default: return d->regs[reg];
    }
}

static bool mx_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    mx_fifo(d);
    uint8_t fifo = mx_old(d) ? 0x05 : 0x07;
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) mx_write(d, (uint8_t)(w[0] + i - 1), w[i]);
        if (wn > 1) d->ptr = (uint8_t)(w[0] + wn - 1);
    }
    for (size_t i = 0; i < rn; i++) {
        if (d->ptr == fifo) r[i] = mx_fifo_byte(d);      /* FIFO_DATA keeps the pointer */
        else r[i] = mx_rd(d, d->ptr++);
    }
    return true;
}

static const sim_model_t M_MAX30102 = { "max30102", 0x57, mx102_reset, mx_xfer };
static const sim_model_t M_MAX30100 = { "max30100", 0x57, mx100_reset, mx_xfer };

/* -------------------------------------------------------------------------- */
/* MPR121                                                                      */
/* -------------------------------------------------------------------------- */

static void mpr_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x5C] = 0x10;
    d->regs[0x5D] = 0x24;
}

static void mpr_update(sim_emu_t *d)
{
    int ne = d->regs[0x5E] & 0x0F;
    if (ne > 12) ne = 12;
    uint16_t touch = 0;
    if (ne) {
        double t = sim_now_s();
        int k = (int)(t / 2.0) % 12;                /* a finger walking along the keys */
        if (fmod(t, 2.0) < 0.8) touch |= (uint16_t)(1u << k);
        if (fmod(t, 15.0) < 1.0) touch |= 0x0003;   /* and two at once, now and then */
        touch &= (uint16_t)((1u << ne) - 1);
    }
    d->regs[0x00] = (uint8_t)touch;
    d->regs[0x01] = (uint8_t)(touch >> 8);
    for (int e = 0; e < 13; e++) {
        uint16_t f = (e < ne && (touch >> e & 1)) ? 650 : 720;
        d->regs[0x04 + 2 * e] = (uint8_t)f;
        d->regs[0x05 + 2 * e] = (uint8_t)(f >> 8);
        d->regs[0x1E + e] = e < ne ? 720 >> 2 : 0;
    }
}

static bool mpr_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = (uint8_t)(w[0] + i - 1), v = w[i];
            bool stop = (d->regs[0x5E] & 0x3F) == 0;
            if (reg == 0x80) { if (v == 0x63) mpr_reset(d); }
            else if (reg == 0x5E) d->regs[0x5E] = v;
            else if (reg >= 0x2B && reg <= 0x7F && stop) d->regs[reg] = v;
        }
        if (wn > 1) d->ptr = (uint8_t)(w[0] + wn - 1);
    }
    if (rn) mpr_update(d);
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(d->ptr + i)];
    return true;
}

static const sim_model_t M_MPR121 = { "mpr121", 0x5A, mpr_reset, mpr_xfer };

/* -------------------------------------------------------------------------- */
/* Seesaw soil sensor                                                          */
/* -------------------------------------------------------------------------- */

enum { SS_REQ };

static void ss_reset(sim_emu_t *d)
{
    memset(d->u, 0, sizeof d->u);
    d->resp_ms = 0;
}

static bool ss_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn >= 2) {
        d->u[SS_REQ] = (uint32_t)(w[0] << 8 | w[1]);
        d->resp_ms = sim_ms() + (w[0] == 0x0F ? 3 : 1);
        if (w[0] == 0x00 && w[1] == 0x7F && wn >= 3 && w[2] == 0xFF) d->resp_ms = sim_ms() + 500;   /* SWRST */
    }
    if (!rn) return true;
    memset(r, 0xFF, rn);
    if (wn || sim_ms() < d->resp_ms) return true;  /* no time to answer */
    uint8_t a[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    double t = sim_now_s();
    switch (d->u[SS_REQ]) {
    case 0x0001: a[0] = 0x55; break;                /* HW_ID: SAMD09 */
    case 0x0002: {                                  /* VERSION: product 4026, a date code */
        uint32_t v = 4026u << 16 | 0x2A51;
        a[0] = (uint8_t)(v >> 24); a[1] = (uint8_t)(v >> 16); a[2] = (uint8_t)(v >> 8); a[3] = (uint8_t)v;
        break;
    }
    case 0x0004: {                                  /* TEMP, 16.16 */
        int32_t v = (int32_t)lrint((sim_room_t(t) + 1.5) * 65536.0);
        a[0] = (uint8_t)((uint32_t)v >> 24); a[1] = (uint8_t)((uint32_t)v >> 16); a[2] = (uint8_t)((uint32_t)v >> 8); a[3] = (uint8_t)v;
        break;
    }
    case 0x0F10: {                                  /* TOUCH channel 0: the moisture */
        double m = 350.0 + 665.0 * exp(-fmod(t, 600.0) / 200.0) + sim_noise(4);
        uint16_t v = (uint16_t)lrint(m);
        a[0] = (uint8_t)(v >> 8); a[1] = (uint8_t)v;
        break;
    }
    default: break;
    }
    for (size_t i = 0; i < rn && i < 4; i++) r[i] = a[i];
    return true;
}

static const sim_model_t M_SEESAW = { "seesaw", 0x36, ss_reset, ss_xfer };

/* -------------------------------------------------------------------------- */
/* AS5600                                                                      */
/* -------------------------------------------------------------------------- */

static void as_reset(sim_emu_t *d) { memset(d->regs, 0, sizeof d->regs); }

static void as_update(sim_emu_t *d)
{
    double t = sim_now_s();
    bool mag = fmod(t, 60.0) < 55.0;
    double ang = 180.0 + 160.0 * sin(TAU * t / 24.0) + sim_noise(0.1);
    uint16_t raw = mag ? (uint16_t)((long)lrint(ang / 360.0 * 4096.0) & 0x0FFF) : (uint16_t)((uint32_t)lrint(sim_noise(2000) + 2048) & 0x0FFF);
    d->regs[0x0B] = mag ? 0x20 : 0x10;              /* MD, or ML */
    d->regs[0x0C] = (uint8_t)(raw >> 8); d->regs[0x0D] = (uint8_t)raw;
    d->regs[0x0E] = d->regs[0x0C]; d->regs[0x0F] = d->regs[0x0D];
    d->regs[0x1A] = mag ? 0x60 : 0xFF;              /* AGC */
    uint16_t mg = mag ? 0x0700 : 0x0050;
    d->regs[0x1B] = (uint8_t)(mg >> 8); d->regs[0x1C] = (uint8_t)mg;
}

static bool as_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = (uint8_t)(w[0] + i - 1), v = w[i];
            if (reg >= 0x01 && reg <= 0x08) d->regs[reg] = v;
            else if (reg == 0xFF && (v & 0xC0)) {
                d->regs[0x00] = (uint8_t)(d->regs[0x00] + 1);
                aos_hal_log("sim", "AS5600 at 0x%02X: OTP burned (0xFF <- 0x%02X), ZMCO %u", d->addr, v, d->regs[0x00]);
            }
        }
        if (wn > 1) d->ptr = (uint8_t)(w[0] + wn - 1);
    }
    if (rn) as_update(d);
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(d->ptr + i)];
    return true;
}

static const sim_model_t M_AS5600 = { "as5600", 0x36, as_reset, as_xfer };

/* -------------------------------------------------------------------------- */
/* DS3231                                                                      */
/* -------------------------------------------------------------------------- */

static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static void ds_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->u[0] = 12 * 3600;                            /* 12:00:00 */
    d->t0 = sim_ms();
    d->regs[3] = 1;                                 /* Monday */
    d->regs[4] = 0x19; d->regs[5] = 0x10; d->regs[6] = 0x26;
    d->regs[0x0E] = 0x1C;
    d->regs[0x0F] = 0x88;                           /* OSF (first power-up), EN32kHz */
}

static void ds_update(sim_emu_t *d)
{
    uint32_t s = (uint32_t)(d->u[0] + (sim_ms() - d->t0) / 1000) % 86400u;
    d->regs[0] = bcd((int)(s % 60));
    d->regs[1] = bcd((int)(s / 60 % 60));
    d->regs[2] = bcd((int)(s / 3600));
    double t = sim_now_s();
    double tq = floor(t / 64.0) * 64.0;            /* the last conversion */
    long q = lrint(sim_room_t(tq) * 4.0 + 2.0);   /* quarters; the chip runs a bit warm */
    d->regs[0x11] = (uint8_t)(int8_t)(q >> 2);
    d->regs[0x12] = (uint8_t)((q & 3) << 6);
}

static bool ds_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = (uint8_t)(w[0] % 0x13);
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = d->ptr;
            if (reg <= 0x10) d->regs[reg] = w[i];
            if (reg <= 0x02) {
                d->u[0] = (uint32_t)(unbcd(d->regs[0] & 0x7F) + 60 * unbcd(d->regs[1] & 0x7F) + 3600 * unbcd(d->regs[2] & 0x3F));
                d->t0 = sim_ms();
            }
            d->ptr = (uint8_t)((d->ptr + 1) % 0x13);
        }
    }
    if (rn) ds_update(d);
    for (size_t i = 0; i < rn; i++) {
        r[i] = d->regs[d->ptr];
        d->ptr = (uint8_t)((d->ptr + 1) % 0x13);   /* wraps after 0x12 */
    }
    return true;
}

static const sim_model_t M_DS3231 = { "ds3231", 0x68, ds_reset, ds_xfer };

/* -------------------------------------------------------------------------- */
/* PCF8574                                                                     */
/* -------------------------------------------------------------------------- */

static void pcf_reset(sim_emu_t *d) { d->u[0] = 0xFF; }

static bool pcf_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    for (size_t i = 0; i < wn; i++) d->u[0] = w[i];
    double t = sim_now_s();
    uint8_t ext = 0xFF;
    if (fmod(t, 4.0) < 0.5) ext &= (uint8_t)~0x01;    /* the button */
    if (fmod(t, 20.0) < 10.0) ext &= (uint8_t)~0x02;  /* the switch */
    if (fmod(t, 13.0) < 3.0) ext &= (uint8_t)~0x20;   /* the sensor's output */
    for (size_t i = 0; i < rn; i++) r[i] = (uint8_t)(d->u[0] & ext);
    return true;
}

static const sim_model_t M_PCF8574 = { "pcf8574", 0x20, pcf_reset, pcf_xfer };

/* -------------------------------------------------------------------------- */
/* PCA9685 (not a sensor: the PWM app's outputs over I2C)                      */
/* -------------------------------------------------------------------------- */

/* Its registers as the datasheet has them at power-on (MODE1 0x11 asleep and
 * answering all-call, MODE2 0x04, SUBADR E2 E4 E8, ALLCALLADR E0, PRE_SCALE
 * 0x1E); auto-increment only when MODE1's AI is set; PRE_SCALE only takes a
 * write while asleep; RESTART clears when written 1. What the outputs do is
 * said in the log when a channel changes, to see the app work. */
static void pca_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = 0x11;
    d->regs[0x01] = 0x04;
    d->regs[0x02] = 0xE2;
    d->regs[0x03] = 0xE4;
    d->regs[0x04] = 0xE8;
    d->regs[0x05] = 0xE0;
    for (int ch = 0; ch < 16; ch++) d->regs[0x06 + 4 * ch + 3] = 0x10;   /* full off */
    d->regs[0xFE] = 0x1E;
}

static void pca_say(sim_emu_t *d, int ch)
{
    const uint8_t *r = &d->regs[0x06 + 4 * ch];
    unsigned on = (unsigned)(r[0] | (r[1] & 0x0F) << 8), off = (unsigned)(r[2] | (r[3] & 0x0F) << 8);
    float hz = 25e6f / (4096.0f * (float)(d->regs[0xFE] + 1));
    float duty = (r[3] & 0x10) ? 0.0f : (r[1] & 0x10) ? 1.0f : (float)((off - on) & 0xFFF) / 4096.0f;
    uint32_t key = (uint32_t)(duty * 4096.0f) | (uint32_t)d->regs[0xFE] << 16;
    if (d->u[ch % 16] == key + 1) return;
    d->u[ch % 16] = key + 1;
    aos_hal_log("sim", "PCA9685 0x%02X ch%d: %.1f%% at %.0f Hz (%.0f us high)", d->addr, ch, duty * 100.0f, hz,
                duty * 1e6f / hz);
}

static bool pca_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        bool ai = d->regs[0x00] & 0x20;
        uint8_t reg = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t v = w[i];
            if (reg == 0xFE && !(d->regs[0x00] & 0x10)) { /* awake: ignored */ }
            else if (reg == 0x00) d->regs[0x00] = (uint8_t)(v & 0x7F);          /* RESTART reads back 0 */
            else if (reg >= 0xFA && reg <= 0xFD) {
                d->regs[reg] = v;
                for (int ch = 0; ch < 16; ch++) d->regs[0x06 + 4 * ch + (reg - 0xFA)] = v;
            } else d->regs[reg] = v;
            if (reg >= 0x06 && reg <= 0x45 && (reg - 0x06) % 4 == 3) pca_say(d, (reg - 0x06) / 4);
            if (reg == 0xFD) for (int ch = 0; ch < 16; ch++) pca_say(d, ch);
            if (ai) reg = (uint8_t)(reg + 1);
        }
        if (!rn) return true;
    }
    bool ai = d->regs[0x00] & 0x20;
    uint8_t reg = d->ptr;
    for (size_t i = 0; i < rn; i++) {
        r[i] = d->regs[reg];
        if (ai) reg = (uint8_t)(reg + 1);
    }
    return true;
}

static const sim_model_t M_PCA9685 = { "pca9685", 0x40, pca_reset, pca_xfer };

const sim_model_t *const SIM_M_MISC[] = {
    &M_MAX30102, &M_MAX30100, &M_MPR121, &M_SEESAW, &M_AS5600, &M_DS3231, &M_PCF8574,
    &M_PCA9685,
    NULL,
};
