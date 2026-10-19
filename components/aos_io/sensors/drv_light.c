/*
 * P4OS - Sensor drivers: light, UV and colour on I2C (sens_int.h, docs/SENSORS.md).
 *
 * VEML7700/VEML6030, VEML6075, TSL2561, TSL2591, TCS34725, APDS-9960,
 * LTR390-UV, OPT3001, Si1145/46/47, MAX44009 and LTR-329/LTR-303.
 *
 * Most of these chips integrate light continuously once they are powered,
 * so a read only collects the last finished integration and nothing ever
 * waits for a conversion. The ones with a narrow range (VEML7700, TSL2561,
 * TSL2591, TCS34725, LTR-329) range themselves: a table of settings, most
 * sensitive first, and after each read the most sensitive one where the
 * reading would fill less than half the ADC is chosen. A reading that fills
 * more than 80 % (or saturates) moves down; one that would fill less than
 * 40 % at the next more sensitive setting moves up. Between those two the
 * setting stays, so it does not hunt. After a change the chip needs two of
 * its integrations before a reading is whole again ("settle"): with a read
 * a second that is always over by the next round, except right after
 * init(), which returns R_NOTREADY once.
 *
 * Raw channels (full, ir, r, g, b, clear) of the self-ranging chips are
 * given normalised to one fixed setting, named at each driver, so that
 * their history does not jump when the gain changes.
 *
 * At 0x29 the DIST family's VL53L1X and VL6180X read a 16-bit register
 * index first (SP_REG); an 8-bit read before them could upset them, so the
 * chips here that live at 0x29 use SP_REG2. Among those, the exact IDs
 * (TSL2591, TCS34725, LTR-329) are tried before the TSL2561, whose ID is a
 * nibble: an LTR-329's data register sits where the TSL2561 keeps its ID.
 */
#include "sens_int.h"

/* ---- self-ranging ---- */

/* The setting to use next, given how full the ADC was (0..1) at setting
 * lvl; sens[] is each setting's sensitivity (gain x time), decreasing. */
static int lt_pick(int lvl, int n, const float *sens, float fill, bool sat)
{
    if (sat) return n - 1;
    bool down = fill > 0.8f;
    bool up = lvl > 0 && fill * sens[lvl - 1] / sens[lvl] < 0.4f;
    if (!down && !up) return lvl;
    for (int j = 0; j < n; j++)
        if (fill * sens[j] / sens[lvl] < 0.5f) return j;
    return n - 1;
}

static bool lt_settled(uint32_t t_cfg, uint32_t settle_ms) { return sens_now() - t_cfg >= settle_ms; }

/* ---- VEML7700 / VEML6030 (Vishay) ----
 * 16-bit little-endian registers by command code: 0x00 ALS_CONF (gain
 * 12:11, integration 9:6, ALS_SD bit 0), 0x04 ALS, 0x05 WHITE, 0x07 ID
 * (low byte 0x81, high byte 0xC4 at 0x10, 0xD4 at the VEML6030's 0x48).
 * Lux per count is 0.0042 at gain x2 and 800 ms (the current Vishay
 * datasheet, doc 84286; the 2016 one, Adafruit and ESPHome still say
 * 0.0036), proportional to 1/gain and 1/time. Above 1000 lx the response
 * bends, and Vishay's application note gives the polynomial that undoes it
 * (as ESPHome and Adafruit apply it). */

typedef struct { uint8_t lvl; uint32_t t_cfg; } veml_t;
SPRIV_CHECK(veml_t);

static const struct { uint8_t g, it; float gain; uint16_t ms; } VEML_LV[] = {
    { 0x01, 0x01, 2.0f, 200 },          /* x2, 200 ms: 0.0168 lx a count, up to 1100 lx */
    { 0x01, 0x00, 2.0f, 100 },          /* x2, 100 ms: up to 2200 lx */
    { 0x03, 0x00, 0.25f, 100 },         /* x1/4, 100 ms: up to 17600 lx */
    { 0x02, 0x0C, 0.125f, 25 },         /* x1/8, 25 ms: 2.15 lx a count, up to 140000 lx */
};
#define VEML_N ((int)(sizeof VEML_LV / sizeof VEML_LV[0]))
static const float VEML_SENS[VEML_N] = { 400.0f, 200.0f, 25.0f, 3.125f };

static uint32_t veml_settle(int lvl) { return 2u * VEML_LV[lvl].ms + 10u; }

static int veml7700_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t id;
    if (!si2c_rreg16le(b, a, 0x07, &id)) return ID_NO;
    return (id & 0xFF) == 0x81 && ((id >> 8) == 0xC4 || (id >> 8) == 0xD4) ? ID_YES : ID_NO;
}

/* Settings are changed with the ALS shut down, as the application note asks */
static bool veml_set(aos_io_i2c_t *b, uint8_t a, int lvl)
{
    uint16_t conf = (uint16_t)(VEML_LV[lvl].g << 11 | VEML_LV[lvl].it << 6);
    return si2c_wreg16le(b, a, 0x00, conf | 0x0001) && si2c_wreg16le(b, a, 0x00, conf);
}

static int veml7700_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t id;
    if (!si2c_rreg16le(io->i2c, s->pub.addr, 0x07, &id)) return R_NACK;
    if ((id & 0xFF) != 0x81) { sens_err(s, "0x%04X", id); return R_ID; }
    veml_t *p = SPRIV(s, veml_t);
    p->lvl = 2;
    if (!veml_set(io->i2c, s->pub.addr, p->lvl)) return R_NACK;
    p->t_cfg = sens_now();
    return R_OK;
}

static float veml_lux(uint16_t raw, int lvl)
{
    float res = 0.0042f * (800.0f / VEML_LV[lvl].ms) * (2.0f / VEML_LV[lvl].gain);
    float lx = raw * res;
    if (lx > 1000.0f) lx = (((6.0135e-13f * lx - 9.3924e-9f) * lx + 8.1488e-5f) * lx + 1.0023f) * lx;
    return lx;
}

