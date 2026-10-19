/*
 * P4OS - Sensor drivers: temperature and humidity on I2C (sens_int.h, docs/SENSORS.md).
 *
 * SHT2x/HTU21D/Si70xx, HDC1080, HDC2080, HDC302x, SHTC3/SHTC1, AM2320,
 * MCP9808, TMP117/116/119, TMP102, the LM75 family, MLX90614 and STS3x.
 *
 * Two of them share a guard. A Sensirion chip that has nothing to give
 * NACKs a read header, while a register chip (a TCA9548A mux at 0x70, an
 * ADS1115 or a TMP102 at 0x4A) ACKs it and returns its current register.
 * So the SHTC3 and the STS3x try a plain read first, and only a chip that
 * refuses it gets a 16-bit command: that keeps a mux's channel mask and a
 * TMP102's limits untouched.
 */
#include "sens_int.h"

/* Sensirion's word: two bytes and a CRC (0x31 from 0xFF) */
static bool word_ok(const uint8_t *p) { return sens_crc8_si(p, 2) == p[2]; }

static float clamp_rh(float h) { return h < 0 ? 0 : h > 100 ? 100 : h; }

/* A plain read that something ACKed: a register chip, not a Sensirion one */
static bool reg_chip_answers(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[3];
    return si2c_rd(b, a, d, 3);
}

/* ---- SHT2x / HTU21D / Si7021 / Si7020 / Si7013 at 0x40 ----
 * Sensirion SHT21 datasheet (5.3-5.7, 6.1-6.2), Silicon Labs Si7021-A20
 * datasheet (5.1, 5.5, 5.6), ESPHome htu21d, Adafruit_Si7021. Commands are
 * single bytes: soft reset 0xFE (15 ms), user register 0xE7, measure in
 * no-hold mode 0xF3 (T) and 0xF5 (RH): the chip NACKs the read header until
 * it is done, 85 ms at most for a 14-bit T on an SHT21, 29 ms for a 12-bit
 * RH. The result is MSB, LSB (the two low bits are status: bit 1 set for
 * RH) and a CRC-8, 0x31 from 0x00. The user register after a reset is 0x02
 * on an SHT2x/HTU21D and 0x3A on an Si70xx (reserved bits read as 1):
 * resolution 00, no heater, OTP reload off either way. */

typedef struct { bool t_pend; uint32_t t_ms; } sht2_t;
SPRIV_CHECK(sht2_t);

/* read a no-hold result, polling while the chip still NACKs */
static bool sht2_result(aos_io_i2c_t *b, uint8_t a, uint32_t since, uint32_t max_ms, uint8_t d[3])
{
    for (;;) {
        if (si2c_rd(b, a, d, 3)) return true;
        if (sens_now() - since >= max_ms) return false;
        aos_hal_sleep_ms(5);
    }
}

static int sht2_check(const uint8_t *d, bool rh)
{
    if (sens_crc8(d, 2, 0x31, 0x00) != d[2]) return R_CRC;
    return ((d[1] & 0x02) != 0) == rh ? R_OK : R_ID;
}

/* Soft reset, 15 ms. The INA226's and HDC1080's probes read 0xFE, which an
 * SHT2x takes for a reset too: it may still be in it, NACKing. */
static bool sht2_reset(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t c = 0xFE;
    if (!si2c_wr(b, a, &c, 1)) {
        aos_hal_sleep_ms(15);
        if (!si2c_wr(b, a, &c, 1)) return false;
    }
    aos_hal_sleep_ms(15);
    return true;
}

static int sht2_identify(aos_io_i2c_t *b, uint8_t a)
{
    /* every write here is one byte: on a register chip it only moves the
     * pointer */
    uint8_t c, ur = 0, d[3];
    if (!sht2_reset(b, a)) return ID_NO;
    c = 0xE7;
    if (!si2c_wrrd(b, a, &c, 1, &ur, 1) || (ur & 0xC7) != 0x02) return ID_NO;
    c = 0xF3;
    if (!si2c_wr(b, a, &c, 1)) return ID_NO;
    uint32_t t0 = sens_now();
    aos_hal_sleep_ms(12);
    if (!sht2_result(b, a, t0, 100, d)) return ID_NO;
    return sht2_check(d, false) == R_OK ? ID_YES : ID_NO;
}

