/*
 * P4OS simulator - emulated power sensors for the sensor service
 * (sensors_emu.h): INA260, INA3221, INA228, INA238, MAX17048, MAX17043,
 * ADS1015, PCF8591, MCP3421, MCP3424, NAU7802, MCP9600 and MCP9601.
 *
 * Each speaks its datasheet's protocol, and the registers hold the bench's
 * physical values (amperes, volts, grams, degrees) encoded the way the
 * chip encodes them, so a driver's scaling is checked against the
 * datasheet and not against itself:
 *   INA260   a 12 V LED strip being dimmed, through its 2 mΩ shunt
 *   INA3221  a 5 V USB rail with radio bursts, a 3.3 V rail, a 12 V fan,
 *            each through 100 mΩ
 *   INA228/238  a 24 V motor through 15 mΩ (Adafruit's boards)
 *   MAX17048 a LiPo cell at ~72 %, discharging 2.1 %/h
 *   ADS1015  a 2.5 V reference, a slider, 0.512 V, GND (ads1015c: one
 *            left converting continuously by another program)
 *   PCF8591  a YL-40: LDR, thermistor, a free input, the pot
 *   MCP342x  a reference, a thermocouple's millivolts, a slow sine, −0.3 V
 *   NAU7802  a 1 kg, 1 mV/V load cell with a 350 g mug put on and lifted
 *   MCP9600  a K thermocouple in an oven at ~180 °C, the board at room
 */
#include "sensors_emu.h"

#include <math.h>
#include <string.h>

#define TAU 6.283185307179586

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static double getd(const sim_emu_t *d, int i) { double v; memcpy(&v, &d->u[i], sizeof v); return v; }
static void setd(sim_emu_t *d, int i, double v) { memcpy(&d->u[i], &v, sizeof v); }

/* A plain 16-bit register file behind an 8-bit pointer, big-endian, as the
 * TI and Maxim parts have: rd() gives a register's value at the moment, wr()
 * takes a write (pointer + 2 bytes). */
typedef uint16_t (*rd16_fn)(sim_emu_t *d, uint8_t reg);
typedef void (*wr16_fn)(sim_emu_t *d, uint8_t reg, uint16_t v);

static bool xfer16(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, rd16_fn rd, wr16_fn wr)
{
    if (wn >= 1) d->ptr = w[0];
    if (wn >= 3) wr(d, d->ptr, (uint16_t)(w[1] << 8 | w[2]));
    if (!rn) return true;
    uint16_t v = rd(d, d->ptr);
    for (size_t i = 0; i < rn; i++) r[i] = i & 1 ? (uint8_t)v : (uint8_t)(v >> 8);
    return true;
}

/* -------------------------------------------------------------------------- */
/* INA260                                                                      */
/* -------------------------------------------------------------------------- */

static void ina260_reset(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[0x00] = 0x6127;
    d->r16[0xFE] = 0x5449;
    d->r16[0xFF] = 0x2270;
}

static uint16_t ina260_rd(sim_emu_t *d, uint8_t reg)
{
    if (reg >= 1 && reg <= 3 && (d->r16[0] & 3)) {         /* MODE 0 / 4: powered down, the last values */
        double t = sim_now_s();
        double i = 1.10 + 0.35 * sin(TAU * t / 20.0) + sim_noise(0.004);
        double v = 12.08 - 0.05 * i + sim_noise(0.002);
        d->r16[1] = (uint16_t)(int16_t)lrint(i / 1.25e-3);
        d->r16[2] = (uint16_t)lrint(v / 1.25e-3);
        d->r16[3] = (uint16_t)lrint(fabs(v * i) / 0.010);
    }
    return reg <= 7 || reg >= 0xFE ? d->r16[reg] : 0;
}

static void ina260_wr(sim_emu_t *d, uint8_t reg, uint16_t v)
{
    if (reg == 0) {
        if (v & 0x8000) ina260_reset(d);
        else d->r16[0] = (uint16_t)((v & 0x0FFF) | 0x6000);   /* 14..12 read 110 */
    } else if (reg == 6 || reg == 7) d->r16[reg] = v;
}

static bool ina260_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    return xfer16(d, w, wn, r, rn, ina260_rd, ina260_wr);
}

static const sim_model_t M_INA260 = { "ina260", 0x40, ina260_reset, ina260_xfer };

/* -------------------------------------------------------------------------- */
/* INA3221                                                                     */
/* -------------------------------------------------------------------------- */

