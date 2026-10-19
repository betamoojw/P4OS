/*
 * P4OS - Sensor drivers: air quality on I2C: CO2, gases, particles
 * (sens_int.h, docs/SENSORS.md).
 *
 *   SCD4x (SCD40/41/43)  0x62  CO2 (photoacoustic), temperature, humidity
 *   SCD30                0x61  CO2 (NDIR), temperature, humidity
 *   SGP30                0x58  eCO2 and TVOC
 *   SGP40 / SGP41        0x59  VOC and NOx index (Sensirion's algorithm, here)
 *   CCS811               0x5A/0x5B  eCO2 and TVOC
 *   ENS160 / ENS161      0x52/0x53  AQI-UBA, eCO2 and TVOC
 *   PMSA003I             0x12  PM1, PM2.5, PM10
 *   SEN50/54/55          0x69  PM1..PM10, temperature, humidity, VOC, NOx
 *   SPS30                0x69  PM1..PM10
 *   HM3301               0x40  PM1, PM2.5, PM10
 *
 * Most of these chips give a new result every second or every few seconds,
 * and say so with a "data ready" flag. A read that finds no new result
 * hands the previous one back (air_keep): the values stay where they were
 * instead of the sensor going to an error for a round.
 *
 * Module-line arguments these drivers take:
 *   press=<hPa>      the air pressure, for the CO2 of an SCD4x or SCD30
 *   comp=<sensor>    another sensor whose temp/hum (and press) compensate
 *                    this one: "module co2 i2c.ext chip=scd41 comp=bme280"
 *   raw=1            the SGP30's and SGP4x's raw signals as extra values
 */
#include "sens_int.h"

/* No new result: hand back the last one, kept in the slot's values (the
 * service starts each read with a copy of them and xn = 0). */
static int air_keep(sens_slot_t *s, uint8_t nv)
{
    if (!nv) return R_NOTREADY;
    s->xn = nv;
    return R_OK;
}

/* A value of the sensor named in comp=, if there is one and it has it */
static bool air_comp(const sens_slot_t *s, const char *key, float *v)
{
    char name[24];
    if (!sens_arg_str(s, "comp", name, sizeof name) || !name[0]) return false;
    return aos_sensor_value(name, key, v) && !isnan(*v);
}

/* A Sensirion command, its execution time, then n words back, each CRC'd */
static int sir_read(aos_io_i2c_t *b, uint8_t a, uint16_t cmd, int wait_ms, uint16_t *w, int n)
{
    if (!si2c_cmd16(b, a, cmd)) return R_NACK;
    if (wait_ms) aos_hal_sleep_ms((uint32_t)wait_ms);
    bool ok = false;
    if (!si2c_words(b, a, w, n, &ok)) return R_NACK;
    return ok ? R_OK : R_CRC;
}

/* Two words, high first, as the big-endian float the SCD30 and SPS30 give */
static float sir_float(const uint16_t *w)
{
    uint32_t u = (uint32_t)w[0] << 16 | w[1];
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* Sensirion's humidity and temperature words (SGP4x compensation) */
static uint16_t sir_rh_ticks(float rh)
{
    if (rh < 0) rh = 0;
    if (rh > 100) rh = 100;
    return (uint16_t)lrintf(rh * 65535.0f / 100.0f);
}
static uint16_t sir_t_ticks(float t)
{
    if (t < -45) t = -45;
    if (t > 130) t = 130;
    return (uint16_t)lrintf((t + 45.0f) * 65535.0f / 175.0f);
}

/* ---- SCD40 / SCD41 / SCD43 (Sensirion SCD4x datasheet, section 3) ----
 *
 * identify() asks get_data_ready_status (0xE4B8), the one read-only
 * command the chip answers both idle and while measuring, and takes a word
 * with a good CRC as the proof. It never stops the measurement, so it can
 * run as often as autodetect likes (every 5 s while somebody looks, also
 * while the first result is still 5 s away), and it costs ~2 ms instead of
 * the 500 ms a stop_periodic_measurement would. It is also the safe choice
 * at 0x62, where an Adafruit MCP4725 DAC lives: 0xE4's top bits are a
 * reserved command for the DAC, while 0x3F86 (stop) and 0x3682 (serial)
 * would be fast-mode writes that power its output down.
 *
 * init() stops a measurement left running (by a reboot of the board, not
 * of the sensor) and the first read() 500 ms later, with the chip idle,
 * asks its variant (get_sensor_variant 0x202F, bits 15:12: 0 SCD40,
 * 1 SCD41, 5 SCD43; an old SCD40 firmware without it proves itself with
 * its serial), sets the pressure and starts the periodic measurement. From
 * then on each round asks whether there is a result (one every 5 s). */

typedef struct {
    uint8_t  phase;                 /* 0 stopping, 1 measuring */
    uint8_t  nv;
    uint32_t t;                     /* when it was stopped */
    uint32_t press_ms;              /* last pressure sent from comp= */
} scd4x_t;
SPRIV_CHECK(scd4x_t);

static int scd4x_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t w;
    return sir_read(b, a, 0xE4B8, 1, &w, 1) == R_OK ? ID_YES : ID_NO;
}

static int scd4x_init(sens_io_t *io, sens_slot_t *s)
{
    scd4x_t *p = SPRIV(s, scd4x_t);
    memset(p, 0, sizeof *p);
    if (!si2c_cmd16(io->i2c, s->pub.addr, 0x3F86)) return R_NACK;     /* stop_periodic_measurement */
    p->t = sens_now();
    return R_OK;
}

/* set_ambient_pressure, in hPa (allowed idle and while measuring) */
static void scd4x_pressure(aos_io_i2c_t *b, uint8_t a, const sens_slot_t *s)
{
    float hpa = sens_arg_f(s, "press", 0);
    float c;
    if (air_comp(s, "press", &c)) hpa = c;
    if (hpa >= 700 && hpa <= 1200) {
        uint16_t w = (uint16_t)lrintf(hpa);
        si2c_cmd16_w(b, a, 0xE000, &w, 1);
    }
}