static int veml7700_read(sens_io_t *io, sens_slot_t *s)
{
    veml_t *p = SPRIV(s, veml_t);
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    if (!lt_settled(p->t_cfg, veml_settle(p->lvl))) return R_NOTREADY;
    uint16_t als, white;
    int lvl;
    for (int pass = 0;; pass++) {
        if (!si2c_rreg16le(b, a, 0x04, &als) || !si2c_rreg16le(b, a, 0x05, &white)) return R_NACK;
        lvl = p->lvl;                   /* the setting this reading was taken at */
        bool sat = als == 0xFFFF;
        int nl = lt_pick(lvl, VEML_N, VEML_SENS, als / 65535.0f, sat);
        if (nl == lvl) break;
        if (!veml_set(b, a, nl)) return R_NACK;
        p->lvl = (uint8_t)nl;
        p->t_cfg = sens_now();
        if (!sat) break;
        /* saturated: the least sensitive setting integrates for 25 ms, so
         * wait for it here rather than lose the round */
        if (pass || veml_settle(nl) > 120) return R_NOTREADY;
        aos_hal_sleep_ms(veml_settle(nl));
    }
    sens_setv(s, 0, "lux", AOS_SQ_LUX, veml_lux(als, lvl));
    sens_setv(s, 1, "white", AOS_SQ_LUX, veml_lux(white, lvl));
    return R_OK;
}

/* ---- VEML6075 (Vishay): UVA and UVB ----
 * 16-bit little-endian registers: 0x00 UV_CONF (UV_IT 6:4, HD 3, SD 0),
 * 0x07 UVA, 0x09 UVB, 0x0A UVCOMP1 (visible), 0x0B UVCOMP2 (infrared),
 * 0x0C ID = 0x0026. The compensation and responsivities are Vishay's
 * ("Designing the VEML6075 into an Application", open air, no diffuser),
 * as Adafruit's driver uses them, for 100 ms. */

typedef struct { uint32_t t_cfg; } veml75_t;
SPRIV_CHECK(veml75_t);

static int veml6075_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t id;
    return si2c_rreg16le(b, a, 0x0C, &id) && id == 0x0026 ? ID_YES : ID_NO;
}

static int veml6075_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t id;
    if (!si2c_rreg16le(io->i2c, s->pub.addr, 0x0C, &id)) return R_NACK;
    if (id != 0x0026) { sens_err(s, "0x%04X", id); return R_ID; }
    /* 100 ms, normal dynamic, continuous, powered */
    if (!si2c_wreg16le(io->i2c, s->pub.addr, 0x00, 0x0010)) return R_NACK;
    SPRIV(s, veml75_t)->t_cfg = sens_now();
    return R_OK;
}

static int veml6075_read(sens_io_t *io, sens_slot_t *s)
{
    if (!lt_settled(SPRIV(s, veml75_t)->t_cfg, 210)) return R_NOTREADY;
    uint16_t uva, uvb, c1, c2;
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    if (!si2c_rreg16le(b, a, 0x07, &uva) || !si2c_rreg16le(b, a, 0x09, &uvb) ||
        !si2c_rreg16le(b, a, 0x0A, &c1) || !si2c_rreg16le(b, a, 0x0B, &c2)) return R_NACK;
    float ua = uva - 2.22f * c1 - 1.33f * c2;
    float ub = uvb - 2.95f * c1 - 1.74f * c2;
    if (ua < 0) ua = 0;
    if (ub < 0) ub = 0;
    float uvi = (ua * 0.001461f + ub * 0.002591f) / 2.0f;
    sens_setv(s, 0, "uvi", AOS_SQ_UVI, uvi);
    sens_setv(s, 1, "uva", AOS_SQ_RAW, ua);
    sens_setv(s, 2, "uvb", AOS_SQ_RAW, ub);
    return R_OK;
}

/* ---- TSL2561 (TAOS/ams): broadband and infrared ----
 * Every access starts with a command byte: CMD 0x80, WORD 0x20, register
 * in the low nibble. 0x0 CONTROL (0x03 powers it up), 0x1 TIMING (GAIN
 * bit 4 = 16x, INTEG 1:0 = 13.7/101/402 ms), 0xA ID (part number in the
 * high nibble: 0x5 TSL2561T/FN/CL, 0x1 TSL2561CS), 0xC/0xD channel 0
 * (visible + IR), 0xE/0xF channel 1 (IR). Lux is the datasheet's integer
 * algorithm (section "Calculating Lux", as Adafruit_TSL2561_U does), with
 * its T and CS package tables, kept in fractions of a lux. "full" and "ir"
 * are the channels scaled to 402 ms and 16x, the setting the lux table is
 * written for. */

typedef struct { uint8_t lvl; bool cs; uint32_t t_cfg; } tsl61_t;
SPRIV_CHECK(tsl61_t);

static const struct { uint8_t timing; uint16_t clip, settle; } TSL61_LV[] = {
    { 0x11, 37000, 215 },               /* 16x, 101 ms */
    { 0x01, 37000, 215 },               /* 1x, 101 ms */
    { 0x00, 4900, 40 },                 /* 1x, 13.7 ms */
};
#define TSL61_N 3
static const float TSL61_SENS[TSL61_N] = { 16.0f * 81, 81, 11 };

static int tsl2561_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    if (!si2c_rreg8(b, a, 0x8A, &id)) return ID_NO;
    return (id >> 4) == 0x5 || (id >> 4) == 0x1 ? ID_YES : ID_NO;
}

static int tsl2561_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id;
    if (!si2c_rreg8(b, a, 0x8A, &id)) return R_NACK;
    int part = id >> 4;                 /* declared: the TSL2560 parts (0, 4) read the same */
    if (part != 0 && part != 1 && part != 4 && part != 5) { sens_err(s, "0x%02X", id); return R_ID; }
    tsl61_t *p = SPRIV(s, tsl61_t);
    p->cs = part <= 1;
    p->lvl = 1;
    if (!si2c_wreg8(b, a, 0x80, 0x03) || !si2c_wreg8(b, a, 0x81, TSL61_LV[p->lvl].timing)) return R_NACK;
    p->t_cfg = sens_now();
    return R_OK;
}

