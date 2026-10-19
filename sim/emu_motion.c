/*
 * P4OS simulator - emulated motion sensors for the sensor service
 * (sensors_emu.h): accelerometers, gyroscopes, magnetometers and the
 * BNO055.
 *
 * The bench: a board lying on the desk, tilted a few degrees and turned by
 * hand now and then about its vertical axis, in a place where the earth's
 * field is 23 µT pointing north and 36° up (the southern hemisphere). From
 * that pose every chip works out what it would measure in its own axes -
 * the specific force (gravity, +1 g up), the rates of turn the pose's
 * changes make, the field - and encodes it with its datasheet's sensitivity
 * for the range its registers hold, so the driver's scaling is checked
 * against the chip's numbers, not its own. The ST parts use ST's
 * sensitivities (0.061 mg, 8.75 mdps...), which are not 32768/FS.
 *
 * What each chip does that a driver could get wrong is played too: the
 * MPU-6050 wakes asleep, the ICM-4268x reads -32768 from a sensor that is
 * off, ST's chips auto-increment only with bit 7 of the address (the
 * LSM6DS with IF_INC), the QMI8658 only with CTRL1's ADDR_AI and NACKs
 * while it resets, the BNO055 ignores configuration written outside CONFIG
 * mode, the Memsic chips measure only when told to.
 *
 * The magnetometer behind an IMU (AK8963 in an MPU-9250/9255, AK09916 in
 * an ICM-20948) is its own model at 0x0C ("mpu9250,ak8963"), which answers
 * only while an emulated IMU of the right kind has its I2C bypass on - as
 * the real one does. A "9250" whose chip says 0x70 is model mpu6500.
 */
#include "sensors_emu.h"

#include <math.h>
#include <string.h>

#define TAU 6.283185307179586
#define D2R (TAU / 360.0)

/* -------------------------------------------------------------------------- */
/* The bench                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    double a[3];                    /* g, the chip's axes (z up when flat) */
    double g[3];                    /* °/s */
    double m[3];                    /* µT */
    double heading, pitch, roll;    /* ° */
    double tc;                      /* the die's temperature, °C */
} bench_t;

/* the world: x north, y west, z up */
static const double FIELD[3] = { 18.6, 0.0, 13.5 };

/* v_body = (Rz(yaw) Ry(pitch) Rx(roll))^T v_world */
static void to_body(const double v[3], double yaw, double pitch, double roll, double o[3])
{
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double x = cy * v[0] + sy * v[1], y = -sy * v[0] + cy * v[1], z = v[2];
    double x2 = cp * x - sp * z, z2 = sp * x + cp * z;
    o[0] = x2;
    o[1] = cr * y + sr * z2;
    o[2] = -sr * y + cr * z2;
}

static void bench(bench_t *o)
{
    double t = sim_now_s();
    /* heading (clockwise from north, as a compass), pitch and roll, and
     * how fast they change */
    double w1 = TAU / 80.0, w2 = TAU / 23.0, w3 = TAU / 31.0;
    double h = 40.0 + 35.0 * sin(w1 * t), hd = 35.0 * w1 * cos(w1 * t);
    double p = 4.0 + 2.0 * sin(w2 * t), pd = 2.0 * w2 * cos(w2 * t);
    double r = -2.5 + 1.5 * sin(w3 * t), rd = 1.5 * w3 * cos(w3 * t);
    o->heading = fmod(h + 360.0, 360.0);
    o->pitch = p;
    o->roll = r;
    double yaw = -h * D2R, yd = -hd, th = p * D2R, ph = r * D2R;
    static const double UP[3] = { 0, 0, 1 };
    to_body(UP, yaw, th, ph, o->a);
    to_body(FIELD, yaw, th, ph, o->m);
    /* body rates of a Z-Y-X rotation, plus each gyro's bias */
    o->g[0] = rd - yd * sin(th) + 0.35 + sim_noise(0.05);
    o->g[1] = pd * cos(ph) + yd * sin(ph) * cos(th) - 0.20 + sim_noise(0.05);
    o->g[2] = -pd * sin(ph) + yd * cos(ph) * cos(th) + 0.12 + sim_noise(0.05);
    for (int i = 0; i < 3; i++) {
        o->a[i] += sim_noise(0.002);
        o->m[i] += sim_noise(0.08);
    }
    o->tc = sim_room_t(t) + 4.2;                   /* a die runs warmer than the room */
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static int16_t sat16(double v)
{
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)lrint(v);
}

static void be16(uint8_t *p, int16_t v) { p[0] = (uint8_t)((uint16_t)v >> 8); p[1] = (uint8_t)v; }
static void le16(uint8_t *p, int16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)((uint16_t)v >> 8); }

/* which of a model table's names this device is */
static int variant(const sim_emu_t *d, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) if (!strcmp(d->m->name, names[i])) return i;
    return 0;
}

/* An 8-bit register chip with the per-chip parts as callbacks. inc_bit:
 * 0 always auto-increments, else only when the address has that bit (ST).
 * u[15] keeps whether the last pointer write asked to. A chip that is
 * resetting NACKs until resp_ms. */
typedef struct {
    void    (*write)(sim_emu_t *d, uint8_t reg, uint8_t v);
    void    (*refresh)(sim_emu_t *d, uint8_t reg, size_t n);    /* before a read of n bytes from reg */
    uint8_t (*read)(sim_emu_t *d, uint8_t reg);                 /* NULL: regs[] */
    uint8_t inc_bit;
    bool    (*incr)(sim_emu_t *d);                              /* NULL: as inc_bit says */
} rw_ops_t;

static bool rw8(sim_emu_t *d, const rw_ops_t *o, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (sim_ms() < d->resp_ms) return false;
    if (wn) {
        uint8_t reg = w[0];
        bool inc = true;
        if (o->inc_bit) { inc = (reg & o->inc_bit) != 0; reg = (uint8_t)(reg & ~o->inc_bit); }
        if (o->incr) inc = o->incr(d);
        d->ptr = reg;
        d->u[15] = inc;
        for (size_t i = 1; i < wn; i++) {
            o->write(d, d->ptr, w[i]);
            if (sim_ms() < d->resp_ms) return true;            /* it went into reset */
            if (inc) d->ptr++;
        }
        if (wn > 1) return rn ? false : true;
    }
    if (!rn) return true;
    uint8_t reg = d->ptr;
    bool inc = d->u[15] != 0;
    if (o->refresh) o->refresh(d, reg, inc ? rn : 1);
    for (size_t i = 0; i < rn; i++) {
        r[i] = o->read ? o->read(d, reg) : d->regs[reg];
        if (inc) reg++;
    }
    d->ptr = reg;
    return true;
}

