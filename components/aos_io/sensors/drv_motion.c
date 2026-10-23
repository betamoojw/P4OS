/*
 * P4OS - Sensor drivers: accelerometers, gyroscopes, magnetometers and
 * orientation on I2C (sens_int.h, docs/SENSORS.md).
 *
 * Six-axis parts give ax ay az in g, gx gy gz in °/s and the die's
 * temperature; magnetometers give mx my mz in µT and a heading, the angle of
 * the horizontal field from the X axis (atan2 of Y over X, so it is a
 * compass only with the board flat); the BNO055 gives its own fused
 * heading, pitch and roll. The ranges are ±8 g and ±2000 °/s unless the
 * module line says otherwise: "g=2|4|8|16" and "dps=250|500|1000|2000". A
 * bench sees a board turned by hand, which passes 500 °/s easily, and a
 * reading a second has no use for the finer step of a smaller range.
 *
 * Where the magnetometer sits behind the IMU (AK8963 in an MPU-9250,
 * AK09916 in an ICM-20948), the IMU's I2C bypass puts it on the same bus at
 * 0x0C, and its axes are turned into the IMU's own so that one frame holds
 * all nine values.
 *
 * A chip that lost power and came back keeps answering, asleep or with its
 * configuration gone. Every read() checks one configuration register (or
 * looks at it when the data is all zeros) and, if it is back at its reset
 * value, clears s->inited so that the service runs init() again.
 *
 * Sources, per chip: the TDK/InvenSense register maps (MPU-6000/6050 rev
 * 4.2, MPU-6500 and MPU-9250 rev 1.6, ICM-20948 rev 1.3, ICM-42688-P,
 * ICM-42670-P), the AK8963 and AK09916 datasheets, ST's (LSM6DS3/DSL/DSO,
 * LIS3DH, LIS3MDL), ADI's ADXL345, Bosch's BMI160 and BNO055, QST's
 * QMI8658, QMC5883L and QMC5883P, Honeywell's HMC5883L and Memsic's
 * MMC5983MA and MMC5603NJ. Checked against ESPHome (mpu6050, mpu6886),
 * esp-bsp's icm42670, lewisxhe's SensorLib (QMI8658), PX4's qmc5883p,
 * Adafruit_MMC56x3 and SparkFun's MMC5983MA library.
 */
#include "sens_int.h"

#define RAD2DEG 57.29577951f

/* ---- shared pieces ---- */

static const int G_FS[4] = { 2, 4, 8, 16 };
static const int DPS_FS[4] = { 250, 500, 1000, 2000 };

/* The module line's range, as an index into one of the tables above */
static int range_ix(const sens_slot_t *s, const char *key, const int *tab, int def_ix)
{
    int32_t v = sens_arg_int(s, key, tab[def_ix]);
    for (int i = 0; i < 4; i++) if (tab[i] == v) return i;
    return def_ix;
}

/* What every IMU keeps between calls */
typedef struct {
    uint8_t id;                     /* the WHO_AM_I it gave */
    uint8_t a_ix, g_ix;             /* the ranges, G_FS / DPS_FS */
    float   a_lsb, g_lsb;           /* g and °/s per count */
    bool    mag;                    /* a magnetometer behind it */
    float   asa[3];                 /* AK8963: the fuse ROM's sensitivity adjustment */
    float   m[3];                   /* the last field, in the IMU's frame */
    bool    m_have;
    uint8_t m_stale;                /* reads in a row without new magnetometer data */
} imu_t;
SPRIV_CHECK(imu_t);

static void imu_ranges(sens_slot_t *s, imu_t *p)
{
    p->a_ix = (uint8_t)range_ix(s, "g", G_FS, 2);
    p->g_ix = (uint8_t)range_ix(s, "dps", DPS_FS, 3);
    p->a_lsb = (float)G_FS[p->a_ix] / 32768.0f;
    p->g_lsb = (float)DPS_FS[p->g_ix] / 32768.0f;
}

static void put_accel(sens_slot_t *s, int k, float x, float y, float z)
{
    sens_setv(s, k, "ax", AOS_SQ_ACCEL, x);
    sens_setv(s, k + 1, "ay", AOS_SQ_ACCEL, y);
    sens_setv(s, k + 2, "az", AOS_SQ_ACCEL, z);
}

static void put_gyro(sens_slot_t *s, int k, float x, float y, float z)
{
    sens_setv(s, k, "gx", AOS_SQ_GYRO, x);
    sens_setv(s, k + 1, "gy", AOS_SQ_GYRO, y);
    sens_setv(s, k + 2, "gz", AOS_SQ_GYRO, z);
}

/* mx my mz and the heading they make with the board flat */
static void put_mag(sens_slot_t *s, int k, const float m[3])
{
    sens_setv(s, k, "mx", AOS_SQ_MAG, m[0]);
    sens_setv(s, k + 1, "my", AOS_SQ_MAG, m[1]);
    sens_setv(s, k + 2, "mz", AOS_SQ_MAG, m[2]);
    float h = atan2f(m[1], m[0]) * RAD2DEG;
    if (h < 0) h += 360.0f;
    sens_setv(s, k + 3, "heading", AOS_SQ_ANGLE, h);
}

static bool all_zero(const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) if (d[i]) return false;
    return true;
}

/* The chip came back from a power loss: start it again next round */
static int lost_config(sens_slot_t *s)
{
    s->inited = false;
    sens_scpy(s->pub.err, sizeof s->pub.err, "reset");
    return R_NOTREADY;
}

static int16_t be16s(const uint8_t *p) { return (int16_t)sens_be16(p); }
static int16_t le16s(const uint8_t *p) { return (int16_t)sens_le16(p); }

/* ---- the AK8963 / AK09916 behind an IMU's bypass, at 0x0C ---- */

#define AK_ADDR 0x0C

/* AK8963 (MPU-9250/9255): fuse ROM first, then 16-bit continuous mode 2
 * (100 Hz). The adjustment is H * ((ASA - 128) / 256 + 1), datasheet 8.3.11. */
static bool ak8963_start(aos_io_i2c_t *b, imu_t *p)
{
    uint8_t wia = 0;
    if (!si2c_rreg8(b, AK_ADDR, 0x00, &wia) || wia != 0x48) return false;
    uint8_t asa[3];
    si2c_wreg8(b, AK_ADDR, 0x0A, 0x00);            /* power down before any mode change */
    aos_hal_sleep_ms(1);
    si2c_wreg8(b, AK_ADDR, 0x0A, 0x0F);            /* fuse ROM access */
    aos_hal_sleep_ms(1);
    if (!si2c_rreg(b, AK_ADDR, 0x10, asa, 3)) return false;
    si2c_wreg8(b, AK_ADDR, 0x0A, 0x00);
    aos_hal_sleep_ms(1);
    if (!si2c_wreg8(b, AK_ADDR, 0x0A, 0x16)) return false;
    for (int i = 0; i < 3; i++) p->asa[i] = ((float)asa[i] - 128.0f) / 256.0f + 1.0f;
    return true;
}

