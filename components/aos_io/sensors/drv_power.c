/*
 * P4OS - Sensor drivers: power monitors, ADCs, scales and thermocouple
 * amplifiers on I2C (sens_int.h, docs/SENSORS.md). INA260, INA3221,
 * INA228/INA238/INA237, MAX17048/MAX17049/MAX17043, ADS1015, PCF8591,
 * MCP3421..MCP3428, NAU7802 and MCP9600/MCP9601.
 *
 * Sources, besides each chip's datasheet: Adafruit's INA260, INA3221,
 * INA228, INA237_INA238, MAX1704X, ADS1X15, PCF8591, MCP9600 and NAU7802
 * libraries, SparkFun's Qwiic Scale (NAU7802) and MAX1704x libraries, and
 * ESPHome's ina260/ina2xx/mcp3421 components.
 */
#include "sens_int.h"

#include <ctype.h>

/* The model the module line asks for: model=, chip= or the module's name,
 * lowercase ("mcp3424", "ina237"); a detected sensor's name is its id */
static void model_of(const sens_slot_t *s, char *out, size_t n)
{
    if (!sens_arg_str(s, "model", out, n) && !sens_arg_str(s, "chip", out, n)) sens_scpy(out, n, s->pub.name);
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static float shunt_ohm(const sens_slot_t *s, const char *key)
{
    int32_t m = key ? sens_arg_int(s, key, s->shunt_mohm) : s->shunt_mohm;
    return (float)(m > 0 ? m : 100) / 1000.0f;
}

/* TI's manufacturer id 0x5449 ("TI") at 0xFE and a die id at 0xFF, whose
 * low nibble is the revision. HDC1080 (0x1050), INA226 (0x2260), INA260
 * (0x2270) and INA3221 (0x3220) all have the "TI": the die id tells. */
static int ti_identify(aos_io_i2c_t *b, uint8_t a, uint16_t die)
{
    uint16_t m, d;
    if (!si2c_rreg16be(b, a, 0xFE, &m) || m != 0x5449) return ID_NO;
    if (!si2c_rreg16be(b, a, 0xFF, &d)) return ID_NO;
    return (d & 0xFFF0) == die ? ID_YES : ID_NO;
}

static int ti_check(sens_io_t *io, sens_slot_t *s, uint16_t die)
{
    uint16_t d = 0;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0xFF, &d)) return R_NACK;
    if ((d & 0xFFF0) != die) { sens_err(s, "0x%04X", d); return R_ID; }
    return R_OK;
}

/* ---- INA260: its own 2 mΩ shunt, so the chip gives amperes and watts ----
 * Current 0x01 (signed) × 1.25 mA, bus 0x02 × 1.25 mV, power 0x03 × 10 mW.
 * It can sit at any of 0x40..0x4F, and a probe list holds eight: a hidden
 * twin looks at 0x48..0x4F. There a PCF8591 may live, and a pointer write
 * is its control byte: 0xFE would switch its DAC output on. So the twin
 * first reads the configuration, whose reserved bits 14..12 an INA260
 * always reads 110 (reset 0x6127), and only then the ids. */

static int ina260_identify(aos_io_i2c_t *b, uint8_t a) { return ti_identify(b, a, 0x2270); }

static int ina260_identify_hi(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t c;
    if (!si2c_rreg16be(b, a, 0x00, &c) || (c & 0x7000) != 0x6000) return ID_NO;
    return ti_identify(b, a, 0x2270);
}

static int ina260_init(sens_io_t *io, sens_slot_t *s)
{
    int r = ti_check(io, s, 0x2270);
    if (r != R_OK) return r;
    /* averages of 16, 1.1 ms per conversion, current and bus continuous */
    return si2c_wreg16be(io->i2c, s->pub.addr, 0x00, 0x6527) ? R_OK : R_NACK;
}

static int ina260_read(sens_io_t *io, sens_slot_t *s)
{
    uint16_t i, v, p;
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    if (!si2c_rreg16be(b, a, 0x01, &i) || !si2c_rreg16be(b, a, 0x02, &v) || !si2c_rreg16be(b, a, 0x03, &p)) return R_NACK;
    sens_setv(s, 0, "bus", AOS_SQ_VOLT, (float)v * 0.00125f);
    sens_setv(s, 1, "current", AOS_SQ_CURR, (float)(int16_t)i * 0.00125f);
    sens_setv(s, 2, "power", AOS_SQ_POWER, (float)p * 0.010f);
    return R_OK;
}

const sens_drv_t DRV_INA260 = {
    .prio = SP_REG,
    .pub = { .id = "ina260", .name = "INA260", .addrs = { 0x40, 0x41, 0x44, 0x45 }, .naddr = 4 },
    .probe = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 }, .nprobe = 8,
    .identify = ina260_identify, .init = ina260_init, .read = ina260_read,
};
const sens_drv_t DRV_INA260_HI = {
    .prio = SP_REG,
    .pub = { .id = "ina260", .name = "INA260", .addrs = { 0x48, 0x49, 0x4C, 0x4D }, .naddr = 4 },
    .flags = SF_HIDDEN,
    .probe = { 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F }, .nprobe = 8,
    .identify = ina260_identify_hi, .init = ina260_init, .read = ina260_read,
};

