/*
 * P4OS - Sensor drivers: distance and proximity on I2C (sens_int.h, docs/SENSORS.md).
 *
 * ST's time-of-flight lasers (VL53L0X, VL53L1X, VL6180X) all live at 0x29,
 * and so do a BNO055, a TSL2561/2591 and a TCS34725. The two that index
 * their registers with 16 bits are asked first (prio SP_REG), because an
 * 8-bit register read can leave them holding the bus; the VL53L0X, 8-bit,
 * comes right after them (prio 12) and the light and motion chips after
 * that (SP_REG2).
 *
 * What the 16-bit probes do to the 8-bit chips at 0x29: the index is a
 * 2-byte write, so an 8-bit chip takes its first byte as a register and the
 * second as data.
 * - The VL53L1X probe ([0x01 0x0F], then 2 bytes): a VL53L0X gets 0x0F in
 *   SYSTEM_SEQUENCE_CONFIG (no final range: no distance). Its init() and
 *   every read() write it again, because a sensor out of range is not
 *   "reading" and autodetect asks the address again. A BNO055's 0x01 is
 *   ACC_ID, read only.
 * - The VL6180X probe ([0x00 0x00], then 1 byte): a VL53L0X gets 0 in
 *   SYSRANGE_START (stop: it is idle anyway); a BNO055's 0x00 is CHIP_ID,
 *   read only.
 * - ams's TCS34725, TSL2591 and TSL2561 want bit 7 (CMD) set in the first
 *   byte of a write; what they do with a byte without it is not documented.
 *   At worst they take it as a register address, and then [0x01 0x0F] sets
 *   ATIME (TCS) or CONTROL (TSL2591, TIMING on a TSL2561), and [0x00 0x00]
 *   clears ENABLE/CONTROL: the chip powers down. Their own init() writes
 *   both again, and a chip that is already reading is never identified
 *   again (aos_sensors.c), so this happens before they are known, or while
 *   one is failing.
 *
 * The ultrasonic RCWL-9620 (0x57, where the MAX3010x and the EEPROM of a
 * DS3231 board live) and the TF-Luna (0x10, a VEML7700's address) have no
 * register that proves them: they are declared only.
 *
 * Out of range: a laser that sees nothing (the VL53L0X's 8190/8191, the
 * VL53L1X's and VL6180X's error statuses, the TF-Luna's weak signal) and an
 * echo that never came back return R_RANGE, so there is never a made-up
 * distance in the values or the history.
 */
#include "sens_int.h"

/* ---- VL53L0X: Pololu's vl53l0x-arduino (MIT), itself from ST's API ----
 * init() is Pololu's: data init, the reference SPADs from the NVM, ST's
 * default tuning table, the interrupt on new sample, MSRC and TCC off, a
 * timing budget (budget=, ms, 33 by default), and the VHV and phase
 * calibrations. read() is one single-shot ranging. */

#define VL0_SYSRANGE_START      0x00
#define VL0_SEQUENCE_CONFIG     0x01
#define VL0_INT_CONFIG_GPIO     0x0A
#define VL0_INT_CLEAR           0x0B
#define VL0_RESULT_INT_STATUS   0x13
#define VL0_RESULT_RANGE_STATUS 0x14
#define VL0_MSRC_CONFIG_CONTROL 0x60
#define VL0_FINAL_RATE_LIMIT    0x44
#define VL0_GPIO_HV_MUX         0x84
#define VL0_SPAD_ENABLES_REF_0  0xB0

typedef struct {
    uint8_t stop;                   /* the "stop variable" the API keeps */
} vl0_t;
SPRIV_CHECK(vl0_t);

/* ST's DefaultTuningSettings (vl53l0x_tuning.h), as Pololu writes them:
 * register, value; 0xFF selects the page */
