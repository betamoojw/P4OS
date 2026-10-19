/*
 * P4OS - Sensor drivers: sensors on plain GPIOs (sens_int.h, docs/SENSORS.md).
 *
 * Declared only, on a GPIO port: "port gpio.dht pin=20" and "module dht22
 * gpio.dht"; a second pin is the module's pin2= ("module hcsr04 gpio.us
 * pin2=27": pin= the trigger or the clock, pin2= the echo or the data).
 * The pins are the service's while the sensor exists, and freed when it
 * goes. The timing is aos_io.h's pulses and counts: edges timed by an
 * interrupt, bits clocked in, edges counted by the PCNT.
 */
#include "sens_int.h"

#define OWNER AOS_IO_OWNER_SENSORS

static int need_pin2(sens_io_t *io, sens_slot_t *s)
{
    if (io->pin[1] >= 0 && io->pin[1] != io->pin[0]) return R_OK;
    sens_err(s, "pin2");
    return R_CFG;
}

static int pin_fail(sens_slot_t *s)
{
    sens_err(s, "gpio");
    return R_NACK;
}

/* ---- DHT11, DHT22 and kin: one wire, their own protocol ----
 * The board pulls the line low (18 ms for a DHT11, 1 ms is enough for the
 * rest), lets it go, and the sensor answers 80 us low and 80 high, then 40
 * bits: 50 us low and a high of 26 us (0) or 70 us (1). The bits are the
 * last 40 high times the edges give. The DHT22 sends tenths with the sign
 * in the top bit of the temperature; the DHT11 (and DHT12) integer and
 * decimal bytes. A 4.7-10 k pull-up to 3V3 is the usual; the internal one
 * does for a short cable. Two seconds between reads, as the parts want. */
typedef struct { aos_io_edges_t *e; } dht_priv_t;
SPRIV_CHECK(dht_priv_t);

static int dht_init(sens_io_t *io, sens_slot_t *s)
{
    dht_priv_t *p = SPRIV(s, dht_priv_t);
    if (!aos_io_gpio_mode(io->pin[0], AOS_GPIO_INPUT_PULLUP, OWNER)) return pin_fail(s);
    if (!p->e) p->e = aos_io_edges_open(io->pin[0], 100, OWNER);
    return p->e ? R_OK : pin_fail(s);
}

static int dht_read_any(sens_io_t *io, sens_slot_t *s, bool dht11)
{
    dht_priv_t *p = SPRIV(s, dht_priv_t);
    int pin = io->pin[0];
    aos_io_gpio_mode(pin, AOS_GPIO_OUTPUT, OWNER);
    aos_io_gpio_set(pin, 0);
    aos_hal_sleep_ms(dht11 ? 20 : 2);
    /* let go, then time: its answer starts 20-40 us later, the bits ~190 */
    aos_io_gpio_mode(pin, AOS_GPIO_INPUT_PULLUP, OWNER);
    aos_io_edges_arm(p->e);
    aos_io_edges_wait(p->e, 300, 30);
    uint32_t t[100];
    uint8_t lv[100];
    int n = aos_io_edges_get(p->e, t, lv, 100);
    if (n < 4) return pin_fail(s);
    uint32_t hi[100];
    int nh = 0;
    for (int i = 0; i + 1 < n; i++)
        if (lv[i] == 1 && lv[i + 1] == 0) hi[nh++] = t[i + 1] - t[i];
    if (nh < 40) { sens_err(s, "%d/40", nh); return R_CRC; }
    uint8_t d[5] = { 0 };
    for (int i = 0; i < 40; i++)
        if (hi[nh - 40 + i] > 48) d[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) return R_CRC;
    float rh, tc;
    if (dht11) {
        rh = d[0] + d[1] * 0.1f;
        tc = d[2] + (d[3] & 0x7F) * 0.1f;
        if (d[3] & 0x80) tc = -tc;
    } else {
        rh = (float)(d[0] << 8 | d[1]) * 0.1f;
        tc = (float)((d[2] & 0x7F) << 8 | d[3]) * 0.1f;
        if (d[2] & 0x80) tc = -tc;
    }
    if (rh > 100.0f || tc < -50.0f || tc > 90.0f || (!d[0] && !d[1] && !d[2] && !d[3])) return R_RANGE;
    sens_setv(s, 0, "temp", AOS_SQ_TEMP, tc);
    sens_setv(s, 1, "hum", AOS_SQ_HUM, rh);
    return R_OK;
}

static int dht22_read(sens_io_t *io, sens_slot_t *s) { return dht_read_any(io, s, false); }
static int dht11_read(sens_io_t *io, sens_slot_t *s) { return dht_read_any(io, s, true); }