static int scd4x_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    scd4x_t *p = SPRIV(s, scd4x_t);
    uint16_t w[3];
    int r;
    if (p->phase == 0) {
        if (sens_now() - p->t < 500) return R_NOTREADY;
        r = sir_read(b, a, 0x202F, 1, w, 1);                    /* get_sensor_variant */
        if (r == R_OK) {
            unsigned v = w[0] >> 12;
            sens_scpy(s->pub.chip, sizeof s->pub.chip, v == 0 ? "SCD40" : v == 1 ? "SCD41" : v == 5 ? "SCD43" : "SCD4x");
        } else {
            r = sir_read(b, a, 0x3682, 1, w, 3);                /* get_serial_number */
            if (r != R_OK) return r;
            sens_scpy(s->pub.chip, sizeof s->pub.chip, "SCD4x");
        }
        scd4x_pressure(b, a, s);
        if (!si2c_cmd16(b, a, 0x21B1)) return R_NACK;           /* start_periodic_measurement */
        p->phase = 1;
        p->press_ms = sens_now();
        return R_NOTREADY;
    }
    if (air_comp(s, "press", &(float){ 0 }) && sens_now() - p->press_ms >= 60000) {
        scd4x_pressure(b, a, s);
        p->press_ms = sens_now();
    }
    r = sir_read(b, a, 0xE4B8, 1, w, 1);
    if (r != R_OK) return r;
    if (!(w[0] & 0x07FF)) return air_keep(s, p->nv);
    r = sir_read(b, a, 0xEC05, 1, w, 3);                        /* read_measurement */
    if (r != R_OK) return r;
    if (w[0] == 0) return air_keep(s, p->nv);                   /* 0 ppm: a result not valid yet */
    sens_setv(s, 0, "co2", AOS_SQ_CO2, (float)w[0]);
    sens_setv(s, 1, "temp", AOS_SQ_TEMP, -45.0f + 175.0f * (float)w[1] / 65535.0f);
    sens_setv(s, 2, "hum", AOS_SQ_HUM, 100.0f * (float)w[2] / 65535.0f);
    p->nv = 3;
    return R_OK;
}

const sens_drv_t DRV_SCD4X = {
    .prio = SP_CMD,
    .pub = { .id = "scd4x", .name = "SCD4x", .addrs = { 0x62 }, .naddr = 1 },
    .identify = scd4x_identify, .init = scd4x_init, .read = scd4x_read,
};

/* ---- SCD30 (Sensirion "Interface Description SCD30", v1.0) ----
 * Clock stretching up to 150 ms and 3 ms between a command and its read.
 * identify: read firmware version (0xD100), a word with its CRC; 0xD1's top
 * bits are a reserved command for an MCP4725 at 0x61. Continuous
 * measurement every 2 s; the interval is kept in the chip's flash, so it
 * is only written when it is not 2 already. */

typedef struct {
    uint8_t  nv;
    uint16_t press;                 /* the one in force, hPa, 0 none */
    uint32_t press_ms;
} scd30_t;
SPRIV_CHECK(scd30_t);

static int scd30_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t w;
    return sir_read(b, a, 0xD100, 3, &w, 1) == R_OK ? ID_YES : ID_NO;
}

static uint16_t scd30_press(const sens_slot_t *s)
{
    float hpa = sens_arg_f(s, "press", 0), c;
    if (air_comp(s, "press", &c)) hpa = c;
    return hpa >= 700 && hpa <= 1400 ? (uint16_t)lrintf(hpa) : 0;
}

static int scd30_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    scd30_t *p = SPRIV(s, scd30_t);
    memset(p, 0, sizeof *p);
    uint16_t w;
    int r = sir_read(b, a, 0xD100, 3, &w, 1);
    if (r != R_OK) return r;
    if (sir_read(b, a, 0x4600, 3, &w, 1) == R_OK && w != 2) {   /* measurement interval */
        uint16_t two = 2;
        if (!si2c_cmd16_w(b, a, 0x4600, &two, 1)) return R_NACK;
        aos_hal_sleep_ms(3);
    }
    p->press = scd30_press(s);
    if (!si2c_cmd16_w(b, a, 0x0010, &p->press, 1)) return R_NACK;   /* continuous, with the pressure */
    p->press_ms = sens_now();
    return R_OK;
}

static int scd30_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    scd30_t *p = SPRIV(s, scd30_t);
    if (sens_now() - p->press_ms >= 60000) {    /* a new pressure from comp=: start again with it */
        uint16_t hpa = scd30_press(s);
        if (hpa != p->press && si2c_cmd16_w(b, a, 0x0010, &hpa, 1)) p->press = hpa;
        p->press_ms = sens_now();
        aos_hal_sleep_ms(3);
    }
    uint16_t w[6];
    int r = sir_read(b, a, 0x0202, 3, w, 1);                    /* get data ready */
    if (r != R_OK) return r;
    if (w[0] != 1) return air_keep(s, p->nv);
    r = sir_read(b, a, 0x0300, 3, w, 6);
    if (r != R_OK) return r;
    float co2 = sir_float(w), t = sir_float(w + 2), h = sir_float(w + 4);
    if (!(co2 >= 0 && co2 <= 40000) || !(t > -40 && t < 125) || !(h >= 0 && h <= 100)) return R_RANGE;
    sens_setv(s, 0, "co2", AOS_SQ_CO2, co2);
    sens_setv(s, 1, "temp", AOS_SQ_TEMP, t);
    sens_setv(s, 2, "hum", AOS_SQ_HUM, h);
    p->nv = 3;
    return R_OK;
}

const sens_drv_t DRV_SCD30 = {
    .prio = SP_CMD,
    .pub = { .id = "scd30", .name = "SCD30", .addrs = { 0x61 }, .naddr = 1 },
    .flags = SF_SLOW, .min_ms = 2000,
    .identify = scd30_identify, .init = scd30_init, .read = scd30_read,
};

/* ---- SGP30 (Sensirion SGP30 datasheet, section 6) ----
 * identify: get_feature_set (0x202F, 10 ms): product type in bits 15:12 is
 * 0, the version in the low byte 0x20 or 0x22. init: init_air_quality
 * (0x2003); then measure_air_quality (0x2008, 12 ms) once a round - the
 * chip's own baseline wants it once a second. It answers 400 ppm / 0 ppb
 * for the first 15 s. comp= gives it the absolute humidity (0x2061). */

