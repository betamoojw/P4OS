/*
 * P4OS - Sensor drivers: the rest on I2C: touch, soil, heart rate, angle, RTC temperature, inputs (sens_int.h, docs/SENSORS.md).
 */
#include "sens_int.h"

/* ---- MAX30102 / MAX30105 / MAX30101, and the older MAX30100 ----
 * The red and infrared light a finger reflects, from the chip's FIFO, as
 * the average of the samples since the last read (raw ADC counts), the
 * die's temperature, and, on the 30102 family, the pulse.
 *
 * MAX30102 (datasheet 19-7740): SpO2 mode (red and IR), 100 samples a
 * second averaged by 4: 25 a second into a 32-deep FIFO, 1.28 s, so a
 * read a second loses nothing. The pulse is found in the IR samples
 * across reads: the slow level taken away, a peak at least half the recent
 * swing and 0.3 s after the last one is a beat; the average of the last
 * intervals (4 to 8 of them) gives the beats a minute. Without a finger
 * (IR below 50000 counts) there is no pulse. hr is the fourth value and
 * only there while a pulse is known, so the others keep their place.
 *
 * MAX30100 (19-7065): another register map and a 16-deep FIFO of 16-bit
 * samples that cannot average: at 100 a second a read a second sees the
 * last 0.16 s only, enough for the light, not for a pulse.
 *
 * Identified by the part id (0xFF: 0x15 for the 30102/30105/30101, 0x11
 * for the 30100), read twice, with the FIFO pointers' and the interrupt
 * status' reserved bits at 0: 0x57 is also an RCWL-9620, whose three bytes
 * of distance could start with 0x15, and a DS3231 board's EEPROM. */

typedef struct {
    bool     old;                   /* a MAX30100 */
    float    die;                   /* the last die temperature */
    /* the pulse (30102 family) */
    float    dc, lp, lp1, lp2, env;
    float    since;                 /* samples since the last beat; < 0 none yet */
    float    iv[8];                 /* the last intervals, in samples */
    uint8_t  niv, iv_pos;
    bool     primed;
} mx_t;
SPRIV_CHECK(mx_t);

#define MX_RATE     25.0f           /* samples a second into the FIFO */
#define MX_FINGER   50000           /* IR counts with a finger on it */

static int mx_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id, id2, ptr[3], st;
    if (!si2c_rreg8(b, a, 0xFF, &id)) return ID_NO;
    if (id == 0x15) {
        if (!si2c_rreg(b, a, 0x04, ptr, 3) || !si2c_rreg8(b, a, 0x01, &st)) return ID_NO;
        if ((ptr[0] | ptr[1] | ptr[2]) & 0xE0 || st & 0xFD) return ID_NO;      /* 5-bit pointers; DIE_TEMP_RDY only */
    } else if (id == 0x11) {
        if (!si2c_rreg(b, a, 0x02, ptr, 3) || !si2c_rreg8(b, a, 0x01, &st)) return ID_NO;
        if ((ptr[0] | ptr[1] | ptr[2]) & 0xF0 || st & 0x0F) return ID_NO;      /* 4-bit pointers; reserved enables */
    } else return ID_NO;
    return si2c_rreg8(b, a, 0xFF, &id2) && id2 == id ? ID_YES : ID_NO;
}

static void mx_reset_pulse(mx_t *p)
{
    p->primed = false;
    p->since = -1;
    p->niv = p->iv_pos = 0;
    p->env = 0;
}