/* Datasheet integer lux, in 1/16384 lx; ch0/ch1 as read at the timing */
static float tsl2561_lux(uint32_t ch0, uint32_t ch1, uint8_t timing, bool cs, float *n0, float *n1)
{
    /* K (ratio limit), B, M: T package, then CS */
    static const uint16_t KBM[2][8][3] = {
        { { 0x0040, 0x01F2, 0x01BE }, { 0x0080, 0x0214, 0x02D1 }, { 0x00C0, 0x023F, 0x037B },
          { 0x0100, 0x0270, 0x03FE }, { 0x0138, 0x016F, 0x01FC }, { 0x019A, 0x00D2, 0x00FB },
          { 0x029A, 0x0018, 0x0012 }, { 0x029A, 0x0000, 0x0000 } },
        { { 0x0043, 0x0204, 0x01AD }, { 0x0085, 0x0228, 0x02C1 }, { 0x00C8, 0x0253, 0x0363 },
          { 0x010A, 0x0282, 0x03DF }, { 0x014D, 0x0177, 0x01DD }, { 0x019A, 0x0101, 0x0127 },
          { 0x029A, 0x0037, 0x002B }, { 0x029A, 0x0000, 0x0000 } },
    };
    uint64_t sc;
    switch (timing & 0x03) {
    case 0: sc = 0x7517; break;         /* 322/11 in Q10 */
    case 1: sc = 0x0FE7; break;         /* 322/81 */
    default: sc = 1u << 10; break;
    }
    if (!(timing & 0x10)) sc <<= 4;     /* 1x: scale to 16x */
    uint64_t c0 = (ch0 * sc) >> 10, c1 = (ch1 * sc) >> 10;
    *n0 = (float)ch0 * (float)sc / 1024.0f;
    *n1 = (float)ch1 * (float)sc / 1024.0f;
    uint64_t r1 = c0 ? (c1 << 10) / c0 : 0;
    uint64_t ratio = (r1 + 1) >> 1;
    const uint16_t (*t)[3] = KBM[cs ? 1 : 0];
    int k = 0;
    while (k < 7 && ratio > t[k][0]) k++;
    int64_t v = (int64_t)(c0 * t[k][1]) - (int64_t)(c1 * t[k][2]);
    if (v < 0) v = 0;
    return (float)v / 16384.0f;
}

static int tsl2561_read(sens_io_t *io, sens_slot_t *s)
{
    tsl61_t *p = SPRIV(s, tsl61_t);
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    if (!lt_settled(p->t_cfg, TSL61_LV[p->lvl].settle)) return R_NOTREADY;
    uint16_t ch0, ch1;
    int lvl;
    for (int pass = 0;; pass++) {
        if (!si2c_rreg16le(b, a, 0xAC, &ch0) || !si2c_rreg16le(b, a, 0xAE, &ch1)) return R_NACK;
        lvl = p->lvl;
        uint16_t clip = TSL61_LV[lvl].clip;
        bool sat = ch0 > clip || ch1 > clip;
        int nl = lt_pick(lvl, TSL61_N, TSL61_SENS, (float)ch0 / clip, sat);
        if (nl == lvl) break;
        if (!si2c_wreg8(b, a, 0x81, TSL61_LV[nl].timing)) return R_NACK;
        p->lvl = (uint8_t)nl;
        p->t_cfg = sens_now();
        if (!sat) break;
        if (pass || TSL61_LV[nl].settle > 120) return R_NOTREADY;
        aos_hal_sleep_ms(TSL61_LV[nl].settle);
    }
    float n0, n1;
    float lx = tsl2561_lux(ch0, ch1, TSL61_LV[lvl].timing, p->cs, &n0, &n1);
    sens_setv(s, 0, "lux", AOS_SQ_LUX, lx);
    sens_setv(s, 1, "full", AOS_SQ_RAW, n0);
    sens_setv(s, 2, "ir", AOS_SQ_RAW, n1);
    return R_OK;
}

/* ---- TSL2591 (ams): high dynamic range ----
 * Command byte 0xA0 | register (CMD, normal transaction). 0x00 ENABLE
 * (PON 0x01, AEN 0x02), 0x01 CONTROL (AGAIN 5:4, ATIME 2:0 = 100..600 ms),
 * 0x12 ID = 0x50, 0x13 STATUS (AVALID bit 0), 0x14..0x17 channel 0 then
 * channel 1, little-endian; reading 0x14 latches all four bytes. Lux is
 * Adafruit's: counts per lux = time x gain / 408, lux = (C0 - C1)(1 -
 * C1/C0) / cpl, with the datasheet's typical gains 1/25/428/9876. Always
 * 100 ms here (37888 counts at most). "full" and "ir" are per 1x gain. */

typedef struct { uint8_t lvl; uint32_t t_cfg; } tsl91_t;
SPRIV_CHECK(tsl91_t);

static const uint8_t TSL91_AGAIN[4] = { 0x30, 0x20, 0x10, 0x00 };
static const float TSL91_SENS[4] = { 9876.0f, 428.0f, 25.0f, 1.0f };

static int tsl2591_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0xB2, &id) && id == 0x50 ? ID_YES : ID_NO;
}

static int tsl2591_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id;
    if (!si2c_rreg8(b, a, 0xB2, &id)) return R_NACK;
    if (id != 0x50) { sens_err(s, "0x%02X", id); return R_ID; }
    tsl91_t *p = SPRIV(s, tsl91_t);
    p->lvl = 2;
    if (!si2c_wreg8(b, a, 0xA1, TSL91_AGAIN[p->lvl]) || !si2c_wreg8(b, a, 0xA0, 0x03)) return R_NACK;
    p->t_cfg = sens_now();
    return R_OK;
}

