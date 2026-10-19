/*
 * P4OS - Sensor drivers: sensors on a serial port: CO2 (MH-Z19, Senseair
 * S8), particles (the PMS5003 family, SDS011), mains energy (PZEM-004T
 * v3.0) and presence radars (HLK-LD2410, LD2450) (sens_int.h,
 * docs/SENSORS.md).
 *
 * They are declared only ("module co2 uart.a chip=mhz19"): a serial port
 * has no addresses to probe, and a byte sent to the wrong device is a
 * command to it. The service opens the port at the driver's baud for each
 * read and closes it again. A sensor that answers a request is asked; one
 * that streams on its own (the particle counters, the radars) is read by
 * waiting for its next frame, within a bound, because the round is one
 * second and the other sensors wait behind this one. Whatever the sensor
 * sent while the port was closed is lost and the first frame may arrive
 * cut, so every reader syncs on its frame's header and checks the frame
 * whole (checksum, CRC or fixed tail) before it believes it.
 *
 * A streaming sensor that reports slower than the round (a PMS5003 in its
 * stable mode sends every 2.3 s) keeps its last good frame in the slot: a
 * read that sees no new frame reports that one again, for a few seconds,
 * since it is still the sensor's latest reading.
 *
 * aos_io_uart_read() returns what there is on the Mac and waits for all of
 * it on the board (uart_read_bytes), so the readers loop to a deadline and
 * work either way.
 */
#include "sens_int.h"

/* ---- the port ---- */

#define STREAM_MS 1150      /* the longest a read waits for a streamed frame */

/* Bytes left from before: the start of a frame the sensor began earlier */
static void u_flush(aos_io_uart_t *u)
{
    uint8_t junk[64];
    for (int i = 0; i < 32 && aos_io_uart_read(u, junk, sizeof junk, 0) > 0; i++) {}
}

/* n bytes, or fewer when the deadline passes: how many came */
static int u_read(aos_io_uart_t *u, uint8_t *b, int n, uint32_t until)
{
    int got = 0;
    while (got < n) {
        int32_t left = (int32_t)(until - sens_now());
        if (left <= 0) break;
        int r = aos_io_uart_read(u, b + got, n - got, (int)left);
        if (r < 0) break;
        got += r;
    }
    return got;
}

/* Bytes until the header has gone by (true) or the deadline (false); seen
 * counts every byte that came */
static bool u_sync(aos_io_uart_t *u, const uint8_t *hdr, int hn, uint32_t until, int *seen)
{
    int m = 0;
    for (;;) {
        uint8_t c;
        if (u_read(u, &c, 1, until) != 1) return false;
        (*seen)++;
        if (c == hdr[m]) { if (++m == hn) return true; }
        else m = c == hdr[0] ? 1 : 0;
    }
}

/* The rest of a frame whose header came just before the deadline still
 * gets the time its bytes take on the wire */
static uint32_t u_tail(uint32_t until, int bytes, uint32_t baud)
{
    uint32_t t = sens_now() + 20 + (uint32_t)bytes * 10000u / baud;
    return (int32_t)(t - until) > 0 ? t : until;
}

/* Nothing that made sense came: silence is a missing sensor (or a crossed
 * RX/TX), bytes with no frame in them a wrong baud or another device */
static int u_fail(sens_slot_t *s, int seen, int bad)
{
    if (bad) return R_CRC;
    if (seen) { sens_err(s, "sync"); return R_CRC; }
    sens_err(s, "uart");
    return R_NACK;
}

/* A streamed sensor's last good frame, kept between reads */
typedef struct {
    uint32_t t_ms;
    uint8_t  n;
    uint8_t  f[64];
} held_t;
SPRIV_CHECK(held_t);

typedef int (*pub_fn)(sens_slot_t *s, const uint8_t *f, int n);

static int keep_frame(sens_slot_t *s, const uint8_t *f, int n, pub_fn pub)
{
    held_t *h = SPRIV(s, held_t);
    if (n > (int)sizeof h->f) n = (int)sizeof h->f;
    memcpy(h->f, f, (size_t)n);
    h->n = (uint8_t)n;
    h->t_ms = sens_now();
    return pub(s, h->f, n);
}