typedef struct {
    uint8_t  nv;
    uint32_t ah_ms;
} sgp30_t;
SPRIV_CHECK(sgp30_t);

static int sgp30_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t w;
    if (sir_read(b, a, 0x202F, 10, &w, 1) != R_OK) return ID_NO;
    return (w & 0xF000) == 0 && (w & 0xFF) >= 0x20 && (w & 0xFF) <= 0x2F ? ID_YES : ID_NO;
}

static int sgp30_init(sens_io_t *io, sens_slot_t *s)
{
    sgp30_t *p = SPRIV(s, sgp30_t);
    memset(p, 0, sizeof *p);
    uint16_t w;
    int r = sir_read(io->i2c, s->pub.addr, 0x202F, 10, &w, 1);
    if (r != R_OK) return r;
    if (w & 0xF000) { sens_err(s, "0x%04X", w); return R_ID; }
    if (!si2c_cmd16(io->i2c, s->pub.addr, 0x2003)) return R_NACK;
    aos_hal_sleep_ms(10);
    p->ah_ms = sens_now() - 60000;
    return R_OK;
}

/* g/m³ from °C and %RH (Sensirion's application note for the SGP30) */
static float abs_humidity(float t, float rh)
{
    return 216.7f * (rh / 100.0f * 6.112f * expf(17.62f * t / (243.12f + t)) / (273.15f + t));
}

static int sgp30_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    sgp30_t *p = SPRIV(s, sgp30_t);
    float t, h;
    if (sens_now() - p->ah_ms >= 60000 && air_comp(s, "temp", &t) && air_comp(s, "hum", &h)) {
        float ah = abs_humidity(t, h) * 256.0f;              /* 8.8 fixed point; 0 turns it off */
        uint16_t w = (uint16_t)(ah < 1 ? 1 : ah > 65535 ? 65535 : lrintf(ah));
        if (si2c_cmd16_w(b, a, 0x2061, &w, 1)) aos_hal_sleep_ms(10);
        p->ah_ms = sens_now();
    }
    uint16_t w[2];
    int r = sir_read(b, a, 0x2008, 12, w, 2);
    if (r != R_OK) return r;
    sens_setv(s, 0, "eco2", AOS_SQ_CO2, (float)w[0]);
    sens_setv(s, 1, "tvoc", AOS_SQ_TVOC, (float)w[1]);
    if (sens_arg_int(s, "raw", 0)) {
        r = sir_read(b, a, 0x2050, 25, w, 2);                   /* measure_raw: H2, ethanol */
        if (r != R_OK) return r;
        sens_setv(s, 2, "h2", AOS_SQ_RAW, (float)w[0]);
        sens_setv(s, 3, "ethanol", AOS_SQ_RAW, (float)w[1]);
    }
    p->nv = (uint8_t)s->xn;
    return R_OK;
}

const sens_drv_t DRV_SGP30 = {
    .prio = SP_CMD,
    .pub = { .id = "sgp30", .name = "SGP30", .addrs = { 0x58 }, .naddr = 1 },
    .identify = sgp30_identify, .init = sgp30_init, .read = sgp30_read,
};

/* ---- Sensirion's Gas Index Algorithm, for the SGP40/41 ----
 *
 * Ported from Sensirion's gas-index-algorithm 3.2.0
 * (github.com/Sensirion/gas-index-algorithm), the same arithmetic step by
 * step, with the parameters that only depend on VOC/NOx turned into
 * constants (no tuning, 1 s sampling) so the state of both fits in a
 * sensor's slot. Its licence:
 *
 * Copyright (c) 2022, Sensirion AG
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Sensirion AG nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define GIA_DT              1.0f        /* sampling interval, s */
#define GIA_BLACKOUT        45.0f
#define GIA_GAIN            230.0f
#define GIA_STD_INITIAL     50.0f
#define GIA_STD_BONUS_VOC   220.0f
#define GIA_STD_NOX         2000.0f
#define GIA_TAU_HOURS       12.0f       /* mean and variance alike */
#define GIA_TAU_INIT_VAR    2500.0f
#define GIA_GATING_INITIAL  510.0f
#define GIA_GATING_TRANS    0.09f
#define GIA_GATING_RATIO    0.3f
#define GIA_SIGMOID_L       500.0f
#define GIA_GSCALE          64.0f       /* mean variance estimator: gamma scaling */
#define GIA_GSCALE_MEAN     8.0f        /* and the additional one of the mean */
#define GIA_FIX16_MAX       32767.0f
#define GIA_LP_FAST         20.0f
#define GIA_LP_SLOW         500.0f
#define GIA_LP_ALPHA        (-0.2f)

typedef struct {
    float uptime, sraw, index;
    float mean, offset, std, g_mean, g_var, up_gamma, up_gating, gating_min;
    float mox_std, mox_mean;
    float x1, x2, x3;
    bool  nox, mve_init, lp_init;
} gia_t;

/* the parameters of VOC [0] and NOx [1] */
static const struct {
    float offset, sraw_min, gating_max_min, init_mean, init_var, gating_thr, tau_init_mean, sig_x0, sig_k;
} GIA[2] = {
    { 100.0f, 20000.0f, 60.0f * 3.0f,  3600.0f * 0.75f, 3600.0f * 1.45f, 340.0f, 20.0f,   213.0f, -0.0065f },
    { 1.0f,   10000.0f, 60.0f * 12.0f, 3600.0f * 4.75f, 3600.0f * 5.70f, 30.0f,  1200.0f, 614.0f, -0.0101f },
};

static void gia_init(gia_t *g, bool nox)
{
    memset(g, 0, sizeof *g);
    g->nox = nox;
    g->std = GIA_STD_INITIAL;
    g->mox_std = g->std;
}

static float gia_sigmoid(float x0, float k, float sample)
{
    float x = k * (sample - x0);
    if (x < -50.0f) return 1.0f;
    if (x > 50.0f) return 0.0f;
    return 1.0f / (1.0f + expf(x));
}