static int sht2_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, c, ur = 0;
    sht2_t *p = SPRIV(s, sht2_t);
    memset(p, 0, sizeof *p);
    if (!sht2_reset(b, a)) return R_NACK;
    c = 0xE7;
    if (!si2c_wrrd(b, a, &c, 1, &ur, 1)) return R_NACK;
    if ((ur & 0xC7) != 0x02) { sens_err(s, "ur %02X", ur); return R_ID; }
    const char *label = "SHT2x/HTU21";
    if (ur == 0x3A) {
        /* Si70xx: the electronic ID's second half, SNB_3 is the model
         * (Si7021-A20 datasheet 5.6, Adafruit_Si7021) */
        uint8_t w[2] = { 0xFC, 0xC9 }, id[6];
        label = "Si70xx";
        if (si2c_wrrd(b, a, w, 2, id, 6) && sens_crc8(id, 2, 0x31, 0x00) == id[2]) {
            if (id[0] == 0x0D) label = "Si7013";
            else if (id[0] == 0x14) label = "Si7020";
            else if (id[0] == 0x15) label = "Si7021";
        }
    }
    sens_scpy(s->pub.chip, sizeof s->pub.chip, label);
    c = 0xF3;                                       /* the first T, for the first read */
    if (!si2c_wr(b, a, &c, 1)) return R_NACK;
    p->t_pend = true;
    p->t_ms = sens_now();
    return R_OK;
}

/* Each read collects the T started by the one before, measures RH and
 * starts the next T: about 20-45 ms of the round instead of 110. */
static int sht2_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, c, dt[3], dh[3];
    sht2_t *p = SPRIV(s, sht2_t);
    if (!p->t_pend) {
        c = 0xF3;
        if (!si2c_wr(b, a, &c, 1)) return R_NACK;
        p->t_pend = true;
        p->t_ms = sens_now();
        return R_NOTREADY;
    }
    p->t_pend = false;
    if (!sht2_result(b, a, p->t_ms, 100, dt)) return R_NACK;
    int r = sht2_check(dt, false);
    if (r != R_OK) return r;
    c = 0xF5;
    if (!si2c_wr(b, a, &c, 1)) return R_NACK;
    uint32_t t0 = sens_now();
    aos_hal_sleep_ms(14);
    if (!sht2_result(b, a, t0, 50, dh)) return R_NACK;
    r = sht2_check(dh, true);
    if (r != R_OK) return r;
    c = 0xF3;
    if (si2c_wr(b, a, &c, 1)) { p->t_pend = true; p->t_ms = sens_now(); }
    float t = -46.85f + 175.72f * (float)(sens_be16(dt) & 0xFFFC) / 65536.0f;
    float h = -6.0f + 125.0f * (float)(sens_be16(dh) & 0xFFFC) / 65536.0f;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, t);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, clamp_rh(h));
    return R_OK;
}

const sens_drv_t DRV_SHT2X = {
    .prio = SP_CMD,                 /* after INA219/226, HDC1080/2080: their reads come first */
    .pub = { .id = "sht2x", .name = "SHT2x", .addrs = { 0x40 }, .naddr = 1 },
    .identify = sht2_identify, .init = sht2_init, .read = sht2_read,
};

/* ---- HDC1080 (and HDC1000/1008/1050) ----
 * TI HDC1080 datasheet (8.5, 8.6): manufacturer 0xFE = 0x5449, device 0xFF
 * = 0x1050 (0x1000 on an HDC1000/1008). Configuration 0x02 = 0x1000: T and
 * RH in sequence, 14 bits each. Writing the pointer 0x00 starts both; the
 * chip NACKs the read until they are done (6.35 + 6.5 ms), then gives four
 * bytes. T = r / 2^16 x 165 - 40, RH = r / 2^16 x 100. */

static int hdc1_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t m, d;
    if (!si2c_rreg16be(b, a, 0xFE, &m) || m != 0x5449) return ID_NO;
    if (!si2c_rreg16be(b, a, 0xFF, &d)) return ID_NO;
    return d == 0x1050 || d == 0x1000 ? ID_YES : ID_NO;
}

