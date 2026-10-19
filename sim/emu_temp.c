/*
 * P4OS simulator - emulated temperature and humidity sensors for the sensor service (sensors_emu.h).
 *
 * Each model speaks its chip's protocol from the datasheet: the IDs and the
 * power-on values, the commands, the CRCs (Sensirion's 0x31 from 0xFF, the
 * SHT2x's from 0x00, the MLX90614's SMBus PEC, the AM2320's Modbus CRC-16),
 * and the conversion times, NACKing a read that comes too early where the
 * chip does. The raw values come from inverting the datasheet's formula
 * for the bench's room, each sensor a little off the others the way real
 * ones are.
 */
#include "sensors_emu.h"

#include <math.h>
#include <string.h>

/* u[] slots every model here uses the same way */
enum { U_VAR, U_BUSY, U_KIND, U_ASLEEP, U_LAST };

static uint16_t u16clamp(double v) { return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)lrint(v); }

static uint8_t crc_si(const uint8_t *p, int n) { return sim_crc8(p, n, 0x31, 0xFF); }

static void put_si(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; p[2] = crc_si(p, 2); }

/* the answer of a command, ready at a time; a read before it is NACKed */
static void answer_at(sim_emu_t *d, int n, uint32_t delay_ms)
{
    d->resp_n = n;
    d->resp_ms = sim_ms() + delay_ms;
}

static bool give(sim_emu_t *d, uint8_t *r, size_t rn)
{
    if (!d->resp_n || sim_ms() < d->resp_ms) return false;
    for (size_t i = 0; i < rn; i++) r[i] = i < (size_t)d->resp_n ? d->resp[i] : 0xFF;
    d->resp_n = 0;
    return true;
}

/* ---- SHT21 / HTU21D / Si7021 (Sensirion SHT21, Si7021-A20 datasheets) ---- */

enum { V_SHT21, V_HTU21D, V_SI7021 };

static void sht2_reset(sim_emu_t *d)
{
    const char *n = d->m->name;
    d->u[U_VAR] = !strcmp(n, "si7021") ? V_SI7021 : !strcmp(n, "htu21d") ? V_HTU21D : V_SHT21;
    d->regs[0] = d->u[U_VAR] == V_SI7021 ? 0x3A : 0x02;          /* the user register */
    d->resp_n = 0;
}

static void sht2_word(sim_emu_t *d, bool rh, uint32_t ms)
{
    double t = sim_now_s();
    uint16_t raw = rh ? u16clamp((sim_room_h(t) - 0.9 + 6.0) / 125.0 * 65536.0)
                      : u16clamp((sim_room_t(t) + 0.15 + 46.85) / 175.72 * 65536.0);
    raw = (uint16_t)((raw & 0xFFFC) | (rh ? 0x02 : 0x00));
    d->resp[0] = (uint8_t)(raw >> 8);
    d->resp[1] = (uint8_t)raw;
    d->resp[2] = sim_crc8(d->resp, 2, 0x31, 0x00);
    if (rh) d->u[4] = u16clamp((sim_room_t(t) + 0.15 + 46.85) / 175.72 * 65536.0) & 0xFFFC;   /* for 0xE0 */
    answer_at(d, 3, ms);
}

