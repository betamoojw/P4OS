/*
 * P4OS simulator - emulated sensors on a serial port, for the sensor
 * service's UART drivers (components/aos_io/sensors/drv_uart.c).
 *
 * P4_SIM_UART_A=sensor:<model> (and _B: P4_SIM_<PORT>, as io_sim.c names
 * them) puts one of these on the port instead of the make-believe ESP32:
 *
 *   mhz19                   NDIR CO2, answers FF 01 86 ...
 *   senseair_s8             NDIR CO2, Modbus RTU at 0xFE (any) or 0x68
 *   pms5003 pms7003 pmsa003 particles, a frame every 2.3 s (faster while
 *                           the air changes); pms5003p starts in passive
 *                           mode; pms3003 has the short frame and no commands
 *   sds011                  particles, a frame a second; sds011q in query mode
 *   pzem004t                mains meter, Modbus RTU at 0x01 or 0xF8
 *   ld2410                  presence radar at 256000 baud, ten basic frames
 *                           a second; ld2410e sends engineering frames
 *   ld2450                  radar that tracks up to three people
 *
 * The line is timed as a real one: each byte the sensor sends lands at the
 * moment its ten bits have gone by at the sensor's baud, a stream keeps
 * going while the port is closed (what it sent then is lost, and the open
 * may fall in the middle of a frame), and a request is answered after the
 * request's own bytes and the sensor's latency. A port opened at another
 * baud than the sensor's gets garbage and the sensor does not understand
 * it. The state lives with the port, not with the open: a PZEM's energy
 * keeps counting, a PMS put in passive mode stays there.
 *
 * The readings: CO2 between 450 and 900 ppm over ten minutes, a few µg/m³
 * of particles with somebody cooking for a minute every four, mains at
 * 220-230 V and 50 Hz with a fridge, a TV and a kettle, and a person who
 * walks up to the radar, sits for a while and leaves, once a minute.
 */
#include "sensors_emu.h"

#include "aos_hal.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PI 3.141592653589793

typedef struct udev udev_t;

typedef struct {
    const char *name;
    uint32_t baud;
    int variant;
    /* a streaming sensor's next frame, started now: its length (0: none
     * this time), and *next_ms until the one after */
    int  (*frame)(udev_t *d, uint8_t *out, uint32_t *next_ms);
    /* a byte reached the sensor's RX; the last bytes are in d->in */
    void (*rx)(udev_t *d);
} umodel_t;

#define QN 4096

struct udev {
    const umodel_t *m;
    char     port[16];
    bool     used, open, garbled;
    uint64_t open_us;
    uint64_t next_us;               /* the stream's next frame */
    uint64_t line_us;               /* the sensor's TX is busy until then */
    uint8_t  q[QN];                 /* what is on its way to the P4 */
    uint64_t qt[QN];                /* when each byte has arrived */
    int      qh, qn;
    uint8_t  in[64];                /* the last bytes the sensor received */
    int      in_n;
    int      mode;                  /* the model's: passive, query, asleep */
    bool     asleep;
    double   wh;                    /* the PZEM's energy */
    double   last_s;
};

static udev_t s_dev[4];
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint32_t byte_us(const udev_t *d) { return 10000000u / d->m->baud; }

/* n bytes from the sensor, the first starting at t (or when its line is free) */
static void send_at(udev_t *d, const uint8_t *b, int n, uint64_t t)
{
    if (t < d->line_us) t = d->line_us;
    uint32_t bu = byte_us(d);
    for (int i = 0; i < n; i++) {
        uint64_t at = t + (uint64_t)(i + 1) * bu;
        if (!d->open || at < d->open_us || d->qn >= QN) continue;      /* nobody listening */
        int k = (d->qh + d->qn++) % QN;
        /* at the wrong baud the P4 samples something else */
        d->q[k] = d->garbled ? (uint8_t)(b[i] * 37u + 11u) | (uint8_t)(i & 1 ? 0x80 : 0) : b[i];
        d->qt[k] = at;
    }
    d->line_us = t + (uint64_t)n * bu;
}

static void reply(udev_t *d, const uint8_t *b, int n, uint32_t ms) { send_at(d, b, n, now_us() + (uint64_t)ms * 1000u); }

