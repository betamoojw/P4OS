/*
 * P4OS - Datalog: where a line's value comes from.
 *
 *   I2C, SPI, 1-Wire  the system's sensor service (aos_sensors.h), which
 *                     reads every sensor of modules.txt and the ones it
 *                     detects once a second; a line takes the latest value
 *                     of one sensor by name and key. The app never talks to
 *                     those buses itself, so Módulos, Bus and the portal
 *                     see the same readings.
 *   GPIO digital      a pin of the header, read here as an input with the
 *                     pull the line asks for (the engine claims it).
 *   GPIO analog       an ADS1115 input from the same service, in volts, in
 *                     the converter's counts, or through a straight line
 *                     into the user's unit (a 0.5-4.5 V pressure sender as
 *                     0-10 bar). Its range is the module's gain= in
 *                     modules.txt, which the form can change.
 *   Otros             the board itself: Wi-Fi signal, free memory, uptime.
 */
#include "dl.h"
#include "aos_modules.h"

#include <stdlib.h>

/* The quantity of a service key, for a sensor that is not there now (the
 * service says it with every value: ch_q() asks it first) */
static int key_q(const char *key)
{
    if (!strcmp(key, "temp") || !strcmp(key, "obj") || !strcmp(key, "amb") || !strcmp(key, "cj")) return AOS_SQ_TEMP;
    if (!strcmp(key, "hum")) return AOS_SQ_HUM;
    if (!strcmp(key, "press")) return AOS_SQ_PRESS;
    if (!strcmp(key, "lux")) return AOS_SQ_LUX;
    if (!strcmp(key, "current")) return AOS_SQ_CURR;
    if (!strcmp(key, "power")) return AOS_SQ_POWER;
    if (!strcmp(key, "co2") || !strcmp(key, "eco2")) return AOS_SQ_CO2;
    if (!strcmp(key, "tvoc")) return AOS_SQ_TVOC;
    if (!strncmp(key, "pm", 2)) return AOS_SQ_PM;
    if (!strcmp(key, "dist")) return AOS_SQ_DIST;
    return AOS_SQ_VOLT;     /* bus, shunt, a0..a3 */
}

/* Every value of the service's sensor 'name' (up to AOS_SENSOR_XVALUES) */
int dl_src_values(const char *name, aos_sensor_value_t *out, int max)
{
    int cnt = aos_sensor_count();
    for (int i = 0; i < cnt; i++) {
        aos_sensor_t s;
        if (aos_sensor_at(i, &s) && !strcmp(s.name, name)) return aos_sensor_values(i, out, max);
    }
    return 0;
}

static int ch_q(const dl_ch_t *c)
{
    aos_sensor_value_t v[AOS_SENSOR_XVALUES];
    int n = dl_src_values(c->sensor, v, AOS_SENSOR_XVALUES);
    for (int k = 0; k < n; k++) if (!strcmp(v[k].key, c->key)) return v[k].q;
    return key_q(c->key);
}

static bool same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return !*a && !*b;
}

static bool is_ads(const aos_sensor_t *s) { return same_ci(s->chip, "ads1115"); }

const char *dl_kind_name(int kind)
{
    switch (kind) {
    case DL_K_GPIO: return "GPIO";
    case DL_K_I2C: return "I2C";
    case DL_K_SPI: return "SPI";
    case DL_K_OW: return "1-Wire";
    case DL_K_PIN: return _("Pines");
    case DL_K_UART: return _("Serie");
    default: return _("Otros");
    }
}

const char *dl_sys_name(int sys)
{
    switch (sys) {
    case DL_SYS_RSSI: return _("Señal Wi-Fi");
    case DL_SYS_PSRAM: return _("PSRAM libre");
    case DL_SYS_RAM: return _("RAM interna libre");
    default: return _("Tiempo encendida");
    }
}