static const uint8_t VL0_TUNING[][2] = {
    { 0xFF, 0x01 }, { 0x00, 0x00 }, { 0xFF, 0x00 }, { 0x09, 0x00 }, { 0x10, 0x00 }, { 0x11, 0x00 },
    { 0x24, 0x01 }, { 0x25, 0xFF }, { 0x75, 0x00 }, { 0xFF, 0x01 }, { 0x4E, 0x2C }, { 0x48, 0x00 },
    { 0x30, 0x20 }, { 0xFF, 0x00 }, { 0x30, 0x09 }, { 0x54, 0x00 }, { 0x31, 0x04 }, { 0x32, 0x03 },
    { 0x40, 0x83 }, { 0x46, 0x25 }, { 0x60, 0x00 }, { 0x27, 0x00 }, { 0x50, 0x06 }, { 0x51, 0x00 },
    { 0x52, 0x96 }, { 0x56, 0x08 }, { 0x57, 0x30 }, { 0x61, 0x00 }, { 0x62, 0x00 }, { 0x64, 0x00 },
    { 0x65, 0x00 }, { 0x66, 0xA0 }, { 0xFF, 0x01 }, { 0x22, 0x32 }, { 0x47, 0x14 }, { 0x49, 0xFF },
    { 0x4A, 0x00 }, { 0xFF, 0x00 }, { 0x7A, 0x0A }, { 0x7B, 0x00 }, { 0x78, 0x21 }, { 0xFF, 0x01 },
    { 0x23, 0x34 }, { 0x42, 0x00 }, { 0x44, 0xFF }, { 0x45, 0x26 }, { 0x46, 0x05 }, { 0x40, 0x40 },
    { 0x0E, 0x06 }, { 0x20, 0x1A }, { 0x43, 0x40 }, { 0xFF, 0x00 }, { 0x34, 0x03 }, { 0x35, 0x44 },
    { 0xFF, 0x01 }, { 0x31, 0x04 }, { 0x4B, 0x09 }, { 0x4C, 0x05 }, { 0x4D, 0x04 }, { 0xFF, 0x00 },
    { 0x44, 0x00 }, { 0x45, 0x20 }, { 0x47, 0x08 }, { 0x48, 0x28 }, { 0x67, 0x00 }, { 0x70, 0x04 },
    { 0x71, 0x01 }, { 0x72, 0xFE }, { 0x76, 0x00 }, { 0x77, 0x00 }, { 0xFF, 0x01 }, { 0x0D, 0x01 },
    { 0xFF, 0x00 }, { 0x80, 0x01 }, { 0x01, 0xF8 }, { 0xFF, 0x01 }, { 0x8E, 0x01 }, { 0x00, 0x01 },
    { 0xFF, 0x00 }, { 0x80, 0x00 },
};

static bool vl0_w(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t v) { return si2c_wreg8(b, a, reg, v); }

static int vl0_r(aos_io_i2c_t *b, uint8_t a, uint8_t reg)
{
    uint8_t v;
    return si2c_rreg8(b, a, reg, &v) ? v : -1;
}

/* Wait until (reg & mask) is not 0 (or is 0, when zero is set) */
static bool vl0_wait(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t mask, bool zero, int ms)
{
    for (int t = 0; t <= ms; t += 2) {
        int v = vl0_r(b, a, reg);
        if (v < 0) return false;
        if (zero ? !(v & mask) : (v & mask)) return true;
        aos_hal_sleep_ms(2);
    }
    return false;
}

static int vl0_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id[3];
    if (!si2c_rreg(b, a, 0xC0, id, 3)) return ID_NO;
    return id[0] == 0xEE && id[1] == 0xAA && id[2] == 0x10 ? ID_YES : ID_NO;
}

/* The timing budget's arithmetic (Pololu: setMeasurementTimingBudget) */
static uint32_t vl0_macro_ns(uint32_t vcsel_pclks) { return (2304u * vcsel_pclks * 1655u + 500u) / 1000u; }
static uint32_t vl0_us(uint32_t mclks, uint32_t pclks) { uint32_t m = vl0_macro_ns(pclks); return (mclks * m + m / 2) / 1000u; }
static uint32_t vl0_mclks(uint32_t us, uint32_t pclks) { uint32_t m = vl0_macro_ns(pclks); return (us * 1000u + m / 2) / m; }
static uint32_t vl0_dec(uint16_t r) { return ((uint32_t)(r & 0xFF) << (r >> 8)) + 1; }

static uint16_t vl0_enc(uint32_t mclks)
{
    if (!mclks) return 0;
    uint32_t ls = mclks - 1;
    uint16_t ms = 0;
    while (ls & 0xFFFFFF00u) { ls >>= 1; ms++; }
    return (uint16_t)(ms << 8 | (ls & 0xFF));
}

static bool vl0_budget(aos_io_i2c_t *b, uint8_t a, uint32_t budget_us)
{
    int seq = vl0_r(b, a, VL0_SEQUENCE_CONFIG), pre_v = vl0_r(b, a, 0x50), fin_v = vl0_r(b, a, 0x70), msrc = vl0_r(b, a, 0x46);
    uint16_t pre_r;
    if (seq < 0 || pre_v < 0 || fin_v < 0 || msrc < 0 || !si2c_rreg16be(b, a, 0x51, &pre_r)) return false;
    uint32_t pre_p = ((uint32_t)pre_v + 1) << 1, fin_p = ((uint32_t)fin_v + 1) << 1;
    uint32_t msrc_us = vl0_us((uint32_t)msrc + 1, pre_p);
    uint32_t pre_mclks = vl0_dec(pre_r), pre_us = vl0_us(pre_mclks, pre_p);
    bool tcc = seq >> 4 & 1, dss = seq >> 3 & 1, ms = seq >> 2 & 1, pre = seq >> 6 & 1, fin = seq >> 7 & 1;
    uint32_t used = 1910 + 960;                     /* start and end overheads */
    if (tcc) used += msrc_us + 590;
    if (dss) used += 2 * (msrc_us + 690);
    else if (ms) used += msrc_us + 660;
    if (pre) used += pre_us + 660;
    if (!fin) return true;
    used += 550;
    if (used > budget_us) return false;
    uint32_t f = vl0_mclks(budget_us - used, fin_p);
    if (pre) f += pre_mclks;
    return si2c_wreg16be(b, a, 0x71, vl0_enc(f));
}

