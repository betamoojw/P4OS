/*
 * P4OS simulator - emulated air sensors for the sensor service (sensors_emu.h).
 *
 * The room breathes: CO2 rises and falls over ten minutes as people come
 * and go, and every four minutes somebody cooks for one - VOCs, NOx and
 * particles go up together and settle again. Each chip gives that in its
 * own protocol, from its datasheet: Sensirion's 16-bit commands with a CRC
 * per word (NACK while a command executes, and on a read with nothing to
 * give), the CCS811's mailbox registers and boot loader, the ENS160's
 * register file and operating modes, and the particle counters' frames
 * with their checksums. Raw words come from inverting the datasheets'
 * formulas, so the drivers' conversions are checked against them.
 *
 *   scd40 scd41 scd43 @0x62   scd30 @0x61   sgp30 @0x58
 *   sgp40 sgp41 @0x59         ccs811 @0x5A  ens160 ens161 @0x53
 *   pmsa003i @0x12            sen50 sen54 sen55 sps30 @0x69   hm3301 @0x40
 */
#include "sensors_emu.h"

#include <math.h>
#include <string.h>

#define PI 3.141592653589793

/* ---- the room ---- */

static double co2_ppm(double t) { return 640.0 + 280.0 * sin(2 * PI * t / 600.0) + sim_noise(6.0); }

/* somebody cooking: 0 most of the time, up to 1 for a minute every 4 */
static double cooking(double t)
{
    double e = fmod(t + 150.0, 240.0);
    if (e >= 60.0) return 0;
    double s = sin(PI * e / 60.0);
    return s * s;
}

static double pm25_ugm3(double t) { double v = 7.0 + 2.5 * sin(2 * PI * t / 90.0) + 45.0 * cooking(t) + sim_noise(0.4); return v < 0 ? 0 : v; }
static double tvoc_ppb(double t) { double v = 90.0 + 850.0 * cooking(t) + sim_noise(4.0); return v < 0 ? 0 : v; }

/* ---- Sensirion's command protocol ----
 * u[15] is when the command in progress ends: everything NACKs until then. */

static void put_word(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    p[2] = sim_crc8(p, 2, 0x31, 0xFF);
}

static void answer(sim_emu_t *d, const uint16_t *w, int n, int ms)
{
    if (n > 21) n = 21;
    for (int i = 0; i < n; i++) put_word(d->resp + 3 * i, w[i]);
    d->resp_n = 3 * n;
    d->resp_ms = sim_ms() + (uint64_t)ms;
}

static void busy(sim_emu_t *d, int ms) { d->u[15] = (uint32_t)(sim_ms() + (uint64_t)ms); }

/* the argument words after the command, each with a good CRC */
static bool args(const uint8_t *a, size_t an, uint16_t *w, int n)
{
    if (an != (size_t)(3 * n)) return false;
    for (int i = 0; i < n; i++) {
        if (sim_crc8(a + 3 * i, 2, 0x31, 0xFF) != a[3 * i + 2]) return false;
        w[i] = (uint16_t)(a[3 * i] << 8 | a[3 * i + 1]);
    }
    return true;
}

typedef bool (*sir_cmd_fn)(sim_emu_t *d, uint16_t cmd, const uint8_t *a, size_t an);

static bool sir_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, sir_cmd_fn fn)
{
    if (sim_ms() < (uint64_t)d->u[15]) return false;
    if (wn) {
        if (wn < 2) return false;                   /* a register pointer: not ours */
        d->resp_n = 0;
        if (!fn(d, (uint16_t)(w[0] << 8 | w[1]), w + 2, wn - 2)) return false;
        if (!rn) return true;
    }
    if (!rn) return true;
    if (!d->resp_n || sim_ms() < d->resp_ms) return false;
    for (size_t i = 0; i < rn; i++) r[i] = i < (size_t)d->resp_n ? d->resp[i] : 0xFF;
    d->resp_n = 0;
    return true;
}

static uint16_t t_word(double t) { return (uint16_t)lrint((t + 45.0) * 65535.0 / 175.0); }
static uint16_t rh_word(double h) { return (uint16_t)lrint(h * 65535.0 / 100.0); }

static void float_words(float f, uint16_t *w)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    w[0] = (uint16_t)(u >> 16);
    w[1] = (uint16_t)u;
}

