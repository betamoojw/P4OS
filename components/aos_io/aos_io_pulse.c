/*
 * P4OS - Pulses and counts on the header's pins (aos_io.h): the part the
 * board and the simulator share. It checks the pins and claims them for
 * the owner; the backends (aos_io_pulse_p4.c, sim/io_pulse_sim.c) time the
 * edges, clock the bits and count.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool take(int gpio, const char *owner, const char *what)
{
    const aos_io_pin_t *p = aos_io_pin_of_gpio(gpio);
    if (!owner || !owner[0] || !p || (p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) {
        aos_hal_log("io", "%s: GPIO%d is not a usable pin of the header", what, gpio);
        return false;
    }
    return aos_io_claim(gpio, owner);
}

aos_io_edges_t *aos_io_edges_open(int gpio, int max, const char *owner)
{
    if (max < 1 || max > AOS_IO_EDGES_MAX || !take(gpio, owner, "edges")) return NULL;
    aos_io_edges_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->gpio = (int8_t)gpio;
    e->max = max;
    snprintf(e->owner, sizeof e->owner, "%s", owner);
    if (!aos_io_be_edges_open(e)) { free(e); return NULL; }
    return e;
}

void aos_io_edges_arm(aos_io_edges_t *e) { if (e) aos_io_be_edges_arm(e); }

int aos_io_edges_wait(aos_io_edges_t *e, uint32_t idle_us, uint32_t timeout_ms)
{
    return e ? aos_io_be_edges_wait(e, idle_us, timeout_ms) : -1;
}

int aos_io_edges_get(aos_io_edges_t *e, uint32_t *t_us, uint8_t *level, int max)
{
    return e && t_us && level && max > 0 ? aos_io_be_edges_get(e, t_us, level, max) : 0;
}

void aos_io_edges_close(aos_io_edges_t *e)
{
    if (!e) return;
    aos_io_be_edges_close(e);
    free(e);
}

static bool owned(int gpio, const char *owner)
{
    const char *o = aos_io_owner(gpio);
    return owner && o && !strcmp(o, owner);
}

bool aos_io_pulse_out(int gpio, int level, uint32_t us, const char *owner)
{
    if (!owned(gpio, owner) || us > 100000) return false;
    return aos_io_be_pulse_out(gpio, level, us);
}

bool aos_io_shift_in(int data, int clk, int bits, int extra, uint32_t *out, const char *owner)
{
    if (!out || bits < 1 || bits > 32 || extra < 0 || extra > 8) return false;
    if (!owned(data, owner) || !owned(clk, owner) || data == clk) return false;
    return aos_io_be_shift_in(data, clk, bits, extra, out);
}

aos_io_count_t *aos_io_count_open(int gpio, aos_gpio_mode_t pull, bool both_edges, uint32_t debounce_us, const char *owner)
{
    if (pull == AOS_GPIO_OUTPUT || !take(gpio, owner, "count")) return NULL;
    aos_io_count_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->gpio = (int8_t)gpio;
    c->both = both_edges;
    c->debounce_us = debounce_us;
    snprintf(c->owner, sizeof c->owner, "%s", owner);
    if (!aos_io_be_count_open(c, pull)) { free(c); return NULL; }
    return c;
}

uint32_t aos_io_count_get(aos_io_count_t *c) { return c ? aos_io_be_count_get(c) : 0; }

void aos_io_count_close(aos_io_count_t *c)
{
    if (!c) return;
    aos_io_be_count_close(c);
    free(c);
}
