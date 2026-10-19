/*
 * P4OS simulator - emulated SPI sensors for the sensor service's SPI
 * drivers (components/aos_io/sensors/drv_spi.c). io_sim.c puts them on a
 * CS of an SPI port when P4_SIM_SPI_A names them ("45=max31856,
 * 44=mcp3208"); the MAX31855 and the MCP3008 live in io_sim.c itself.
 *
 *   max31856          thermocouple converter by registers: a K probe near
 *                     120 °C, swinging 30 degrees over five minutes (a
 *                     kettle on and off); the cold junction a little over
 *                     the room. ":open" leaves the probe unplugged.
 *   max31865          RTD converter: a PT100 in the room, on Adafruit's
 *                     430 Ω reference. ":pt1000" a PT1000 in a freezer near
 *                     -18 °C on 4300 Ω (below 0, where the drivers need the
 *                     other formula); ":open" an RTD with a wire off.
 *   mcp3208           8-channel 12-bit ADC, bit by bit as the MCP3008 in
 *                     io_sim.c, VREF 3.3 V: ch0 a thermistor divider, ch1 a
 *                     slow sine, ch2 a pot at a third, ch3 ground, ch4 the
 *                     3V3 rail, ch5 a 60 s ramp, ch6-7 floating.
 *
 * Each speaks its chip's protocol from the datasheet: the power-on
 * register values, the address auto-increment and its wrap, the read-only
 * registers, the self-clearing bits, the conversion only after the bias
 * and the automatic mode are on and the first conversion's time has gone
 * by, the faults in their status registers. The readings are encoded by
 * inverting the datasheet's formula, so the driver's conversion is checked
 * against an independent one. Called with io_sim.c's SPI lock held.
 */
#include "sensors_emu.h"
#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { E_M56, E_M56_OPEN, E_M65, E_M65_PT1000, E_M65_OPEN, E_ADC12, E_COUNT };

static const char *const NAMES[E_COUNT] = {
    "max31856", "max31856:open", "max31865", "max31865:pt1000", "max31865:open", "mcp3208",
};

typedef struct {
    uint8_t  reg[16];
    bool     por;               /* the power-on values are in */
    uint64_t conv_t0;           /* when automatic conversion (or a one-shot) started */
    bool     one_shot;
} spi_emu_t;

static spi_emu_t s_emu[E_COUNT];

int sim_spi_emu_named(const char *n, size_t len)
{
    for (int i = 0; i < E_COUNT; i++)
        if (strlen(NAMES[i]) == len && !strncasecmp(n, NAMES[i], len)) return i;
    return -1;
}

static uint64_t now_ms(void) { return aos_hal_uptime_ms(); }

/* ---- MAX31856 (datasheet 19-7726, Table 6 and the register descriptions) ---- */