static void ina3221_reset(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[0x00] = 0x7127;
    for (int k = 0x07; k <= 0x0C; k++) d->r16[k] = 0x7FF8;     /* critical and warning limits */
    d->r16[0x0E] = 0x7FFE;                                  /* shunt-voltage sum limit */
    d->r16[0x0F] = 0x0002;
    d->r16[0x10] = 0x2710;
    d->r16[0x11] = 0x2328;
    d->r16[0xFE] = 0x5449;
    d->r16[0xFF] = 0x3220;
}

static void ina3221_ch(double t, int ch, double *v, double *i)
{
    switch (ch) {
    case 0: {
        double s = sin(TAU * t / 8.0);
        *i = 0.32 + 0.25 * (s > 0 ? s * s * s * s : 0) + sim_noise(0.002);
        *v = 5.02 - 0.10 * *i + sim_noise(0.004);
        break;
    }
    case 1: *i = 0.085 + sim_noise(0.001); *v = 3.301 + sim_noise(0.004); break;
    default: *i = 0.150 + 0.010 * sin(TAU * t / 13.0) + sim_noise(0.001); *v = 12.02 + sim_noise(0.01); break;
    }
}

static uint16_t ina3221_rd(sim_emu_t *d, uint8_t reg)
{
    if (reg >= 1 && reg <= 6 && (d->r16[0] & 7) && (d->r16[0] & 7) != 4) {
        int ch = (reg - 1) / 2;
        if (d->r16[0] & (0x4000 >> ch)) {                   /* the channel is enabled */
            double v, i;
            ina3221_ch(sim_now_s(), ch, &v, &i);
            long sh = lrint(clampd(i * 0.100, -0.16384, 0.16376) / 40e-6);
            long bus = lrint(clampd(v, 0, 32.76) / 8e-3);
            d->r16[1 + 2 * ch] = (uint16_t)((int16_t)sh * 8);
            d->r16[2 + 2 * ch] = (uint16_t)(bus << 3);
        }
    }
    return d->r16[reg];
}

static void ina3221_wr(sim_emu_t *d, uint8_t reg, uint16_t v)
{
    if (reg == 0) { if (v & 0x8000) ina3221_reset(d); else d->r16[0] = v; }
    else if ((reg >= 0x07 && reg <= 0x11)) d->r16[reg] = v;
}

static bool ina3221_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    return xfer16(d, w, wn, r, rn, ina3221_rd, ina3221_wr);
}

static const sim_model_t M_INA3221 = { "ina3221", 0x40, ina3221_reset, ina3221_xfer };

/* -------------------------------------------------------------------------- */
/* INA228 / INA238                                                             */
/* -------------------------------------------------------------------------- */
/* u[0..1] the energy in J since RSTACC, u[2..3] the charge in C, u[4] the
 * last update (ms), u[15] 1 for the INA238. Registers of 24 and 40 bits are
 * made at the read from the bench. */

#define INA228_R 0.015

static void ina228_defaults(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[0x01] = 0xFB68;
    d->r16[0x02] = 0x1000;
    d->r16[0x0B] = 0x0001;
    d->r16[0x0D] = 0x7FFF;
    d->r16[0x0F] = 0x7FFF;
    d->r16[0x10] = 0x7FFF;
    d->r16[0x11] = 0xFFFF;
    d->r16[0x3E] = 0x5449;
    d->r16[0x3F] = d->u[15] ? 0x2381 : 0x2281;
    setd(d, 0, 0);
    setd(d, 2, 0);
    d->u[4] = (uint32_t)sim_ms();
}
static void ina228_reset(sim_emu_t *d) { d->u[15] = 0; ina228_defaults(d); }
static void ina238_reset(sim_emu_t *d) { d->u[15] = 1; ina228_defaults(d); }

static void ina228_bench(double t, double *v, double *i)
{
    *i = 0.60 + 0.40 * fabs(sin(TAU * t / 6.0)) + sim_noise(0.003);
    *v = 24.05 - 0.20 * *i + sim_noise(0.004);
}

/* CURRENT_LSB from SHUNT_CAL (datasheets 8.1.2): 0 when not calibrated */
static double ina228_ilsb(const sim_emu_t *d)
{
    double k = d->u[15] ? 819.2e6 : 13107.2e6;
    double cal = (double)(d->r16[2] & 0x7FFF) / ((d->r16[0] & 0x10) ? 4.0 : 1.0);
    return cal / (k * INA228_R);
}

