/*
 * P4OS - The first drivers of the sensor service (2026-09 to 2026-10-18),
 * moved here as they were: BME280/BMP280, SHT3x, SHT4x, AHT20/AHT10,
 * BH1750, INA219, INA226, ADS1115, and the MAX6675 on SPI. The DS18B20's
 * entry is here too; the service reads its 1-Wire buses itself.
 */
#include "sens_int.h"

/* ---- BME280 / BMP280 (Bosch datasheet, section 4.2.3 and 8.2) ---- */

typedef struct {
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1, H3; int16_t H2, H4, H5; int8_t H6;
    bool     hum;
} bme_cal_t;
SPRIV_CHECK(bme_cal_t);

static int bme_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0xD0, &id) && id == 0x60 ? ID_YES : ID_NO;
}

static int bmp_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0xD0, &id) && (id == 0x58 || id == 0x56 || id == 0x57) ? ID_YES : ID_NO;
}

static int bme_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0xD0, &id)) return R_NACK;
    bme_cal_t *k = SPRIV(s, bme_cal_t);
    memset(k, 0, sizeof *k);
    if (id == 0x60) k->hum = true;
    else if (id == 0x58 || id == 0x56 || id == 0x57) k->hum = false;
    else { sens_err(s, "0x%02X", id); return R_ID; }
    /* declared as the other one: trust the chip */
    sens_scpy(s->pub.chip, sizeof s->pub.chip, k->hum ? "BME280" : "BMP280");
    si2c_wreg8(b, a, 0xE0, 0xB6);                  /* soft reset: a known state */
    aos_hal_sleep_ms(4);
    for (int i = 0; i < 10; i++) {                  /* im_update: the NVM copy is done */
        uint8_t st = 1;
        if (si2c_rreg8(b, a, 0xF3, &st) && !(st & 0x01)) break;
        aos_hal_sleep_ms(2);
    }
    uint8_t c[26], h[7];
    if (!si2c_rreg(b, a, 0x88, c, 26)) return R_NACK;
    k->T1 = sens_le16(c + 0);  k->T2 = (int16_t)sens_le16(c + 2);  k->T3 = (int16_t)sens_le16(c + 4);
    k->P1 = sens_le16(c + 6);  k->P2 = (int16_t)sens_le16(c + 8);  k->P3 = (int16_t)sens_le16(c + 10);
    k->P4 = (int16_t)sens_le16(c + 12); k->P5 = (int16_t)sens_le16(c + 14); k->P6 = (int16_t)sens_le16(c + 16);
    k->P7 = (int16_t)sens_le16(c + 18); k->P8 = (int16_t)sens_le16(c + 20); k->P9 = (int16_t)sens_le16(c + 22);
    k->H1 = c[25];
    if (k->hum) {
        if (!si2c_rreg(b, a, 0xE1, h, 7)) return R_NACK;
        k->H2 = (int16_t)sens_le16(h);
        k->H3 = h[2];
        k->H4 = (int16_t)((int16_t)(int8_t)h[3] * 16 | (h[4] & 0x0F));
        k->H5 = (int16_t)((int16_t)(int8_t)h[5] * 16 | (h[4] >> 4));
        k->H6 = (int8_t)h[6];
        si2c_wreg8(b, a, 0xF2, 0x01);              /* humidity x1: before ctrl_meas, which latches it */
    }
    if (k->T1 == 0 || k->P1 == 0) return R_ID;     /* no calibration: not a real one */
    si2c_wreg8(b, a, 0xF5, 0x88);                  /* standby 500 ms, IIR filter 4 */
    si2c_wreg8(b, a, 0xF4, 0x57);                  /* T x2, P x16, normal mode */
    return R_OK;
}

