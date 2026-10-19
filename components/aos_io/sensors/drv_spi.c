/*
 * P4OS - Sensor drivers: SPI sensors (sens_int.h, docs/SENSORS.md).
 *
 * None of them can be found: an SPI chip has no address to knock at, so
 * they are all declared ("module max31865 spi.a cs=46"), and the service
 * opens the port with each one's own chip select, mode and clock for the
 * length of one read. MISO has the internal pull-up, so a chip that is not
 * there reads all ones; every driver checks for that and for the bits its
 * chip always sends as zero.
 *
 * MAX31855 and MAX31856: thermocouples. MAX31865: a PT100/PT1000. MCP3008
 * and MCP3208: 8-channel ADCs, 10 and 12 bits.
 */
#include "sens_int.h"

#include <ctype.h>

/* A module named after a smaller sibling ("module pt1000 spi.a",
 * "mcp3204_b") takes that sibling's defaults: the alias maps to the same
 * driver, and the name is all the driver has left of it. */
static bool named(const sens_slot_t *s, const char *base)
{
    return !strncmp(s->pub.name, base, strlen(base));
}

/* ---- MAX31855: a thermocouple, 32 bits read-only (Maxim datasheet, Table 2) ----
 * D31..D18 the thermocouple in quarters of a degree (14 bits, signed), D16
 * a fault, D15..D4 the cold junction in sixteenths (12 bits, signed), D2
 * short to VCC, D1 short to GND, D0 open. D17 and D3 are always 0. Mode 0,
 * at most 5 MHz; it converts all the time (100 ms) and a read only stops it
 * for the length of the read. The temperature is the chip's linear 41.276
 * µV/°C reading, as Adafruit and ESPHome give it (no NIST correction): good
 * for a K type, off by degrees for the other types the part numbers make. */
static int max31855_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t rx[4];
    if (!aos_io_spi_xfer(io->spi, NULL, rx, 4, SENS_XFER_MS)) return R_NACK;
    uint32_t w = (uint32_t)rx[0] << 24 | (uint32_t)rx[1] << 16 | (uint32_t)rx[2] << 8 | rx[3];
    if (w == 0xFFFFFFFFu) {             /* MISO's pull-up: nobody drove it */
        sens_scpy(s->pub.err, sizeof s->pub.err, "spi");
        return R_NACK;
    }
    /* the reserved bits, and a fault flag with no fault (or the reverse) */
    if ((w & 0x00020008u) || !(w & 0x10000u) != !(w & 7)) {
        sens_err(s, "%08lX", (unsigned long)w);
        return R_ID;
    }
    if (w & 1) return R_OPEN;
    if (w & 6) {
        sens_err(s, "%s", w & 4 ? "vcc" : "gnd");
        return R_OPEN;
    }
    int32_t tc = (int32_t)w >> 18;                  /* arithmetic: the sign comes along */
    int32_t cj = (int32_t)(int16_t)(uint16_t)(w & 0xFFF0u) >> 4;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)tc * 0.25f);
    sens_setv(s, 1, "cj", AOS_SQ_TEMP, (float)cj * 0.0625f);
    return R_OK;
}

const sens_drv_t DRV_MAX31855 = {
    .prio = SP_REG,
    .cat = SC(TEMP),
    .pub = { "max31855", "MAX31855", { 0 }, 0, false, false, true, false, AOS_SENSOR_BUS_SPI, 0 },
    .flags = SF_NOPROBE, .spi_mode = 0, .spi_hz = 1000000,
    .read = max31855_read,
};

