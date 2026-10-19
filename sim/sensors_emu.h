/*
 * P4OS simulator - The chips the sensor service's drivers talk to, by
 * family: each sim/emu_<family>.c brings a NULL-terminated SIM_M_<FAMILY>
 * of models; sensors_sim.c puts them on the I2C bus when P4_SIM_SENSORS
 * names them ("bme680@0x77,scd41,vl53l0x"). A model speaks its chip's real
 * protocol (registers, commands, CRCs, conversion times), from its
 * datasheet, so that a driver is checked against the chip and not against
 * itself.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sim_emu sim_emu_t;

typedef struct sim_model {
    const char *name;               /* as P4_SIM_SENSORS spells it */
    uint8_t     addr;               /* its usual address */
    void (*reset)(sim_emu_t *d);    /* power-on state */
    /* one I2C transaction: wn bytes written, then rn read (either may be 0);
     * false is a NACK */
    bool (*xfer)(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);
} sim_model_t;

struct sim_emu {
    const sim_model_t *m;
    uint8_t  addr;
    uint8_t  ptr;                   /* a register pointer */
    uint8_t  page;                  /* a register bank */
    uint8_t  regs[256];             /* an 8-bit register file */
    uint16_t r16[256];              /* or a 16-bit one */
    uint8_t  resp[64];              /* a command's answer, waiting to be read */
    int      resp_n;
    uint64_t resp_ms;               /* when it can be read */
    uint64_t t0;                    /* the model's own time mark */
    uint32_t u[16];                 /* and its own state */
};

/* The bench, as a function of time (sensors_sim.c) */
uint64_t sim_ms(void);
double   sim_now_s(void);
double   sim_noise(double amp);
double   sim_room_t(double t);      /* °C */
double   sim_room_h(double t);      /* %RH */
double   sim_room_p(double t);      /* hPa */
double   sim_light(double t);       /* lx */
uint8_t  sim_crc8(const uint8_t *d, int n, uint8_t poly, uint8_t init);
/* the usual 8-bit register file: a write sets the pointer and stores the
 * rest from there (auto-increment), a read returns from the pointer */
bool     sim_reg8_xfer(sim_emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);