static int bme_read(sens_io_t *io, sens_slot_t *s)
{
    const bme_cal_t *k = SPRIV(s, bme_cal_t);
    uint8_t d[8];
    int n = k->hum ? 8 : 6;
    if (!si2c_rreg(io->i2c, s->pub.addr, 0xF7, d, (size_t)n)) return R_NACK;
    int32_t adc_P = (int32_t)(d[0] << 12 | d[1] << 4 | d[2] >> 4);
    int32_t adc_T = (int32_t)(d[3] << 12 | d[4] << 4 | d[5] >> 4);
    if (adc_T == 0x80000) return R_NOTREADY;       /* the reset value: no measurement yet */

    /* temperature, 0.01 °C, and t_fine for the other two */
    int32_t v1 = ((((adc_T >> 3) - ((int32_t)k->T1 << 1))) * (int32_t)k->T2) >> 11;
    int32_t v2 = (((((adc_T >> 4) - (int32_t)k->T1) * ((adc_T >> 4) - (int32_t)k->T1)) >> 12) * (int32_t)k->T3) >> 14;
    int32_t t_fine = v1 + v2;
    int32_t T = (t_fine * 5 + 128) >> 8;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, T / 100.0f);

    /* pressure, Pa in Q24.8 */
    int64_t p1 = (int64_t)t_fine - 128000;
    int64_t p2 = p1 * p1 * (int64_t)k->P6;
    p2 += (p1 * (int64_t)k->P5) << 17;
    p2 += ((int64_t)k->P4) << 35;
    p1 = ((p1 * p1 * (int64_t)k->P3) >> 8) + ((p1 * (int64_t)k->P2) << 12);
    p1 = ((((int64_t)1) << 47) + p1) * (int64_t)k->P1 >> 33;
    if (p1 == 0 || adc_P == 0x80000) return R_NOTREADY;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - p2) * 3125) / p1;
    p1 = ((int64_t)k->P9 * (p >> 13) * (p >> 13)) >> 25;
    p2 = ((int64_t)k->P8 * p) >> 19;
    p = ((p + p1 + p2) >> 8) + ((int64_t)k->P7 << 4);
    sens_setv(s, 1, "press", AOS_SQ_PRESS, (float)p / 256.0f / 100.0f);

    if (k->hum) {
        int32_t adc_H = (int32_t)(d[6] << 8 | d[7]);
        if (adc_H == 0x8000) return R_NOTREADY;
        int32_t x = t_fine - 76800;
        x = (((((adc_H << 14) - ((int32_t)k->H4 << 20) - ((int32_t)k->H5 * x)) + 16384) >> 15) *
             (((((((x * (int32_t)k->H6) >> 10) * (((x * (int32_t)k->H3) >> 11) + 32768)) >> 10) + 2097152) *
               (int32_t)k->H2 + 8192) >> 14));
        x = x - (((((x >> 15) * (x >> 15)) >> 7) * (int32_t)k->H1) >> 4);
        if (x < 0) x = 0;
        if (x > 419430400) x = 419430400;
        sens_setv(s, 2, "hum", AOS_SQ_HUM, (float)(x >> 12) / 1024.0f);
    }
    return R_OK;
}

const sens_drv_t DRV_BME280 = {
    .prio = SP_REG,
    .cat = SC(ENV),
    .pub = { .id = "bme280", .name = "BME280", .addrs = { 0x76, 0x77 }, .naddr = 2 },
    .identify = bme_identify, .init = bme_init, .read = bme_read,
};
const sens_drv_t DRV_BMP280 = {
    .prio = SP_REG,
    .cat = SC(ENV),
    .pub = { .id = "bmp280", .name = "BMP280", .addrs = { 0x76, 0x77 }, .naddr = 2 },
    .identify = bmp_identify, .init = bme_init, .read = bme_read,
};

/* ---- SHT3x / SHT4x: single shot, high repeatability, no clock stretching ---- */