static bool ina228_running(const sim_emu_t *d)
{
    int mode = d->r16[1] >> 12;
    return mode != 0 && mode != 8;
}

static void ina228_tick(sim_emu_t *d)
{
    uint32_t now = (uint32_t)sim_ms();
    double dt = (double)(now - d->u[4]) / 1000.0;
    d->u[4] = now;
    if (!ina228_running(d) || dt <= 0) return;
    double v, i;
    ina228_bench(sim_now_s(), &v, &i);
    setd(d, 0, getd(d, 0) + v * i * dt);
    setd(d, 2, getd(d, 2) + i * dt);
}

/* the register's value, n bytes big-endian into out; its width back */
static int ina228_reg(sim_emu_t *d, uint8_t reg, uint8_t *out)
{
    bool i238 = d->u[15] != 0;
    double v, i, t = sim_now_s();
    ina228_bench(t, &v, &i);
    double vsh = i * INA228_R, lsb = ina228_ilsb(d), die = sim_room_t(t) + 9.0;
    bool hi = (d->r16[0] & 0x10) != 0;
    uint64_t x = 0;
    int n = 2;
    switch (reg) {
    case 0x04:
        if (i238) x = (uint16_t)(int16_t)lrint(clampd(vsh / (hi ? 1.25e-6 : 5e-6), -32768, 32767));
        else { n = 3; x = ((uint32_t)lrint(clampd(vsh / (hi ? 78.125e-9 : 312.5e-9), -524288, 524287)) << 4) & 0xFFFFFF; }
        break;
    case 0x05:
        if (i238) x = (uint16_t)lrint(clampd(v / 3.125e-3, 0, 32767));
        else { n = 3; x = ((uint32_t)lrint(clampd(v / 195.3125e-6, 0, 524287)) << 4) & 0xFFFFFF; }
        break;
    case 0x06:
        x = i238 ? (uint16_t)((int16_t)lrint(die / 0.125) * 16) : (uint16_t)(int16_t)lrint(die / 7.8125e-3);
        break;
    case 0x07:
        if (i238) x = lsb > 0 ? (uint16_t)(int16_t)lrint(clampd(i / lsb, -32768, 32767)) : 0;
        else { n = 3; x = lsb > 0 ? ((uint32_t)lrint(clampd(i / lsb, -524288, 524287)) << 4) & 0xFFFFFF : 0; }
        break;
    case 0x08:
        n = 3;
        x = lsb > 0 ? (uint32_t)lrint(clampd(fabs(v * i) / ((i238 ? 0.2 : 3.2) * lsb), 0, 16777215)) : 0;
        break;
    case 0x09: case 0x0A:
        if (i238) { x = 0; break; }
        n = 5;
        if (lsb > 0) {
            double q = reg == 0x09 ? getd(d, 0) / (16.0 * 3.2 * lsb) : getd(d, 2) / lsb;
            x = (uint64_t)llround(clampd(q, 0, 1099511627775.0));
        }
        break;
    default:
        x = d->r16[reg];
        break;
    }
    for (int k = 0; k < n; k++) out[k] = (uint8_t)(x >> (8 * (n - 1 - k)));
    return n;
}

static bool ina228_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    ina228_tick(d);
    if (wn >= 1) d->ptr = w[0];
    if (wn >= 3) {
        uint16_t v = (uint16_t)(w[1] << 8 | w[2]);
        uint8_t reg = d->ptr;
        if (reg == 0x00) {
            if (v & 0x8000) { ina228_defaults(d); }
            else {
                if (v & 0x4000) { setd(d, 0, 0); setd(d, 2, 0); }   /* RSTACC, self-clearing */
                d->r16[0] = v & 0x3FF0;
            }
        } else if ((reg >= 0x01 && reg <= 0x03) || (reg >= 0x0B && reg <= 0x11)) {
            d->r16[reg] = reg == 0x02 ? (v & 0x7FFF) : v;
        }
    }
    if (!rn) return true;
    uint8_t b[8];
    int n = ina228_reg(d, d->ptr, b);
    for (size_t k = 0; k < rn; k++) r[k] = (int)k < n ? b[k] : 0;
    return true;
}

static const sim_model_t M_INA228 = { "ina228", 0x40, ina228_reset, ina228_xfer };
static const sim_model_t M_INA238 = { "ina238", 0x40, ina238_reset, ina228_xfer };