static int held(sens_slot_t *s, uint32_t keep_ms, int seen, int bad, pub_fn pub)
{
    held_t *h = SPRIV(s, held_t);
    if (h->n && sens_now() - h->t_ms <= keep_ms) return pub(s, h->f, h->n);
    return u_fail(s, seen, bad);
}

/* ---- Modbus RTU: read input registers (function 0x04) ----
 * n words from reg into w. 0xFE (Senseair's "any sensor") and 0xF8 (the
 * PZEM's general address) are answered by whoever is on the line, so the
 * reply's address is not checked for them. The sync is on address,
 * function and byte count, which also skips the echo of the request that
 * some RS-485 adapters give back. */
static int mb_input(aos_io_uart_t *u, sens_slot_t *s, uint8_t addr, uint16_t reg, int n, uint16_t *w, int ms)
{
    uint8_t q[8] = { addr, 0x04, (uint8_t)(reg >> 8), (uint8_t)reg, 0, (uint8_t)n };
    uint16_t c = sens_crc16_modbus(q, 6);
    q[6] = (uint8_t)c;
    q[7] = (uint8_t)(c >> 8);
    u_flush(u);
    if (aos_io_uart_write(u, q, 8) != 8) { sens_err(s, "uart"); return R_NACK; }
    bool any = addr == 0xFE || addr == 0xF8;
    uint32_t until = sens_now() + (uint32_t)ms;
    uint8_t r[3 + 2 * 16 + 2] = { 0 };
    int seen = 0, bad = 0;
    for (;;) {
        uint8_t b;
        if (u_read(u, &b, 1, until) != 1) break;
        seen++;
        r[0] = r[1]; r[1] = r[2]; r[2] = b;
        if (seen < 3 || !(any || r[0] == addr)) continue;
        if (r[1] == 0x84) {
            /* an exception: the code and the CRC */
            if (u_read(u, r + 3, 2, u_tail(until, 2, 9600)) != 2) break;
            if (sens_crc16_modbus(r, 3) != sens_le16(r + 3)) { bad++; continue; }
            sens_err(s, "exc%02X", r[2]);
            return R_RANGE;
        }
        if (r[1] != 0x04 || r[2] != 2 * n) continue;
        int rest = 2 * n + 2;
        if (u_read(u, r + 3, rest, u_tail(until, rest, 9600)) != rest) break;
        if (sens_crc16_modbus(r, 3 + 2 * n) != sens_le16(r + 3 + 2 * n)) { bad++; memset(r, 0, 3); continue; }
        for (int i = 0; i < n; i++) w[i] = sens_be16(r + 3 + 2 * i);
        return R_OK;
    }
    return u_fail(s, seen, bad);
}

/* ---- MH-Z19B / C / E: NDIR CO2 ----
 * Winsen's datasheet: "read CO2" is FF 01 86 00 00 00 00 00 79, the answer
 * FF 86 HH LL followed by four bytes and a checksum, the negated sum of
 * bytes 1 to 7. Byte 4 is the sensor's temperature plus 40, undocumented
 * and only rough (ESPHome's mhz19 reads it the same way). The calibration
 * commands (zero, span, ABC) are never sent. */

static int mhz19_read(sens_io_t *io, sens_slot_t *s)
{
    static const uint8_t Q[9] = { 0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0x79 };
    static const uint8_t H[2] = { 0xFF, 0x86 };
    aos_io_uart_t *u = io->uart;
    u_flush(u);
    if (aos_io_uart_write(u, Q, sizeof Q) != (int)sizeof Q) { sens_err(s, "uart"); return R_NACK; }
    uint32_t until = sens_now() + 300;
    int seen = 0;
    uint8_t r[9] = { 0xFF, 0x86 };
    if (!u_sync(u, H, 2, until, &seen)) return u_fail(s, seen, 0);
    if (u_read(u, r + 2, 7, u_tail(until, 7, 9600)) != 7) return R_CRC;
    uint8_t sum = 0;
    for (int i = 1; i < 8; i++) sum = (uint8_t)(sum + r[i]);
    if ((uint8_t)(0u - sum) != r[8]) return R_CRC;
    sens_setv(s, 0, "co2", AOS_SQ_CO2, (float)sens_be16(r + 2));
    sens_setv(s, 1, "temp", AOS_SQ_TEMP, (float)r[4] - 40.0f);
    return R_OK;
}

