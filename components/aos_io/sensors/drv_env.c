/*
 * P4OS - Sensor drivers: pressure (barometers) on I2C (sens_int.h, docs/SENSORS.md).
 *
 * BME680/688, BMP180, BMP388/390, BMP581/585, DPS310 (and SPL06, HP303B),
 * the LPS22/LPS25 family, MS5611/MS5607 and QMP6988. The BME280/BMP280 are
 * in drv_basic.c.
 *
 * Who answers at 0x76/0x77 is decided by the table's order: every chip with
 * an id register (BME280/BMP280 first, then the BME680, BMP180, DPS310 and
 * BMP3xx here) before the MS5611, which has no id and proves itself only by
 * the CRC-4 of its PROM (SP_LAST). A DPS310 and a BMP3xx each check that
 * they are not the other one: the BMP3xx's chip id is at 0x00, where a
 * DPS310 keeps its pressure, and the DPS310's product id at 0x0D, where a
 * BMP3xx keeps its sensor time.
 */
#include "sens_int.h"

/* Declared as this chip: the module's name ("ms5607", "spl06_patio") or
 * its chip= ("chip=ms5607") starts with it */
static bool declared_as(const sens_slot_t *s, const char *pre)
{
    char c[16] = "";
    sens_arg_str(s, "chip", c, sizeof c);
    size_t n = strlen(pre);
    return !strncmp(s->pub.name, pre, n) || !strncmp(c, pre, n);
}

/* bits-wide two's complement */
static int32_t sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    v &= (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1);
    return (int32_t)(v ^ m) - (int32_t)m;
}

/* ---- BME680 / BME688: temperature, humidity, pressure and the gas
 * resistance of the hot plate, in forced mode. The compensation is Bosch's
 * floating-point one (BME68x_SensorAPI, bme68x.c: calc_temperature,
 * calc_pressure, calc_humidity, calc_gas_resistance_low/high,
 * calc_res_heat, calc_gas_wait). No BSEC: the gas value is the raw
 * resistance, not an air-quality index.
 *
 * A measurement is T x2, P x16, H x1 (~43 ms) plus the heater (150 ms by
 * default, heat_ms=): longer than a read may block. So read() starts one
 * and returns, and the next round collects it and starts the next one; the
 * values lag a round behind. ---- */

typedef struct {
    uint16_t t1; int16_t t2; int8_t t3;
    uint16_t p1; int16_t p2; int8_t p3; int16_t p4, p5; int8_t p6, p7; int16_t p8, p9; uint8_t p10;
    uint16_t h1, h2; int8_t h3, h4, h5; uint8_t h6; int8_t h7;
    int8_t   gh1; int16_t gh2; int8_t gh3;
    uint8_t  heat_range; int8_t heat_val, sw_err;
    bool     high;              /* BME688: the "gas high" ADC (variant 1) */
    uint16_t heat_c, heat_ms;
    uint16_t wait_ms;           /* a measurement's length, heater included */
    uint32_t trig_ms;           /* when the measurement in progress started; 0 none */
    float    amb;               /* the last temperature, for the heater's setting */
} bme68_t;
SPRIV_CHECK(bme68_t);

static int bme68_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0xD0, &id) && id == 0x61 ? ID_YES : ID_NO;
}

/* res_heat_0 for a plate temperature, from the ambient one */
static uint8_t bme68_res_heat(const bme68_t *k, float temp)
{
    if (temp > 400) temp = 400;
    float v1 = (float)k->gh1 / 16.0f + 49.0f;
    float v2 = (float)k->gh2 / 32768.0f * 0.0005f + 0.00235f;
    float v3 = (float)k->gh3 / 1024.0f;
    float v4 = v1 * (1.0f + v2 * temp);
    float v5 = v4 + v3 * k->amb;
    float r = 3.4f * (v5 * (4.0f / (4.0f + (float)k->heat_range)) * (1.0f / (1.0f + (float)k->heat_val * 0.002f)) - 25.0f);
    return r < 0 ? 0 : r > 255 ? 255 : (uint8_t)r;
}

/* gas_wait_0: 6 bits of time and 2 of a multiplier (1, 4, 16, 64) */
static uint8_t bme68_gas_wait(uint16_t ms)
{
    if (ms >= 0xFC0) return 0xFF;
    uint8_t f = 0;
    while (ms > 0x3F) { ms /= 4; f++; }
    return (uint8_t)(ms + f * 64);
}

static int bme68_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0, var = 0;
    if (!si2c_rreg8(b, a, 0xD0, &id)) return R_NACK;
    if (id != 0x61) { sens_err(s, "0x%02X", id); return R_ID; }
    bme68_t *k = SPRIV(s, bme68_t);
    memset(k, 0, sizeof *k);
    si2c_wreg8(b, a, 0xE0, 0xB6);                  /* soft reset, 10 ms */
    aos_hal_sleep_ms(10);
    /* the three calibration blocks, in the API's coeff_array order */
    uint8_t c[42];
    if (!si2c_rreg8(b, a, 0xF0, &var) || !si2c_rreg(b, a, 0x8A, c, 23) ||
        !si2c_rreg(b, a, 0xE1, c + 23, 14) || !si2c_rreg(b, a, 0x00, c + 37, 5)) return R_NACK;
    k->high = (var & 0x0F) == 1;
    k->t1 = (uint16_t)(c[32] << 8 | c[31]);  k->t2 = (int16_t)(c[1] << 8 | c[0]);  k->t3 = (int8_t)c[2];
    k->p1 = (uint16_t)(c[5] << 8 | c[4]);    k->p2 = (int16_t)(c[7] << 8 | c[6]);  k->p3 = (int8_t)c[8];
    k->p4 = (int16_t)(c[11] << 8 | c[10]);   k->p5 = (int16_t)(c[13] << 8 | c[12]);
    k->p6 = (int8_t)c[15];                   k->p7 = (int8_t)c[14];
    k->p8 = (int16_t)(c[19] << 8 | c[18]);   k->p9 = (int16_t)(c[21] << 8 | c[20]); k->p10 = c[22];
    k->h1 = (uint16_t)(c[25] << 4 | (c[24] & 0x0F));
    k->h2 = (uint16_t)(c[23] << 4 | c[24] >> 4);
    k->h3 = (int8_t)c[26]; k->h4 = (int8_t)c[27]; k->h5 = (int8_t)c[28]; k->h6 = c[29]; k->h7 = (int8_t)c[30];
    k->gh1 = (int8_t)c[35]; k->gh2 = (int16_t)(c[34] << 8 | c[33]); k->gh3 = (int8_t)c[36];
    k->heat_val = (int8_t)c[37];
    k->heat_range = (uint8_t)((c[39] & 0x30) >> 4);
    k->sw_err = (int8_t)((int8_t)(c[41] & 0xF0) / 16);
    if (k->t1 == 0 || k->p1 == 0) return R_ID;     /* no calibration: not a real one */

    int32_t hc = sens_arg_int(s, "heat", 320), hm = sens_arg_int(s, "heat_ms", 150);
    k->heat_c = (uint16_t)(hc < 200 ? 200 : hc > 400 ? 400 : hc);
    k->heat_ms = (uint16_t)(hm < 1 ? 1 : hm > 4000 ? 4000 : hm);
    /* 19 oversampling cycles of 1963 µs, the switching and gas phases
     * (477 µs x 9) and the wake-up (1 ms): bme68x_get_meas_dur() */
    k->wait_ms = (uint16_t)(43 + k->heat_ms + 5);
    k->amb = 25.0f;
    sens_scpy(s->pub.chip, sizeof s->pub.chip, k->high ? "BME688" : "BME680");
    /* humidity x1 (latched by the ctrl_meas write), no filter, T x2 P x16 asleep */
    if (!si2c_wreg8(b, a, 0x72, 0x01) || !si2c_wreg8(b, a, 0x75, 0x00) || !si2c_wreg8(b, a, 0x74, 0x54)) return R_NACK;
    /* heater profile 0: its wait, heater on, run_gas (0x10 on a 680, 0x20 on a 688), nb_conv 0 */
    if (!si2c_wreg8(b, a, 0x64, bme68_gas_wait(k->heat_ms)) || !si2c_wreg8(b, a, 0x70, 0x00) ||
        !si2c_wreg8(b, a, 0x71, k->high ? 0x20 : 0x10)) return R_NACK;
    return R_OK;
}

