/*
 * P4OS simulator - emulated dist sensors for the sensor service (sensors_emu.h).
 *
 * What the distance sensors on the bench see: a hand that comes and goes
 * in front of them, and every 70 s, for 6 s, nothing at all (they look at
 * the ceiling, beyond their range).
 *
 * VL53L0X: 8-bit registers with ST's page register (0xFF): page 0 is the
 *   real register file; the other pages are folded into one (the stop
 *   variable at 0x91, the SPAD info at 0x92, the SPAD discovery at 0x83,
 *   which reports done 2 ms after it is started). SYSRANGE_START starts a
 *   ranging (30 ms, or 5 ms for a VHV/phase calibration), with the new
 *   sample in RESULT_INTERRUPT_STATUS and the range at 0x1E; nothing in
 *   front gives 8190, as the chip does.
 * VL53L1X: 16-bit indexes, the model id 0xEACC at 0x010F, booted in
 *   0x00E5; 0x0087 = 0x40 ranges every 100 ms, each result held until
 *   0x0086 clears it, data ready on GPIO__TIO_HV_STATUS bit 0 with the
 *   polarity of GPIO_HV_MUX__CTRL bit 4. Raw status 9 valid, 4 (signal
 *   fail) for nothing in front. An 8-bit index (one byte) is refused.
 * VL6180X: 16-bit indexes, model id 0xB4 at 0x000, fresh out of reset at
 *   0x016; single-shot range (8 ms, 0..200 mm, error 6 beyond) and ALS
 *   (the integration time, 0.32 lx a count at gain 1, scaled by the gain
 *   and the time), each with its bits in RESULT__INTERRUPT_STATUS_GPIO and
 *   cleared by SYSTEM__INTERRUPT_CLEAR.
 * URM09: its 9 registers (DFRobot_URM09); passive mode measures on the
 *   command, ready after 20/30/40 ms by range, automatic mode on every read;
 *   beyond its range the distance is -1.
 * RCWL-9620: 0x01 starts a measurement (80 ms); three bytes of micrometres
 *   back, the last one until a new one is ready; no echo reads 6 m.
 * TF-Luna: the registers of its I2C table, refreshed on every read; nothing
 *   in front gives Amp 40 and distance 0.
 */
#include "sensors_emu.h"

#include <math.h>
#include <string.h>

#define TAU 6.283185307179586

/* What is in front: mm between lo and hi, or < 0 for nothing */
static double target_mm(double t, double lo, double hi)
{
    if (fmod(t, 70.0) >= 64.0) return -1;
    double m = 0.5 + 0.5 * sin(TAU * t / 40.0) * cos(TAU * t / 17.0);
    return lo + (hi - lo) * m + sim_noise(1.5);
}

static uint32_t now32(void) { return (uint32_t)sim_ms(); }

/* -------------------------------------------------------------------------- */
/* VL53L0X                                                                     */
/* -------------------------------------------------------------------------- */

enum { L0_MEAS, L0_START, L0_SPAD, L0_KIND };

static void l0_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->r16, 0, sizeof d->r16);
    memset(d->u, 0, sizeof d->u);
    d->page = 0;
    d->regs[0xC0] = 0xEE; d->regs[0xC1] = 0xAA; d->regs[0xC2] = 0x10;
    d->regs[0x01] = 0xFF;
    d->regs[0x46] = 0x25; d->regs[0x50] = 0x06; d->regs[0x51] = 0x00; d->regs[0x52] = 0x96;
    d->regs[0x70] = 0x04; d->regs[0x71] = 0x01; d->regs[0x72] = 0xFE;
    d->regs[0x84] = 0x11;
    static const uint8_t SPADS[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F };
    memcpy(d->regs + 0xB0, SPADS, 6);
    d->r16[0x91] = 0x3C;                            /* the stop variable */
    d->r16[0x92] = 0x85;                            /* 5 reference SPADs, aperture */
}