static int hdc1_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t d = 0;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0xFF, &d)) return R_NACK;
    if (d != 0x1050 && d != 0x1000) { sens_err(s, "0x%04X", d); return R_ID; }
    sens_scpy(s->pub.chip, sizeof s->pub.chip, d == 0x1000 ? "HDC1000" : "HDC1080");
    if (!si2c_wreg16be(io->i2c, s->pub.addr, 0x02, 0x1000)) return R_NACK;
    return R_OK;
}

static int hdc1_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t ptr = 0x00, d[4];
    if (!si2c_wr(io->i2c, s->pub.addr, &ptr, 1)) return R_NACK;
    aos_hal_sleep_ms(15);
    bool ok = false;
    for (int i = 0; i < 4 && !ok; i++) {
        ok = si2c_rd(io->i2c, s->pub.addr, d, 4);
        if (!ok) aos_hal_sleep_ms(5);
    }
    if (!ok) return R_NACK;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)sens_be16(d) * 165.0f / 65536.0f - 40.0f);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, clamp_rh((float)sens_be16(d + 2) * 100.0f / 65536.0f));
    return R_OK;
}

const sens_drv_t DRV_HDC1080 = {
    .prio = SP_REG,
    .pub = { .id = "hdc1080", .name = "HDC1080", .addrs = { 0x40, 0x41, 0x42, 0x43 }, .naddr = 4 },
    .identify = hdc1_identify, .init = hdc1_init, .read = hdc1_read,
};

/* ---- HDC2080 / HDC2010 ----
 * TI HDC2080 datasheet SNAS678C (7.6): 0xFC-0xFD manufacturer 0x5449 and
 * 0xFE-0xFF device 0x07D0, little-endian. 0x0E = 0x00: manual mode, no
 * auto measurement; 0x0F = 0x01: 14-bit T and RH, start. DRDY is bit 7 of
 * 0x04. Data at 0x00-0x03, little-endian. RH = r / 2^16 x 100; T = r / 2^16
 * x 165 - 40.5 since revision C (the older one, and ESPHome, say -40; the
 * Zephyr driver moved to -40.5 with it). */

static int hdc2_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[4];
    if (!si2c_rreg(b, a, 0xFC, d, 4)) return ID_NO;
    return d[0] == 0x49 && d[1] == 0x54 && d[2] == 0xD0 && d[3] == 0x07 ? ID_YES : ID_NO;
}

static int hdc2_init(sens_io_t *io, sens_slot_t *s)
{
    uint8_t d[4];
    if (!si2c_rreg(io->i2c, s->pub.addr, 0xFC, d, 4)) return R_NACK;
    if (d[2] != 0xD0 || d[3] != 0x07) { sens_err(s, "0x%02X%02X", d[3], d[2]); return R_ID; }
    if (!si2c_wreg8(io->i2c, s->pub.addr, 0x0E, 0x00)) return R_NACK;
    return R_OK;
}

static int hdc2_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, st = 0, d[4];
    if (!si2c_wreg8(b, a, 0x0F, 0x01)) return R_NACK;
    aos_hal_sleep_ms(2);
    for (int i = 0; i < 5; i++) {
        if (!si2c_rreg8(b, a, 0x04, &st)) return R_NACK;
        if (st & 0x80) break;
        aos_hal_sleep_ms(2);
    }
    if (!(st & 0x80)) return R_NOTREADY;
    if (!si2c_rreg(b, a, 0x00, d, 4)) return R_NACK;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)sens_le16(d) * 165.0f / 65536.0f - 40.5f);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, clamp_rh((float)sens_le16(d + 2) * 100.0f / 65536.0f));
    return R_OK;
}

const sens_drv_t DRV_HDC2080 = {
    .prio = SP_REG,
    .pub = { .id = "hdc2080", .name = "HDC2080", .addrs = { 0x40, 0x41 }, .naddr = 2 },
    .identify = hdc2_identify, .init = hdc2_init, .read = hdc2_read,
};