/* -------------------------------------------------------------------------- */
/* SCD4x: u[0] measuring, u[1] variant word, u[2] samples read, u[3] hPa       */
/* -------------------------------------------------------------------------- */

static uint32_t scd4x_sample(const sim_emu_t *d) { return (uint32_t)((sim_ms() - d->t0) / 5000); }

static bool scd4x_cmd(sim_emu_t *d, uint16_t c, const uint8_t *a, size_t an)
{
    uint16_t w[3];
    switch (c) {
    case 0x21B1:                                    /* start_periodic_measurement */
        if (d->u[0]) return false;
        d->u[0] = 1;
        d->t0 = sim_ms();
        d->u[2] = 0;
        return true;
    case 0x3F86:                                    /* stop_periodic_measurement */
        d->u[0] = 0;
        busy(d, 500);
        return true;
    case 0xE4B8:                                    /* get_data_ready_status */
        w[0] = (uint16_t)(d->u[0] && scd4x_sample(d) > d->u[2] ? 0x8006 : 0x8000);
        answer(d, w, 1, 1);
        return true;
    case 0xEC05: {                                  /* read_measurement */
        if (!d->u[0] || scd4x_sample(d) <= d->u[2]) return true;   /* nothing: the read NACKs */
        d->u[2] = scd4x_sample(d);
        double t = sim_now_s();
        w[0] = (uint16_t)lrint(co2_ppm(t));
        w[1] = t_word(sim_room_t(t) + 0.6);         /* it runs a bit warm */
        w[2] = rh_word(sim_room_h(t) - 2.0);
        answer(d, w, 3, 1);
        return true;
    }
    case 0x3682:                                    /* get_serial_number: idle only */
        if (d->u[0]) return false;
        w[0] = 0xF896; w[1] = 0x9F07; w[2] = 0x3BB3;
        answer(d, w, 3, 1);
        return true;
    case 0x202F:                                    /* get_sensor_variant: idle only */
        if (d->u[0]) return false;
        w[0] = (uint16_t)d->u[1];
        answer(d, w, 1, 1);
        return true;
    case 0xE000:                                    /* ambient pressure: set or get */
        if (an) {
            if (!args(a, an, w, 1)) return false;
            d->u[3] = w[0];
        } else {
            w[0] = (uint16_t)d->u[3];
            answer(d, w, 1, 1);
        }
        return true;
    default:
        return false;
    }
}

static bool scd4x_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return sir_xfer(d, w, wn, r, rn, scd4x_cmd); }
static void scd40_reset(sim_emu_t *d) { d->u[1] = 0x0440; d->u[3] = 1013; }
static void scd41_reset(sim_emu_t *d) { d->u[1] = 0x1440; d->u[3] = 1013; }
static void scd43_reset(sim_emu_t *d) { d->u[1] = 0x5440; d->u[3] = 1013; }

static const sim_model_t M_SCD40 = { "scd40", 0x62, scd40_reset, scd4x_xfer };
static const sim_model_t M_SCD41 = { "scd41", 0x62, scd41_reset, scd4x_xfer };
static const sim_model_t M_SCD43 = { "scd43", 0x62, scd43_reset, scd4x_xfer };

/* -------------------------------------------------------------------------- */
/* SCD30: u[0] continuous, u[1] interval s, u[2] samples read, u[3] mbar       */
/* -------------------------------------------------------------------------- */

static uint32_t scd30_sample(const sim_emu_t *d) { return (uint32_t)((sim_ms() - d->t0) / (1000u * (d->u[1] ? d->u[1] : 2))); }