static void l0_update(sim_emu_t *d)
{
    if (!d->u[L0_MEAS]) return;
    bool range = (d->u[L0_KIND] & 0x40) == 0 && (d->regs[0x01] & 0x80);   /* not a VHV/phase calibration */
    uint32_t dur = range ? 30 : 5;
    if (now32() - d->u[L0_START] < dur) return;
    d->u[L0_MEAS] = 0;
    d->regs[0x13] = 0x04;                           /* new sample ready */
    if (!range) return;
    double mm = target_mm(sim_now_s(), 60.0, 1200.0);
    uint16_t v = mm < 0 ? 8190 : (uint16_t)lrint(mm);
    d->regs[0x14] = (uint8_t)((mm < 0 ? 4 : 11) << 3);   /* device range status: 11 complete, 4 phase fail */
    d->regs[0x1E] = (uint8_t)(v >> 8);
    d->regs[0x1F] = (uint8_t)v;
}

static void l0_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0xFF) { d->page = v; return; }
    if (d->page) {
        d->r16[reg] = v;
        if (reg == 0x83) d->u[L0_SPAD] = v == 0x00 ? now32() + 1 : 0;   /* SPAD discovery */
        return;
    }
    switch (reg) {
    case 0x00:                                      /* SYSRANGE_START */
        if (v & 0x01) {
            d->u[L0_MEAS] = 1;
            d->u[L0_START] = now32();
            d->u[L0_KIND] = v;
            d->regs[0x13] = 0;
        } else d->u[L0_MEAS] = 0;
        d->regs[0x00] = 0;                          /* the start bit clears when it starts */
        return;
    case 0x0B:                                      /* SYSTEM_INTERRUPT_CLEAR */
        if (v & 0x01) d->regs[0x13] = 0;
        return;
    case 0x13: case 0x14: case 0xC0: case 0xC1: case 0xC2:
        return;                                     /* read only */
    default:
        d->regs[reg] = v;
    }
}

static uint8_t l0_rd(sim_emu_t *d, uint8_t reg)
{
    if (d->page) {
        if (reg == 0x83 && d->u[L0_SPAD]) return now32() >= d->u[L0_SPAD] + 1 ? 0x10 : 0x00;
        return (uint8_t)d->r16[reg];
    }
    return d->regs[reg];
}

static bool l0_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    l0_update(d);
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) l0_write(d, (uint8_t)(w[0] + i - 1), w[i]);
        if (wn > 1) d->ptr = (uint8_t)(w[0] + wn - 1);
    }
    for (size_t i = 0; i < rn; i++) r[i] = l0_rd(d, (uint8_t)(d->ptr + i));
    return true;
}

static const sim_model_t M_VL53L0X = { "vl53l0x", 0x29, l0_reset, l0_xfer };

/* -------------------------------------------------------------------------- */
/* VL53L1X                                                                     */
/* -------------------------------------------------------------------------- */

enum { L1_RUN, L1_NEXT, L1_READY };

static void l1_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->u, 0, sizeof d->u);
    d->regs[0xE5] = 0x03;                           /* FIRMWARE__SYSTEM_STATUS: booted */
    d->regs[0x30] = 0x01;
    d->regs[0x31] = 0x02;
}

static void l1_update(sim_emu_t *d)
{
    if (!d->u[L1_RUN] || d->u[L1_READY] || now32() < d->u[L1_NEXT]) return;
    double mm = target_mm(sim_now_s(), 120.0, 3200.0);
    uint16_t v = mm < 0 ? 0 : (uint16_t)lrint(mm);
    d->regs[0x89] = mm < 0 ? 4 : 9;                 /* raw range status */
    d->regs[0x96] = (uint8_t)(v >> 8);
    d->regs[0x97] = (uint8_t)v;
    d->u[L1_READY] = 1;
    d->u[L1_NEXT] = now32() + 100;
}