static int bme68_trigger(aos_io_i2c_t *b, uint8_t a, bme68_t *k)
{
    if (!si2c_wreg8(b, a, 0x5A, bme68_res_heat(k, k->heat_c)) || !si2c_wreg8(b, a, 0x74, 0x55)) return R_NACK;
    k->trig_ms = sens_now() | 1;
    return R_NOTREADY;
}

static int bme68_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    bme68_t *k = SPRIV(s, bme68_t);
    if (!k->trig_ms) return bme68_trigger(b, a, k);
    uint32_t since = sens_now() - k->trig_ms;
    if (since < k->wait_ms) return R_NOTREADY;
    uint8_t d[17];                                 /* field 0: 0x1D..0x2D */
    if (!si2c_rreg(b, a, 0x1D, d, sizeof d)) return R_NACK;
    if (!(d[0] & 0x80)) {
        if ((d[0] & 0x60) && since < 2000) return R_NOTREADY;   /* still measuring */
        return bme68_trigger(b, a, k);                          /* lost: start another */
    }
    uint32_t adc_p = (uint32_t)d[2] << 12 | (uint32_t)d[3] << 4 | d[4] >> 4;
    uint32_t adc_t = (uint32_t)d[5] << 12 | (uint32_t)d[6] << 4 | d[7] >> 4;
    uint16_t adc_h = (uint16_t)(d[8] << 8 | d[9]);
    if (adc_t == 0x80000) return bme68_trigger(b, a, k);       /* skipped: not configured */

    float v1 = ((float)adc_t / 16384.0f - (float)k->t1 / 1024.0f) * (float)k->t2;
    float dt = (float)adc_t / 131072.0f - (float)k->t1 / 8192.0f;
    float v2 = dt * dt * ((float)k->t3 * 16.0f);
    float t_fine = v1 + v2;
    float temp = t_fine / 5120.0f;

    v1 = t_fine / 2.0f - 64000.0f;
    v2 = v1 * v1 * ((float)k->p6 / 131072.0f);
    v2 = v2 + v1 * (float)k->p5 * 2.0f;
    v2 = v2 / 4.0f + (float)k->p4 * 65536.0f;
    v1 = ((float)k->p3 * v1 * v1 / 16384.0f + (float)k->p2 * v1) / 524288.0f;
    v1 = (1.0f + v1 / 32768.0f) * (float)k->p1;
    float pres = 1048576.0f - (float)adc_p;
    if ((int)v1 == 0) return R_RANGE;
    pres = (pres - v2 / 4096.0f) * 6250.0f / v1;
    v1 = (float)k->p9 * pres * pres / 2147483648.0f;
    v2 = pres * ((float)k->p8 / 32768.0f);
    float v3 = (pres / 256.0f) * (pres / 256.0f) * (pres / 256.0f) * ((float)k->p10 / 131072.0f);
    pres = pres + (v1 + v2 + v3 + (float)k->p7 * 128.0f) / 16.0f;

    v1 = (float)adc_h - ((float)k->h1 * 16.0f + (float)k->h3 / 2.0f * temp);
    v2 = v1 * ((float)k->h2 / 262144.0f * (1.0f + (float)k->h4 / 16384.0f * temp + (float)k->h5 / 1048576.0f * temp * temp));
    v3 = (float)k->h6 / 16384.0f;
    float v4 = (float)k->h7 / 2097152.0f;
    float hum = v2 + (v3 + v4 * temp) * v2 * v2;
    if (hum > 100) hum = 100;
    if (hum < 0) hum = 0;

    sens_setv(s, 0, "temp", AOS_SQ_TEMP, temp);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, hum);
    sens_setv(s, 2, "press", AOS_SQ_PRESS, pres / 100.0f);

    /* the gas: valid and with the plate at its temperature, else left out */
    const uint8_t *g = k->high ? d + 15 : d + 13;
    if ((g[1] & 0x30) == 0x30) {
        uint16_t adc_g = (uint16_t)(g[0] << 2 | g[1] >> 6);
        uint8_t rng = g[1] & 0x0F;
        float ohm;
        if (k->high) {
            uint32_t u1 = 262144u >> rng;
            int32_t u2 = 4096 + ((int32_t)adc_g - 512) * 3;
            ohm = u2 ? 1000000.0f * (float)u1 / (float)u2 : 0;
        } else {
            static const float K1[16] = { 0, 0, 0, 0, 0, -1.0f, 0, -0.8f, 0, 0, -0.2f, -0.5f, 0, -1.0f, 0, 0 };
            static const float K2[16] = { 0, 0, 0, 0, 0.1f, 0.7f, 0, -0.8f, -0.1f, 0, 0, 0, 0, 0, 0, 0 };
            float g1 = 1340.0f + 5.0f * (float)k->sw_err;
            float g2 = g1 * (1.0f + K1[rng] / 100.0f);
            float g3 = 1.0f + K2[rng] / 100.0f;
            ohm = 1.0f / (g3 * 0.000000125f * (float)(1u << rng) * (((float)adc_g - 512.0f) / g2 + 1.0f));
        }
        if (ohm > 0) sens_setv(s, 3, "gas", AOS_SQ_OHM, ohm);
    }
    k->amb = temp;
    return bme68_trigger(b, a, k) == R_NACK ? R_NACK : R_OK;
}