/* One VHV or phase calibration (Pololu: performSingleRefCalibration) */
static bool vl0_refcal(aos_io_i2c_t *b, uint8_t a, uint8_t vhv)
{
    if (!vl0_w(b, a, VL0_SYSRANGE_START, (uint8_t)(0x01 | vhv))) return false;
    bool ok = vl0_wait(b, a, VL0_RESULT_INT_STATUS, 0x07, false, 100);
    vl0_w(b, a, VL0_INT_CLEAR, 0x01);
    vl0_w(b, a, VL0_SYSRANGE_START, 0x00);
    return ok;
}

static int vl0_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    vl0_t *p = SPRIV(s, vl0_t);
    uint8_t id[3];
    if (!si2c_rreg(b, a, 0xC0, id, 3)) return R_NACK;
    if (id[0] != 0xEE) { sens_err(s, "0x%02X", id[0]); return R_ID; }

    /* data init: 2V8 pads (the modules have 2.8 V regulators), standard I2C mode, the stop variable */
    int v = vl0_r(b, a, 0x89);
    if (v < 0) return R_NACK;
    vl0_w(b, a, 0x89, (uint8_t)(v | 0x01));
    vl0_w(b, a, 0x88, 0x00);
    vl0_w(b, a, 0x80, 0x01); vl0_w(b, a, 0xFF, 0x01); vl0_w(b, a, 0x00, 0x00);
    v = vl0_r(b, a, 0x91);
    vl0_w(b, a, 0x00, 0x01); vl0_w(b, a, 0xFF, 0x00); vl0_w(b, a, 0x80, 0x00);
    if (v < 0) return R_NACK;
    p->stop = (uint8_t)v;
    /* no MSRC and pre-range signal rate checks; final range limit 0.25 MCPS (Q9.7) */
    v = vl0_r(b, a, VL0_MSRC_CONFIG_CONTROL);
    vl0_w(b, a, VL0_MSRC_CONFIG_CONTROL, (uint8_t)((v < 0 ? 0 : v) | 0x12));
    si2c_wreg16be(b, a, VL0_FINAL_RATE_LIMIT, 32);
    vl0_w(b, a, VL0_SEQUENCE_CONFIG, 0xFF);

    /* the reference SPADs: how many and of which kind, from the NVM */
    vl0_w(b, a, 0x80, 0x01); vl0_w(b, a, 0xFF, 0x01); vl0_w(b, a, 0x00, 0x00);
    vl0_w(b, a, 0xFF, 0x06);
    v = vl0_r(b, a, 0x83);
    vl0_w(b, a, 0x83, (uint8_t)((v < 0 ? 0 : v) | 0x04));
    vl0_w(b, a, 0xFF, 0x07); vl0_w(b, a, 0x81, 0x01);
    vl0_w(b, a, 0x80, 0x01);
    vl0_w(b, a, 0x94, 0x6B); vl0_w(b, a, 0x83, 0x00);
    bool spad_ok = vl0_wait(b, a, 0x83, 0xFF, false, 100);
    vl0_w(b, a, 0x83, 0x01);
    int info = vl0_r(b, a, 0x92);
    vl0_w(b, a, 0x81, 0x00); vl0_w(b, a, 0xFF, 0x06);
    v = vl0_r(b, a, 0x83);
    vl0_w(b, a, 0x83, (uint8_t)((v < 0 ? 0 : v) & ~0x04));
    vl0_w(b, a, 0xFF, 0x01); vl0_w(b, a, 0x00, 0x01);
    vl0_w(b, a, 0xFF, 0x00); vl0_w(b, a, 0x80, 0x00);
    if (!spad_ok || info < 0) return R_NOTREADY;
    uint8_t count = (uint8_t)(info & 0x7F);
    bool aperture = info & 0x80;
    uint8_t map[6];
    if (!si2c_rreg(b, a, VL0_SPAD_ENABLES_REF_0, map, 6)) return R_NACK;
    vl0_w(b, a, 0xFF, 0x01);
    vl0_w(b, a, 0x4F, 0x00);                        /* DYNAMIC_SPAD_REF_EN_START_OFFSET */
    vl0_w(b, a, 0x4E, 0x2C);                        /* DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD */
    vl0_w(b, a, 0xFF, 0x00);
    vl0_w(b, a, 0xB6, 0xB4);                        /* GLOBAL_CONFIG_REF_EN_START_SELECT */
    uint8_t first = aperture ? 12 : 0, on = 0;      /* 12: the first aperture SPAD */
    for (int i = 0; i < 48; i++) {
        if (i < first || on == count) map[i / 8] &= (uint8_t)~(1u << (i % 8));
        else if (map[i / 8] >> (i % 8) & 1) on++;
    }
    uint8_t w[7] = { VL0_SPAD_ENABLES_REF_0 };
    memcpy(w + 1, map, 6);
    if (!si2c_wr(b, a, w, 7)) return R_NACK;

    for (size_t i = 0; i < sizeof VL0_TUNING / sizeof VL0_TUNING[0]; i++)
        if (!vl0_w(b, a, VL0_TUNING[i][0], VL0_TUNING[i][1])) return R_NACK;

    /* interrupt on a new sample, active low, cleared */
    vl0_w(b, a, VL0_INT_CONFIG_GPIO, 0x04);
    v = vl0_r(b, a, VL0_GPIO_HV_MUX);
    vl0_w(b, a, VL0_GPIO_HV_MUX, (uint8_t)((v < 0 ? 0 : v) & ~0x10));
    vl0_w(b, a, VL0_INT_CLEAR, 0x01);
    /* MSRC and TCC off, and the budget worked out again without them */
    vl0_w(b, a, VL0_SEQUENCE_CONFIG, 0xE8);
    int32_t ms = sens_arg_int(s, "budget", 33);
    if (ms < 20) ms = 20;
    if (ms > 100) ms = 100;                         /* a read stays well under the round */
    vl0_budget(b, a, (uint32_t)ms * 1000u);
    /* reference calibration: VHV, then phase */
    vl0_w(b, a, VL0_SEQUENCE_CONFIG, 0x01);
    if (!vl0_refcal(b, a, 0x40)) return R_NOTREADY;
    vl0_w(b, a, VL0_SEQUENCE_CONFIG, 0x02);
    if (!vl0_refcal(b, a, 0x00)) return R_NOTREADY;
    vl0_w(b, a, VL0_SEQUENCE_CONFIG, 0xE8);
    return R_OK;
}