/* The stream, up to now */
static void pump(udev_t *d, uint64_t now)
{
    if (!d->m->frame) return;
    if (d->next_us + 3000000u < now) d->next_us = now - 3000000u;
    while (d->next_us <= now) {
        uint8_t f[64];
        uint32_t next = 100;
        int n = d->m->frame(d, f, &next);
        if (n > 0) send_at(d, f, n, d->next_us);
        d->next_us += (uint64_t)next * 1000u;
    }
}

/* the last n bytes received, or NULL */
static const uint8_t *last(const udev_t *d, int n) { return d->in_n >= n ? d->in + d->in_n - n : NULL; }

/* ---- the room ---- */

static double co2_ppm(double t) { return 675.0 + 200.0 * sin(2 * PI * t / 600.0) + 20.0 * sin(2 * PI * t / 97.0) + sim_noise(4.0); }

static double cooking(double t)
{
    double e = fmod(t + 150.0, 240.0);
    if (e >= 60.0) return 0;
    double s = sin(PI * e / 60.0);
    return s * s;
}

static double pm25(double t) { double v = 6.0 + 2.0 * sin(2 * PI * t / 90.0) + 22.0 * cooking(t) + sim_noise(0.3); return v < 0 ? 0 : v; }

static uint16_t u16r(double v) { return v <= 0 ? 0 : v >= 65535.0 ? 65535 : (uint16_t)lround(v); }

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- MH-Z19B ----
 * Winsen's datasheet: 9-byte commands starting FF 01, checksum the negated
 * sum of bytes 1-7. 0x86 reads CO2 (and the undocumented temperature + 40
 * in byte 4); the rest (calibration, ABC, range) are taken silently. */

static uint8_t mhz_sum(const uint8_t *p)
{
    uint8_t s = 0;
    for (int i = 1; i < 8; i++) s = (uint8_t)(s + p[i]);
    return (uint8_t)(0u - s);
}

static void mhz19_rx(udev_t *d)
{
    const uint8_t *c = last(d, 9);
    if (!c || c[0] != 0xFF || c[1] != 0x01 || mhz_sum(c) != c[8]) return;
    d->in_n = 0;
    if (c[2] != 0x86) return;
    double t = sim_now_s();
    uint8_t r[9] = { 0xFF, 0x86 };
    be16(r + 2, u16r(co2_ppm(t)));
    r[4] = (uint8_t)lround(sim_room_t(t) + 2.5 + 40.0);      /* its own heat */
    r[8] = mhz_sum(r);
    reply(d, r, 9, 6);
}

/* ---- Modbus RTU, shared by the S8 and the PZEM ---- */

static uint16_t crc16(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (uint16_t)(c & 1 ? (c >> 1) ^ 0xA001 : c >> 1);
    }
    return c;
}

static void mb_send(udev_t *d, uint8_t *r, int n, uint32_t ms)
{
    uint16_t c = crc16(r, n);
    r[n] = (uint8_t)c;
    r[n + 1] = (uint8_t)(c >> 8);
    reply(d, r, n + 2, ms);
}

static void mb_exc(udev_t *d, uint8_t addr, uint8_t fn, uint8_t code, uint32_t ms)
{
    uint8_t r[5] = { addr, (uint8_t)(fn | 0x80), code };
    mb_send(d, r, 3, ms);
}

/* The 8-byte request that just ended, CRC checked, for one of these addresses */
static const uint8_t *mb_req(udev_t *d, uint8_t a1, uint8_t a2)
{
    const uint8_t *q = last(d, 8);
    if (!q || (q[0] != a1 && q[0] != a2) || crc16(q, 6) != (uint16_t)(q[6] | q[7] << 8)) return NULL;
    d->in_n = 0;
    return q;
}

static void mb_words(udev_t *d, uint8_t addr, uint8_t fn, const uint16_t *w, int n, uint32_t ms)
{
    uint8_t r[3 + 2 * 32 + 2] = { addr, fn, (uint8_t)(2 * n) };
    for (int i = 0; i < n; i++) be16(r + 3 + 2 * i, w[i]);
    mb_send(d, r, 3 + 2 * n, ms);
}