static bool overlaps(uint8_t reg, size_t n, uint8_t lo, uint8_t hi) { return reg <= hi && reg + n > lo; }

/* -------------------------------------------------------------------------- */
/* MPU-6050 / 6500 / 9250 / 9255 / 6886, ICM-20602 / 20608 / 20689            */
/* -------------------------------------------------------------------------- */

static const char *const MPU_NAME[] = { "mpu6050", "mpu6500", "mpu9250", "mpu9255", "mpu6886", "icm20602", "icm20608", "icm20689" };
static const struct { uint8_t who, pwr; double t_div, t_off; uint8_t st[3]; } MPU_V[] = {
    { 0x68, 0x40, 340.0, 36.53, { 0x81, 0x01, 0x00 } },            /* XG_OFFS_TC... */
    { 0x70, 0x01, 333.87, 21.0, { 0xC6, 0xC9, 0xB4 } },            /* SELF_TEST_X/Y/Z_GYRO */
    { 0x71, 0x01, 333.87, 21.0, { 0xC4, 0xCB, 0xB7 } },
    { 0x73, 0x01, 333.87, 21.0, { 0xC2, 0xC7, 0xBA } },
    { 0x19, 0x41, 326.8, 25.0, { 0x00, 0x00, 0x00 } },
    { 0x12, 0x41, 326.8, 25.0, { 0x00, 0x00, 0x00 } },
    { 0xAF, 0x41, 326.8, 25.0, { 0x00, 0x00, 0x00 } },
    { 0x98, 0x41, 326.8, 25.0, { 0x00, 0x00, 0x00 } },
};

/* the AK8963 is on the bus while an MPU-9250/9255 has its bypass on */
static int s_ak8963_on;

static void mpu_bypass(sim_emu_t *d)
{
    int v = (int)d->u[0];
    if (v == 2 || v == 3) s_ak8963_on = (d->regs[0x37] & 0x02) && !(d->regs[0x6A] & 0x20);
}

static void mpu_reset(sim_emu_t *d)
{
    int v = variant(d, MPU_NAME, 8);
    memset(d->regs, 0, sizeof d->regs);
    d->u[0] = (uint32_t)v;
    d->regs[0x75] = MPU_V[v].who;
    d->regs[0x6B] = MPU_V[v].pwr;
    memcpy(d->regs, MPU_V[v].st, 3);
    mpu_bypass(d);
}

static void mpu_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x75) return;
    if (reg == 0x6B && (v & 0x80)) { mpu_reset(d); return; }   /* DEVICE_RESET */
    d->regs[reg] = v;
    if (reg == 0x37 || reg == 0x6A) mpu_bypass(d);
}

static void mpu_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x3B, 0x48) || (d->regs[0x6B] & 0x40)) return;   /* asleep: the data stays */
    int v = (int)d->u[0];
    bench_t b;
    bench(&b);
    double a_cnt = 16384.0 / (1 << (d->regs[0x1C] >> 3 & 3));             /* 16384 LSB/g at ±2 g */
    double g_cnt = 131.0 / (1 << (d->regs[0x1B] >> 3 & 3));               /* 131 LSB/(°/s) at ±250 */
    for (int i = 0; i < 3; i++) {
        be16(&d->regs[0x3B + 2 * i], sat16(b.a[i] * a_cnt));
        be16(&d->regs[0x43 + 2 * i], sat16(b.g[i] * g_cnt));
    }
    be16(&d->regs[0x41], sat16((b.tc - MPU_V[v].t_off) * MPU_V[v].t_div));
}

static const rw_ops_t MPU_OPS = { mpu_write, mpu_refresh, NULL, 0, NULL };
static bool mpu_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &MPU_OPS, w, wn, r, rn); }

static const sim_model_t M_MPU6050 = { "mpu6050", 0x68, mpu_reset, mpu_xfer };
static const sim_model_t M_MPU6500 = { "mpu6500", 0x68, mpu_reset, mpu_xfer };
static const sim_model_t M_MPU9250 = { "mpu9250", 0x68, mpu_reset, mpu_xfer };
static const sim_model_t M_MPU9255 = { "mpu9255", 0x68, mpu_reset, mpu_xfer };
static const sim_model_t M_MPU6886 = { "mpu6886", 0x68, mpu_reset, mpu_xfer };
static const sim_model_t M_ICM20602 = { "icm20602", 0x68, mpu_reset, mpu_xfer };
static const sim_model_t M_ICM20608 = { "icm20608", 0x68, mpu_reset, mpu_xfer };
static const sim_model_t M_ICM20689 = { "icm20689", 0x68, mpu_reset, mpu_xfer };

/* -------------------------------------------------------------------------- */
/* AK8963 (behind an MPU-9250) and AK09916 (behind an ICM-20948), at 0x0C      */
/* -------------------------------------------------------------------------- */

static const uint8_t AK_ASA[3] = { 0xB0, 0xB3, 0xA6 };    /* a typical part's fuse ROM */

/* u[1]: when the current sample was taken (ms); u[2]: 1 while it waits to be read */
static void ak8963_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = 0x48;                          /* WIA */
    d->regs[0x01] = 0x9A;                          /* INFO */
    d->u[1] = 0;
    d->u[2] = 0;
}

static uint32_t ak8963_period(const sim_emu_t *d)
{
    switch (d->regs[0x0A] & 0x0F) {
    case 0x02: return 125;                         /* continuous mode 1, 8 Hz */
    case 0x06: return 10;                          /* continuous mode 2, 100 Hz */
    default: return 0;
    }
}