/* AK09916 (ICM-20948): WIA2 0x09, soft reset, continuous mode 4 (100 Hz) */
static bool ak09916_start(aos_io_i2c_t *b)
{
    uint8_t wia[2] = { 0 };
    if (!si2c_rreg(b, AK_ADDR, 0x00, wia, 2) || wia[0] != 0x48 || wia[1] != 0x09) return false;
    si2c_wreg8(b, AK_ADDR, 0x32, 0x01);
    aos_hal_sleep_ms(1);
    return si2c_wreg8(b, AK_ADDR, 0x31, 0x08);
}

/* One sample of either into p->m, in the IMU's frame. Both are 0.15 µT a
 * count. The AK8963's X and Y are the MPU-9250's Y and X and its Z the
 * opposite (MPU-9250 datasheet 9.1); the AK09916's Y and Z are the
 * ICM-20948's opposite (ICM-20948 datasheet 12). With no new data the last
 * sample stays; after a few, the mode is set again (the magnetometer may
 * have been reset alone). */
static void ak_read(aos_io_i2c_t *b, imu_t *p, bool ak09916)
{
    uint8_t d[9];
    uint8_t reg = ak09916 ? 0x10 : 0x02;
    int n = ak09916 ? 9 : 8;                       /* ST1, six bytes, (TMPS,) ST2: reading ST2 ends the sample */
    if (!si2c_rreg(b, AK_ADDR, reg, d, (size_t)n)) return;
    uint8_t st2 = d[n - 1];
    if (!(d[0] & 0x01)) {
        if (++p->m_stale >= 3) {
            p->m_stale = 0;
            if (ak09916) si2c_wreg8(b, AK_ADDR, 0x31, 0x08);
            else si2c_wreg8(b, AK_ADDR, 0x0A, 0x16);
        }
        return;
    }
    p->m_stale = 0;
    if (st2 & 0x08) return;                        /* HOFL: the field overflowed the sensor */
    float h[3] = { (float)le16s(d + 1), (float)le16s(d + 3), (float)le16s(d + 5) };
    if (ak09916) {
        p->m[0] = h[0] * 0.15f;
        p->m[1] = -h[1] * 0.15f;
        p->m[2] = -h[2] * 0.15f;
    } else {
        float lsb = st2 & 0x10 ? 0.15f : 0.6f;     /* BITM: 16 or 14 bits */
        for (int i = 0; i < 3; i++) h[i] *= lsb * p->asa[i];
        p->m[0] = h[1];
        p->m[1] = h[0];
        p->m[2] = -h[2];
    }
    p->m_have = true;
}

/* ---- MPU-6050 / 6500 / 9250 / 9255 / 6886, ICM-20602 / 20608 / 20689 ----
 * One register map from 0x19 to 0x75: data at 0x3B, big-endian, accel,
 * temperature, gyro. Only the temperature's formula and the magnetometer
 * differ. */

typedef struct { uint8_t id; const char *chip; float t_div, t_off; } mpu_part_t;

static const mpu_part_t MPU_PART[] = {
    { 0x68, "MPU-6050", 340.0f, 36.53f },
    { 0x70, "MPU-6500", 333.87f, 21.0f },
    { 0x71, "MPU-9250", 333.87f, 21.0f },
    { 0x73, "MPU-9255", 333.87f, 21.0f },
    { 0x72, "MPU-6xxx", 333.87f, 21.0f },          /* seen on clones; the 6500's map */
    { 0x19, "MPU-6886", 326.8f, 25.0f },
    { 0x12, "ICM-20602", 326.8f, 25.0f },
    { 0xAF, "ICM-20608", 326.8f, 25.0f },
    { 0x98, "ICM-20689", 326.8f, 25.0f },
};

static const mpu_part_t *mpu_part(uint8_t id)
{
    for (size_t i = 0; i < sizeof MPU_PART / sizeof MPU_PART[0]; i++) if (MPU_PART[i].id == id) return &MPU_PART[i];
    return NULL;
}

/* At 0x68 a DS3231 answers too; its registers stop at 0x12, and none of
 * the ids above is what it gives for 0x75. */
static int mpu_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x75, &id) && mpu_part(id) ? ID_YES : ID_NO;
}

static int mpu_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x75, &id)) return R_NACK;
    const mpu_part_t *pt = mpu_part(id);
    if (!pt) { sens_err(s, "0x%02X", id); return R_ID; }
    imu_t *p = SPRIV(s, imu_t);
    memset(p, 0, sizeof *p);
    p->id = id;
    imu_ranges(s, p);
    sens_scpy(s->pub.chip, sizeof s->pub.chip, pt->chip);

    if (!si2c_wreg8(b, a, 0x6B, 0x80)) return R_NACK;           /* DEVICE_RESET */
    aos_hal_sleep_ms(100);
    if (!si2c_wreg8(b, a, 0x6B, 0x01)) return R_NACK;           /* awake, clock from the gyro's PLL */
    aos_hal_sleep_ms(5);
    si2c_wreg8(b, a, 0x19, 9);                                  /* 1 kHz / (1 + 9): 100 Hz */
    si2c_wreg8(b, a, 0x1A, 0x03);                               /* DLPF ~42 Hz */
    si2c_wreg8(b, a, 0x1B, (uint8_t)(p->g_ix << 3));
    si2c_wreg8(b, a, 0x1C, (uint8_t)(p->a_ix << 3));
    if (id != 0x68) si2c_wreg8(b, a, 0x1D, 0x03);               /* ACCEL_CONFIG2: DLPF ~45 Hz (not on a 6050) */

    /* An MPU-9250/9255, or a "9250" board whose chip says 0x70: the
     * bypass, then whether an AK8963 answers behind it. */
    if (id == 0x71 || id == 0x73 || id == 0x70 || id == 0x72) {
        si2c_wreg8(b, a, 0x6A, 0x00);                           /* USER_CTRL: I2C master off */
        si2c_wreg8(b, a, 0x37, 0x02);                           /* INT_PIN_CFG: BYPASS_EN */
        aos_hal_sleep_ms(1);
        p->mag = ak8963_start(b, p);
        if (p->mag && (id == 0x70 || id == 0x72)) sens_scpy(s->pub.chip, sizeof s->pub.chip, "MPU-9250");
    }
    /* The data registers still hold what was sampled at the factory ranges
     * until a sample passes the new filter: read at once, a board at rest
     * said 4 g (seen on the board after a reset, 2026-10-21) */
    aos_hal_sleep_ms(30);
    return R_OK;
}

static int mpu_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    imu_t *p = SPRIV(s, imu_t);
    const mpu_part_t *pt = mpu_part(p->id);
    uint8_t d[14];
    if (!pt) return R_ID;
    if (!si2c_rreg(b, a, 0x3B, d, 14)) return R_NACK;
    if (all_zero(d, 14)) {
        uint8_t pw = 0;
        if (si2c_rreg8(b, a, 0x6B, &pw) && (pw & 0x40)) return lost_config(s);    /* asleep: its reset state */
        return R_NOTREADY;
    }
    /* A chip that reset by itself (a loose wire on the bench, 2026-10-21)
     * does not always go to sleep: an MPU-9250 comes back awake, at ±2 g and
     * ±250 °/s with the bypass shut, and was read as ±8 g with its compass
     * frozen behind it. The ranges and the bypass are checked every read. */
    uint8_t gc = 0, ac = 0, ip = 0x02;
    if (!si2c_rreg8(b, a, 0x1B, &gc) || !si2c_rreg8(b, a, 0x1C, &ac)) return R_NACK;
    if (p->mag && !si2c_rreg8(b, a, 0x37, &ip)) return R_NACK;
    if ((gc & 0x18) != (uint8_t)(p->g_ix << 3) || (ac & 0x18) != (uint8_t)(p->a_ix << 3) || !(ip & 0x02))
        return lost_config(s);
    put_accel(s, 0, be16s(d) * p->a_lsb, be16s(d + 2) * p->a_lsb, be16s(d + 4) * p->a_lsb);
    put_gyro(s, 3, be16s(d + 8) * p->g_lsb, be16s(d + 10) * p->g_lsb, be16s(d + 12) * p->g_lsb);
    sens_setv(s, 6, "temp", AOS_SQ_TEMP, be16s(d + 6) / pt->t_div + pt->t_off);
    if (p->mag) {
        ak_read(b, p, false);
        if (p->m_have) put_mag(s, 7, p->m);
    }
    return R_OK;
}

