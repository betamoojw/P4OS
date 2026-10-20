/*
 * P4OS - PCA9685: sixteen PWM outputs over I2C (aos_io.h).
 *
 * NXP's 12-bit LED controller, the board behind most "16 servos" modules.
 * One oscillator (25 MHz inside) and one prescaler for the whole chip, so
 * the sixteen outputs share a frequency, 24 Hz to 1526 Hz; each has its own
 * on and off counts out of 4096. Registers (datasheet rev. 4, 7.3):
 *
 *   0x00 MODE1   RESTART 7, EXTCLK 6, AI 5 (auto-increment), SLEEP 4,
 *                ALLCALL 0; 0x11 at power-on (asleep, answering 0x70)
 *   0x01 MODE2   INVRT 4, OCH 3, OUTDRV 2 (totem pole), OUTNE 1..0; 0x04
 *   0x02..0x05   SUBADR1..3, ALLCALLADR: E2 E4 E8 E0 at power-on
 *   0x06 + 4n    LEDn ON_L, ON_H, OFF_L, OFF_H; bit 4 of ON_H is full on,
 *                of OFF_H full off (which wins)
 *   0xFA..0xFD   ALL_LED, the same for every output at once
 *   0xFE         PRE_SCALE, written only while asleep: osc / (4096 f) - 1
 *
 * A chip is a module line: "module servos i2c.ext chip=pca9685 addr=0x40"
 * (the name alone, "pca9685" or "pca9685_b", says it too). Its arguments:
 * freq= the frequency it starts with (50, servos), osc= the oscillator in
 * Hz when it is not 25 MHz (many clones run fast: measure and set it),
 * od=1 open drain outputs instead of totem pole, invert=1 all of them
 * inverted. The I2C port is shared (aos_io.c): the sensor service goes on
 * reading the same bus while an app has a PCA9685 open. Opening it turns
 * the all-call address off, so the chip stops answering at 0x70, where a
 * TCA9548A mux may live.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCA_OSC     25000000u
#define PCA_MODE1   0x00
#define PCA_MODE2   0x01
#define PCA_LED0    0x06
#define PCA_ALL     0xFA
#define PCA_PRE     0xFE

struct aos_io_pca {
    aos_io_i2c_t *b;
    uint8_t       addr;
    uint32_t      osc, hz;
    uint8_t       pre;
    char          desc[48];
};

static bool arg_str(const char *args, const char *key, char *out, size_t n)
{
    size_t k = strlen(key);
    for (const char *p = args; p && *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, k) && p[k] == '=') {
            p += k + 1;
            size_t i = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '#' && i + 1 < n) out[i++] = *p++;
            out[i] = 0;
            return true;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return false;
}

static bool is_pca(const aos_io_module_t *m)
{
    char chip[24];
    if (arg_str(m->args, "chip", chip, sizeof chip)) return !strcmp(chip, "pca9685");
    return !strncmp(m->name, "pca9685", 7) && (!m->name[7] || strchr("_-.", m->name[7]));
}

int aos_io_pca_modules(char names[][24], int max)
{
    int n = 0;
    for (int i = 0; i < aos_io_module_count() && n < max; i++) {
        const aos_io_module_t *m = aos_io_module_at(i);
        if (is_pca(m)) snprintf(names[n++], 24, "%s", m->name);
    }
    return n;
}

static bool wr(aos_io_pca_t *p, uint8_t reg, const uint8_t *d, size_t n)
{
    uint8_t w[5];
    if (n > 4) return false;
    w[0] = reg;
    memcpy(w + 1, d, n);
    return aos_io_i2c_xfer(p->b, p->addr, w, n + 1, NULL, 0, 50);
}

static bool wr8(aos_io_pca_t *p, uint8_t reg, uint8_t v) { return wr(p, reg, &v, 1); }

static bool rd8(aos_io_pca_t *p, uint8_t reg, uint8_t *v)
{
    return aos_io_i2c_xfer(p->b, p->addr, &reg, 1, v, 1, 50);
}

/* The prescaler is written asleep; awake again, the oscillator wants
 * 500 us before RESTART brings the outputs back where they were */
static bool set_pre(aos_io_pca_t *p, uint8_t pre)
{
    uint8_t m1;
    if (!rd8(p, PCA_MODE1, &m1)) return false;
    m1 &= (uint8_t)~0x80;
    if (!wr8(p, PCA_MODE1, (uint8_t)(m1 | 0x10)) || !wr8(p, PCA_PRE, pre) || !wr8(p, PCA_MODE1, m1)) return false;
    aos_hal_sleep_ms(1);
    if (!wr8(p, PCA_MODE1, (uint8_t)(m1 | 0x80))) return false;
    p->pre = pre;
    p->hz = (uint32_t)lroundf((float)p->osc / (4096.0f * (float)(pre + 1)));
    return true;
}