static void ak8963_sample(sim_emu_t *d)
{
    bench_t b;
    bench(&b);
    bool b16 = d->regs[0x0A] & 0x10;
    double lsb = b16 ? 0.15 : 0.6, lim = b16 ? 32760 : 8190;
    /* the AK8963's x is the MPU's y, its y the MPU's x, its z the opposite */
    double h[3] = { b.m[1], b.m[0], -b.m[2] };
    bool ovf = false;
    for (int i = 0; i < 3; i++) {
        double adj = (AK_ASA[i] - 128) / 256.0 + 1.0;
        double c = h[i] / (lsb * adj);
        if (fabs(c) > lim) ovf = true;
        le16(&d->regs[0x03 + 2 * i], sat16(c));
    }
    d->regs[0x09] = (uint8_t)((b16 ? 0x10 : 0) | (ovf ? 0x08 : 0));
    d->regs[0x02] = 0x01;                          /* DRDY */
    d->u[1] = (uint32_t)sim_ms();
    d->u[2] = 1;
}

static void ak8963_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x0B) { if (v & 1) ak8963_reset(d); return; }   /* CNTL2: SRST */
    if (reg == 0x0A) {
        d->regs[0x0A] = v;
        if ((v & 0x0F) == 0x01) { d->u[1] = (uint32_t)sim_ms(); d->u[2] = 2; }   /* single: 7.2 ms */
        return;
    }
    if (reg == 0x0C) d->regs[0x0C] = v & 0x40;
}

static void ak8963_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    (void)n;
    if (reg > 0x09) return;
    uint32_t now = (uint32_t)sim_ms(), per = ak8963_period(d);
    if (d->u[2] == 2 && now - d->u[1] >= 8) {      /* the single measurement is done: back to power-down */
        ak8963_sample(d);
        d->regs[0x0A] &= 0xF0;
    } else if (per && now - d->u[1] >= per && reg <= 0x02) {
        if (d->u[2] == 1 && now - d->u[1] >= per) d->regs[0x02] |= 0x02;   /* DOR: one was skipped */
        ak8963_sample(d);
    }
}

static uint8_t ak8963_read(sim_emu_t *d, uint8_t reg)
{
    if (reg >= 0x10 && reg <= 0x12) return (d->regs[0x0A] & 0x0F) == 0x0F ? AK_ASA[reg - 0x10] : 0x00;
    uint8_t v = d->regs[reg];
    if (reg == 0x09) { d->regs[0x02] = 0; d->u[2] = 0; }    /* reading ST2 ends the sample */
    return v;
}

static const rw_ops_t AK8963_OPS = { ak8963_write, ak8963_refresh, ak8963_read, 0, NULL };
static bool ak8963_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (!s_ak8963_on) return false;                /* behind the MPU, out of sight */
    return rw8(d, &AK8963_OPS, w, wn, r, rn);
}
static const sim_model_t M_AK8963 = { "ak8963", 0x0C, ak8963_reset, ak8963_xfer };

static int s_ak09916_on;

static void ak09916_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = 0x48;                          /* WIA1 */
    d->regs[0x01] = 0x09;                          /* WIA2 */
    d->u[1] = 0;
    d->u[2] = 0;
}

static uint32_t ak09916_period(const sim_emu_t *d)
{
    switch (d->regs[0x31] & 0x1F) {
    case 0x02: return 100;
    case 0x04: return 50;
    case 0x06: return 20;
    case 0x08: return 10;
    default: return 0;
    }
}

static void ak09916_sample(sim_emu_t *d)
{
    bench_t b;
    bench(&b);
    /* the AK09916's y and z are the ICM-20948's opposite */
    double h[3] = { b.m[0], -b.m[1], -b.m[2] };
    for (int i = 0; i < 3; i++) le16(&d->regs[0x11 + 2 * i], sat16(h[i] / 0.15));
    d->regs[0x18] = 0x00;
    d->regs[0x10] = 0x01;
    d->u[1] = (uint32_t)sim_ms();
    d->u[2] = 1;
}

static void ak09916_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x32) { if (v & 1) ak09916_reset(d); return; }
    if (reg == 0x31) {
        d->regs[0x31] = v & 0x1F;
        if ((v & 0x1F) == 0x01) { d->u[1] = (uint32_t)sim_ms(); d->u[2] = 2; }
    }
}

static void ak09916_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    (void)n;
    if (reg < 0x10 || reg > 0x18) return;
    uint32_t now = (uint32_t)sim_ms(), per = ak09916_period(d);
    if (d->u[2] == 2 && now - d->u[1] >= 9) {
        ak09916_sample(d);
        d->regs[0x31] = 0;
    } else if (per && now - d->u[1] >= per && reg == 0x10) {
        ak09916_sample(d);
    }
}

static uint8_t ak09916_read(sim_emu_t *d, uint8_t reg)
{
    uint8_t v = d->regs[reg];
    if (reg == 0x18) { d->regs[0x10] = 0; d->u[2] = 0; }
    return v;
}

static const rw_ops_t AK09916_OPS = { ak09916_write, ak09916_refresh, ak09916_read, 0, NULL };
static bool ak09916_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (!s_ak09916_on) return false;
    return rw8(d, &AK09916_OPS, w, wn, r, rn);
}
static const sim_model_t M_AK09916 = { "ak09916", 0x0C, ak09916_reset, ak09916_xfer };

/* -------------------------------------------------------------------------- */
/* ICM-20948: banks 0..3 behind REG_BANK_SEL (0x7F in every bank)             */
/* -------------------------------------------------------------------------- */

/* bank 0 in regs[], banks 1 and 2 in r16[]'s bytes, bank 3 not kept */
static uint8_t *icm20948_bank(sim_emu_t *d, int bank)
{
    if (bank == 0) return d->regs;
    if (bank <= 2) return (uint8_t *)d->r16 + 256 * (bank - 1);
    return NULL;
}

static void icm20948_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->r16, 0, sizeof d->r16);
    d->page = 0;
    d->regs[0x00] = 0xEA;
    d->regs[0x06] = 0x41;                          /* PWR_MGMT_1: SLEEP, auto clock */
    uint8_t *b2 = icm20948_bank(d, 2);
    b2[0x01] = 0x01;                               /* GYRO_CONFIG_1 */
    b2[0x14] = 0x01;                               /* ACCEL_CONFIG */
    s_ak09916_on = 0;
}

static void icm20948_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x7F) { d->page = (v >> 4) & 3; return; }
    if (d->page == 0) {
        if (reg == 0x00) return;
        if (reg == 0x06 && (v & 0x80)) { icm20948_reset(d); return; }
        d->regs[reg] = v;
        if (reg == 0x0F || reg == 0x03) s_ak09916_on = (d->regs[0x0F] & 0x02) && !(d->regs[0x03] & 0x20);
        return;
    }
    uint8_t *bk = icm20948_bank(d, d->page);
    if (bk) bk[reg] = v;
}