static bool scd30_cmd(sim_emu_t *d, uint16_t c, const uint8_t *a, size_t an)
{
    uint16_t w[6];
    switch (c) {
    case 0xD100: w[0] = 0x0342; answer(d, w, 1, 2); return true;           /* firmware 3.66 */
    case 0x0010:                                    /* continuous, with the pressure */
        if (!args(a, an, w, 1)) return false;
        d->u[3] = w[0];
        if (!d->u[0]) { d->u[0] = 1; d->t0 = sim_ms(); d->u[2] = 0; }
        return true;
    case 0x0104: d->u[0] = 0; return true;
    case 0x4600:
        if (an) {
            if (!args(a, an, w, 1) || w[0] < 2 || w[0] > 1800) return false;
            d->u[1] = w[0];
            d->t0 = sim_ms();
            d->u[2] = 0;
        } else {
            w[0] = (uint16_t)d->u[1];
            answer(d, w, 1, 2);
        }
        return true;
    case 0x0202:
        w[0] = d->u[0] && scd30_sample(d) > d->u[2];
        answer(d, w, 1, 2);
        return true;
    case 0x0300: {
        if (!d->u[0] || scd30_sample(d) <= d->u[2]) return true;
        d->u[2] = scd30_sample(d);
        double t = sim_now_s();
        float_words((float)co2_ppm(t), w);
        float_words((float)(sim_room_t(t) + 0.3), w + 2);
        float_words((float)sim_room_h(t), w + 4);
        answer(d, w, 6, 2);
        return true;
    }
    case 0xD304: d->u[0] = 0; busy(d, 30); return true;
    case 0x5306: case 0x5403: case 0x5102: case 0x5204: return true;       /* ASC, offsets: accepted */
    default: return false;
    }
}

static bool scd30_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return sir_xfer(d, w, wn, r, rn, scd30_cmd); }
static void scd30_reset(sim_emu_t *d) { d->u[1] = 5; }                     /* someone else's interval in its flash */
static const sim_model_t M_SCD30 = { "scd30", 0x61, scd30_reset, scd30_xfer };

/* -------------------------------------------------------------------------- */
/* SGP30: u[0] air quality started, u[1] absolute humidity                     */
/* -------------------------------------------------------------------------- */

static bool sgp30_cmd(sim_emu_t *d, uint16_t c, const uint8_t *a, size_t an)
{
    uint16_t w[3];
    double t = sim_now_s();
    switch (c) {
    case 0x2003: d->u[0] = 1; d->t0 = sim_ms(); busy(d, 10); return true;
    case 0x2008:
        if (!d->u[0] || sim_ms() - d->t0 < 15000) { w[0] = 400; w[1] = 0; }
        else {
            double tv = tvoc_ppb(t);
            w[0] = (uint16_t)lrint(410.0 + 0.55 * tv + 0.3 * (co2_ppm(t) - 400.0));
            w[1] = (uint16_t)lrint(tv);
        }
        answer(d, w, 2, 12);
        return true;
    case 0x202F: w[0] = 0x0022; answer(d, w, 1, 10); return true;
    case 0x2050:
        w[0] = (uint16_t)lrint(13600.0 - 900.0 * cooking(t) + sim_noise(5));
        w[1] = (uint16_t)lrint(19200.0 - 1500.0 * cooking(t) + sim_noise(5));
        answer(d, w, 2, 25);
        return true;
    case 0x2061: if (!args(a, an, w, 1)) return false; d->u[1] = w[0]; busy(d, 10); return true;
    case 0x3682: w[0] = 0x0000; w[1] = 0x0148; w[2] = 0xA6B2; answer(d, w, 3, 1); return true;
    case 0x2015: w[0] = 0x8F2C; w[1] = 0x9104; answer(d, w, 2, 10); return true;
    default: return false;
    }
}

static bool sgp30_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return sir_xfer(d, w, wn, r, rn, sgp30_cmd); }
static const sim_model_t M_SGP30 = { "sgp30", 0x58, NULL, sgp30_xfer };

/* -------------------------------------------------------------------------- */
/* SGP40 / SGP41: u[0] is an SGP41                                             */
/* -------------------------------------------------------------------------- */

static uint16_t sraw_voc(double t) { return (uint16_t)lrint(30500.0 + 150.0 * sin(2 * PI * t / 70.0) - 3200.0 * cooking(t) + sim_noise(12)); }
static uint16_t sraw_nox(double t) { return (uint16_t)lrint(16200.0 + 1800.0 * cooking(t) + sim_noise(6)); }