static int tsl2591_read(sens_io_t *io, sens_slot_t *s)
{
    tsl91_t *p = SPRIV(s, tsl91_t);
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    if (!lt_settled(p->t_cfg, 220)) return R_NOTREADY;
    uint8_t st, d[4];
    if (!si2c_rreg8(b, a, 0xB3, &st)) return R_NACK;
    if (!(st & 0x01)) return R_NOTREADY;
    if (!si2c_rreg(b, a, 0xB4, d, 4)) return R_NACK;
    uint16_t ch0 = sens_le16(d), ch1 = sens_le16(d + 2);
    int lvl = p->lvl;
    bool sat = ch0 >= 37000 || ch1 >= 37000;
    int nl = lt_pick(lvl, 4, TSL91_SENS, ch0 / 37888.0f, sat);
    if (nl != lvl) {
        /* AEN off and on: the next integration starts with the new gain */
        if (!si2c_wreg8(b, a, 0xA0, 0x01) || !si2c_wreg8(b, a, 0xA1, TSL91_AGAIN[nl]) ||
            !si2c_wreg8(b, a, 0xA0, 0x03)) return R_NACK;
        p->lvl = (uint8_t)nl;
        p->t_cfg = sens_now();
        if (sat) return R_NOTREADY;
    }
    float g = TSL91_SENS[lvl], cpl = 100.0f * g / 408.0f;
    float lx = ch0 ? ((float)ch0 - ch1) * (1.0f - (float)ch1 / ch0) / cpl : 0.0f;
    if (lx < 0) lx = 0;
    sens_setv(s, 0, "lux", AOS_SQ_LUX, lx);
    sens_setv(s, 1, "full", AOS_SQ_RAW, ch0 / g);
    sens_setv(s, 2, "ir", AOS_SQ_RAW, ch1 / g);
    return R_OK;
}

/* ---- TCS34725 (ams): colour ----
 * Command byte 0x80 | register, 0xA0 to auto-increment. 0x00 ENABLE (PON,
 * AEN), 0x01 ATIME (256 - cycles of 2.4 ms), 0x0F CONTROL (AGAIN 1/4/16/
 * 60x), 0x12 ID (0x44 TCS34721/5, 0x4D TCS34723/7), 0x13 STATUS (AVALID),
 * 0x14..0x1B clear, red, green, blue, little-endian. Lux and colour
 * temperature by ams' design note DN40 (IR taken out of each channel,
 * G'' = 0.136 R' + G' - 0.444 B', counts per lux = time x gain / 310,
 * CCT = 3810 B'/R' + 1391), with its saturation rule (75 % under 150 ms
 * for ripple) as Adafruit_TCS34725 has it. cct is in kelvin, given as
 * AOS_SQ_RAW (there is no kelvin quantity); 0 when it cannot be worked
 * out. r g b and clear are per 1x gain and 100 ms. */

typedef struct { uint8_t lvl; uint32_t t_cfg; } tcs_t;
SPRIV_CHECK(tcs_t);

static const struct { uint8_t again, atime; float gain, ms; } TCS_LV[] = {
    { 3, 0xC0, 60.0f, 153.6f },
    { 2, 0xC0, 16.0f, 153.6f },
    { 1, 0xC0, 4.0f, 153.6f },
    { 0, 0xC0, 1.0f, 153.6f },
    { 0, 0xF6, 1.0f, 24.0f },
};
#define TCS_N ((int)(sizeof TCS_LV / sizeof TCS_LV[0]))
static const float TCS_SENS[TCS_N] = { 60 * 153.6f, 16 * 153.6f, 4 * 153.6f, 153.6f, 24.0f };

static uint32_t tcs_settle(int lvl) { return (uint32_t)(2.0f * TCS_LV[lvl].ms) + 10u; }

static float tcs_sat(int lvl)
{
    int cycles = 256 - TCS_LV[lvl].atime;
    float sat = cycles > 63 ? 65535.0f : 1024.0f * cycles;
    if (cycles <= 63) sat -= sat / 4;
    return sat;
}

static int tcs34725_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x92, &id) && (id == 0x44 || id == 0x4D) ? ID_YES : ID_NO;
}

static bool tcs_set(aos_io_i2c_t *b, uint8_t a, int lvl)
{
    return si2c_wreg8(b, a, 0x81, TCS_LV[lvl].atime) && si2c_wreg8(b, a, 0x8F, TCS_LV[lvl].again);
}

static int tcs34725_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id;
    if (!si2c_rreg8(b, a, 0x92, &id)) return R_NACK;
    if (id != 0x44 && id != 0x4D) { sens_err(s, "0x%02X", id); return R_ID; }
    tcs_t *p = SPRIV(s, tcs_t);
    p->lvl = 2;
    if (!tcs_set(b, a, p->lvl) || !si2c_wreg8(b, a, 0x80, 0x01)) return R_NACK;
    aos_hal_sleep_ms(3);                /* 2.4 ms from PON before AEN */
    if (!si2c_wreg8(b, a, 0x80, 0x03)) return R_NACK;
    p->t_cfg = sens_now();
    return R_OK;
}