const char *dl_key_name(const char *key)
{
    if (!strcmp(key, "temp")) return _("Temperatura");
    if (!strcmp(key, "hum")) return _("Humedad");
    if (!strcmp(key, "press")) return _("Presión");
    if (!strcmp(key, "lux")) return _("Luz");
    if (!strcmp(key, "bus")) return _("Tensión");
    if (!strcmp(key, "current")) return _("Corriente");
    if (!strcmp(key, "power")) return _("Potencia");
    if (!strcmp(key, "shunt")) return _("En el shunt");
    if (key[0] == 'a' && key[1] >= '0' && key[1] <= '3' && !key[2]) {
        static const char *const A[4] = { "A0", "A1", "A2", "A3" };
        return A[key[1] - '0'];
    }
    return aos_sensor_key_name(key);
}

const char *dl_src_unit(const dl_ch_t *c)
{
    switch (c->kind) {
    case DL_K_GPIO:
        if (!c->analog) return "";
        if (c->an_mode == DL_AN_RAW) return "";
        if (c->an_mode == DL_AN_SCALE) return c->unit;
        return "V";
    case DL_K_SYS:
        if (c->sys == DL_SYS_RSSI) return "dBm";
        if (c->sys == DL_SYS_UPTIME) return "h";
        return "KB";
    default:
        return aos_sensor_unit(ch_q(c));
    }
}

/* Decimals for the CSV: what the sensors resolve, not more */
int dl_src_decimals(const dl_ch_t *c)
{
    switch (c->kind) {
    case DL_K_GPIO:
        if (!c->analog || c->an_mode == DL_AN_RAW) return 0;
        return c->an_mode == DL_AN_SCALE ? 3 : 4;
    case DL_K_SYS:
        return c->sys == DL_SYS_UPTIME ? 2 : 0;
    default:
        switch (ch_q(c)) {
        case AOS_SQ_TEMP: case AOS_SQ_PRESS: case AOS_SQ_UVI: case AOS_SQ_GYRO: case AOS_SQ_MAG:
        case AOS_SQ_FREQ: case AOS_SQ_FLOW: case AOS_SQ_SPEED: case AOS_SQ_RAIN: case AOS_SQ_MASS: return 2;
        case AOS_SQ_HUM: case AOS_SQ_LUX: case AOS_SQ_ANGLE: case AOS_SQ_PERCENT: return 1;
        case AOS_SQ_POWER: case AOS_SQ_ACCEL: case AOS_SQ_VOLUME: case AOS_SQ_DOSE: return 3;
        case AOS_SQ_VOLT: case AOS_SQ_CURR: case AOS_SQ_ENERGY: return 4;
        default: return 0;  /* ppm, ppb, µg/m³, mm, counts, an index, a state */
        }
    }
}

const char *dl_src_glyph(const dl_ch_t *c)
{
    switch (c->kind) {
    case DL_K_GPIO: return c->analog ? AOS_SYM_SINE_WAVE : AOS_SYM_TOGGLE_SWITCH;
    case DL_K_SYS:
        return c->sys == DL_SYS_RSSI ? AOS_SYM_WIFI : c->sys == DL_SYS_UPTIME ? AOS_SYM_TIMER_OUTLINE : AOS_SYM_MEMORY;
    default:
        return aos_sensor_glyph(ch_q(c), c->key);
    }
}

void dl_fmt_num(float v, int dec, char *out, size_t n)
{
    if (isnan(v)) { dl_scpy(out, n, "--"); return; }
    snprintf(out, n, "%.*f", dec, (double)v);
    for (char *s = out; *s; s++) if (*s == '.') *s = ',';
}

/* For the eye: a degree in tenths, the rest as the CSV has it */
void dl_fmt_value(const dl_ch_t *c, float v, bool unit, char *out, size_t n)
{
    int dec = dl_src_decimals(c);
    if (DL_K_SERVICE(c->kind) && ch_q(c) == AOS_SQ_TEMP) dec = 1;
    if (c->kind == DL_K_GPIO && c->analog && c->an_mode == DL_AN_SCALE) dec = fabsf(v) >= 100 ? 1 : 2;
    char num[32];
    dl_fmt_num(v, dec, num, sizeof num);
    const char *u = unit ? dl_src_unit(c) : "";
    if (u[0] && !isnan(v)) snprintf(out, n, "%s %s", num, u);
    else dl_scpy(out, n, num);
}