static int vl0_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    const vl0_t *p = SPRIV(s, vl0_t);
    vl0_w(b, a, 0x80, 0x01); vl0_w(b, a, 0xFF, 0x01); vl0_w(b, a, 0x00, 0x00);
    vl0_w(b, a, 0x91, p->stop);
    vl0_w(b, a, 0x00, 0x01); vl0_w(b, a, 0xFF, 0x00); vl0_w(b, a, 0x80, 0x00);
    /* A sensor out of range is not "reading", so autodetect asks 0x29 who
     * it is again, and the VL53L1X probe leaves 0x0F in the sequence
     * config (no final range): put ours back every time. */
    vl0_w(b, a, VL0_SEQUENCE_CONFIG, 0xE8);
    if (!vl0_w(b, a, VL0_SYSRANGE_START, 0x01)) return R_NACK;
    if (!vl0_wait(b, a, VL0_SYSRANGE_START, 0x01, true, 20)) return R_NOTREADY;
    if (!vl0_wait(b, a, VL0_RESULT_INT_STATUS, 0x07, false, 120)) return R_NOTREADY;
    uint16_t mm;
    bool ok = si2c_rreg16be(b, a, VL0_RESULT_RANGE_STATUS + 10, &mm);
    vl0_w(b, a, VL0_INT_CLEAR, 0x01);
    if (!ok) return R_NACK;
    if (mm >= 8190) return R_RANGE;                 /* 8190/8191: nothing in front */
    sens_setv(s, 0, "dist", AOS_SQ_DIST, (float)mm);
    return R_OK;
}

const sens_drv_t DRV_VL53L0X = {
    .prio = 12,                                     /* after the 16-bit probes of the VL53L1X and VL6180X */
    .pub = { .id = "vl53l0x", .name = "VL53L0X", .addrs = { 0x29 }, .naddr = 1 },
    .identify = vl0_identify, .init = vl0_init, .read = vl0_read,
};

/* ---- VL53L1X: ST's ultra lite driver (VL53L1X_api.c, BSD-3) ----
 * The 91 bytes of its default configuration (long distance mode, 100 ms
 * budget and period), the dummy ranging it does for the VHV, then ranging
 * all the time: a read takes the latest result. Register indexes are 16
 * bits. */