static int tcs34725_read(sens_io_t *io, sens_slot_t *s)
{
    tcs_t *p = SPRIV(s, tcs_t);
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    if (!lt_settled(p->t_cfg, tcs_settle(p->lvl))) return R_NOTREADY;
    uint8_t st, d[8];
    int lvl;
    for (int pass = 0;; pass++) {
        if (!si2c_rreg8(b, a, 0x93, &st)) return R_NACK;
        if (!(st & 0x01)) return R_NOTREADY;
        if (!si2c_rreg(b, a, 0xB4, d, 8)) return R_NACK;
        lvl = p->lvl;
        float sat = tcs_sat(lvl);
        uint16_t c = sens_le16(d);
        bool over = c >= sat;
        int nl = lt_pick(lvl, TCS_N, TCS_SENS, c / sat, over);
        if (nl == lvl) break;
        if (!tcs_set(b, a, nl)) return R_NACK;
        p->lvl = (uint8_t)nl;
        p->t_cfg = sens_now();
        if (!over) break;
        if (pass || tcs_settle(nl) > 120) return R_NOTREADY;
        aos_hal_sleep_ms(tcs_settle(nl));
    }
    float c = sens_le16(d), r = sens_le16(d + 2), g = sens_le16(d + 4), bl = sens_le16(d + 6);
    float ir = r + g + bl > c ? (r + g + bl - c) / 2.0f : 0.0f;
    float r2 = r - ir, g2 = g - ir, b2 = bl - ir;
    float cpl = TCS_LV[lvl].ms * TCS_LV[lvl].gain / 310.0f;
    float lx = (0.136f * r2 + g2 - 0.444f * b2) / cpl;
    if (lx < 0) lx = 0;
    float cct = r2 > 0 ? 3810.0f * b2 / r2 + 1391.0f : 0.0f;
    float k = 100.0f / (TCS_LV[lvl].ms * TCS_LV[lvl].gain);
    sens_setv(s, 0, "lux", AOS_SQ_LUX, lx);
    sens_setv(s, 1, "cct", AOS_SQ_RAW, cct);
    sens_setv(s, 2, "r", AOS_SQ_RAW, r * k);
    sens_setv(s, 3, "g", AOS_SQ_RAW, g * k);
    sens_setv(s, 4, "b", AOS_SQ_RAW, bl * k);
    sens_setv(s, 5, "clear", AOS_SQ_RAW, c * k);
    return R_OK;
}

/* ---- APDS-9960 (Broadcom): proximity and colour; no gestures ----
 * 8-bit registers from 0x80: ENABLE (PON 0x01, AEN 0x02, PEN 0x04), ATIME
 * 0x81 (256 - cycles of 2.78 ms), PPULSE 0x8E, CONFIG1 0x8D, CONTROL 0x8F
 * (LDRIVE 7:6, PGAIN 3:2, AGAIN 1:0), CONFIG2 0x90, ID 0x92 (0xAB; 0x9C
 * and 0xA8 on some modules), STATUS 0x93 (AVALID 0x01, PVALID 0x02), clear,
 * red, green, blue at 0x94..0x9B (little-endian), PDATA 0x9C. SparkFun's
 * defaults: ATIME 219 (103 ms), AGAIN 4x, PGAIN 4x, LED 100 mA, 8 pulses
 * of 16 us. The chip has no lux formula; its counts are given as they are
 * (the setting never changes). prox is 0..255, larger nearer. */

static int apds9960_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x92, &id) && (id == 0xAB || id == 0x9C || id == 0xA8) ? ID_YES : ID_NO;
}

static int apds9960_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id;
    if (!si2c_rreg8(b, a, 0x92, &id)) return R_NACK;
    if (id != 0xAB && id != 0x9C && id != 0xA8) { sens_err(s, "0x%02X", id); return R_ID; }
    static const uint8_t SEQ[][2] = {
        { 0x80, 0x00 }, { 0x81, 0xDB }, { 0x8E, 0x87 }, { 0x8D, 0x60 },
        { 0x8F, 0x05 }, { 0x90, 0x01 }, { 0xAB, 0x00 }, { 0x80, 0x01 },
    };
    for (size_t i = 0; i < sizeof SEQ / sizeof SEQ[0]; i++)
        if (!si2c_wreg8(b, a, SEQ[i][0], SEQ[i][1])) return R_NACK;
    aos_hal_sleep_ms(7);                /* 5.7 ms from PON */
    if (!si2c_wreg8(b, a, 0x80, 0x07)) return R_NACK;
    return R_OK;
}

static int apds9960_read(sens_io_t *io, sens_slot_t *s)
{
    if (sens_now() - s->init_ms < 220) return R_NOTREADY;
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, st, d[9];
    if (!si2c_rreg8(b, a, 0x93, &st)) return R_NACK;
    if (!(st & 0x01)) return R_NOTREADY;
    if (!si2c_rreg(b, a, 0x94, d, 9)) return R_NACK;
    sens_setv(s, 0, "prox", AOS_SQ_RAW, d[8]);
    sens_setv(s, 1, "clear", AOS_SQ_RAW, sens_le16(d));
    sens_setv(s, 2, "r", AOS_SQ_RAW, sens_le16(d + 2));
    sens_setv(s, 3, "g", AOS_SQ_RAW, sens_le16(d + 4));
    sens_setv(s, 4, "b", AOS_SQ_RAW, sens_le16(d + 6));
    return R_OK;
}

/* ---- LTR390-UV (Lite-On) ----
 * 8-bit registers: 0x00 MAIN_CTRL (LS_EN 0x02, UVS_MODE 0x08, SW reset
 * 0x10), 0x04 MEAS_RATE (resolution 6:4, rate 2:0; 0x22 = 18 bits/100 ms,
 * every 100 ms), 0x05 GAIN (0x01 = 3x), 0x06 PART_ID = 0xB2, 0x07
 * MAIN_STATUS (new data 0x08), 0x0D ALS and 0x10 UVS, 20-bit little-
 * endian. It measures one thing at a time, so each read collects the mode
 * that ran during the last second and switches to the other; both values
 * are then a second or two old. Datasheet: lux = 0.6 ALS / (gain x int),
 * UVI = UVS / (2300 x gain/18 x int/4), int in 100 ms units, no window. */

typedef struct { bool uv, have_als, have_uv; uint32_t t_sw; float lux, uvi; } ltr390_t;
SPRIV_CHECK(ltr390_t);

static int ltr390_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x06, &id) && id == 0xB2 ? ID_YES : ID_NO;
}

static int ltr390_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id;
    if (!si2c_rreg8(b, a, 0x06, &id)) return R_NACK;
    if ((id >> 4) != 0xB) { sens_err(s, "0x%02X", id); return R_ID; }
    si2c_wreg8(b, a, 0x00, 0x10);       /* software reset: it may not acknowledge */
    aos_hal_sleep_ms(10);
    if (!si2c_wreg8(b, a, 0x04, 0x22) || !si2c_wreg8(b, a, 0x05, 0x01) || !si2c_wreg8(b, a, 0x00, 0x02)) return R_NACK;
    ltr390_t *p = SPRIV(s, ltr390_t);
    memset(p, 0, sizeof *p);
    p->t_sw = sens_now();
    return R_OK;
}