static void edges_fini(sens_slot_t *s)
{
    dht_priv_t *p = SPRIV(s, dht_priv_t);
    aos_io_edges_close(p->e);
    p->e = NULL;
}

const sens_drv_t DRV_DHT22 = {
    .prio = SP_REG, .cat = SC(TEMP),
    .pub = { "dht22", "DHT22", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_GPIO, 1 },
    .flags = SF_NOPROBE, .min_ms = 2000,
    .init = dht_init, .read = dht22_read, .fini = edges_fini,
};

const sens_drv_t DRV_DHT11 = {
    .prio = SP_REG, .cat = SC(TEMP),
    .pub = { "dht11", "DHT11", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_GPIO, 1 },
    .flags = SF_NOPROBE, .min_ms = 2000,
    .init = dht_init, .read = dht11_read, .fini = edges_fini,
};

/* ---- ultrasonic: HC-SR04 and kin (JSN-SR04T, US-100 in pulse mode,
 * RCWL-1601), and the one-pin ones (Grove, Parallax PING) ----
 * A 10 us pulse on TRIG, and ECHO goes high as long as the sound took
 * there and back. 343 m/s at 20 °C; temp= (°C) or comp=<sensor> (one with
 * a "temp") corrects it. No echo in 30 ms is nothing in range. The 5 V
 * HC-SR04's ECHO is 5 V: a divider (1 k and 2 k) before the pin, or an
 * HC-SR04P, which runs at 3.3 V. */
typedef struct { aos_io_edges_t *e; } us_priv_t;
SPRIV_CHECK(us_priv_t);

static int us_init_pins(sens_slot_t *s, int trig, int echo)
{
    us_priv_t *p = SPRIV(s, us_priv_t);
    if (!aos_io_gpio_mode(trig, AOS_GPIO_OUTPUT, OWNER)) return pin_fail(s);
    aos_io_gpio_set(trig, 0);
    if (trig != echo && !aos_io_gpio_mode(echo, AOS_GPIO_INPUT, OWNER)) return pin_fail(s);
    if (!p->e) p->e = aos_io_edges_open(echo, 2, OWNER);
    return p->e ? R_OK : pin_fail(s);
}

static int sr04_init(sens_io_t *io, sens_slot_t *s)
{
    int r = need_pin2(io, s);
    return r == R_OK ? us_init_pins(s, io->pin[0], io->pin[1]) : r;
}

static int ping_init(sens_io_t *io, sens_slot_t *s) { return us_init_pins(s, io->pin[0], io->pin[0]); }

static int us_measure(sens_slot_t *s, int trig, int echo)
{
    us_priv_t *p = SPRIV(s, us_priv_t);
    if (trig == echo) {
        /* one pin: the trigger out, then the same pin listens */
        aos_io_gpio_mode(trig, AOS_GPIO_OUTPUT, OWNER);
        aos_io_pulse_out(trig, 1, 10, OWNER);
        aos_io_gpio_mode(echo, AOS_GPIO_INPUT, OWNER);
        aos_io_edges_arm(p->e);
    } else {
        aos_io_edges_arm(p->e);
        aos_io_pulse_out(trig, 1, 10, OWNER);
    }
    int n = aos_io_edges_wait(p->e, 0, 40);
    uint32_t t[2];
    uint8_t lv[2];
    n = aos_io_edges_get(p->e, t, lv, 2);
    if (n == 0) return pin_fail(s);
    if (n < 2 || lv[0] != 1 || t[1] > 30000) return R_RANGE;
    float c = 331.3f + 0.606f * sens_arg_f(s, "temp", 20.0f);
    char comp[24];
    float tv;
    if (sens_arg_str(s, "comp", comp, sizeof comp) && aos_sensor_value(comp, "temp", &tv)) c = 331.3f + 0.606f * tv;
    float mm = (float)t[1] * c / 2000.0f;
    if (mm < 20.0f) return R_RANGE;
    sens_setv(s, 0, "dist", AOS_SQ_DIST, mm);
    return R_OK;
}

static int sr04_read(sens_io_t *io, sens_slot_t *s) { return us_measure(s, io->pin[0], io->pin[1]); }
static int ping_read(sens_io_t *io, sens_slot_t *s) { return us_measure(s, io->pin[0], io->pin[0]); }

static void us_fini(sens_slot_t *s)
{
    us_priv_t *p = SPRIV(s, us_priv_t);
    aos_io_edges_close(p->e);
    p->e = NULL;
}