/* ---- INA3221: three channels, shunt and bus each ----
 * Channel n's shunt at 0x01 + 2n, its bus at 0x02 + 2n, both 13 bits in
 * 15..3: 40 µV and 8 mV. One shunt= for the three, or shunt1= shunt2=
 * shunt3= each (mΩ). */

static int ina3221_identify(aos_io_i2c_t *b, uint8_t a) { return ti_identify(b, a, 0x3220); }

static int ina3221_init(sens_io_t *io, sens_slot_t *s)
{
    int r = ti_check(io, s, 0x3220);
    if (r != R_OK) return r;
    /* the three channels, averages of 16, 1.1 ms each, continuous: 106 ms a cycle */
    return si2c_wreg16be(io->i2c, s->pub.addr, 0x00, 0x7527) ? R_OK : R_NACK;
}

static int ina3221_read(sens_io_t *io, sens_slot_t *s)
{
    static const char *const KV[3] = { "v1", "v2", "v3" }, *const KI[3] = { "i1", "i2", "i3" };
    static const char *const KS[3] = { "shunt1", "shunt2", "shunt3" };
    for (int ch = 0; ch < 3; ch++) {
        uint16_t sh, bus;
        if (!si2c_rreg16be(io->i2c, s->pub.addr, (uint8_t)(0x01 + 2 * ch), &sh) ||
            !si2c_rreg16be(io->i2c, s->pub.addr, (uint8_t)(0x02 + 2 * ch), &bus)) return R_NACK;
        float vsh = (float)((int16_t)sh >> 3) * 40e-6f;
        sens_setv(s, 2 * ch, KV[ch], AOS_SQ_VOLT, (float)((int16_t)bus >> 3) * 0.008f);
        sens_setv(s, 2 * ch + 1, KI[ch], AOS_SQ_CURR, vsh / shunt_ohm(s, KS[ch]));
    }
    return R_OK;
}

const sens_drv_t DRV_INA3221 = {
    .prio = SP_REG,
    .pub = { .id = "ina3221", .name = "INA3221", .addrs = { 0x40, 0x41, 0x42, 0x43 }, .naddr = 4, .shunt = true },
    .identify = ina3221_identify, .init = ina3221_init, .read = ina3221_read,
};

/* ---- INA228 (20 bits) / INA238 and INA237 (16 bits) ----
 * The newer register map: manufacturer 0x5449 at 0x3E, device id at 0x3F
 * whose bits 15..4 are 0x228 or 0x238 (the INA237 says 0x238 too: the
 * module line tells them apart). ADCRANGE stays 0, ±163.84 mV.
 *   INA228: shunt 0x04 and bus 0x05 are 24 bits with the value in 23..4,
 *     312.5 nV and 195.3125 µV; die 0x06 × 7.8125 m°C; ENERGY 0x09, 40
 *     bits, counts 16 × 3.2 × CURRENT_LSB joules once SHUNT_CAL is set.
 *     With CURRENT_LSB = 163.84 mV / R / 2^19, SHUNT_CAL = 13107.2e6 ×
 *     CURRENT_LSB × R is 4096 for any shunt.
 *   INA238: shunt 0x04 × 5 µV, bus 0x05 × 3.125 mV, die 0x06 in 15..4 ×
 *     125 m°C; no energy register, so the energy is summed here.
 * The 0x3E/0x3F pointer writes are harmless to a PCF8591 too (bit 6, its
 * DAC output, is clear in both). */

typedef struct {
    bool     is238;
    double   wh;                    /* INA238: the energy summed here */
    uint32_t last_ms;
} ina228_t;
SPRIV_CHECK(ina228_t);

static int ina228_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t m, d;
    if (!si2c_rreg16be(b, a, 0x3E, &m) || m != 0x5449) return ID_NO;
    if (!si2c_rreg16be(b, a, 0x3F, &d)) return ID_NO;
    return (d >> 4) == 0x228 || (d >> 4) == 0x238 ? ID_YES : ID_NO;
}

static int ina228_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    uint16_t m = 0, d = 0;
    if (!si2c_rreg16be(b, a, 0x3E, &m) || !si2c_rreg16be(b, a, 0x3F, &d)) return R_NACK;
    if (m != 0x5449 || ((d >> 4) != 0x228 && (d >> 4) != 0x238)) { sens_err(s, "0x%04X", d); return R_ID; }
    ina228_t *p = SPRIV(s, ina228_t);
    memset(p, 0, sizeof *p);
    p->is238 = (d >> 4) == 0x238;
    char mod[24];
    model_of(s, mod, sizeof mod);
    sens_scpy(s->pub.chip, sizeof s->pub.chip, !p->is238 ? "INA228" : strstr(mod, "237") ? "INA237" : "INA238");
    /* ADCRANGE 0; on the INA228 also RSTACC: the energy starts from 0 */
    if (!si2c_wreg16be(b, a, 0x00, p->is238 ? 0x0000 : 0x4000)) return R_NACK;
    /* bus, shunt and die continuous, 1052 µs each, averages of 16: 50 ms */
    if (!si2c_wreg16be(b, a, 0x01, 0xFB6A)) return R_NACK;
    if (!p->is238 && !si2c_wreg16be(b, a, 0x02, 4096)) return R_NACK;
    p->last_ms = sens_now();
    return R_OK;
}