aos_io_pca_t *aos_io_pca_open(const char *module, const char *owner)
{
    const aos_io_module_t *m = module ? aos_io_module_find(module) : NULL;
    if (!m || !is_pca(m) || !owner) return NULL;
    aos_io_pca_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->addr = (uint8_t)aos_io_arg_int(m->args, "addr", 0x40);
    p->osc = (uint32_t)aos_io_arg_int(m->args, "osc", PCA_OSC);
    if (p->osc < 20000000u || p->osc > 30000000u) p->osc = PCA_OSC;
    snprintf(p->desc, sizeof p->desc, "%.15s (%.15s 0x%02X)", m->name, m->port, p->addr);
    p->b = aos_io_i2c_open(m->port, owner);
    uint8_t probe;
    if (!p->b || !rd8(p, PCA_MODE1, &probe)) {
        aos_hal_log("io", "PCA9685 %s: no answer", p->desc);
        aos_io_i2c_close(p->b);
        free(p);
        return NULL;
    }
    /* every output off, totem pole (or open drain), auto-increment on and
     * all-call off, then the frequency the module line asks for */
    uint8_t mode2 = (uint8_t)((aos_io_arg_int(m->args, "od", 0) ? 0x00 : 0x04) | (aos_io_arg_int(m->args, "invert", 0) ? 0x10 : 0));
    uint8_t off[4] = { 0, 0, 0, 0x10 };
    bool ok = wr(p, PCA_ALL, off, 4) && wr8(p, PCA_MODE2, mode2) && wr8(p, PCA_MODE1, 0x20);
    aos_hal_sleep_ms(1);
    if (!ok || !aos_io_pca_set_freq(p, (uint32_t)aos_io_arg_int(m->args, "freq", 50))) {
        aos_hal_log("io", "PCA9685 %s: could not be set up", p->desc);
        aos_io_i2c_close(p->b);
        free(p);
        return NULL;
    }
    return p;
}

uint32_t aos_io_pca_set_freq(aos_io_pca_t *p, uint32_t hz)
{
    if (!p || !hz) return 0;
    long pre = lroundf((float)p->osc / (4096.0f * (float)hz)) - 1;
    if (pre < 3) pre = 3;
    if (pre > 255) pre = 255;
    if ((uint8_t)pre == p->pre && p->hz) return p->hz;
    return set_pre(p, (uint8_t)pre) ? p->hz : 0;
}

uint32_t aos_io_pca_freq(const aos_io_pca_t *p) { return p ? p->hz : 0; }

/* The output's period in exact terms, for a pulse in microseconds */
static float period_us(const aos_io_pca_t *p) { return 4096.0f * (float)(p->pre + 1) * 1e6f / (float)p->osc; }

bool aos_io_pca_set_duty(aos_io_pca_t *p, int ch, float duty)
{
    if (!p || ch < 0 || ch > 15) return false;
    if (!(duty > 0.0f)) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    int counts = (int)lroundf(duty * 4096.0f);
    uint8_t d[4] = { 0, 0, 0, 0 };
    if (counts >= 4096) d[1] = 0x10;                /* full on */
    else if (counts <= 0) d[3] = 0x10;              /* full off */
    else { d[2] = (uint8_t)counts; d[3] = (uint8_t)(counts >> 8); }
    return wr(p, (uint8_t)(PCA_LED0 + 4 * ch), d, 4);
}

bool aos_io_pca_set_pulse_us(aos_io_pca_t *p, int ch, float us)
{
    if (!p || !p->pre) return false;
    return aos_io_pca_set_duty(p, ch, us / period_us(p));
}

float aos_io_pca_period_us(const aos_io_pca_t *p) { return p && p->pre ? period_us(p) : 0.0f; }

const char *aos_io_pca_desc(const aos_io_pca_t *p) { return p ? p->desc : ""; }

void aos_io_pca_close(aos_io_pca_t *p)
{
    if (!p) return;
    /* every output off and the oscillator asleep: a servo goes limp, a LED dark */
    uint8_t off[4] = { 0, 0, 0, 0x10 };
    wr(p, PCA_ALL, off, 4);
    wr8(p, PCA_MODE1, 0x30);
    aos_io_i2c_close(p->b);
    free(p);
}