/* ---- HDC3020 / HDC3021 / HDC3022 ----
 * TI HDC302x datasheet (8.5): Sensirion-style 16-bit commands and words
 * with CRC-8 0x31 from 0xFF. It answers the SHT3x's commands too, status
 * 0xF32D among them, so it goes before the SHT3x (prio 45 < SP_CMD) and
 * proves itself with "read manufacturer ID", 0x3781, which gives 0x3000
 * (an SHT3x NACKs it). Trigger-on-demand 0x2400 (LPM0, under 15 ms), then
 * six bytes: T = -45 + 175 r / 65535, RH = 100 r / 65535. 0x3093 leaves
 * an auto-measurement mode somebody else may have started. */

static int hdc3_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[3];
    if (!si2c_cmd16(b, a, 0x3781)) return ID_NO;
    if (!si2c_rd(b, a, d, 3) || !word_ok(d)) return ID_NO;
    return sens_be16(d) == 0x3000 ? ID_YES : ID_NO;
}

static int hdc3_init(sens_io_t *io, sens_slot_t *s)
{
    si2c_cmd16(io->i2c, s->pub.addr, 0x3093);
    aos_hal_sleep_ms(1);
    return R_OK;
}

static int hdc3_read(sens_io_t *io, sens_slot_t *s)
{
    if (!si2c_cmd16(io->i2c, s->pub.addr, 0x2400)) return R_NACK;
    aos_hal_sleep_ms(15);
    uint8_t d[6];
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        ok = si2c_rd(io->i2c, s->pub.addr, d, 6);
        if (!ok) aos_hal_sleep_ms(5);
    }
    if (!ok) return R_NACK;
    if (!word_ok(d) || !word_ok(d + 3)) return R_CRC;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, -45.0f + 175.0f * (float)sens_be16(d) / 65535.0f);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, clamp_rh(100.0f * (float)sens_be16(d + 3) / 65535.0f));
    return R_OK;
}

const sens_drv_t DRV_HDC302X = {
    .prio = 45,                     /* before the SHT3x (SP_CMD), which it would pass for */
    .pub = { .id = "hdc302x", .name = "HDC302x", .addrs = { 0x44, 0x45, 0x46, 0x47 }, .naddr = 4 },
    .identify = hdc3_identify, .init = hdc3_init, .read = hdc3_read,
};

/* ---- SHTC3 / SHTC1 at 0x70 ----
 * Sensirion SHTC3 datasheet (5.3-5.9), ESPHome shtcx. Wake 0x3517, sleep
 * 0xB098 (the SHTC1 has neither), ID 0xEFC8: (id & 0x3F) == 0x07, bit 11
 * set on an SHTC3. Measure 0x7866 (normal mode, T first, no clock
 * stretching), 12.1 ms on an SHTC3 and 14.4 on an SHTC1, then T and RH
 * words: T = -45 + 175 r / 2^16, RH = 100 r / 2^16. It is never put to
 * sleep here: asleep it NACKs its address, and the service's probe would
 * take that for a chip that went away.
 * 0x70 is also a TCA9548A's, and any byte written to one becomes its
 * channel mask: the plain-read guard goes first (see the top). */

typedef struct { bool c3; } shtc_t;
SPRIV_CHECK(shtc_t);

static bool shtc_id(aos_io_i2c_t *b, uint8_t a, uint16_t *id)
{
    uint8_t d[3];
    si2c_cmd16(b, a, 0x3517);                       /* wake an SHTC3; an SHTC1 NACKs it */
    aos_hal_sleep_ms(1);
    if (!si2c_cmd16(b, a, 0xEFC8) || !si2c_rd(b, a, d, 3) || !word_ok(d)) return false;
    *id = sens_be16(d);
    return (*id & 0x3F) == 0x07;
}

static int shtc_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t id;
    if (reg_chip_answers(b, a)) return ID_NO;
    return shtc_id(b, a, &id) ? ID_YES : ID_NO;
}

static int shtc_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t id = 0;
    shtc_t *p = SPRIV(s, shtc_t);
    if (!shtc_id(io->i2c, s->pub.addr, &id)) {
        if (id) { sens_err(s, "0x%04X", id); return R_ID; }
        return R_NACK;
    }
    p->c3 = (id & 0x0800) != 0;
    sens_scpy(s->pub.chip, sizeof s->pub.chip, p->c3 ? "SHTC3" : "SHTC1");
    return R_OK;
}

