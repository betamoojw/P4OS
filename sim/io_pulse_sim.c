/*
 * P4OS simulator - pulses and counts (aos_io.h, components/aos_io/aos_io_pulse.c).
 *
 * P4_SIM_GPIO_SENSORS="<gpio>=<model>[:<arg>],..." puts an emulated sensor
 * on a pin, the pin the API is called on:
 *
 *   dht22, dht11   the data pin: edges_wait() after an arm gives the chip's
 *                  answer, the room's temperature and humidity (sensors_sim.c)
 *   hcsr04, ping   the echo pin (the only one for ping): an echo as wide as a
 *                  target 250-1250 mm away, moving slowly; ":<mm>" fixes it
 *   hx711          the data pin: shift_in() gives a load cell's counts, a
 *                  350 g mug on and off the plate every 20 s
 *   pulse:<hz>     count() climbs at that rate (a flow meter, an anemometer);
 *                  ":<hz>" defaults to 10
 *
 * It does not check that the driver drove the pins the way the part wants
 * (the start pulse, the trigger): the board does that. Without a model on
 * the pin, edges time out with none and the counts stay at zero.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"
#include "sensors_emu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum { PM_NONE = 0, PM_DHT22, PM_DHT11, PM_SR04, PM_HX711, PM_PULSE };

static struct { int model; double arg; } s_pin[64];
static bool s_parsed;

static void parse(void)
{
    if (s_parsed) return;
    s_parsed = true;
    const char *spec = getenv("P4_SIM_GPIO_SENSORS");
    if (!spec) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
        int g = atoi(tok);
        char *eq = strchr(tok, '=');
        if (!eq || g < 0 || g >= 64) continue;
        char *name = eq + 1, *colon = strchr(name, ':');
        double arg = 0;
        if (colon) { *colon = 0; arg = atof(colon + 1); }
        int m = !strcasecmp(name, "dht22") || !strcasecmp(name, "am2302") ? PM_DHT22
              : !strcasecmp(name, "dht11") ? PM_DHT11
              : !strcasecmp(name, "hcsr04") || !strcasecmp(name, "ping") ? PM_SR04
              : !strcasecmp(name, "hx711") ? PM_HX711
              : !strcasecmp(name, "pulse") ? PM_PULSE : PM_NONE;
        if (m == PM_NONE) { aos_hal_log("io", "sim GPIO sensors: no model \"%s\"", name); continue; }
        s_pin[g].model = m;
        s_pin[g].arg = arg;
        aos_hal_log("io", "sim GPIO sensors: %s on GPIO%d", name, g);
    }
}

static int model_at(int gpio)
{
    parse();
    return gpio >= 0 && gpio < 64 ? s_pin[gpio].model : PM_NONE;
}

/* ---- edges ---- */

typedef struct {
    int      n;
    uint32_t t[AOS_IO_EDGES_MAX];
    uint8_t  lv[AOS_IO_EDGES_MAX];
} edges_sim_t;

static void put(edges_sim_t *b, int max, uint32_t t, int lv)
{
    if (b->n < max) { b->t[b->n] = t; b->lv[b->n] = (uint8_t)lv; b->n++; }
}

/* A DHT's answer from the release on: the pull-up, its 80/80 us, then 40
 * bits as 50 us low and 26 or 70 us high, and the line let go */
static void dht_train(edges_sim_t *b, int max, bool dht11)
{
    double now = sim_now_s();
    double tc = sim_room_t(now) + sim_noise(0.05), rh = sim_room_h(now) + sim_noise(0.2);
    uint8_t d[5];
    if (dht11) {
        int ti = (int)floor(fabs(tc)), tf = (int)((fabs(tc) - ti) * 10);
        d[0] = (uint8_t)lround(rh); d[1] = 0;
        d[2] = (uint8_t)ti; d[3] = (uint8_t)(tf | (tc < 0 ? 0x80 : 0));
    } else {
        int h = (int)lround(rh * 10), t = (int)lround(fabs(tc) * 10);
        d[0] = (uint8_t)(h >> 8); d[1] = (uint8_t)h;
        d[2] = (uint8_t)((t >> 8) | (tc < 0 ? 0x80 : 0)); d[3] = (uint8_t)t;
    }
    d[4] = (uint8_t)(d[0] + d[1] + d[2] + d[3]);
    uint32_t t = 0;
    put(b, max, t, 1);                      /* released */
    put(b, max, t += 30, 0);                /* its answer */
    put(b, max, t += 80, 1);
    put(b, max, t += 80, 0);
    for (int i = 0; i < 40; i++) {
        int bit = (d[i / 8] >> (7 - i % 8)) & 1;
        put(b, max, t += 50 + (uint32_t)(rand() % 4), 1);
        put(b, max, t += (bit ? 70 : 26) + (uint32_t)(rand() % 4), 0);
    }
    put(b, max, t += 50, 1);                /* let go */
}