const sens_drv_t DRV_MHZ19 = {
    .prio = SP_REG,
    .cat = SC(AIR),
    .pub = { "mhz19", "MH-Z19", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_UART, 0 },
    .flags = SF_NOPROBE, .baud = 9600, .min_ms = 2000,
    .read = mhz19_read,
};

/* ---- Senseair S8: NDIR CO2 over Modbus RTU ----
 * Senseair's "Modbus on Senseair S8" (TDE2067): input registers IR1 meter
 * status, IR2 alarm status, IR3 output status, IR4 CO2 in ppm, read from
 * the "any sensor" address 0xFE; the four at once, as ESPHome's senseair
 * does. Status bits: 0 fatal error, 1 offset regulation, 2 algorithm,
 * 3 output, 4 self-diagnostics, 5 out of range, 6 memory. It measures
 * every two seconds. */

static int s8_read(sens_io_t *io, sens_slot_t *s)
{
    uint16_t w[4];
    int r = mb_input(io->uart, s, (uint8_t)sens_arg_int(s, "addr", 0xFE), 0x0000, 4, w, 300);
    if (r != R_OK) return r;
    if (w[0] & 0x61) { sens_err(s, "status:%04X", w[0]); return R_RANGE; }
    sens_setv(s, 0, "co2", AOS_SQ_CO2, (float)(int16_t)w[3]);
    return R_OK;
}

const sens_drv_t DRV_SENSEAIR_S8 = {
    .prio = SP_REG,
    .cat = SC(AIR),
    .pub = { "senseair_s8", "Senseair S8", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_UART, 0 },
    .flags = SF_NOPROBE, .baud = 9600, .min_ms = 2000,
    .read = s8_read,
};

/* ---- Plantower PMS5003 / PMS7003 / PMSA003 / PMS3003: particles ----
 * Plantower's PMS5003 datasheet (appendix I): frames 42 4D, a big-endian
 * length (28 on the PMS5003/7003/A003, 20 on the PMS3003, 36 on the
 * PMS5003ST), words of data and the 16-bit sum of every byte before it.
 * Words 3 to 5 are PM1, PM2.5 and PM10 under atmospheric conditions, in
 * µg/m³. In active mode (the power-on one) it sends a frame every 2.3 s
 * while the air is steady and every 200 to 800 ms while it changes; a
 * sensor somebody put in passive mode answers the read command (42 4D E2),
 * which is sent every time and is ignored in active mode. The mode itself
 * is left as it is. A read right after a frame does not wait for the next:
 * with frames 2.3 s apart and the round a second long, the reads fall into
 * step with them and most of the waits are short. */

static int pms_pub(sens_slot_t *s, const uint8_t *f, int n)
{
    (void)n;
    sens_setv(s, 0, "pm1", AOS_SQ_PM, (float)sens_be16(f + 10));
    sens_setv(s, 1, "pm25", AOS_SQ_PM, (float)sens_be16(f + 12));
    sens_setv(s, 2, "pm10", AOS_SQ_PM, (float)sens_be16(f + 14));
    return R_OK;
}

static int pms_read(sens_io_t *io, sens_slot_t *s)
{
    static const uint8_t RD[7] = { 0x42, 0x4D, 0xE2, 0x00, 0x00, 0x01, 0x71 };
    static const uint8_t H[2] = { 0x42, 0x4D };
    aos_io_uart_t *u = io->uart;
    /* a frame from the last round is still the latest: the next one is
     * more than a second away, and waiting for it would hold up the round */
    const held_t *h = SPRIV(s, held_t);
    if (h->n && sens_now() - h->t_ms < 1200) return pms_pub(s, h->f, h->n);
    u_flush(u);
    aos_io_uart_write(u, RD, sizeof RD);
    uint32_t until = sens_now() + STREAM_MS;
    int seen = 0, bad = 0;
    uint8_t f[4 + 36] = { 0x42, 0x4D };
    while (u_sync(u, H, 2, until, &seen)) {
        if (u_read(u, f + 2, 2, u_tail(until, 2, 9600)) != 2) break;
        int len = sens_be16(f + 2);
        if (len == 4) {                             /* the answer to a mode command */
            uint8_t a[4];
            u_read(u, a, 4, u_tail(until, 4, 9600));
            continue;
        }
        if (len != 20 && len != 28 && len != 36) { bad++; continue; }
        if (u_read(u, f + 4, len, u_tail(until, len, 9600)) != len) { bad++; break; }
        uint16_t sum = 0;
        for (int i = 0; i < 2 + len; i++) sum = (uint16_t)(sum + f[i]);
        if (sum != sens_be16(f + 2 + len)) { bad++; continue; }
        return keep_frame(s, f, 4 + len, pms_pub);
    }
    return held(s, 5000, seen, bad, pms_pub);
}