/* ---- Senseair S8 ----
 * "Modbus on Senseair S8": input registers IR1-IR4 (meter status, alarm
 * status, output status, CO2) at 0-3, holding registers HR1-HR... answered
 * as zeros. A new CO2 value every two seconds. */

static void s8_rx(udev_t *d)
{
    uint8_t q[8];
    const uint8_t *p = mb_req(d, 0xFE, 0x68);
    if (!p) return;
    memcpy(q, p, 8);
    uint16_t reg = (uint16_t)(q[2] << 8 | q[3]), n = (uint16_t)(q[4] << 8 | q[5]);
    if (q[1] != 0x03 && q[1] != 0x04) { mb_exc(d, q[0], q[1], 1, 15); return; }
    if (n < 1 || n > 32 || reg + n > 32) { mb_exc(d, q[0], q[1], 2, 15); return; }
    uint16_t ir[32] = { 0 };
    if (q[1] == 0x04) {
        double t = floor(sim_now_s() / 2.0) * 2.0;
        ir[3] = u16r(co2_ppm(t));
        ir[21] = ir[3];                         /* IR22: the PWM output, in ppm */
        ir[25] = 0x0001;                        /* IR26/IR27: the sensor type */
    }
    mb_words(d, q[0], q[1], ir + reg, n, 15);
}

/* ---- PZEM-004T v3.0 ----
 * Peacefair's manual: input registers 0-9 (V, I low/high, P low/high,
 * Wh low/high, Hz, PF, alarm), holding 1 (power alarm threshold, W) and 2
 * (address), 0x42 resets the energy. Address 0x01, or 0xF8 for whoever. */

static void mains(double t, double *v, double *a, double *w, double *hz, double *pf)
{
    *v = 225.0 + 3.5 * sin(2 * PI * t / 300.0) + sim_noise(0.3);
    *hz = 50.0 + 0.06 * sin(2 * PI * t / 47.0) + sim_noise(0.01);
    double p = 0, sva = 0;
    double fridge = fmod(t, 90.0) < 40.0 ? 110.0 + sim_noise(2.0) : 0.0;     /* compressor, pf 0.78 */
    double tv = 62.0 + sim_noise(1.0);                                       /* pf 0.92 */
    double kettle = fmod(t + 60.0, 180.0) < 30.0 ? 1950.0 + sim_noise(5.0) : 0.0;
    p += fridge + tv + kettle;
    sva += fridge / 0.78 + tv / 0.92 + kettle;
    *w = p;
    *pf = p / sva;
    *a = sva / *v;
}

static void pzem_rx(udev_t *d)
{
    uint8_t q[8];
    const uint8_t *p = mb_req(d, 0x01, 0xF8);
    if (!p) {
        /* the energy reset is four bytes: address, 0x42, CRC */
        const uint8_t *r = last(d, 4);
        if (r && (r[0] == 0x01 || r[0] == 0xF8) && r[1] == 0x42 && crc16(r, 2) == (uint16_t)(r[2] | r[3] << 8)) {
            uint8_t a[4] = { r[0], 0x42 };
            d->in_n = 0;
            d->wh = 0;
            mb_send(d, a, 2, 40);
        }
        return;
    }
    memcpy(q, p, 8);
    uint16_t reg = (uint16_t)(q[2] << 8 | q[3]), n = (uint16_t)(q[4] << 8 | q[5]);
    double t = sim_now_s(), v, a, w, hz, pf;
    mains(t, &v, &a, &w, &hz, &pf);
    if (d->last_s > 0 && t > d->last_s) d->wh += w * (t - d->last_s) / 3600.0;
    d->last_s = t;
    if (q[1] == 0x04) {
        if (n < 1 || reg + n > 10) { mb_exc(d, q[0], 0x04, 2, 40); return; }
        uint32_t ma = (uint32_t)lround(a * 1000.0), dw = (uint32_t)lround(w * 10.0), wh = (uint32_t)d->wh;
        uint16_t ir[10] = {
            u16r(v * 10.0), (uint16_t)ma, (uint16_t)(ma >> 16), (uint16_t)dw, (uint16_t)(dw >> 16),
            (uint16_t)wh, (uint16_t)(wh >> 16), u16r(hz * 10.0), u16r(pf * 100.0), w >= 2300.0 ? 0xFFFF : 0x0000,
        };
        mb_words(d, q[0], 0x04, ir + reg, n, 40);
    } else if (q[1] == 0x03) {
        if (n < 1 || reg < 1 || reg + n > 3) { mb_exc(d, q[0], 0x03, 2, 40); return; }
        uint16_t hr[3] = { 0, 2300, 0x01 };
        mb_words(d, q[0], 0x03, hr + reg, n, 40);
    } else {
        mb_exc(d, q[0], q[1], 1, 40);
    }
}