static int ina228_read(sens_io_t *io, sens_slot_t *s)
{
    ina228_t *p = SPRIV(s, ina228_t);
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, sh[3], bus[3], e[5];
    uint16_t die;
    float r = shunt_ohm(s, NULL), vsh, vbus, tdie;
    if (p->is238) {
        uint16_t s16, b16;
        if (!si2c_rreg16be(b, a, 0x04, &s16) || !si2c_rreg16be(b, a, 0x05, &b16) || !si2c_rreg16be(b, a, 0x06, &die)) return R_NACK;
        vsh = (float)(int16_t)s16 * 5e-6f;
        vbus = (float)(int16_t)b16 * 3.125e-3f;
        tdie = (float)((int16_t)die >> 4) * 0.125f;
    } else {
        if (!si2c_rreg(b, a, 0x04, sh, 3) || !si2c_rreg(b, a, 0x05, bus, 3) || !si2c_rreg16be(b, a, 0x06, &die) ||
            !si2c_rreg(b, a, 0x09, e, 5)) return R_NACK;
        vsh = (float)(sext(sens_be24(sh), 24) >> 4) * 312.5e-9f;
        vbus = (float)(sext(sens_be24(bus), 24) >> 4) * 195.3125e-6f;
        tdie = (float)(int16_t)die * 7.8125e-3f;
    }
    float i = vsh / r, w = vbus * i;
    uint32_t now = sens_now();
    if (p->is238) {
        p->wh += (double)w * (double)(now - p->last_ms) / 3600000.0;
    } else {
        uint64_t en = (uint64_t)e[0] << 32 | (uint64_t)e[1] << 24 | (uint64_t)e[2] << 16 | (uint64_t)e[3] << 8 | e[4];
        double lsb = 0.16384 / r / 524288.0;
        p->wh = (double)en * 16.0 * 3.2 * lsb / 3600.0;
    }
    p->last_ms = now;
    sens_setv(s, 0, "bus", AOS_SQ_VOLT, vbus);
    sens_setv(s, 1, "current", AOS_SQ_CURR, i);
    sens_setv(s, 2, "power", AOS_SQ_POWER, w);
    sens_setv(s, 3, "energy", AOS_SQ_ENERGY, (float)p->wh);
    sens_setv(s, 4, "die", AOS_SQ_TEMP, tdie);
    sens_setv(s, 5, "shunt", AOS_SQ_VOLT, vsh);
    return R_OK;
}

const sens_drv_t DRV_INA228 = {
    .prio = SP_REG,
    .pub = { .id = "ina228", .name = "INA228", .addrs = { 0x40, 0x41, 0x44, 0x45 }, .naddr = 4, .shunt = true },
    .probe = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 }, .nprobe = 8,
    .identify = ina228_identify, .init = ina228_init, .read = ina228_read,
};
const sens_drv_t DRV_INA228_HI = {
    .prio = SP_REG,
    .pub = { .id = "ina228", .name = "INA228", .addrs = { 0x48, 0x49, 0x4C, 0x4D }, .naddr = 4, .shunt = true },
    .flags = SF_HIDDEN,
    .probe = { 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F }, .nprobe = 8,
    .identify = ina228_identify, .init = ina228_init, .read = ina228_read,
};

/* ---- MAX17048 / MAX17049 / MAX17043: a lithium cell's fuel gauge ----
 * VCELL 0x02 × 78.125 µV (per cell on the two-cell MAX17049; the MAX17043's
 * 12 bits in 15..4 × 1.25 mV are the same thing), SOC 0x04 = high byte %
 * plus low / 256, CRATE 0x16 signed × 0.208 %/h (not on the MAX17043).
 * VERSION 0x08 is 0x001x on the MAX17048/49 only. Nothing is written: the
 * gauge's model learns from the cell, and a reset would throw that away. */

typedef struct { bool crate; } max1704x_t;
SPRIV_CHECK(max1704x_t);

static int max17048_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t v;
    return si2c_rreg16be(b, a, 0x08, &v) && (v & 0xFFF0) == 0x0010 ? ID_YES : ID_NO;
}