const sens_drv_t DRV_MPU6050 = {
    .prio = SP_REG,
    .pub = { .id = "mpu6050", .name = "MPU-6050", .addrs = { 0x68, 0x69 }, .naddr = 2 },
    .identify = mpu_identify, .init = mpu_init, .read = mpu_read,
};

/* ---- ICM-20948: four register banks (0x7F), AK09916 behind it ---- */

static bool mpu_family_id(uint8_t id) { return mpu_part(id) || id == 0x47 || id == 0x42 || id == 0x67 || id == 0x60; }

static int icm20948_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id, w;
    if (!si2c_rreg8(b, a, 0x00, &id) || id != 0xEA) return ID_NO;
    /* an MPU-6500's self-test byte at 0x00 could be anything: its 0x75 is not */
    if (si2c_rreg8(b, a, 0x75, &w) && mpu_family_id(w)) return ID_NO;
    return ID_YES;
}

static bool bank(aos_io_i2c_t *b, uint8_t a, int n) { return si2c_wreg8(b, a, 0x7F, (uint8_t)(n << 4)); }

static int icm20948_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!bank(b, a, 0) || !si2c_rreg8(b, a, 0x00, &id)) return R_NACK;
    if (id != 0xEA) { sens_err(s, "0x%02X", id); return R_ID; }
    imu_t *p = SPRIV(s, imu_t);
    memset(p, 0, sizeof *p);
    p->id = id;
    imu_ranges(s, p);
    si2c_wreg8(b, a, 0x06, 0x80);                  /* PWR_MGMT_1: DEVICE_RESET */
    aos_hal_sleep_ms(50);
    bank(b, a, 0);
    if (!si2c_wreg8(b, a, 0x06, 0x01)) return R_NACK;           /* awake, best clock */
    aos_hal_sleep_ms(5);
    si2c_wreg8(b, a, 0x07, 0x00);                  /* PWR_MGMT_2: accel and gyro on */
    bank(b, a, 2);
    si2c_wreg8(b, a, 0x00, 10);                    /* GYRO_SMPLRT_DIV: 1.1 kHz / 11 = 100 Hz */
    si2c_wreg8(b, a, 0x01, (uint8_t)(3 << 3 | p->g_ix << 1 | 1));   /* GYRO_CONFIG_1: DLPF 3, FS */
    si2c_wreg8(b, a, 0x10, 0);
    si2c_wreg8(b, a, 0x11, 10);                    /* ACCEL_SMPLRT_DIV */
    si2c_wreg8(b, a, 0x14, (uint8_t)(3 << 3 | p->a_ix << 1 | 1));   /* ACCEL_CONFIG: DLPF 3, FS */
    bank(b, a, 0);
    si2c_wreg8(b, a, 0x03, 0x00);                  /* USER_CTRL: I2C master off */
    si2c_wreg8(b, a, 0x0F, 0x02);                  /* INT_PIN_CFG: BYPASS_EN */
    aos_hal_sleep_ms(1);
    p->mag = ak09916_start(b);
    return R_OK;
}

static int icm20948_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    imu_t *p = SPRIV(s, imu_t);
    uint8_t d[14], pw = 0;
    if (!si2c_rreg8(b, a, 0x06, &pw)) return R_NACK;
    if (pw & 0x40) return lost_config(s);          /* SLEEP: back at its reset value 0x41 */
    if (!si2c_rreg(b, a, 0x2D, d, 14)) return R_NACK;
    put_accel(s, 0, be16s(d) * p->a_lsb, be16s(d + 2) * p->a_lsb, be16s(d + 4) * p->a_lsb);
    put_gyro(s, 3, be16s(d + 6) * p->g_lsb, be16s(d + 8) * p->g_lsb, be16s(d + 10) * p->g_lsb);
    sens_setv(s, 6, "temp", AOS_SQ_TEMP, be16s(d + 12) / 333.87f + 21.0f);
    if (p->mag) {
        ak_read(b, p, true);
        if (p->m_have) put_mag(s, 7, p->m);
    }
    return R_OK;
}

const sens_drv_t DRV_ICM20948 = {
    .prio = SP_REG,
    .pub = { .id = "icm20948", .name = "ICM-20948", .addrs = { 0x69, 0x68 }, .naddr = 2 },
    .identify = icm20948_identify, .init = icm20948_init, .read = icm20948_read,
};

/* ---- ICM-42688-P / ICM-42605 and ICM-42670-P / 42607-P ----
 * Two maps under one WHO_AM_I register: the 4268x's data at 0x1D and its
 * power at 0x4E, the 4267x's at 0x09 and 0x1F. Big-endian; a sensor that
 * is off reads -32768. FS codes count down: 0 is ±2000 °/s and ±16 g. */

static bool icm426_small(uint8_t id) { return id == 0x67 || id == 0x60; }

static int icm42_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x75, &id) && (id == 0x47 || id == 0x42 || icm426_small(id)) ? ID_YES : ID_NO;
}

static int icm42_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x75, &id)) return R_NACK;
    if (id != 0x47 && id != 0x42 && !icm426_small(id)) { sens_err(s, "0x%02X", id); return R_ID; }
    imu_t *p = SPRIV(s, imu_t);
    memset(p, 0, sizeof *p);
    p->id = id;
    imu_ranges(s, p);
    uint8_t gc = (uint8_t)((3 - p->g_ix) << 5), ac = (uint8_t)((3 - p->a_ix) << 5);
    if (icm426_small(id)) {
        sens_scpy(s->pub.chip, sizeof s->pub.chip, id == 0x67 ? "ICM-42670" : "ICM-42607");
        si2c_wreg8(b, a, 0x02, 0x10);              /* SIGNAL_PATH_RESET: SOFT_RESET_DEVICE_CONFIG */
        aos_hal_sleep_ms(2);
        si2c_wreg8(b, a, 0x20, gc | 0x09);         /* GYRO_CONFIG0: FS, 100 Hz */
        si2c_wreg8(b, a, 0x21, ac | 0x09);         /* ACCEL_CONFIG0 */
        if (!si2c_wreg8(b, a, 0x1F, 0x0F)) return R_NACK;       /* PWR_MGMT0: both low-noise */
    } else {
        sens_scpy(s->pub.chip, sizeof s->pub.chip, id == 0x47 ? "ICM-42688" : "ICM-42605");
        si2c_wreg8(b, a, 0x76, 0x00);              /* REG_BANK_SEL 0 */
        si2c_wreg8(b, a, 0x11, 0x01);              /* DEVICE_CONFIG: SOFT_RESET_CONFIG */
        aos_hal_sleep_ms(2);
        si2c_wreg8(b, a, 0x4F, gc | 0x08);         /* GYRO_CONFIG0: FS, 100 Hz */
        si2c_wreg8(b, a, 0x50, ac | 0x08);         /* ACCEL_CONFIG0 */
        if (!si2c_wreg8(b, a, 0x4E, 0x0F)) return R_NACK;       /* PWR_MGMT0: both low-noise */
    }
    aos_hal_sleep_ms(1);                           /* 200 µs without writes after power mode changes */
    return R_OK;
}