static void l1_write(sim_emu_t *d, uint16_t idx, uint8_t v)
{
    if (idx >= 0x100) return;
    if (idx == 0x0087) {
        d->u[L1_RUN] = (v & 0x40) != 0;
        d->u[L1_READY] = 0;
        d->u[L1_NEXT] = now32() + 100;
    } else if (idx == 0x0086) {
        if (v & 1) d->u[L1_READY] = 0;
        return;
    } else if (idx == 0x00E5 || idx == 0x0089 || idx == 0x0096 || idx == 0x0097) return;
    d->regs[idx] = v;
}

static uint8_t l1_rd(sim_emu_t *d, uint16_t idx)
{
    switch (idx) {
    case 0x010F: return 0xEA;
    case 0x0110: return 0xCC;
    case 0x0111: return 0x10;
    case 0x0031: {
        uint8_t pol = (d->regs[0x30] & 0x10) ? 0 : 1;
        return (uint8_t)((d->regs[0x31] & 0xFE) | (d->u[L1_READY] ? pol : !pol));
    }
    default: return idx < 0x100 ? d->regs[idx] : 0;
    }
}

static bool l1_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn == 1) return false;                      /* half an index: refused */
    l1_update(d);
    uint16_t idx = (uint16_t)(d->r16[0]);
    if (wn >= 2) {
        idx = (uint16_t)(w[0] << 8 | w[1]);
        for (size_t i = 2; i < wn; i++) l1_write(d, (uint16_t)(idx + i - 2), w[i]);
        idx = (uint16_t)(idx + (wn - 2));
    }
    for (size_t i = 0; i < rn; i++) r[i] = l1_rd(d, (uint16_t)(idx + i));
    d->r16[0] = (uint16_t)(idx + rn);
    return true;
}

static const sim_model_t M_VL53L1X = { "vl53l1x", 0x29, l1_reset, l1_xfer };

/* -------------------------------------------------------------------------- */
/* VL6180X                                                                     */
/* -------------------------------------------------------------------------- */

enum { L6_RANGE, L6_RANGE_MS, L6_ALS, L6_ALS_MS, L6_IDX };

static void l6_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->r16, 0, sizeof d->r16);
    memset(d->u, 0, sizeof d->u);
    static const uint8_t ID[5] = { 0xB4, 0x01, 0x03, 0x01, 0x02 };   /* model, model rev, module rev */
    memcpy(d->regs, ID, 5);
    d->regs[0x16] = 0x01;                           /* SYSTEM__FRESH_OUT_OF_RESET */
    d->regs[0x3F] = 0x06;                           /* ALS gain 1 */
    d->regs[0x4D] = 0x01;                           /* RESULT__RANGE_STATUS: device ready */
}

static void l6_update(sim_emu_t *d)
{
    uint32_t now = now32();
    if (d->u[L6_RANGE] && now >= d->u[L6_RANGE_MS]) {
        d->u[L6_RANGE] = 0;
        double mm = target_mm(sim_now_s(), 12.0, 260.0);
        if (mm < 0 || mm > 200) { d->regs[0x62] = 255; d->regs[0x4D] = 6 << 4 | 1; }
        else { d->regs[0x62] = (uint8_t)lrint(mm); d->regs[0x4D] = 0x01; }
        d->regs[0x4F] = (uint8_t)((d->regs[0x4F] & ~0x07) | 0x04);
    }
    if (d->u[L6_ALS] && now >= d->u[L6_ALS_MS]) {
        static const double GAIN[8] = { 20, 10, 5, 2.5, 1.67, 1.25, 1, 40 };
        d->u[L6_ALS] = 0;
        double it = (double)((d->regs[0x40] << 8 | d->regs[0x41]) + 1);
        double raw = sim_light(sim_now_s()) / 0.32 * GAIN[d->regs[0x3F] & 7] * it / 100.0;
        uint16_t v = raw > 65535 ? 65535 : (uint16_t)lrint(raw);
        d->regs[0x50] = (uint8_t)(v >> 8);
        d->regs[0x51] = (uint8_t)v;
        d->regs[0x4F] = (uint8_t)((d->regs[0x4F] & ~0x38) | 0x20);
    }
}