bool dl_parse_num(const char *s, float *out)
{
    char t[32];
    size_t o = 0;
    for (; *s && o + 1 < sizeof t; s++) t[o++] = *s == ',' ? '.' : *s;
    t[o] = 0;
    char *end;
    float v = strtof(t, &end);
    if (end == t || isnan(v) || isinf(v)) return false;
    while (*end == ' ') end++;
    if (*end) return false;
    *out = v;
    return true;
}

/* ---- the sensor service ---- */

/* The driver of a sensor of the service: by its name ("dht22", "ds18b20_A1B2C3"),
 * the chip= of its module line, or the chip it shows */
static const aos_sensor_chip_t *sensor_chip(const aos_sensor_t *s)
{
    const aos_sensor_chip_t *c = aos_sensor_chip_find(s->name);
    if (c) return c;
    const aos_io_module_t *m = aos_io_module_find(s->name);
    const char *a = m ? strstr(m->args, "chip=") : NULL;
    if (a) {
        char id[24];
        size_t k = 0;
        for (a += 5; *a && *a != ' ' && k + 1 < sizeof id; a++) id[k++] = *a;
        id[k] = 0;
        if ((c = aos_sensor_chip_find(id))) return c;
    }
    char low[16];
    size_t k = 0;
    for (const char *p = s->chip; *p && k + 1 < sizeof low; p++) low[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    low[k] = 0;
    return aos_sensor_chip_find(low);
}

static int sensor_kind(const aos_sensor_t *s)
{
    const aos_io_port_t *p = aos_io_port_find(s->port);
    if (!p) return -1;
    switch (p->kind) {
    case AOS_PORT_I2C: return DL_K_I2C;
    case AOS_PORT_SPI: return DL_K_SPI;
    case AOS_PORT_UART: return DL_K_UART;
    case AOS_PORT_GPIO: {
        /* a GPIO port is a 1-Wire bus or a sensor's pin */
        const aos_sensor_chip_t *c = sensor_chip(s);
        return c && c->bus == AOS_SENSOR_BUS_GPIO ? DL_K_PIN : DL_K_OW;
    }
    default: return -1;
    }
}

int dl_src_sensors(int kind, bool analog, aos_sensor_t *out, int max)
{
    int n = 0, cnt = aos_sensor_count();
    for (int i = 0; i < cnt && n < max; i++) {
        aos_sensor_t s;
        if (!aos_sensor_at(i, &s)) continue;
        if (analog ? !is_ads(&s) : sensor_kind(&s) != kind) continue;
        out[n++] = s;
    }
    return n;
}

bool dl_src_find(const char *name, aos_sensor_t *out)
{
    int cnt = aos_sensor_count();
    for (int i = 0; i < cnt; i++)
        if (aos_sensor_at(i, out) && !strcmp(out->name, name)) return true;
    return false;
}

void dl_sensor_where(const aos_sensor_t *s, char *out, size_t n)
{
    const aos_io_port_t *p = aos_io_port_find(s->port);
    if (p && p->kind == AOS_PORT_SPI) snprintf(out, n, "%.15s CS%u", s->port, (unsigned)s->addr);
    else if (p && p->kind == AOS_PORT_GPIO) snprintf(out, n, "%.15s GPIO%u", s->port, (unsigned)s->addr);
    else if (p && p->kind == AOS_PORT_UART) snprintf(out, n, "%.15s", s->port);
    else snprintf(out, n, "%.15s 0x%02X", s->port, s->addr);
}

/* The service's "why:detail" in words, as Módulos says it */
void dl_err_text(const char *err, char *out, size_t n)
{
    const char *d = strchr(err, ':');
    d = d ? d + 1 : "";
    if (!strncmp(err, "nack:spi", 8)) dl_scpy(out, n, _("No contesta: MISO quedó en 1. ¿Alimentación, CS, cables?"));
    else if (!strncmp(err, "nack:ow", 7)) dl_scpy(out, n, _("No contesta en el bus 1-Wire. ¿Alimentación, la resistencia de 4,7 kΩ, cables?"));
    else if (!strncmp(err, "nack:gpio", 9)) dl_scpy(out, n, _("No contesta en el pin. ¿Alimentación, cables, el modelo?"));
    else if (!strncmp(err, "nack:uart", 9)) dl_scpy(out, n, _("No contesta por el puerto serie. ¿TX y RX cruzados, alimentación?"));
    else if (!strncmp(err, "nack", 4)) dl_scpy(out, n, _("No contesta en esa dirección"));
    else if (!strncmp(err, "crc:sync", 8)) dl_scpy(out, n, _("No se entiende lo que manda: ¿otra velocidad u otro sensor?"));
    else if (!strncmp(err, "crc", 3)) dl_scpy(out, n, _("Llegó una lectura con el CRC mal"));
    else if (!strncmp(err, "id", 2)) snprintf(out, n, _("En esa dirección hay otro chip (dice %s)"), d);
    else if (!strncmp(err, "notready", 8)) dl_scpy(out, n, _("Todavía no midió"));
    else if (!strncmp(err, "busy", 4)) snprintf(out, n, _("Los pines los tiene %s"), d);
    else if (!strncmp(err, "port", 4)) dl_scpy(out, n, _("No se pudo abrir el puerto"));
    else if (!strncmp(err, "board", 5)) snprintf(out, n, _("Ahí está el %s de la placa"), d);
    else if (!strncmp(err, "open:rtd", 8)) dl_scpy(out, n, _("La RTD está abierta o le falta un cable"));
    else if (!strncmp(err, "open:gnd", 8)) dl_scpy(out, n, _("La termocupla está en corto a GND"));
    else if (!strncmp(err, "open:vcc", 8)) dl_scpy(out, n, _("La termocupla está en corto a VCC"));
    else if (!strncmp(err, "open:ovuv", 9)) dl_scpy(out, n, _("Una punta de la termocupla toca VCC o GND"));
    else if (!strncmp(err, "open", 4)) dl_scpy(out, n, _("La termocupla está abierta (o no está conectada)"));
    else if (!strncmp(err, "range:short", 11)) dl_scpy(out, n, _("La RTD está en corto"));
    else if (!strncmp(err, "range", 5)) dl_scpy(out, n, _("Fuera de rango"));
    else if (!strncmp(err, "cfg", 3)) snprintf(out, n, _("Falta %s= en la línea del módulo"), d);
    else dl_scpy(out, n, err);
}

/* ---- the ADS1115's range: gain= in modules.txt ---- */

int dl_ads_gain(const char *sensor)
{
    const aos_io_module_t *m = aos_io_module_find(sensor);
    int g = m ? (int)aos_io_arg_int(m->args, "gain", 4096) : 4096;
    return g > 0 ? g : 4096;
}

/* The module line of 'sensor' with gain= set, or a new line declaring it
 * (a detected one). Through aos_modules_save(), which checks the file,
 * reloads it and has the service look again, as Módulos does. */
bool dl_ads_set_gain(const char *sensor, int gain_mv)
{
    bool from_file;
    char *text = aos_modules_read(&from_file);
    if (!text) return false;
    size_t len = strlen(text), cap = len + 160;
    char *out = malloc(cap);
    if (!out) { free(text); return false; }
    size_t o = 0;
    bool done = false;
    for (char *p = text; *p;) {
        char *e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        char line[160];
        size_t cl = ln < sizeof line - 1 ? ln : sizeof line - 1;
        memcpy(line, p, cl);
        line[cl] = 0;
        char name[32] = "";
        if (!done && sscanf(line, " module %31s", name) == 1 && !strcmp(name, sensor)) {
            /* the line without its old gain=, then the new one (before a comment) */
            char keep[160], *hash = strchr(line, '#');
            if (hash) *hash = 0;
            size_t k = 0;
            for (char *t = line; *t;) {
                while (*t == ' ' || *t == '\t') t++;
                char *w = t;
                while (*t && *t != ' ' && *t != '\t') t++;
                if (t == w) break;
                if (strncmp(w, "gain=", 5)) {
                    k += (size_t)snprintf(keep + k, sizeof keep - k, "%s%.*s", k ? " " : "", (int)(t - w), w);
                    if (k >= sizeof keep) k = sizeof keep - 1;
                }
            }
            keep[k] = 0;
            o += (size_t)snprintf(out + o, cap - o, "%s gain=%d\n", keep, gain_mv);
            done = true;
        } else {
            o += (size_t)snprintf(out + o, cap - o, "%.*s\n", (int)ln, p);
        }
        if (o >= cap) o = cap - 1;
        p = e ? e + 1 : p + ln;
    }
    if (!done) {
        aos_sensor_t s;
        if (!dl_src_find(sensor, &s)) { free(text); free(out); return false; }
        o += (size_t)snprintf(out + o, cap - o, "module %s %s addr=0x%02X gain=%d\n", sensor, s.port, s.addr, gain_mv);
        if (o >= cap) o = cap - 1;
    }
    free(text);
    aos_mod_check_t ck;
    bool ok = aos_modules_save(out, o, &ck);
    free(out);
    return ok;
}

/* ---- pins ---- */

static bool in_bus_port(int gpio)
{
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        int np = p->kind == AOS_PORT_I2C ? 2 : p->kind == AOS_PORT_SPI ? 4 : p->kind == AOS_PORT_GPIO ? 1 : 0;
        bool hit = false;
        for (int k = 0; k < np; k++) hit |= p->pins[k] == gpio;
        if (!hit) continue;
        if (p->kind != AOS_PORT_GPIO) return true;
        /* a GPIO port with a module on it is a 1-Wire bus of the service */
        for (int m = 0; m < aos_io_module_count(); m++)
            if (!strcmp(aos_io_module_at(m)->port, p->name)) return true;
    }
    return false;
}