static const uint8_t VL1_DEFAULT[91] = {            /* 0x2D..0x87 */
    0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x02, 0x08, 0x00, 0x08, 0x10, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x0B, 0x00, 0x00, 0x02, 0x0A, 0x21,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x00, 0x38, 0xFF, 0x01, 0x00, 0x08, 0x00,
    0x00, 0x01, 0xCC, 0x0F, 0x01, 0xF1, 0x0D, 0x01, 0x68, 0x00, 0x80, 0x08, 0xB8, 0x00, 0x00, 0x00,
    0x00, 0x0F, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0F, 0x0D, 0x0E, 0x0E, 0x00,
    0x00, 0x02, 0xC7, 0xFF, 0x9B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
};

#define VL1_GPIO_HV_MUX_CTRL  0x0030
#define VL1_GPIO_TIO_STATUS   0x0031
#define VL1_INT_CLEAR         0x0086
#define VL1_MODE_START        0x0087
#define VL1_RESULT_STATUS     0x0089
#define VL1_FW_STATUS         0x00E5
#define VL1_MODEL_ID          0x010F

typedef struct { uint8_t pol; } vl1_t;              /* the level that means "data ready" */
SPRIV_CHECK(vl1_t);

static bool vl1_w8(aos_io_i2c_t *b, uint8_t a, uint16_t reg, uint8_t v) { return si2c_wreg_a16(b, a, reg, &v, 1); }

static int vl1_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id[2];
    if (!si2c_rreg_a16(b, a, VL1_MODEL_ID, id, 2)) return ID_NO;
    return id[0] == 0xEA && id[1] == 0xCC ? ID_YES : ID_NO;    /* a VL53L4CD says 0xEBAA */
}

static bool vl1_ready(aos_io_i2c_t *b, uint8_t a, uint8_t pol, bool *ready)
{
    uint8_t v;
    if (!si2c_rreg_a16(b, a, VL1_GPIO_TIO_STATUS, &v, 1)) return false;
    *ready = (v & 1) == pol;
    return true;
}

static int vl1_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id[2];
    vl1_t *p = SPRIV(s, vl1_t);
    if (!si2c_rreg_a16(b, a, VL1_MODEL_ID, id, 2)) return R_NACK;
    if (id[0] != 0xEA || id[1] != 0xCC) { sens_err(s, "0x%02X%02X", id[0], id[1]); return R_ID; }
    /* booted: FIRMWARE__SYSTEM_STATUS bit 0 */
    uint8_t st = 0;
    for (int t = 0; t < 50 && !(st & 1); t++) {
        if (!si2c_rreg_a16(b, a, VL1_FW_STATUS, &st, 1)) return R_NACK;
        if (!(st & 1)) aos_hal_sleep_ms(2);
    }
    if (!(st & 1)) return R_NOTREADY;
    for (int off = 0; off < 91; off += 32) {
        int n = 91 - off < 32 ? 91 - off : 32;
        if (!si2c_wreg_a16(b, a, (uint16_t)(0x2D + off), VL1_DEFAULT + off, (size_t)n)) return R_NACK;
    }
    /* the polarity: GPIO_HV_MUX__CTRL bit 4 set is active low */
    uint8_t mux = 0;
    si2c_rreg_a16(b, a, VL1_GPIO_HV_MUX_CTRL, &mux, 1);
    p->pol = (mux & 0x10) ? 0 : 1;
    /* one ranging for the VHV, as the ULD's SensorInit does */
    vl1_w8(b, a, VL1_MODE_START, 0x40);
    bool rd = false;
    for (int t = 0; t < 40 && !rd; t++) {
        aos_hal_sleep_ms(5);
        if (!vl1_ready(b, a, p->pol, &rd)) return R_NACK;
    }
    vl1_w8(b, a, VL1_INT_CLEAR, 0x01);
    vl1_w8(b, a, VL1_MODE_START, 0x00);
    if (!rd) return R_NOTREADY;
    vl1_w8(b, a, 0x0008, 0x09);                     /* VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND: two bounds */
    vl1_w8(b, a, 0x000B, 0x00);                     /* start the VHV from the last temperature */
    return vl1_w8(b, a, VL1_MODE_START, 0x40) ? R_OK : R_NACK;   /* ranging from now on */
}

static int vl1_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr;
    const vl1_t *p = SPRIV(s, vl1_t);
    bool rd;
    if (!vl1_ready(b, a, p->pol, &rd)) return R_NACK;
    if (!rd) return R_NOTREADY;
    uint8_t r[17];                                  /* the ULD's GetResult: 0x0089..0x0099 */
    bool ok = si2c_rreg_a16(b, a, VL1_RESULT_STATUS, r, sizeof r);
    vl1_w8(b, a, VL1_INT_CLEAR, 0x01);
    if (!ok) return R_NACK;
    /* the raw status as the ULD maps it: 9 valid, 8 valid but clipped at
     * the minimum, 19 valid without the wrap check; 18 the first one after
     * a start; the rest (sigma, signal, phase, wrap...) no distance */
    uint8_t st = r[0] & 0x1F;
    if (st == 18) return R_NOTREADY;
    if (st != 9 && st != 8 && st != 19) { sens_err(s, "%u", st); return R_RANGE; }
    sens_setv(s, 0, "dist", AOS_SQ_DIST, (float)sens_be16(r + 13));
    return R_OK;
}