static void l6_write(sim_emu_t *d, uint16_t idx, uint8_t v)
{
    uint32_t now = now32();
    if (idx >= 0x200) return;                       /* private registers: accepted, nothing to keep */
    if (idx >= 0x100) { d->r16[idx & 0xFF] = v; return; }
    switch (idx) {
    case 0x018:                                     /* SYSRANGE__START */
        if (v & 1) { d->u[L6_RANGE] = 1; d->u[L6_RANGE_MS] = now + 8; d->regs[0x4D] &= 0xFE; }
        return;
    case 0x038:                                     /* SYSALS__START */
        if (v & 1) {
            d->u[L6_ALS] = 1;
            d->u[L6_ALS_MS] = now + (uint32_t)(d->regs[0x40] << 8 | d->regs[0x41]) + 1;
        }
        return;
    case 0x015:                                     /* SYSTEM__INTERRUPT_CLEAR */
        if (v & 1) d->regs[0x4F] &= (uint8_t)~0x07;
        if (v & 2) d->regs[0x4F] &= (uint8_t)~0x38;
        if (v & 4) d->regs[0x4F] &= (uint8_t)~0xC0;
        return;
    case 0x000: case 0x001: case 0x002: case 0x003: case 0x004:
    case 0x04D: case 0x04F: case 0x050: case 0x051: case 0x062:
        return;                                     /* read only */
    default:
        d->regs[idx] = v;
    }
}

static uint8_t l6_rd(sim_emu_t *d, uint16_t idx)
{
    if (idx < 0x100) return d->regs[idx];
    if (idx < 0x200) return (uint8_t)d->r16[idx & 0xFF];
    return 0;
}

static bool l6_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn == 1) return false;
    l6_update(d);
    uint16_t idx = (uint16_t)d->u[L6_IDX];
    if (wn >= 2) {
        idx = (uint16_t)(w[0] << 8 | w[1]);
        for (size_t i = 2; i < wn; i++) l6_write(d, (uint16_t)(idx + i - 2), w[i]);
        idx = (uint16_t)(idx + (wn - 2));
    }
    for (size_t i = 0; i < rn; i++) r[i] = l6_rd(d, (uint16_t)(idx + i));
    d->u[L6_IDX] = (uint16_t)(idx + rn);
    return true;
}

static const sim_model_t M_VL6180X = { "vl6180x", 0x29, l6_reset, l6_xfer };

/* -------------------------------------------------------------------------- */
/* URM09                                                                       */
/* -------------------------------------------------------------------------- */

static void urm_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->u, 0, sizeof d->u);
    d->regs[0] = d->addr;
    d->regs[1] = 0x01;                              /* PID */
    d->regs[2] = 0x10;                              /* version */
    d->regs[7] = 0x80 | 0x20;                       /* automatic, 500 cm */
}

static void urm_measure(sim_emu_t *d)
{
    static const int RANGE[4] = { 150, 300, 500, 500 };
    double t = sim_now_s();
    double cm = target_mm(t, 150.0, 3500.0) / 10.0;
    int16_t v = cm < 2 || cm > RANGE[(d->regs[7] >> 4) & 3] ? -1 : (int16_t)lrint(cm);
    int16_t tc = (int16_t)lrint((sim_room_t(t) + 0.4) * 10.0);
    d->regs[3] = (uint8_t)((uint16_t)v >> 8); d->regs[4] = (uint8_t)v;
    d->regs[5] = (uint8_t)((uint16_t)tc >> 8); d->regs[6] = (uint8_t)tc;
}