const sens_drv_t DRV_BME680 = {
    .prio = SP_REG,
    .pub = { .id = "bme680", .name = "BME680", .addrs = { 0x77, 0x76 }, .naddr = 2 },
    .identify = bme68_identify, .init = bme68_init, .read = bme68_read,
};

/* ---- BMP180 / BMP085: temperature then pressure, each a conversion of its
 * own (4.5 ms, and 25.5 ms at oss=3). Datasheet BST-BMP180-DS000 §3.5; the
 * integer arithmetic as in Linux's bmp280-core.c (bmp180_compensate_press). ---- */

typedef struct {
    int16_t  ac1, ac2, ac3; uint16_t ac4, ac5, ac6;
    int16_t  b1, b2, mb, mc, md;
    uint8_t  oss;
} bmp180_t;
SPRIV_CHECK(bmp180_t);

static int bmp180_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0xD0, &id) && id == 0x55 ? ID_YES : ID_NO;
}

static int bmp180_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0, c[22];
    if (!si2c_rreg8(b, a, 0xD0, &id)) return R_NACK;
    if (id != 0x55) { sens_err(s, "0x%02X", id); return R_ID; }
    if (!si2c_rreg(b, a, 0xAA, c, sizeof c)) return R_NACK;
    /* "none of the words has the value 0 or 0xFFFF": the datasheet's check of the link */
    for (int i = 0; i < 11; i++) {
        uint16_t w = sens_be16(c + 2 * i);
        if (w == 0 || w == 0xFFFF) return R_CRC;
    }
    bmp180_t *k = SPRIV(s, bmp180_t);
    k->ac1 = (int16_t)sens_be16(c + 0);  k->ac2 = (int16_t)sens_be16(c + 2);  k->ac3 = (int16_t)sens_be16(c + 4);
    k->ac4 = sens_be16(c + 6);           k->ac5 = sens_be16(c + 8);           k->ac6 = sens_be16(c + 10);
    k->b1 = (int16_t)sens_be16(c + 12);  k->b2 = (int16_t)sens_be16(c + 14);
    k->mb = (int16_t)sens_be16(c + 16);  k->mc = (int16_t)sens_be16(c + 18);  k->md = (int16_t)sens_be16(c + 20);
    int32_t oss = sens_arg_int(s, "oss", 3);
    k->oss = (uint8_t)(oss < 0 ? 0 : oss > 3 ? 3 : oss);
    return R_OK;
}

/* one conversion: start it, wait its time, and wait for sco (0xF4 bit 5) to drop */
static bool bmp180_conv(aos_io_i2c_t *b, uint8_t a, uint8_t cmd, int ms, uint8_t *out, size_t n)
{
    if (!si2c_wreg8(b, a, 0xF4, cmd)) return false;
    aos_hal_sleep_ms((uint32_t)ms);
    for (int i = 0; i < 5; i++) {
        uint8_t c = 0;
        if (!si2c_rreg8(b, a, 0xF4, &c)) return false;
        if (!(c & 0x20)) break;
        aos_hal_sleep_ms(2);
    }
    return si2c_rreg(b, a, 0xF6, out, n);
}

static int bmp180_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    const bmp180_t *k = SPRIV(s, bmp180_t);
    uint8_t d[3];
    if (!bmp180_conv(b, a, 0x2E, 5, d, 2)) return R_NACK;
    int32_t ut = (int32_t)sens_be16(d);
    int oss = k->oss;
    if (!bmp180_conv(b, a, (uint8_t)(0x34 | oss << 6), 2 + (3 << oss) + 1, d, 3)) return R_NACK;
    uint32_t up = sens_be24(d) >> (8 - oss);

    int32_t x1 = (int32_t)(((int64_t)ut - k->ac6) * k->ac5 >> 15);
    if (x1 + k->md == 0) return R_RANGE;
    int32_t x2 = ((int32_t)k->mc << 11) / (x1 + k->md);
    int32_t b5 = x1 + x2;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)((b5 + 8) >> 4) / 10.0f);

    int32_t b6 = b5 - 4000;
    x1 = (k->b2 * (b6 * b6 >> 12)) >> 11;
    x2 = k->ac2 * b6 >> 11;
    int32_t x3 = x1 + x2;
    int32_t b3 = ((((int32_t)k->ac1 * 4 + x3) << oss) + 2) / 4;
    x1 = k->ac3 * b6 >> 13;
    x2 = (k->b1 * ((b6 * b6) >> 12)) >> 16;
    x3 = (x1 + x2 + 2) >> 2;
    uint32_t b4 = (uint32_t)k->ac4 * (uint32_t)(x3 + 32768) >> 15;
    if (!b4) return R_RANGE;
    uint32_t b7 = (up - (uint32_t)b3) * (uint32_t)(50000 >> oss);
    int32_t p = b7 < 0x80000000u ? (int32_t)(b7 * 2 / b4) : (int32_t)(b7 / b4 * 2);
    x1 = (p >> 8) * (p >> 8);
    x1 = (x1 * 3038) >> 16;
    x2 = (-7357 * p) >> 16;
    p += (x1 + x2 + 3791) >> 4;
    sens_setv(s, 1, "press", AOS_SQ_PRESS, (float)p / 100.0f);
    return R_OK;
}

const sens_drv_t DRV_BMP180 = {
    .prio = SP_REG,
    .pub = { .id = "bmp180", .name = "BMP180", .addrs = { 0x77 }, .naddr = 1 },
    .identify = bmp180_identify, .init = bmp180_init, .read = bmp180_read,
};

/* ---- DPS310 / DPS368 / SPL06-001 / HP303B: background mode, pressure x16
 * and temperature x1, four of each a second. Infineon's datasheet §4.9
 * (the scale factors kP/kT and the polynomial); Linux's dps310.c for the
 * start-up: the temperature always from the MEMS element (TMP_EXT, as the
 * coefficients are made with it; on the SPL06 too), MEAS_CFG written 0
 * before the mode, and the fix for the chips that read some 60 °C when
 * register 0x32 bit 1 is clear (undocumented, not on a declared SPL06). ---- */

typedef struct {
    int16_t  c0, c1;
    int32_t  c00, c10;
    int16_t  c01, c11, c20, c21, c30;
    bool     had;
} dps_t;
SPRIV_CHECK(dps_t);