static int sht_pair(sens_slot_t *s, const uint8_t *d, bool sht4)
{
    if (sens_crc8_si(d, 2) != d[2] || sens_crc8_si(d + 3, 2) != d[5]) return R_CRC;
    float t = -45.0f + 175.0f * (float)sens_be16(d) / 65535.0f;
    float h = sht4 ? -6.0f + 125.0f * (float)sens_be16(d + 3) / 65535.0f
                   : 100.0f * (float)sens_be16(d + 3) / 65535.0f;
    if (h < 0) h = 0;
    if (h > 100) h = 100;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, t);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, h);
    return R_OK;
}

static int sht3_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[3];
    if (si2c_cmd16(b, a, 0xF32D) && si2c_rd(b, a, d, 3) && sens_crc8_si(d, 2) == d[2]) return ID_YES;
    return ID_NO;
}

static int sht4_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t sn = 0x89, d[6];
    if (!si2c_wr(b, a, &sn, 1)) return ID_NO;
    aos_hal_sleep_ms(2);
    if (si2c_rd(b, a, d, 6) && sens_crc8_si(d, 2) == d[2] && sens_crc8_si(d + 3, 2) == d[5]) return ID_YES;
    return ID_NO;
}

/* something at 0x44/0x45 that proved nothing above: most likely an SHT3x */
static int sht3_candidate(aos_io_i2c_t *b, uint8_t a) { (void)b; (void)a; return ID_MAYBE; }

static int sht3_read(sens_io_t *io, sens_slot_t *s)
{
    if (!si2c_cmd16(io->i2c, s->pub.addr, 0x2400)) return R_NACK;
    aos_hal_sleep_ms(16);
    uint8_t d[6];
    if (!si2c_rd(io->i2c, s->pub.addr, d, 6)) return R_NACK;
    return sht_pair(s, d, false);
}

static int sht4_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t c = 0xFD;                              /* high precision */
    if (!si2c_wr(io->i2c, s->pub.addr, &c, 1)) return R_NACK;
    aos_hal_sleep_ms(10);
    uint8_t d[6];
    if (!si2c_rd(io->i2c, s->pub.addr, d, 6)) return R_NACK;
    return sht_pair(s, d, true);
}

const sens_drv_t DRV_SHT3X = {
    .prio = SP_CMD,
    .cat = SC(TEMP),
    .pub = { .id = "sht3x", .name = "SHT3x", .addrs = { 0x44, 0x45 }, .naddr = 2 },
    .identify = sht3_identify, .read = sht3_read,
};
const sens_drv_t DRV_SHT4X = {
    .prio = SP_CMD,
    .cat = SC(TEMP),
    .pub = { .id = "sht4x", .name = "SHT4x", .addrs = { 0x44, 0x45 }, .naddr = 2 },
    .probe = { 0x44, 0x45, 0x46 }, .nprobe = 3,
    .identify = sht4_identify, .read = sht4_read,
};
/* the last word at 0x44/0x45, after every chip that proves itself there */
const sens_drv_t DRV_SHT3X_CAND = {
    .prio = SP_LAST,
    .cat = SC(TEMP),
    .pub = { .id = "sht3x", .name = "SHT3x", .addrs = { 0x44, 0x45 }, .naddr = 2 },
    .flags = SF_HIDDEN,
    .identify = sht3_candidate, .read = sht3_read,
};

/* ---- AHT20 / AHT10 (and DHT20, AM2301B, AHT25/30: the same at 0x38) ----
 * The two speak alike, but an AHT20 ends every read with a CRC byte and an
 * AHT10 does not: its seventh byte is anything. Nothing else tells them
 * apart, so autodetect calls it an AHT20 when that byte is a good CRC and
 * an AHT10 when it is not, and the reads go on looking: two good CRCs in a
 * row make it an AHT20 for good (a CRC is checked from then on), three bad
 * ones with nothing good before make it an AHT10. Declared as an aht10, it
 * is one from the start. */
