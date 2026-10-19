/*
 * P4OS - What the sensor service (aos_sensors.c) and its drivers share.
 *
 * A driver is one sens_drv_t in the table (sens_table.c). The table's order
 * is the order autodetect tries the drivers at an address. So the drivers
 * that prove themselves with a plain register read come first, those that
 * need a command (Sensirion's 16-bit ones) later, and those that cannot
 * prove anything (a candidate, ID_MAYBE) last. A driver that cannot be told
 * apart safely at all has SF_NOPROBE and is only ever declared.
 *
 * The service gives a driver its slot (a copy, outside the lock) and the
 * open bus. init() runs once, until it succeeds; read() runs every round, or
 * every min_ms. Both fill the slot's values with sens_setv() and return an
 * R_* code. A driver keeps what it needs between calls in s->priv:
 * calibration, the time it started a conversion.
 *
 * The rules autodetect keeps (docs/SENSORS.md):
 * - identify() only reads. It writes a register pointer and reads, or sends
 *   the documented read-only command of that chip family. It never sends a
 *   2-byte write to an unknown chip: that would set an MCP4725's output,
 *   reconfigure an MCP3421, or burn an AS5600's OTP.
 * - A sensor already in the table and reading is never identified again.
 *   An SCD4x stops measuring when it is asked who it is.
 */
#pragma once

#include "aos_sensors.h"
#include "aos_io.h"
#include "aos_hal.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENS_XFER_MS 50

/* what init() and read() return */
enum { R_OK = 0, R_NACK, R_CRC, R_ID, R_NOTREADY, R_OPEN, R_RANGE };

/* what identify() returns */
enum { ID_NO = 0, ID_YES, ID_MAYBE };

/* sens_drv_t.flags */
#define SF_NOPROBE  0x01        /* declared only: nothing safe proves it is there */
#define SF_SLOW     0x02        /* I2C at 100 kHz with a long clock-stretching wait */
#define SF_KEEPREAD 0x04        /* read() even while min_ms has not passed (it counts pulses) */
#define SF_HIDDEN   0x08        /* a second entry of a chip (a later candidate): not in the forms */

typedef struct sens_slot sens_slot_t;

/* The bus a driver works on, opened by the service */
typedef struct {
    aos_io_i2c_t  *i2c;
    aos_io_spi_t  *spi;
    aos_io_uart_t *uart;
    int            pin[2];          /* a GPIO port's pin= (and pin2=), -1 none */
} sens_io_t;

typedef struct sens_drv {
    aos_sensor_chip_t pub;          /* what aos_sensor_chip_at() gives the UIs */
    uint8_t  flags;
    uint8_t  probe[8];              /* I2C: where autodetect looks; none: pub.addrs */
    uint8_t  nprobe;
    uint16_t min_ms;                /* least time between two reads; 0 every round */
    uint8_t  spi_mode;
    uint32_t spi_hz;
    uint32_t baud;                  /* UART */
    int  (*identify)(aos_io_i2c_t *b, uint8_t addr);
    int  (*init)(sens_io_t *io, sens_slot_t *s);
    int  (*read)(sens_io_t *io, sens_slot_t *s);
} sens_drv_t;

#define SENS_PRIV 320

struct sens_slot {
    bool     used;
    aos_sensor_t pub;               /* under the lock; v[] is xv[0..3] */
    aos_sensor_value_t xv[AOS_SENSOR_XVALUES];
    int      xn;
    int      chip;                  /* index in SENS_DRV */
    bool     declared_now;          /* in modules.txt this round */
    bool     detected;              /* autodetect found it */
    char     args[96];              /* the module line's arguments */
    int32_t  shunt_mohm, gain_mv;
    bool     inited;
    uint32_t init_ms;               /* uptime when init() succeeded */
    uint32_t read_ms;               /* uptime of the last read() */
    uint64_t rom;                   /* 1-Wire: the sensor's id (family in the low byte) */
    uint8_t  ow_miss;               /* 1-Wire: searches in a row that did not find it */
    union { uint8_t b[SENS_PRIV]; uint64_t align; } priv;
    float   *hist;                  /* [AOS_SENSOR_XVALUES][AOS_SENSOR_HIST], PSRAM */
    int      h_pos, h_n;
};

/* The driver's own state in the slot */
#define SPRIV(s, T) ((T *)(void *)(s)->priv.b)
#define SPRIV_CHECK(T) _Static_assert(sizeof(T) <= SENS_PRIV, #T " does not fit in the slot")

/* ---- the table (sens_table.c) ---- */
extern const sens_drv_t *const SENS_DRV[];
extern const int SENS_NDRV;
int  sens_drv_find(const char *id);         /* index from an id, an alias or "bme280_2"; -1 */
const sens_drv_t *sens_drv(const sens_slot_t *s);

/* ---- helpers (sens_util.c) ---- */
void     sens_scpy(char *d, size_t n, const char *s);
void     sens_setv(sens_slot_t *s, int k, const char *key, int q, float v);
void     sens_err(sens_slot_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int32_t  sens_arg_int(const sens_slot_t *s, const char *key, int32_t def);
float    sens_arg_f(const sens_slot_t *s, const char *key, float def);
bool     sens_arg_str(const sens_slot_t *s, const char *key, char *out, size_t n);
uint32_t sens_now(void);                    /* uptime in ms */

uint16_t sens_be16(const uint8_t *p);
uint16_t sens_le16(const uint8_t *p);
uint32_t sens_be24(const uint8_t *p);
uint32_t sens_le24(const uint8_t *p);

/* CRC-8 for any polynomial and start; Sensirion's is 0x31 from 0xFF */
uint8_t  sens_crc8(const uint8_t *d, int n, uint8_t poly, uint8_t init);
#define  sens_crc8_si(d, n) sens_crc8((d), (n), 0x31, 0xFF)
uint16_t sens_crc16_modbus(const uint8_t *d, int n);

bool si2c_wr(aos_io_i2c_t *b, uint8_t a, const void *w, size_t n);
bool si2c_rd(aos_io_i2c_t *b, uint8_t a, void *r, size_t n);
bool si2c_wrrd(aos_io_i2c_t *b, uint8_t a, const void *w, size_t wn, void *r, size_t rn);
bool si2c_rreg(aos_io_i2c_t *b, uint8_t a, uint8_t reg, void *r, size_t n);
bool si2c_wreg8(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t v);
bool si2c_rreg8(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t *v);
bool si2c_rreg16be(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t *v);
bool si2c_rreg16le(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t *v);
bool si2c_wreg16be(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t v);
bool si2c_wreg16le(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t v);
/* 16-bit register addresses (VL53L1X, VL6180X) */
bool si2c_rreg_a16(aos_io_i2c_t *b, uint8_t a, uint16_t reg, void *r, size_t n);
bool si2c_wreg_a16(aos_io_i2c_t *b, uint8_t a, uint16_t reg, const void *v, size_t n);
/* Sensirion: a 16-bit command, optionally with words (each with its CRC),
 * and words back, each CRC checked (false on a NACK; *crc_ok false on a bad
 * CRC) */
bool si2c_cmd16(aos_io_i2c_t *b, uint8_t a, uint16_t cmd);
bool si2c_cmd16_w(aos_io_i2c_t *b, uint8_t a, uint16_t cmd, const uint16_t *w, int n);
bool si2c_words(aos_io_i2c_t *b, uint8_t a, uint16_t *w, int n, bool *crc_ok);