/* The port or module that names a pin without the sensor service using it
 * (a UART port, a programmer's en= and boot=), NULL for a free pin */
const char *dl_pin_port(int gpio)
{
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p->kind != AOS_PORT_UART) continue;
        for (int k = 0; k < 3; k++) if (p->pins[k] == gpio) return p->name;
    }
    static const char *const KEYS[] = { "en", "boot", "cs", "rst", "busy", "irq", "int", "dio0", "dio1", "de", "pin" };
    for (int m = 0; m < aos_io_module_count(); m++) {
        const aos_io_module_t *md = aos_io_module_at(m);
        for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++)
            if (aos_io_arg_int(md->args, KEYS[k], -1) == gpio) return md->name;
    }
    return NULL;
}

/* The header's GPIOs a line may read or drive: not the board's, not
 * reserved, not on a bus the sensor service uses, and nobody else's now.
 * The free ones first, then those a UART port or a module names. */
int dl_pins(int8_t *out, int max)
{
    const aos_io_pin_t *h = aos_io_header();
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int g = 0; g < 64 && n < max; g++) {
            const aos_io_pin_t *p = NULL;
            for (int i = 0; i < 40; i++) if (h[i].gpio == g) p = &h[i];
            if (!p || (p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) continue;
            if (in_bus_port(g)) continue;
            const char *ow = aos_io_owner(g);
            if (ow && strcmp(ow, DL_OWNER)) continue;
            if ((dl_pin_port(g) != NULL) != (pass == 1)) continue;
            out[n++] = (int8_t)g;
        }
    return n;
}