static void gia_gamma(gia_t *g)
{
    const int t = g->nox;
    float lim = GIA_FIX16_MAX - GIA_DT;
    if (g->up_gamma < lim) g->up_gamma += GIA_DT;
    if (g->up_gating < lim) g->up_gating += GIA_DT;
    float h = GIA_DT / 3600.0f;
    float gm = GIA_GSCALE_MEAN * GIA_GSCALE * h / (GIA_TAU_HOURS + h);
    float gv = GIA_GSCALE * h / (GIA_TAU_HOURS + h);
    float gim = GIA_GSCALE_MEAN * GIA_GSCALE * GIA_DT / (GIA[t].tau_init_mean + GIA_DT);
    float giv = GIA_GSCALE * GIA_DT / (GIA_TAU_INIT_VAR + GIA_DT);
    float thr = GIA[t].gating_thr;

    float s_gm = gia_sigmoid(GIA[t].init_mean, 0.01f, g->up_gamma);
    float gamma_mean = gm + (gim - gm) * s_gm;
    float thr_mean = thr + (GIA_GATING_INITIAL - thr) * gia_sigmoid(GIA[t].init_mean, 0.01f, g->up_gating);
    float s_gate_mean = gia_sigmoid(thr_mean, GIA_GATING_TRANS, g->index);
    g->g_mean = s_gate_mean * gamma_mean;

    float s_gv = gia_sigmoid(GIA[t].init_var, 0.01f, g->up_gamma);
    float gamma_var = gv + (giv - gv) * (s_gv - s_gm);
    float thr_var = thr + (GIA_GATING_INITIAL - thr) * gia_sigmoid(GIA[t].init_var, 0.01f, g->up_gating);
    float s_gate_var = gia_sigmoid(thr_var, GIA_GATING_TRANS, g->index);
    g->g_var = s_gate_var * gamma_var;

    g->gating_min += GIA_DT / 60.0f * ((1.0f - s_gate_mean) * (1.0f + GIA_GATING_RATIO) - GIA_GATING_RATIO);
    if (g->gating_min < 0) g->gating_min = 0;
    if (g->gating_min > GIA[t].gating_max_min) g->up_gating = 0;
}

static void gia_mve(gia_t *g, float sraw)
{
    if (!g->mve_init) {
        g->mve_init = true;
        g->offset = sraw;
        g->mean = 0;
        return;
    }
    if (g->mean >= 100.0f || g->mean <= -100.0f) {
        g->offset += g->mean;
        g->mean = 0;
    }
    sraw -= g->offset;
    gia_gamma(g);
    float d = (sraw - g->mean) / GIA_GSCALE;
    float c = g->std + (d < 0 ? -d : d);
    float add = c > 1440.0f ? (c / 1440.0f) * (c / 1440.0f) : 1.0f;
    g->std = sqrtf(add * (GIA_GSCALE - g->g_var)) *
             sqrtf(g->std * (g->std / (GIA_GSCALE * add)) + g->g_var * d / add * d);
    g->mean += g->g_mean * d / GIA_GSCALE_MEAN;
}

static float gia_scaled(const gia_t *g, float sample)
{
    const int t = g->nox;
    float x = GIA[t].sig_k * (sample - GIA[t].sig_x0);
    if (x < -50.0f) return GIA_SIGMOID_L;
    if (x > 50.0f) return 0;
    float off = GIA[t].offset;                  /* index offset = its default: no tuning */
    if (sample >= 0) {
        float shift = off == 1.0f ? 500.0f / 499.0f * (1.0f - off) : (GIA_SIGMOID_L - 5.0f * off) / 4.0f;
        return (GIA_SIGMOID_L + shift) / (1.0f + expf(x)) - shift;
    }
    return GIA_SIGMOID_L / (1.0f + expf(x));
}

static float gia_lowpass(gia_t *g, float s)
{
    const float a1 = GIA_DT / (GIA_LP_FAST + GIA_DT), a2 = GIA_DT / (GIA_LP_SLOW + GIA_DT);
    if (!g->lp_init) { g->x1 = g->x2 = g->x3 = s; g->lp_init = true; }
    g->x1 = (1.0f - a1) * g->x1 + a1 * s;
    g->x2 = (1.0f - a2) * g->x2 + a2 * s;
    float d = g->x1 - g->x2;
    if (d < 0) d = -d;
    float tau = (GIA_LP_SLOW - GIA_LP_FAST) * expf(GIA_LP_ALPHA * d) + GIA_LP_FAST;
    float a3 = GIA_DT / (GIA_DT + tau);
    g->x3 = (1.0f - a3) * g->x3 + a3 * s;
    return g->x3;
}

/* One raw sample a second in; the index out (0 during the first 45 s) */
static int gia_process(gia_t *g, int32_t sraw)
{
    const int t = g->nox;
    if (g->uptime <= GIA_BLACKOUT) {
        g->uptime += GIA_DT;
    } else {
        if (sraw > 0 && sraw < 65000) {
            int32_t mn = (int32_t)GIA[t].sraw_min;
            if (sraw < mn + 1) sraw = mn + 1;
            else if (sraw > mn + 32767) sraw = mn + 32767;
            g->sraw = (float)(sraw - mn);
        }
        float idx;
        if (!g->nox || g->mve_init) {
            idx = g->nox ? (g->sraw - g->mox_mean) / GIA_STD_NOX * GIA_GAIN
                         : (g->sraw - g->mox_mean) / -(g->mox_std + GIA_STD_BONUS_VOC) * GIA_GAIN;
            idx = gia_scaled(g, idx);
        } else {
            idx = GIA[t].offset;
        }
        idx = gia_lowpass(g, idx);
        if (idx < 0.5f) idx = 0.5f;
        g->index = idx;
        if (g->sraw > 0) {
            gia_mve(g, g->sraw);
            g->mox_std = g->std;
            g->mox_mean = g->mean + g->offset;
        }
    }
    return (int)(g->index + 0.5f);
}