const sens_drv_t DRV_HCSR04 = {
    .prio = SP_REG, .cat = SC(DIST),
    .pub = { "hcsr04", "HC-SR04", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_GPIO, 2 },
    .flags = SF_NOPROBE,
    .init = sr04_init, .read = sr04_read, .fini = us_fini,
};

const sens_drv_t DRV_PING = {
    .prio = SP_REG, .cat = SC(DIST),
    .pub = { "ping", "Ultrasonido", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_GPIO, 1 },
    .flags = SF_NOPROBE,
    .init = ping_init, .read = ping_read, .fini = us_fini,
};

/* ---- HX711: a load cell's 24-bit converter ----
 * Two pins: pin= the clock (PD_SCK), pin2= the data (DOUT). DOUT goes low
 * when a conversion is ready (10 a second); 24 clocks read it, MSB first,
 * and one to three more choose the next: channel A at 128 (25), channel B
 * at 32 (26), channel A at 64 (27). gain=128|64|32. A clock held high
 * past 60 us powers it down, which is why shift_in() keeps them short.
 * Values: raw counts, and with scale= (counts a gram) the weight in grams
 * from tare= (the counts with nothing on it), as the NAU7802. */
typedef struct { uint32_t since; bool settled; } hx_priv_t;
SPRIV_CHECK(hx_priv_t);

static int hx_init(sens_io_t *io, sens_slot_t *s)
{
    int r = need_pin2(io, s);
    if (r != R_OK) return r;
    if (!aos_io_gpio_mode(io->pin[0], AOS_GPIO_OUTPUT, OWNER)) return pin_fail(s);
    aos_io_gpio_set(io->pin[0], 0);
    if (!aos_io_gpio_mode(io->pin[1], AOS_GPIO_INPUT, OWNER)) return pin_fail(s);
    hx_priv_t *p = SPRIV(s, hx_priv_t);
    p->since = sens_now();
    p->settled = false;
    return R_OK;
}

static int hx_read(sens_io_t *io, sens_slot_t *s)
{
    hx_priv_t *p = SPRIV(s, hx_priv_t);
    int sck = io->pin[0], dt = io->pin[1];
    /* ready within one conversion (100 ms at 10 SPS) */
    for (int i = 0; i < 25 && aos_io_gpio_get(dt) != 0; i++) aos_hal_sleep_ms(5);
    if (aos_io_gpio_get(dt) != 0) {
        if (sens_now() - p->since > 2000) return pin_fail(s);
        return R_NOTREADY;
    }
    p->since = sens_now();
    int g = sens_arg_int(s, "gain", 128), extra = g == 32 ? 2 : g == 64 ? 3 : 1;
    uint32_t v;
    if (!aos_io_shift_in(dt, sck, 24, extra, &v, OWNER)) return pin_fail(s);
    if (v == 0xFFFFFF) return pin_fail(s);              /* DOUT floating high */
    int32_t raw = (int32_t)(v << 8) >> 8;
    /* the conversion after a change of gain is the first right one */
    if (!p->settled && extra != 1) { p->settled = true; return R_NOTREADY; }
    p->settled = true;
    int k = 0;
    float scale = sens_arg_f(s, "scale", 0.0f);
    if (scale != 0.0f) sens_setv(s, k++, "weight", AOS_SQ_MASS, ((float)raw - sens_arg_f(s, "tare", 0.0f)) / scale);
    sens_setv(s, k, "raw", AOS_SQ_RAW, (float)raw);
    return R_OK;
}

const sens_drv_t DRV_HX711 = {
    .prio = SP_REG, .cat = SC(POWER),
    .pub = { "hx711", "HX711", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_GPIO, 2 },
    .flags = SF_NOPROBE,
    .init = hx_init, .read = hx_read,
};

/* ---- pulses: a counter and a rate over a window ----
 * The PCNT counts (a long debounce: an interrupt). The rate is the count's
 * change over the last 'window' seconds, from snapshots kept a little apart
 * so that a minute fits. Each sensor turns it into its unit:
 *   pulse    count and Hz, as they are
 *   flow     YF-S201 and kin: L/min = Hz / k (k= 7.5), litres since start
 *   rain     a tipping bucket: mm= a tip (0.2794), a reed switch (10 ms debounce)
 *   wind     a cup anemometer: m/s = Hz × factor= (0.667), a reed switch
 *   geiger   a GM tube board (RadiationD, J305): counts a minute, and
 *            µSv/h = CPM × factor= (0.00812, a J305; an M4011 is ~0.0057)
 * pull=up|down|none, edge=both, debounce= us override the defaults. */