static void icm20948_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (d->page != 0 || !overlaps(reg, n, 0x2D, 0x3A) || (d->regs[0x06] & 0x40)) return;
    uint8_t *b2 = icm20948_bank(d, 2);
    bench_t b;
    bench(&b);
    double a_cnt = 16384.0 / (1 << (b2[0x14] >> 1 & 3));
    double g_cnt = 131.0 / (1 << (b2[0x01] >> 1 & 3));
    for (int i = 0; i < 3; i++) {
        be16(&d->regs[0x2D + 2 * i], sat16(b.a[i] * a_cnt));
        be16(&d->regs[0x33 + 2 * i], sat16(b.g[i] * g_cnt));
    }
    be16(&d->regs[0x39], sat16((b.tc - 21.0) * 333.87));
}

static uint8_t icm20948_read(sim_emu_t *d, uint8_t reg)
{
    if (reg == 0x7F) return (uint8_t)(d->page << 4);
    uint8_t *bk = icm20948_bank(d, d->page);
    return bk ? bk[reg] : 0;
}

static const rw_ops_t ICM20948_OPS = { icm20948_write, icm20948_refresh, icm20948_read, 0, NULL };
static bool icm20948_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &ICM20948_OPS, w, wn, r, rn); }
static const sim_model_t M_ICM20948 = { "icm20948", 0x69, icm20948_reset, icm20948_xfer };

/* -------------------------------------------------------------------------- */
/* ICM-42688-P and ICM-42670-P                                                 */
/* -------------------------------------------------------------------------- */

static const char *const ICM42_NAME[] = { "icm42688", "icm42670" };
/* where things are: reset, power, gyro and accel config, temperature/data */
static const struct { uint8_t who, rst_reg, rst_bit, pwr, gcfg, acfg, data; double t_lsb; uint8_t cfg_reset; } ICM42_V[] = {
    { 0x47, 0x11, 0x01, 0x4E, 0x4F, 0x50, 0x1D, 132.48, 0x06 },
    { 0x67, 0x02, 0x10, 0x1F, 0x20, 0x21, 0x09, 128.0, 0x69 },
};

static void icm42_reset(sim_emu_t *d)
{
    int v = variant(d, ICM42_NAME, 2);
    memset(d->regs, 0, sizeof d->regs);
    d->u[0] = (uint32_t)v;
    d->regs[0x75] = ICM42_V[v].who;
    d->regs[ICM42_V[v].gcfg] = ICM42_V[v].cfg_reset;
    d->regs[ICM42_V[v].acfg] = ICM42_V[v].cfg_reset;
}

static void icm42_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    int k = (int)d->u[0];
    if (reg == 0x75) return;
    if (reg == ICM42_V[k].rst_reg && (v & ICM42_V[k].rst_bit)) { icm42_reset(d); d->resp_ms = sim_ms() + 1; return; }
    d->regs[reg] = v;
}

static void icm42_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    int k = (int)d->u[0];
    uint8_t base = ICM42_V[k].data;
    if (!overlaps(reg, n, base, (uint8_t)(base + 13))) return;
    uint8_t pw = d->regs[ICM42_V[k].pwr];
    bool a_on = (pw & 0x03) >= 2, g_on = (pw >> 2 & 0x03) == 3;   /* LP or LN; gyro LN */
    bench_t b;
    bench(&b);
    double a_fs = 16.0 / (1 << (d->regs[ICM42_V[k].acfg] >> 5 & 3));
    double g_fs = 2000.0 / (1 << (d->regs[ICM42_V[k].gcfg] >> 5 & 3));
    for (int i = 0; i < 3; i++) {
        be16(&d->regs[base + 2 + 2 * i], a_on ? sat16(b.a[i] * 32768.0 / a_fs) : -32768);
        be16(&d->regs[base + 8 + 2 * i], g_on ? sat16(b.g[i] * 32768.0 / g_fs) : -32768);
    }
    be16(&d->regs[base], a_on || g_on ? sat16((b.tc - 25.0) * ICM42_V[k].t_lsb) : -32768);
}

static const rw_ops_t ICM42_OPS = { icm42_write, icm42_refresh, NULL, 0, NULL };
static bool icm42_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &ICM42_OPS, w, wn, r, rn); }
static const sim_model_t M_ICM42688 = { "icm42688", 0x68, icm42_reset, icm42_xfer };
static const sim_model_t M_ICM42670 = { "icm42670", 0x68, icm42_reset, icm42_xfer };

/* -------------------------------------------------------------------------- */
/* BMI160                                                                      */
/* -------------------------------------------------------------------------- */

static void bmi160_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = 0xD1;
    d->regs[0x40] = 0x28;                          /* ACC_CONF */
    d->regs[0x41] = 0x03;                          /* ACC_RANGE ±2 g */
    d->regs[0x42] = 0x28;                          /* GYR_CONF */
    d->regs[0x43] = 0x00;                          /* GYR_RANGE ±2000 */
    d->regs[0x20] = 0x00; d->regs[0x21] = 0x80;    /* temperature: invalid */
}

static void bmi160_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x7E) {
        uint8_t *pmu = &d->regs[0x03];
        switch (v) {
        case 0xB6: bmi160_reset(d); break;
        case 0x10: *pmu = (uint8_t)(*pmu & ~0x30); break;
        case 0x11: *pmu = (uint8_t)((*pmu & ~0x30) | 0x10); break;
        case 0x14: *pmu = (uint8_t)(*pmu & ~0x0C); break;
        case 0x15: *pmu = (uint8_t)((*pmu & ~0x0C) | 0x04); break;
        default: break;
        }
        return;
    }
    if (reg >= 0x40 && reg <= 0x7D) d->regs[reg] = v;
}

static void bmi160_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x0C, 0x21)) return;
    bool a_on = (d->regs[0x03] & 0x30) == 0x10, g_on = (d->regs[0x03] & 0x0C) == 0x04;
    bench_t b;
    bench(&b);
    double a_fs;
    switch (d->regs[0x41] & 0x0F) {
    case 0x05: a_fs = 4; break;
    case 0x08: a_fs = 8; break;
    case 0x0C: a_fs = 16; break;
    default: a_fs = 2; break;
    }
    double g_fs = 2000.0 / (1 << (d->regs[0x43] & 7));
    for (int i = 0; i < 3; i++) {
        if (g_on) le16(&d->regs[0x0C + 2 * i], sat16(b.g[i] * 32768.0 / g_fs));
        if (a_on) le16(&d->regs[0x12 + 2 * i], sat16(b.a[i] * 32768.0 / a_fs));
    }
    le16(&d->regs[0x20], a_on || g_on ? sat16((b.tc - 23.0) * 512.0) : -32768);
}

