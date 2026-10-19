/*
 * P4OS - The sensors on the header: drivers, autodetect and the service
 * that keeps their latest readings (aos_sensors.c and the drivers in
 * components/aos_io/sensors/, docs/MODULES.md and docs/SENSORS.md).
 *
 * A thread reads every sensor about once a second and keeps, per sensor, its
 * last values and a five-minute history. The sensors come from two places:
 *
 *   declared   a "module" line of modules.txt whose name (or chip=) is a
 *              chip with a driver: "module bme280 i2c.ext addr=0x76",
 *              "module banco ina219 i2c.ext addr=0x40 shunt=100"...
 *   detected   the I2C ports other than the board's own are probed at the
 *              addresses the supported chips use, and a chip that proves
 *              what it is (a chip id, a manufacturer id, a CRC, registers
 *              only it has at power-on) is read without being declared.
 *              One that cannot prove it (a BH1750 looks like any PCF8574)
 *              is only a CANDIDATE: the UI offers to declare it.
 *
 * The service opens a port only for the few milliseconds of a round and
 * closes it again, so the Bus app (or anybody) can use the same port in
 * between; when the pins are someone else's, the round is skipped.
 *
 * SPI chips (a MAX6675 thermocouple) are read in the same rounds, declared
 * only: "module max6675 spi.a", "module horno spi.a chip=max6675", and
 * "... cs=48" for a second one on the same wires. Their port is opened and
 * closed around each read too.
 *
 * 1-Wire temperature sensors (DS18B20, DS18S20, DS1822, MAX31850) hang off a
 * GPIO port that a module line makes a bus: "module ds18b20 gpio.ow". Every
 * sensor a search finds there is read, named after the module and the last
 * six hex digits of its serial ("ds18b20_A1B2C3": the same name forever);
 * "module heladera gpio.ow chip=ds18b20 rom=28-0123456789AB" names one. A
 * GPIO port with no such line is never driven. One conversion for the whole
 * bus starts at the beginning of a round and is read at its end (750 ms).
 *
 * Sensors on a plain GPIO port (DHT11/22, HC-SR04, HX711, pulses, a digital
 * input) and on a UART port (MH-Z19, PMS5003...) are declared the same way:
 * "module clima gpio.dht chip=dht22", "module co2 uart.a chip=mhz19".
 *
 * The same code runs in the simulator, where sim/sensors_sim.c answers on
 * i2c.ext as a BME280, an SHT31, a BH1750, an INA219 and an ADS1115
 * (P4_SIM_SENSORS), and more.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a value measures; each has one unit, always the same. */
typedef enum {
    AOS_SQ_TEMP = 0,        /* °C  */
    AOS_SQ_HUM,             /* %RH */
    AOS_SQ_PRESS,           /* hPa */
    AOS_SQ_LUX,             /* lx  */
    AOS_SQ_VOLT,            /* V   */
    AOS_SQ_CURR,            /* A   */
    AOS_SQ_POWER,           /* W   */
    /* 2026-10-19: appended, so the numbers above keep their meaning for
     * apps built before. An app that meets one it does not know shows the
     * value with aos_sensor_unit(). */
    AOS_SQ_CO2,             /* ppm (CO2, and other gases in ppm)   */
    AOS_SQ_TVOC,            /* ppb                                 */
    AOS_SQ_INDEX,           /* an index with no unit: AQI, VOC/NOx index */
    AOS_SQ_PM,              /* µg/m³                               */
    AOS_SQ_UVI,             /* UV index                            */
    AOS_SQ_DIST,            /* mm                                  */
    AOS_SQ_ACCEL,           /* g                                   */
    AOS_SQ_GYRO,            /* °/s                                 */
    AOS_SQ_MAG,             /* µT                                  */
    AOS_SQ_ANGLE,           /* °                                   */
    AOS_SQ_PERCENT,         /* % (a battery's charge, a level)     */
    AOS_SQ_OHM,             /* Ω (a gas sensor's resistance)       */
    AOS_SQ_RAW,             /* counts, as the chip gives them      */
    AOS_SQ_FREQ,            /* Hz                                  */
    AOS_SQ_FLOW,            /* L/min                               */
    AOS_SQ_VOLUME,          /* L                                   */
    AOS_SQ_SPEED,           /* m/s                                 */
    AOS_SQ_RAIN,            /* mm                                  */
    AOS_SQ_DOSE,            /* µSv/h                               */
    AOS_SQ_ENERGY,          /* Wh                                  */
    AOS_SQ_MASS,            /* g                                   */
    AOS_SQ_STATE,           /* 0 or 1: motion, a switch, a key     */
    AOS_SQ_BPM,             /* beats a minute                      */
    AOS_SQ_COUNT
} aos_sensor_q_t;