#define PULSE_SNAP 30
typedef struct {
    aos_io_count_t *c;
    uint8_t  pos, n;
    uint32_t cnt[PULSE_SNAP];
    uint32_t ms[PULSE_SNAP];
} pulse_priv_t;
SPRIV_CHECK(pulse_priv_t);

enum { PK_PULSE, PK_FLOW, PK_RAIN, PK_WIND, PK_GEIGER };

static int pulse_kind(const sens_slot_t *s)
{
    const char *id = sens_drv(s) ? sens_drv(s)->pub.id : "";
    return !strcmp(id, "flow") ? PK_FLOW : !strcmp(id, "rain") ? PK_RAIN : !strcmp(id, "wind") ? PK_WIND
         : !strcmp(id, "geiger") ? PK_GEIGER : PK_PULSE;
}

static int pulse_init(sens_io_t *io, sens_slot_t *s)
{
    pulse_priv_t *p = SPRIV(s, pulse_priv_t);
    if (p->c) return R_OK;
    int kind = pulse_kind(s);
    static const uint32_t DEBOUNCE[] = { 0, 2, 10000, 2000, 0 };
    char pull[8] = "up", edge[8] = "";
    sens_arg_str(s, "pull", pull, sizeof pull);
    sens_arg_str(s, "edge", edge, sizeof edge);
    aos_gpio_mode_t m = !strcmp(pull, "down") ? AOS_GPIO_INPUT_PULLDOWN : !strcmp(pull, "none") ? AOS_GPIO_INPUT : AOS_GPIO_INPUT_PULLUP;
    uint32_t db = (uint32_t)sens_arg_int(s, "debounce", (int32_t)DEBOUNCE[kind]);
    memset(p, 0, sizeof *p);
    p->c = aos_io_count_open(io->pin[0], m, !strcmp(edge, "both"), db, OWNER);
    return p->c ? R_OK : pin_fail(s);
}

static int pulse_read(sens_io_t *io, sens_slot_t *s)
{
    (void)io;
    pulse_priv_t *p = SPRIV(s, pulse_priv_t);
    int kind = pulse_kind(s);
    static const int WINDOW[] = { 1, 3, 600, 3, 60 };
    int window = sens_arg_int(s, "window", WINDOW[kind]);
    if (window < 1) window = 1;
    uint32_t now = sens_now(), c = aos_io_count_get(p->c);
    /* a snapshot every window/PULSE_SNAP seconds (every read for short ones) */
    uint32_t step = (uint32_t)window * 1000 / PULSE_SNAP;
    int last = (p->pos + PULSE_SNAP - 1) % PULSE_SNAP;
    if (!p->n || now - p->ms[last] >= step) {
        p->cnt[p->pos] = c;
        p->ms[p->pos] = now;
        p->pos = (uint8_t)((p->pos + 1) % PULSE_SNAP);
        if (p->n < PULSE_SNAP) p->n++;
    }
    /* the oldest snapshot within the window, or the oldest there is */
    int from = -1;
    for (int i = p->n; i >= 1; i--) {
        int k = (p->pos + PULSE_SNAP - i) % PULSE_SNAP;
        if (now - p->ms[k] <= (uint32_t)window * 1000 + 500) { from = k; break; }
    }
    float hz = 0.0f;
    if (from >= 0 && now > p->ms[from]) hz = (float)(c - p->cnt[from]) * 1000.0f / (float)(now - p->ms[from]);
    if (p->n < 2) return R_NOTREADY;
    switch (kind) {
    case PK_FLOW: {
        float k = sens_arg_f(s, "k", 7.5f);
        if (k <= 0.0f) k = 7.5f;
        sens_setv(s, 0, "flow", AOS_SQ_FLOW, hz / k);
        sens_setv(s, 1, "volume", AOS_SQ_VOLUME, (float)c / (k * 60.0f));
        break;
    }
    case PK_RAIN: {
        float mm = sens_arg_f(s, "mm", 0.2794f);
        sens_setv(s, 0, "rain", AOS_SQ_RAIN, (float)c * mm);
        sens_setv(s, 1, "rainrate", AOS_SQ_RAIN, hz * 3600.0f * mm);
        break;
    }
    case PK_WIND:
        sens_setv(s, 0, "speed", AOS_SQ_SPEED, hz * sens_arg_f(s, "factor", 0.667f));
        break;
    case PK_GEIGER:
        sens_setv(s, 0, "cpm", AOS_SQ_RAW, hz * 60.0f);
        sens_setv(s, 1, "dose", AOS_SQ_DOSE, hz * 60.0f * sens_arg_f(s, "factor", 0.00812f));
        break;
    default:
        sens_setv(s, 0, "freq", AOS_SQ_FREQ, hz);
        sens_setv(s, 1, "count", AOS_SQ_RAW, (float)c);
        break;
    }
    return R_OK;
}