const sens_drv_t DRV_PMS5003 = {
    .prio = SP_REG,
    .cat = SC(AIR),
    .pub = { "pms5003", "PMS5003", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_UART, 0 },
    .flags = SF_NOPROBE, .baud = 9600,
    .read = pms_read,
};

/* ---- Nova Fitness SDS011: particles ----
 * Nova's "Laser Dust Sensor Control Protocol" V1.3: the data frame is
 * AA C0, PM2.5 and PM10 in tenths of µg/m³ (little-endian), the device id,
 * the sum of those six bytes and AB. In its default active mode it sends
 * one a second; in query mode it answers the query command (AA B4 04 ...),
 * which is sent every time, as with the PMS. A sleeping one is not woken. */

static int sds_pub(sens_slot_t *s, const uint8_t *f, int n)
{
    (void)n;
    sens_setv(s, 0, "pm25", AOS_SQ_PM, (float)sens_le16(f + 2) / 10.0f);
    sens_setv(s, 1, "pm10", AOS_SQ_PM, (float)sens_le16(f + 4) / 10.0f);
    return R_OK;
}

static int sds_read(sens_io_t *io, sens_slot_t *s)
{
    static const uint8_t Q[19] = { 0xAA, 0xB4, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0x02, 0xAB };
    static const uint8_t H[2] = { 0xAA, 0xC0 };
    aos_io_uart_t *u = io->uart;
    u_flush(u);
    aos_io_uart_write(u, Q, sizeof Q);
    uint32_t until = sens_now() + STREAM_MS;
    int seen = 0, bad = 0;
    uint8_t f[10] = { 0xAA, 0xC0 };
    while (u_sync(u, H, 2, until, &seen)) {
        if (u_read(u, f + 2, 8, u_tail(until, 8, 9600)) != 8) { bad++; break; }
        uint8_t sum = 0;
        for (int i = 2; i < 8; i++) sum = (uint8_t)(sum + f[i]);
        if (sum != f[8] || f[9] != 0xAB) { bad++; continue; }
        return keep_frame(s, f, 10, sds_pub);
    }
    return held(s, 3000, seen, bad, sds_pub);
}

const sens_drv_t DRV_SDS011 = {
    .prio = SP_REG,
    .cat = SC(AIR),
    .pub = { "sds011", "SDS011", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_UART, 0 },
    .flags = SF_NOPROBE, .baud = 9600,
    .read = sds_read,
};

/* ---- Peacefair PZEM-004T v3.0: mains voltage, current, power, energy ----
 * Its user manual: Modbus RTU at 9600 8N1, input registers 0 to 9:
 * voltage (0.1 V), current (0.001 A, 32 bits, low word first), power
 * (0.1 W, 32 bits), energy (1 Wh, 32 bits), frequency (0.1 Hz), power
 * factor (0.01) and the power alarm (0xFFFF on). addr= is its slave
 * address, 0xF8 (the general one, for a single PZEM on the line) unless
 * told. The power factor goes out as a plain number from 0 to 1. */