static bool sht2_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    bool si = d->u[U_VAR] == V_SI7021;
    uint64_t now = sim_ms();
    if (now < d->t0) return false;                   /* in its soft reset */
    if (wn) {
        if (d->resp_n && now < d->resp_ms) return false;   /* no-hold: busy measuring */
        switch (w[0]) {
        case 0xFE: sht2_reset(d); d->t0 = now + 15; return true;
        case 0xE7: d->resp[0] = (uint8_t)d->regs[0]; answer_at(d, 1, 0); break;
        case 0xE6:
            if (wn < 2) return false;
            d->regs[0] = (uint8_t)((d->regs[0] & 0x38) | (w[1] & 0xC7));
            return true;
        case 0xF3: sht2_word(d, false, si ? 11 : d->u[U_VAR] == V_HTU21D ? 44 : 70); break;
        case 0xF5: sht2_word(d, true, si ? 20 : d->u[U_VAR] == V_HTU21D ? 14 : 22); break;
        case 0xE3: case 0xE5:                        /* hold master: stretched to the end of it */
            sht2_word(d, w[0] == 0xE5, 0);
            break;
        case 0xE0:                                   /* Si70xx: the T of the last RH */
            if (!si) return false;
            d->resp[0] = (uint8_t)(d->u[4] >> 8); d->resp[1] = (uint8_t)d->u[4];
            answer_at(d, 2, 0);
            break;
        case 0xFA: case 0xFC: {                      /* Si70xx electronic ID, CRCs cumulative */
            if (!si || wn < 2) return false;
            static const uint8_t SNA[4] = { 0xA1, 0x52, 0x33, 0x0C }, SNB[4] = { 0x15, 0xFF, 0xB5, 0xFF };
            uint8_t acc[4];
            int n = 0;
            if (w[0] == 0xFA && w[1] == 0x0F) {
                for (int i = 0; i < 4; i++) {
                    acc[i] = SNA[i];
                    d->resp[n++] = SNA[i];
                    d->resp[n++] = sim_crc8(acc, i + 1, 0x31, 0x00);
                }
            } else if (w[0] == 0xFC && w[1] == 0xC9) {
                memcpy(acc, SNB, 4);
                d->resp[n++] = SNB[0]; d->resp[n++] = SNB[1]; d->resp[n++] = sim_crc8(acc, 2, 0x31, 0x00);
                d->resp[n++] = SNB[2]; d->resp[n++] = SNB[3]; d->resp[n++] = sim_crc8(acc, 4, 0x31, 0x00);
            } else return false;
            answer_at(d, n, 0);
            break;
        }
        case 0x84:                                   /* Si70xx firmware revision: 2.0 */
            if (!si || wn < 2 || w[1] != 0xB8) return false;
            d->resp[0] = 0x20;
            answer_at(d, 1, 0);
            break;
        default: return false;
        }
    }
    return rn ? give(d, r, rn) : true;
}

static const sim_model_t SIM_SHT21 = { "sht21", 0x40, sht2_reset, sht2_xfer };
static const sim_model_t SIM_HTU21D = { "htu21d", 0x40, sht2_reset, sht2_xfer };
static const sim_model_t SIM_SI7021 = { "si7021", 0x40, sht2_reset, sht2_xfer };

/* ---- HDC1080 (TI datasheet 8.5-8.6): 16-bit registers, the pointer starts it ---- */

static void hdc1_reset(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[0x02] = 0x1000;
    d->r16[0xFB] = 0x0123; d->r16[0xFC] = 0x4567; d->r16[0xFD] = 0x8900;
    d->r16[0xFE] = 0x5449; d->r16[0xFF] = 0x1050;
    d->resp_n = 0;
}

static bool hdc1_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn) {
        d->ptr = w[0];
        if (wn >= 3) {
            uint16_t v = (uint16_t)(w[1] << 8 | w[2]);
            if (d->ptr == 0x02) { if (v & 0x8000) hdc1_reset(d); else d->r16[2] = v & 0x3700; }
            return !rn;
        }
        if (d->ptr == 0x00 || d->ptr == 0x01) {      /* a conversion starts */
            double t = sim_now_s();
            uint16_t rt = u16clamp((sim_room_t(t) - 0.25 + 40.0) / 165.0 * 65536.0) & 0xFFFC;
            uint16_t rh = u16clamp((sim_room_h(t) + 1.6) / 100.0 * 65536.0) & 0xFFFC;
            bool both = d->r16[2] & 0x1000;
            int n = 0;
            if (d->ptr == 0x00) { d->resp[n++] = (uint8_t)(rt >> 8); d->resp[n++] = (uint8_t)rt; }
            if (d->ptr == 0x01 || both) { d->resp[n++] = (uint8_t)(rh >> 8); d->resp[n++] = (uint8_t)rh; }
            answer_at(d, n, both ? 13 : 7);
            if (rn) return false;                    /* read straight away: not ready */
            return true;
        }
    }
    if (!rn) return true;
    if (d->ptr == 0x00 || d->ptr == 0x01) return give(d, r, rn);
    uint16_t v = d->r16[d->ptr];
    for (size_t i = 0; i < rn; i++) r[i] = i & 1 ? (uint8_t)v : (uint8_t)(v >> 8);
    return true;
}