static int icm42_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    imu_t *p = SPRIV(s, imu_t);
    bool small = icm426_small(p->id);
    uint8_t d[14], pw = 0;
    if (!si2c_rreg8(b, a, small ? 0x1F : 0x4E, &pw)) return R_NACK;
    if ((pw & 0x0F) == 0) return lost_config(s);   /* both off: the reset value */
    if (!si2c_rreg(b, a, small ? 0x09 : 0x1D, d, 14)) return R_NACK;
    if (be16s(d + 2) == -32768 || be16s(d + 8) == -32768) return R_NOTREADY;     /* not started yet */
    put_accel(s, 0, be16s(d + 2) * p->a_lsb, be16s(d + 4) * p->a_lsb, be16s(d + 6) * p->a_lsb);
    put_gyro(s, 3, be16s(d + 8) * p->g_lsb, be16s(d + 10) * p->g_lsb, be16s(d + 12) * p->g_lsb);
    sens_setv(s, 6, "temp", AOS_SQ_TEMP, be16s(d) / (small ? 128.0f : 132.48f) + 25.0f);
    return R_OK;
}

const sens_drv_t DRV_ICM42688 = {
    .prio = SP_REG,
    .pub = { .id = "icm42688", .name = "ICM-42688", .addrs = { 0x68, 0x69 }, .naddr = 2 },
    .identify = icm42_identify, .init = icm42_init, .read = icm42_read,
};

/* ---- BMI160 / BMX160 (the BMX160's BMM150 is not read: it sits behind
 * the BMI's own magnetometer interface, with its trim registers) ---- */

static int bmi160_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id, w;
    if (!si2c_rreg8(b, a, 0x00, &id) || (id != 0xD1 && id != 0xD8)) return ID_NO;
    if (si2c_rreg8(b, a, 0x75, &w) && mpu_part(w)) return ID_NO;   /* an MPU-6500's self-test byte */
    return ID_YES;
}

static int bmi160_init(sens_io_t *io, sens_slot_t *s)
{
    static const uint8_t ACC_RANGE[4] = { 0x03, 0x05, 0x08, 0x0C };
    static const uint8_t GYR_RANGE[4] = { 0x03, 0x02, 0x01, 0x00 };
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x00, &id)) return R_NACK;
    if (id != 0xD1 && id != 0xD8) { sens_err(s, "0x%02X", id); return R_ID; }
    imu_t *p = SPRIV(s, imu_t);
    memset(p, 0, sizeof *p);
    p->id = id;
    imu_ranges(s, p);
    sens_scpy(s->pub.chip, sizeof s->pub.chip, id == 0xD8 ? "BMX160" : "BMI160");
    si2c_wreg8(b, a, 0x7E, 0xB6);                  /* CMD: softreset */
    aos_hal_sleep_ms(2);
    if (!si2c_wreg8(b, a, 0x7E, 0x11)) return R_NACK;           /* accel normal mode: 3.8 ms */
    aos_hal_sleep_ms(5);
    si2c_wreg8(b, a, 0x7E, 0x15);                  /* gyro normal mode: 80 ms */
    aos_hal_sleep_ms(81);
    si2c_wreg8(b, a, 0x40, 0x28);                  /* ACC_CONF: normal filter, 100 Hz */
    si2c_wreg8(b, a, 0x41, ACC_RANGE[p->a_ix]);
    si2c_wreg8(b, a, 0x42, 0x28);                  /* GYR_CONF: normal filter, 100 Hz */
    si2c_wreg8(b, a, 0x43, GYR_RANGE[p->g_ix]);
    return R_OK;
}

static int bmi160_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    imu_t *p = SPRIV(s, imu_t);
    uint8_t d[12], t[2], pmu = 0;
    if (!si2c_rreg8(b, a, 0x03, &pmu)) return R_NACK;
    if ((pmu & 0x3C) == 0) return lost_config(s);  /* accel and gyro suspended: power-on state */
    if (!si2c_rreg(b, a, 0x0C, d, 12)) return R_NACK;
    if (!si2c_rreg(b, a, 0x20, t, 2)) return R_NACK;
    put_accel(s, 0, le16s(d + 6) * p->a_lsb, le16s(d + 8) * p->a_lsb, le16s(d + 10) * p->a_lsb);
    put_gyro(s, 3, le16s(d) * p->g_lsb, le16s(d + 2) * p->g_lsb, le16s(d + 4) * p->g_lsb);
    if (le16s(t) != -32768) sens_setv(s, 6, "temp", AOS_SQ_TEMP, 23.0f + le16s(t) / 512.0f);   /* 0x8000: invalid */
    return R_OK;
}

const sens_drv_t DRV_BMI160 = {
    .prio = SP_REG,
    .pub = { .id = "bmi160", .name = "BMI160", .addrs = { 0x68, 0x69 }, .naddr = 2 },
    .identify = bmi160_identify, .init = bmi160_init, .read = bmi160_read,
};

/* ---- LSM6DS3 / DSL / DSO / DSOX / DSR, ISM330: one map from 0x10 ----
 * WHO_AM_I 0x69 is the first LSM6DS3 (16 counts a degree), the others
 * 256. Little-endian; data from 0x20: temperature, gyro, accel. */

static int lsm6_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x0F, &id) && id >= 0x69 && id <= 0x6C ? ID_YES : ID_NO;
}

static int lsm6_init(sens_io_t *io, sens_slot_t *s)
{
    static const uint8_t FS_XL[4] = { 0, 2, 3, 1 };        /* 2, 4, 8, 16 g in CTRL1_XL's code */
    static const char *const CHIP[4] = { "LSM6DS3", "LSM6DSL", "LSM6DSR", "LSM6DSO" };
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x0F, &id)) return R_NACK;
    if (id < 0x69 || id > 0x6C) { sens_err(s, "0x%02X", id); return R_ID; }
    imu_t *p = SPRIV(s, imu_t);
    memset(p, 0, sizeof *p);
    p->id = id;
    imu_ranges(s, p);
    sens_scpy(s->pub.chip, sizeof s->pub.chip, CHIP[id - 0x69]);
    si2c_wreg8(b, a, 0x12, 0x01);                  /* CTRL3_C: SW_RESET */
    aos_hal_sleep_ms(2);
    if (!si2c_wreg8(b, a, 0x12, 0x44)) return R_NACK;           /* BDU, IF_INC */
    si2c_wreg8(b, a, 0x10, (uint8_t)(0x40 | FS_XL[p->a_ix] << 2));   /* CTRL1_XL: 104 Hz */
    si2c_wreg8(b, a, 0x11, (uint8_t)(0x40 | p->g_ix << 2));          /* CTRL2_G: 104 Hz */
    /* ST's sensitivities are not quite 32768 / FS: 0.061 mg and 8.75 mdps at the smallest */
    p->a_lsb = 0.000061f * (float)(1 << p->a_ix);
    p->g_lsb = 0.00875f * (float)(1 << p->g_ix);
    return R_OK;
}