static int shtc_read(sens_io_t *io, sens_slot_t *s)
{
    const shtc_t *p = SPRIV(s, shtc_t);
    if (p->c3) si2c_cmd16(io->i2c, s->pub.addr, 0x3517);
    if (!si2c_cmd16(io->i2c, s->pub.addr, 0x7866)) return R_NACK;
    aos_hal_sleep_ms(15);
    uint8_t d[6];
    if (!si2c_rd(io->i2c, s->pub.addr, d, 6)) return R_NACK;
    if (!word_ok(d) || !word_ok(d + 3)) return R_CRC;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, -45.0f + 175.0f * (float)sens_be16(d) / 65536.0f);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, clamp_rh(100.0f * (float)sens_be16(d + 3) / 65536.0f));
    return R_OK;
}

const sens_drv_t DRV_SHTC3 = {
    .prio = SP_LAST,                /* after every chip that proves itself by registers at 0x70 */
    .pub = { .id = "shtc3", .name = "SHTC3", .addrs = { 0x70 }, .naddr = 1 },
    .identify = shtc_identify, .init = shtc_init, .read = shtc_read,
};

/* ---- AM2320 at 0x5C ----
 * Aosong AM2320 datasheet (7.2), Adafruit_AM2320. Asleep, it NACKs the
 * first transaction and wakes up (it sleeps again 3 s after the last
 * one): a one-byte write that may fail, then 0.8-3 ms. Read is a Modbus
 * function: write 0x03, the first register, the count; 1.5 ms later the
 * answer is 0x03, the count, the registers and a CRC-16 (Modbus, low byte
 * first). Registers 0x00-0x03: RH and T in tenths, T sign and magnitude.
 * The answer is the previous measurement (each read starts the next), and
 * it measures at most every 2 s. */

static int am2320_xfer(aos_io_i2c_t *b, uint8_t a, uint8_t d[8])
{
    uint8_t wake = 0x00, cmd[3] = { 0x03, 0x00, 0x04 };
    si2c_wr(b, a, &wake, 1);                        /* NACKed when it was asleep */
    aos_hal_sleep_ms(3);
    if (!si2c_wr(b, a, cmd, 3)) return R_NACK;
    aos_hal_sleep_ms(2);
    if (!si2c_rd(b, a, d, 8)) return R_NACK;
    if (sens_crc16_modbus(d, 6) != sens_le16(d + 6)) return R_CRC;
    if (d[0] != 0x03 || d[1] != 0x04) return R_ID;
    return R_OK;
}

static int am2320_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[8];
    return am2320_xfer(b, a, d) == R_OK ? ID_YES : ID_NO;
}

static int am2320_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t d[8];
    int r = am2320_xfer(io->i2c, s->pub.addr, d);
    if (r == R_ID) sens_err(s, "%02X%02X", d[0], d[1]);
    if (r != R_OK) return r;
    uint16_t rh = sens_be16(d + 2), t = sens_be16(d + 4);
    float tc = (float)(t & 0x7FFF) / 10.0f;
    if (t & 0x8000) tc = -tc;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, tc);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, clamp_rh((float)rh / 10.0f));
    return R_OK;
}

const sens_drv_t DRV_AM2320 = {
    .prio = SP_CMD,                 /* after the LPS22's WHO_AM_I; before the BH1750 candidate */
    .pub = { .id = "am2320", .name = "AM2320", .addrs = { 0x5C }, .naddr = 1 },
    .min_ms = 2000,
    .identify = am2320_identify, .read = am2320_read,
};

/* ---- MCP9808 at 0x18-0x1F ----
 * Microchip MCP9808 datasheet (5.1): manufacturer 0x06 = 0x0054, device
 * and revision 0x07 = 0x04xx. Ambient 0x05: bits 12..0 in 1/16 °C, bit 12
 * the sign; bits 15..13 are the alert flags. Continuous at 0.0625 °C,
 * 250 ms, unless SHDN (config 0x01 bit 8) is set. */