/* ---- MAX31856: a thermocouple of any type, by registers (Maxim datasheet
 * 19-7726, Table 6; Adafruit_MAX31856) ----
 * Mode 1 or 3, at most 5 MHz. A register is read sending its address and
 * written sending the address | 0x80, and the address goes on by itself
 * through 0x0F (and wraps to 0x00), so one read of 16 bytes gives them all.
 * CR0 0x00: b7 automatic conversion, b5:4 open-thermocouple detection, b0
 * the 50 Hz filter (changed only while not converting). CR1 0x01: b6:4
 * averaging (1, 2, 4, 8, 16), b3:0 the type (B E J K N R S T = 0..7).
 * CJTH:L 0x0A-0x0B the cold junction, int16 / 256 °C. LTCBH:M:L 0x0C-0x0E
 * the thermocouple, 19 bits signed from b23, 1/128 °C. SR 0x0F the faults.
 * No ID register: init() writes CR1 and reads it back. */

enum { M56_CR0 = 0x00, M56_CR1 = 0x01, M56_CJTH = 0x0A, M56_LTCBH = 0x0C, M56_SR = 0x0F };

typedef struct {
    uint8_t cr0, cr1;                   /* what init() wrote: read() checks they are still there */
} m56_t;
SPRIV_CHECK(m56_t);

static bool reg_wr(aos_io_spi_t *h, uint8_t reg, uint8_t v)
{
    uint8_t w[2] = { (uint8_t)(reg | 0x80), v };
    return aos_io_spi_xfer(h, w, NULL, 2, SENS_XFER_MS);
}

static bool reg_rd(aos_io_spi_t *h, uint8_t reg, uint8_t *r, size_t n)
{
    uint8_t tx[17], rx[17];
    if (n + 1 > sizeof tx) return false;
    memset(tx, 0xFF, sizeof tx);
    tx[0] = reg & 0x7F;
    if (!aos_io_spi_xfer(h, tx, rx, n + 1, SENS_XFER_MS)) return false;
    memcpy(r, rx + 1, n);
    return true;
}

/* All ones on the bytes that are never all ones on the chip: MISO's pull-up */
static bool all_ff(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) if (d[i] != 0xFF) return false;
    return true;
}

static int m56_setup(sens_io_t *io, sens_slot_t *s)
{
    static const char TYPES[] = "BEJKNRST";
    char t[4];
    int type = 3;                                   /* K */
    if (sens_arg_str(s, "type", t, sizeof t) && t[0]) {
        const char *p = strchr(TYPES, toupper((unsigned char)t[0]));
        if (!p || t[1]) { sens_err(s, "type=%s", t); return R_RANGE; }
        type = (int)(p - TYPES);
    }
    int avg = sens_arg_int(s, "avg", 4), a = 0;
    while (a < 4 && (1 << a) < avg) a++;
    m56_t *k = SPRIV(s, m56_t);
    k->cr1 = (uint8_t)(a << 4 | type);
    /* open detection on (fault mode 01, for a probe under 5 kΩ), the mains
     * filter by mains= (50 unless 60), all while conversions are off */
    k->cr0 = (uint8_t)(0x10 | (sens_arg_int(s, "mains", 50) == 60 ? 0x00 : 0x01));
    if (!reg_wr(io->spi, M56_CR0, k->cr0) || !reg_wr(io->spi, M56_CR1, k->cr1)) return R_NACK;
    uint8_t rb[2];
    if (!reg_rd(io->spi, M56_CR0, rb, 2)) return R_NACK;
    if (all_ff(rb, 2)) { sens_scpy(s->pub.err, sizeof s->pub.err, "spi"); return R_NACK; }
    if (rb[0] != k->cr0 || (rb[1] & 0x7F) != k->cr1) { sens_err(s, "%02X%02X", rb[0], rb[1]); return R_ID; }
    k->cr0 |= 0x80;                                 /* now convert, continuously */
    return reg_wr(io->spi, M56_CR0, k->cr0) ? R_OK : R_NACK;
}

static int max31856_init(sens_io_t *io, sens_slot_t *s)
{
    return m56_setup(io, s);
}