static const sim_model_t SIM_HDC1080 = { "hdc1080", 0x40, hdc1_reset, hdc1_xfer };

/* ---- HDC2080 (TI SNAS678C 7.6): 8-bit registers, one-shot trigger, DRDY ---- */

static void hdc2_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0xFC] = 0x49; d->regs[0xFD] = 0x54; d->regs[0xFE] = 0xD0; d->regs[0xFF] = 0x07;
    d->resp_ms = 0;
    d->u[U_BUSY] = 0;
}

static void hdc2_update(sim_emu_t *d)
{
    if (!d->u[U_BUSY] || sim_ms() < d->resp_ms) return;
    double t = sim_now_s();
    uint16_t rt = u16clamp((sim_room_t(t) + 0.3 + 40.5) / 165.0 * 65536.0);
    uint16_t rh = u16clamp((sim_room_h(t) - 1.4) / 100.0 * 65536.0);
    d->regs[0x00] = (uint8_t)rt; d->regs[0x01] = (uint8_t)(rt >> 8);
    d->regs[0x02] = (uint8_t)rh; d->regs[0x03] = (uint8_t)(rh >> 8);
    d->regs[0x04] |= 0x80;                           /* DRDY */
    d->regs[0x0F] &= 0xFE;                           /* MEAS_TRIG clears itself */
    d->u[U_BUSY] = 0;
}

static bool hdc2_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    hdc2_update(d);
    if (wn >= 2) {
        uint8_t reg = w[0], v = w[1];
        if (reg == 0x0E && (v & 0x80)) { hdc2_reset(d); return true; }
        if (reg >= 0x05 && reg <= 0x0F) d->regs[reg] = v;
        if (reg == 0x0F && (v & 0x01)) {
            d->u[U_BUSY] = 1;
            d->resp_ms = sim_ms() + 2;               /* 14-bit T and RH: ~1.3 ms */
            d->regs[0x04] &= 0x7F;
        }
        d->ptr = (uint8_t)(reg + 1);
        return true;
    }
    if (wn == 1) d->ptr = w[0];
    for (size_t i = 0; i < rn; i++) {
        uint8_t reg = (uint8_t)(d->ptr + i);
        r[i] = d->regs[reg];
        if (reg == 0x04) d->regs[0x04] &= 0x7F;      /* DRDY clears when read */
    }
    return true;
}

static const sim_model_t SIM_HDC2080 = { "hdc2080", 0x40, hdc2_reset, hdc2_xfer };

/* ---- HDC3020 (TI datasheet 8.5): Sensirion-style commands, ID 0x3000 ---- */

static void hdc3_reset(sim_emu_t *d) { d->resp_n = 0; }

static bool hdc3_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn == 1) return false;
    if (wn >= 2) {
        uint16_t c = (uint16_t)(w[0] << 8 | w[1]);
        double t = sim_now_s();
        switch (c) {
        case 0x2400: case 0x240B: case 0x2416: case 0x24FF:     /* trigger on demand, LPM0..3 */
            put_si(d->resp, u16clamp((sim_room_t(t) - 0.1 + 45.0) / 175.0 * 65535.0));
            put_si(d->resp + 3, u16clamp((sim_room_h(t) + 0.5) / 100.0 * 65535.0));
            answer_at(d, 6, c == 0x2400 ? 12 : 5);
            break;
        case 0x3781: put_si(d->resp, 0x3000); answer_at(d, 3, 0); break;   /* manufacturer ID */
        case 0x3683: put_si(d->resp, 0x1F2E); answer_at(d, 3, 0); break;   /* NIST ID words */
        case 0x3684: put_si(d->resp, 0x3D4C); answer_at(d, 3, 0); break;
        case 0x3685: put_si(d->resp, 0x5B6A); answer_at(d, 3, 0); break;
        case 0xF32D: put_si(d->resp, 0x0000); answer_at(d, 3, 0); break;   /* status, as an SHT3x */
        case 0x30A2: case 0x3093: d->resp_n = 0; break;                     /* reset, exit auto mode */
        default: return false;
        }
    }
    return rn ? give(d, r, rn) : true;
}