#define DPS_KP 253952.0f        /* x16 */
#define DPS_KT 524288.0f        /* x1 */

static bool bmp3_looks(aos_io_i2c_t *b, uint8_t a);

/* product id 0x10 at 0x0D and the sensor and coefficients ready (MEAS_CFG
 * bits 7 and 6); and not a BMP3xx whose sensor time happens to say 0x10 */
static bool dps_looks(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t pid, mc;
    return si2c_rreg8(b, a, 0x0D, &pid) && pid == 0x10 && si2c_rreg8(b, a, 0x08, &mc) && (mc & 0xC0) == 0xC0;
}

static int dps_identify(aos_io_i2c_t *b, uint8_t a)
{
    return dps_looks(b, a) && !bmp3_looks(b, a) ? ID_YES : ID_NO;
}

static int dps_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, pid = 0, mc = 0;
    if (!si2c_rreg8(b, a, 0x0D, &pid)) return R_NACK;
    if (pid != 0x10) { sens_err(s, "0x%02X", pid); return R_ID; }
    si2c_wreg8(b, a, 0x0C, 0x89);                  /* FIFO flush and soft reset */
    aos_hal_sleep_ms(40);                          /* the coefficients are ready 40 ms after */
    for (int i = 0; i < 10; i++) {
        if (si2c_rreg8(b, a, 0x08, &mc) && (mc & 0xC0) == 0xC0) break;
        aos_hal_sleep_ms(5);
    }
    if ((mc & 0xC0) != 0xC0) return R_NOTREADY;
    uint8_t c[18];
    if (!si2c_rreg(b, a, 0x10, c, sizeof c)) return R_NACK;
    dps_t *k = SPRIV(s, dps_t);
    memset(k, 0, sizeof *k);
    k->c0 = (int16_t)sext((uint32_t)c[0] << 4 | c[1] >> 4, 12);
    k->c1 = (int16_t)sext((uint32_t)(c[1] & 0x0F) << 8 | c[2], 12);
    k->c00 = sext((uint32_t)c[3] << 12 | (uint32_t)c[4] << 4 | c[5] >> 4, 20);
    k->c10 = sext((uint32_t)(c[5] & 0x0F) << 16 | (uint32_t)c[6] << 8 | c[7], 20);
    k->c01 = (int16_t)sens_be16(c + 8);  k->c11 = (int16_t)sens_be16(c + 10);
    k->c20 = (int16_t)sens_be16(c + 12); k->c21 = (int16_t)sens_be16(c + 14);
    k->c30 = (int16_t)sens_be16(c + 16);
    bool spl = declared_as(s, "spl06");
    uint8_t fix = 0x02;
    if (!spl && si2c_rreg8(b, a, 0x32, &fix) && !(fix & 0x02)) {
        static const uint8_t W[5][2] = { { 0x0E, 0xA5 }, { 0x0F, 0x96 }, { 0x62, 0x02 }, { 0x0E, 0x00 }, { 0x0F, 0x00 } };
        for (int i = 0; i < 5; i++) si2c_wreg8(b, a, W[i][0], W[i][1]);
    }
    sens_scpy(s->pub.chip, sizeof s->pub.chip, spl ? "SPL06" : declared_as(s, "hp303b") ? "HP303B" :
              declared_as(s, "dps368") ? "DPS368" : "DPS310");
    /* P: 4 a second x16 (27.6 ms each); T: 4 a second x1, external;
     * P_SHIFT, required above x8 */
    if (!si2c_wreg8(b, a, 0x06, 0x24) || !si2c_wreg8(b, a, 0x07, 0xA0) || !si2c_wreg8(b, a, 0x09, 0x04) ||
        !si2c_wreg8(b, a, 0x08, 0x00) || !si2c_wreg8(b, a, 0x08, 0x07)) return R_NACK;
    return R_OK;
}

static int dps_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, mc = 0, d[6];
    dps_t *k = SPRIV(s, dps_t);
    if (!si2c_rreg8(b, a, 0x08, &mc)) return R_NACK;
    if ((mc & 0x07) != 0x07) return R_NACK;        /* lost its mode: it was reset, start again */
    if (!k->had && (mc & 0x30) != 0x30) return R_NOTREADY;
    if (!si2c_rreg(b, a, 0x00, d, sizeof d)) return R_NACK;
    k->had = true;
    float psc = (float)sext(sens_be24(d), 24) / DPS_KP;
    float tsc = (float)sext(sens_be24(d + 3), 24) / DPS_KT;
    float t = (float)k->c0 * 0.5f + (float)k->c1 * tsc;
    float p = (float)k->c00 + psc * ((float)k->c10 + psc * ((float)k->c20 + psc * (float)k->c30)) +
              tsc * (float)k->c01 + tsc * psc * ((float)k->c11 + psc * (float)k->c21);
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, t);
    sens_setv(s, 1, "press", AOS_SQ_PRESS, p / 100.0f);
    return R_OK;
}

const sens_drv_t DRV_DPS310 = {
    .prio = SP_REG,
    .pub = { .id = "dps310", .name = "DPS310", .addrs = { 0x77, 0x76 }, .naddr = 2 },
    .identify = dps_identify, .init = dps_init, .read = dps_read,
};

/* ---- BMP388 / BMP390: normal mode at 12.5 Hz, P x8 T x1, IIR 3. The
 * floating-point compensation of Bosch's BMP3_SensorAPI (bmp3.c:
 * parse_calib_data, compensate_temperature/pressure), in double. ---- */

typedef struct {
    double t1, t2, t3, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
    bool   had;
} bmp3_t;
SPRIV_CHECK(bmp3_t);

/* chip id 0x50 (388) or 0x60 (390) at 0x00, the reserved bits of ERR_REG
 * clear, and not a DPS310 (whose pressure is what 0x00 holds there) */
static bool bmp3_looks(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id, err;
    if (!si2c_rreg8(b, a, 0x00, &id) || (id != 0x50 && id != 0x60)) return false;
    return si2c_rreg8(b, a, 0x02, &err) && !(err & 0xF8);
}

static int bmp3_identify(aos_io_i2c_t *b, uint8_t a)
{
    return bmp3_looks(b, a) && !dps_looks(b, a) ? ID_YES : ID_NO;
}