static int mx_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id, v = 0;
    mx_t *p = SPRIV(s, mx_t);
    memset(p, 0, sizeof *p);
    mx_reset_pulse(p);
    if (!si2c_rreg8(b, a, 0xFF, &id)) return R_NACK;
    if (id != 0x15 && id != 0x11) { sens_err(s, "0x%02X", id); return R_ID; }
    p->old = id == 0x11;
    uint8_t mode = p->old ? 0x06 : 0x09;
    if (!si2c_wreg8(b, a, mode, 0x40)) return R_NACK;              /* reset */
    for (int t = 0; t < 20; t++) {
        aos_hal_sleep_ms(2);
        if (si2c_rreg8(b, a, mode, &v) && !(v & 0x40)) break;
    }
    if (v & 0x40) return R_NOTREADY;
    if (p->old) {
        si2c_wreg8(b, a, 0x07, 0x47);               /* high resolution, 100 sps, 1600 us: 16 bits */
        si2c_wreg8(b, a, 0x09, 0x8F);               /* red 27.1 mA, IR 50 mA */
        si2c_wreg8(b, a, 0x02, 0); si2c_wreg8(b, a, 0x03, 0); si2c_wreg8(b, a, 0x04, 0);
        si2c_wreg8(b, a, 0x06, 0x03 | 0x08);        /* SpO2 mode, and a temperature */
    } else {
        si2c_wreg8(b, a, 0x08, 0x50);               /* average 4, FIFO rolls over */
        si2c_wreg8(b, a, 0x0A, 0x27);               /* 4096 nA, 100 sps, 411 us: 18 bits */
        si2c_wreg8(b, a, 0x0C, 0x24);               /* red about 7 mA */
        si2c_wreg8(b, a, 0x0D, 0x24);               /* IR about 7 mA */
        si2c_wreg8(b, a, 0x04, 0); si2c_wreg8(b, a, 0x05, 0); si2c_wreg8(b, a, 0x06, 0);
        si2c_wreg8(b, a, 0x09, 0x03);               /* SpO2 mode: red and IR */
        si2c_wreg8(b, a, 0x21, 0x01);               /* a temperature */
    }
    /* the die temperature for the first read (29 ms) */
    aos_hal_sleep_ms(35);
    uint8_t t[2];
    if (!si2c_rreg(b, a, p->old ? 0x16 : 0x1F, t, 2)) return R_NACK;
    p->die = (float)(int8_t)t[0] + (float)(t[1] & 0x0F) * 0.0625f;
    return R_OK;
}

/* One IR sample through the beat detector */
static void mx_pulse(mx_t *p, float x)
{
    if (x < MX_FINGER) { mx_reset_pulse(p); return; }
    if (!p->primed) {
        p->dc = x;
        p->lp = p->lp1 = p->lp2 = 0;
        p->primed = true;
        return;
    }
    p->dc += (x - p->dc) * 0.05f;                   /* the slow level, about 0.8 s */
    float ac = p->dc - x;                           /* blood absorbs: a beat is a dip in IR */
    p->lp2 = p->lp1;
    p->lp1 = p->lp;
    p->lp = 0.6f * p->lp + 0.4f * ac;
    p->env *= 0.97f;
    if (p->lp > p->env) p->env = p->lp;
    if (p->since >= 0) p->since += 1;
    /* a peak at the previous sample */
    if (p->lp1 > p->lp && p->lp1 >= p->lp2 && p->lp1 > 0.5f * p->env && p->env > 0) {
        float den = p->lp2 - 2 * p->lp1 + p->lp;
        float off = den != 0 ? 0.5f * (p->lp2 - p->lp) / den : 0;    /* parabola through the three */
        if (off < -0.5f) off = -0.5f;
        if (off > 0.5f) off = 0.5f;
        if (p->since < 0) p->since = 1 - off;                      /* the first: its time only */
        else if (p->since - 1 + off >= 0.3f * MX_RATE) {
            float iv = p->since - 1 + off;
            if (iv <= 2.0f * MX_RATE) {                            /* 30..200 a minute */
                p->iv[p->iv_pos] = iv;
                p->iv_pos = (uint8_t)((p->iv_pos + 1) % 8);
                if (p->niv < 8) p->niv++;
            }
            p->since = 1 - off;
        }
    }
    if (p->since > 2.5f * MX_RATE) { p->niv = 0; p->iv_pos = 0; p->since = -1; }   /* lost it */
}