static int mcp9808_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t m, d;
    if (!si2c_rreg16be(b, a, 0x06, &m) || m != 0x0054) return ID_NO;
    if (!si2c_rreg16be(b, a, 0x07, &d)) return ID_NO;
    return (d >> 8) == 0x04 ? ID_YES : ID_NO;
}

static int mcp9808_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t m = 0, cfg = 0;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x06, &m)) return R_NACK;
    if (m != 0x0054) { sens_err(s, "0x%04X", m); return R_ID; }
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x01, &cfg)) return R_NACK;
    if (cfg & 0x0100 && !si2c_wreg16be(io->i2c, s->pub.addr, 0x01, (uint16_t)(cfg & ~0x0100))) return R_NACK;
    return R_OK;
}

static int mcp9808_read(sens_io_t *io, sens_slot_t *s)
{
    uint16_t v;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x05, &v)) return R_NACK;
    float t = (float)(v & 0x0FFF) / 16.0f;
    if (v & 0x1000) t -= 256.0f;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, t);
    return R_OK;
}

const sens_drv_t DRV_MCP9808 = {
    .prio = SP_REG,
    .pub = { .id = "mcp9808", .name = "MCP9808", .addrs = { 0x18, 0x19, 0x1A, 0x1B }, .naddr = 4 },
    .probe = { 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }, .nprobe = 8,
    .identify = mcp9808_identify, .init = mcp9808_init, .read = mcp9808_read,
};

/* ---- TMP117 / TMP116 / TMP119 at 0x48-0x4B ----
 * TI TMP117 datasheet (7.6): device ID 0x0F = 0x0117 (TMP116 0x1116,
 * TMP119 0x2117: the top nibble is the revision). Temperature 0x00, int16
 * x 7.8125 m°C, 0x8000 until the first conversion. Continuous from power
 * on (config 0x01 = 0x0220, one a second, 8 averages); MOD = 01 in bits
 * 11:10 is shutdown. */

static int tmp117_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t id;
    if (!si2c_rreg16be(b, a, 0x0F, &id)) return ID_NO;
    return (id & 0x0FFF) == 0x117 || (id & 0x0FFF) == 0x116 ? ID_YES : ID_NO;
}

static int tmp117_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t id = 0, cfg = 0;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x0F, &id)) return R_NACK;
    if ((id & 0x0FFF) != 0x117 && (id & 0x0FFF) != 0x116) { sens_err(s, "0x%04X", id); return R_ID; }
    sens_scpy(s->pub.chip, sizeof s->pub.chip,
              (id & 0x0FFF) == 0x116 ? "TMP116" : (id >> 12) == 2 ? "TMP119" : "TMP117");
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x01, &cfg)) return R_NACK;
    if ((cfg & 0x0C00) == 0x0400 && !si2c_wreg16be(io->i2c, s->pub.addr, 0x01, (uint16_t)(cfg & ~0x0C00))) return R_NACK;
    return R_OK;
}

static int tmp117_read(sens_io_t *io, sens_slot_t *s)
{
    uint16_t v;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x00, &v)) return R_NACK;
    if (v == 0x8000) return R_NOTREADY;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)(int16_t)v * 0.0078125f);
    return R_OK;
}

const sens_drv_t DRV_TMP117 = {
    .prio = SP_REG,
    .pub = { .id = "tmp117", .name = "TMP117", .addrs = { 0x48, 0x49, 0x4A, 0x4B }, .naddr = 4 },
    .identify = tmp117_identify, .init = tmp117_init, .read = tmp117_read,
};

/* ---- TMP102 (and TMP112) at 0x48-0x4B ----
 * TI TMP102 datasheet (7.5): no ID. Configuration 0x01 powers on as
 * 0x60A0; R1:R0 (0x6000) always read 1 and bits 3..0 always 0. T_LOW 0x02
 * = 0x4B00 (75 °C) and T_HIGH 0x03 = 0x5000 (80 °C) at power on. All of it
 * is a chip nobody configured: ID_YES; the config bits alone, ID_MAYBE.
 * An ADS1115 (0x8583) or an INA219 (0x399F) fails the config bits.
 * Temperature 0x00: 12 bits left-justified x 0.0625; bit 0 set in
 * extended mode (13 bits). SD (config bit 8) is shutdown. */