void dl_pin_label(int gpio, char *out, size_t n)
{
    const char *p = dl_pin_port(gpio);
    if (p) snprintf(out, n, "GPIO%d · %.15s", gpio, p);
    else snprintf(out, n, "GPIO%d", gpio);
}

/* ---- reading ---- */

static float uptime_hours(void)
{
    uint64_t ms = aos_hal_uptime_ms();
    uint32_t hi = (uint32_t)(ms >> 32), lo = (uint32_t)ms;
    return (float)hi * (4294967.296f / 3600.0f) + (float)lo / 3600000.0f;
}

bool dl_src_read(const dl_ch_t *c, float *v, char *err, size_t n)
{
    err[0] = 0;
    if (c->kind == DL_K_GPIO && !c->analog) {
        int l = c->pin >= 0 ? aos_io_gpio_get(c->pin) : -1;
        if (l < 0) { dl_scpy(err, n, _("No se pudo leer el pin")); return false; }
        *v = (float)((l != 0) ^ c->invert);
        return true;
    }
    if (c->kind == DL_K_SYS) {
        uint32_t ri = 0, rp = 0;
        switch (c->sys) {
        case DL_SYS_RSSI:
            if (aos_hal_net_state() != AOS_NET_CONNECTED) { dl_scpy(err, n, _("Sin Wi-Fi")); return false; }
            *v = (float)aos_hal_net_rssi();
            return true;
        case DL_SYS_PSRAM: aos_hal_heap_info(&ri, &rp); *v = (float)(rp / 1024); return true;
        case DL_SYS_RAM: aos_hal_heap_info(&ri, &rp); *v = (float)(ri / 1024); return true;
        default: *v = uptime_hours(); return true;
        }
    }
    float x;
    if (!aos_sensor_value(c->sensor, c->key, &x)) {
        aos_sensor_t s;
        if (!dl_src_find(c->sensor, &s)) dl_scpy(err, n, _("El sensor no está: ¿se desconectó o se borró de modules.txt?"));
        else if (s.err[0]) dl_err_text(s.err, err, n);
        else dl_scpy(err, n, _("Todavía no midió"));
        return false;
    }
    if (c->kind == DL_K_GPIO) {
        if (c->an_mode == DL_AN_RAW) x = roundf(x * 32768.0f / ((float)dl_ads_gain(c->sensor) / 1000.0f));
        else if (c->an_mode == DL_AN_SCALE) {
            float dx = c->x2 - c->x1;
            if (fabsf(dx) < 1e-6f) { dl_scpy(err, n, _("La escala tiene los dos puntos en la misma tensión")); return false; }
            x = c->y1 + (x - c->x1) * (c->y2 - c->y1) / dx;
        }
    }
    *v = x;
    return true;
}