static int lsm6_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    imu_t *p = SPRIV(s, imu_t);
    uint8_t d[14], c1 = 0;
    if (!si2c_rreg8(b, a, 0x10, &c1)) return R_NACK;
    if ((c1 & 0xF0) == 0) return lost_config(s);   /* accel powered down: the reset value */
    if (!si2c_rreg(b, a, 0x20, d, 14)) return R_NACK;
    put_accel(s, 0, le16s(d + 8) * p->a_lsb, le16s(d + 10) * p->a_lsb, le16s(d + 12) * p->a_lsb);
    put_gyro(s, 3, le16s(d + 2) * p->g_lsb, le16s(d + 4) * p->g_lsb, le16s(d + 6) * p->g_lsb);
    sens_setv(s, 6, "temp", AOS_SQ_TEMP, 25.0f + le16s(d) / (p->id == 0x69 ? 16.0f : 256.0f));
    return R_OK;
}

const sens_drv_t DRV_LSM6DS = {
    .prio = SP_REG,
    .pub = { .id = "lsm6ds", .name = "LSM6DS", .addrs = { 0x6A, 0x6B }, .naddr = 2 },
    .identify = lsm6_identify, .init = lsm6_init, .read = lsm6_read,
};

/* ---- QMI8658: little-endian with CTRL1 0x40 (auto-increment); data from
 * 0x33: temperature, accel, gyro. The gyro's codes go from ±16 to ±2048
 * °/s, so the four ranges here are 256 to 2048. ---- */

static int qmi_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x00, &id) && id == 0x05 ? ID_YES : ID_NO;
}

static int qmi_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x00, &id)) return R_NACK;
    if (id != 0x05) { sens_err(s, "0x%02X", id); return R_ID; }
    imu_t *p = SPRIV(s, imu_t);
    memset(p, 0, sizeof *p);
    p->id = id;
    imu_ranges(s, p);
    si2c_wreg8(b, a, 0x60, 0xB0);                  /* RESET; it NACKs while resetting (up to 15 ms) */
    aos_hal_sleep_ms(15);
    uint8_t done = 0;
    for (int i = 0; i < 4 && done != 0x80; i++) {  /* dQY_L 0x4D = 0x80: reset done */
        if (!si2c_rreg8(b, a, 0x4D, &done)) done = 0;
        if (done != 0x80) aos_hal_sleep_ms(5);
    }
    if (!si2c_wreg8(b, a, 0x02, 0x40)) return R_NACK;           /* CTRL1: auto-increment, little-endian */
    si2c_wreg8(b, a, 0x03, (uint8_t)(p->a_ix << 4 | 0x06));     /* CTRL2: FS, ~120 Hz */
    si2c_wreg8(b, a, 0x04, (uint8_t)((p->g_ix + 4) << 4 | 0x06)); /* CTRL3: 256..2048 °/s */
    si2c_wreg8(b, a, 0x08, 0x03);                  /* CTRL7: accel and gyro on */
    p->g_lsb = (float)(256 << p->g_ix) / 32768.0f;
    return R_OK;
}

static int qmi_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    imu_t *p = SPRIV(s, imu_t);
    uint8_t d[14], c7 = 0;
    if (!si2c_rreg8(b, a, 0x08, &c7)) return R_NACK;
    if ((c7 & 0x03) == 0) return lost_config(s);
    if (!si2c_rreg(b, a, 0x33, d, 14)) return R_NACK;
    put_accel(s, 0, le16s(d + 2) * p->a_lsb, le16s(d + 4) * p->a_lsb, le16s(d + 6) * p->a_lsb);
    put_gyro(s, 3, le16s(d + 8) * p->g_lsb, le16s(d + 10) * p->g_lsb, le16s(d + 12) * p->g_lsb);
    sens_setv(s, 6, "temp", AOS_SQ_TEMP, le16s(d) / 256.0f);
    return R_OK;
}

const sens_drv_t DRV_QMI8658 = {
    .prio = SP_REG,
    .pub = { .id = "qmi8658", .name = "QMI8658", .addrs = { 0x6B, 0x6A }, .naddr = 2 },
    .identify = qmi_identify, .init = qmi_init, .read = qmi_read,
};

/* ---- LIS3DH / LIS2DH12: 12-bit high-resolution mode, 100 Hz ----
 * A multi-byte read needs bit 7 of the register address (auto-increment).
 * The temperature they give is relative (no offset trim): not shown. */

static int lis3dh_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x0F, &id) && id == 0x33 ? ID_YES : ID_NO;
}

static int lis3dh_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x0F, &id)) return R_NACK;
    if (id != 0x33) { sens_err(s, "0x%02X", id); return R_ID; }
    imu_t *p = SPRIV(s, imu_t);
    memset(p, 0, sizeof *p);
    imu_ranges(s, p);
    sens_scpy(s->pub.chip, sizeof s->pub.chip, !strncmp(s->pub.name, "lis2dh", 6) ? "LIS2DH12" : "LIS3DH");
    si2c_wreg8(b, a, 0x24, 0x80);                  /* CTRL_REG5: BOOT, reload the trimming */
    aos_hal_sleep_ms(5);
    if (!si2c_wreg8(b, a, 0x20, 0x57)) return R_NACK;           /* CTRL_REG1: 100 Hz, XYZ */
    si2c_wreg8(b, a, 0x23, (uint8_t)(0x88 | p->a_ix << 4));     /* CTRL_REG4: BDU, HR, FS */
    static const float MG[4] = { 1.0f, 2.0f, 4.0f, 12.0f };     /* mg a count in HR mode */
    p->a_lsb = MG[p->a_ix] / 1000.0f;
    return R_OK;
}

static int lis3dh_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    imu_t *p = SPRIV(s, imu_t);
    uint8_t d[6], c1 = 0;
    if (!si2c_rreg8(b, a, 0x20, &c1)) return R_NACK;
    if ((c1 & 0xF0) == 0) return lost_config(s);   /* power-down: CTRL_REG1's reset value 0x07 */
    if (!si2c_rreg(b, a, 0x28 | 0x80, d, 6)) return R_NACK;
    put_accel(s, 0, (le16s(d) >> 4) * p->a_lsb, (le16s(d + 2) >> 4) * p->a_lsb, (le16s(d + 4) >> 4) * p->a_lsb);
    return R_OK;
}

const sens_drv_t DRV_LIS3DH = {
    .prio = SP_REG,
    .pub = { .id = "lis3dh", .name = "LIS3DH", .addrs = { 0x18, 0x19 }, .naddr = 2 },
    .identify = lis3dh_identify, .init = lis3dh_init, .read = lis3dh_read,
};

/* ---- ADXL345 / ADXL343: full resolution, 3.9 mg a count at any range ---- */

static int adxl_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x00, &id) && id == 0xE5 ? ID_YES : ID_NO;
}