/* -------------------------------------------------------------------------- */
/* MAX17048 / MAX17043                                                         */
/* -------------------------------------------------------------------------- */
/* u[15] 1 for the MAX17043 (12-bit VCELL × 1.25 mV, no CRATE) */

static void max1704x_defaults(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[0x08] = d->u[15] ? 0x0003 : 0x0012;
    d->r16[0x0A] = d->u[15] ? 0x0000 : 0x8030;
    d->r16[0x0C] = 0x971C;
    d->r16[0x14] = d->u[15] ? 0x0000 : 0x00FF;
    d->r16[0x18] = d->u[15] ? 0x0000 : 0x9601;
    d->r16[0x1A] = d->u[15] ? 0x0000 : 0x0100;       /* RI: it has just been reset */
}
static void max17048_reset(sim_emu_t *d) { d->u[15] = 0; max1704x_defaults(d); }
static void max17043_reset(sim_emu_t *d) { d->u[15] = 1; max1704x_defaults(d); }

static uint16_t max1704x_rd(sim_emu_t *d, uint8_t reg)
{
    double t = sim_now_s();
    double soc = 72.4 - 2.1 * t / 3600.0 + sim_noise(0.01);
    double v = 3.40 + 0.0080 * soc + sim_noise(0.002);
    switch (reg) {
    case 0x02: return d->u[15] ? (uint16_t)(lrint(v / 1.25e-3) << 4) : (uint16_t)lrint(v / 78.125e-6);
    case 0x04: return (uint16_t)lrint(soc * 256.0);
    case 0x16: return d->u[15] ? 0 : (uint16_t)(int16_t)lrint(-2.1 / 0.208);
    default: return d->r16[reg];
    }
}

static void max1704x_wr(sim_emu_t *d, uint8_t reg, uint16_t v)
{
    if (reg == 0xFE) { if (v == 0x5400) max1704x_defaults(d); }
    else if (reg == 0x06 || reg == 0x0A || reg == 0x0C || reg == 0x14 || reg == 0x18 || reg == 0x1A) d->r16[reg] = v;
}

static bool max1704x_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    return xfer16(d, w, wn, r, rn, max1704x_rd, max1704x_wr);
}

static const sim_model_t M_MAX17048 = { "max17048", 0x36, max17048_reset, max1704x_xfer };
static const sim_model_t M_MAX17043 = { "max17043", 0x36, max17043_reset, max1704x_xfer };

/* -------------------------------------------------------------------------- */
/* ADS1015                                                                     */
/* -------------------------------------------------------------------------- */
/* r16[0..3] behind a 2-bit pointer; t0 the end of the conversion running */

static double ads1015_in(int ch, double t)
{
    switch (ch) {
    case 0: return 2.500 + sim_noise(0.001);
    case 1: return 1.80 + 0.60 * sin(TAU * t / 40.0);
    case 2: return 0.512 + sim_noise(0.0005);
    default: return sim_noise(0.0003);
    }
}

static void ads1015_reset(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[1] = 0x8583;
    d->r16[2] = 0x8000;
    d->r16[3] = 0x7FFF;
    d->t0 = 0;
}

static void ads1015_convert(sim_emu_t *d)
{
    static const double FSR[8] = { 6.144, 4.096, 2.048, 1.024, 0.512, 0.256, 0.256, 0.256 };
    static const int SPS[8] = { 128, 250, 490, 920, 1600, 2400, 3300, 3300 };
    uint16_t c = d->r16[1];
    int mux = (c >> 12) & 7;
    double t = sim_now_s(), v;
    switch (mux) {
    case 0: v = ads1015_in(0, t) - ads1015_in(1, t); break;
    case 1: v = ads1015_in(0, t) - ads1015_in(3, t); break;
    case 2: v = ads1015_in(1, t) - ads1015_in(3, t); break;
    case 3: v = ads1015_in(2, t) - ads1015_in(3, t); break;
    default: v = ads1015_in(mux - 4, t); break;
    }
    long code = lrint(clampd(v / FSR[(c >> 9) & 7] * 2048.0, -2048, 2047));
    d->r16[0] = (uint16_t)((int16_t)code * 16);                     /* 12 bits, left-justified */
    int sps = SPS[(c >> 5) & 7];
    d->t0 = sim_ms() + (uint64_t)((1000 + sps - 1) / sps);
}