/* ---- Plantower PMS5003 family ----
 * Frames 42 4D, length, words, 16-bit sum. Active mode: every 2.3 s while
 * steady, 0.7 s while the air changes. Commands 42 4D cmd dH dL sumH sumL:
 * E1 mode (0 passive, 1 active), E2 read (passive), E4 sleep (0) / wake (1);
 * mode and sleep are acknowledged with a short frame. The PMS3003 takes no
 * commands. */

enum { V_PMS5003 = 0, V_PMS5003P, V_PMS3003 };

static int pms_build(uint8_t *f, bool short_frame)
{
    double t = sim_now_s(), p = pm25(t);
    double atm[3] = { 0.68 * p, p, 1.3 * p + 1.0 };
    int len = short_frame ? 20 : 28;
    memset(f, 0, 4 + (size_t)len);
    f[0] = 0x42; f[1] = 0x4D;
    be16(f + 2, (uint16_t)len);
    for (int i = 0; i < 3; i++) {
        double cf1 = atm[i] > 30.0 ? atm[i] * 1.35 : atm[i];          /* CF=1 only parts from atmospheric higher up */
        be16(f + 4 + 2 * i, u16r(cf1));
        be16(f + 10 + 2 * i, u16r(atm[i]));
    }
    if (!short_frame) {
        double n03 = 60.0 + 115.0 * p;
        double c[6] = { n03, n03 * 0.3, n03 * 0.055, n03 * 0.006, n03 * 0.0018, n03 * 0.0006 };
        for (int i = 0; i < 6; i++) be16(f + 16 + 2 * i, u16r(c[i]));
        f[28] = 0x97;                                                   /* version, error 0 */
    }
    uint16_t sum = 0;
    for (int i = 0; i < 2 + len; i++) sum = (uint16_t)(sum + f[i]);
    be16(f + 2 + len, sum);
    return 4 + len;
}

static int pms_frame(udev_t *d, uint8_t *out, uint32_t *next)
{
    if (d->mode || d->asleep) { *next = 200; return 0; }
    double t = sim_now_s();
    *next = fabs(pm25(t) - pm25(t - 2.3)) > 2.5 ? 700 : 2300;
    return pms_build(out, d->m->variant == V_PMS3003);
}

static void pms_rx(udev_t *d)
{
    if (d->m->variant == V_PMS3003) { d->in_n = 0; return; }
    const uint8_t *c = last(d, 7);
    if (!c || c[0] != 0x42 || c[1] != 0x4D) return;
    uint16_t sum = 0;
    for (int i = 0; i < 5; i++) sum = (uint16_t)(sum + c[i]);
    if (sum != (uint16_t)(c[5] << 8 | c[6])) return;
    uint8_t cmd = c[2], v = c[4];
    d->in_n = 0;
    if (cmd == 0xE2) {
        if (d->mode && !d->asleep) { uint8_t f[40]; reply(d, f, pms_build(f, false), 20); }
        return;
    }
    if (cmd != 0xE1 && cmd != 0xE4) return;
    if (cmd == 0xE1) d->mode = v ? 0 : 1;
    else d->asleep = !v;
    uint8_t a[8] = { 0x42, 0x4D, 0x00, 0x04, cmd, v };
    uint16_t s = 0;
    for (int i = 0; i < 6; i++) s = (uint16_t)(s + a[i]);
    be16(a + 6, s);
    reply(d, a, 8, 20);
}

/* ---- Nova SDS011 ----
 * Data frames AA C0 pm25(LE, 0.1) pm10 id id sum AB, one a second in
 * active mode. Commands are 19 bytes, AA B4 cmd data[12] id id sum AB,
 * the sum over bytes 2-16: 02 reporting mode (data1 1 = set, data2 0
 * active / 1 query), 04 query, 06 sleep (data2 0) / work (1); the settings
 * are answered with AA C5 frames. */