static int max31856_read(sens_io_t *io, sens_slot_t *s)
{
    const m56_t *k = SPRIV(s, m56_t);
    uint8_t r[16];
    if (!reg_rd(io->spi, M56_CR0, r, 16)) return R_NACK;
    if (all_ff(r, 16)) { sens_scpy(s->pub.err, sizeof s->pub.err, "spi"); return R_NACK; }
    /* it lost power, or something else wrote it: set it up again */
    if (r[M56_CR0] != k->cr0 || (r[M56_CR1] & 0x7F) != k->cr1) {
        int e = m56_setup(io, s);
        return e == R_OK ? R_NOTREADY : e;
    }
    /* the first conversion: 185 ms at 50 Hz, plus 40 ms for each average */
    if (sens_now() - s->init_ms < 200u + 40u * (1u << ((k->cr1 >> 4) & 7))) return R_NOTREADY;
    uint8_t sr = r[M56_SR];
    if (sr & 0x01) return R_OPEN;                   /* the open-thermocouple test */
    if (sr & 0x02) { sens_scpy(s->pub.err, sizeof s->pub.err, "ovuv"); return R_OPEN; }   /* a lead on VCC or GND */
    if (sr & 0xC0) { sens_err(s, "%02X", sr); return R_RANGE; }                       /* out of the type's range */
    int32_t tc = (int32_t)((uint32_t)r[M56_LTCBH] << 24 | (uint32_t)r[M56_LTCBH + 1] << 16 | (uint32_t)r[M56_LTCBH + 2] << 8) >> 13;
    int16_t cj = (int16_t)sens_be16(r + M56_CJTH);
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, (float)tc / 128.0f);
    sens_setv(s, 1, "cj", AOS_SQ_TEMP, (float)cj / 256.0f);
    return R_OK;
}

const sens_drv_t DRV_MAX31856 = {
    .prio = SP_REG,
    .cat = SC(TEMP),
    .pub = { "max31856", "MAX31856", { 0 }, 0, false, false, true, false, AOS_SENSOR_BUS_SPI, 0 },
    .flags = SF_NOPROBE, .spi_mode = 1, .spi_hz = 1000000,
    .init = max31856_init, .read = max31856_read,
};

/* ---- MAX31865: a platinum RTD, PT100 or PT1000 (Maxim datasheet 19-6478,
 * Tables 1-2 and 7; Adafruit_MAX31865 for the conversion) ----
 * Mode 1 or 3, at most 5 MHz, registers as on the MAX31856 (address, or
 * address | 0x80 to write, auto-increment): config 0x00 (b7 bias, b6
 * automatic conversion, b4 three wires, b1 clear the faults, b0 the 50 Hz
 * filter, changed only while not converting), RTD 0x01-0x02 (15 bits, the
 * low bit a fault flag), thresholds 0x03-0x06, faults 0x07. One read of 8
 * bytes gives them all. R = code / 32768 * Rref: the reference resistor on
 * the board, 430 Ω for a PT100 on Adafruit's, 4300 for a PT1000; many
 * others carry 400/4000 (rref=). r0= is the RTD at 0 °C. A module named
 * pt1000 takes 1000 and 4300; wires= 2, 3 or 4 (as the board's jumpers). */

typedef struct {
    uint8_t cfg;                        /* what init() wrote, without the self-clearing bits */
    float   rref, r0;
} m65_t;
SPRIV_CHECK(m65_t);

static int m65_setup(sens_io_t *io, sens_slot_t *s)
{
    m65_t *k = SPRIV(s, m65_t);
    bool pt1000 = named(s, "pt1000");
    k->r0 = sens_arg_f(s, "r0", pt1000 ? 1000.0f : 100.0f);
    k->rref = sens_arg_f(s, "rref", k->r0 > 500.0f ? 4300.0f : 430.0f);
    if (k->r0 <= 0 || k->rref <= 0) { sens_err(s, "r0/rref"); return R_RANGE; }
    uint8_t base = (uint8_t)((sens_arg_int(s, "wires", 4) == 3 ? 0x10 : 0x00) |
                             (sens_arg_int(s, "mains", 50) == 60 ? 0x00 : 0x01));
    /* the filter first, with conversions off; then bias and automatic
     * conversion, clearing whatever fault was latched */
    if (!reg_wr(io->spi, 0x00, base)) return R_NACK;
    k->cfg = (uint8_t)(0xC0 | base);
    if (!reg_wr(io->spi, 0x00, (uint8_t)(k->cfg | 0x02))) return R_NACK;
    uint8_t rb;
    if (!reg_rd(io->spi, 0x00, &rb, 1)) return R_NACK;
    if (rb == 0xFF) { sens_scpy(s->pub.err, sizeof s->pub.err, "spi"); return R_NACK; }
    if ((rb & 0xD1) != k->cfg) { sens_err(s, "%02X", rb); return R_ID; }
    return R_OK;
}