/* ---- SGP40 / SGP41 (Sensirion SGP40 and SGP41 datasheets) ----
 * identify: get_serial_number (0x3682), three words with good CRCs. init
 * tells one from the other: only an SGP41 knows execute_conditioning
 * (0x2612), which it also needs for its first 10 s (no more) before the
 * NOx measurement. Then each round: SGP40 measure_raw_signal (0x260F,
 * 30 ms), SGP41 measure_raw_signals (0x2619, 50 ms), both with the
 * humidity and temperature words (50 %RH and 25 °C, or comp='s), and the
 * raw signals go through the Gas Index Algorithm: a VOC index (100 is the
 * room's average of the last hours, up to 500 worse, down to 1 better) and
 * a NOx index (1 is clean air, up to 500). Both learn for a while: the VOC
 * one is meaningful after a few minutes, the NOx one after ~5 h. */

typedef struct {
    gia_t    voc, nox;
    bool     sgp41;
    uint8_t  nv;
    uint32_t t0;
} sgp4x_t;
SPRIV_CHECK(sgp4x_t);

static int sgp4x_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t w[3];
    return sir_read(b, a, 0x3682, 1, w, 3) == R_OK ? ID_YES : ID_NO;
}

static void sgp4x_comp(const sens_slot_t *s, uint16_t *w)
{
    float t = 25, h = 50, v;
    if (air_comp(s, "temp", &v)) t = v;
    if (air_comp(s, "hum", &v)) h = v;
    w[0] = sir_rh_ticks(h);
    w[1] = sir_t_ticks(t);
}

static int sgp4x_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    sgp4x_t *p = SPRIV(s, sgp4x_t);
    memset(p, 0, sizeof *p);
    uint16_t w[3];
    int r = sir_read(b, a, 0x3682, 1, w, 3);
    if (r != R_OK) return r;
    uint16_t c[2];
    sgp4x_comp(s, c);
    bool ok = false;
    if (si2c_cmd16_w(b, a, 0x2612, c, 2)) {
        aos_hal_sleep_ms(50);
        ok = si2c_words(b, a, w, 1, &ok) && ok;
    }
    p->sgp41 = ok;
    sens_scpy(s->pub.chip, sizeof s->pub.chip, ok ? "SGP41" : "SGP40");
    gia_init(&p->voc, false);
    gia_init(&p->nox, true);
    p->t0 = sens_now();
    return R_OK;
}

static int sgp4x_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    sgp4x_t *p = SPRIV(s, sgp4x_t);
    uint16_t c[2], w[2];
    sgp4x_comp(s, c);
    bool cond = p->sgp41 && sens_now() - p->t0 < 10000;
    uint16_t cmd = !p->sgp41 ? 0x260F : cond ? 0x2612 : 0x2619;
    int n = p->sgp41 && !cond ? 2 : 1;
    if (!si2c_cmd16_w(b, a, cmd, c, 2)) return R_NACK;
    aos_hal_sleep_ms(p->sgp41 ? 50 : 30);
    bool ok = false;
    if (!si2c_words(b, a, w, n, &ok)) return R_NACK;
    if (!ok) return R_CRC;
    int voc = gia_process(&p->voc, w[0]);
    int nox = n == 2 ? gia_process(&p->nox, w[1]) : 0;
    bool raw = sens_arg_int(s, "raw", 0) != 0;
    if (!voc && !raw) return R_NOTREADY;                       /* the algorithm's first 45 s */
    int k = 0;
    sens_setv(s, k++, "voc", AOS_SQ_INDEX, voc ? (float)voc : NAN);
    if (p->sgp41) sens_setv(s, k++, "nox", AOS_SQ_INDEX, nox ? (float)nox : NAN);
    if (raw) {
        sens_setv(s, k++, "vocraw", AOS_SQ_RAW, (float)w[0]);
        if (p->sgp41) sens_setv(s, k++, "noxraw", AOS_SQ_RAW, n == 2 ? (float)w[1] : NAN);
    }
    p->nv = (uint8_t)k;
    return R_OK;
}

const sens_drv_t DRV_SGP4X = {
    .prio = SP_CMD,
    .pub = { .id = "sgp4x", .name = "SGP4x", .addrs = { 0x59 }, .naddr = 1 },
    .identify = sgp4x_identify, .init = sgp4x_init, .read = sgp4x_read,
};

/* ---- CCS811 (ams datasheet, section "I2C interface" and "Registers") ----
 * HW_ID (0x20) is 0x81 and HW_VERSION (0x21) 0x1X: two plain register
 * reads, which also tell it from an MPR121 (whose 0x20/0x21 are baselines)
 * and an MLX90614 (identified before, by its PEC). Clock stretching:
 * SF_SLOW. Its nWAKE pin must be low. init: APP_START (0xF4, a pointer
 * write alone) out of the boot loader, then MEAS_MODE (0x01) = 0x10: a
 * result every second. The first 20 minutes (and 48 h of a new one) its
 * readings are not to be trusted yet. */

typedef struct {
    uint8_t  nv;
    uint32_t env_ms;
} ccs811_t;
SPRIV_CHECK(ccs811_t);

static int ccs811_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id, ver;
    if (!si2c_rreg8(b, a, 0x20, &id) || id != 0x81) return ID_NO;
    if (!si2c_rreg8(b, a, 0x21, &ver) || (ver & 0xF0) != 0x10) return ID_NO;
    return ID_YES;
}

static int ccs811_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    ccs811_t *p = SPRIV(s, ccs811_t);
    memset(p, 0, sizeof *p);
    uint8_t id = 0, st = 0;
    if (!si2c_rreg8(b, a, 0x20, &id)) return R_NACK;
    if (id != 0x81) { sens_err(s, "0x%02X", id); return R_ID; }
    if (!si2c_rreg8(b, a, 0x00, &st)) return R_NACK;
    if (!(st & 0x10)) { sens_err(s, "noapp"); return R_ID; }     /* no valid application firmware */
    if (!(st & 0x80)) {                                            /* in the boot loader */
        uint8_t c = 0xF4;
        if (!si2c_wr(b, a, &c, 1)) return R_NACK;
        aos_hal_sleep_ms(2);
        if (!si2c_rreg8(b, a, 0x00, &st)) return R_NACK;
        if (!(st & 0x80)) return R_NOTREADY;
    }
    if (!si2c_wreg8(b, a, 0x01, 0x10)) return R_NACK;
    p->env_ms = sens_now() - 60000;
    return R_OK;
}