enum { V_SDS = 0, V_SDSQ };

static int sds_build(uint8_t *f)
{
    double t = sim_now_s(), p = pm25(t) * 1.08;
    f[0] = 0xAA; f[1] = 0xC0;
    le16(f + 2, u16r(p * 10.0));
    le16(f + 4, u16r((p * 1.6 + 1.0) * 10.0));
    f[6] = 0xA1; f[7] = 0x60;
    uint8_t s = 0;
    for (int i = 2; i < 8; i++) s = (uint8_t)(s + f[i]);
    f[8] = s;
    f[9] = 0xAB;
    return 10;
}

static int sds_frame(udev_t *d, uint8_t *out, uint32_t *next)
{
    *next = 1000;
    if (d->mode || d->asleep) return 0;
    return sds_build(out);
}

static void sds_rx(udev_t *d)
{
    const uint8_t *c = last(d, 19);
    if (!c || c[0] != 0xAA || c[1] != 0xB4 || c[18] != 0xAB) return;
    uint8_t s = 0;
    for (int i = 2; i < 17; i++) s = (uint8_t)(s + c[i]);
    if (s != c[17]) return;
    uint8_t cmd = c[2], set = c[3], v = c[4];
    d->in_n = 0;
    if (cmd == 0x04) {
        if (d->mode && !d->asleep) { uint8_t f[10]; reply(d, f, sds_build(f), 10); }
        return;
    }
    if (cmd != 0x02 && cmd != 0x06) return;
    if (set && cmd == 0x02) d->mode = v ? 1 : 0;
    if (set && cmd == 0x06) d->asleep = !v;
    uint8_t a[10] = { 0xAA, 0xC5, cmd, set, cmd == 0x02 ? (uint8_t)d->mode : (uint8_t)!d->asleep, 0, 0xA1, 0x60, 0, 0xAB };
    for (int i = 2; i < 8; i++) a[8] = (uint8_t)(a[8] + a[i]);
    reply(d, a, 10, 10);
}

/* ---- HLK-LD2410 ----
 * Report frames F4 F3 F2 F1, LE length, type (2 basic, 1 engineering), AA,
 * state, moving cm + energy, still cm + energy, detection cm, [engineering:
 * max gates, energies per gate, light, OUT], 55 00, F8 F7 F6 F5. About ten
 * a second. Somebody walks in from 6 m to 1.5 m, sits there (still, with a
 * fidget now and then), walks out; once a minute. The configuration frames
 * (FD FC FB FA) are not emulated: the driver sends none. */

enum { V_LD_BASIC = 0, V_LD_ENG };

static void person(double t, int *st, double *mcm, double *scm)
{
    double e = fmod(t, 60.0);
    *st = 0; *mcm = 0; *scm = 0;
    if (e >= 8.0 && e < 18.0) { *st = 1; *mcm = 600.0 - 45.0 * (e - 8.0); }
    else if (e >= 18.0 && e < 42.0) {
        *st = 2; *scm = 150.0 + sim_noise(4.0);
        if ((e >= 25.0 && e < 27.0) || (e >= 35.0 && e < 36.0)) { *st = 3; *mcm = 145.0 + sim_noise(6.0); }
    } else if (e >= 42.0 && e < 52.0) { *st = 1; *mcm = 150.0 + 45.0 * (e - 42.0); }
}