static int pzem_read(sens_io_t *io, sens_slot_t *s)
{
    uint16_t w[10];
    int r = mb_input(io->uart, s, (uint8_t)sens_arg_int(s, "addr", 0xF8), 0x0000, 10, w, 400);
    if (r != R_OK) return r;
    sens_setv(s, 0, "bus", AOS_SQ_VOLT, (float)w[0] * 0.1f);
    sens_setv(s, 1, "current", AOS_SQ_CURR, (float)((uint32_t)w[1] | (uint32_t)w[2] << 16) * 0.001f);
    sens_setv(s, 2, "power", AOS_SQ_POWER, (float)((uint32_t)w[3] | (uint32_t)w[4] << 16) * 0.1f);
    sens_setv(s, 3, "energy", AOS_SQ_ENERGY, (float)((uint32_t)w[5] | (uint32_t)w[6] << 16));
    sens_setv(s, 4, "freq", AOS_SQ_FREQ, (float)w[7] * 0.1f);
    sens_setv(s, 5, "pf", AOS_SQ_RAW, (float)w[8] * 0.01f);
    sens_setv(s, 6, "alarm", AOS_SQ_STATE, w[9] == 0xFFFF ? 1.0f : 0.0f);
    return R_OK;
}

const sens_drv_t DRV_PZEM004T = {
    .prio = SP_REG,
    .cat = SC(POWER),
    .pub = { "pzem004t", "PZEM-004T", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_UART, 0 },
    .flags = SF_NOPROBE, .baud = 9600,
    .read = pzem_read,
};

/* ---- HLK-LD2410 / B / C: 24 GHz presence radar ----
 * Hi-Link's serial protocol (V1.02) and ESPHome's ld2410: at 256000 baud
 * it streams report frames F4 F3 F2 F1, a little-endian length, the data
 * and F8 F7 F6 F5, about ten a second. The data: its type (2 basic, 1
 * engineering, which adds the gates' energies), AA, the target state (0
 * none, 1 moving, 2 still, 3 both), the moving target's distance (cm, LE)
 * and energy (0-100), the still one's, the detection distance (cm), then
 * 55 00. No checksum: the head, the tail and the frame's ends are what
 * prove it. Distances of a target that is not there go out as NaN. */

static int ld2410_pub(sens_slot_t *s, const uint8_t *f, int n)
{
    (void)n;
    const uint8_t *d = f + 6;
    uint8_t st = d[2];
    sens_setv(s, 0, "presence", AOS_SQ_STATE, st ? 1.0f : 0.0f);
    sens_setv(s, 1, "dist", AOS_SQ_DIST, st ? (float)sens_le16(d + 9) * 10.0f : NAN);
    sens_setv(s, 2, "move", AOS_SQ_STATE, st & 1 ? 1.0f : 0.0f);
    sens_setv(s, 3, "still", AOS_SQ_STATE, st & 2 ? 1.0f : 0.0f);
    sens_setv(s, 4, "move_dist", AOS_SQ_DIST, st & 1 ? (float)sens_le16(d + 3) * 10.0f : NAN);
    sens_setv(s, 5, "still_dist", AOS_SQ_DIST, st & 2 ? (float)sens_le16(d + 6) * 10.0f : NAN);
    sens_setv(s, 6, "move_en", AOS_SQ_RAW, (float)d[5]);
    sens_setv(s, 7, "still_en", AOS_SQ_RAW, (float)d[8]);
    return R_OK;
}

static int ld2410_read(sens_io_t *io, sens_slot_t *s)
{
    static const uint8_t H[4] = { 0xF4, 0xF3, 0xF2, 0xF1 };
    aos_io_uart_t *u = io->uart;
    u_flush(u);
    uint32_t until = sens_now() + 400;
    int seen = 0, bad = 0;
    uint8_t f[64] = { 0xF4, 0xF3, 0xF2, 0xF1 };
    while (u_sync(u, H, 4, until, &seen)) {
        if (u_read(u, f + 4, 2, u_tail(until, 2, 256000)) != 2) break;
        int len = sens_le16(f + 4);
        if (len < 13 || len > (int)sizeof f - 10) { bad++; continue; }
        if (u_read(u, f + 6, len + 4, u_tail(until, len + 4, 256000)) != len + 4) { bad++; break; }
        const uint8_t *d = f + 6, *e = f + 6 + len;
        if ((d[0] != 1 && d[0] != 2) || d[1] != 0xAA || d[len - 2] != 0x55 || d[len - 1] != 0x00 ||
            e[0] != 0xF8 || e[1] != 0xF7 || e[2] != 0xF6 || e[3] != 0xF5) { bad++; continue; }
        return keep_frame(s, f, 10 + len, ld2410_pub);
    }
    return held(s, 1500, seen, bad, ld2410_pub);
}