static int adxl_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x00, &id)) return R_NACK;
    if (id != 0xE5) { sens_err(s, "0x%02X", id); return R_ID; }
    imu_t *p = SPRIV(s, imu_t);
    memset(p, 0, sizeof *p);
    imu_ranges(s, p);
    if (!si2c_wreg8(b, a, 0x2D, 0x00)) return R_NACK;           /* POWER_CTL: standby while configuring */
    si2c_wreg8(b, a, 0x2C, 0x0A);                  /* BW_RATE: 100 Hz */
    si2c_wreg8(b, a, 0x31, (uint8_t)(0x08 | p->a_ix));          /* DATA_FORMAT: FULL_RES, range */
    si2c_wreg8(b, a, 0x2D, 0x08);                  /* POWER_CTL: measure */
    p->a_lsb = 1.0f / 256.0f;
    return R_OK;
}

static int adxl_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    imu_t *p = SPRIV(s, imu_t);
    uint8_t d[6], pc = 0;
    if (!si2c_rreg8(b, a, 0x2D, &pc)) return R_NACK;
    if (!(pc & 0x08)) return lost_config(s);       /* standby: the reset value */
    if (!si2c_rreg(b, a, 0x32, d, 6)) return R_NACK;
    put_accel(s, 0, le16s(d) * p->a_lsb, le16s(d + 2) * p->a_lsb, le16s(d + 4) * p->a_lsb);
    return R_OK;
}

const sens_drv_t DRV_ADXL345 = {
    .prio = SP_REG,
    .pub = { .id = "adxl345", .name = "ADXL345", .addrs = { 0x53, 0x1D }, .naddr = 2 },
    .identify = adxl_identify, .init = adxl_init, .read = adxl_read,
};

/* ---- BNO055: its own fusion (NDOF), heading/roll/pitch in sixteenths of
 * a degree. At 0x29 the 16-bit-index probes of the VL53L1X/VL6180X go
 * first (SP_REG2). It stretches the clock: SF_SLOW. Units: mg, °/s, °,
 * °C, Windows orientation (UNIT_SEL 0x01). The external crystal is used
 * only with "xtal=1" (boards without one would stop). ---- */

typedef struct { uint8_t lost; } bno_t;
SPRIV_CHECK(bno_t);

static int bno_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id[4];
    if (!si2c_rreg(b, a, 0x00, id, 4)) return ID_NO;
    return id[0] == 0xA0 && id[1] == 0xFB && id[2] == 0x32 && id[3] == 0x0F ? ID_YES : ID_NO;
}

static int bno_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    si2c_wreg8(b, a, 0x07, 0x00);                  /* PAGE_ID 0 (the ids are on page 0) */
    if (!si2c_rreg8(b, a, 0x00, &id)) return R_NACK;   /* NACKs for ~650 ms after power-on */
    if (id != 0xA0) { sens_err(s, "0x%02X", id); return R_ID; }
    if (!si2c_wreg8(b, a, 0x3D, 0x00)) return R_NACK;           /* OPR_MODE: CONFIG (19 ms) */
    aos_hal_sleep_ms(25);
    si2c_wreg8(b, a, 0x3E, 0x00);                  /* PWR_MODE: normal */
    si2c_wreg8(b, a, 0x3B, 0x01);                  /* UNIT_SEL: mg, °/s, °, °C, Windows */
    si2c_wreg8(b, a, 0x3F, sens_arg_int(s, "xtal", 0) ? 0x80 : 0x00);
    aos_hal_sleep_ms(10);
    if (!si2c_wreg8(b, a, 0x3D, 0x0C)) return R_NACK;           /* NDOF (7 ms) */
    aos_hal_sleep_ms(20);
    return R_OK;
}

static int bno_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    uint8_t d[24], tc[2], mode = 0;
    if (!si2c_rreg8(b, a, 0x3D, &mode)) return R_NACK;
    if ((mode & 0x0F) != 0x0C) return lost_config(s);           /* CONFIG: its power-on mode */
    if (!si2c_rreg(b, a, 0x08, d, 24)) return R_NACK;           /* accel, mag, gyro, euler */
    if (!si2c_rreg(b, a, 0x34, tc, 2)) return R_NACK;           /* TEMP, CALIB_STAT */
    sens_setv(s, 0, "heading", AOS_SQ_ANGLE, le16s(d + 18) / 16.0f);
    sens_setv(s, 1, "pitch", AOS_SQ_ANGLE, le16s(d + 22) / 16.0f);
    sens_setv(s, 2, "roll", AOS_SQ_ANGLE, le16s(d + 20) / 16.0f);
    put_accel(s, 3, le16s(d) / 1000.0f, le16s(d + 2) / 1000.0f, le16s(d + 4) / 1000.0f);
    put_gyro(s, 6, le16s(d + 12) / 16.0f, le16s(d + 14) / 16.0f, le16s(d + 16) / 16.0f);
    sens_setv(s, 9, "temp", AOS_SQ_TEMP, (float)(int8_t)tc[0]);
    sens_setv(s, 10, "calib", AOS_SQ_INDEX, (float)(tc[1] >> 6));   /* system calibration, 0..3 */
    return R_OK;
}

const sens_drv_t DRV_BNO055 = {
    .prio = SP_REG2,
    .flags = SF_SLOW,
    .pub = { .id = "bno055", .name = "BNO055", .addrs = { 0x28, 0x29 }, .naddr = 2 },
    .identify = bno_identify, .init = bno_init, .read = bno_read,
};

/* ---- magnetometers on their own ---- */

typedef struct { uint8_t id; float lsb; } mag_t;
SPRIV_CHECK(mag_t);

/* QMC5883L at 0x0D: its "chip id" 0x0D = 0xFF is what a missing register
 * gives on many chips, so it proves itself only with the rest of its
 * power-on picture: the reserved bits of the status and control registers
 * clear, and 0x0C (reserved) = 0x01. That last value is what community
 * drivers report for the part, not something we found in the datasheet;
 * a part that gives anything else is still offered as a candidate. */
static int qmc5883l_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t r[8];                                  /* 0x06..0x0D */
    if (!si2c_rreg(b, a, 0x06, r, 8)) return ID_NO;
    if (r[7] != 0xFF) return ID_NO;
    bool plausible = (r[0] & 0xF8) == 0 && (r[3] & 0x03) != 0x03 && (r[4] & 0x3E) == 0;
    if (!plausible) return ID_NO;
    return r[6] == 0x01 ? ID_YES : ID_MAYBE;
}

static int qmc5883l_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x0D, &id)) return R_NACK;
    if (id != 0xFF) { sens_err(s, "0x%02X", id); return R_ID; }
    if (!si2c_wreg8(b, a, 0x0A, 0x80)) return R_NACK;           /* soft reset */
    aos_hal_sleep_ms(2);
    si2c_wreg8(b, a, 0x0B, 0x01);                  /* SET/RESET period, as recommended */
    si2c_wreg8(b, a, 0x09, 0x1D);                  /* OSR 512, ±8 G, 200 Hz, continuous */
    SPRIV(s, mag_t)->lsb = 100.0f / 3000.0f;       /* 3000 counts a gauss */
    return R_OK;
}