static double sr04_mm(int gpio)
{
    if (s_pin[gpio].arg > 0) return s_pin[gpio].arg;
    return 750.0 + 500.0 * sin(sim_now_s() / 15.0) + sim_noise(2.0);
}

bool aos_io_be_edges_open(aos_io_edges_t *e)
{
    e->be = calloc(1, sizeof(edges_sim_t));
    return e->be != NULL;
}

void aos_io_be_edges_arm(aos_io_edges_t *e) { ((edges_sim_t *)e->be)->n = 0; }

int aos_io_be_edges_wait(aos_io_edges_t *e, uint32_t idle_us, uint32_t timeout_ms)
{
    (void)idle_us;
    edges_sim_t *b = e->be;
    b->n = 0;
    switch (model_at(e->gpio)) {
    case PM_DHT22: case PM_DHT11:
        dht_train(b, e->max, model_at(e->gpio) == PM_DHT11);
        usleep(5000);
        break;
    case PM_SR04: {
        /* 343 m/s there and back: 5.83 us a millimetre */
        uint32_t w = (uint32_t)lround(sr04_mm(e->gpio) * 2.0 / 0.343);
        put(b, e->max, 0, 1);
        put(b, e->max, w, 0);
        usleep(w + 500);
        break;
    }
    default:
        usleep(timeout_ms * 1000);
        break;
    }
    return b->n;
}

int aos_io_be_edges_get(aos_io_edges_t *e, uint32_t *t_us, uint8_t *level, int max)
{
    edges_sim_t *b = e->be;
    int n = b->n < max ? b->n : max;
    for (int i = 0; i < n; i++) { t_us[i] = b->t[i] - b->t[0]; level[i] = b->lv[i]; }
    return n;
}

void aos_io_be_edges_close(aos_io_edges_t *e)
{
    free(e->be);
    e->be = NULL;
}

/* ---- a pulse out, bits in ---- */

bool aos_io_be_pulse_out(int gpio, int level, uint32_t us)
{
    aos_io_be_gpio_set(gpio, level);
    usleep(us);
    aos_io_be_gpio_set(gpio, !level);
    return true;
}

bool aos_io_be_shift_in(int data, int clk, int bits, int extra, uint32_t *out)
{
    (void)clk;
    (void)extra;
    if (model_at(data) != PM_HX711) {
        *out = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;     /* nothing there: DOUT floats high */
        return true;
    }
    /* a load cell's zero at 84000 counts and 412.5 counts a gram */
    double t = sim_now_s();
    double g = fmod(t, 20.0) < 10.0 ? 350.0 : 0.0;
    int32_t raw = (int32_t)lround(84000.0 + 412.5 * g + sim_noise(60.0));
    *out = (uint32_t)raw & ((1u << bits) - 1);
    return true;
}

/* ---- count ---- */

typedef struct { double t0, hz; } count_sim_t;

bool aos_io_be_count_open(aos_io_count_t *c, aos_gpio_mode_t pull)
{
    (void)pull;
    count_sim_t *b = calloc(1, sizeof *b);
    if (!b) return false;
    b->t0 = sim_now_s();
    if (model_at(c->gpio) == PM_PULSE) b->hz = s_pin[c->gpio].arg > 0 ? s_pin[c->gpio].arg : 10.0;
    c->be = b;
    return true;
}

uint32_t aos_io_be_count_get(aos_io_count_t *c)
{
    count_sim_t *b = c->be;
    double e = (sim_now_s() - b->t0) * b->hz;
    return (uint32_t)(c->both ? 2.0 * e : e);
}

void aos_io_be_count_close(aos_io_count_t *c)
{
    free(c->be);
    c->be = NULL;
}