static bool sgp4x_cmd(sim_emu_t *d, uint16_t c, const uint8_t *a, size_t an)
{
    uint16_t w[3];
    double t = sim_now_s();
    bool is41 = d->u[0] != 0;
    switch (c) {
    case 0x3682: w[0] = 0x0000; w[1] = 0x0412; w[2] = 0x8C3E; answer(d, w, 3, 1); return true;
    case 0x260F:                                    /* SGP40 measure_raw_signal */
        if (is41 || !args(a, an, w, 2)) return false;
        w[0] = sraw_voc(t);
        answer(d, w, 1, 30);
        return true;
    case 0x2612:                                    /* SGP41 execute_conditioning */
        if (!is41 || !args(a, an, w, 2)) return false;
        w[0] = sraw_voc(t);
        answer(d, w, 1, 50);
        return true;
    case 0x2619:                                    /* SGP41 measure_raw_signals */
        if (!is41 || !args(a, an, w, 2)) return false;
        w[0] = sraw_voc(t);
        w[1] = sraw_nox(t);
        answer(d, w, 2, 50);
        return true;
    case 0x280E: w[0] = 0xD400; answer(d, w, 1, 320); return true;          /* self test: passed */
    case 0x3615: return true;                                               /* heater off */
    default: return false;
    }
}

static bool sgp4x_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return sir_xfer(d, w, wn, r, rn, sgp4x_cmd); }
static void sgp41_reset(sim_emu_t *d) { d->u[0] = 1; }
static const sim_model_t M_SGP40 = { "sgp40", 0x59, NULL, sgp4x_xfer };
static const sim_model_t M_SGP41 = { "sgp41", 0x59, sgp41_reset, sgp4x_xfer };

/* -------------------------------------------------------------------------- */
/* CCS811: u[0] application running, u[1] MEAS_MODE, u[2] samples read,       */
/* u[3] ERROR_ID; a mailbox interface, a register pointer and no auto-increment */
/* beyond each mailbox                                                         */
/* -------------------------------------------------------------------------- */

static uint32_t ccs_sample(const sim_emu_t *d)
{
    static const uint32_t PERIOD[5] = { 0, 1000, 10000, 60000, 250 };
    int m = (int)(d->u[1] >> 4 & 7);
    if (!d->u[0] || m < 1 || m > 4) return 0;
    return (uint32_t)((sim_ms() - d->t0) / PERIOD[m]);
}

static uint8_t ccs_status(const sim_emu_t *d)
{
    return (uint8_t)(0x10 | (d->u[0] ? 0x80 : 0) | (ccs_sample(d) > d->u[2] ? 0x08 : 0) | (d->u[3] ? 0x01 : 0));
}

static bool ccs811_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        if (w[0] == 0xF4 && wn == 1) d->u[0] = 1;                       /* APP_START */
        else if (w[0] == 0x01 && wn == 2) {
            if (!d->u[0]) d->u[3] = 0x01;                               /* WRITE_REG_INVALID in boot mode */
            else { d->u[1] = w[1]; d->t0 = sim_ms(); d->u[2] = 0; }
        } else if (w[0] == 0x05 && wn == 5) memcpy(d->regs + 0x05, w + 1, 4);
        else if (w[0] == 0xFF && wn == 5 && w[1] == 0x11 && w[2] == 0xE5 && w[3] == 0x72 && w[4] == 0x8A) {
            d->u[0] = 0; d->u[1] = 0; d->u[3] = 0;                      /* SW_RESET */
        }
    }
    if (!rn) return true;
    uint8_t b[8] = { 0 };
    switch (d->ptr) {
    case 0x00: b[0] = ccs_status(d); break;
    case 0x01: b[0] = (uint8_t)d->u[1]; break;
    case 0x02: {
        uint8_t st = ccs_status(d);
        double t = sim_now_s();
        double tv = tvoc_ppb(t) * 0.8;
        uint16_t eco2 = (uint16_t)lrint(400.0 + 0.5 * tv + 0.4 * (co2_ppm(t) - 400.0));
        b[0] = (uint8_t)(eco2 >> 8); b[1] = (uint8_t)eco2;
        b[2] = (uint8_t)((int)tv >> 8); b[3] = (uint8_t)tv;
        b[4] = st; b[5] = (uint8_t)d->u[3];
        b[6] = 0x18; b[7] = 0x9C;                                       /* 6 µA, ADC 156 */
        if (st & 0x08) d->u[2] = ccs_sample(d);                         /* read: DATA_READY clears */
        break;
    }
    case 0x05: memcpy(b, d->regs + 0x05, 4); break;
    case 0x20: b[0] = 0x81; break;
    case 0x21: b[0] = 0x12; break;
    case 0x23: b[0] = 0x10; b[1] = 0x00; break;
    case 0x24: b[0] = 0x20; b[1] = 0x00; break;
    case 0xE0: b[0] = (uint8_t)d->u[3]; d->u[3] = 0; break;
    default: break;
    }
    for (size_t i = 0; i < rn; i++) r[i] = i < sizeof b ? b[i] : 0;
    return true;
}