static int qmc5883l_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, d[7], c1 = 0;
    if (!si2c_rreg8(b, a, 0x09, &c1)) return R_NACK;
    if ((c1 & 0x03) == 0) return lost_config(s);   /* standby: the reset value */
    if (!si2c_rreg(b, a, 0x00, d, 7)) return R_NACK;
    if (d[6] & 0x02) return R_RANGE;               /* OVL */
    float lsb = SPRIV(s, mag_t)->lsb;
    float m[3] = { le16s(d) * lsb, le16s(d + 2) * lsb, le16s(d + 4) * lsb };
    put_mag(s, 0, m);
    return R_OK;
}

const sens_drv_t DRV_QMC5883L = {
    .prio = SP_LAST,
    .pub = { .id = "qmc5883l", .name = "QMC5883L", .addrs = { 0x0D }, .naddr = 1 },
    .identify = qmc5883l_identify, .init = qmc5883l_init, .read = qmc5883l_read,
};

/* QMC5883P at 0x2C: chip id 0x00 = 0x80; data from 0x01, status 0x09 */
static int qmc5883p_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x00, &id) && id == 0x80 ? ID_YES : ID_NO;
}

static int qmc5883p_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x00, &id)) return R_NACK;
    if (id != 0x80) { sens_err(s, "0x%02X", id); return R_ID; }
    si2c_wreg8(b, a, 0x0B, 0x80);                  /* CNTL2: SOFT_RST */
    aos_hal_sleep_ms(10);
    si2c_wreg8(b, a, 0x29, 0x06);                  /* the axes' signs, as the datasheet's example */
    si2c_wreg8(b, a, 0x0B, 0x08);                  /* CNTL2: ±8 G, set/reset on */
    if (!si2c_wreg8(b, a, 0x0A, 0x8F)) return R_NACK;           /* CNTL1: OSR, 200 Hz, continuous */
    SPRIV(s, mag_t)->lsb = 100.0f / 3750.0f;       /* 3750 counts a gauss at ±8 G */
    return R_OK;
}

static int qmc5883p_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, d[9], c1 = 0;
    if (!si2c_rreg8(b, a, 0x0A, &c1)) return R_NACK;
    if ((c1 & 0x03) == 0) return lost_config(s);   /* suspend: the reset value */
    if (!si2c_rreg(b, a, 0x01, d, 9)) return R_NACK;
    if (d[8] & 0x02) return R_RANGE;               /* OVL */
    float lsb = SPRIV(s, mag_t)->lsb;
    float m[3] = { le16s(d) * lsb, le16s(d + 2) * lsb, le16s(d + 4) * lsb };
    put_mag(s, 0, m);
    return R_OK;
}

const sens_drv_t DRV_QMC5883P = {
    .prio = SP_REG,
    .pub = { .id = "qmc5883p", .name = "QMC5883P", .addrs = { 0x2C }, .naddr = 1 },
    .identify = qmc5883p_identify, .init = qmc5883p_init, .read = qmc5883p_read,
};

/* HMC5883L at 0x1E: 'H43' at 0x0A; data big-endian in X, Z, Y order;
 * -4096 is an overflow. 1090 counts a gauss at ±1.3 G. */
static int hmc_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id[3];
    return si2c_rreg(b, a, 0x0A, id, 3) && id[0] == 'H' && id[1] == '4' && id[2] == '3' ? ID_YES : ID_NO;
}

static int hmc_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id[3] = { 0 };
    if (!si2c_rreg(b, a, 0x0A, id, 3)) return R_NACK;
    if (id[0] != 'H' || id[1] != '4' || id[2] != '3') { sens_err(s, "%02X%02X%02X", id[0], id[1], id[2]); return R_ID; }
    if (!si2c_wreg8(b, a, 0x00, 0x70)) return R_NACK;           /* CRA: 8 averages, 15 Hz */
    si2c_wreg8(b, a, 0x01, 0x20);                  /* CRB: ±1.3 G */
    si2c_wreg8(b, a, 0x02, 0x00);                  /* MODE: continuous */
    SPRIV(s, mag_t)->lsb = 100.0f / 1090.0f;
    return R_OK;
}

static int hmc_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, d[6], mode = 0;
    if (!si2c_rreg8(b, a, 0x02, &mode)) return R_NACK;
    if ((mode & 0x03) != 0) return lost_config(s); /* idle or single: its reset value is 0x01 */
    if (!si2c_rreg(b, a, 0x03, d, 6)) return R_NACK;
    int16_t x = be16s(d), z = be16s(d + 2), y = be16s(d + 4);
    if (x == -4096 || y == -4096 || z == -4096) return R_RANGE;
    float lsb = SPRIV(s, mag_t)->lsb;
    float m[3] = { x * lsb, y * lsb, z * lsb };
    put_mag(s, 0, m);
    return R_OK;
}

const sens_drv_t DRV_HMC5883L = {
    .prio = SP_REG,
    .pub = { .id = "hmc5883l", .name = "HMC5883L", .addrs = { 0x1E }, .naddr = 1 },
    .identify = hmc_identify, .init = hmc_init, .read = hmc_read,
};

/* LIS3MDL at 0x1C/0x1E: ±4 G, 6842 counts a gauss, ultra-high performance */
static int lis3mdl_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x0F, &id) && id == 0x3D ? ID_YES : ID_NO;
}

static int lis3mdl_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x0F, &id)) return R_NACK;
    if (id != 0x3D) { sens_err(s, "0x%02X", id); return R_ID; }
    if (!si2c_wreg8(b, a, 0x21, 0x04)) return R_NACK;           /* CTRL_REG2: SOFT_RST */
    aos_hal_sleep_ms(2);
    si2c_wreg8(b, a, 0x20, 0x70);                  /* CTRL_REG1: X/Y ultra-high, 10 Hz */
    si2c_wreg8(b, a, 0x21, 0x00);                  /* ±4 G */
    si2c_wreg8(b, a, 0x23, 0x0C);                  /* CTRL_REG4: Z ultra-high */
    si2c_wreg8(b, a, 0x24, 0x40);                  /* CTRL_REG5: BDU */
    si2c_wreg8(b, a, 0x22, 0x00);                  /* CTRL_REG3: continuous */
    SPRIV(s, mag_t)->lsb = 100.0f / 6842.0f;
    return R_OK;
}

static int lis3mdl_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, d[6], c3 = 0;
    if (!si2c_rreg8(b, a, 0x22, &c3)) return R_NACK;
    if ((c3 & 0x03) != 0) return lost_config(s);   /* power-down: CTRL_REG3's reset value 0x03 */
    if (!si2c_rreg(b, a, 0x28 | 0x80, d, 6)) return R_NACK;
    float lsb = SPRIV(s, mag_t)->lsb;
    float m[3] = { le16s(d) * lsb, le16s(d + 2) * lsb, le16s(d + 4) * lsb };
    put_mag(s, 0, m);
    return R_OK;
}

const sens_drv_t DRV_LIS3MDL = {
    .prio = SP_REG,
    .pub = { .id = "lis3mdl", .name = "LIS3MDL", .addrs = { 0x1C, 0x1E }, .naddr = 2 },
    .identify = lis3mdl_identify, .init = lis3mdl_init, .read = lis3mdl_read,
};