static const rw_ops_t BMI160_OPS = { bmi160_write, bmi160_refresh, NULL, 0, NULL };
static bool bmi160_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &BMI160_OPS, w, wn, r, rn); }
static const sim_model_t M_BMI160 = { "bmi160", 0x68, bmi160_reset, bmi160_xfer };

/* -------------------------------------------------------------------------- */
/* LSM6DS3 (0x69) and LSM6DSO (0x6C)                                           */
/* -------------------------------------------------------------------------- */

static const char *const LSM6_NAME[] = { "lsm6ds3", "lsm6dso" };

static void lsm6_reset(sim_emu_t *d)
{
    int v = variant(d, LSM6_NAME, 2);
    memset(d->regs, 0, sizeof d->regs);
    d->u[0] = (uint32_t)v;
    d->regs[0x0F] = v ? 0x6C : 0x69;
    d->regs[0x12] = 0x04;                          /* CTRL3_C: IF_INC */
}

static void lsm6_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x0F) return;
    if (reg == 0x12 && (v & 0x01)) { lsm6_reset(d); return; }   /* SW_RESET */
    d->regs[reg] = v;
}

static void lsm6_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x20, 0x2D)) return;
    uint8_t xl = d->regs[0x10], g = d->regs[0x11];
    bench_t b;
    bench(&b);
    /* ST's sensitivities: mg and mdps per count */
    static const double XL[4] = { 0.061, 0.488, 0.122, 0.244 };          /* FS_XL 00 2 g, 01 16 g, 10 4 g, 11 8 g */
    static const double G[4] = { 8.75, 17.5, 35.0, 70.0 };
    double mg = XL[xl >> 2 & 3], mdps = g & 0x02 ? 4.375 : G[g >> 2 & 3];
    for (int i = 0; i < 3; i++) {
        if (g >> 4) le16(&d->regs[0x22 + 2 * i], sat16(b.g[i] * 1000.0 / mdps));
        if (xl >> 4) le16(&d->regs[0x28 + 2 * i], sat16(b.a[i] * 1000.0 / mg));
    }
    if ((xl | g) >> 4) le16(&d->regs[0x20], sat16((b.tc - 25.0) * (d->u[0] ? 256.0 : 16.0)));
}

static bool lsm6_incr(sim_emu_t *d) { return (d->regs[0x12] & 0x04) != 0; }

static const rw_ops_t LSM6_OPS = { lsm6_write, lsm6_refresh, NULL, 0, lsm6_incr };
static bool lsm6_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &LSM6_OPS, w, wn, r, rn); }
static const sim_model_t M_LSM6DS3 = { "lsm6ds3", 0x6A, lsm6_reset, lsm6_xfer };
static const sim_model_t M_LSM6DSO = { "lsm6dso", 0x6A, lsm6_reset, lsm6_xfer };

/* -------------------------------------------------------------------------- */
/* QMI8658                                                                     */
/* -------------------------------------------------------------------------- */

static void qmi_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = 0x05;
    d->regs[0x01] = 0x7C;
    d->regs[0x02] = 0x20;                          /* CTRL1: no auto-increment */
    d->regs[0x4D] = 0x80;                          /* dQY_L: reset done */
}

static void qmi_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x60) {
        if (v == 0xB0) { qmi_reset(d); d->resp_ms = sim_ms() + 10; }   /* NACKs while it resets */
        return;
    }
    if (reg >= 0x02 && reg <= 0x0A) d->regs[reg] = v;
}

static void qmi_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x2E, 0x40)) return;
    uint8_t en = d->regs[0x08];
    bench_t b;
    bench(&b);
    double a_fs = 2 << (d->regs[0x03] >> 4 & 3), g_fs = 16 << (d->regs[0x04] >> 4 & 7);
    /* little-endian data (what SensorLib and the waveshare driver read) */
    for (int i = 0; i < 3; i++) {
        if (en & 1) le16(&d->regs[0x35 + 2 * i], sat16(b.a[i] * 32768.0 / a_fs));
        if (en & 2) le16(&d->regs[0x3B + 2 * i], sat16(b.g[i] * 32768.0 / g_fs));
    }
    if (en & 3) le16(&d->regs[0x33], sat16(b.tc * 256.0));
    d->regs[0x2E] = en & 3;                        /* STATUS0: aDA, gDA */
}

static bool qmi_incr(sim_emu_t *d) { return (d->regs[0x02] & 0x40) != 0; }

static const rw_ops_t QMI_OPS = { qmi_write, qmi_refresh, NULL, 0, qmi_incr };
static bool qmi_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &QMI_OPS, w, wn, r, rn); }
static const sim_model_t M_QMI8658 = { "qmi8658", 0x6B, qmi_reset, qmi_xfer };

/* -------------------------------------------------------------------------- */
/* LIS3DH                                                                      */
/* -------------------------------------------------------------------------- */

static void lis3dh_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x0F] = 0x33;
    d->regs[0x20] = 0x07;                          /* CTRL_REG1: power-down, XYZ */
}

static void lis3dh_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x24 && (v & 0x80)) { d->regs[0x24] = v & 0x7F; return; }   /* BOOT: reloads the trim, clears itself */
    if (reg >= 0x1E && reg <= 0x3F && reg != 0x27) d->regs[reg] = v;
}

static void lis3dh_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x27, 0x2D) || !(d->regs[0x20] >> 4)) return;
    bool lp = d->regs[0x20] & 0x08, hr = d->regs[0x23] & 0x08;
    int bits = lp ? 8 : hr ? 12 : 10;
    static const double HR_MG[4] = { 1, 2, 4, 12 };                     /* mg per count in HR mode */
    double mg = HR_MG[d->regs[0x23] >> 4 & 3] * (1 << (12 - bits));
    bench_t b;
    bench(&b);
    for (int i = 0; i < 3; i++) {
        long c = lrint(b.a[i] * 1000.0 / mg), lim = (1L << (bits - 1)) - 1;
        if (c > lim) c = lim;
        if (c < -lim - 1) c = -lim - 1;
        le16(&d->regs[0x28 + 2 * i], (int16_t)(c * (1 << (16 - bits))));     /* left-justified */
    }
    d->regs[0x27] = 0x0F;
}