static const sim_model_t M_CCS811 = { "ccs811", 0x5A, NULL, ccs811_xfer };

/* -------------------------------------------------------------------------- */
/* ENS160 / ENS161: the register file; u[0] part id, u[1] samples given,       */
/* u[2] samples read                                                           */
/* -------------------------------------------------------------------------- */

static void ens_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = (uint8_t)d->u[0];
    d->regs[0x01] = (uint8_t)(d->u[0] >> 8);
    d->regs[0x10] = 0x00;                           /* deep sleep */
    d->u[1] = d->u[2] = 0;
}

static void ens160_reset(sim_emu_t *d) { d->u[0] = 0x0160; ens_reset(d); }
static void ens161_reset(sim_emu_t *d) { d->u[0] = 0x0161; ens_reset(d); }

static void ens_update(sim_emu_t *d)
{
    bool std = d->regs[0x10] == 0x02;
    uint32_t n = std ? (uint32_t)((sim_ms() - d->t0) / 1000) : 0;
    if (std && n > d->u[1]) {
        d->u[1] = n;
        double t = sim_now_s();
        double tv = tvoc_ppb(t) * 1.1;
        uint16_t eco2 = (uint16_t)lrint(420.0 + 0.9 * tv);
        int aqi = tv < 65 ? 1 : tv < 220 ? 2 : tv < 660 ? 3 : tv < 2200 ? 4 : 5;
        d->regs[0x21] = (uint8_t)aqi;
        d->regs[0x22] = (uint8_t)lrint(tv); d->regs[0x23] = (uint8_t)((long)lrint(tv) >> 8);
        d->regs[0x24] = (uint8_t)eco2; d->regs[0x25] = (uint8_t)(eco2 >> 8);
    }
    int validity = !std ? 0 : sim_ms() - d->t0 < 180000 ? 1 : 0;      /* warming up, then normal */
    d->regs[0x20] = (uint8_t)((std ? 0x80 : 0) | validity << 2 | (d->u[1] > d->u[2] ? 0x02 : 0));
}

static bool ens160_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn >= 2 && w[0] == 0x10) {
        if (w[1] == 0xF0) { ens_reset(d); d->ptr = 0x10; return true; }
        if (w[1] == 0x02 && d->regs[0x10] != 0x02) { d->t0 = sim_ms(); d->u[1] = d->u[2] = 0; }
        d->regs[0x10] = w[1];
        return true;
    }
    if (wn >= 2 && (w[0] < 0x10 || (w[0] >= 0x20 && w[0] < 0x40))) { d->ptr = w[0]; return true; }  /* read-only */
    if (rn) {
        if (wn) d->ptr = w[0];
        ens_update(d);
        uint8_t p = d->ptr;
        bool ok = sim_reg8_xfer(d, NULL, 0, r, rn);
        if (p <= 0x25 && p + rn > 0x21) d->u[2] = d->u[1];              /* the data read: NEWDAT clears */
        return ok;
    }
    return sim_reg8_xfer(d, w, wn, r, rn);
}

static const sim_model_t M_ENS160 = { "ens160", 0x53, ens160_reset, ens160_xfer };
static const sim_model_t M_ENS161 = { "ens161", 0x53, ens161_reset, ens160_xfer };

/* -------------------------------------------------------------------------- */
/* PMSA003I: a 32-byte frame on every read                                     */
/* -------------------------------------------------------------------------- */

static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static bool pmsa_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    (void)d; (void)w; (void)wn;
    if (!rn) return true;
    double pm = pm25_ugm3(sim_now_s());
    unsigned p1 = (unsigned)lrint(pm * 0.68), p25 = (unsigned)lrint(pm), p10 = (unsigned)lrint(pm * 1.25);
    uint8_t f[32] = { 0x42, 0x4D };
    be16(f + 2, 28);
    be16(f + 4, p1); be16(f + 6, p25); be16(f + 8, p10);                /* CF=1 */
    be16(f + 10, p1); be16(f + 12, p25); be16(f + 14, p10);             /* atmospheric */
    be16(f + 16, (unsigned)lrint(pm * 160)); be16(f + 18, (unsigned)lrint(pm * 48));
    be16(f + 20, (unsigned)lrint(pm * 9)); be16(f + 22, (unsigned)lrint(pm * 1.2));
    be16(f + 24, (unsigned)lrint(pm * 0.3)); be16(f + 26, (unsigned)lrint(pm * 0.1));
    f[28] = 0x97;                                                        /* version */
    unsigned sum = 0;
    for (int i = 0; i < 30; i++) sum += f[i];
    be16(f + 30, sum & 0xFFFF);
    for (size_t i = 0; i < rn; i++) r[i] = i < 32 ? f[i] : 0;
    return true;
}