static int max31865_init(sens_io_t *io, sens_slot_t *s)
{
    return m65_setup(io, s);
}

/* Callendar-Van Dusen, IEC 60751: R = R0 (1 + A T + B T²) from 0 °C up,
 * solved for T; below 0 Adafruit's fifth-order fit to the table, on the
 * resistance scaled to a PT100. */
static float rtd_temp(float r, float r0)
{
    const float A = 3.9083e-3f, B = -5.775e-7f;
    float t = (sqrtf(A * A - 4.0f * B + 4.0f * B / r0 * r) - A) / (2.0f * B);
    if (t >= 0) return t;
    float x = r / r0 * 100.0f, p = x;
    t = -242.02f + 2.2228f * p;
    p *= x; t += 2.5859e-3f * p;
    p *= x; t -= 4.8260e-6f * p;
    p *= x; t -= 2.8183e-8f * p;
    p *= x; t += 1.5243e-10f * p;
    return t;
}

static int max31865_read(sens_io_t *io, sens_slot_t *s)
{
    const m65_t *k = SPRIV(s, m65_t);
    uint8_t r[8];
    if (!reg_rd(io->spi, 0x00, r, 8)) return R_NACK;
    if (all_ff(r, 8)) { sens_scpy(s->pub.err, sizeof s->pub.err, "spi"); return R_NACK; }
    if ((r[0] & 0xD1) != k->cfg) {                  /* lost power: again */
        int e = m65_setup(io, s);
        return e == R_OK ? R_NOTREADY : e;
    }
    /* the bias settles and the first conversion takes 62.5 ms at 50 Hz */
    if (sens_now() - s->init_ms < 100) return R_NOTREADY;
    uint16_t w = sens_be16(r + 1);
    if (w & 1) {
        uint8_t f = r[7];
        reg_wr(io->spi, 0x00, (uint8_t)(k->cfg | 0x02));   /* clear it: the next read tells if it is still there */
        /* above the high threshold, or FORCE-/REFIN- found open: an open
         * RTD or wire; below the low one, a short; else the inputs went
         * over or under the supply */
        if (f & 0xB8) { sens_err(s, "rtd%02X", f); return R_OPEN; }
        if (f & 0x40) sens_scpy(s->pub.err, sizeof s->pub.err, "short");
        else sens_err(s, "%02X", f);
        return R_RANGE;
    }
    float ohm = (float)(w >> 1) / 32768.0f * k->rref;
    if (w >> 1 == 0) return R_NOTREADY;             /* no conversion yet */
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, rtd_temp(ohm, k->r0));
    sens_setv(s, 1, "rtd", AOS_SQ_OHM, ohm);
    return R_OK;
}

const sens_drv_t DRV_MAX31865 = {
    .prio = SP_REG,
    .cat = SC(TEMP),
    .pub = { "max31865", "MAX31865", { 0 }, 0, false, false, true, false, AOS_SENSOR_BUS_SPI, 0 },
    .flags = SF_NOPROBE, .spi_mode = 1, .spi_hz = 1000000,
    .init = max31865_init, .read = max31865_read,
};