static int max17048_init(sens_io_t *io, sens_slot_t *s)
{
    uint16_t v = 0;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x08, &v)) return R_NACK;
    char mod[24];
    model_of(s, mod, sizeof mod);
    max1704x_t *p = SPRIV(s, max1704x_t);
    p->crate = (v & 0xFFF0) == 0x0010;
    if (strstr(mod, "17043") || strstr(mod, "17044")) {
        /* no version to check: an old MAX17043 says 0x0002 or 0x0003 */
        sens_scpy(s->pub.chip, sizeof s->pub.chip, p->crate ? "MAX17048" : "MAX17043");
    } else {
        if (!p->crate) { sens_err(s, "0x%04X", v); return R_ID; }
        sens_scpy(s->pub.chip, sizeof s->pub.chip, strstr(mod, "17049") ? "MAX17049" : "MAX17048");
    }
    return R_OK;
}

static int max17048_read(sens_io_t *io, sens_slot_t *s)
{
    const max1704x_t *p = SPRIV(s, max1704x_t);
    uint16_t vc, soc, cr = 0;
    if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x02, &vc) || !si2c_rreg16be(io->i2c, s->pub.addr, 0x04, &soc)) return R_NACK;
    if (p->crate && !si2c_rreg16be(io->i2c, s->pub.addr, 0x16, &cr)) return R_NACK;
    sens_setv(s, 0, "soc", AOS_SQ_PERCENT, (float)soc / 256.0f);
    sens_setv(s, 1, "cell", AOS_SQ_VOLT, (float)vc * 78.125e-6f);
    if (p->crate) sens_setv(s, 2, "rate", AOS_SQ_PERCENT, (float)(int16_t)cr * 0.208f);
    return R_OK;
}

const sens_drv_t DRV_MAX17048 = {
    .prio = SP_REG,
    .pub = { .id = "max17048", .name = "MAX17048", .addrs = { 0x36 }, .naddr = 1 },
    .identify = max17048_identify, .init = max17048_init, .read = max17048_read,
};

/* ---- ADS1015: the ADS1115's 12-bit sibling ----
 * The same four registers, the same power-on values (config 0x8583,
 * thresholds 0x8000/0x7FFF), the result left-justified in 15..4 with 0 in
 * 3..0, and DR=100 is 1600 SPS instead of 128. In its power-on state
 * (single shot, the conversion register 0) nothing a read can see tells it
 * from an ADS1115; the difference shows only after a conversion, and
 * starting one is a write. So identify() proves it only when the chip is
 * already converting continuously: several results, at least two of them
 * different, all with 3..0 at 0. Otherwise the ADS1115 driver takes it,
 * and reads it right (the same left-justified scale); "module ads1015"
 * names it. A declared one that turns out to fill 3..0 is an ADS1115,
 * and says so. */

static int ads1015_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t lo, hi, cfg, v[6];
    if (!si2c_rreg16be(b, a, 0x02, &lo) || !si2c_rreg16be(b, a, 0x03, &hi) || lo != 0x8000 || hi != 0x7FFF) return ID_NO;
    if (!si2c_rreg16be(b, a, 0x01, &cfg) || (cfg & 0x0100)) return ID_NO;         /* single shot: nothing to see */
    bool differ = false;
    for (int i = 0; i < 6; i++) {
        if (i) aos_hal_sleep_ms(2);
        if (!si2c_rreg16be(b, a, 0x00, &v[i]) || (v[i] & 0x000F)) return ID_NO;
        if (i && v[i] != v[0]) differ = true;
    }
    return differ ? ID_YES : ID_NO;
}

static int ads1015_read(sens_io_t *io, sens_slot_t *s)
{
    static const int32_t FSR[6] = { 6144, 4096, 2048, 1024, 512, 256 };
    static const char *const KEY[4] = { "a0", "a1", "a2", "a3" };
    int pga = 1;
    for (int i = 0; i < 6; i++) if (s->gain_mv == FSR[i]) pga = i;
    bool full = false;
    for (int ch = 0; ch < 4; ch++) {
        /* one shot, AINx against GND, 1600 SPS (0.63 ms), comparator off */
        uint16_t cfg = (uint16_t)(0x8000 | (4 + ch) << 12 | pga << 9 | 0x0100 | 4 << 5 | 0x0003);
        if (!si2c_wreg16be(io->i2c, s->pub.addr, 0x01, cfg)) return R_NACK;
        aos_hal_sleep_ms(1);
        uint16_t st = 0;
        for (int t = 0; t < 12; t++) {              /* an ADS1115 in its place needs 8 ms */
            if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x01, &st)) return R_NACK;
            if (st & 0x8000) break;
            aos_hal_sleep_ms(1);
        }
        if (!(st & 0x8000)) return R_NOTREADY;
        uint16_t raw;
        if (!si2c_rreg16be(io->i2c, s->pub.addr, 0x00, &raw)) return R_NACK;
        if (raw & 0x000F) full = true;
        /* (int16 >> 4) × FSR / 2048 on an ADS1015; the 16-bit form is the
         * same number there and right on an ADS1115 too */
        sens_setv(s, ch, KEY[ch], AOS_SQ_VOLT, (float)(int16_t)raw * (float)FSR[pga] / 32768.0f / 1000.0f);
    }
    if (full) sens_scpy(s->pub.chip, sizeof s->pub.chip, "ADS1115");
    return R_OK;
}