static bool ads1015_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn >= 1) d->ptr = w[0] & 3;
    if (wn >= 3) {
        uint16_t v = (uint16_t)(w[1] << 8 | w[2]);
        if (d->ptr == 1) {
            d->r16[1] = v & 0x7FFF;
            if ((v & 0x8000) || !(v & 0x0100)) ads1015_convert(d);  /* one shot, or continuous */
        } else if (d->ptr >= 2) d->r16[d->ptr] = v;
    }
    if (!rn) return true;
    uint64_t now = sim_ms();
    if (!(d->r16[1] & 0x0100) && now >= d->t0) ads1015_convert(d);  /* continuous: the next one */
    uint16_t v = d->r16[d->ptr];
    if (d->ptr == 1) v = (uint16_t)(v | (now >= d->t0 ? 0x8000 : 0));   /* OS: 1 = not converting */
    for (size_t i = 0; i < rn; i++) r[i] = i & 1 ? (uint8_t)v : (uint8_t)(v >> 8);
    return true;
}

/* one that some other program left converting continuously (MODE = 0): the
 * only state in which an ADS1015 can be told from an ADS1115 by reading */
static void ads1015c_reset(sim_emu_t *d)
{
    ads1015_reset(d);
    d->r16[1] = 0x8483;
    ads1015_convert(d);
}

static const sim_model_t M_ADS1015 = { "ads1015", 0x48, ads1015_reset, ads1015_xfer };
static const sim_model_t M_ADS1015C = { "ads1015c", 0x48, ads1015c_reset, ads1015_xfer };

/* -------------------------------------------------------------------------- */
/* PCF8591 (YL-40)                                                             */
/* -------------------------------------------------------------------------- */
/* u[0] the control byte, u[1] the result held for the next byte read, u[2]
 * the DAC. Datasheet 8.3-8.4: a conversion of the selected channel runs
 * while each byte goes out, so a read's first byte is the previous result;
 * with auto-increment (bit 2) the channel steps after each conversion.
 * After power-on the held result is 0x80. */

#define PCF_VREF 3.3

static double pcf_ain(int k, double t)
{
    switch (k) {
    case 0: {                                       /* LDR to VCC... its divider: more light, lower */
        double lux = sim_light(t) + 1.0, rl = 10000.0 * pow(lux / 100.0, -0.7);
        return PCF_VREF * rl / (rl + 10000.0);
    }
    case 1: {                                       /* 10 k NTC, B 3950, against 10 k */
        double tk = sim_room_t(t) + 273.15, rn = 10000.0 * exp(3950.0 * (1.0 / tk - 1.0 / 298.15));
        return PCF_VREF * rn / (rn + 10000.0);
    }
    case 2: return 0.90 + 0.30 * sin(TAU * t / 75.0) + sim_noise(0.005);
    default: return clampd(1.65 + 1.40 * sin(TAU * t / 30.0), 0, PCF_VREF);
    }
}

static uint8_t pcf_convert(const sim_emu_t *d)
{
    double t = sim_now_s();
    int ctrl = (int)d->u[0], ch = ctrl & 3, prog = (ctrl >> 4) & 3;
    double a[4];
    for (int k = 0; k < 4; k++) a[k] = pcf_ain(k, t);
    double v;
    bool diff = false;
    switch (prog) {
    case 0: v = a[ch]; break;                                     /* four single-ended */
    case 1: v = a[ch < 3 ? ch : 2] - a[3]; diff = true; break;    /* three differential against AIN3 */
    case 2: if (ch < 2) v = a[ch]; else { v = a[2] - a[3]; diff = true; } break;
    default: if (ch == 0) v = a[0] - a[1]; else v = a[2] - a[3]; diff = true; break;
    }
    if (diff) return (uint8_t)(int8_t)lrint(clampd(v / PCF_VREF * 256.0, -128, 127));
    return (uint8_t)clampd(floor(v / PCF_VREF * 256.0), 0, 255);
}

static void pcf_reset(sim_emu_t *d) { d->u[0] = 0; d->u[1] = 0x80; d->u[2] = 0; }

static bool pcf_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn >= 1) d->u[0] = w[0] & 0x77;
    if (wn >= 2) d->u[2] = w[wn - 1];
    for (size_t i = 0; i < rn; i++) {
        r[i] = (uint8_t)d->u[1];
        d->u[1] = pcf_convert(d);
        if (d->u[0] & 0x04) {
            static const int NCH[4] = { 4, 3, 3, 2 };
            int n = NCH[(d->u[0] >> 4) & 3], ch = (int)(d->u[0] & 3);
            d->u[0] = (d->u[0] & ~3u) | (uint32_t)((ch + 1) % n);
        }
    }
    return true;
}