typedef struct {
    bool    set;            /* crc decided from the module line once */
    int8_t  crc;            /* 1 has one, 0 has none, -1 not known yet */
    uint8_t good, bad;      /* CRCs in a row, while not known */
} aht_t;
SPRIV_CHECK(aht_t);

static bool aht_crc_ok(const uint8_t *r) { return sens_crc8_si(r, 6) == r[6]; }

/* Any read of an AHT gives its status, then the last measurement and its
 * CRC: a plain read, with no 0x71 written first, because a PCF8574A shares
 * 0x38 and takes any byte written to it as its outputs. An expander answers
 * a read with its port, the same byte over and over: that is not an AHT. */
static int aht_probe(aos_io_i2c_t *b, uint8_t a, bool *crc)
{
    uint8_t r[7];
    if (!si2c_rd(b, a, r, sizeof r)) return ID_NO;
    bool same = true;
    for (int i = 1; i < 7; i++) same &= r[i] == r[0];
    if (same) return ID_NO;
    *crc = aht_crc_ok(r);
    return !(r[0] & 0x80) && (r[0] & 0x08) ? ID_YES : ID_MAYBE;
}

static int aht20_identify(aos_io_i2c_t *b, uint8_t a)
{
    bool crc;
    int r = aht_probe(b, a, &crc);
    return r == ID_YES && !crc ? ID_NO : r;     /* no CRC: the AHT10's */
}

static int aht10_identify(aos_io_i2c_t *b, uint8_t a)
{
    bool crc;
    return aht_probe(b, a, &crc) == ID_YES && !crc ? ID_YES : ID_NO;
}

static void aht_label(sens_slot_t *s, bool crc)
{
    sens_scpy(s->pub.chip, sizeof s->pub.chip, crc ? "AHT20" : "AHT10");
}

static int aht_init(sens_io_t *io, sens_slot_t *s)
{
    aht_t *p = SPRIV(s, aht_t);
    if (!p->set) {
        /* declared by its name, it is that; detected, the reads decide */
        char c[16] = "";
        sens_arg_str(s, "chip", c, sizeof c);
        bool named10 = !strncmp(s->pub.name, "aht10", 5) || !strcmp(c, "aht10");
        /* an "aht20" that is really an AHT10 still finds out by itself */
        p->crc = s->pub.declared && named10 ? 0 : -1;
        p->set = true;
    }
    uint8_t cmd = 0x71, st = 0;
    if (!si2c_wrrd(io->i2c, s->pub.addr, &cmd, 1, &st, 1)) return R_NACK;
    if (!(st & 0x08)) {
        /* not calibrated: the AHT20's init command, then the AHT10's */
        uint8_t w[3] = { (uint8_t)(p->crc == 0 ? 0xE1 : 0xBE), 0x08, 0x00 };
        if (!si2c_wr(io->i2c, s->pub.addr, w, 3)) return R_NACK;
        aos_hal_sleep_ms(10);
        if (p->crc < 0 && si2c_wrrd(io->i2c, s->pub.addr, &cmd, 1, &st, 1) && !(st & 0x08)) {
            w[0] = 0xE1;
            si2c_wr(io->i2c, s->pub.addr, w, 3);
            aos_hal_sleep_ms(10);
        }
    }
    return R_OK;
}