typedef enum {
    AOS_SENSOR_WAIT = 0,    /* not read yet */
    AOS_SENSOR_OK,
    AOS_SENSOR_ERR,         /* the last read failed: err says why */
    AOS_SENSOR_BUSY,        /* the port's pins were someone else's this round */
} aos_sensor_state_t;

#define AOS_SENSOR_MAX      32      /* 12 before 2026-10-19 */
/* aos_sensor_t carries the first four values, as it always has; a sensor
 * has up to AOS_SENSOR_XVALUES (an IMU, a particle counter), all of them
 * through aos_sensor_values(). */
#define AOS_SENSOR_VALUES   4
#define AOS_SENSOR_XVALUES  12
#define AOS_SENSOR_HIST     300     /* samples of history, one a second */
#define AOS_SENSOR_CAND_MAX 12

typedef struct {
    char    key[12];                /* "temp", "hum", "press", "lux", "bus", "current", "power", "a0".."a3", "co2", "pm25", "ax"... */
    uint8_t q;                      /* aos_sensor_q_t */
    float   v;
} aos_sensor_value_t;

typedef struct {
    char     name[24];              /* the module's name, or the chip's for a detected one */
    char     chip[12];              /* "BME280" */
    char     port[16];
    uint8_t  addr;                  /* I2C: the address; SPI: the chip select's GPIO; 1-Wire: the bus's GPIO */
    bool     declared;              /* from modules.txt */
    uint8_t  state;                 /* aos_sensor_state_t */
    /* why it failed, "<why>" or "<why>:<detail>": nack (no answer), crc,
     * id:<what the chip said it is>, notready, busy:<owner of the pins>,
     * port (cannot open it), board:<the board's chip at that address>,
     * open (a thermocouple not connected), nack:spi (an SPI chip whose
     * MISO stayed high: nobody there), nack:ow (no presence pulse on the
     * 1-Wire bus, or a sensor that did not answer with a good CRC) */
    char     err[48];
    uint32_t t_ms;                  /* uptime of the last good read */
    uint32_t reads, errors;
    int      n;
    aos_sensor_value_t v[AOS_SENSOR_VALUES];
} aos_sensor_t;

/* Something answered where a supported chip lives, but did not prove it is
 * that chip: the UI offers "module <chip> <port> addr=<addr>". */
typedef struct {
    char    port[16];
    uint8_t addr;
    char    chip[12];               /* the driver it would get ("bh1750") */
    char    guess[40];              /* aos_io_i2c_guess() */
} aos_sensor_cand_t;

/* The chips with a driver, for forms ("Agregar módulo"). */
typedef struct {
    const char *id;                 /* "bme280": the module name in modules.txt */
    const char *name;               /* "BME280" */
    uint8_t     addrs[4];           /* the addresses it can have, the usual one first */
    uint8_t     naddr;
    bool        shunt;              /* takes shunt= (milliohms) */
    bool        gain;               /* takes gain= (full scale in mV) */
    /* On an SPI port, declared only (nothing to probe): no addresses; its
     * chip select is the port's cs= or the module's. In aos_sensor_t its
     * addr is that CS's GPIO. Last, so that apps built before it still
     * read the fields above it where they were. */
    bool        spi;
    /* On a 1-Wire bus: a "port gpio.<x> pin=N" whose GPIO is the data line.
     * Its sensors are found by a search of the bus; in aos_sensor_t, addr
     * is that GPIO and the name ends in the sensor's serial. */
    bool        onewire;
    /* The bus it hangs from (AOS_SENSOR_BUS_*), and for GPIO the number of
     * pins its port gives (1, or 2: trigger and echo, clock and data).
     * Appended 2026-10-19. */
    uint8_t     bus;
    uint8_t     pins;
} aos_sensor_chip_t;