static int ld2410_frame(udev_t *d, uint8_t *f, uint32_t *next)
{
    *next = 100;
    bool eng = d->m->variant == V_LD_ENG;
    int st;
    double mcm, scm;
    person(sim_now_s(), &st, &mcm, &scm);
    int len = eng ? 35 : 13;
    memset(f, 0, 10 + (size_t)len);
    f[0] = 0xF4; f[1] = 0xF3; f[2] = 0xF2; f[3] = 0xF1;
    le16(f + 4, (uint16_t)len);
    uint8_t *p = f + 6;
    p[0] = eng ? 0x01 : 0x02;
    p[1] = 0xAA;
    p[2] = (uint8_t)st;
    uint8_t me = st & 1 ? (uint8_t)(70 + sim_noise(15.0)) : 0, se = st & 2 ? (uint8_t)(45 + sim_noise(8.0)) : 0;
    le16(p + 3, u16r(mcm)); p[5] = me;
    le16(p + 6, u16r(scm)); p[8] = se;
    le16(p + 9, u16r(st & 1 ? mcm : scm));
    int k = 11;
    if (eng) {
        p[k++] = 8; p[k++] = 8;                                 /* the farthest gates, 0.75 m each */
        for (int g = 0; g <= 8; g++) p[k++] = (st & 1) && fabs(mcm - g * 75.0) < 75.0 ? me : (uint8_t)(4 + g);
        for (int g = 0; g <= 8; g++) p[k++] = (st & 2) && fabs(scm - g * 75.0) < 75.0 ? se : (uint8_t)(2 + g);
        p[k++] = 90;                                            /* light */
        p[k++] = st ? 1 : 0;                                    /* OUT */
    }
    p[k++] = 0x55;
    p[k++] = 0x00;
    uint8_t *e = p + len;
    e[0] = 0xF8; e[1] = 0xF7; e[2] = 0xF6; e[3] = 0xF5;
    return 10 + len;
}

/* ---- HLK-LD2450 ----
 * AA FF 03 00, three targets (x, y mm, speed cm/s, gate size mm; LE, the
 * top bit set for positive), 55 CC, about ten a second. One person crosses
 * the room for 30 s of every 45, another stands still at the back from
 * 15 s to 40 s. */

static void ld2450_put(uint8_t *p, double v)
{
    long r = lround(fabs(v));
    if (r > 0x7FFF) r = 0x7FFF;
    le16(p, (uint16_t)(v >= 0 ? (uint16_t)r | 0x8000 : (uint16_t)r));
}

static void walker(double t, double *x, double *y)
{
    *x = -1800.0 + 120.0 * t;
    *y = 1500.0 + 600.0 * sin(2 * PI * t / 30.0);
}

static int ld2450_frame(udev_t *d, uint8_t *f, uint32_t *next)
{
    (void)d;
    *next = 100;
    double t = sim_now_s(), e = fmod(t, 45.0);
    memset(f, 0, 30);
    f[0] = 0xAA; f[1] = 0xFF; f[2] = 0x03; f[3] = 0x00;
    if (e < 30.0) {
        double x, y, x2, y2;
        walker(e, &x, &y);
        walker(e + 0.1, &x2, &y2);
        double sp = (sqrt(x2 * x2 + y2 * y2) - sqrt(x * x + y * y)) / 0.1 / 10.0;     /* radial, cm/s */
        ld2450_put(f + 4, x); ld2450_put(f + 6, y); ld2450_put(f + 8, sp); le16(f + 10, 360);
    }
    if (e >= 15.0 && e < 40.0) {
        ld2450_put(f + 12, 800.0 + sim_noise(15.0)); ld2450_put(f + 14, 3200.0 + sim_noise(15.0));
        ld2450_put(f + 16, 0); le16(f + 18, 360);
    }
    f[28] = 0x55; f[29] = 0xCC;
    return 30;
}

static void ignore_rx(udev_t *d) { if (d->in_n >= (int)sizeof d->in) d->in_n = 0; }

static const umodel_t MODELS[] = {
    { "mhz19",       9600,   0,           NULL,         mhz19_rx },
    { "senseair_s8", 9600,   0,           NULL,         s8_rx },
    { "pms5003",     9600,   V_PMS5003,   pms_frame,    pms_rx },
    { "pms7003",     9600,   V_PMS5003,   pms_frame,    pms_rx },
    { "pmsa003",     9600,   V_PMS5003,   pms_frame,    pms_rx },
    { "pms5003p",    9600,   V_PMS5003P,  pms_frame,    pms_rx },
    { "pms3003",     9600,   V_PMS3003,   pms_frame,    pms_rx },
    { "sds011",      9600,   V_SDS,       sds_frame,    sds_rx },
    { "sds011q",     9600,   V_SDSQ,      sds_frame,    sds_rx },
    { "pzem004t",    9600,   0,           NULL,         pzem_rx },
    { "ld2410",      256000, V_LD_BASIC,  ld2410_frame, ignore_rx },
    { "ld2410e",     256000, V_LD_ENG,    ld2410_frame, ignore_rx },
    { "ld2450",      256000, 0,           ld2450_frame, ignore_rx },
};