static const sim_model_t SIM_HDC3020 = { "hdc3020", 0x44, hdc3_reset, hdc3_xfer };

/* ---- SHTC3 (Sensirion datasheet 5.3-5.9): sleep, wake, ID 0xEFC8 ---- */

static void shtc3_reset(sim_emu_t *d) { d->resp_n = 0; d->u[U_ASLEEP] = 0; }

static bool shtc3_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn == 1) return false;
    uint16_t c = wn >= 2 ? (uint16_t)(w[0] << 8 | w[1]) : 0;
    if (d->u[U_ASLEEP]) {                            /* only the wakeup gets an ACK */
        if (c != 0x3517) return false;
        d->u[U_ASLEEP] = 0;
        return !rn;
    }
    if (wn >= 2) {
        double t = sim_now_s();
        switch (c) {
        case 0x7866: case 0x7CA2: case 0x609C: case 0x6458:      /* T first: normal, stretched, low power */
            put_si(d->resp, (uint16_t)lrint(fmin(65535.0, (sim_room_t(t) + 0.45 + 45.0) / 175.0 * 65536.0)));
            put_si(d->resp + 3, (uint16_t)lrint(fmin(65535.0, (sim_room_h(t) - 0.7) / 100.0 * 65536.0)));
            answer_at(d, 6, c == 0x609C || c == 0x6458 ? 1 : 12);
            break;
        case 0x58E0: case 0x5C24: case 0x401A: case 0x44DE: {    /* RH first */
            uint8_t tw[3];
            put_si(tw, (uint16_t)lrint(fmin(65535.0, (sim_room_t(t) + 0.45 + 45.0) / 175.0 * 65536.0)));
            put_si(d->resp, (uint16_t)lrint(fmin(65535.0, (sim_room_h(t) - 0.7) / 100.0 * 65536.0)));
            memcpy(d->resp + 3, tw, 3);
            answer_at(d, 6, c == 0x401A || c == 0x44DE ? 1 : 12);
            break;
        }
        case 0xEFC8: put_si(d->resp, 0x0887); answer_at(d, 3, 0); break;   /* (id & 0x083F) == 0x0807 */
        case 0x3517: break;
        case 0xB098: d->u[U_ASLEEP] = 1; d->resp_n = 0; return !rn;
        case 0x805D: d->resp_n = 0; break;
        default: return false;
        }
    }
    return rn ? give(d, r, rn) : true;
}

static const sim_model_t SIM_SHTC3 = { "shtc3", 0x70, shtc3_reset, shtc3_xfer };

/* ---- AM2320 (Aosong datasheet 7.2): asleep it NACKs and wakes; Modbus 0x03 ---- */

static void am2320_reset(sim_emu_t *d)
{
    d->u[U_ASLEEP] = 1;
    d->resp_n = 0;
    d->u[U_LAST] = 0;
}