static const sim_model_t M_PCF8591 = { "pcf8591", 0x48, pcf_reset, pcf_xfer };

/* -------------------------------------------------------------------------- */
/* MCP3421 / MCP3424                                                           */
/* -------------------------------------------------------------------------- */
/* u[0] the configuration (bit 7 aside), u[1] the result's code, u[2] 1 when
 * it is new (RDY reads 0), u[3] 1 while converting, t0 when that ends,
 * u[15] the inputs. Power-on: continuous, channel 1, 12 bits, ×1 (0x90). */

static double mcp342x_in(int ch, double t)
{
    switch (ch) {
    case 0: return 1.2345 + sim_noise(5e-6);
    case 1: return 0.0041 + 0.0003 * sin(TAU * t / 20.0) + sim_noise(2e-6);
    case 2: return 0.75 + 0.50 * sin(TAU * t / 50.0);
    default: return -0.300 + sim_noise(1e-5);
    }
}

static uint32_t mcp342x_tconv(uint32_t cfg)
{
    static const uint32_t MS[4] = { 5, 17, 67, 267 };      /* 240, 60, 15, 3.75 SPS */
    return MS[(cfg >> 2) & 3];
}

static void mcp342x_reset1(sim_emu_t *d, int nch)
{
    d->u[15] = (uint32_t)nch;
    d->u[0] = 0x10;
    d->u[1] = 0;
    d->u[2] = 0;
    d->u[3] = 1;
    d->t0 = sim_ms() + mcp342x_tconv(d->u[0]);
}
static void mcp3421_reset(sim_emu_t *d) { mcp342x_reset1(d, 1); }
static void mcp3424_reset(sim_emu_t *d) { mcp342x_reset1(d, 4); }

static void mcp342x_tick(sim_emu_t *d)
{
    uint64_t now = sim_ms();
    if (!d->u[3] || now < d->t0) return;
    int bits = 12 + 2 * (int)((d->u[0] >> 2) & 3), pga = 1 << (d->u[0] & 3);
    int ch = (int)((d->u[0] >> 5) & 3);
    double v = ch < (int)d->u[15] ? mcp342x_in(ch, sim_now_s()) : 0.0;
    double full = (double)(1 << (bits - 1));
    d->u[1] = (uint32_t)(int32_t)lrint(clampd(v * pga * (double)(1 << bits) / 4.096, -full, full - 1));
    d->u[2] = 1;
    if (d->u[0] & 0x10) d->t0 = now + mcp342x_tconv(d->u[0]);   /* continuous: the next one */
    else d->u[3] = 0;
}

static bool mcp342x_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    mcp342x_tick(d);
    if (wn >= 1) {
        uint8_t c = w[wn - 1];
        d->u[0] = c & 0x7F;
        if ((c & 0x10) || (c & 0x80)) {                    /* continuous, or a one-shot asked for */
            d->u[3] = 1;
            d->t0 = sim_ms() + mcp342x_tconv(d->u[0]);
        } else d->u[3] = 0;
    }
    if (!rn) return true;
    int32_t code = (int32_t)d->u[1];
    bool b18 = ((d->u[0] >> 2) & 3) == 3;
    uint8_t out[3];
    int n = 0;
    if (b18) out[n++] = (uint8_t)(code >> 16);             /* 23..18 repeat the sign */
    out[n++] = (uint8_t)(code >> 8);
    out[n++] = (uint8_t)code;
    uint8_t cfg = (uint8_t)(d->u[0] | (d->u[2] ? 0 : 0x80));
    for (size_t i = 0; i < rn; i++) r[i] = (int)i < n ? out[i] : cfg;
    if (rn >= (size_t)n) d->u[2] = 0;                      /* read: RDY goes back to 1 */
    return true;
}

static const sim_model_t M_MCP3421 = { "mcp3421", 0x68, mcp3421_reset, mcp342x_xfer };
static const sim_model_t M_MCP3424 = { "mcp3424", 0x68, mcp3424_reset, mcp342x_xfer };

/* -------------------------------------------------------------------------- */
/* NAU7802                                                                     */
/* -------------------------------------------------------------------------- */
/* regs[] as written; PUR, CR and CALS are worked out at the read. u[0] PUR,
 * u[1] CR, u[2..3] the calibration's end (ms), u[4] the result (24 bits),
 * t0 the next conversion. */