static int bmp3_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0, st = 0, c[21];
    if (!si2c_rreg8(b, a, 0x00, &id)) return R_NACK;
    if (id != 0x50 && id != 0x60) { sens_err(s, "0x%02X", id); return R_ID; }
    si2c_wreg8(b, a, 0x7E, 0xB6);                  /* soft reset */
    aos_hal_sleep_ms(5);
    for (int i = 0; i < 10; i++) {                 /* cmd_rdy */
        if (si2c_rreg8(b, a, 0x03, &st) && (st & 0x10)) break;
        aos_hal_sleep_ms(2);
    }
    if (!si2c_rreg(b, a, 0x31, c, sizeof c)) return R_NACK;
    bmp3_t *k = SPRIV(s, bmp3_t);
    memset(k, 0, sizeof *k);
    uint16_t t1 = (uint16_t)(c[1] << 8 | c[0]), t2 = (uint16_t)(c[3] << 8 | c[2]);
    if (t1 == 0 || t1 == 0xFFFF) return R_ID;
    k->t1 = (double)t1 * 256.0;
    k->t2 = (double)t2 / 1073741824.0;
    k->t3 = (double)(int8_t)c[4] / 281474976710656.0;
    k->p1 = ((double)(int16_t)(c[6] << 8 | c[5]) - 16384.0) / 1048576.0;
    k->p2 = ((double)(int16_t)(c[8] << 8 | c[7]) - 16384.0) / 536870912.0;
    k->p3 = (double)(int8_t)c[9] / 4294967296.0;
    k->p4 = (double)(int8_t)c[10] / 137438953472.0;
    k->p5 = (double)(uint16_t)(c[12] << 8 | c[11]) * 8.0;
    k->p6 = (double)(uint16_t)(c[14] << 8 | c[13]) / 64.0;
    k->p7 = (double)(int8_t)c[15] / 256.0;
    k->p8 = (double)(int8_t)c[16] / 32768.0;
    k->p9 = (double)(int16_t)(c[18] << 8 | c[17]) / 281474976710656.0;
    k->p10 = (double)(int8_t)c[19] / 281474976710656.0;
    k->p11 = (double)(int8_t)c[20] / 36893488147419103232.0;
    sens_scpy(s->pub.chip, sizeof s->pub.chip, id == 0x60 ? "BMP390" : "BMP388");
    /* OSR P x8 T x1, ODR 12.5 Hz, IIR coefficient 3, then press_en temp_en normal */
    if (!si2c_wreg8(b, a, 0x1C, 0x03) || !si2c_wreg8(b, a, 0x1D, 0x04) || !si2c_wreg8(b, a, 0x1F, 0x04) ||
        !si2c_wreg8(b, a, 0x1B, 0x33)) return R_NACK;
    uint8_t err = 0;
    if (si2c_rreg8(b, a, 0x02, &err) && (err & 0x04)) { sens_err(s, "conf"); return R_RANGE; }
    return R_OK;
}

static int bmp3_read(sens_io_t *io, sens_slot_t *s)
{
    bmp3_t *k = SPRIV(s, bmp3_t);
    uint8_t d[7];                                  /* STATUS, then pressure and temperature, LE */
    if (!si2c_rreg(io->i2c, s->pub.addr, 0x03, d, sizeof d)) return R_NACK;
    if (!k->had && (d[0] & 0x60) != 0x60) return R_NOTREADY;
    k->had = true;
    double up = (double)sens_le24(d + 1), ut = (double)sens_le24(d + 4);
    double pd1 = ut - k->t1;
    double tl = pd1 * k->t2 + pd1 * pd1 * k->t3;
    double o1 = k->p5 + k->p6 * tl + k->p7 * tl * tl + k->p8 * tl * tl * tl;
    double o2 = up * (k->p1 + k->p2 * tl + k->p3 * tl * tl + k->p4 * tl * tl * tl);
    double o3 = up * up * (k->p9 + k->p10 * tl) + up * up * up * k->p11;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)tl);
    sens_setv(s, 1, "press", AOS_SQ_PRESS, (float)((o1 + o2 + o3) / 100.0));
    return R_OK;
}

const sens_drv_t DRV_BMP3XX = {
    .prio = SP_REG,
    .pub = { .id = "bmp3xx", .name = "BMP388", .addrs = { 0x77, 0x76 }, .naddr = 2 },
    .identify = bmp3_identify, .init = bmp3_init, .read = bmp3_read,
};

/* ---- BMP581 / BMP585: normal mode at 5 Hz, P x16 T x2. The chip
 * compensates itself: T = s24 / 2^16 °C, P = u24 / 2^6 Pa. Bosch's
 * BMP5_SensorAPI (bmp5.c: bmp5_init's NVM check, bmp5_soft_reset,
 * bmp5_get_sensor_data). At 0x46/0x47, before the INA219 (SP_REG2). ---- */

typedef struct { bool had; } bmp5_t;
SPRIV_CHECK(bmp5_t);

static int bmp5_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id, st;
    if (!si2c_rreg8(b, a, 0x01, &id) || (id != 0x50 && id != 0x51)) return ID_NO;
    /* STATUS: nvm_rdy set and nvm_err clear, what bmp5_init() asks */
    return si2c_rreg8(b, a, 0x28, &st) && (st & 0x06) == 0x02 ? ID_YES : ID_NO;
}

static int bmp5_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0, st = 0;
    if (!si2c_rreg8(b, a, 0x01, &id)) return R_NACK;
    if (id != 0x50 && id != 0x51) { sens_err(s, "0x%02X", id); return R_ID; }
    si2c_wreg8(b, a, 0x7E, 0xB6);                  /* soft reset, 2 ms */
    aos_hal_sleep_ms(3);
    si2c_rreg8(b, a, 0x27, &st);                   /* INT_STATUS: reading clears the POR flag */
    if (!si2c_rreg8(b, a, 0x28, &st)) return R_NACK;
    if ((st & 0x06) != 0x02) { sens_err(s, "nvm"); return R_NOTREADY; }
    SPRIV(s, bmp5_t)->had = false;
    sens_scpy(s->pub.chip, sizeof s->pub.chip, id == 0x51 ? "BMP585" : "BMP581");
    /* standby with deep standby off, then press_en P x16 T x2, then 5 Hz normal */
    const uint8_t odr = 0x18;
    if (!si2c_wreg8(b, a, 0x37, (uint8_t)(0x80 | odr << 2))) return R_NACK;
    aos_hal_sleep_ms(3);
    if (!si2c_wreg8(b, a, 0x36, 0x40 | 4 << 3 | 1) || !si2c_wreg8(b, a, 0x37, (uint8_t)(0x80 | odr << 2 | 0x01))) return R_NACK;
    return R_OK;
}