static int ltr390_read(sens_io_t *io, sens_slot_t *s)
{
    ltr390_t *p = SPRIV(s, ltr390_t);
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, st, d[3];
    if (!lt_settled(p->t_sw, 250)) return R_NOTREADY;
    if (!si2c_rreg8(b, a, 0x07, &st)) return R_NACK;
    if (st & 0x08) {
        if (!si2c_rreg(b, a, p->uv ? 0x10 : 0x0D, d, 3)) return R_NACK;
        uint32_t raw = sens_le24(d) & 0xFFFFF;
        if (p->uv) { p->uvi = raw / (2300.0f * 3.0f / 18.0f * 1.0f / 4.0f); p->have_uv = true; }
        else { p->lux = 0.6f * raw / (3.0f * 1.0f); p->have_als = true; }
        p->uv = !p->uv;
        if (!si2c_wreg8(b, a, 0x00, p->uv ? 0x0A : 0x02)) return R_NACK;
        p->t_sw = sens_now();
    }
    if (!p->have_als || !p->have_uv) return R_NOTREADY;
    sens_setv(s, 0, "uvi", AOS_SQ_UVI, p->uvi);
    sens_setv(s, 1, "lux", AOS_SQ_LUX, p->lux);
    return R_OK;
}

/* ---- OPT3001 (TI) ----
 * 16-bit big-endian registers: 0x00 result (exponent 15:12, mantissa
 * 11:0, lux = 0.01 x 2^E x M), 0x01 configuration, 0x7E manufacturer
 * 0x5449 ("TI"), 0x7F device 0x3001. 0xCE00: automatic full-scale, 800 ms,
 * continuous. It must be asked before the SHT3x's command probe at 0x44/
 * 0x45, which would write into its registers. */

static int opt3001_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t m, d;
    if (!si2c_rreg16be(b, a, 0x7E, &m) || m != 0x5449) return ID_NO;
    return si2c_rreg16be(b, a, 0x7F, &d) && d == 0x3001 ? ID_YES : ID_NO;
}

static int opt3001_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t d;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x7F, &d)) return R_NACK;
    if (d != 0x3001) { sens_err(s, "0x%04X", d); return R_ID; }
    return si2c_wreg16be(io->i2c, s->pub.addr, 0x01, 0xCE00) ? R_OK : R_NACK;
}

static int opt3001_read(sens_io_t *io, sens_slot_t *s)
{
    if (sens_now() - s->init_ms < 900) return R_NOTREADY;
    uint16_t cfg, res;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x01, &cfg) || !si2c_rreg16be(io->i2c, s->pub.addr, 0x00, &res)) return R_NACK;
    if (cfg & 0x0100) return R_RANGE;  /* OVF */
    if (!(cfg & 0x0600)) { s->inited = false; return R_NOTREADY; }   /* shut down behind our back */
    sens_setv(s, 0, "lux", AOS_SQ_LUX, 0.01f * (float)(1u << (res >> 12)) * (float)(res & 0x0FFF));
    return R_OK;
}

/* ---- Si1145/46/47 (Silicon Labs): UV index, visible and infrared ----
 * Registers: 0x00 PART_ID (0x45/46/47), 0x02 SEQ_ID (0x08), 0x07 HW_KEY
 * (0x17 or it does not run), 0x08/0x09 MEAS_RATE, 0x13..0x16 UCOEF,
 * 0x17 PARAM_WR, 0x18 COMMAND, 0x20 RESPONSE, 0x22 visible, 0x24 IR,
 * 0x2C UV index x 100, 0x2E PARAM_RD. Parameters are set through
 * PARAM_WR and a PARAM_SET command; CHLIST (0x01) = UV, IR and visible.
 * The UV coefficients and the autonomous mode every 8 ms are the
 * datasheet's and Adafruit_SI1145's. Visible and IR are the ADC's counts,
 * with its offset of about 256 in the dark. */

static int si1145_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id, seq;
    if (!si2c_rreg8(b, a, 0x00, &id) || id < 0x45 || id > 0x47) return ID_NO;
    return si2c_rreg8(b, a, 0x02, &seq) && seq == 0x08 ? ID_YES : ID_NO;
}

/* A command, the datasheet's way: NOP clears RESPONSE, then the command
 * moves it */
static bool si1145_cmd(aos_io_i2c_t *b, uint8_t a, uint8_t cmd)
{
    uint8_t r = 0xFF;
    if (!si2c_wreg8(b, a, 0x18, 0x00)) return false;
    if (!si2c_rreg8(b, a, 0x20, &r) || r != 0) return false;
    if (!si2c_wreg8(b, a, 0x18, cmd)) return false;
    for (int i = 0; i < 10; i++) {
        if (!si2c_rreg8(b, a, 0x20, &r)) return false;
        if (r) return !(r & 0x80);
        aos_hal_sleep_ms(1);
    }
    return false;
}

static bool si1145_param(aos_io_i2c_t *b, uint8_t a, uint8_t p, uint8_t v)
{
    uint8_t rd = 0;
    return si2c_wreg8(b, a, 0x17, v) && si1145_cmd(b, a, (uint8_t)(0xA0 | p)) &&
           si2c_rreg8(b, a, 0x2E, &rd) && rd == v;
}