static bool urm_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = (uint8_t)(w[0] + i - 1);
            if (reg == 7) d->regs[7] = w[i];
            else if (reg == 8 && w[i] == 0x01) {
                static const uint32_t MS[4] = { 20, 30, 40, 40 };
                d->u[0] = 1;
                d->u[1] = now32() + MS[(d->regs[7] >> 4) & 3];
            }
        }
    }
    if (!rn) return true;
    if (d->regs[7] & 0x80) urm_measure(d);
    else if (d->u[0] && now32() >= d->u[1]) { d->u[0] = 0; urm_measure(d); }
    for (size_t i = 0; i < rn; i++) r[i] = (uint8_t)(d->ptr + i) < 9 ? d->regs[(uint8_t)(d->ptr + i)] : 0;
    return true;
}

static const sim_model_t M_URM09 = { "urm09", 0x11, urm_reset, urm_xfer };

/* -------------------------------------------------------------------------- */
/* RCWL-9620 (M5Stack Unit Sonic I2C)                                          */
/* -------------------------------------------------------------------------- */

static void rcwl_reset(sim_emu_t *d)
{
    memset(d->resp, 0, sizeof d->resp);
    memset(d->u, 0, sizeof d->u);
}

static bool rcwl_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn && w[0] == 0x01) { d->u[0] = 1; d->u[1] = now32() + 80; }
    if (d->u[0] && now32() >= d->u[1]) {
        d->u[0] = 0;
        double mm = target_mm(sim_now_s(), 40.0, 2600.0);
        uint32_t um = mm < 0 ? 6000000u : (uint32_t)lrint(mm * 1000.0);
        d->resp[0] = (uint8_t)(um >> 16); d->resp[1] = (uint8_t)(um >> 8); d->resp[2] = (uint8_t)um;
    }
    for (size_t i = 0; i < rn; i++) r[i] = i < 3 ? d->resp[i] : 0;
    return true;
}

static const sim_model_t M_RCWL9620 = { "rcwl9620", 0x57, rcwl_reset, rcwl_xfer };

/* -------------------------------------------------------------------------- */
/* TF-Luna (I2C mode)                                                          */
/* -------------------------------------------------------------------------- */

static void tfl_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x0A] = 5; d->regs[0x0B] = 3; d->regs[0x0C] = 3;         /* firmware 3.3.5 */
    memcpy(d->regs + 0x10, "LUNA2026101900", 14);                    /* serial number */
    d->regs[0x22] = d->addr;
    d->regs[0x25] = 0x01;                                            /* enabled */
    d->regs[0x26] = 0x64;                                            /* 100 Hz */
    d->regs[0x2A] = 100;                                             /* Amp threshold */
    d->regs[0x30] = 0x20; d->regs[0x31] = 0x03;                      /* 800 cm */
}

static bool tfl_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (rn) {
        double t = sim_now_s();
        double mm = target_mm(t, 200.0, 7800.0);
        uint16_t cm = mm < 0 ? 0 : (uint16_t)lrint(mm / 10.0);
        uint16_t amp = mm < 0 ? 40 : (uint16_t)lrint(9000.0 / (1.0 + mm / 400.0) + sim_noise(20));
        int16_t tc = (int16_t)lrint((sim_room_t(t) + 17.0) * 100.0);
        uint16_t tick = (uint16_t)sim_ms();
        uint16_t v[4] = { cm, amp, (uint16_t)tc, tick };
        for (int i = 0; i < 4; i++) { d->regs[2 * i] = (uint8_t)v[i]; d->regs[2 * i + 1] = (uint8_t)(v[i] >> 8); }
    }
    return sim_reg8_xfer(d, w, wn, r, rn);
}

static const sim_model_t M_TFLUNA = { "tfluna", 0x10, tfl_reset, tfl_xfer };

const sim_model_t *const SIM_M_DIST[] = {
    &M_VL53L0X, &M_VL53L1X, &M_VL6180X, &M_URM09, &M_RCWL9620, &M_TFLUNA,
    NULL,
};