static int bmp5_read(sens_io_t *io, sens_slot_t *s)
{
    bmp5_t *k = SPRIV(s, bmp5_t);
    uint8_t d[6];
    if (!si2c_rreg(io->i2c, s->pub.addr, 0x1D, d, sizeof d)) return R_NACK;
    uint32_t rt = sens_le24(d), rp = sens_le24(d + 3);
    if (!k->had && rt == 0 && rp == 0) return R_NOTREADY;
    if (rp == 0) return R_NOTREADY;
    k->had = true;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)sext(rt, 24) / 65536.0f);
    sens_setv(s, 1, "press", AOS_SQ_PRESS, (float)rp / 64.0f / 100.0f);
    return R_OK;
}

const sens_drv_t DRV_BMP5XX = {
    .prio = SP_REG,
    .pub = { .id = "bmp5xx", .name = "BMP581", .addrs = { 0x47, 0x46 }, .naddr = 2 },
    .identify = bmp5_identify, .init = bmp5_init, .read = bmp5_read,
};

/* ---- ST LPS22HB (and LPS33HW/LPS35HW, the same chip), LPS22HH, LPS22DF,
 * LPS25HB: continuous at 10 Hz (7 Hz on the 25), block data update. P =
 * s24 / 4096 hPa on all; T = s16 / 100 °C, and 42.5 + s16 / 480 on the 25.
 * The register maps from ST's lps22hh-pid and lps22df-pid drivers and
 * Adafruit_LPS2X (LPS22HB/LPS25HB). The LPS25HB auto-increments only with
 * bit 7 of the sub-address set; the others always do (IF_ADD_INC). ---- */

typedef struct { uint8_t who; bool had; } lps_t;
SPRIV_CHECK(lps_t);

static bool lps_who(uint8_t w) { return w == 0xB1 || w == 0xB3 || w == 0xB4 || w == 0xBD; }

static int lps_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t w, st;
    if (!si2c_rreg8(b, a, 0x0F, &w) || !lps_who(w)) return ID_NO;
    /* STATUS keeps bits 7, 6, 3 and 2 at 0 on all of them: a BH1750 at 0x5C
     * answers every read with its count, the same byte both times */
    return si2c_rreg8(b, a, 0x27, &st) && !(st & 0xCC) ? ID_YES : ID_NO;
}

static int lps_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, w = 0;
    if (!si2c_rreg8(b, a, 0x0F, &w)) return R_NACK;
    if (!lps_who(w)) { sens_err(s, "0x%02X", w); return R_ID; }
    lps_t *k = SPRIV(s, lps_t);
    k->who = w;
    k->had = false;
    bool ok;
    switch (w) {
    case 0xBD:                                     /* LPS25HB: SWRESET, then PD, 7 Hz, BDU */
        si2c_wreg8(b, a, 0x21, 0x04);
        aos_hal_sleep_ms(5);
        ok = si2c_wreg8(b, a, 0x20, 0xA4);
        break;
    case 0xB4:                                     /* LPS22DF: SWRESET; BDU; 10 Hz, 16 averages */
        si2c_wreg8(b, a, 0x11, 0x04);
        aos_hal_sleep_ms(5);
        ok = si2c_wreg8(b, a, 0x11, 0x08) && si2c_wreg8(b, a, 0x10, 0x03 << 3 | 0x02);
        break;
    case 0xB3:                                     /* LPS22HH: SWRESET; IF_ADD_INC + low noise (in power-down); 10 Hz, BDU */
        si2c_wreg8(b, a, 0x11, 0x14);
        aos_hal_sleep_ms(5);
        ok = si2c_wreg8(b, a, 0x11, 0x12) && si2c_wreg8(b, a, 0x10, 0x22);
        break;
    default:                                       /* LPS22HB: SWRESET; IF_ADD_INC; 10 Hz, BDU */
        si2c_wreg8(b, a, 0x11, 0x14);
        aos_hal_sleep_ms(5);
        ok = si2c_wreg8(b, a, 0x11, 0x10) && si2c_wreg8(b, a, 0x10, 0x22);
        break;
    }
    if (!ok) return R_NACK;
    sens_scpy(s->pub.chip, sizeof s->pub.chip,
              w == 0xBD ? "LPS25HB" : w == 0xB4 ? "LPS22DF" : w == 0xB3 ? "LPS22HH" : "LPS22HB");
    return R_OK;
}

static int lps_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, st = 0, d[5];
    lps_t *k = SPRIV(s, lps_t);
    if (!si2c_rreg8(b, a, 0x27, &st)) return R_NACK;
    if (!k->had && (st & 0x03) != 0x03) return R_NOTREADY;
    if (!si2c_rreg(b, a, k->who == 0xBD ? 0xA8 : 0x28, d, sizeof d)) return R_NACK;
    k->had = true;
    int32_t p = sext(sens_le24(d), 24);
    int16_t t = (int16_t)sens_le16(d + 3);
    float tc = k->who == 0xBD ? 42.5f + (float)t / 480.0f : (float)t / 100.0f;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, tc);
    sens_setv(s, 1, "press", AOS_SQ_PRESS, (float)p / 4096.0f);
    return R_OK;
}

/* 0x5D is the touch's on i2c.board; on i2c.ext it is the usual one */
const sens_drv_t DRV_LPS22 = {
    .prio = SP_REG,
    .pub = { .id = "lps22", .name = "LPS22HB", .addrs = { 0x5D, 0x5C }, .naddr = 2 },
    .identify = lps_identify, .init = lps_init, .read = lps_read,
};

/* ---- MS5611 / MS5607 (GY-63): commands, not registers. Reset 0x1E, PROM
 * 0xA0..0xAE, convert D1 0x48 / D2 0x58 at OSR 4096 (9.04 ms), ADC read
 * 0x00. No id: the PROM's CRC-4 (TE AN520) proves it, so it is tried last
 * at 0x76/0x77. Formulas as in the datasheets and Linux's ms5611_core.c.
 *
 * The two differ only in the scale of OFF and SENS, and nothing in the
 * chip says which one it is: read with the other's formula, the pressure
 * comes out halved or doubled. So model=5607 / model=5611 says it, a module
 * declared as ms5607 is one, one declared otherwise is an MS5611, and a
 * detected one is told by its first reading: the formula that gives 600 to
 * 1200 hPa, the MS5611's when both do or neither does. (An MS5611 above
 * some 4200 m, under 600 hPa, would be taken for an MS5607: declare it.) ---- */

typedef struct { uint16_t c[8]; uint8_t model; } ms56_t;   /* 0 not known yet, 1 MS5611, 2 MS5607 */
SPRIV_CHECK(ms56_t);