static bool am2320_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    uint64_t now = sim_ms();
    if (!d->u[U_ASLEEP] && now - d->t0 > 3000) d->u[U_ASLEEP] = 1;     /* 3 s without a word: sleeps */
    if (d->u[U_ASLEEP]) {
        d->u[U_ASLEEP] = 0;
        d->t0 = now;
        d->resp_ms = now + 1;                        /* awake 0.8 ms later */
        return false;
    }
    if (now < d->resp_ms && !d->resp_n) return false;
    d->t0 = now;
    if (wn >= 3 && w[0] == 0x03) {
        /* registers: RH hi/lo, T hi/lo (sign and magnitude), ..., model,
         * version, device id */
        double t = sim_now_s();
        uint8_t reg[32] = { 0 };
        uint16_t rh = (uint16_t)lrint((sim_room_h(t) + 2.1) * 10.0);
        double tc = sim_room_t(t) - 0.4;
        uint16_t tv = (uint16_t)lrint(fabs(tc) * 10.0) | (tc < 0 ? 0x8000 : 0);
        reg[0] = (uint8_t)(rh >> 8); reg[1] = (uint8_t)rh; reg[2] = (uint8_t)(tv >> 8); reg[3] = (uint8_t)tv;
        reg[0x08] = 0x01; reg[0x09] = 0x40; reg[0x0A] = 0x10;
        reg[0x0B] = 0x12; reg[0x0C] = 0x34; reg[0x0D] = 0x56; reg[0x0E] = 0x78;
        uint8_t start = w[1], n = w[2];
        int k = 0;
        if (n == 0 || n > 10 || start + n > 0x20) {
            d->resp[k++] = 0x83; d->resp[k++] = 0x03;          /* illegal address */
        } else {
            d->resp[k++] = 0x03; d->resp[k++] = n;
            for (int i = 0; i < n; i++) d->resp[k++] = reg[start + i];
        }
        uint16_t crc = 0xFFFF;                       /* Modbus CRC-16, low byte first */
        for (int i = 0; i < k; i++) {
            crc ^= d->resp[i];
            for (int b = 0; b < 8; b++) crc = (uint16_t)(crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1);
        }
        d->resp[k++] = (uint8_t)crc; d->resp[k++] = (uint8_t)(crc >> 8);
        answer_at(d, k, 2);
        return !rn;
    }
    if (wn) return !rn;                              /* a stray byte, the wake's when awake */
    return give(d, r, rn);
}

static const sim_model_t SIM_AM2320 = { "am2320", 0x5C, am2320_reset, am2320_xfer };

/* ---- 16-bit register chips: MCP9808, TMP117/116/119, TMP102, LM75A ---- */

enum { K_MCP9808, K_TMP117, K_TMP102, K_LM75 };

static void r16_reset(sim_emu_t *d)
{
    const char *n = d->m->name;
    memset(d->r16, 0, sizeof d->r16);
    d->t0 = sim_ms();
    if (!strcmp(n, "mcp9808")) {
        d->u[U_KIND] = K_MCP9808;
        d->r16[0x06] = 0x0054; d->r16[0x07] = 0x0400; d->r16[0x08] = 0x0300;   /* 0x08 is 8-bit: 0x03 */
    } else if (!strncmp(n, "tmp11", 5)) {
        d->u[U_KIND] = K_TMP117;
        d->r16[0x00] = 0x8000;                       /* until the first conversion */
        d->r16[0x01] = 0x0220; d->r16[0x02] = 0x6000; d->r16[0x03] = 0x8000;
        d->r16[0x0F] = !strcmp(n, "tmp116") ? 0x1116 : !strcmp(n, "tmp119") ? 0x2117 : 0x0117;
    } else if (!strcmp(n, "tmp102")) {
        d->u[U_KIND] = K_TMP102;
        d->r16[0x01] = 0x60A0; d->r16[0x02] = 0x4B00; d->r16[0x03] = 0x5000;
    } else {
        d->u[U_KIND] = K_LM75;
        d->r16[0x01] = 0x0000; d->r16[0x02] = 0x4B00; d->r16[0x03] = 0x5000;
    }
}