static int ccs811_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    ccs811_t *p = SPRIV(s, ccs811_t);
    float t, h;
    if (sens_now() - p->env_ms >= 60000 && air_comp(s, "temp", &t) && air_comp(s, "hum", &h)) {
        /* ENV_DATA: humidity and temperature + 25 °C, both in 1/512 */
        uint16_t hh = (uint16_t)lrintf(fminf(fmaxf(h, 0), 100) * 512.0f);
        uint16_t tt = (uint16_t)lrintf(fminf(fmaxf(t + 25.0f, 0), 100) * 512.0f);
        uint8_t w[5] = { 0x05, (uint8_t)(hh >> 8), (uint8_t)hh, (uint8_t)(tt >> 8), (uint8_t)tt };
        si2c_wr(b, a, w, 5);
        p->env_ms = sens_now();
    }
    uint8_t st;
    if (!si2c_rreg8(b, a, 0x00, &st)) return R_NACK;
    if (!(st & 0x80)) return R_NACK;                    /* back in the boot loader: it was reset */
    if (st & 0x01) {
        uint8_t e = 0;
        si2c_rreg8(b, a, 0xE0, &e);                     /* ERROR_ID, which also clears it */
        sens_err(s, "E%02X", e);
        return R_RANGE;
    }
    if (!(st & 0x08)) return air_keep(s, p->nv);
    uint8_t d[4];
    if (!si2c_rreg(b, a, 0x02, d, 4)) return R_NACK;
    sens_setv(s, 0, "eco2", AOS_SQ_CO2, (float)sens_be16(d));
    sens_setv(s, 1, "tvoc", AOS_SQ_TVOC, (float)sens_be16(d + 2));
    p->nv = 2;
    return R_OK;
}

const sens_drv_t DRV_CCS811 = {
    .prio = SP_REG,
    .pub = { .id = "ccs811", .name = "CCS811", .addrs = { 0x5A, 0x5B }, .naddr = 2 },
    .flags = SF_SLOW,
    .identify = ccs811_identify, .init = ccs811_init, .read = ccs811_read,
};

/* ---- ENS160 / ENS161 (ScioSense datasheet; ScioSense/Adafruit driver) ----
 * PART_ID (0x00-0x01, little endian) 0x0160 or 0x0161: a plain register
 * read; at 0x53 an LTR390 gives 0x2200 there, an ADXL345 0xE5 first.
 * init: OPMODE (0x10) reset (0xF0), idle (0x01), clear the general purpose
 * registers (COMMAND 0x12 = 0xCC), standard (0x02): a result each second.
 * DATA_STATUS (0x20): b7 running, b6 error, b3:2 validity (1 warming up,
 * the first 3 minutes; 2 the first hour of a new one), b1 new data.
 * comp= sets TEMP_IN (K x 64) and RH_IN (% x 512). */

typedef struct {
    uint8_t  nv;
    uint32_t comp_ms;
} ens160_t;
SPRIV_CHECK(ens160_t);

static int ens160_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t id;
    return si2c_rreg16le(b, a, 0x00, &id) && (id == 0x0160 || id == 0x0161) ? ID_YES : ID_NO;
}

static int ens160_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    ens160_t *p = SPRIV(s, ens160_t);
    memset(p, 0, sizeof *p);
    uint16_t id = 0;
    if (!si2c_rreg16le(b, a, 0x00, &id)) return R_NACK;
    if (id != 0x0160 && id != 0x0161) { sens_err(s, "0x%04X", id); return R_ID; }
    sens_scpy(s->pub.chip, sizeof s->pub.chip, id == 0x0161 ? "ENS161" : "ENS160");
    if (!si2c_wreg8(b, a, 0x10, 0xF0)) return R_NACK;
    aos_hal_sleep_ms(10);
    if (!si2c_wreg8(b, a, 0x10, 0x01)) return R_NACK;
    si2c_wreg8(b, a, 0x12, 0x00);
    si2c_wreg8(b, a, 0x12, 0xCC);
    aos_hal_sleep_ms(10);
    if (!si2c_wreg8(b, a, 0x10, 0x02)) return R_NACK;
    p->comp_ms = sens_now() - 60000;
    return R_OK;
}

static int ens160_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    ens160_t *p = SPRIV(s, ens160_t);
    float t, h;
    if (sens_now() - p->comp_ms >= 60000 && air_comp(s, "temp", &t) && air_comp(s, "hum", &h)) {
        uint16_t tk = (uint16_t)lrintf((t + 273.15f) * 64.0f);
        uint16_t rh = (uint16_t)lrintf(fminf(fmaxf(h, 0), 100) * 512.0f);
        uint8_t w[5] = { 0x13, (uint8_t)tk, (uint8_t)(tk >> 8), (uint8_t)rh, (uint8_t)(rh >> 8) };
        si2c_wr(b, a, w, 5);
        p->comp_ms = sens_now();
    }
    uint8_t st;
    if (!si2c_rreg8(b, a, 0x20, &st)) return R_NACK;
    if (st & 0x40) { sens_err(s, "S%02X", st); return R_RANGE; }
    if (!(st & 0x80)) return R_NACK;                    /* not running: it was reset, start it again */
    if (!(st & 0x02)) return air_keep(s, p->nv);
    uint8_t d[5];
    if (!si2c_rreg(b, a, 0x21, d, 5)) return R_NACK;
    int aqi = d[0] & 0x07;
    if (aqi < 1 || aqi > 5) return air_keep(s, p->nv);
    sens_setv(s, 0, "aqi", AOS_SQ_INDEX, (float)aqi);
    sens_setv(s, 1, "eco2", AOS_SQ_CO2, (float)sens_le16(d + 3));
    sens_setv(s, 2, "tvoc", AOS_SQ_TVOC, (float)sens_le16(d + 1));
    p->nv = 3;
    return R_OK;
}