const sens_drv_t DRV_ADS1015 = {
    .prio = 5,                      /* before the ADS1115's threshold check, which an ADS1015 passes too */
    .pub = { .id = "ads1015", .name = "ADS1015", .addrs = { 0x48, 0x49, 0x4A, 0x4B }, .naddr = 4, .gain = true },
    .identify = ads1015_identify, .read = ads1015_read,
};

/* ---- PCF8591 (the YL-40 module): four 8-bit inputs and a DAC ----
 * A control byte selects the input (bits 1..0; 5..4 = 00, four single
 * ended; bit 6 the DAC output). A read starts a conversion of that input
 * at each byte's acknowledge, while sending the previous result: the
 * first byte is stale, the second is the input asked for. Without
 * auto-increment, one write and a 2-byte read per input, as Adafruit's
 * library does. Any write is its control byte, so it is declared only.
 * vref= is the reference (VCC on a YL-40, 3.3 V); dac=0..255 keeps the
 * analog output on with that value (otherwise it is off). On a YL-40,
 * the LDR, the thermistor and the pot sit on three inputs through their
 * jumpers, in an order that changes from maker to maker (one on the bench
 * had the pot on AIN0 and the LDR on AIN1). The PCF8591 is a 100 kHz part
 * and converts on SCL's clock: it is read at 100 kHz (SF_SLOW). */

static int pcf8591_read(sens_io_t *io, sens_slot_t *s)
{
    static const char *const KEY[4] = { "a0", "a1", "a2", "a3" };
    float vref = sens_arg_f(s, "vref", 3.3f);
    int32_t dac = sens_arg_int(s, "dac", -1);
    for (int ch = 0; ch < 4; ch++) {
        uint8_t w[2] = { (uint8_t)(ch | (dac >= 0 ? 0x40 : 0)), (uint8_t)(dac < 0 ? 0 : dac > 255 ? 255 : dac) };
        uint8_t d[2];
        if (!si2c_wr(io->i2c, s->pub.addr, w, dac >= 0 ? 2 : 1) || !si2c_rd(io->i2c, s->pub.addr, d, 2)) return R_NACK;
        sens_setv(s, ch, KEY[ch], AOS_SQ_VOLT, (float)d[1] * vref / 256.0f);
    }
    return R_OK;
}

const sens_drv_t DRV_PCF8591 = {
    .prio = SP_REG,
    .pub = { .id = "pcf8591", .name = "PCF8591", .addrs = { 0x48, 0x49, 0x4A, 0x4B }, .naddr = 4 },
    /* 100 kHz at most, and its converter runs on SCL: at 400 kHz the
     * readings came out anywhere on the board (2026-10-19) */
    .flags = SF_NOPROBE | SF_SLOW,
    .read = pcf8591_read,
};

/* ---- MCP3421..MCP3428: delta-sigma, 12 to 18 bits, 1 to 4 inputs ----
 * No registers: a write is the configuration byte (bit 7 starts a one-shot
 * conversion, 6..5 the channel, 4 continuous, 3..2 the rate: 240 SPS 12
 * bits, 60 SPS 14, 15 SPS 16, 3.75 SPS 18; 1..0 the PGA), and a read gives
 * the result (3 bytes at 18 bits, else 2) and the configuration again,
 * whose bit 7 is 0 when the result is new. LSB = 4.096 V / 2^bits / PGA.
 * Any write reconfigures it: declared only. The model from model=, chip=
 * or the name: 3421/3425 one input, 3422/3423/3426/3427 two, 3424/3428
 * four (3425..3428 stop at 16 bits). bits= 12/14/16/18 (18 by default);
 * gain= the full scale in mV, 2048 1024 512 256. An 18-bit conversion
 * takes 267 ms: it is started in one read and collected in the next, a
 * channel a round. */

typedef struct {
    uint8_t  nch, rate, pga, bits;
    uint8_t  next;                  /* the channel to convert next */
    bool     pending;               /* a conversion of next is running */
    uint8_t  have;                  /* channels with a value */
    uint32_t t0;
    float    v[4];
} mcp342x_t;
SPRIV_CHECK(mcp342x_t);

static int mcp342x_init(sens_io_t *io, sens_slot_t *s)
{
    mcp342x_t *p = SPRIV(s, mcp342x_t);
    memset(p, 0, sizeof *p);
    char mod[24];
    model_of(s, mod, sizeof mod);
    const char *m = strstr(mod, "342");
    int k = m && m[3] >= '1' && m[3] <= '8' ? m[3] - '0' : 1;
    static const uint8_t NCH[9] = { 1, 1, 2, 2, 4, 1, 2, 2, 4 };
    p->nch = NCH[k];
    int bits = sens_arg_int(s, "bits", k >= 5 ? 16 : 18);
    if (k >= 5 && bits > 16) bits = 16;
    p->rate = bits >= 18 ? 3 : bits >= 16 ? 2 : bits >= 14 ? 1 : 0;
    p->bits = (uint8_t)(12 + 2 * p->rate);
    int32_t g = s->gain_mv;
    p->pga = g <= 256 ? 3 : g <= 512 ? 2 : g <= 1024 ? 1 : 0;
    char name[12];
    snprintf(name, sizeof name, "MCP342%d", k);
    sens_scpy(s->pub.chip, sizeof s->pub.chip, name);
    /* only a read: is anybody there */
    uint8_t d[4];
    return si2c_rd(io->i2c, s->pub.addr, d, 4) ? R_OK : R_NACK;
}