static void pulse_fini(sens_slot_t *s)
{
    pulse_priv_t *p = SPRIV(s, pulse_priv_t);
    aos_io_count_close(p->c);
    p->c = NULL;
}

#define PULSE_DRV(NAME, ID, LABEL, CAT) \
    const sens_drv_t NAME = { \
        .prio = SP_REG, .cat = SC(CAT), \
        .pub = { ID, LABEL, { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_GPIO, 1 }, \
        .flags = SF_NOPROBE, .init = pulse_init, .read = pulse_read, .fini = pulse_fini, \
    }

PULSE_DRV(DRV_PULSE, "pulse", "Pulsos", OTHER);
PULSE_DRV(DRV_FLOW, "flow", "Caudal", OTHER);
PULSE_DRV(DRV_RAIN, "rain", "Lluvia", ENV);
PULSE_DRV(DRV_WIND, "wind", "Anemómetro", ENV);
PULSE_DRV(DRV_GEIGER, "geiger", "Geiger", OTHER);

/* ---- a digital input: a contact, a button, a PIR ----
 * input: the level as a state, pull=up (the default: a contact to GND
 * closes it) or down or none, invert=1 for the other sense. A contact to
 * GND with the pull-up reads 1 when open: invert=1 makes "closed" 1.
 * pir: HC-SR501, AM312, RCWL-0516: their output is high while they see
 * movement (no pull). */
static int input_init(sens_io_t *io, sens_slot_t *s)
{
    bool pir = sens_drv(s) && !strcmp(sens_drv(s)->pub.id, "pir");
    char pull[8];
    sens_scpy(pull, sizeof pull, pir ? "none" : "up");
    sens_arg_str(s, "pull", pull, sizeof pull);
    aos_gpio_mode_t m = !strcmp(pull, "down") ? AOS_GPIO_INPUT_PULLDOWN : !strcmp(pull, "none") ? AOS_GPIO_INPUT : AOS_GPIO_INPUT_PULLUP;
    return aos_io_gpio_mode(io->pin[0], m, OWNER) ? R_OK : pin_fail(s);
}

static int input_read(sens_io_t *io, sens_slot_t *s)
{
    int l = aos_io_gpio_get(io->pin[0]);
    if (l < 0) return pin_fail(s);
    if (sens_arg_int(s, "invert", 0)) l = !l;
    bool pir = sens_drv(s) && !strcmp(sens_drv(s)->pub.id, "pir");
    sens_setv(s, 0, pir ? "motion" : "state", AOS_SQ_STATE, (float)l);
    return R_OK;
}

const sens_drv_t DRV_INPUT = {
    .prio = SP_REG, .cat = SC(OTHER),
    .pub = { "input", "Entrada", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_GPIO, 1 },
    .flags = SF_NOPROBE, .init = input_init, .read = input_read,
};

const sens_drv_t DRV_PIR = {
    .prio = SP_REG, .cat = SC(DIST),
    .pub = { "pir", "PIR", { 0 }, 0, false, false, false, false, AOS_SENSOR_BUS_GPIO, 1 },
    .flags = SF_NOPROBE, .init = input_init, .read = input_read,
};

const sens_drv_t *const SENS_G_GPIO[] = {
    &DRV_DHT22, &DRV_DHT11, &DRV_HCSR04, &DRV_PING, &DRV_HX711,
    &DRV_PULSE, &DRV_FLOW, &DRV_RAIN, &DRV_WIND, &DRV_GEIGER,
    &DRV_INPUT, &DRV_PIR, NULL,
};

const sens_alias_t SENS_A_GPIO[] = {
    { "am2302", "dht22" }, { "am2303", "dht22" }, { "dht21", "dht22" }, { "am2301", "dht22" },
    { "dht12", "dht11" },
    { "sr04", "hcsr04" }, { "hcsr04p", "hcsr04" }, { "jsnsr04t", "hcsr04" }, { "us100", "hcsr04" },
    { "aj_sr04m", "hcsr04" },
    { "grove_ultrasonic", "ping" }, { "parallax_ping", "ping" },
    { "yfs201", "flow" }, { "yfs401", "flow" }, { "fs300a", "flow" },
    { "anemometer", "wind" }, { "rain_gauge", "rain" },
    { "radiationd", "geiger" },
    { "contact", "input" }, { "button", "input" }, { "reed", "input" }, { "switch", "input" },
    { "hcsr501", "pir" }, { "am312", "pir" }, { "rcwl0516", "pir" },
    { NULL, NULL },
};