static int mx_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    mx_t *p = SPRIV(s, mx_t);
    uint8_t ptr[3];
    if (!si2c_rreg(b, a, p->old ? 0x02 : 0x04, ptr, 3)) return R_NACK;
    int depth = p->old ? 16 : 32, bps = p->old ? 4 : 6;
    int n = (ptr[0] - ptr[2]) & (depth - 1);
    if (ptr[1]) {                                   /* it overflowed: a gap in the samples */
        n = depth;
        mx_reset_pulse(p);
    }
    if (n == 0) return R_NOTREADY;
    double sr = 0, si = 0;
    uint8_t d[96];
    for (int done = 0; done < n; ) {
        int k = n - done;
        if (k > (int)sizeof d / bps) k = (int)sizeof d / bps;
        if (!si2c_rreg(b, a, p->old ? 0x05 : 0x07, d, (size_t)(k * bps))) return R_NACK;
        for (int i = 0; i < k; i++) {
            const uint8_t *q = d + i * bps;
            uint32_t red, ir;
            if (p->old) { ir = sens_be16(q); red = sens_be16(q + 2); }
            else { red = sens_be24(q) & 0x3FFFF; ir = sens_be24(q + 3) & 0x3FFFF; }
            sr += red;
            si += ir;
            if (!p->old) mx_pulse(p, (float)ir);
        }
        done += k;
    }
    /* the die temperature: collect the last one, start the next */
    uint8_t c;
    if (p->old) {
        if (si2c_rreg8(b, a, 0x06, &c) && !(c & 0x08)) {
            uint8_t t[2];
            if (si2c_rreg(b, a, 0x16, t, 2)) p->die = (float)(int8_t)t[0] + (float)(t[1] & 0x0F) * 0.0625f;
            si2c_wreg8(b, a, 0x06, (uint8_t)(c | 0x08));
        }
    } else if (si2c_rreg8(b, a, 0x21, &c) && !(c & 0x01)) {
        uint8_t t[2];
        if (si2c_rreg(b, a, 0x1F, t, 2)) p->die = (float)(int8_t)t[0] + (float)(t[1] & 0x0F) * 0.0625f;
        si2c_wreg8(b, a, 0x21, 0x01);
    }
    sens_setv(s, 0, "red", AOS_SQ_RAW, (float)(sr / n));
    sens_setv(s, 1, "ir", AOS_SQ_RAW, (float)(si / n));
    sens_setv(s, 2, "die", AOS_SQ_TEMP, p->die);
    if (!p->old && p->niv >= 4) {
        float sum = 0;
        for (int i = 0; i < p->niv; i++) sum += p->iv[i];
        sens_setv(s, 3, "hr", AOS_SQ_BPM, 60.0f * MX_RATE * (float)p->niv / sum);
    }
    return R_OK;
}

const sens_drv_t DRV_MAX3010X = {
    .prio = SP_REG,
    .pub = { .id = "max3010x", .name = "MAX3010x", .addrs = { 0x57 }, .naddr = 1 },
    .identify = mx_identify, .init = mx_init, .read = mx_read,
};

/* ---- MPR121: 12 capacitive keys (NXP datasheet rev. 4, Adafruit_MPR121) ----
 * Identified without writing: after power-on its AFE configuration
 * registers hold 0x5C = 0x10 and 0x5D = 0x24, and this driver leaves them
 * so (Adafruit's sets 0x5D to 0x20), so a rescan finds it again. init() is
 * Adafruit's: soft reset, thresholds 12/6, the filters, all 12 electrodes
 * with baseline tracking. The value is the touched keys as a bitmap. */

static int mpr_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t r[2];
    if (!si2c_rreg(b, a, 0x5C, r, 2)) return ID_NO;
    return r[0] == 0x10 && r[1] == 0x24 ? ID_YES : ID_NO;
}