static int tmp102_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t cfg, lo, hi;
    if (!si2c_rreg16be(b, a, 0x01, &cfg) || (cfg & 0x600F) != 0x6000) return ID_NO;
    if (si2c_rreg16be(b, a, 0x02, &lo) && si2c_rreg16be(b, a, 0x03, &hi) && lo == 0x4B00 && hi == 0x5000) return ID_YES;
    return ID_MAYBE;
}

static int tmp102_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t cfg = 0;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x01, &cfg)) return R_NACK;
    if (cfg & 0x0100 && !si2c_wreg16be(io->i2c, s->pub.addr, 0x01, (uint16_t)(cfg & ~0x0100))) return R_NACK;
    return R_OK;
}

static int tmp102_read(sens_io_t *io, sens_slot_t *s)
{
    uint16_t v;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x00, &v)) return R_NACK;
    int16_t r = (int16_t)v;
    r = (int16_t)(v & 1 ? r >> 3 : r >> 4);
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)r * 0.0625f);
    return R_OK;
}

const sens_drv_t DRV_TMP102 = {
    .prio = SP_LAST,
    .pub = { .id = "tmp102", .name = "TMP102", .addrs = { 0x48, 0x49, 0x4A, 0x4B }, .naddr = 4 },
    .identify = tmp102_identify, .init = tmp102_init, .read = tmp102_read,
};

/* ---- LM75 / LM75A / LM75B / TMP75 / PCT2075 / DS75: declared only ----
 * NXP LM75A, TI TMP75, NXP PCT2075 datasheets: temperature 0x00, two's
 * complement left-justified, °C = int16 / 256 with only the top bits
 * valid (bits=): 9 on an LM75, 11 on an LM75A/B and PCT2075 (the default here),
 * up to 12 on a TMP75 or DS75 set to it. No ID, and the power-on limits
 * equal a TMP102's: nothing safe proves it. */

static int lm75_read(sens_io_t *io, sens_slot_t *s)
{
    uint16_t v;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x00, &v)) return R_NACK;
    int32_t bits = sens_arg_int(s, "bits", 11);
    if (bits < 9) bits = 9;
    if (bits > 12) bits = 12;
    uint16_t mask = (uint16_t)(0xFFFFu << (16 - bits));
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)(int16_t)(v & mask) / 256.0f);
    return R_OK;
}

const sens_drv_t DRV_LM75 = {
    .prio = SP_LAST,
    .pub = { .id = "lm75", .name = "LM75", .addrs = { 0x48, 0x49, 0x4A, 0x4B }, .naddr = 4 },
    .flags = SF_NOPROBE,
    .read = lm75_read,
};

/* ---- MLX90614 at 0x5A ----
 * Melexis MLX90614 datasheet (8.4): SMBus at 100 kHz at most. Read word:
 * the command, a repeated start, LSB, MSB and a PEC, CRC-8 0x07 from 0x00
 * over the address+W, the command, the address+R and the two bytes. RAM
 * 0x06 is the ambient (Ta), 0x07 the object (Tobj1); K = r x 0.02, bit 15
 * of an object reading is its error flag. EEPROM 0x0E (command 0x2E)
 * holds the SMBus address in its low byte: the chip's own address and a
 * good PEC prove it. */

static bool mlx_word(aos_io_i2c_t *b, uint8_t a, uint8_t cmd, uint16_t *v, bool *pec_ok)
{
    uint8_t d[3];
    if (!si2c_wrrd(b, a, &cmd, 1, d, 3)) return false;
    uint8_t m[5] = { (uint8_t)(a << 1), cmd, (uint8_t)(a << 1 | 1), d[0], d[1] };
    *pec_ok = sens_crc8(m, 5, 0x07, 0x00) == d[2];
    *v = sens_le16(d);
    return true;
}

static int mlx_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t v;
    bool ok;
    if (!mlx_word(b, a, 0x2E, &v, &ok) || !ok) return ID_NO;
    return (v & 0xFF) == a ? ID_YES : ID_NO;
}