bool dl_src_ready(const dl_ch_t *c)
{
    if (c->kind == DL_K_GPIO && !c->analog) return c->pin >= 0;
    if (c->kind == DL_K_SYS) return true;
    return c->sensor[0] && c->key[0];
}

void dl_src_desc(const dl_ch_t *c, char *out, size_t n)
{
    if (c->kind == DL_K_GPIO && !c->analog) {
        const char *pull = c->pull == DL_PULL_UP ? "pull-up" : c->pull == DL_PULL_DOWN ? "pull-down" : _("sin resistencia");
        snprintf(out, n, "GPIO%d · %s%s", c->pin, pull, c->invert ? _(" · invertida") : "");
        return;
    }
    if (c->kind == DL_K_SYS) { dl_scpy(out, n, dl_sys_name(c->sys)); return; }
    aos_sensor_t s;
    char where[32] = "";
    const char *chip = c->sensor;
    if (dl_src_find(c->sensor, &s)) {
        dl_sensor_where(&s, where, sizeof where);
        chip = s.chip;
    }
    if (c->kind == DL_K_GPIO) {
        static const char *const MODE[3] = { N_("en volts"), N_("crudo"), N_("escalado") };
        snprintf(out, n, "%s %s · %s · %s", chip, where, dl_key_name(c->key), dl_tr(MODE[c->an_mode < 3 ? c->an_mode : 0]));
    } else if (c->kind == DL_K_OW) {
        snprintf(out, n, "%s · %s", c->sensor, where);
    } else {
        snprintf(out, n, "%s %s · %s", chip, where, dl_key_name(c->key));
    }
}