static int si1145_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id;
    if (!si2c_rreg8(b, a, 0x00, &id)) return R_NACK;
    if (id < 0x45 || id > 0x47) { sens_err(s, "0x%02X", id); return R_ID; }
    static const uint8_t QUIET[] = { 0x08, 0x00, 0x00 };          /* MEAS_RATE 0: no autonomous runs */
    if (!si2c_wr(b, a, QUIET, sizeof QUIET) || !si2c_wreg8(b, a, 0x04, 0x00) ||
        !si2c_wreg8(b, a, 0x03, 0x00) || !si2c_wreg8(b, a, 0x21, 0xFF)) return R_NACK;
    si2c_wreg8(b, a, 0x18, 0x01);                                  /* RESET */
    aos_hal_sleep_ms(10);
    if (!si2c_wreg8(b, a, 0x07, 0x17)) return R_NACK;
    aos_hal_sleep_ms(10);
    static const uint8_t UCOEF[] = { 0x13, 0x29, 0x89, 0x02, 0x00 };
    if (!si2c_wr(b, a, UCOEF, sizeof UCOEF)) return R_NACK;
    if (!si1145_param(b, a, 0x01, 0xB0)) return R_NACK;           /* CHLIST: UV, ALS IR, ALS visible */
    if (!si2c_wreg8(b, a, 0x08, 0xFF)) return R_NACK;             /* every 255 x 31.25 us */
    if (!si1145_cmd(b, a, 0x0E)) return R_NACK;                   /* ALS_AUTO */
    return R_OK;
}

static int si1145_read(sens_io_t *io, sens_slot_t *s)
{
    if (sens_now() - s->init_ms < 30) return R_NOTREADY;
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, d[4], u[2], resp;
    if (!si2c_rreg8(b, a, 0x20, &resp)) return R_NACK;
    if (resp & 0x80) {                  /* an ADC overflowed: clear it, try next round */
        si2c_wreg8(b, a, 0x18, 0x00);
        return R_RANGE;
    }
    if (!si2c_rreg(b, a, 0x22, d, 4) || !si2c_rreg(b, a, 0x2C, u, 2)) return R_NACK;
    sens_setv(s, 0, "uvi", AOS_SQ_UVI, sens_le16(u) / 100.0f);
    sens_setv(s, 1, "visible", AOS_SQ_RAW, sens_le16(d));
    sens_setv(s, 2, "ir", AOS_SQ_RAW, sens_le16(d + 2));
    return R_OK;
}

/* ---- MAX44009 (Maxim) ----
 * No ID. Left as it powers on (configuration 0x02 = 0x03: a conversion
 * every 800 ms, automatic range), it is told apart by its power-on
 * registers: 0x02 = 0x03, upper threshold 0x05 = 0xFF, lower 0x06 = 0x00,
 * timer 0x07 = 0xFF. Lux in 0x03 (exponent 7:4, mantissa high 3:0) and
 * 0x04 (mantissa low 3:0): 2^E x M x 0.045. The datasheet wants both
 * bytes read with a repeated start between them; without a combined
 * transfer here, the high byte is read again after the low one and the
 * pair is taken only when it did not change. Nothing is ever written. */

static int max44009_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t c, hi, lo, t;
    if (!si2c_rreg8(b, a, 0x02, &c) || c != 0x03) return ID_NO;
    if (!si2c_rreg8(b, a, 0x05, &hi) || hi != 0xFF) return ID_NO;
    if (!si2c_rreg8(b, a, 0x06, &lo) || lo != 0x00) return ID_NO;
    return si2c_rreg8(b, a, 0x07, &t) && t == 0xFF ? ID_YES : ID_NO;
}

static int max44009_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, hi = 0, lo = 0, hi2 = 0;
    for (int i = 0;; i++) {
        if (!si2c_rreg8(b, a, 0x03, &hi) || !si2c_rreg8(b, a, 0x04, &lo) || !si2c_rreg8(b, a, 0x03, &hi2)) return R_NACK;
        if (hi == hi2) break;
        if (i >= 2) return R_NOTREADY;
    }
    int e = hi >> 4;
    if (e == 0x0F) return R_RANGE;
    int m = (hi & 0x0F) << 4 | (lo & 0x0F);
    sens_setv(s, 0, "lux", AOS_SQ_LUX, (float)(1u << e) * (float)m * 0.045f);
    return R_OK;
}

/* ---- LTR-329ALS / LTR-303ALS (Lite-On) ----
 * 8-bit registers: 0x80 ALS_CONTR (active 0x01, reset 0x02, gain 4:2 =
 * 1/2/4/8/-/-/48/96x), 0x85 MEAS_RATE (integration 5:3, repeat 2:0; 0x01
 * = 100 ms every 100 ms), 0x86 PART_ID 0xA0, 0x87 MANUFAC_ID 0x05, 0x88
 * channel 1 (IR) then 0x8A channel 0 (visible + IR), little-endian, read
 * together from 0x88; 0x8C status (0x80 data invalid). Lux by the
 * datasheet's appendix, by the ratio CH1/(CH0+CH1). "full" (channel 0)
 * and "ir" (channel 1) are per 1x gain at 100 ms. */

typedef struct { uint8_t lvl; uint32_t t_cfg; } ltr329_t;
SPRIV_CHECK(ltr329_t);

static const uint8_t LTR329_GCODE[3] = { 6, 3, 0 };
static const float LTR329_SENS[3] = { 48.0f, 8.0f, 1.0f };

static int ltr329_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t part, man;
    if (!si2c_rreg8(b, a, 0x86, &part) || part != 0xA0) return ID_NO;
    return si2c_rreg8(b, a, 0x87, &man) && man == 0x05 ? ID_YES : ID_NO;
}

static int ltr329_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, part;
    if (!si2c_rreg8(b, a, 0x86, &part)) return R_NACK;
    if (part != 0xA0) { sens_err(s, "0x%02X", part); return R_ID; }
    ltr329_t *p = SPRIV(s, ltr329_t);
    p->lvl = 1;
    si2c_wreg8(b, a, 0x80, 0x02);       /* software reset */
    aos_hal_sleep_ms(10);
    if (!si2c_wreg8(b, a, 0x85, 0x01) || !si2c_wreg8(b, a, 0x80, (uint8_t)(LTR329_GCODE[p->lvl] << 2 | 0x01))) return R_NACK;
    p->t_cfg = sens_now();
    return R_OK;
}