static bool ms56_crc_ok(const uint16_t *prom)
{
    uint16_t n[8], rem = 0;
    memcpy(n, prom, sizeof n);
    uint16_t want = n[7] & 0x000F;
    n[7] &= 0xFF00;
    for (int i = 0; i < 16; i++) {
        rem ^= (uint16_t)(i & 1 ? n[i >> 1] & 0x00FF : n[i >> 1] >> 8);
        for (int k = 0; k < 8; k++) rem = (uint16_t)(rem & 0x8000 ? (rem << 1) ^ 0x3000 : rem << 1);
    }
    return ((rem >> 12) & 0x0F) == want;
}

static bool ms56_prom(aos_io_i2c_t *b, uint8_t a, uint16_t *prom)
{
    for (int i = 0; i < 8; i++) {
        uint8_t d[2];
        if (!si2c_rreg(b, a, (uint8_t)(0xA0 + 2 * i), d, 2)) return false;
        prom[i] = sens_be16(d);
    }
    return true;
}

/* The CRC matches and C1..C6 are real coefficients. A register chip read the
 * same way shows its registers 0xA0..0xAF: one in 16 would pass the CRC,
 * which is why every chip with an id goes first. */
static bool ms56_valid(const uint16_t *p)
{
    for (int i = 1; i <= 6; i++) if (p[i] == 0 || p[i] == 0xFFFF) return false;
    return ms56_crc_ok(p);
}

static int ms56_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t p[8];
    return ms56_prom(b, a, p) && ms56_valid(p) ? ID_YES : ID_NO;
}

static int ms56_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, rst = 0x1E;
    if (!si2c_wr(b, a, &rst, 1)) return R_NACK;
    aos_hal_sleep_ms(3);                           /* the PROM reloads in 2.8 ms */
    ms56_t *k = SPRIV(s, ms56_t);
    if (!ms56_prom(b, a, k->c)) return R_NACK;
    if (!ms56_valid(k->c)) { sens_err(s, "prom"); return R_CRC; }
    int32_t m = sens_arg_int(s, "model", 0);
    if (m == 5607 || m == 5611) k->model = m == 5607 ? 2 : 1;
    else if (s->pub.declared) k->model = declared_as(s, "ms5607") ? 2 : 1;
    else k->model = 0;
    if (k->model) sens_scpy(s->pub.chip, sizeof s->pub.chip, k->model == 2 ? "MS5607" : "MS5611");
    return R_OK;
}

static int ms56_adc(aos_io_i2c_t *b, uint8_t a, uint8_t cmd, uint32_t *v)
{
    uint8_t rd = 0x00, d[3];
    if (!si2c_wr(b, a, &cmd, 1)) return R_NACK;
    aos_hal_sleep_ms(10);
    if (!si2c_wrrd(b, a, &rd, 1, d, 3)) return R_NACK;
    *v = sens_be24(d);
    return *v ? R_OK : R_NOTREADY;                 /* 0: no conversion finished */
}

/* temperature (0.01 °C) and pressure (0.01 hPa) with one model's formulas */
static void ms56_calc(const uint16_t *c, bool m5607, uint32_t d1, uint32_t d2, int64_t *temp, int64_t *press)
{
    int64_t dt = (int64_t)d2 - ((int64_t)c[5] << 8);
    int64_t t = 2000 + ((dt * c[6]) >> 23);
    int64_t off, sens;
    if (m5607) {
        off = ((int64_t)c[2] << 17) + ((c[4] * dt) >> 6);
        sens = ((int64_t)c[1] << 16) + ((c[3] * dt) >> 7);
    } else {
        off = ((int64_t)c[2] << 16) + ((c[4] * dt) >> 7);
        sens = ((int64_t)c[1] << 15) + ((c[3] * dt) >> 8);
    }
    if (t < 2000) {                                /* the second order, below 20 °C */
        int64_t t2 = (dt * dt) >> 31, q = (t - 2000) * (t - 2000), off2, sens2;
        if (m5607) { off2 = (61 * q) >> 4; sens2 = q << 1; }
        else { off2 = (5 * q) >> 1; sens2 = (5 * q) >> 2; }
        if (t < -1500) {
            int64_t q2 = (t + 1500) * (t + 1500);
            if (m5607) { off2 += 15 * q2; sens2 += 8 * q2; }
            else { off2 += 7 * q2; sens2 += (11 * q2) >> 1; }
        }
        t -= t2;
        off -= off2;
        sens -= sens2;
    }
    *temp = t;
    *press = ((((int64_t)d1 * sens) >> 21) - off) >> 15;
}

static int ms56_read(sens_io_t *io, sens_slot_t *s)
{
    ms56_t *k = SPRIV(s, ms56_t);
    uint32_t d1, d2;
    int r = ms56_adc(io->i2c, s->pub.addr, 0x48, &d1);
    if (r == R_OK) r = ms56_adc(io->i2c, s->pub.addr, 0x58, &d2);
    if (r != R_OK) return r;
    int64_t t, p;
    if (!k->model) {
        ms56_calc(k->c, false, d1, d2, &t, &p);
        bool ok11 = p >= 60000 && p <= 120000;
        ms56_calc(k->c, true, d1, d2, &t, &p);
        bool ok07 = p >= 60000 && p <= 120000;
        k->model = ok07 && !ok11 ? 2 : 1;
        sens_scpy(s->pub.chip, sizeof s->pub.chip, k->model == 2 ? "MS5607" : "MS5611");
    }
    ms56_calc(k->c, k->model == 2, d1, d2, &t, &p);
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)t / 100.0f);
    sens_setv(s, 1, "press", AOS_SQ_PRESS, (float)p / 100.0f);
    return R_OK;
}

const sens_drv_t DRV_MS5611 = {
    .prio = SP_LAST,
    .pub = { .id = "ms5611", .name = "MS5611", .addrs = { 0x77, 0x76 }, .naddr = 2 },
    .identify = ms56_identify, .init = ms56_init, .read = ms56_read,
};

/* ---- QMP6988 (M5Stack ENV III): normal mode, T x2 P x16, IIR 4. The OTP
 * coefficients and the fixed-point compensation as in ESPHome's
 * qmp6988.cpp (QST's reference code); they match the datasheet's
 * conversion table (A + S * k / 32767).
 *
 * Its two addresses are 0x70 and 0x56. 0x70 is also where a TCA9548A
 * multiplexer lives, and the mux takes ANY written byte as its channel
 * mask: the pointer write of 0xD1 to read the chip id would switch four of
 * its channels on, joining their buses to this one. So autodetect looks
 * for it at 0x56 only (probe[]); at 0x70 it is declared:
 * "module qmp6988 i2c.ext" (0x70 is its default address). ---- */

typedef struct {
    int32_t a0, b00;                    /* 20Q4 */
    int64_t a1, a2, bt1, bt2, bp1, b11, bp2, b12, b21, bp3;
    bool    had;
} qmp_t;
SPRIV_CHECK(qmp_t);