const sens_drv_t DRV_VL53L1X = {
    .prio = SP_REG,
    .pub = { .id = "vl53l1x", .name = "VL53L1X", .addrs = { 0x29 }, .naddr = 1 },
    .identify = vl1_identify, .init = vl1_init, .read = vl1_read,
};

/* ---- VL6180X: ST's AN4545 settings, as Adafruit_VL6180X loads them ----
 * A single-shot range each read (a few ms), and the ambient light started
 * at the end of one read (100 ms of integration, gain 1) and collected at
 * the next. */

static const struct { uint16_t reg; uint8_t v; } VL6_PRIVATE[] = {
    { 0x0207, 0x01 }, { 0x0208, 0x01 }, { 0x0096, 0x00 }, { 0x0097, 0xFD }, { 0x00E3, 0x00 },
    { 0x00E4, 0x04 }, { 0x00E5, 0x02 }, { 0x00E6, 0x01 }, { 0x00E7, 0x03 }, { 0x00F5, 0x02 },
    { 0x00D9, 0x05 }, { 0x00DB, 0xCE }, { 0x00DC, 0x03 }, { 0x00DD, 0xF8 }, { 0x009F, 0x00 },
    { 0x00A3, 0x3C }, { 0x00B7, 0x00 }, { 0x00BB, 0x3C }, { 0x00B2, 0x09 }, { 0x00CA, 0x09 },
    { 0x0198, 0x01 }, { 0x01B0, 0x17 }, { 0x01AD, 0x00 }, { 0x00FF, 0x05 }, { 0x0100, 0x05 },
    { 0x0199, 0x05 }, { 0x01A6, 0x1B }, { 0x01AC, 0x3E }, { 0x01A7, 0x1F }, { 0x0030, 0x00 },
}, VL6_PUBLIC[] = {
    { 0x0011, 0x10 },               /* GPIO1: new sample ready, polled */
    { 0x010A, 0x30 },               /* averaging sample period */
    { 0x003F, 0x46 },               /* ALS gain 1 */
    { 0x0031, 0xFF },               /* recalibrate every 255 ranges */
    { 0x0040, 0x00 }, { 0x0041, 0x63 },   /* ALS integration 100 ms */
    { 0x002E, 0x01 },               /* one temperature calibration now */
    { 0x001B, 0x09 },               /* inter-measurement periods: range 100 ms, ALS 500 ms */
    { 0x003E, 0x31 },
    { 0x0014, 0x24 },               /* interrupts on new range and new ALS sample */
};

#define VL6_FRESH_OUT_OF_RESET 0x0016
#define VL6_SYSRANGE_START     0x0018
#define VL6_SYSALS_START       0x0038
#define VL6_RANGE_STATUS       0x004D
#define VL6_INT_STATUS_GPIO    0x004F
#define VL6_ALS_VAL            0x0050
#define VL6_RANGE_VAL          0x0062
#define VL6_INT_CLEAR          0x0015

typedef struct { bool als; } vl6_t;                 /* an ALS measurement is running */
SPRIV_CHECK(vl6_t);

static bool vl6_w8(aos_io_i2c_t *b, uint8_t a, uint16_t reg, uint8_t v) { return si2c_wreg_a16(b, a, reg, &v, 1); }

static int vl6_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t id;
    if (!si2c_rreg_a16(b, a, 0x0000, &id, 1)) return ID_NO;
    return id == 0xB4 ? ID_YES : ID_NO;
}

static int vl6_init(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, id, fresh;
    if (!si2c_rreg_a16(b, a, 0x0000, &id, 1)) return R_NACK;
    if (id != 0xB4) { sens_err(s, "0x%02X", id); return R_ID; }
    if (!si2c_rreg_a16(b, a, VL6_FRESH_OUT_OF_RESET, &fresh, 1)) return R_NACK;
    if (fresh & 1) {
        for (size_t i = 0; i < sizeof VL6_PRIVATE / sizeof VL6_PRIVATE[0]; i++)
            if (!vl6_w8(b, a, VL6_PRIVATE[i].reg, VL6_PRIVATE[i].v)) return R_NACK;
        vl6_w8(b, a, VL6_FRESH_OUT_OF_RESET, 0x00);
    }
    for (size_t i = 0; i < sizeof VL6_PUBLIC / sizeof VL6_PUBLIC[0]; i++)
        if (!vl6_w8(b, a, VL6_PUBLIC[i].reg, VL6_PUBLIC[i].v)) return R_NACK;
    SPRIV(s, vl6_t)->als = false;
    return R_OK;
}