static int ltr329_read(sens_io_t *io, sens_slot_t *s)
{
    ltr329_t *p = SPRIV(s, ltr329_t);
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, st, d[4];
    if (!lt_settled(p->t_cfg, 230)) return R_NOTREADY;
    if (!si2c_rreg8(b, a, 0x8C, &st) || !si2c_rreg(b, a, 0x88, d, 4)) return R_NACK;
    float ch1 = sens_le16(d), ch0 = sens_le16(d + 2);
    int lvl = p->lvl;
    bool sat = (st & 0x80) || ch0 >= 65535 || ch1 >= 65535;
    int nl = lt_pick(lvl, 3, LTR329_SENS, (ch0 > ch1 ? ch0 : ch1) / 65535.0f, sat);
    if (nl != lvl) {
        if (!si2c_wreg8(b, a, 0x80, (uint8_t)(LTR329_GCODE[nl] << 2 | 0x01))) return R_NACK;
        p->lvl = (uint8_t)nl;
        p->t_cfg = sens_now();
        if (sat) return R_NOTREADY;
    }
    float g = LTR329_SENS[lvl], lx = 0;
    if (ch0 + ch1 > 0) {
        float r = ch1 / (ch0 + ch1);
        if (r < 0.45f) lx = 1.7743f * ch0 + 1.1059f * ch1;
        else if (r < 0.64f) lx = 4.2785f * ch0 - 1.9548f * ch1;
        else if (r < 0.85f) lx = 0.5926f * ch0 + 0.1185f * ch1;
        lx /= g;                        /* integration 100 ms: ALS_INT = 1 */
    }
    if (lx < 0) lx = 0;
    sens_setv(s, 0, "lux", AOS_SQ_LUX, lx);
    sens_setv(s, 1, "full", AOS_SQ_RAW, ch0 / g);
    sens_setv(s, 2, "ir", AOS_SQ_RAW, ch1 / g);
    return R_OK;
}

/* ---- the table ---- */

static const sens_drv_t DRV_VEML7700 = {
    .prio = SP_REG,
    .pub = { .id = "veml7700", .name = "VEML7700", .addrs = { 0x10, 0x48 }, .naddr = 2 },
    .identify = veml7700_identify, .init = veml7700_init, .read = veml7700_read,
};
static const sens_drv_t DRV_VEML6075 = {
    .prio = SP_REG,
    .pub = { .id = "veml6075", .name = "VEML6075", .addrs = { 0x10 }, .naddr = 1 },
    .identify = veml6075_identify, .init = veml6075_init, .read = veml6075_read,
};
static const sens_drv_t DRV_APDS9960 = {
    .prio = SP_REG,
    .pub = { .id = "apds9960", .name = "APDS-9960", .addrs = { 0x39 }, .naddr = 1 },
    .identify = apds9960_identify, .init = apds9960_init, .read = apds9960_read,
};
static const sens_drv_t DRV_LTR390 = {
    .prio = SP_REG,
    .pub = { .id = "ltr390", .name = "LTR390", .addrs = { 0x53 }, .naddr = 1 },
    .identify = ltr390_identify, .init = ltr390_init, .read = ltr390_read,
};
static const sens_drv_t DRV_OPT3001 = {
    .prio = SP_REG,
    .pub = { .id = "opt3001", .name = "OPT3001", .addrs = { 0x44, 0x45, 0x46, 0x47 }, .naddr = 4 },
    .identify = opt3001_identify, .init = opt3001_init, .read = opt3001_read,
};
static const sens_drv_t DRV_SI1145 = {
    .prio = SP_REG,
    .pub = { .id = "si1145", .name = "Si1145", .addrs = { 0x60 }, .naddr = 1 },
    .identify = si1145_identify, .init = si1145_init, .read = si1145_read,
};
/* at 0x29, after the 16-bit-index probes; exact IDs before the TSL2561's nibble */
static const sens_drv_t DRV_TSL2591 = {
    .prio = SP_REG2,
    .pub = { .id = "tsl2591", .name = "TSL2591", .addrs = { 0x29 }, .naddr = 1 },
    .identify = tsl2591_identify, .init = tsl2591_init, .read = tsl2591_read,
};
static const sens_drv_t DRV_TCS34725 = {
    .prio = SP_REG2,
    .pub = { .id = "tcs34725", .name = "TCS34725", .addrs = { 0x29 }, .naddr = 1 },
    .identify = tcs34725_identify, .init = tcs34725_init, .read = tcs34725_read,
};
static const sens_drv_t DRV_LTR329 = {
    .prio = SP_REG2,
    .pub = { .id = "ltr329", .name = "LTR-329", .addrs = { 0x29 }, .naddr = 1 },
    .identify = ltr329_identify, .init = ltr329_init, .read = ltr329_read,
};
static const sens_drv_t DRV_TSL2561 = {
    .prio = SP_REG2,
    .pub = { .id = "tsl2561", .name = "TSL2561", .addrs = { 0x39, 0x29, 0x49 }, .naddr = 3 },
    .identify = tsl2561_identify, .init = tsl2561_init, .read = tsl2561_read,
};
/* no ID: its power-on registers, after everything else at 0x4A/0x4B */
static const sens_drv_t DRV_MAX44009 = {
    .prio = SP_LAST,
    .pub = { .id = "max44009", .name = "MAX44009", .addrs = { 0x4A, 0x4B }, .naddr = 2 },
    .identify = max44009_identify, .read = max44009_read,
};

const sens_drv_t *const SENS_G_LIGHT[] = {
    &DRV_VEML7700, &DRV_VEML6075, &DRV_APDS9960, &DRV_LTR390, &DRV_OPT3001, &DRV_SI1145,
    &DRV_TSL2591, &DRV_TCS34725, &DRV_LTR329, &DRV_TSL2561, &DRV_MAX44009,
    NULL,
};

const sens_alias_t SENS_A_LIGHT[] = {
    { "veml6030", "veml7700" }, { "gy2561", "tsl2561" }, { "tsl2560", "tsl2561" },
    { "tcs34727", "tcs34725" }, { "tcs3472", "tcs34725" }, { "gy9960", "apds9960" },
    { "ltr390uv", "ltr390" }, { "si1146", "si1145" }, { "si1147", "si1145" }, { "gy1145", "si1145" },
    { "gy49", "max44009" }, { "ltr303", "ltr329" },
    { NULL, NULL },
};