static int mpr_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, c;
    if (!si2c_wreg8(b, a, 0x80, 0x63)) return R_NACK;               /* soft reset */
    aos_hal_sleep_ms(1);
    si2c_wreg8(b, a, 0x5E, 0x00);                                    /* stop mode: the rest can be written */
    if (!si2c_rreg8(b, a, 0x5D, &c)) return R_NACK;
    if (c != 0x24) { sens_err(s, "0x%02X", c); return R_ID; }
    for (int i = 0; i < 12; i++) {
        si2c_wreg8(b, a, (uint8_t)(0x41 + 2 * i), 12);              /* touch threshold */
        si2c_wreg8(b, a, (uint8_t)(0x42 + 2 * i), 6);               /* release threshold */
    }
    static const uint8_t FILT[11] = { 0x01, 0x01, 0x0E, 0x00, 0x01, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00 };
    for (int i = 0; i < 11; i++) si2c_wreg8(b, a, (uint8_t)(0x2B + i), FILT[i]);   /* MHDR..FDLT */
    si2c_wreg8(b, a, 0x5B, 0x00);                                    /* no debounce */
    si2c_wreg8(b, a, 0x5C, 0x10);                                    /* 16 uA charge current */
    si2c_wreg8(b, a, 0x5D, 0x24);                                    /* 0.5 us, 4 samples, 16 ms period */
    return si2c_wreg8(b, a, 0x5E, 0x8C) ? R_OK : R_NACK;             /* baseline tracking, 12 electrodes */
}

static int mpr_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t r[2];
    if (!si2c_rreg(io->i2c, s->pub.addr, 0x00, r, 2)) return R_NACK;
    uint16_t st = sens_le16(r);
    if (st & 0x8000) {                              /* over current on REXT: it stopped */
        si2c_wreg8(io->i2c, s->pub.addr, 0x01, 0x80);
        si2c_wreg8(io->i2c, s->pub.addr, 0x5E, 0x8C);
        sens_err(s, "ovcf");
        return R_RANGE;
    }
    sens_setv(s, 0, "touch", AOS_SQ_RAW, (float)(st & 0x0FFF));
    return R_OK;
}

const sens_drv_t DRV_MPR121 = {
    .prio = SP_REG,
    .pub = { .id = "mpr121", .name = "MPR121", .addrs = { 0x5A, 0x5B, 0x5C, 0x5D }, .naddr = 4 },
    .identify = mpr_identify, .init = mpr_init, .read = mpr_read,
};

/* ---- Adafruit STEMMA soil sensor: a seesaw (SAMD09 or ATtiny8x7) ----
 * A seesaw is read by writing the module and function bytes, a stop, a
 * wait, and then the read (no repeated start; Adafruit_seesaw). STATUS
 * 0x00: HW_ID 0x01 (0x55 SAMD09, 0x84..0x89 ATtiny8x6/8x7/16x6/16x7),
 * TEMP 0x04 (16.16 fixed point, °C); TOUCH 0x0F, channel 0x10: the
 * moisture, a raw capacitance from about 200 (dry) to 2000 (wet).
 * The HW_ID read is the family's documented read-only one; it is only
 * tried at 0x36 and 0x37, after the chips there that prove themselves by
 * a register (MAX17048), because 0x38/0x39 (the other two jumper choices)
 * are an AHT20's and a PCF8574A's, and the 2-byte write would set a
 * PCF8574A's outputs. There it is declared. */

static bool ss_get(aos_io_i2c_t *b, uint8_t a, uint8_t mod, uint8_t fn, int wait_ms, uint8_t *r, size_t n)
{
    uint8_t w[2] = { mod, fn };
    if (!si2c_wr(b, a, w, 2)) return false;
    aos_hal_sleep_ms((uint32_t)wait_ms);
    return si2c_rd(b, a, r, n);
}