static int vl6_read(sens_io_t *io, sens_slot_t *s)
{
    aos_io_i2c_t *b = io->i2c;
    uint8_t a = s->pub.addr, st = 0;
    vl6_t *p = SPRIV(s, vl6_t);
    bool want_als = sens_arg_int(s, "als", 1) != 0;
    float lux = -1;
    /* the light started by the last read */
    if (p->als) {
        uint8_t v[2];
        if (si2c_rreg_a16(b, a, VL6_INT_STATUS_GPIO, &st, 1) && (st >> 3 & 7) == 4 && si2c_rreg_a16(b, a, VL6_ALS_VAL, v, 2))
            lux = 0.32f * (float)sens_be16(v);      /* 0.32 lx a count at gain 1 and 100 ms */
        vl6_w8(b, a, VL6_INT_CLEAR, 0x02);
        p->als = false;
    }
    /* the range: device ready, start, new sample */
    int t;
    for (t = 0; t < 10; t++) {
        if (!si2c_rreg_a16(b, a, VL6_RANGE_STATUS, &st, 1)) return R_NACK;
        if (st & 1) break;
        aos_hal_sleep_ms(2);
    }
    if (!(st & 1)) return R_NOTREADY;
    if (!vl6_w8(b, a, VL6_SYSRANGE_START, 0x01)) return R_NACK;
    for (t = 0; t < 40; t++) {
        aos_hal_sleep_ms(2);
        if (!si2c_rreg_a16(b, a, VL6_INT_STATUS_GPIO, &st, 1)) return R_NACK;
        if ((st & 7) == 4) break;
    }
    if ((st & 7) != 4) return R_NOTREADY;
    uint8_t mm = 0, rs = 0;
    bool ok = si2c_rreg_a16(b, a, VL6_RANGE_VAL, &mm, 1) && si2c_rreg_a16(b, a, VL6_RANGE_STATUS, &rs, 1);
    vl6_w8(b, a, VL6_INT_CLEAR, 0x07);
    if (!ok) return R_NACK;
    if (want_als && vl6_w8(b, a, VL6_SYSALS_START, 0x01)) p->als = true;
    /* RESULT__RANGE_STATUS bits 7:4: 0 no error; 6 no target (ECE), 7 no
     * convergence, 11 noise, 12-15 under/overflow... */
    if (rs >> 4) { sens_err(s, "%u", rs >> 4); return R_RANGE; }
    sens_setv(s, 0, "dist", AOS_SQ_DIST, (float)mm);
    if (lux >= 0) sens_setv(s, 1, "lux", AOS_SQ_LUX, lux);
    return R_OK;
}

const sens_drv_t DRV_VL6180X = {
    .prio = SP_REG,
    .pub = { .id = "vl6180x", .name = "VL6180X", .addrs = { 0x29 }, .naddr = 1 },
    .identify = vl6_identify, .init = vl6_init, .read = vl6_read,
};

/* ---- URM09 (DFRobot SEN0304): ultrasonic, 2-500 cm, and its temperature ----
 * Registers (DFRobot_URM09): 0x00 its own address, 0x01 PID (0x01), 0x02
 * version, 0x03-0x04 distance in cm, 0x05-0x06 temperature in tenths of a
 * degree (both signed big-endian), 0x07 mode and range, 0x08 command (0x01:
 * measure). Passive mode, 500 cm: triggered at the end of one read and
 * collected at the next. */

typedef struct { uint32_t trig_ms; bool trig; } urm_t;
SPRIV_CHECK(urm_t);

static int urm_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t r[2];
    if (!si2c_rreg(b, a, 0x00, r, 2)) return ID_NO;
    return r[0] == a && r[1] == 0x01 ? ID_YES : ID_NO;     /* its address register, and the PID */
}

static int urm_init(sens_io_t *io, sens_slot_t *s)
{
    uint8_t r[2];
    urm_t *p = SPRIV(s, urm_t);
    if (!si2c_rreg(io->i2c, s->pub.addr, 0x00, r, 2)) return R_NACK;
    if (r[1] != 0x01) { sens_err(s, "0x%02X", r[1]); return R_ID; }
    if (!si2c_wreg8(io->i2c, s->pub.addr, 0x07, 0x20)) return R_NACK;     /* passive, 500 cm */
    p->trig = si2c_wreg8(io->i2c, s->pub.addr, 0x08, 0x01);
    p->trig_ms = sens_now();
    return R_OK;
}