static const rw_ops_t LIS3DH_OPS = { lis3dh_write, lis3dh_refresh, NULL, 0x80, NULL };
static bool lis3dh_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &LIS3DH_OPS, w, wn, r, rn); }
static const sim_model_t M_LIS3DH = { "lis3dh", 0x18, lis3dh_reset, lis3dh_xfer };

/* -------------------------------------------------------------------------- */
/* ADXL345                                                                     */
/* -------------------------------------------------------------------------- */

static void adxl_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = 0xE5;
    d->regs[0x2C] = 0x0A;
    d->regs[0x30] = 0x02;                          /* INT_SOURCE: watermark */
}

static void adxl_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg >= 0x1D && reg <= 0x39 && reg != 0x30 && !(reg >= 0x32 && reg <= 0x37)) d->regs[reg] = v;
}

static void adxl_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x32, 0x37) || !(d->regs[0x2D] & 0x08)) return;
    uint8_t fmt = d->regs[0x31];
    /* 256 counts a g at ±2 g; full resolution keeps that scale at every range */
    double cnt = fmt & 0x08 ? 256.0 : 256.0 / (1 << (fmt & 3));
    double lim = fmt & 0x08 ? 256.0 * (2 << (fmt & 3)) : 512.0;
    bench_t b;
    bench(&b);
    for (int i = 0; i < 3; i++) {
        double c = b.a[i] * cnt;
        if (c > lim - 1) c = lim - 1;
        if (c < -lim) c = -lim;
        le16(&d->regs[0x32 + 2 * i], sat16(c));
    }
}

static const rw_ops_t ADXL_OPS = { adxl_write, adxl_refresh, NULL, 0, NULL };
static bool adxl_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &ADXL_OPS, w, wn, r, rn); }
static const sim_model_t M_ADXL345 = { "adxl345", 0x53, adxl_reset, adxl_xfer };

/* -------------------------------------------------------------------------- */
/* BNO055: page 0 in regs[], page 1 in r16[]'s bytes                           */
/* -------------------------------------------------------------------------- */

/* u[1]: when fusion started (for the calibration status) */
static void bno_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    memset(d->r16, 0, sizeof d->r16);
    uint8_t *m = d->regs;
    m[0x00] = 0xA0; m[0x01] = 0xFB; m[0x02] = 0x32; m[0x03] = 0x0F;
    m[0x04] = 0x11; m[0x05] = 0x03; m[0x06] = 0x15;
    m[0x36] = 0x0F;                                /* ST_RESULT: all passed */
    m[0x3B] = 0x80;                                /* UNIT_SEL: Android orientation, m/s² */
    m[0x41] = 0x24; m[0x42] = 0x00;                /* AXIS_MAP_CONFIG / SIGN */
    d->page = 0;
}

static bool bno_fusion(const sim_emu_t *d) { return (d->regs[0x3D] & 0x0F) >= 0x08; }

static void bno_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x07) { d->page = v & 1; return; }
    if (d->page == 1) { ((uint8_t *)d->r16)[reg] = v; return; }
    if (reg == 0x3D) {
        bool was = bno_fusion(d);
        d->regs[0x3D] = v & 0x0F;
        if (!was && bno_fusion(d)) d->u[1] = (uint32_t)sim_ms();
        d->regs[0x39] = bno_fusion(d) ? 0x05 : 0x00;               /* SYS_STATUS */
        return;
    }
    if (reg == 0x3F) {
        if (v & 0x20) { bno_reset(d); d->resp_ms = sim_ms() + 650; }   /* RST_SYS: gone for 650 ms */
        else d->regs[0x3F] = v & 0x80;
        return;
    }
    if (d->regs[0x3D] & 0x0F) return;              /* outside CONFIG mode only OPR_MODE is written */
    if (reg >= 0x3B && reg <= 0x6A && reg != 0x3D) d->regs[reg] = v;
    else if (reg == 0x3E) d->regs[reg] = v & 3;
}

static void bno_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (d->page != 0 || !bno_fusion(d) || !overlaps(reg, n, 0x08, 0x35)) return;
    uint8_t u = d->regs[0x3B];
    bench_t b;
    bench(&b);
    uint8_t *m = d->regs;
    double acc = u & 0x01 ? 1000.0 : 981.0;        /* mg, or m/s² in hundredths */
    double kg = u & 0x02 ? 900.0 / 57.29577951 : 16.0;    /* rad/s at 900 a unit, or °/s at 16 */
    double k = u & 0x04 ? 900.0 / 57.29577951 : 16.0;     /* rad at 900, or ° at 16 */
    for (int i = 0; i < 3; i++) {
        le16(&m[0x08 + 2 * i], sat16(b.a[i] * acc));
        le16(&m[0x0E + 2 * i], sat16(b.m[i] * 16.0));
        le16(&m[0x14 + 2 * i], sat16(b.g[i] * kg));
    }
    double pitch = u & 0x80 ? -b.pitch : b.pitch;  /* Android turns pitch the other way */
    le16(&m[0x1A], sat16(b.heading * k));
    le16(&m[0x1C], sat16(b.roll * k));
    le16(&m[0x1E], sat16(pitch * k));
    m[0x34] = (uint8_t)(int8_t)lrint(u & 0x10 ? b.tc * 1.8 + 32.0 : b.tc);   /* °C, or °F at 1 a count */
    /* calibration: gyro at once, accel after 8 s, magnetometer after 15,
     * the system after 20 (the turning the bench does is enough) */
    double s = (double)((uint32_t)sim_ms() - d->u[1]) / 1000.0;
    int cg = 3, ca = s > 8 ? 3 : s > 4 ? 1 : 0, cm = s > 15 ? 3 : s > 6 ? 2 : 0, cs = s > 20 ? 3 : s > 10 ? 1 : 0;
    m[0x35] = (uint8_t)(cs << 6 | cg << 4 | ca << 2 | cm);
}

static uint8_t bno_read(sim_emu_t *d, uint8_t reg)
{
    if (reg == 0x07) return d->page;
    return d->page ? ((uint8_t *)d->r16)[reg] : d->regs[reg];
}