static uint32_t mcp342x_ms(const mcp342x_t *p)
{
    static const uint16_t MS[4] = { 5, 17, 67, 267 };
    return MS[p->rate];
}

/* The running conversion's result into v[]: R_OK, R_NOTREADY or R_NACK */
static int mcp342x_collect(sens_io_t *io, sens_slot_t *s, mcp342x_t *p)
{
    uint8_t d[4];
    int n = p->bits == 18 ? 3 : 2;
    if (!si2c_rd(io->i2c, s->pub.addr, d, (size_t)n + 1)) return R_NACK;
    uint8_t cfg = d[n];
    if (cfg & 0x80) return R_NOTREADY;
    if (((cfg >> 5) & 3) != p->next) { p->pending = false; return R_NOTREADY; }    /* somebody else's */
    int32_t code = n == 3 ? sext(sens_be24(d), 18) : sext(sens_be16(d), p->bits);
    p->v[p->next] = (float)code * 4.096f / (float)(1u << p->bits) / (float)(1 << p->pga);
    p->have |= (uint8_t)(1 << p->next);
    p->next = (uint8_t)((p->next + 1) % p->nch);
    p->pending = false;
    return R_OK;
}

static bool mcp342x_start(sens_io_t *io, sens_slot_t *s, mcp342x_t *p)
{
    uint8_t c = (uint8_t)(0x80 | p->next << 5 | p->rate << 2 | p->pga);
    if (!si2c_wr(io->i2c, s->pub.addr, &c, 1)) return false;
    p->pending = true;
    p->t0 = sens_now();
    return true;
}

static int mcp342x_read(sens_io_t *io, sens_slot_t *s)
{
    static const char *const KEY[4] = { "a0", "a1", "a2", "a3" };
    mcp342x_t *p = SPRIV(s, mcp342x_t);
    uint32_t t = sens_now(), ms = mcp342x_ms(p);
    /* the internal clock may run 30 % slow (datasheet, data rate min) */
    uint32_t most = ms * 3 / 2 + 4;
    if (p->pending) {
        int r = mcp342x_collect(io, s, p);
        if (r == R_NACK) return r;
        if (r == R_NOTREADY && p->pending && t - p->t0 > 2 * most + 200) p->pending = false;   /* lost: again */
    }
    /* the fast rates: convert and collect here while the round has time */
    while (!p->pending && sens_now() - t + most <= 120) {
        if (!mcp342x_start(io, s, p)) return R_NACK;
        aos_hal_sleep_ms(ms);
        int r = R_NOTREADY;
        uint32_t step = ms / 8 > 2 ? ms / 8 : 2;
        while (p->pending && (r = mcp342x_collect(io, s, p)) == R_NOTREADY && p->pending && sens_now() - p->t0 < most)
            aos_hal_sleep_ms(step);
        if (r == R_NACK) return r;
        if (r != R_OK) break;
        if (p->next == 0 && p->have == (1 << p->nch) - 1) break;       /* every input once this round */
    }
    /* the slow ones: started now, collected next round */
    if (!p->pending && !mcp342x_start(io, s, p)) return R_NACK;
    for (int ch = 0; ch < p->nch; ch++) if (p->have & (1 << ch)) sens_setv(s, ch, KEY[ch], AOS_SQ_VOLT, p->v[ch]);
    return p->have == (1 << p->nch) - 1 ? R_OK : R_NOTREADY;
}

const sens_drv_t DRV_MCP3421 = {
    .prio = SP_LAST,
    .pub = { .id = "mcp3421", .name = "MCP3421", .addrs = { 0x68, 0x69, 0x6A, 0x6B }, .naddr = 4, .gain = true },
    .flags = SF_NOPROBE,
    .init = mcp342x_init, .read = mcp342x_read,
};

/* ---- NAU7802: a 24-bit ADC for a load cell ----
 * Started as SparkFun's library does (datasheet 9.1): reset (RR), power
 * up digital and analog (PUD, PUA) and wait for PUR, internal LDO at 3.3 V
 * and AVDDS, PGA 128, 10 SPS, CLK_CHP off (0x15 |= 0x30), the 330 pF cap on
 * channel 2 (0x1C bit 7), LDOMODE clear (0x1B bit 6), cycle start (CS).
 * The LDO settles in ~200 ms; then an internal offset calibration (CALS),
 * a round each, before reading. ADCO 0x12..0x14, 24 bits signed, when
 * CR (PU_CTRL bit 5) is set. REVISION 0x1F reads 0xF in its low nibble.
 * scale= counts per gram and tare= counts make the weight; without
 * scale= only the raw count. */