static int aht_read(sens_io_t *io, sens_slot_t *s)
{
    aht_t *p = SPRIV(s, aht_t);
    uint8_t w[3] = { 0xAC, 0x33, 0x00 }, d[7];
    if (!si2c_wr(io->i2c, s->pub.addr, w, 3)) return R_NACK;
    aos_hal_sleep_ms(80);
    for (int tries = 0;; tries++) {
        if (!si2c_rd(io->i2c, s->pub.addr, d, 7)) return R_NACK;
        if (!(d[0] & 0x80)) break;
        if (tries >= 2) return R_NOTREADY;
        aos_hal_sleep_ms(20);
    }
    bool ok = aht_crc_ok(d);
    if (p->crc < 0) {
        /* still finding out: two good CRCs in a row, or three bad */
        if (ok) { p->good++; p->bad = 0; }
        else { p->bad++; p->good = 0; }
        if (p->good >= 2) p->crc = 1;
        else if (p->bad >= 3) p->crc = 0;
        else return R_NOTREADY;
        aht_label(s, p->crc == 1);
    } else if (p->crc == 1 && !ok) {
        return R_CRC;
    }
    uint32_t rh = (uint32_t)d[1] << 12 | (uint32_t)d[2] << 4 | d[3] >> 4;
    uint32_t rt = (uint32_t)(d[3] & 0x0F) << 16 | (uint32_t)d[4] << 8 | d[5];
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)rt * 200.0f / 1048576.0f - 50.0f);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, (float)rh * 100.0f / 1048576.0f);
    return R_OK;
}

const sens_drv_t DRV_AHT20 = {
    .prio = SP_CMD,
    .cat = SC(TEMP),
    .pub = { .id = "aht20", .name = "AHT20", .addrs = { 0x38 }, .naddr = 1 },
    .identify = aht20_identify, .init = aht_init, .read = aht_read,
};

const sens_drv_t DRV_AHT10 = {
    .prio = SP_CMD,
    .cat = SC(TEMP),
    .pub = { .id = "aht10", .name = "AHT10", .addrs = { 0x38, 0x39 }, .naddr = 2 },
    .identify = aht10_identify, .init = aht_init, .read = aht_read,
};

/* ---- BH1750: continuous high resolution, 1 lx, 120 ms ---- */

static int bh_identify(aos_io_i2c_t *b, uint8_t a) { (void)b; (void)a; return ID_MAYBE; }   /* no id: a PCF8574 looks the same */

static int bh_init(sens_io_t *io, sens_slot_t *s)
{
    uint8_t on = 0x01, mode = 0x10;
    if (!si2c_wr(io->i2c, s->pub.addr, &on, 1) || !si2c_wr(io->i2c, s->pub.addr, &mode, 1)) return R_NACK;
    return R_OK;
}

static int bh_read(sens_io_t *io, sens_slot_t *s)
{
    if (sens_now() - s->init_ms < 180) return R_NOTREADY;
    uint8_t d[2];
    if (!si2c_rd(io->i2c, s->pub.addr, d, 2)) return R_NACK;
    sens_setv(s, 0, "lux", AOS_SQ_LUX, (float)sens_be16(d) / 1.2f);
    return R_OK;
}

const sens_drv_t DRV_BH1750 = {
    .prio = SP_LAST,
    .cat = SC(LIGHT),
    .pub = { .id = "bh1750", .name = "BH1750", .addrs = { 0x23, 0x5C }, .naddr = 2 },
    .identify = bh_identify, .init = bh_init, .read = bh_read,
};

/* ---- INA219 / INA226: the shunt's voltage over its resistance ----
 * No calibration register: the current is worked out here from the shunt
 * voltage, so the chip keeps its power-on configuration - which is also
 * what identifies an INA219 (0x399F). */

static int ina226_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t m, d;
    if (!si2c_rreg16be(b, a, 0xFE, &m) || m != 0x5449) return ID_NO;          /* "TI" */
    if (!si2c_rreg16be(b, a, 0xFF, &d)) return ID_NO;
    return (d & 0xFFF0) == 0x2260 ? ID_YES : ID_NO;                             /* not an HDC1080, INA260... */
}

static int ina219_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t v;
    if (si2c_rreg16be(b, a, 0x00, &v) && v == 0x399F) return ID_YES;           /* its power-on config */
    return ID_MAYBE;
}