/* Memsic MMC5983MA and MMC5603NJ, both at 0x30: one measurement on demand
 * (with automatic set/reset), then the temperature. Their control
 * registers are write-only, so every read sets what it needs. */

/* wait for a status bit, ~10 ms at most */
static bool mmc_wait(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t bit, int first_ms)
{
    aos_hal_sleep_ms((uint32_t)first_ms);
    for (int i = 0; i < 5; i++) {
        uint8_t st = 0;
        if (si2c_rreg8(b, a, reg, &st) && (st & bit)) return true;
        aos_hal_sleep_ms(2);
    }
    return false;
}

/* MMC5983MA: product id 0x2F = 0x30; 18 bits, 131072 at zero, 16384 a gauss */
static int mmc5983_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x2F, &id) && id == 0x30 ? ID_YES : ID_NO;
}

static int mmc5983_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x2F, &id)) return R_NACK;
    if (id != 0x30) { sens_err(s, "0x%02X", id); return R_ID; }
    if (!si2c_wreg8(b, a, 0x0A, 0x80)) return R_NACK;           /* SW_RST: 10 ms */
    aos_hal_sleep_ms(15);
    si2c_wreg8(b, a, 0x0A, 0x00);                  /* 100 Hz bandwidth: 8 ms a measurement */
    return R_OK;
}

static int mmc5983_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, d[7], t = 0;
    if (!si2c_wreg8(b, a, 0x09, 0x21)) return R_NACK;           /* TM_M, Auto_SR_en */
    if (!mmc_wait(b, a, 0x08, 0x01, 9)) return R_NOTREADY;
    if (!si2c_rreg(b, a, 0x00, d, 7)) return R_NACK;
    uint32_t x = (uint32_t)d[0] << 10 | (uint32_t)d[1] << 2 | (d[6] >> 6 & 3);
    uint32_t y = (uint32_t)d[2] << 10 | (uint32_t)d[3] << 2 | (d[6] >> 4 & 3);
    uint32_t z = (uint32_t)d[4] << 10 | (uint32_t)d[5] << 2 | (d[6] >> 2 & 3);
    float m[3] = { ((float)x - 131072.0f) * 100.0f / 16384.0f, ((float)y - 131072.0f) * 100.0f / 16384.0f,
                   ((float)z - 131072.0f) * 100.0f / 16384.0f };
    put_mag(s, 0, m);
    if (si2c_wreg8(b, a, 0x09, 0x02) && mmc_wait(b, a, 0x08, 0x02, 2) && si2c_rreg8(b, a, 0x07, &t))   /* TM_T */
        sens_setv(s, 4, "temp", AOS_SQ_TEMP, -75.0f + (float)t * 200.0f / 255.0f);
    return R_OK;
}

const sens_drv_t DRV_MMC5983 = {
    .prio = SP_REG,
    .pub = { .id = "mmc5983", .name = "MMC5983MA", .addrs = { 0x30 }, .naddr = 1 },
    .identify = mmc5983_identify, .init = mmc5983_init, .read = mmc5983_read,
};

/* MMC5603NJ: product id 0x39 = 0x10; 20 bits, 524288 at zero, 0.00625 µT a count */
static int mmc5603_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    return si2c_rreg8(b, a, 0x39, &id) && id == 0x10 ? ID_YES : ID_NO;
}

static int mmc5603_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id = 0;
    if (!si2c_rreg8(b, a, 0x39, &id)) return R_NACK;
    if (id != 0x10) { sens_err(s, "0x%02X", id); return R_ID; }
    if (!si2c_wreg8(b, a, 0x1C, 0x80)) return R_NACK;           /* CTRL1: SW_RESET */
    aos_hal_sleep_ms(20);
    si2c_wreg8(b, a, 0x1B, 0x08);                  /* SET */
    aos_hal_sleep_ms(1);
    si2c_wreg8(b, a, 0x1B, 0x10);                  /* RESET */
    aos_hal_sleep_ms(1);
    si2c_wreg8(b, a, 0x1C, 0x00);                  /* bandwidth 0: 6.6 ms a measurement */
    return R_OK;
}

static int mmc5603_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, d[9], t = 0;
    if (!si2c_wreg8(b, a, 0x1B, 0x21)) return R_NACK;           /* Take_meas_M, Auto_SR_en */
    if (!mmc_wait(b, a, 0x18, 0x40, 8)) return R_NOTREADY;
    if (!si2c_rreg(b, a, 0x00, d, 9)) return R_NACK;
    float m[3];
    for (int i = 0; i < 3; i++) {
        uint32_t v = (uint32_t)d[2 * i] << 12 | (uint32_t)d[2 * i + 1] << 4 | d[6 + i] >> 4;
        m[i] = ((float)v - 524288.0f) * 0.00625f;
    }
    put_mag(s, 0, m);
    if (si2c_wreg8(b, a, 0x1B, 0x02) && mmc_wait(b, a, 0x18, 0x80, 2) && si2c_rreg8(b, a, 0x09, &t))   /* Take_meas_T */
        sens_setv(s, 4, "temp", AOS_SQ_TEMP, -75.0f + (float)t * 0.8f);
    return R_OK;
}

const sens_drv_t DRV_MMC5603 = {
    .prio = SP_REG,
    .pub = { .id = "mmc5603", .name = "MMC5603NJ", .addrs = { 0x30 }, .naddr = 1 },
    .identify = mmc5603_identify, .init = mmc5603_init, .read = mmc5603_read,
};

/* The order breaks ties of prio at an address: at 0x68/0x69 the MPU
 * family's 0x75 first, then the ICM-4268x's, then the chips that answer at
 * 0x00 (ICM-20948, BMI160), whose 0x00 an MPU-6500 could echo by chance. */
const sens_drv_t *const SENS_G_MOTION[] = {
    &DRV_MPU6050, &DRV_ICM42688, &DRV_ICM20948, &DRV_BMI160,
    &DRV_LSM6DS, &DRV_QMI8658, &DRV_LIS3DH, &DRV_ADXL345,
    &DRV_BNO055,
    &DRV_HMC5883L, &DRV_LIS3MDL, &DRV_QMC5883P, &DRV_MMC5603, &DRV_MMC5983, &DRV_QMC5883L,
    NULL,
};

const sens_alias_t SENS_A_MOTION[] = {
    { "mpu6500", "mpu6050" }, { "mpu9250", "mpu6050" }, { "mpu9255", "mpu6050" }, { "mpu6886", "mpu6050" },
    { "icm20602", "mpu6050" }, { "icm20608", "mpu6050" }, { "icm20689", "mpu6050" },
    { "gy521", "mpu6050" }, { "gy91", "mpu6050" }, { "gy9250", "mpu6050" },
    { "icm42670", "icm42688" }, { "icm42605", "icm42688" },
    { "lsm6ds3", "lsm6ds" }, { "lsm6dsl", "lsm6ds" }, { "lsm6dso", "lsm6ds" }, { "lsm6dsox", "lsm6ds" },
    { "ism330", "lsm6ds" },
    { "lis2dh12", "lis3dh" },
    { "adxl343", "adxl345" }, { "gy291", "adxl345" },
    { "bmx160", "bmi160" },
    { "gy271", "qmc5883l" },
    { "gy273", "hmc5883l" }, { "gy85", "hmc5883l" },
    { NULL, NULL },
};