static bool ss_hwid_ok(uint8_t id) { return id == 0x55 || (id >= 0x84 && id <= 0x89); }

static int ss_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return ss_get(b, a, 0x00, 0x01, 2, &id, 1) && ss_hwid_ok(id) ? ID_YES : ID_NO;
}

static int ss_init(sens_io_t *io, sens_slot_t *s)
{
    uint8_t id;
    if (!ss_get(io->i2c, s->pub.addr, 0x00, 0x01, 2, &id, 1)) return R_NACK;
    if (!ss_hwid_ok(id)) { sens_err(s, "0x%02X", id); return R_ID; }
    return R_OK;
}

static int ss_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, t[4], m[2];
    if (!ss_get(b, a, 0x00, 0x04, 2, t, 4)) return R_NACK;
    uint16_t moist = 0xFFFF;
    for (int i = 0; i < 4 && moist == 0xFFFF; i++) {          /* 0xFFFF: not measured yet */
        if (!ss_get(b, a, 0x0F, 0x10, 3 + i, m, 2)) return R_NACK;
        moist = sens_be16(m);
    }
    if (moist == 0xFFFF) return R_NOTREADY;
    sens_setv(s, 0, "moist", AOS_SQ_RAW, (float)moist);
    int32_t tq = (int32_t)((uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3]);
    sens_setv(s, 1, "temp", AOS_SQ_TEMP, (float)tq / 65536.0f);
    return R_OK;
}

const sens_drv_t DRV_SEESAW = {
    .prio = SP_CMD,
    .pub = { .id = "seesaw", .name = "Seesaw", .addrs = { 0x36, 0x37, 0x38, 0x39 }, .naddr = 4 },
    .flags = SF_SLOW,
    .probe = { 0x36, 0x37 }, .nprobe = 2,
    .identify = ss_identify, .init = ss_init, .read = ss_read,
};

/* ---- AS5600 / AS5600L: a magnet's angle, 12 bits (ams datasheet v1-06) ----
 * Only read: RAW_ANGLE 0x0C-0x0D (0..4095 a turn) and STATUS 0x0B (MD a
 * magnet, ML too weak, MH too strong). Its OTP burns with a write to 0xFF,
 * and any write could set ZPOS/MPOS: no identify, declared only (0x36,
 * the AS5600L at 0x40). Without a magnet the angle means nothing, but it
 * is given anyway, with magnet 0. */
static int as_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t st, r[2];
    if (!si2c_rreg8(io->i2c, s->pub.addr, 0x0B, &st) || !si2c_rreg(io->i2c, s->pub.addr, 0x0C, r, 2)) return R_NACK;
    sens_setv(s, 0, "angle", AOS_SQ_ANGLE, (float)(sens_be16(r) & 0x0FFF) * 360.0f / 4096.0f);
    sens_setv(s, 1, "magnet", AOS_SQ_STATE, (st & 0x20) ? 1.0f : 0.0f);
    return R_OK;
}

const sens_drv_t DRV_AS5600 = {
    .prio = SP_REG,
    .pub = { .id = "as5600", .name = "AS5600", .addrs = { 0x36, 0x40 }, .naddr = 2 },
    .flags = SF_NOPROBE,
    .read = as_read,
};

/* ---- DS3231: the temperature of the RTC (Maxim 19-5170) ----
 * 0x11 is the integer part (signed), 0x12 bits 7:6 the quarters; the chip
 * converts every 64 s. Never written. Nothing at 0x68 says "DS3231", so
 * it comes last there, after the IMUs' WHO_AM_I: the 19 registers must be
 * a valid BCD time and date, status bits 6:4 and the temperature's low six
 * bits zero, the temperature between -40 and 85 °C. A DS1307 (the same
 * time registers, RAM from 0x08) passes that only by chance, and then
 * reads as a strange temperature, without anything written to it. */