static const sim_model_t M_PMSA003I = { "pmsa003i", 0x12, NULL, pmsa_xfer };

/* -------------------------------------------------------------------------- */
/* SEN5x: u[0] '0'/'4'/'5', u[1] measuring, u[2] samples read                  */
/* -------------------------------------------------------------------------- */

static uint32_t sen_sample(const sim_emu_t *d) { return (uint32_t)((sim_ms() - d->t0) / 1000); }

static void str_words(const char *s, uint16_t *w, int n)
{
    size_t l = strlen(s);
    for (int i = 0; i < n; i++) {
        uint8_t a = (size_t)(2 * i) < l ? (uint8_t)s[2 * i] : 0, b = (size_t)(2 * i + 1) < l ? (uint8_t)s[2 * i + 1] : 0;
        w[i] = (uint16_t)(a << 8 | b);
    }
}

static bool sen5x_cmd(sim_emu_t *d, uint16_t c, const uint8_t *a, size_t an)
{
    (void)a; (void)an;
    uint16_t w[16];
    switch (c) {
    case 0xD014: {
        char name[8] = "SEN5x";
        name[4] = (char)d->u[0];
        str_words(name, w, 16);
        answer(d, w, 16, 50);
        return true;
    }
    case 0xD033: str_words("8E5A5C0E4B1F21A7", w, 16); answer(d, w, 16, 50); return true;
    case 0x0021:
        if (d->u[1]) return false;
        d->u[1] = 1; d->t0 = sim_ms(); d->u[2] = 0;
        busy(d, 50);
        return true;
    case 0x0104: d->u[1] = 0; busy(d, 200); return true;
    case 0xD304: d->u[1] = 0; busy(d, 200); return true;
    case 0x0202:
        w[0] = d->u[1] && sen_sample(d) > d->u[2];
        answer(d, w, 1, 20);
        return true;
    case 0x03C4: {
        if (!d->u[1]) return false;
        d->u[2] = sen_sample(d);
        double t = sim_now_s(), pm = pm25_ugm3(t), ck = cooking(t);
        w[0] = (uint16_t)lrint(pm * 0.7 * 10); w[1] = (uint16_t)lrint(pm * 10);
        w[2] = (uint16_t)lrint(pm * 1.12 * 10); w[3] = (uint16_t)lrint(pm * 1.2 * 10);
        bool early = sim_ms() - d->t0 < 3000;
        char m = (char)d->u[0];
        w[4] = m == '0' ? 0x7FFF : (uint16_t)(int16_t)lrint((sim_room_h(t) - 1.0) * 100);
        w[5] = m == '0' ? 0x7FFF : (uint16_t)(int16_t)lrint((sim_room_t(t) + 0.4) * 200);
        w[6] = m == '0' || early ? 0x7FFF : (uint16_t)(int16_t)lrint((100.0 + 260.0 * ck) * 10);
        w[7] = m != '5' || early ? 0x7FFF : (uint16_t)(int16_t)lrint((1.0 + 18.0 * ck) * 10);
        answer(d, w, 8, 20);
        return true;
    }
    default: return false;
    }
}

static bool sen5x_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return sir_xfer(d, w, wn, r, rn, sen5x_cmd); }
static void sen50_reset(sim_emu_t *d) { d->u[0] = '0'; }
static void sen54_reset(sim_emu_t *d) { d->u[0] = '4'; }
static void sen55_reset(sim_emu_t *d) { d->u[0] = '5'; }
static const sim_model_t M_SEN50 = { "sen50", 0x69, sen50_reset, sen5x_xfer };
static const sim_model_t M_SEN54 = { "sen54", 0x69, sen54_reset, sen5x_xfer };
static const sim_model_t M_SEN55 = { "sen55", 0x69, sen55_reset, sen5x_xfer };