typedef struct { uint8_t stage; uint32_t t; } nau7802_t;
SPRIV_CHECK(nau7802_t);

static int nau_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t rev, pu, m;
    if (!si2c_rreg8(b, a, 0x1F, &rev) || (rev & 0x0F) != 0x0F) return ID_NO;
    /* PU_CTRL is never 0xFF (RR resets it): not a bus that reads all ones */
    if (!si2c_rreg8(b, a, 0x00, &pu) || pu == 0xFF) return ID_NO;
    /* a CAP1188 at 0x2A has 0x2F at 0x1F: its manufacturer id 0x5D at 0xFE */
    if (si2c_rreg8(b, a, 0xFE, &m) && m == 0x5D) return ID_NO;
    return ID_YES;
}

static bool nau_upd(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t clr, uint8_t set)
{
    uint8_t v;
    return si2c_rreg8(b, a, reg, &v) && si2c_wreg8(b, a, reg, (uint8_t)((v & ~clr) | set));
}

static int nau_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, v = 0;
    if (!si2c_rreg8(b, a, 0x1F, &v)) return R_NACK;
    if ((v & 0x0F) != 0x0F) { sens_err(s, "0x%02X", v); return R_ID; }
    if (!si2c_wreg8(b, a, 0x00, 0x01)) return R_NACK;              /* RR */
    aos_hal_sleep_ms(1);
    if (!si2c_wreg8(b, a, 0x00, 0x00) || !si2c_wreg8(b, a, 0x00, 0x06)) return R_NACK;    /* PUD, PUA */
    for (int i = 0; ; i++) {
        if (!si2c_rreg8(b, a, 0x00, &v)) return R_NACK;
        if (v & 0x08) break;                                       /* PUR */
        if (i >= 20) return R_NOTREADY;
        aos_hal_sleep_ms(1);
    }
    if (!si2c_wreg8(b, a, 0x01, 4 << 3 | 7)) return R_NACK;        /* LDO 3.3 V, gain 128 */
    if (!si2c_wreg8(b, a, 0x00, 0x86)) return R_NACK;              /* AVDDS: the internal LDO */
    if (!si2c_wreg8(b, a, 0x02, 0x00)) return R_NACK;              /* 10 SPS, channel 1 */
    if (!nau_upd(b, a, 0x15, 0, 0x30) || !nau_upd(b, a, 0x1C, 0, 0x80) || !nau_upd(b, a, 0x1B, 0x40, 0)) return R_NACK;
    if (!si2c_wreg8(b, a, 0x00, 0x96)) return R_NACK;              /* CS */
    nau7802_t *p = SPRIV(s, nau7802_t);
    p->stage = 0;
    p->t = sens_now();
    return R_OK;
}

static int nau_read(sens_io_t *io, sens_slot_t *s)
{
    nau7802_t *p = SPRIV(s, nau7802_t);
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, v;
    if (!si2c_rreg8(b, a, 0x00, &v)) return R_NACK;
    if (!(v & 0x08)) return nau_init(io, s) == R_OK ? R_NOTREADY : R_NACK;   /* lost power: again */
    if (p->stage == 0) {
        if (sens_now() - p->t < 250) return R_NOTREADY;
        if (!nau_upd(b, a, 0x02, 0x03, 0x04)) return R_NACK;       /* CALS, internal offset */
        p->stage = 1;
        p->t = sens_now();
        return R_NOTREADY;
    }
    if (p->stage == 1) {
        if (!si2c_rreg8(b, a, 0x02, &v)) return R_NACK;
        if ((v & 0x04) && sens_now() - p->t < 3000) return R_NOTREADY;
        p->stage = 2;                   /* a CAL_ERR or a slow one: it reads anyway */
    }
    for (int i = 0; ; i++) {
        if (!si2c_rreg8(b, a, 0x00, &v)) return R_NACK;
        if (v & 0x20) break;                                       /* CR */
        if (i >= 12) return R_NOTREADY;
        aos_hal_sleep_ms(10);
    }
    uint8_t d[3];
    if (!si2c_rreg(b, a, 0x12, d, 3)) return R_NACK;
    int32_t raw = sext(sens_be24(d), 24);
    float scale = sens_arg_f(s, "scale", 0.0f), tare = sens_arg_f(s, "tare", 0.0f);
    int k = 0;
    if (scale != 0.0f) sens_setv(s, k++, "weight", AOS_SQ_MASS, ((float)raw - tare) / scale);
    sens_setv(s, k, "raw", AOS_SQ_RAW, (float)raw);
    return R_OK;
}

const sens_drv_t DRV_NAU7802 = {
    .prio = SP_REG,
    .pub = { .id = "nau7802", .name = "NAU7802", .addrs = { 0x2A }, .naddr = 1 },
    .identify = nau_identify, .init = nau_init, .read = nau_read,
};