const sens_drv_t DRV_ENS160 = {
    .prio = SP_REG,
    .pub = { .id = "ens160", .name = "ENS160", .addrs = { 0x53, 0x52 }, .naddr = 2 },
    .identify = ens160_identify, .init = ens160_init, .read = ens160_read,
};

/* ---- PMSA003I (Plantower datasheet; Adafruit_PM25AQI) ----
 * A plain 32-byte read, no register: 0x42 0x4D, the frame's length (28),
 * thirteen big-endian words and their sum. The start bytes, the length and
 * the sum prove it. Words 4-6 are the mass at atmospheric conditions, the
 * ones to show (1-3 are "CF=1", standard particle). */

static int pmsa_frame(aos_io_i2c_t *b, uint8_t a, uint8_t *d)
{
    if (!si2c_rd(b, a, d, 32)) return R_NACK;
    if (d[0] != 0x42 || d[1] != 0x4D || sens_be16(d + 2) != 28) return R_ID;
    uint16_t sum = 0;
    for (int i = 0; i < 30; i++) sum = (uint16_t)(sum + d[i]);
    return sum == sens_be16(d + 30) ? R_OK : R_CRC;
}

static int pmsa_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[32];
    return pmsa_frame(b, a, d) == R_OK ? ID_YES : ID_NO;
}

static int pmsa_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t d[32];
    int r = pmsa_frame(io->i2c, s->pub.addr, d);
    if (r == R_ID) sens_err(s, "%02X%02X", d[0], d[1]);
    if (r != R_OK) return r;
    sens_setv(s, 0, "pm1", AOS_SQ_PM, (float)sens_be16(d + 10));
    sens_setv(s, 1, "pm25", AOS_SQ_PM, (float)sens_be16(d + 12));
    sens_setv(s, 2, "pm10", AOS_SQ_PM, (float)sens_be16(d + 14));
    return R_OK;
}

const sens_drv_t DRV_PMSA003I = {
    .prio = SP_REG,
    .pub = { .id = "pmsa003i", .name = "PMSA003I", .addrs = { 0x12 }, .naddr = 1 },
    .flags = SF_SLOW,
    .identify = pmsa_identify, .read = pmsa_read,
};

/* ---- SEN50 / SEN54 / SEN55 (Sensirion SEN5x datasheet; embedded-i2c-sen5x) ----
 * 100 kHz at most (SF_SLOW). identify: get_product_name (0xD014, 50 ms),
 * 16 words of CRC'd ASCII starting "SEN5" - an SPS30 at the same address
 * does not know the command. It comes after the MPU-6050 and the other
 * register chips at 0x69 (SP_CMD). start_measurement (0x0021, 50 ms);
 * read_data_ready (0x0202, 20 ms); read_measured_values (0x03C4, 20 ms):
 * PM1, PM2.5, PM4, PM10 (u16 / 10 µg/m³), RH (i16 / 100), T (i16 / 200),
 * VOC and NOx index (i16 / 10); 0xFFFF / 0x7FFF while not known yet. The
 * SEN54 has no NOx, the SEN50 measures only particles. */

typedef struct {
    uint8_t nv;
    char    model;                  /* '0', '4', '5' */
} sen5x_t;
SPRIV_CHECK(sen5x_t);

static int sen5x_name(aos_io_i2c_t *b, uint8_t a, char *name)
{
    uint16_t w[16];
    int r = sir_read(b, a, 0xD014, 50, w, 16);
    if (r != R_OK) return r;
    for (int i = 0; i < 16; i++) { name[2 * i] = (char)(w[i] >> 8); name[2 * i + 1] = (char)w[i]; }
    name[31] = 0;
    return strncmp(name, "SEN5", 4) ? R_ID : R_OK;
}

static int sen5x_identify(aos_io_i2c_t *b, uint8_t a)
{
    char name[32];
    return sen5x_name(b, a, name) == R_OK ? ID_YES : ID_NO;
}

static int sen5x_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    sen5x_t *p = SPRIV(s, sen5x_t);
    memset(p, 0, sizeof *p);
    char name[32];
    int r = sen5x_name(b, a, name);
    if (r == R_ID) sens_err(s, "%.8s", name);
    if (r != R_OK) return r;
    p->model = name[4] == '0' || name[4] == '4' ? name[4] : '5';
    sens_scpy(s->pub.chip, sizeof s->pub.chip, name);
    /* already measuring (a reboot of the board): the chip refuses the
     * start, and the data-ready flag still answers */
    if (si2c_cmd16(b, a, 0x0021)) aos_hal_sleep_ms(50);
    return R_OK;
}

static float sen5x_v(uint16_t w, bool sgn, float div)
{
    if (sgn ? w == 0x7FFF : w == 0xFFFF) return NAN;
    return (sgn ? (float)(int16_t)w : (float)w) / div;
}

static int sen5x_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    sen5x_t *p = SPRIV(s, sen5x_t);
    uint16_t w[8];
    int r = sir_read(b, a, 0x0202, 20, w, 1);
    if (r != R_OK) return r;
    if (!(w[0] & 0xFF)) return air_keep(s, p->nv);
    r = sir_read(b, a, 0x03C4, 20, w, 8);
    if (r != R_OK) return r;
    int k = 0;
    sens_setv(s, k++, "pm1", AOS_SQ_PM, sen5x_v(w[0], false, 10));
    sens_setv(s, k++, "pm25", AOS_SQ_PM, sen5x_v(w[1], false, 10));
    sens_setv(s, k++, "pm4", AOS_SQ_PM, sen5x_v(w[2], false, 10));
    sens_setv(s, k++, "pm10", AOS_SQ_PM, sen5x_v(w[3], false, 10));
    if (p->model != '0') {
        sens_setv(s, k++, "temp", AOS_SQ_TEMP, sen5x_v(w[5], true, 200));
        sens_setv(s, k++, "hum", AOS_SQ_HUM, sen5x_v(w[4], true, 100));
        sens_setv(s, k++, "voc", AOS_SQ_INDEX, sen5x_v(w[6], true, 10));
    }
    if (p->model == '5') sens_setv(s, k++, "nox", AOS_SQ_INDEX, sen5x_v(w[7], true, 10));
    p->nv = (uint8_t)k;
    return R_OK;
}