static void m56_por(spi_emu_t *e)
{
    static const uint8_t POR[16] = {
        0x00, 0x03, 0xFF, 0x7F, 0xC0, 0x7F, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(e->reg, POR, 16);
}

/* The time of a conversion: 185 ms at 50 Hz (155 at 60) for the first,
 * and each extra sample of the average 40 ms (33) more */
static uint32_t m56_conv_ms(const spi_emu_t *e)
{
    int avg = 1 << ((e->reg[1] >> 4) & 7);
    if (avg > 16) avg = 16;
    bool f50 = e->reg[0] & 1;
    return (f50 ? 185u : 155u) + (uint32_t)(avg - 1) * (f50 ? 40u : 33u);
}

static void m56_convert(spi_emu_t *e, bool open_probe)
{
    double t = sim_now_s();
    double cj = sim_room_t(t) + 1.8 + sim_noise(0.01);
    double tc = 120.0 + 30.0 * sin(t / 48.0) + sim_noise(0.05);
    uint8_t sr = 0;
    if (open_probe) {
        /* with the detection on (OCFAULT != 00) the chip says so; with it
         * off the input floats to the rail and reads out of range */
        if (e->reg[0] & 0x30) sr |= 0x01;
        else sr |= 0x40;
        tc = 2047.0;
    }
    if (!(e->reg[0] & 0x08)) {                         /* CJ sensor off: CJT keeps what was written */
        int32_t c = (int32_t)lround(cj * 256.0) & ~3;  /* 14 bits, 1/64 °C, in the top of the word */
        e->reg[10] = (uint8_t)(c >> 8);
        e->reg[11] = (uint8_t)c;
    }
    int32_t q = (int32_t)lround(tc * 128.0);           /* 19 bits signed, 1/128 °C */
    if (q > 0x3FFFF) q = 0x3FFFF;
    if (q < -0x40000) q = -0x40000;
    uint32_t w = ((uint32_t)q & 0x7FFFF) << 5;
    e->reg[12] = (uint8_t)(w >> 16);
    e->reg[13] = (uint8_t)(w >> 8);
    e->reg[14] = (uint8_t)w;
    e->reg[15] = sr;                                    /* comparator mode: the faults as they are now */
}

static void m56_xfer(spi_emu_t *e, bool open_probe, const uint8_t *tx, uint8_t *rx, size_t n)
{
    memset(rx, 0xFF, n);                                /* SDO high-Z while the address goes in */
    if (!e->por) { m56_por(e); e->por = true; }
    if (!n) return;
    uint8_t a = tx[0] & 0x0F;                           /* 0x10-0x7F mirror nothing: the low nibble is the address */
    bool wr = tx[0] & 0x80;
    if (!wr) {
        uint64_t now = now_ms();
        bool auto_on = e->reg[0] & 0x80;
        if ((auto_on || e->one_shot) && now - e->conv_t0 >= m56_conv_ms(e)) {
            m56_convert(e, open_probe);
            if (e->one_shot) { e->one_shot = false; e->reg[0] &= (uint8_t)~0x40; }
        }
        for (size_t i = 1; i < n; i++, a = (a + 1) & 0x0F) rx[i] = e->reg[a];
        return;
    }
    for (size_t i = 1; i < n; i++, a = (a + 1) & 0x0F) {
        uint8_t v = tx[i];
        if (a >= 0x0C) continue;                        /* LTCB and SR are read-only */
        if ((a == 0x0A || a == 0x0B) && !(e->reg[0] & 0x08)) continue;   /* CJT: only with the CJ sensor off */
        if (a == 0x00) {
            bool was = e->reg[0] & 0x80;
            /* the filter cannot change while it converts: the bit stays */
            if (was) v = (uint8_t)((v & ~1) | (e->reg[0] & 1));
            if (v & 0x02) e->reg[15] = 0;               /* FAULTCLR, self-clearing */
            v &= (uint8_t)~0x02;
            if ((v & 0x80) && !was) e->conv_t0 = now_ms();
            if ((v & 0x40) && !(v & 0x80)) { e->one_shot = true; e->conv_t0 = now_ms(); }
        }
        if (a == 0x01) v &= 0x7F;                       /* b7 reserved */
        e->reg[a] = v;
    }
}

/* ---- MAX31865 (datasheet 19-6478, Tables 1-2 and 7) ---- */

static void m65_por(spi_emu_t *e)
{
    memset(e->reg, 0, sizeof e->reg);
    e->reg[3] = 0xFF;                                   /* high fault threshold */
    e->reg[4] = 0xFF;
}

/* IEC 60751 from the temperature: what the RTD measures */
static double pt_ohm(double r0, double t)
{
    const double A = 3.9083e-3, B = -5.775e-7, C = -4.183e-12;
    return r0 * (1.0 + A * t + B * t * t + (t < 0 ? C * (t - 100.0) * t * t * t : 0.0));
}

static void m65_convert(spi_emu_t *e, int dev)
{
    double t = sim_now_s();
    double r0 = dev == E_M65_PT1000 ? 1000.0 : 100.0, rref = r0 * 4.3;
    double temp = dev == E_M65_PT1000 ? -18.0 + 1.5 * sin(t / 70.0) + sim_noise(0.02)
                                      : sim_room_t(t) + 0.4 + sim_noise(0.02);
    double ohm = pt_ohm(r0, temp);
    long code = dev == E_M65_OPEN ? 0x7FFF : lround(ohm / rref * 32768.0);
    if (code > 0x7FFF) code = 0x7FFF;
    if (code < 0) code = 0;
    uint16_t hft = (uint16_t)(e->reg[3] << 8 | e->reg[4]) >> 1, lft = (uint16_t)(e->reg[5] << 8 | e->reg[6]) >> 1;
    uint8_t f = e->reg[7];
    if (code >= hft) f |= 0x80;
    if (code < lft) f |= 0x40;
    if (dev == E_M65_OPEN) f |= 0x80 | 0x08;            /* full scale, and RTDIN- found open */
    e->reg[7] = f;
    uint16_t w = (uint16_t)(code << 1 | (f ? 1 : 0));
    e->reg[1] = (uint8_t)(w >> 8);
    e->reg[2] = (uint8_t)w;
}

static void m65_xfer(spi_emu_t *e, int dev, const uint8_t *tx, uint8_t *rx, size_t n)
{
    memset(rx, 0xFF, n);
    if (!e->por) { m65_por(e); e->por = true; }
    if (!n) return;
    uint8_t a = tx[0] & 0x07;
    bool wr = tx[0] & 0x80;
    if (!wr) {
        /* a conversion every 20 ms (16.7 at 60 Hz) once the bias and the
         * automatic mode are on, the first after 62.5 ms (52) */
        uint64_t now = now_ms();
        bool f50 = e->reg[0] & 1;
        bool run = (e->reg[0] & 0xC0) == 0xC0 || e->one_shot;
        if (run && (e->reg[0] & 0x80) && now - e->conv_t0 >= (f50 ? 63u : 52u)) {
            m65_convert(e, dev);
            if (e->one_shot) { e->one_shot = false; e->reg[0] &= (uint8_t)~0x20; }
        }
        for (size_t i = 1; i < n; i++, a = (a + 1) & 0x07) rx[i] = e->reg[a];
        return;
    }
    for (size_t i = 1; i < n; i++, a = (a + 1) & 0x07) {
        uint8_t v = tx[i];
        if (a == 1 || a == 2 || a == 7) continue;       /* RTD and the fault status are read-only */
        if (a == 0) {
            bool was = (e->reg[0] & 0xC0) == 0xC0;
            if (was) v = (uint8_t)((v & ~1) | (e->reg[0] & 1));
            if (v & 0x02) e->reg[7] = 0;                /* fault clear */
            v &= (uint8_t)~(0x02 | 0x0C);               /* it, and the fault-detection cycle, clear themselves */
            if ((v & 0xC0) == 0xC0 && !was) e->conv_t0 = now_ms();
            if ((v & 0x20) && !(v & 0x40)) { e->one_shot = true; e->conv_t0 = now_ms(); }
        }
        e->reg[a] = v;
    }
}

/* ---- MCP3208 (DS21298, section 6.1, Figure 6-1) ---- */

static double adc12_volts(int ch)
{
    double t = sim_now_s();
    switch (ch) {
    case 0: {                                            /* 10 kΩ NTC (B 3950) under 10 kΩ to 3.3 V */
        double k = sim_room_t(t) + 273.15;
        double r = 10000.0 * exp(3950.0 * (1.0 / k - 1.0 / 298.15));
        return 3.3 * r / (r + 10000.0);
    }
    case 1: return 1.65 + 1.2 * sin(t / 11.0);
    case 2: return 1.1 + sim_noise(0.001);
    case 3: return 0.001 + sim_noise(0.001);
    case 4: return 3.297 + sim_noise(0.001);
    case 5: return 3.3 * fmod(t, 60.0) / 60.0;
    default: return 1.6 + sim_noise(1.2);
    }
}

/* Bit by bit, MSB first: DOUT high-Z (MISO's pull-up, 1) until the start
 * bit and the four configuration bits are in, then a clock to sample, a
 * null bit, B11..B0, and with CS still low the same word again LSB first
 * from B1, then zeros */
static void adc12_xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    int state = 0, cfg = 0, ncfg = 0, out = 0, code = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t r = 0;
        for (int b = 7; b >= 0; b--) {
            int in = (tx[i] >> b) & 1, o = 1;
            if (state == 0) { if (in) state = 1; }
            else if (state == 1) {
                cfg = cfg << 1 | in;
                if (++ncfg == 4) {
                    int ch = cfg & 7;
                    double v = (cfg & 8) ? adc12_volts(ch) : adc12_volts(ch) - adc12_volts(ch ^ 1);
                    code = (int)lround(v / 3.3 * 4096.0);
                    code = code < 0 ? 0 : code > 4095 ? 4095 : code;
                    state = 2;
                }
            } else if (state == 2) { state = 3; out = 0; }          /* the sample clock */
            else if (state == 3) {
                o = out == 0 ? 0 : (code >> (12 - out)) & 1;         /* null bit, then B11..B0 */
                if (++out > 12) { state = 4; out = 1; }
            } else if (state == 4) {
                o = (code >> out) & 1;                               /* B1..B11, LSB first */
                if (++out > 11) state = 5;
            } else o = 0;
            r = (uint8_t)(r << 1 | o);
        }
        rx[i] = r;
    }
}

/* One CS assertion on the chip; what the line asks of it in max_hz and
 * mode_ok, for io_sim.c to garble the bits the way the real part would */
void sim_spi_emu_xfer(int dev, uint8_t mode, const uint8_t *tx, uint8_t *rx, size_t n, uint32_t *max_hz, bool *mode_ok)
{
    switch (dev) {
    case E_M56:
    case E_M56_OPEN:
        m56_xfer(&s_emu[dev], dev == E_M56_OPEN, tx, rx, n);
        *max_hz = 5000000;
        *mode_ok = mode == 1 || mode == 3;
        break;
    case E_M65:
    case E_M65_PT1000:
    case E_M65_OPEN:
        m65_xfer(&s_emu[dev], dev, tx, rx, n);
        *max_hz = 5000000;
        *mode_ok = mode == 1 || mode == 3;
        break;
    case E_ADC12:
        adc12_xfer(tx, rx, n);
        *max_hz = 2000000;
        *mode_ok = mode == 0 || mode == 3;
        break;
    default:
        memset(rx, 0xFF, n);
        break;
    }
}