static void nau_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x1F] = 0x0F;
    d->u[0] = d->u[1] = 0;
    d->u[2] = d->u[3] = 0;
    d->u[4] = 0;
    d->t0 = 0;
}

static uint32_t nau_period(const sim_emu_t *d)
{
    static const uint32_t MS[8] = { 100, 50, 25, 13, 13, 13, 13, 3 };   /* 10, 20, 40, 80 ... 320 SPS */
    return MS[(d->regs[0x02] >> 4) & 7];
}

static void nau_tick(sim_emu_t *d)
{
    uint64_t now = sim_ms();
    bool run = d->u[0] && (d->regs[0x00] & 0x10);
    if (!run) return;
    if (!d->t0) d->t0 = now + nau_period(d);
    if (now < d->t0) return;
    d->t0 = now + nau_period(d);
    double t = sim_now_s();
    int gain = 1 << (d->regs[0x01] & 7);
    double grams = fmod(t, 30.0) < 20.0 ? 350.0 : 0.0;
    /* ratiometric: counts = (mV/V) × gain × 2^23; 1 mV/V at 1000 g, 0.05 mV/V
     * of zero offset (less once calibrated) */
    double off = (d->regs[0x02] & 0x03) == 0 && d->u[2] ? 0.04e-3 : 0.05e-3;
    double c = (off + 1e-6 * grams) * gain * 8388608.0 + sim_noise(0.2 * gain);
    if (d->regs[0x02] & 0x80) c = sim_noise(0.2 * gain);         /* channel 2: nothing there */
    d->u[4] = (uint32_t)(int32_t)lrint(clampd(c, -8388608, 8388607)) & 0xFFFFFF;
    d->u[1] = 1;
}

static uint8_t nau_rd(sim_emu_t *d, uint8_t reg)
{
    uint64_t now = sim_ms();
    uint64_t cal = (uint64_t)d->u[3] << 32 | d->u[2];
    switch (reg) {
    case 0x00: return (uint8_t)((d->regs[0] & 0xD7) | (d->u[0] ? 0x08 : 0) | (d->u[1] ? 0x20 : 0));
    case 0x02: return (uint8_t)((d->regs[2] & ~0x0C) | (cal && now < cal ? 0x04 : 0));
    case 0x12: return (uint8_t)(d->u[4] >> 16);
    case 0x13: return (uint8_t)(d->u[4] >> 8);
    case 0x14: return (uint8_t)d->u[4];
    default: return d->regs[reg];
    }
}

static void nau_wr(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x00) {
        if (v & 0x01) { nau_reset(d); d->regs[0] = 0x01; return; }
        d->regs[0] = v;
        d->u[0] = (v & 0x06) == 0x06;                       /* PUR: up within ~200 µs */
        if (!(v & 0x10)) d->t0 = 0;
    } else if (reg == 0x02) {
        d->regs[2] = v & ~0x0C;
        if (v & 0x04) {                                     /* CALS: two conversion periods */
            uint64_t end = sim_ms() + 2 * nau_period(d);
            d->u[2] = (uint32_t)end;
            d->u[3] = (uint32_t)(end >> 32);
        }
    } else if (reg >= 0x12 && reg <= 0x14) {
    } else if (reg != 0x1F) d->regs[reg] = v;
}

static bool nau_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    nau_tick(d);
    if (wn >= 1) d->ptr = w[0];
    for (size_t i = 1; i < wn; i++) nau_wr(d, (uint8_t)(d->ptr + i - 1), w[i]);
    for (size_t i = 0; i < rn; i++) {
        uint8_t reg = (uint8_t)(d->ptr + i);
        r[i] = nau_rd(d, reg);
        if (reg == 0x12) d->u[1] = 0;                       /* the result taken: CR clears */
    }
    return true;
}

static const sim_model_t M_NAU7802 = { "nau7802", 0x2A, nau_reset, nau_xfer };

/* -------------------------------------------------------------------------- */
/* MCP9600 / MCP9601                                                           */
/* -------------------------------------------------------------------------- */
/* r16 holds the 16-bit registers (TH 0x00, TΔ 0x01, TC 0x02, limits
 * 0x10-0x13, id 0x20), regs the 8-bit ones (status 0x04, sensor and device
 * configuration 0x05/0x06, alert configuration and hysteresis 0x08-0x0F),
 * u[0] the 24-bit raw ADC; t0 the next conversion. u[15] 1 for an MCP9601. */