const sens_drv_t DRV_SEN5X = {
    .prio = SP_CMD,
    .pub = { .id = "sen5x", .name = "SEN5x", .addrs = { 0x69 }, .naddr = 1 },
    .flags = SF_SLOW,
    .identify = sen5x_identify, .init = sen5x_init, .read = sen5x_read,
};

/* ---- SPS30 (Sensirion SPS30 datasheet; embedded-i2c-sps30) ----
 * 100 kHz at most. identify: read product type (0xD002), 4 words of
 * CRC'd ASCII, "00080000". start_measurement (0x0010) with output format
 * 0x0300, big-endian floats, which every firmware has (the 0x0500 integers
 * came with 2.0 and lose the decimals); read_data_ready (0x0202);
 * read_measured_values (0x0300): the first four floats are PM1, PM2.5, PM4
 * and PM10 in µg/m³, the six after them (number concentrations and the
 * typical size) are not read. */

typedef struct { uint8_t nv; } sps30_t;
SPRIV_CHECK(sps30_t);

static int sps30_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t w[4];
    if (sir_read(b, a, 0xD002, 1, w, 4) != R_OK) return ID_NO;
    return w[0] == 0x3030 && w[1] == 0x3038 && w[2] == 0x3030 && w[3] == 0x3030 ? ID_YES : ID_NO;
}

static int sps30_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    SPRIV(s, sps30_t)->nv = 0;
    uint16_t w[4];
    int r = sir_read(b, a, 0xD002, 1, w, 4);
    if (r != R_OK) return r;
    if (w[0] != 0x3030 || w[1] != 0x3038) { sens_err(s, "%04X%04X", w[0], w[1]); return R_ID; }
    uint16_t fmt = 0x0300;
    if (si2c_cmd16_w(b, a, 0x0010, &fmt, 1)) aos_hal_sleep_ms(20);   /* refused if already measuring */
    return R_OK;
}

static int sps30_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    sps30_t *p = SPRIV(s, sps30_t);
    uint16_t w[8];
    int r = sir_read(b, a, 0x0202, 1, w, 1);
    if (r != R_OK) return r;
    if (!(w[0] & 0xFF)) return air_keep(s, p->nv);
    r = sir_read(b, a, 0x0300, 1, w, 8);
    if (r != R_OK) return r;
    static const char *const KEY[4] = { "pm1", "pm25", "pm4", "pm10" };
    for (int i = 0; i < 4; i++) {
        float v = sir_float(w + 2 * i);
        if (!(v >= 0 && v <= 10000)) return R_RANGE;
        sens_setv(s, i, KEY[i], AOS_SQ_PM, v);
    }
    p->nv = 4;
    return R_OK;
}

const sens_drv_t DRV_SPS30 = {
    .prio = SP_CMD,
    .pub = { .id = "sps30", .name = "SPS30", .addrs = { 0x69 }, .naddr = 1 },
    .flags = SF_SLOW,
    .identify = sps30_identify, .init = sps30_init, .read = sps30_read,
};

/* ---- HM3301 (Seeed Grove "Laser PM2.5 Sensor (HM3301)", Seeed_PM2_5_sensor_HM3301) ----
 * 0x88 selects I2C, then a 29-byte frame: words 5-7 (bytes 10-15) are PM1,
 * PM2.5 and PM10 at atmospheric conditions, byte 28 the sum of the others.
 * 0x40 is the INA219's, the HDC1080's, the SHT2x's... so it comes last,
 * and a frame proves it only when its sum is right, it is not all zeros
 * and the masses grow from PM1 to PM10. */

static int hm3301_frame(aos_io_i2c_t *b, uint8_t a, uint8_t *d)
{
    uint8_t c = 0x88;
    if (!si2c_wr(b, a, &c, 1) || !si2c_rd(b, a, d, 29)) return R_NACK;
    uint8_t sum = 0, any = 0;
    for (int i = 0; i < 28; i++) { sum = (uint8_t)(sum + d[i]); any |= d[i]; }
    if (sum != d[28]) return R_CRC;
    uint16_t p1 = sens_be16(d + 10), p25 = sens_be16(d + 12), p10 = sens_be16(d + 14);
    if (!any || p1 > p25 || p25 > p10 || p10 > 1000) return R_ID;
    return R_OK;
}

static int hm3301_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[29];
    return hm3301_frame(b, a, d) == R_OK ? ID_YES : ID_NO;
}

static int hm3301_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t d[29];
    int r = hm3301_frame(io->i2c, s->pub.addr, d);
    if (r == R_ID) sens_err(s, "frame");
    if (r != R_OK) return r;
    sens_setv(s, 0, "pm1", AOS_SQ_PM, (float)sens_be16(d + 10));
    sens_setv(s, 1, "pm25", AOS_SQ_PM, (float)sens_be16(d + 12));
    sens_setv(s, 2, "pm10", AOS_SQ_PM, (float)sens_be16(d + 14));
    return R_OK;
}

const sens_drv_t DRV_HM3301 = {
    .prio = SP_LAST,
    .pub = { .id = "hm3301", .name = "HM3301", .addrs = { 0x40 }, .naddr = 1 },
    .flags = SF_SLOW,
    .identify = hm3301_identify, .read = hm3301_read,
};

const sens_drv_t *const SENS_G_AIR[] = {
    &DRV_SCD4X, &DRV_SCD30, &DRV_SGP30, &DRV_SGP4X, &DRV_CCS811, &DRV_ENS160,
    &DRV_PMSA003I, &DRV_SEN5X, &DRV_SPS30, &DRV_HM3301, NULL,
};

const sens_alias_t SENS_A_AIR[] = {
    { "scd40", "scd4x" }, { "scd41", "scd4x" }, { "scd43", "scd4x" },
    { "sgp40", "sgp4x" }, { "sgp41", "sgp4x" },
    { "ens161", "ens160" },
    { "sen50", "sen5x" }, { "sen54", "sen5x" }, { "sen55", "sen5x" },
    { NULL, NULL },
};