static int mlx_read(sens_io_t *io, sens_slot_t *s)
{
    uint16_t to, ta;
    bool ok1, ok2;
    if (!mlx_word(io->i2c, s->pub.addr, 0x07, &to, &ok1) || !mlx_word(io->i2c, s->pub.addr, 0x06, &ta, &ok2)) return R_NACK;
    if (!ok1 || !ok2) return R_CRC;
    if (to & 0x8000 || ta & 0x8000) return R_RANGE;
    sens_setv(s, 0, "obj", AOS_SQ_TEMP, (float)to * 0.02f - 273.15f);
    sens_setv(s, 1, "amb", AOS_SQ_TEMP, (float)ta * 0.02f - 273.15f);
    return R_OK;
}

const sens_drv_t DRV_MLX90614 = {
    .prio = SP_REG,                 /* a PEC is proof: before the CCS811's and MPR121's tries */
    .flags = SF_SLOW,
    .pub = { .id = "mlx90614", .name = "MLX90614", .addrs = { 0x5A }, .naddr = 1 },
    .identify = mlx_identify, .read = mlx_read,
};

/* ---- STS3x (STS30/31/35) at 0x4A/0x4B ----
 * Sensirion STS3x-DIS datasheet (4.3-4.11): the SHT3x's commands, T only.
 * Status 0xF32D gives a word with its CRC; single shot 0x2400 (high
 * repeatability, no clock stretching, 15.5 ms at most), then one word: T =
 * -45 + 175 r / 65535. The plain-read guard keeps the command off an
 * ADS1115 or a TMP102 that did not prove itself. */

static int sts3_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[3];
    if (reg_chip_answers(b, a)) return ID_NO;
    if (si2c_cmd16(b, a, 0xF32D) && si2c_rd(b, a, d, 3) && word_ok(d)) return ID_YES;
    return ID_NO;
}

static int sts3_read(sens_io_t *io, sens_slot_t *s)
{
    if (!si2c_cmd16(io->i2c, s->pub.addr, 0x2400)) return R_NACK;
    aos_hal_sleep_ms(16);
    uint8_t d[3];
    if (!si2c_rd(io->i2c, s->pub.addr, d, 3)) return R_NACK;
    if (!word_ok(d)) return R_CRC;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, -45.0f + 175.0f * (float)sens_be16(d) / 65535.0f);
    return R_OK;
}

const sens_drv_t DRV_STS3X = {
    .prio = SP_CMD,
    .pub = { .id = "sts3x", .name = "STS3x", .addrs = { 0x4A, 0x4B }, .naddr = 2 },
    .identify = sts3_identify, .read = sts3_read,
};

const sens_drv_t *const SENS_G_TEMP[] = {
    &DRV_HDC1080, &DRV_HDC2080, &DRV_MCP9808, &DRV_TMP117, &DRV_MLX90614,
    &DRV_HDC302X, &DRV_SHT2X, &DRV_AM2320, &DRV_STS3X,
    &DRV_TMP102, &DRV_SHTC3, &DRV_LM75,
    NULL,
};

const sens_alias_t SENS_A_TEMP[] = {
    { "sht20", "sht2x" }, { "sht21", "sht2x" }, { "sht25", "sht2x" }, { "htu21d", "sht2x" }, { "htu20d", "sht2x" },
    { "htu21", "sht2x" }, { "si7021", "sht2x" }, { "si7020", "sht2x" }, { "si7013", "sht2x" }, { "gy21", "sht2x" },
    { "hdc1000", "hdc1080" }, { "hdc1008", "hdc1080" }, { "hdc1050", "hdc1080" },
    { "hdc2010", "hdc2080" },
    { "hdc3020", "hdc302x" }, { "hdc3021", "hdc302x" }, { "hdc3022", "hdc302x" },
    { "shtc1", "shtc3" },
    { "tmp116", "tmp117" }, { "tmp119", "tmp117" },
    { "tmp112", "tmp102" },
    { "lm75a", "lm75" }, { "lm75b", "lm75" }, { "tmp75", "lm75" }, { "pct2075", "lm75" }, { "ds75", "lm75" },
    { "gy906", "mlx90614" },
    { "sts30", "sts3x" }, { "sts31", "sts3x" }, { "sts35", "sts3x" },
    { NULL, NULL },
};