/* ---- the port (io_sim.c's hook) ---- */

void *sim_uemu_open(const char *port, const char *model, uint32_t baud)
{
    const umodel_t *m = NULL;
    for (size_t i = 0; i < sizeof MODELS / sizeof MODELS[0]; i++) if (!strcmp(MODELS[i].name, model)) m = &MODELS[i];
    if (!m) {
        aos_hal_log("io", "%s: no emulated sensor \"%s\"", port, model);
        return NULL;
    }
    pthread_mutex_lock(&s_mx);
    udev_t *d = NULL;
    for (int i = 0; i < 4 && !d; i++) if (s_dev[i].used && !strcmp(s_dev[i].port, port)) d = &s_dev[i];
    for (int i = 0; i < 4 && !d; i++) if (!s_dev[i].used) d = &s_dev[i];
    if (!d) { pthread_mutex_unlock(&s_mx); return NULL; }
    uint64_t now = now_us();
    if (!d->used || d->m != m) {
        memset(d, 0, sizeof *d);
        d->used = true;
        d->m = m;
        snprintf(d->port, sizeof d->port, "%s", port);
        d->mode = m->variant == V_PMS5003P && m->frame == pms_frame ? 1 : m->variant == V_SDSQ && m->frame == sds_frame ? 1 : 0;
        d->next_us = now + (uint64_t)((sim_noise(0.5) + 0.5) * 300000.0);     /* powered on a moment ago */
        d->wh = 15234.0;            /* a PZEM that has been metering for a while */
        aos_hal_log("sim", "emulated UART sensor on %s: %s at %u baud", port, m->name, (unsigned)m->baud);
    }
    pump(d, now);                   /* what it sent while closed goes by unheard */
    d->open = true;
    d->open_us = now;
    d->garbled = baud != m->baud;
    d->qh = d->qn = 0;
    d->in_n = 0;
    pthread_mutex_unlock(&s_mx);
    return d;
}

int sim_uemu_read(void *h, void *buf, int len, int timeout_ms)
{
    udev_t *d = h;
    uint64_t until = now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    for (;;) {
        pthread_mutex_lock(&s_mx);
        uint64_t now = now_us();
        pump(d, now);
        int n = 0;
        while (n < len && d->qn && d->qt[d->qh] <= now) {
            ((uint8_t *)buf)[n++] = d->q[d->qh];
            d->qh = (d->qh + 1) % QN;
            d->qn--;
        }
        uint64_t wake = d->qn ? d->qt[d->qh] : UINT64_MAX;
        if (d->m->frame && d->next_us < wake) wake = d->next_us + byte_us(d);
        pthread_mutex_unlock(&s_mx);
        if (n || now >= until) return n;
        if (wake > until) wake = until;
        uint64_t w = wake > now ? wake - now : 0;
        usleep((useconds_t)(w < 200 ? 200 : w > 20000 ? 20000 : w));
    }
}

int sim_uemu_write(void *h, const void *buf, int len)
{
    udev_t *d = h;
    pthread_mutex_lock(&s_mx);
    const uint8_t *b = buf;
    for (int i = 0; i < len && !d->garbled; i++) {
        if (d->in_n == (int)sizeof d->in) { memmove(d->in, d->in + 1, sizeof d->in - 1); d->in_n--; }
        d->in[d->in_n++] = b[i];
        d->m->rx(d);
    }
    pthread_mutex_unlock(&s_mx);
    return len;
}

void sim_uemu_baud(void *h, uint32_t baud)
{
    udev_t *d = h;
    pthread_mutex_lock(&s_mx);
    d->garbled = baud != d->m->baud;
    pthread_mutex_unlock(&s_mx);
}

void sim_uemu_close(void *h)
{
    udev_t *d = h;
    pthread_mutex_lock(&s_mx);
    d->open = false;
    d->qn = 0;
    pthread_mutex_unlock(&s_mx);
}