static int ina226_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t id = 0;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0xFE, &id)) return R_NACK;
    if (id != 0x5449) { sens_err(s, "0x%04X", id); return R_ID; }
    /* averages of 16, 1.1 ms per conversion, shunt and bus continuous */
    if (!si2c_wreg16be(io->i2c, s->pub.addr, 0x00, 0x4527)) return R_NACK;
    return R_OK;
}

static int ina_read_common(sens_io_t *io, sens_slot_t *s, bool is219)
{
    uint16_t sh, bus;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x01, &sh) || !si2c_rreg16be(io->i2c, s->pub.addr, 0x02, &bus)) return R_NACK;
    float vsh, vbus;
    if (is219) {
        vsh = (float)(int16_t)sh * 10e-6f;
        vbus = (float)(bus >> 3) * 0.004f;
    } else {
        vsh = (float)(int16_t)sh * 2.5e-6f;
        vbus = (float)bus * 0.00125f;
    }
    float r = (float)(s->shunt_mohm > 0 ? s->shunt_mohm : 100) / 1000.0f;
    float i = vsh / r;
    sens_setv(s, 0, "bus", AOS_SQ_VOLT, vbus);
    sens_setv(s, 1, "current", AOS_SQ_CURR, i);
    sens_setv(s, 2, "power", AOS_SQ_POWER, vbus * i);
    sens_setv(s, 3, "shunt", AOS_SQ_VOLT, vsh);
    return R_OK;
}

static int ina219_read(sens_io_t *io, sens_slot_t *s) { return ina_read_common(io, s, true); }
static int ina226_read(sens_io_t *io, sens_slot_t *s) { return ina_read_common(io, s, false); }

const sens_drv_t DRV_INA226 = {
    .prio = SP_REG,
    .cat = SC(POWER),
    .pub = { .id = "ina226", .name = "INA226", .addrs = { 0x40, 0x41, 0x44, 0x45 }, .naddr = 4, .shunt = true },
    .probe = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 }, .nprobe = 8,
    .identify = ina226_identify, .init = ina226_init, .read = ina226_read,
};
const sens_drv_t DRV_INA219 = {
    .prio = SP_REG2,
    .cat = SC(POWER),
    .pub = { .id = "ina219", .name = "INA219", .addrs = { 0x40, 0x41, 0x44, 0x45 }, .naddr = 4, .shunt = true },
    .probe = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 }, .nprobe = 8,
    .identify = ina219_identify, .read = ina219_read,
};

/* ---- ADS1115: the four inputs against GND, one shot each at 128 SPS ---- */

static int ads_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t lo, hi;
    if (si2c_rreg16be(b, a, 0x02, &lo) && si2c_rreg16be(b, a, 0x03, &hi) && lo == 0x8000 && hi == 0x7FFF) return ID_YES;
    return ID_MAYBE;
}

static int ads_pga(int32_t mv, int32_t *fsr)
{
    static const int32_t FSR[6] = { 6144, 4096, 2048, 1024, 512, 256 };
    int k = 1;
    for (int i = 0; i < 6; i++) if (mv == FSR[i]) k = i;
    *fsr = FSR[k];
    return k;
}

static int ads_read(sens_io_t *io, sens_slot_t *s)
{
    int32_t fsr;
    int pga = ads_pga(s->gain_mv, &fsr);
    static const char *const KEY[4] = { "a0", "a1", "a2", "a3" };
    for (int ch = 0; ch < 4; ch++) {
        uint16_t cfg = (uint16_t)(0x8000 | (4 + ch) << 12 | pga << 9 | 0x0100 | 4 << 5 | 0x0003);
        if (!si2c_wreg16be(io->i2c, s->pub.addr, 0x01, cfg)) return R_NACK;
        aos_hal_sleep_ms(9);
        uint16_t st = 0;
        for (int t = 0; t < 4; t++) {
            if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x01, &st)) return R_NACK;
            if (st & 0x8000) break;                /* OS = 1: not converting any more */
            aos_hal_sleep_ms(2);
        }
        if (!(st & 0x8000)) return R_NOTREADY;
        uint16_t raw;
        if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x00, &raw)) return R_NACK;
        sens_setv(s, ch, KEY[ch], AOS_SQ_VOLT, (float)(int16_t)raw * (float)fsr / 32768.0f / 1000.0f);
    }
    return R_OK;
}