static const rw_ops_t BNO_OPS = { bno_write, bno_refresh, bno_read, 0, NULL };
static bool bno_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &BNO_OPS, w, wn, r, rn); }
static const sim_model_t M_BNO055 = { "bno055", 0x28, bno_reset, bno_xfer };

/* -------------------------------------------------------------------------- */
/* QMC5883L, QMC5883P, HMC5883L, LIS3MDL                                      */
/* -------------------------------------------------------------------------- */

/* one axis in counts, with whether it overflowed the ±lim range */
static int16_t mag_cnt(double ut, double per_gauss, double lim, bool *ovf)
{
    double c = ut / 100.0 * per_gauss;
    if (c > lim || c < -lim - 1) *ovf = true;
    return sat16(c);
}

static void qmc5883l_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x0C] = 0x01;
    d->regs[0x0D] = 0xFF;
}

static void qmc5883l_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x0A && (v & 0x80)) { qmc5883l_reset(d); return; }   /* SOFT_RST */
    if (reg >= 0x09 && reg <= 0x0B) d->regs[reg] = v;
}

static void qmc5883l_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x00, 0x08) || (d->regs[0x09] & 3) != 1) return;
    if (reg > 0x06) return;                        /* the temperature alone does not start a sample */
    bench_t b;
    bench(&b);
    double pg = d->regs[0x09] & 0x10 ? 3000.0 : 12000.0;
    bool ovf = false;
    for (int i = 0; i < 3; i++) le16(&d->regs[2 * i], mag_cnt(b.m[i], pg, 32767, &ovf));
    le16(&d->regs[0x07], sat16((b.tc - 6.0) * 100.0));   /* relative: no trimmed offset */
    d->regs[0x06] = (uint8_t)(0x01 | (ovf ? 0x02 : 0));
}

static const rw_ops_t QMC5883L_OPS = { qmc5883l_write, qmc5883l_refresh, NULL, 0, NULL };
static bool qmc5883l_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &QMC5883L_OPS, w, wn, r, rn); }
static const sim_model_t M_QMC5883L = { "qmc5883l", 0x0D, qmc5883l_reset, qmc5883l_xfer };

static void qmc5883p_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = 0x80;
}

static void qmc5883p_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x0B && (v & 0x80)) { qmc5883p_reset(d); return; }
    if (reg == 0x0A || reg == 0x0B || reg == 0x29) d->regs[reg] = v;
}

static void qmc5883p_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x01, 0x09) || (d->regs[0x0A] & 3) == 0) return;
    static const double PG[4] = { 1000.0, 2500.0, 3750.0, 15000.0 };  /* RNG 30, 12, 8, 2 G */
    bench_t b;
    bench(&b);
    bool ovf = false;
    for (int i = 0; i < 3; i++) le16(&d->regs[1 + 2 * i], mag_cnt(b.m[i], PG[d->regs[0x0B] >> 2 & 3], 32767, &ovf));
    d->regs[0x09] = (uint8_t)(0x01 | (ovf ? 0x02 : 0));
}

static const rw_ops_t QMC5883P_OPS = { qmc5883p_write, qmc5883p_refresh, NULL, 0, NULL };
static bool qmc5883p_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &QMC5883P_OPS, w, wn, r, rn); }
static const sim_model_t M_QMC5883P = { "qmc5883p", 0x2C, qmc5883p_reset, qmc5883p_xfer };

static void hmc_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x00] = 0x10;
    d->regs[0x01] = 0x20;
    d->regs[0x02] = 0x01;                          /* single measurement, then idle */
    d->regs[0x0A] = 'H'; d->regs[0x0B] = '4'; d->regs[0x0C] = '3';
}

static void hmc_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg <= 0x02) d->regs[reg] = v;
}

static void hmc_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x03, 0x08) || (d->regs[0x02] & 3) != 0) return;
    static const double PG[8] = { 1370, 1090, 820, 660, 440, 390, 330, 230 };
    double pg = PG[d->regs[0x01] >> 5];
    bench_t b;
    bench(&b);
    int16_t c[3];
    for (int i = 0; i < 3; i++) {
        bool ovf = false;
        c[i] = mag_cnt(b.m[i], pg, 2047, &ovf);
        if (ovf) c[i] = -4096;
    }
    be16(&d->regs[0x03], c[0]);                    /* X, Z, Y */
    be16(&d->regs[0x05], c[2]);
    be16(&d->regs[0x07], c[1]);
    d->regs[0x09] = 0x01;
}

static const rw_ops_t HMC_OPS = { hmc_write, hmc_refresh, NULL, 0, NULL };
static bool hmc_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &HMC_OPS, w, wn, r, rn); }
static const sim_model_t M_HMC5883L = { "hmc5883l", 0x1E, hmc_reset, hmc_xfer };

static void lis3mdl_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x0F] = 0x3D;
    d->regs[0x20] = 0x10;
    d->regs[0x22] = 0x03;                          /* power-down */
}

static void lis3mdl_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    if (reg == 0x21 && (v & 0x04)) { lis3mdl_reset(d); return; }    /* SOFT_RST */
    if (reg >= 0x20 && reg <= 0x24) d->regs[reg] = v & (reg == 0x21 ? 0x60 : 0xFF);
}

static void lis3mdl_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    if (!overlaps(reg, n, 0x27, 0x2F) || (d->regs[0x22] & 3) >= 2) return;
    static const double PG[4] = { 6842, 3421, 2281, 1711 };
    bench_t b;
    bench(&b);
    bool ovf = false;
    for (int i = 0; i < 3; i++) le16(&d->regs[0x28 + 2 * i], mag_cnt(b.m[i], PG[d->regs[0x21] >> 5 & 3], 32767, &ovf));
    le16(&d->regs[0x2E], sat16((b.tc - 25.0) * 8.0));
    d->regs[0x27] = 0x0F;
    if ((d->regs[0x22] & 3) == 1) d->regs[0x22] |= 3;  /* single: then power-down */
}

static const rw_ops_t LIS3MDL_OPS = { lis3mdl_write, lis3mdl_refresh, NULL, 0x80, NULL };
static bool lis3mdl_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &LIS3MDL_OPS, w, wn, r, rn); }
static const sim_model_t M_LIS3MDL = { "lis3mdl", 0x1C, lis3mdl_reset, lis3mdl_xfer };