static bool bcd_ok(uint8_t v, uint8_t lo, uint8_t hi)
{
    if ((v & 0x0F) > 9 || (v >> 4) > 9) return false;
    uint8_t d = (uint8_t)((v >> 4) * 10 + (v & 0x0F));
    return d >= lo && d <= hi;
}

static int ds_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t r[19];
    if (!si2c_rreg(b, a, 0x00, r, sizeof r)) return ID_NO;
    bool h12 = r[2] & 0x40;
    bool ok = bcd_ok(r[0], 0, 59) && bcd_ok(r[1], 0, 59)
           && (h12 ? !(r[2] & 0x80) && bcd_ok(r[2] & 0x1F, 1, 12) : bcd_ok(r[2], 0, 23))
           && r[3] >= 1 && r[3] <= 7 && bcd_ok(r[4], 1, 31) && bcd_ok(r[5] & 0x7F, 1, 12) && bcd_ok(r[6], 0, 99)
           && bcd_ok(r[7] & 0x7F, 0, 59)                    /* alarm 1 seconds */
           && !(r[0x0F] & 0x70) && !(r[0x12] & 0x3F)
           && (int8_t)r[0x11] >= -40 && (int8_t)r[0x11] <= 85;
    return ok ? ID_YES : ID_NO;
}

static int ds_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t r[2];
    if (!si2c_rreg(io->i2c, s->pub.addr, 0x11, r, 2)) return R_NACK;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)(int8_t)r[0] + (float)(r[1] >> 6) * 0.25f);
    return R_OK;
}

const sens_drv_t DRV_DS3231 = {
    .prio = SP_LAST,
    .cat = SC(TEMP),
    .pub = { .id = "ds3231", .name = "DS3231", .addrs = { 0x68 }, .naddr = 1 },
    .identify = ds_identify, .read = ds_read,
};

/* ---- PCF8574 / PCF8574A: 8 quasi-bidirectional pins as inputs (NXP) ----
 * A pin reads as an input only while its latch is high (a weak pull-up);
 * init() writes 0xFF once, and from then on a read is one byte, a bit per
 * pin, P0 the lowest. A button to GND reads 0 when pressed. It has no
 * registers and answers anything: declared only, 0x20-0x27 (PCF8574) and
 * 0x38-0x3F (PCF8574A). */

static int pcf_init(sens_io_t *io, sens_slot_t *s)
{
    uint8_t ff = 0xFF;
    return si2c_wr(io->i2c, s->pub.addr, &ff, 1) ? R_OK : R_NACK;
}

static int pcf_read(sens_io_t *io, sens_slot_t *s)
{
    static const char *const KEY[8] = { "p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7" };
    uint8_t v;
    if (!si2c_rd(io->i2c, s->pub.addr, &v, 1)) return R_NACK;
    for (int i = 0; i < 8; i++) sens_setv(s, i, KEY[i], AOS_SQ_STATE, (float)(v >> i & 1));
    return R_OK;
}

const sens_drv_t DRV_PCF8574 = {
    .prio = SP_REG,
    .pub = { .id = "pcf8574", .name = "PCF8574", .addrs = { 0x20, 0x38, 0x27, 0x3F }, .naddr = 4 },
    .flags = SF_NOPROBE,
    .init = pcf_init, .read = pcf_read,
};

const sens_drv_t *const SENS_G_MISC[] = {
    &DRV_MAX3010X, &DRV_MPR121, &DRV_SEESAW, &DRV_AS5600, &DRV_DS3231, &DRV_PCF8574,
    NULL,
};

const sens_alias_t SENS_A_MISC[] = {
    { "max30102", "max3010x" }, { "max30105", "max3010x" }, { "max30101", "max3010x" }, { "max30100", "max3010x" },
    { "zs042", "ds3231" }, { "pcf8574a", "pcf8574" }, { "as5600l", "as5600" },
    { "soil", "seesaw" },
    { NULL, NULL },
};