static int mcp9600_width(uint8_t reg)
{
    if (reg <= 0x02 || (reg >= 0x10 && reg <= 0x13) || reg == 0x20) return 2;
    if (reg == 0x03) return 3;
    if (reg >= 0x04 && reg <= 0x0F && reg != 0x07) return 1;
    return 0;
}

static uint32_t mcp9600_tconv(const sim_emu_t *d)
{
    static const uint32_t MS[4] = { 320, 80, 20, 5 };      /* 18, 16, 14, 12 bits */
    return MS[(d->regs[0x06] >> 5) & 3];
}

static void mcp9600_defaults(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    memset(d->regs, 0, sizeof d->regs);
    d->r16[0x20] = d->u[15] ? 0x4111 : 0x4011;
    d->u[0] = 0;
    d->t0 = sim_ms() + mcp9600_tconv(d);
}
static void mcp9600_reset(sim_emu_t *d) { d->u[15] = 0; mcp9600_defaults(d); }
static void mcp9601_reset(sim_emu_t *d) { d->u[15] = 1; mcp9600_defaults(d); }

static void mcp9600_tick(sim_emu_t *d)
{
    uint64_t now = sim_ms();
    if ((d->regs[0x06] & 3) == 1 || now < d->t0) return;   /* shutdown: no conversions */
    d->t0 = now + mcp9600_tconv(d);
    double t = sim_now_s();
    double hot = 180.0 + 25.0 * sin(TAU * t / 120.0) + sim_noise(0.2);
    double cold = sim_room_t(t) + 1.5;
    long th = lrint(hot * 16.0), tc = lrint(cold * 16.0);
    if ((d->regs[0x06] & 0x80)) tc &= ~3L;                 /* 0.25 °C cold junction */
    d->r16[0x00] = (uint16_t)(int16_t)th;
    d->r16[0x02] = (uint16_t)(int16_t)tc;
    d->r16[0x01] = (uint16_t)(int16_t)(th - tc);
    /* the thermocouple's EMF at 2 µV per LSB (K, ~41 µV/°C) */
    d->u[0] = (uint32_t)(int32_t)lrint((hot - cold) * 41.0 / 2.0) & 0xFFFFFF;
    d->regs[0x04] |= 0x40;                                 /* TH updated */
}

static bool mcp9600_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    mcp9600_tick(d);
    if (wn >= 1) d->ptr = w[0];
    if (wn >= 2) {
        uint8_t reg = d->ptr;
        int wd = mcp9600_width(reg);
        if (reg == 0x04) d->regs[0x04] &= (uint8_t)(w[1] | 0x3F);      /* writing 0 clears burst / update */
        else if (wd == 1 && reg != 0x04) d->regs[reg] = reg == 0x05 ? (w[1] & 0x77) : w[1];
        else if (wd == 2 && reg >= 0x10 && reg <= 0x13 && wn >= 3) d->r16[reg] = (uint16_t)(w[1] << 8 | w[2]);
    }
    if (!rn) return true;
    uint8_t reg = d->ptr, b[3] = { 0 };
    int wd = mcp9600_width(reg);
    if (wd == 3) { b[0] = (uint8_t)(d->u[0] >> 16); b[1] = (uint8_t)(d->u[0] >> 8); b[2] = (uint8_t)d->u[0]; }
    else if (wd == 2) { b[0] = (uint8_t)(d->r16[reg] >> 8); b[1] = (uint8_t)d->r16[reg]; }
    else if (wd == 1) b[0] = d->regs[reg];
    for (size_t i = 0; i < rn; i++) r[i] = (int)i < wd ? b[i] : 0;
    return true;
}

static const sim_model_t M_MCP9600 = { "mcp9600", 0x67, mcp9600_reset, mcp9600_xfer };
static const sim_model_t M_MCP9601 = { "mcp9601", 0x67, mcp9601_reset, mcp9600_xfer };

const sim_model_t *const SIM_M_POWER[] = {
    &M_INA260, &M_INA3221, &M_INA228, &M_INA238, &M_MAX17048, &M_MAX17043,
    &M_ADS1015, &M_ADS1015C, &M_PCF8591, &M_MCP3421, &M_MCP3424, &M_NAU7802, &M_MCP9600, &M_MCP9601,
    NULL,
};