static void r16_update(sim_emu_t *d)
{
    double tc = sim_room_t(sim_now_s());
    switch (d->u[U_KIND]) {
    case K_MCP9808: {
        if (d->r16[0x01] & 0x0100) return;           /* shut down: keeps the last */
        long v = lrint((tc + 0.05) * 16.0);
        d->r16[0x05] = (uint16_t)((v < 0 ? (v + 4096) | 0x1000 : v) & 0x1FFF);
        break;
    }
    case K_TMP117:
        if ((d->r16[0x01] & 0x0C00) == 0x0400) return;
        if (sim_ms() - d->t0 < 125) return;          /* 8 averages: 125 ms to the first */
        d->r16[0x00] = (uint16_t)(int16_t)lrint((tc - 0.08) / 0.0078125);
        break;
    case K_TMP102:
        if (d->r16[0x01] & 0x0100) return;
        if (d->r16[0x01] & 0x0010) d->r16[0x00] = (uint16_t)((int16_t)lrint((tc + 0.2) / 0.0625) << 3 | 1);
        else d->r16[0x00] = (uint16_t)((int16_t)lrint((tc + 0.2) / 0.0625) << 4);
        break;
    default:                                         /* LM75A: 11 bits */
        if (d->r16[0x01] & 0x0100) return;
        d->r16[0x00] = (uint16_t)((int16_t)lrint((tc - 0.3) / 0.125) << 5);
        break;
    }
}

static bool r16_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    int kind = (int)d->u[U_KIND];
    uint8_t pmask = kind == K_MCP9808 ? 0x0F : kind == K_TMP117 ? 0xFF : 0x03;
    if (wn) d->ptr = w[0] & pmask;
    uint8_t p = d->ptr;
    bool byte_reg = (kind == K_MCP9808 && p == 0x08) || (kind == K_LM75 && p == 0x01);
    if (wn >= 2) {
        uint16_t v = byte_reg || wn < 3 ? (uint16_t)(w[1] << 8) : (uint16_t)(w[1] << 8 | w[2]);
        if (wn < 3 && !byte_reg) return !rn;         /* half a register: ignored */
        switch (kind) {
        case K_MCP9808: if (p == 0x01 || p == 0x08 || (p >= 0x02 && p <= 0x04)) d->r16[p] = v; break;
        case K_TMP117: if (p == 0x01) d->r16[1] = (uint16_t)((d->r16[1] & 0xF000) | (v & 0x0FFC)); else if (p >= 0x02 && p <= 0x08) d->r16[p] = v; break;
        case K_TMP102: if (p == 0x01) d->r16[1] = (uint16_t)((v & 0x9FF0) | 0x6000); else if (p >= 0x02) d->r16[p] = v; break;
        default: if (p == 0x01) d->r16[1] = v & 0x1F00; else if (p >= 0x02) d->r16[p] = v & 0xFF80; break;
        }
        return !rn;
    }
    if (!rn) return true;
    if (kind == K_TMP117 && p > 0x08 && p != 0x0F) return false;
    r16_update(d);
    uint16_t v = d->r16[p];
    if (kind == K_TMP117 && p == 0x01) d->r16[1] &= 0x1FFF;     /* reading clears the flags */
    if (byte_reg) for (size_t i = 0; i < rn; i++) r[i] = (uint8_t)(v >> 8);
    else for (size_t i = 0; i < rn; i++) r[i] = i & 1 ? (uint8_t)v : (uint8_t)(v >> 8);
    return true;
}

static const sim_model_t SIM_MCP9808 = { "mcp9808", 0x18, r16_reset, r16_xfer };
static const sim_model_t SIM_TMP117 = { "tmp117", 0x48, r16_reset, r16_xfer };
static const sim_model_t SIM_TMP116 = { "tmp116", 0x48, r16_reset, r16_xfer };
static const sim_model_t SIM_TMP119 = { "tmp119", 0x48, r16_reset, r16_xfer };
static const sim_model_t SIM_TMP102 = { "tmp102", 0x48, r16_reset, r16_xfer };
static const sim_model_t SIM_LM75 = { "lm75", 0x48, r16_reset, r16_xfer };

/* ---- MLX90614 (Melexis datasheet 8.3-8.4): SMBus words with a PEC ---- */