static int qmp_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0xD1, &id) && id == 0x5C ? ID_YES : ID_NO;
}

static int qmp_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0, o[25];
    if (!si2c_rreg8(b, a, 0xD1, &id)) return R_NACK;
    if (id != 0x5C) { sens_err(s, "0x%02X", id); return R_ID; }
    si2c_wreg8(b, a, 0xE0, 0xE6);                  /* reset */
    aos_hal_sleep_ms(10);
    si2c_wreg8(b, a, 0xE0, 0x00);
    for (int i = 0; i < 25; i++)                   /* the OTP a byte at a time, as the reference does */
        if (!si2c_rreg8(b, a, (uint8_t)(0xA0 + i), &o[i])) return R_NACK;
    bool same = true;
    for (int i = 1; i < 25; i++) if (o[i] != o[0]) same = false;
    if (same) { sens_err(s, "otp"); return R_ID; }
    qmp_t *k = SPRIV(s, qmp_t);
    memset(k, 0, sizeof *k);
    k->a0 = sext((uint32_t)o[18] << 12 | (uint32_t)o[19] << 4 | (o[24] & 0x0F), 20);
    k->b00 = sext((uint32_t)o[0] << 12 | (uint32_t)o[1] << 4 | o[24] >> 4, 20);
    int16_t w[11];
    for (int i = 0; i < 11; i++) w[i] = (int16_t)sens_be16(o + 2 + 2 * i);
    /* big-endian words from o[2]: bt1 bt2 bp1 b11 bp2 b12 b21 bp3, (a0's
     * high part), a1, a2; o[24] holds the low nibbles of b00 and a0 */
    k->bt1 = 2982LL * w[0] + 107370906LL;          /* 28Q15 */
    k->bt2 = 329854LL * w[1] + 108083093LL;        /* 34Q38 */
    k->bp1 = 19923LL * w[2] + 1133836764LL;        /* 31Q20 */
    k->b11 = 2406LL * w[3] + 118215883LL;          /* 28Q34 */
    k->bp2 = 3079LL * w[4] - 181579595LL;          /* 29Q43 */
    k->b12 = 6846LL * w[5] + 85590281LL;           /* 29Q53 */
    k->b21 = 13836LL * w[6] + 79333336LL;          /* 29Q60 */
    k->bp3 = 2915LL * w[7] + 157155561LL;          /* 28Q65 */
    k->a1 = 3608LL * w[9] - 1731677965LL;          /* 31Q23 */
    k->a2 = 16889LL * w[10] - 87619360LL;          /* 30Q47 */
    /* IIR 4; T x2 P x16, normal */
    if (!si2c_wreg8(b, a, 0xF1, 0x02) || !si2c_wreg8(b, a, 0xF4, 2 << 5 | 5 << 2 | 0x03)) return R_NACK;
    return R_OK;
}

static int qmp_read(sens_io_t *io, sens_slot_t *s)
{
    qmp_t *k = SPRIV(s, qmp_t);
    uint8_t d[6];
    if (!si2c_rreg(io->i2c, s->pub.addr, 0xF7, d, sizeof d)) return R_NACK;
    uint32_t rp = sens_be24(d), rt = sens_be24(d + 3);
    if (!k->had && rp == 0 && rt == 0) return R_NOTREADY;
    k->had = true;
    int64_t dp = (int64_t)rp - 8388608, dtt = (int64_t)rt - 8388608;
    /* temperature, 1/256 °C */
    int64_t w1 = k->a1 * dtt;
    int64_t w2 = (k->a2 * dtt) >> 14;
    w2 = (w2 * dtt) >> 10;
    w2 = ((w1 + w2) / 32767) >> 19;
    int64_t tx = (k->a0 + w2) >> 4;
    /* pressure, 1/16 Pa */
    int64_t p1 = k->bt1 * tx;
    p1 += (k->bp1 * dp) >> 5;
    int64_t q = ((k->bt2 * tx) >> 1);
    q = (q * tx) >> 8;
    int64_t w3 = q;
    q = (k->b11 * tx) >> 4;
    q = (q * dp) >> 1;
    w3 += q;
    q = (k->bp2 * dp) >> 13;
    q = (q * dp) >> 1;
    w3 += q;
    p1 += w3 >> 14;
    q = k->b12 * tx;
    q = (q * tx) >> 22;
    q = (q * dp) >> 1;
    w3 = q;
    q = (k->b21 * tx) >> 6;
    q = (q * dp) >> 23;
    q = (q * dp) >> 1;
    w3 += q;
    q = (k->bp3 * dp) >> 12;
    q = (q * dp) >> 23;
    q = q * dp;
    w3 += q;
    p1 += w3 >> 15;
    p1 /= 32767;
    p1 >>= 11;
    p1 += k->b00;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)tx / 256.0f);
    sens_setv(s, 1, "press", AOS_SQ_PRESS, (float)p1 / 16.0f / 100.0f);
    return R_OK;
}

const sens_drv_t DRV_QMP6988 = {
    .prio = SP_REG,
    .pub = { .id = "qmp6988", .name = "QMP6988", .addrs = { 0x70, 0x56 }, .naddr = 2 },
    .probe = { 0x56 }, .nprobe = 1,
    .identify = qmp_identify, .init = qmp_init, .read = qmp_read,
};

/* The DPS310 before the BMP3xx (each also checks it is not the other); the
 * MS5611 is SP_LAST and goes after all of them anyway. */
const sens_drv_t *const SENS_G_ENV[] = {
    &DRV_BME680, &DRV_BMP180, &DRV_DPS310, &DRV_BMP3XX, &DRV_BMP5XX, &DRV_LPS22, &DRV_QMP6988,
    &DRV_MS5611, NULL,
};

const sens_alias_t SENS_A_ENV[] = {
    { "bme688", "bme680" },
    { "bmp085", "bmp180" },
    { "bmp388", "bmp3xx" }, { "bmp390", "bmp3xx" },
    { "bmp581", "bmp5xx" }, { "bmp585", "bmp5xx" },
    { "dps368", "dps310" }, { "spl06", "dps310" }, { "spl06_001", "dps310" }, { "hp303b", "dps310" },
    { "lps22hb", "lps22" }, { "lps22hh", "lps22" }, { "lps22df", "lps22" }, { "lps25", "lps22" },
    { "lps25hb", "lps22" }, { "lps33", "lps22" }, { "lps33hw", "lps22" }, { "lps35", "lps22" },
    { "lps35hw", "lps22" },
    { "ms5607", "ms5611" }, { "gy63", "ms5611" },
    { NULL, NULL },
};