const sens_drv_t DRV_ADS1115 = {
    .prio = SP_REG,
    .cat = SC(POWER),
    .pub = { .id = "ads1115", .name = "ADS1115", .addrs = { 0x48, 0x49, 0x4A, 0x4B }, .naddr = 4, .gain = true },
    .identify = ads_identify, .read = ads_read,
};

/* ---- MAX6675: a K thermocouple, 0..1023.75 °C in quarters of a degree ----
 * 16 bits per read: D15 always 0, D14..D3 the temperature, D2 set when the
 * thermocouple is open, D1 always 0. Each read (CS low) stops a conversion
 * and CS high starts the next, 220 ms: a read a second is well apart. Mode 0,
 * at most 4.3 MHz; MISO's pull-up makes an absent chip 0xFFFF. */
static int max6675_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t rx[2];
    if (!aos_io_spi_xfer(io->spi, NULL, rx, 2, SENS_XFER_MS)) return R_NACK;
    uint16_t w = sens_be16(rx);
    if (w == 0xFFFF) {                  /* MISO's pull-up: nobody drove it */
        sens_scpy(s->pub.err, sizeof s->pub.err, "spi");
        return R_NACK;
    }
    if (w & 0x8002) {
        sens_err(s, "%04X", w);
        return R_ID;
    }
    if (w & 4) return R_OPEN;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)(w >> 3) * 0.25f);
    return R_OK;
}

const sens_drv_t DRV_MAX6675 = {
    .prio = SP_REG,
    .cat = SC(TEMP),
    .pub = { .id = "max6675", .name = "MAX6675", .spi = true, .bus = AOS_SENSOR_BUS_SPI },
    .flags = SF_NOPROBE, .spi_mode = 0, .spi_hz = 1000000,
    .read = max6675_read,
};

/* ---- DS18B20 and its family: the service reads the 1-Wire buses ---- */
const sens_drv_t DRV_DS18B20 = {
    .prio = SP_REG,
    .cat = SC(TEMP),
    .pub = { .id = "ds18b20", .name = "DS18B20", .onewire = true, .bus = AOS_SENSOR_BUS_ONEWIRE, .pins = 1 },
    .flags = SF_NOPROBE,
};

const sens_drv_t *const SENS_G_BASIC[] = {
    &DRV_BME280, &DRV_BMP280, &DRV_INA226, &DRV_INA219, &DRV_ADS1115,
    &DRV_SHT3X, &DRV_SHT4X, &DRV_AHT20, &DRV_AHT10, &DRV_SHT3X_CAND, &DRV_BH1750,
    &DRV_MAX6675, &DRV_DS18B20, NULL,
};

const sens_alias_t SENS_A_BASIC[] = {
    { "sht30", "sht3x" }, { "sht31", "sht3x" }, { "sht35", "sht3x" }, { "sht85", "sht3x" }, { "gxht30", "sht3x" },
    { "sht40", "sht4x" }, { "sht41", "sht4x" }, { "sht43", "sht4x" }, { "sht45", "sht4x" },
    { "aht21", "aht20" }, { "aht2x", "aht20" }, { "aht25", "aht20" }, { "aht30", "aht20" },
    { "dht20", "aht20" }, { "am2301b", "aht20" }, { "am2315c", "aht20" },
    { "gy302", "bh1750" }, { "gy30", "bh1750" },
    { "ds18s20", "ds18b20" }, { "ds1822", "ds18b20" }, { "max31850", "ds18b20" }, { "onewire", "ds18b20" },
    { NULL, NULL },
};