static void mlx_reset(sim_emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    /* EEPROM at 0x20-0x3F: To max/min, PWM, Ta range, emissivity, config, address */
    d->r16[0x20] = 0x9993; d->r16[0x21] = 0x62E3; d->r16[0x22] = 0x0201; d->r16[0x23] = 0xF71C;
    d->r16[0x24] = 0xFFFF; d->r16[0x25] = 0x9FB4; d->r16[0x2E] = (uint16_t)(0xBE00 | d->addr);
    d->r16[0x3C] = 0x1A2B; d->r16[0x3D] = 0x3C4D; d->r16[0x3E] = 0x5E6F; d->r16[0x3F] = 0x7081;
}

static bool mlx_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (!wn || !rn) return wn > 0;                   /* reads need the command; writes are taken */
    uint8_t cmd = w[0];
    uint16_t v;
    double t = sim_now_s();
    double amb = sim_room_t(t) + 1.2;                /* inside the can, a little warmer */
    double obj = 58.0 + 6.0 * sin(t / 40.0) + sim_noise(0.1);   /* a mate going cold and refilled */
    if (cmd == 0x06) v = (uint16_t)lrint((amb + 273.15) / 0.02);
    else if (cmd == 0x07) v = (uint16_t)lrint((obj + 273.15) / 0.02);
    else if (cmd == 0x08) v = (uint16_t)lrint((amb + 273.15) / 0.02);
    else if (cmd == 0x04 || cmd == 0x05) v = 0x2000;
    else if (cmd >= 0x20 && cmd <= 0x3F) v = d->r16[cmd];
    else return false;
    uint8_t m[5] = { (uint8_t)(d->addr << 1), cmd, (uint8_t)(d->addr << 1 | 1), (uint8_t)v, (uint8_t)(v >> 8) };
    uint8_t out[3] = { m[3], m[4], sim_crc8(m, 5, 0x07, 0x00) };
    for (size_t i = 0; i < rn; i++) r[i] = i < 3 ? out[i] : 0xFF;
    return true;
}

static const sim_model_t SIM_MLX90614 = { "mlx90614", 0x5A, mlx_reset, mlx_xfer };

/* ---- STS31 (Sensirion STS3x-DIS datasheet 4.3-4.11): the SHT3x's commands, T only ---- */

static void sts3_reset(sim_emu_t *d) { d->resp_n = 0; }

static bool sts3_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn == 1) return false;
    if (wn >= 2) {
        uint16_t c = (uint16_t)(w[0] << 8 | w[1]);
        double t = sim_now_s();
        switch (c) {
        case 0x2400: case 0x240B: case 0x2416:
            put_si(d->resp, u16clamp((sim_room_t(t) + 0.12 + 45.0) / 175.0 * 65535.0));
            answer_at(d, 3, c == 0x2400 ? 15 : c == 0x240B ? 6 : 4);
            break;
        case 0x2C06: case 0x2C0D: case 0x2C10:
            put_si(d->resp, u16clamp((sim_room_t(t) + 0.12 + 45.0) / 175.0 * 65535.0));
            answer_at(d, 3, 0);
            break;
        case 0xF32D: put_si(d->resp, 0x8010); answer_at(d, 3, 0); break;   /* alert pending, reset seen */
        case 0x30A2: case 0x3041: case 0x3093: d->resp_n = 0; break;
        default: return false;
        }
    }
    return rn ? give(d, r, rn) : true;
}

static const sim_model_t SIM_STS31 = { "sts31", 0x4A, sts3_reset, sts3_xfer };

const sim_model_t *const SIM_M_TEMP[] = {
    &SIM_SHT21, &SIM_HTU21D, &SIM_SI7021, &SIM_HDC1080, &SIM_HDC2080, &SIM_HDC3020, &SIM_SHTC3,
    &SIM_AM2320, &SIM_MCP9808, &SIM_TMP117, &SIM_TMP116, &SIM_TMP119, &SIM_TMP102, &SIM_LM75,
    &SIM_MLX90614, &SIM_STS31,
    NULL,
};