enum { AOS_SENSOR_BUS_I2C = 0, AOS_SENSOR_BUS_SPI, AOS_SENSOR_BUS_ONEWIRE, AOS_SENSOR_BUS_GPIO, AOS_SENSOR_BUS_UART };

/* The groups the forms show the chips in */
enum {
    AOS_SENSOR_CAT_TEMP = 0,        /* temperature and humidity, thermocouples */
    AOS_SENSOR_CAT_ENV,             /* pressure and weather */
    AOS_SENSOR_CAT_AIR,             /* CO2, gases, particles */
    AOS_SENSOR_CAT_LIGHT,           /* light, UV, colour */
    AOS_SENSOR_CAT_MOTION,          /* accelerometers, gyroscopes, compasses */
    AOS_SENSOR_CAT_POWER,           /* voltage, current, ADCs, load cells */
    AOS_SENSOR_CAT_DIST,            /* distance and presence */
    AOS_SENSOR_CAT_OTHER,
    AOS_SENSOR_CAT_COUNT
};

int                      aos_sensor_chip_count(void);
const aos_sensor_chip_t *aos_sensor_chip_at(int i);
const aos_sensor_chip_t *aos_sensor_chip_find(const char *id);   /* also "sht31", "aht10"... */
int                      aos_sensor_chip_cat(const aos_sensor_chip_t *c);   /* AOS_SENSOR_CAT_* */

/* Starts the service (once; later calls do nothing). Any of the calls below
 * starts it too. */
void aos_sensors_start(void);
/* Starts it only if modules.txt declares a sensor: for boot. */
void aos_sensors_autostart(void);
/* Somebody is looking (an app, the portal): autodetect every 5 s instead of
 * every minute, for the next 10 s. */
void aos_sensors_keep(void);
/* Forget the detected ones and probe again at the next round (after an
 * edit of modules.txt, or a "Buscar"). */
void aos_sensors_rescan(void);
/* Bumped at the end of every round: a UI redraws when it moves. */
uint32_t aos_sensors_seq(void);
/* How the last round went on each I2C port it looked at. */
typedef struct {
    char     port[16];
    uint8_t  state;                 /* OK, BUSY (owner has the pins) or ERR */
    char     owner[24];
    int      found;                 /* sensors on it */
    bool     scanned;               /* autodetect ran on it at least once */
    uint32_t scan_ms;               /* uptime of the last autodetect */
} aos_sensor_port_t;
int aos_sensors_ports(aos_sensor_port_t *out, int max);

int  aos_sensor_count(void);
bool aos_sensor_at(int i, aos_sensor_t *out);                /* a copy */
/* The latest value of one sensor by name and key: ("bme280", "temp").
 * false while it has none (not read yet, or failing). */
bool aos_sensor_value(const char *name, const char *key, float *out);
/* Every value of sensor i (the four of aos_sensor_t and the rest), as the
 * last good read left them: how many, up to max. */
int  aos_sensor_values(int i, aos_sensor_value_t *out, int max);
/* Value k of sensor i (k < AOS_SENSOR_XVALUES), oldest first; returns how
 * many (up to max). */
int  aos_sensor_history(int i, int k, float *out, int max);
int  aos_sensor_candidates(aos_sensor_cand_t *out, int max);

const char *aos_sensor_unit(int q);                          /* "°C", "hPa"... */

#ifdef __cplusplus
}
#endif