/* ---- MCP9600 / MCP9601: a thermocouple amplifier that linearises ----
 * Hot junction 0x00 and cold junction 0x02, signed / 16 °C. STATUS 0x04:
 * bit 4 is "input range" on the MCP9600 (an open thermocouple floats out of
 * range) and "open circuit" on the MCP9601, bit 5 the MCP9601's short.
 * Sensor configuration 0x05: type in 6..4 (K J T N S E B R), filter 2..0;
 * bits 7 and 3 read 0. Device configuration 0x06 = 0: 0.0625 °C cold
 * junction, 18 bits (~320 ms), normal mode. Device id 0x20: high byte 0x40
 * MCP9600, 0x41 MCP9601. It stretches the clock: 100 kHz. type=K by
 * default, filter=0..7. Autodetect looks at 0x60 and 0x63..0x67: 0x61 and
 * 0x62 are the SCD30's and the SCD4x's, which a stray pointer write could
 * upset; an MCP9600 there is declared. */

static int mcp9600_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id[2], cfg;
    if (!si2c_rreg(b, a, 0x20, id, 2) || (id[0] != 0x40 && id[0] != 0x41)) return ID_NO;
    if (!si2c_rreg8(b, a, 0x05, &cfg) || (cfg & 0x88)) return ID_NO;
    /* an MCP4725 DAC (0x60..0x67) ignores the pointer and answers its
     * status byte, 0x40 while it writes its EEPROM: its "cold junction"
     * would be over 1000 °C */
    uint16_t tc;
    if (!si2c_rreg16be(b, a, 0x02, &tc) || (int16_t)tc < -40 * 16 || (int16_t)tc > 125 * 16) return ID_NO;
    return ID_YES;
}

typedef struct { bool is9601; } mcp9600_t;
SPRIV_CHECK(mcp9600_t);

static int mcp9600_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id[2];
    if (!si2c_rreg(b, a, 0x20, id, 2)) return R_NACK;
    if (id[0] != 0x40 && id[0] != 0x41) { sens_err(s, "0x%02X%02X", id[0], id[1]); return R_ID; }
    SPRIV(s, mcp9600_t)->is9601 = id[0] == 0x41;
    sens_scpy(s->pub.chip, sizeof s->pub.chip, id[0] == 0x41 ? "MCP9601" : "MCP9600");
    char ty[8] = "K";
    sens_arg_str(s, "type", ty, sizeof ty);
    const char *T = "KJTNSEBR", *f = strchr(T, toupper((unsigned char)ty[0]));
    int type = f && *f ? (int)(f - T) : 0;
    int filt = sens_arg_int(s, "filter", 0) & 7;
    if (!si2c_wreg8(b, a, 0x05, (uint8_t)(type << 4 | filt))) return R_NACK;
    if (!si2c_wreg8(b, a, 0x06, 0x00)) return R_NACK;
    return R_OK;
}

static int mcp9600_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, st;
    uint16_t th, tc;
    if (!si2c_rreg8(b, a, 0x04, &st) || !si2c_rreg16be(b, a, 0x00, &th) || !si2c_rreg16be(b, a, 0x02, &tc)) return R_NACK;
    if (st & 0x10) return R_OPEN;
    if ((st & 0x20) && SPRIV(s, mcp9600_t)->is9601) return R_RANGE;     /* shorted */
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)(int16_t)th / 16.0f);
    sens_setv(s, 1, "cj", AOS_SQ_TEMP, (float)(int16_t)tc / 16.0f);
    return R_OK;
}

const sens_drv_t DRV_MCP9600 = {
    .prio = SP_REG,
    .pub = { .id = "mcp9600", .name = "MCP9600", .addrs = { 0x67, 0x60, 0x65, 0x66 }, .naddr = 4 },
    .flags = SF_SLOW,
    /* not 0x61/0x62, the SCD30's and SCD4x's: declared there */
    .probe = { 0x60, 0x63, 0x64, 0x65, 0x66, 0x67 }, .nprobe = 6,
    .identify = mcp9600_identify, .init = mcp9600_init, .read = mcp9600_read,
};

const sens_drv_t *const SENS_G_POWER[] = {
    &DRV_INA260, &DRV_INA260_HI, &DRV_INA3221, &DRV_INA228, &DRV_INA228_HI, &DRV_MAX17048,
    &DRV_ADS1015, &DRV_PCF8591, &DRV_MCP3421, &DRV_NAU7802, &DRV_MCP9600,
    NULL,
};

const sens_alias_t SENS_A_POWER[] = {
    { "ina238", "ina228" }, { "ina237", "ina228" },
    { "max17043", "max17048" }, { "max17049", "max17048" }, { "max17044", "max17048" },
    { "yl40", "pcf8591" }, { "yl-40", "pcf8591" },
    { "mcp3422", "mcp3421" }, { "mcp3423", "mcp3421" }, { "mcp3424", "mcp3421" },
    { "mcp3425", "mcp3421" }, { "mcp3426", "mcp3421" }, { "mcp3427", "mcp3421" }, { "mcp3428", "mcp3421" },
    { "mcp9601", "mcp9600" },
    { NULL, NULL },
};