const sens_drv_t DRV_LD2410 = {
    .prio = SP_REG,
    .cat = SC(DIST),
    .pub = { "ld2410", "LD2410", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_UART, 0 },
    .flags = SF_NOPROBE, .baud = 256000,
    .read = ld2410_read,
};

/* ---- HLK-LD2450: 24 GHz radar that tracks up to three people ----
 * Hi-Link's protocol and ESPHome's ld2450: at 256000 baud, about ten frames
 * a second, AA FF 03 00, three targets of eight bytes (x, y in mm, speed in
 * cm/s, the distance gate's size in mm, each little-endian with the top
 * bit as the sign: set is positive) and 55 CC. A target that is not there
 * is all zeros. "dist" is the nearest one's. */

static float ld2450_v(const uint8_t *p)
{
    float v = (float)((p[1] & 0x7F) << 8 | p[0]);
    return p[1] & 0x80 ? v : -v;
}

static int ld2450_pub(sens_slot_t *s, const uint8_t *f, int n)
{
    (void)n;
    static const char *const KX[3] = { "x1", "x2", "x3" }, *const KY[3] = { "y1", "y2", "y3" },
                      *const KS[3] = { "speed1", "speed2", "speed3" };
    int people = 0;
    float near = NAN;
    for (int i = 0; i < 3; i++) {
        const uint8_t *t = f + 4 + 8 * i;
        float x = ld2450_v(t), y = ld2450_v(t + 2), sp = ld2450_v(t + 4);
        bool on = x != 0.0f || y != 0.0f;
        if (on) {
            people++;
            float d = sqrtf(x * x + y * y);
            if (isnan(near) || d < near) near = d;
        }
        sens_setv(s, 3 + 3 * i, KX[i], AOS_SQ_DIST, on ? x : NAN);
        sens_setv(s, 4 + 3 * i, KY[i], AOS_SQ_DIST, on ? y : NAN);
        sens_setv(s, 5 + 3 * i, KS[i], AOS_SQ_SPEED, on ? sp / 100.0f : NAN);
    }
    sens_setv(s, 0, "presence", AOS_SQ_STATE, people ? 1.0f : 0.0f);
    sens_setv(s, 1, "people", AOS_SQ_RAW, (float)people);
    sens_setv(s, 2, "dist", AOS_SQ_DIST, near);
    return R_OK;
}

static int ld2450_read(sens_io_t *io, sens_slot_t *s)
{
    static const uint8_t H[4] = { 0xAA, 0xFF, 0x03, 0x00 };
    aos_io_uart_t *u = io->uart;
    u_flush(u);
    uint32_t until = sens_now() + 400;
    int seen = 0, bad = 0;
    uint8_t f[30] = { 0xAA, 0xFF, 0x03, 0x00 };
    while (u_sync(u, H, 4, until, &seen)) {
        if (u_read(u, f + 4, 26, u_tail(until, 26, 256000)) != 26) { bad++; break; }
        if (f[28] != 0x55 || f[29] != 0xCC) { bad++; continue; }
        return keep_frame(s, f, 30, ld2450_pub);
    }
    return held(s, 1500, seen, bad, ld2450_pub);
}

const sens_drv_t DRV_LD2450 = {
    .prio = SP_REG,
    .cat = SC(DIST),
    .pub = { "ld2450", "LD2450", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_UART, 0 },
    .flags = SF_NOPROBE, .baud = 256000,
    .read = ld2450_read,
};

const sens_drv_t *const SENS_G_UART[] = {
    &DRV_MHZ19, &DRV_SENSEAIR_S8, &DRV_PMS5003, &DRV_SDS011, &DRV_PZEM004T, &DRV_LD2410, &DRV_LD2450,
    NULL,
};

const sens_alias_t SENS_A_UART[] = {
    { "mhz19b", "mhz19" }, { "mhz19c", "mhz19" }, { "mhz19e", "mhz19" },
    { "s8", "senseair_s8" }, { "senseair", "senseair_s8" },
    { "pms7003", "pms5003" }, { "pmsa003", "pms5003" }, { "pms3003", "pms5003" },
    { "pzem", "pzem004t" },
    { "ld2410b", "ld2410" }, { "ld2410c", "ld2410" },
    { NULL, NULL },
};