/* ---- MCP3008 / MCP3004 and MCP3208 / MCP3204: successive-approximation
 * ADCs, a conversion per CS assertion (Microchip DS21295 and DS21298,
 * section 6.1) ----
 * The chip waits for a start bit, takes four configuration bits (single
 * ended, then the channel), samples for one more clock, sends a null bit
 * (always 0) and then the result, MSB first. Aligned so that the result
 * ends the third byte:
 *   MCP3008: 01, 1ccc0000, 00 -> null bit rx[1] b2, result rx[1] b1:0 rx[2]
 *   MCP3208: 0000011c, cc000000, 00 -> null bit rx[1] b4, result rx[1] b3:0 rx[2]
 * Mode 0 or 3; the 10-bit one at most 1.35 MHz at 2.7 V, the 12-bit one 1
 * MHz: 1 MHz for both. ch= how many channels (4 for a module named mcp3004
 * or mcp3204, else 8); vref= the reference, 3.3 V unless told. */

static int mcp_read(sens_io_t *io, sens_slot_t *s, bool twelve)
{
    int nch = sens_arg_int(s, "ch", named(s, "mcp3004") || named(s, "mcp3204") ? 4 : 8);
    if (nch < 1) nch = 1;
    if (nch > 8) nch = 8;
    float vref = sens_arg_f(s, "vref", 3.3f);
    static const char *const KEY[8] = { "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7" };
    for (int ch = 0; ch < nch; ch++) {
        uint8_t tx[3], rx[3];
        if (twelve) { tx[0] = (uint8_t)(0x06 | ch >> 2); tx[1] = (uint8_t)((ch & 3) << 6); }
        else { tx[0] = 0x01; tx[1] = (uint8_t)(0x80 | ch << 4); }
        tx[2] = 0x00;
        if (!aos_io_spi_xfer(io->spi, tx, rx, 3, SENS_XFER_MS)) return R_NACK;
        /* the null bit is driven low: high, nobody drives MISO */
        if (rx[1] & (twelve ? 0x10 : 0x04)) {
            if (rx[1] == 0xFF && rx[2] == 0xFF) sens_scpy(s->pub.err, sizeof s->pub.err, "spi");
            else sens_err(s, "%02X%02X", rx[1], rx[2]);
            return rx[2] == 0xFF ? R_NACK : R_ID;
        }
        int code = twelve ? (rx[1] & 0x0F) << 8 | rx[2] : (rx[1] & 0x03) << 8 | rx[2];
        sens_setv(s, ch, KEY[ch], AOS_SQ_VOLT, (float)code * vref / (twelve ? 4096.0f : 1024.0f));
    }
    return R_OK;
}

static int mcp3008_read(sens_io_t *io, sens_slot_t *s) { return mcp_read(io, s, false); }
static int mcp3208_read(sens_io_t *io, sens_slot_t *s) { return mcp_read(io, s, true); }

const sens_drv_t DRV_MCP3008 = {
    .prio = SP_REG,
    .cat = SC(POWER),
    .pub = { "mcp3008", "MCP3008", { 0 }, 0, false, false, true, false, AOS_SENSOR_BUS_SPI, 0 },
    .flags = SF_NOPROBE, .spi_mode = 0, .spi_hz = 1000000,
    .read = mcp3008_read,
};

const sens_drv_t DRV_MCP3208 = {
    .prio = SP_REG,
    .cat = SC(POWER),
    .pub = { "mcp3208", "MCP3208", { 0 }, 0, false, false, true, false, AOS_SENSOR_BUS_SPI, 0 },
    .flags = SF_NOPROBE, .spi_mode = 0, .spi_hz = 1000000,
    .read = mcp3208_read,
};

const sens_drv_t *const SENS_G_SPI[] = {
    &DRV_MAX31855, &DRV_MAX31856, &DRV_MAX31865, &DRV_MCP3008, &DRV_MCP3208,
    NULL,
};

const sens_alias_t SENS_A_SPI[] = {
    { "max31855k", "max31855" }, { "max31855j", "max31855" }, { "max31855t", "max31855" },
    { "pt100", "max31865" }, { "pt1000", "max31865" },
    { "mcp3004", "mcp3008" }, { "mcp3204", "mcp3208" },
    { NULL, NULL },
};