/* -------------------------------------------------------------------------- */
/* MMC5983MA and MMC5603NJ: measurements on demand, write-only controls       */
/* -------------------------------------------------------------------------- */

/* u[1]: when the field measurement ends (0: none), u[2]: the temperature's */
static void mmc5983_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x08] = 0x10;                          /* OTP_Read_Done */
    d->regs[0x2F] = 0x30;
    d->u[1] = d->u[2] = 0;
    d->u[3] = 0;                                   /* bandwidth */
}

static void mmc5983_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    static const uint32_t MS[4] = { 8, 4, 2, 1 };
    uint32_t now = (uint32_t)sim_ms();
    if (reg == 0x09) {
        if (v & 0x01) { d->u[1] = now + MS[d->u[3] & 3]; d->regs[0x08] &= (uint8_t)~0x01; }
        if (v & 0x02) { d->u[2] = now + 2; d->regs[0x08] &= (uint8_t)~0x02; }
    } else if (reg == 0x0A) {
        if (v & 0x80) { mmc5983_reset(d); d->resp_ms = sim_ms() + 10; }
        else d->u[3] = v & 3;
    }
}

static void mmc5983_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    (void)reg; (void)n;
    uint32_t now = (uint32_t)sim_ms();
    if (d->u[1] && now >= d->u[1]) {
        bench_t b;
        bench(&b);
        uint32_t c[3];
        for (int i = 0; i < 3; i++) {
            double v = 131072.0 + b.m[i] / 100.0 * 16384.0;
            c[i] = v < 0 ? 0 : v > 262143 ? 262143 : (uint32_t)lrint(v);
            d->regs[2 * i] = (uint8_t)(c[i] >> 10);
            d->regs[2 * i + 1] = (uint8_t)(c[i] >> 2);
        }
        d->regs[0x06] = (uint8_t)((c[0] & 3) << 6 | (c[1] & 3) << 4 | (c[2] & 3) << 2);
        d->regs[0x08] |= 0x01;
        d->u[1] = 0;
    }
    if (d->u[2] && now >= d->u[2]) {
        bench_t b;
        bench(&b);
        double t = (b.tc + 75.0) * 255.0 / 200.0;
        d->regs[0x07] = (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : lrint(t));
        d->regs[0x08] |= 0x02;
        d->u[2] = 0;
    }
}

static uint8_t mmc5983_read(sim_emu_t *d, uint8_t reg)
{
    if (reg >= 0x09 && reg <= 0x0C) return 0;      /* write-only */
    return d->regs[reg];
}

static const rw_ops_t MMC5983_OPS = { mmc5983_write, mmc5983_refresh, mmc5983_read, 0, NULL };
static bool mmc5983_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &MMC5983_OPS, w, wn, r, rn); }
static const sim_model_t M_MMC5983 = { "mmc5983", 0x30, mmc5983_reset, mmc5983_xfer };

static void mmc5603_reset(sim_emu_t *d)
{
    memset(d->regs, 0, sizeof d->regs);
    d->regs[0x18] = 0x10;                          /* OTP_read_done */
    d->regs[0x39] = 0x10;
    d->u[1] = d->u[2] = d->u[3] = 0;
}

static void mmc5603_write(sim_emu_t *d, uint8_t reg, uint8_t v)
{
    static const uint32_t MS[4] = { 7, 4, 2, 2 };
    uint32_t now = (uint32_t)sim_ms();
    if (reg == 0x1B) {
        if (v & 0x01) { d->u[1] = now + MS[d->u[3] & 3]; d->regs[0x18] &= (uint8_t)~0x40; }
        if (v & 0x02) { d->u[2] = now + 2; d->regs[0x18] &= (uint8_t)~0x80; }
    } else if (reg == 0x1C) {
        if (v & 0x80) { mmc5603_reset(d); d->resp_ms = sim_ms() + 20; }
        else d->u[3] = v & 3;
    } else if (reg == 0x1A) d->regs[0x1A] = v;
}

static void mmc5603_refresh(sim_emu_t *d, uint8_t reg, size_t n)
{
    (void)reg; (void)n;
    uint32_t now = (uint32_t)sim_ms();
    if (d->u[1] && now >= d->u[1]) {
        bench_t b;
        bench(&b);
        for (int i = 0; i < 3; i++) {
            double v = 524288.0 + b.m[i] / 0.00625;
            uint32_t c = v < 0 ? 0 : v > 1048575 ? 1048575 : (uint32_t)lrint(v);
            d->regs[2 * i] = (uint8_t)(c >> 12);
            d->regs[2 * i + 1] = (uint8_t)(c >> 4);
            d->regs[6 + i] = (uint8_t)((c & 0x0F) << 4);
        }
        d->regs[0x18] |= 0x40;
        d->u[1] = 0;
    }
    if (d->u[2] && now >= d->u[2]) {
        bench_t b;
        bench(&b);
        double t = (b.tc + 75.0) / 0.8;
        d->regs[0x09] = (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : lrint(t));
        d->regs[0x18] |= 0x80;
        d->u[2] = 0;
    }
}

static uint8_t mmc5603_read(sim_emu_t *d, uint8_t reg)
{
    if (reg >= 0x1B && reg <= 0x1D) return 0;
    return d->regs[reg];
}

static const rw_ops_t MMC5603_OPS = { mmc5603_write, mmc5603_refresh, mmc5603_read, 0, NULL };
static bool mmc5603_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) { return rw8(d, &MMC5603_OPS, w, wn, r, rn); }
static const sim_model_t M_MMC5603 = { "mmc5603", 0x30, mmc5603_reset, mmc5603_xfer };

const sim_model_t *const SIM_M_MOTION[] = {
    &M_MPU6050, &M_MPU6500, &M_MPU9250, &M_MPU9255, &M_MPU6886, &M_ICM20602, &M_ICM20608, &M_ICM20689,
    &M_AK8963, &M_ICM20948, &M_AK09916, &M_ICM42688, &M_ICM42670, &M_BMI160,
    &M_LSM6DS3, &M_LSM6DSO, &M_QMI8658, &M_LIS3DH, &M_ADXL345, &M_BNO055,
    &M_QMC5883L, &M_QMC5883P, &M_HMC5883L, &M_LIS3MDL, &M_MMC5983, &M_MMC5603,
    NULL,
};