static int urm_read(sens_io_t *io, sens_slot_t *s)
{
    urm_t *p = SPRIV(s, urm_t);
    int r = R_NOTREADY;
    if (p->trig && sens_now() - p->trig_ms >= 100) {
        uint8_t d[4];
        if (!si2c_rreg(io->i2c, s->pub.addr, 0x03, d, 4)) return R_NACK;
        int16_t cm = (int16_t)sens_be16(d), t10 = (int16_t)sens_be16(d + 2);
        if (cm <= 0 || cm > 500) r = R_RANGE;
        else {
            sens_setv(s, 0, "dist", AOS_SQ_DIST, (float)cm * 10.0f);
            sens_setv(s, 1, "temp", AOS_SQ_TEMP, (float)t10 / 10.0f);
            r = R_OK;
        }
    }
    p->trig = si2c_wreg8(io->i2c, s->pub.addr, 0x08, 0x01);
    p->trig_ms = sens_now();
    return p->trig || r != R_NOTREADY ? r : R_NACK;
}

const sens_drv_t DRV_URM09 = {
    .prio = SP_REG,
    .pub = { .id = "urm09", .name = "URM09", .addrs = { 0x11 }, .naddr = 1 },
    .identify = urm_identify, .init = urm_init, .read = urm_read,
};

/* ---- RCWL-9620 / RCWL-1601 in I2C mode (M5Stack Unit Sonic I2C) ----
 * Write 0x01 to measure; 100 ms or more later three bytes come back, the
 * distance in micrometres (M5Unit-Sonic). No id at all, and 0x57 is also a
 * MAX3010x's and a DS3231 board's EEPROM: declared only. Triggered at the
 * end of one read, collected at the next. Beyond 4.5 m there is no echo. */

typedef struct { uint32_t trig_ms; bool trig; } rcwl_t;
SPRIV_CHECK(rcwl_t);

static int rcwl_read(sens_io_t *io, sens_slot_t *s)
{
    rcwl_t *p = SPRIV(s, rcwl_t);
    int r = R_NOTREADY;
    if (p->trig && sens_now() - p->trig_ms >= 120) {
        uint8_t d[3];
        if (!si2c_rd(io->i2c, s->pub.addr, d, 3)) return R_NACK;
        float mm = (float)sens_be24(d) / 1000.0f;
        if (mm <= 0 || mm > 4500) r = R_RANGE;
        else {
            sens_setv(s, 0, "dist", AOS_SQ_DIST, mm);
            r = R_OK;
        }
    }
    uint8_t c = 0x01;
    p->trig = si2c_wr(io->i2c, s->pub.addr, &c, 1);
    p->trig_ms = sens_now();
    return p->trig || r != R_NOTREADY ? r : R_NACK;
}

const sens_drv_t DRV_RCWL9620 = {
    .prio = SP_REG,
    .pub = { .id = "rcwl9620", .name = "RCWL-9620", .addrs = { 0x57 }, .naddr = 1 },
    .flags = SF_NOPROBE,
    .read = rcwl_read,
};

/* ---- TF-Luna (Benewake) in I2C mode (pin 5 to GND) ----
 * It measures on its own (100 Hz). Registers (the I2C table of its
 * manual): 0x00-0x01 distance in cm, 0x02-0x03 the signal's strength
 * (Amp), 0x04-0x05 the chip's temperature in hundredths of a degree, all
 * little-endian. Below an Amp of 100, or at 0xFFFF (overexposed), the
 * distance is not reliable. Register pointer, stop, then the read, as the
 * manual draws it. Nothing to tell it from a VEML7700 at 0x10 without
 * writing: declared only. */
static int tfl_read(sens_io_t *io, sens_slot_t *s)
{
    uint8_t reg = 0x00, d[6];
    if (!si2c_wr(io->i2c, s->pub.addr, &reg, 1) || !si2c_rd(io->i2c, s->pub.addr, d, 6)) return R_NACK;
    uint16_t cm = sens_le16(d), amp = sens_le16(d + 2);
    int16_t t = (int16_t)sens_le16(d + 4);
    if (amp < 100 || amp == 0xFFFF) { sens_err(s, "amp %u", amp); return R_RANGE; }
    sens_setv(s, 0, "dist", AOS_SQ_DIST, (float)cm * 10.0f);
    sens_setv(s, 1, "signal", AOS_SQ_RAW, (float)amp);
    sens_setv(s, 2, "die", AOS_SQ_TEMP, (float)t / 100.0f);
    return R_OK;
}

const sens_drv_t DRV_TFLUNA = {
    .prio = SP_REG,
    .pub = { .id = "tfluna", .name = "TF-Luna", .addrs = { 0x10 }, .naddr = 1 },
    .flags = SF_NOPROBE,
    .read = tfl_read,
};

const sens_drv_t *const SENS_G_DIST[] = {
    &DRV_VL53L1X, &DRV_VL6180X, &DRV_VL53L0X, &DRV_URM09, &DRV_RCWL9620, &DRV_TFLUNA,
    NULL,
};

const sens_alias_t SENS_A_DIST[] = {
    { "gy530", "vl53l0x" }, { "m5ultrasonic", "rcwl9620" }, { "rcwl1601", "rcwl9620" },
    { NULL, NULL },
};