/* -------------------------------------------------------------------------- */
/* SPS30: u[1] measuring, u[2] samples read, u[3] output format                */
/* -------------------------------------------------------------------------- */

static bool sps30_cmd(sim_emu_t *d, uint16_t c, const uint8_t *a, size_t an)
{
    uint16_t w[20];
    switch (c) {
    case 0xD002: str_words("00080000", w, 4); answer(d, w, 4, 0); return true;
    case 0xD100: w[0] = 0x0202; answer(d, w, 1, 0); return true;
    case 0xD033: str_words("3D8A1E2C5B6F7A01", w, 16); answer(d, w, 16, 0); return true;
    case 0x0010:
        if (d->u[1] || !args(a, an, w, 1) || (w[0] != 0x0300 && w[0] != 0x0500)) return false;
        d->u[1] = 1; d->u[3] = w[0]; d->t0 = sim_ms(); d->u[2] = 0;
        busy(d, 20);
        return true;
    case 0x0104: d->u[1] = 0; busy(d, 20); return true;
    case 0x0202:
        w[0] = d->u[1] && sen_sample(d) > d->u[2];
        answer(d, w, 1, 0);
        return true;
    case 0x0300: {
        if (!d->u[1]) return false;
        d->u[2] = sen_sample(d);
        double pm = pm25_ugm3(sim_now_s());
        double v[10] = { pm * 0.72, pm, pm * 1.1, pm * 1.15, pm * 5.1, pm * 6.0, pm * 6.1, pm * 6.12, pm * 6.13, 0.52 };
        if (d->u[3] == 0x0300) {
            for (int i = 0; i < 10; i++) float_words((float)v[i], w + 2 * i);
            answer(d, w, 20, 0);
        } else {
            for (int i = 0; i < 10; i++) w[i] = (uint16_t)lrint(i == 9 ? v[i] * 1000 : v[i]);
            answer(d, w, 10, 0);
        }
        return true;
    }
    case 0x1103: case 0x1001: case 0x5607: case 0xD304: return true;
    default: return false;
    }
}

static bool sps30_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return sir_xfer(d, w, wn, r, rn, sps30_cmd); }
static const sim_model_t M_SPS30 = { "sps30", 0x69, NULL, sps30_xfer };

/* -------------------------------------------------------------------------- */
/* HM3301: any write (0x88 selects I2C), a 29-byte frame on every read         */
/* -------------------------------------------------------------------------- */

static bool hm3301_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    (void)d; (void)w; (void)wn;
    if (!rn) return true;
    double pm = pm25_ugm3(sim_now_s()) * 1.1;
    unsigned p1 = (unsigned)lrint(pm * 0.7), p25 = (unsigned)lrint(pm), p10 = (unsigned)lrint(pm * 1.3);
    uint8_t f[29] = { 0 };
    be16(f + 2, 1);                                                      /* sensor number */
    be16(f + 4, p1); be16(f + 6, p25); be16(f + 8, p10);                /* standard particle */
    be16(f + 10, p1); be16(f + 12, p25); be16(f + 14, p10);             /* atmospheric */
    be16(f + 16, (unsigned)lrint(pm * 150)); be16(f + 18, (unsigned)lrint(pm * 45));
    be16(f + 20, (unsigned)lrint(pm * 8)); be16(f + 22, (unsigned)lrint(pm));
    be16(f + 24, (unsigned)lrint(pm * 0.3)); be16(f + 26, (unsigned)lrint(pm * 0.1));
    uint8_t sum = 0;
    for (int i = 0; i < 28; i++) sum = (uint8_t)(sum + f[i]);
    f[28] = sum;
    for (size_t i = 0; i < rn; i++) r[i] = i < 29 ? f[i] : 0;
    return true;
}

static const sim_model_t M_HM3301 = { "hm3301", 0x40, NULL, hm3301_xfer };

const sim_model_t *const SIM_M_AIR[] = {
    &M_SCD40, &M_SCD41, &M_SCD43, &M_SCD30, &M_SGP30, &M_SGP40, &M_SGP41,
    &M_CCS811, &M_ENS160, &M_ENS161, &M_PMSA003I, &M_SEN50, &M_SEN54, &M_SEN55,
    &M_SPS30, &M_HM3301, NULL,
};
