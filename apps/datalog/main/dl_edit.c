/*
 * P4OS - Datalog: the form of a line, new or to change.
 *
 * In the order a person thinks it: when it reads (every so often, or at
 * some times of day), what kind of source (GPIO, I2C, SPI, 1-Wire, the
 * board), then what that kind asks (the bus, the sensor on it and which of
 * its values; a pin with its pull; an ADS1115 input with its range and how
 * to show it), what it reads right now, and the alarms. A choice rebuilds
 * the form in place, keeping where it was scrolled, as Módulos' form does.
 *
 * Nothing is written until Guardar: the draft is a dl_ch_t of its own,
 * handed whole to dl_ch_put(). The one exception is the ADS1115's range,
 * which is the module's gain= in modules.txt and is shared by its four
 * inputs: that is written when it is picked, and said so.
 */
#include "dl.h"

static struct {
    dl_ch_t d;
    int idx;                        /* -1: a new line */
    bool name_auto;
    char bus[16];
    uint32_t unit;                  /* the interval's unit: 1, 60, 3600 s */
    lv_obj_t *col, *preview;
    float *num_target;
    char sig[256];
    uint32_t prev_ms;
} D;

static const int GAINS[] = { 6144, 4096, 2048, 1024, 512, 256 };

static void build(void);

static int32_t col_w(void)
{
    int32_t w = U.W - 2 * AOS_UI_PAD;
    return U.land && w > 960 ? 960 : w;
}

/* ---- names ---- */

static void auto_name(void)
{
    if (!D.name_auto) return;
    dl_ch_t *c = &D.d;
    char n[40] = "";
    if (c->kind == DL_K_GPIO && !c->analog) {
        if (c->pin >= 0) snprintf(n, sizeof n, "GPIO%d", c->pin);
    } else if (c->kind == DL_K_SYS) {
        dl_scpy(n, sizeof n, dl_sys_name(c->sys));
    } else if (c->kind == DL_K_GPIO) {
        if (c->key[0]) snprintf(n, sizeof n, _("Analógica %s"), dl_key_name(c->key));
    } else if (c->sensor[0] && c->key[0]) {
        aos_sensor_t s;
        const char *chip = dl_src_find(c->sensor, &s) ? s.chip : c->sensor;
        const char *us = strrchr(c->sensor, '_');
        if (c->kind == DL_K_OW && us) snprintf(n, sizeof n, "%s %s", dl_key_name(c->key), us + 1);
        else snprintf(n, sizeof n, "%s %s", dl_key_name(c->key), chip);
    }
    dl_scpy(c->name, sizeof c->name, n);
}

static void name_done(const char *t)
{
    while (*t == ' ') t++;
    if (*t) { dl_scpy(D.d.name, sizeof D.d.name, t); D.name_auto = false; }
    else { D.name_auto = true; auto_name(); }
    build();
}

static void name_cb(lv_event_t *e) { (void)e; dl_text_entry(_("Nombre"), D.d.name, 30, name_done); }

/* ---- numbers ---- */

static void num_done(float v)
{
    if (D.num_target) *D.num_target = v;
    D.num_target = NULL;
    build();
}

static void ask_num(const char *title, float *target, int dec)
{
    D.num_target = target;
    dl_num_entry(title, *target, dec, num_done);
}

static void every_done(float v)
{
    if (v < 1) v = 1;
    float s = roundf(v) * (float)D.unit;
    if (s > 7 * 86400.0f) s = 7 * 86400.0f;
    D.d.every_s = (uint32_t)s;
    build();
}

/* ---- callbacks of the choices ---- */

#define UD(e) ((int)(intptr_t)lv_event_get_user_data(e))

static void sched_cb(lv_event_t *e) { D.d.sched_at = UD(e) != 0; build(); }
static void unit_cb(lv_event_t *e)
{
    uint32_t u = (uint32_t)UD(e);
    uint32_t n = D.d.every_s / D.unit;
    D.unit = u;
    D.d.every_s = (n ? n : 1) * u;
    build();
}
static void preset_cb(lv_event_t *e)
{
    D.d.every_s = (uint32_t)UD(e);
    D.unit = D.d.every_s % 3600 == 0 ? 3600 : D.d.every_s % 60 == 0 ? 60 : 1;
    build();
}
static void every_cb(lv_event_t *e)
{
    (void)e;
    const char *t = D.unit == 3600 ? _("Cada cuántas horas") : D.unit == 60 ? _("Cada cuántos minutos") : _("Cada cuántos segundos");
    dl_num_entry(t, (float)(D.d.every_s / D.unit), 0, every_done);
}

static void time_add(int m)
{
    for (int i = 0; i < D.d.n_at; i++) if (D.d.at[i] == m) { build(); return; }
    if (D.d.n_at >= DL_MAX_AT) { aos_ui_toast(_("Hasta 8 horas por lectura"), 1500); return; }
    int i = D.d.n_at++;
    while (i > 0 && D.d.at[i - 1] > m) { D.d.at[i] = D.d.at[i - 1]; i--; }
    D.d.at[i] = (uint16_t)m;
    build();
}
static void time_add_cb(lv_event_t *e)
{
    (void)e;
    int m = D.d.n_at ? (D.d.at[D.d.n_at - 1] + 60) % 1440 : 8 * 60;
    dl_time_entry(_("A qué hora lee"), m, time_add);
}
static void time_del_cb(lv_event_t *e)
{
    int k = UD(e);
    if (k < 0 || k >= D.d.n_at) return;
    memmove(&D.d.at[k], &D.d.at[k + 1], sizeof D.d.at[0] * (size_t)(D.d.n_at - k - 1));
    D.d.n_at--;
    build();
}

static void kind_cb(lv_event_t *e)
{
    int k = UD(e);
    if (k == D.d.kind) return;
    D.d.kind = (uint8_t)k;
    D.d.sensor[0] = D.d.key[0] = 0;
    D.bus[0] = 0;
    if (k == DL_K_GPIO) D.d.analog = false;
    auto_name();
    build();
}
static void analog_cb(lv_event_t *e)
{
    D.d.analog = UD(e) != 0;
    D.d.sensor[0] = D.d.key[0] = 0;
    auto_name();
    build();
}
static void pin_cb(lv_event_t *e) { D.d.pin = (int8_t)UD(e); auto_name(); build(); }
static void pull_cb(lv_event_t *e) { D.d.pull = (uint8_t)UD(e); build(); }
static void invert_cb(lv_event_t *e) { D.d.invert = UD(e) != 0; build(); }
static void sys_cb(lv_event_t *e) { D.d.sys = (uint8_t)UD(e); auto_name(); build(); }
static void mode_cb(lv_event_t *e) { D.d.an_mode = (uint8_t)UD(e); build(); }

static aos_sensor_t s_sens[AOS_SENSOR_MAX];
static int s_nsens;

static void bus_cb(lv_event_t *e)
{
    int i = UD(e);
    if (i < 0 || i >= s_nsens) return;
    dl_scpy(D.bus, sizeof D.bus, s_sens[i].port);
    D.d.sensor[0] = D.d.key[0] = 0;
    auto_name();
    build();
}
static void sensor_cb(lv_event_t *e)
{
    int i = UD(e);
    if (i < 0 || i >= s_nsens) return;
    dl_scpy(D.d.sensor, sizeof D.d.sensor, s_sens[i].name);
    dl_scpy(D.bus, sizeof D.bus, s_sens[i].port);
    /* the only value, or the same key it had, stays chosen */
    bool keep = false;
    for (int k = 0; k < s_sens[i].n; k++) keep |= !strcmp(s_sens[i].v[k].key, D.d.key);
    if (!keep) {
        if (s_sens[i].n == 1 || D.d.analog) dl_scpy(D.d.key, sizeof D.d.key, s_sens[i].n ? s_sens[i].v[0].key : "");
        else D.d.key[0] = 0;
    }
    auto_name();
    build();
}
static void key_cb(lv_event_t *e)
{
    int v = UD(e), i = v / AOS_SENSOR_VALUES, k = v % AOS_SENSOR_VALUES;
    if (i < 0 || i >= s_nsens || k >= s_sens[i].n) return;
    dl_scpy(D.d.key, sizeof D.d.key, s_sens[i].v[k].key);
    auto_name();
    build();
}
static void gain_cb(lv_event_t *e)
{
    int g = UD(e);
    if (!D.d.sensor[0]) return;
    if (dl_ads_set_gain(D.d.sensor, g)) {
        char t[96];
        char v[16];
        dl_fmt_num((float)g / 1000.0f, 3, v, sizeof v);
        snprintf(t, sizeof t, _("Rango de ±%s V guardado en modules.txt"), v);
        aos_ui_toast(t, 1800);
    } else {
        aos_ui_toast(_("No se pudo cambiar modules.txt"), 1800);
    }
    build();
}
static void rescan_cb(lv_event_t *e) { (void)e; aos_sensors_rescan(); aos_ui_toast(_("Buscando sensores…"), 1200); }
static void modules_cb(lv_event_t *e) { (void)e; aos_ui_open("aos.modules"); }

static void lo_on_cb(lv_event_t *e) { (void)e; D.d.lo_on = !D.d.lo_on; build(); }
static void hi_on_cb(lv_event_t *e) { (void)e; D.d.hi_on = !D.d.hi_on; build(); }
static void lo_cb(lv_event_t *e) { (void)e; ask_num(_("Alarma baja: menos de"), &D.d.lo, dl_src_decimals(&D.d)); }
static void hi_cb(lv_event_t *e) { (void)e; ask_num(_("Alarma alta: más de"), &D.d.hi, dl_src_decimals(&D.d)); }
static void hyst_cb(lv_event_t *e) { (void)e; ask_num(_("Histéresis"), &D.d.hyst, dl_src_decimals(&D.d)); }
static void sound_cb(lv_event_t *e) { D.d.sound = UD(e) != 0; build(); }
static void out_cb(lv_event_t *e) { D.d.out_pin = (int8_t)UD(e); build(); }
static void level_cb(lv_event_t *e) { D.d.out_high = UD(e) != 0; build(); }

static void x1_cb(lv_event_t *e) { (void)e; ask_num(_("Tensión del punto 1 (V)"), &D.d.x1, 3); }
static void y1_cb(lv_event_t *e) { (void)e; ask_num(_("Valor del punto 1"), &D.d.y1, 3); }
static void x2_cb(lv_event_t *e) { (void)e; ask_num(_("Tensión del punto 2 (V)"), &D.d.x2, 3); }
static void y2_cb(lv_event_t *e) { (void)e; ask_num(_("Valor del punto 2"), &D.d.y2, 3); }
static void unit_done(const char *t) { dl_scpy(D.d.unit, sizeof D.d.unit, t); build(); }
static void unit_txt_cb(lv_event_t *e) { (void)e; dl_text_entry(_("Unidad"), D.d.unit, 10, unit_done); }

/* ---- saving ---- */

static void go_out(void)
{
    if (D.idx >= 0) dl_go(DL_V_DETAIL, D.idx);
    else dl_go(DL_V_LIST, -1);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    dl_ch_t *c = &D.d;
    if (!dl_src_ready(c)) { aos_ui_toast(_("Falta elegir qué lee"), 1600); return; }
    if (c->sched_at && !c->n_at) { aos_ui_toast(_("Agregá al menos una hora"), 1600); return; }
    if (!c->lo_on && !c->hi_on) c->out_pin = -1;
    if (c->lo_on && c->hi_on && c->lo >= c->hi) { aos_ui_toast(_("La alarma baja tiene que ser menor que la alta"), 2000); return; }
    bool digital = c->kind == DL_K_GPIO && !c->analog;
    if (digital && c->out_pin == c->pin) { aos_ui_toast(_("La salida de la alarma no puede ser el pin que lee"), 2000); return; }
    /* a pin is read by lines or driven by alarms, never both */
    char t[96] = "";
    dl_lock();
    dl_cfg_t *cfg = dl_cfg();
    for (int i = 0; i < cfg->n && !t[0]; i++) {
        if (i == D.idx) continue;
        const dl_ch_t *o = &cfg->ch[i];
        bool od = o->kind == DL_K_GPIO && !o->analog;
        if (c->out_pin >= 0 && od && o->pin == c->out_pin)
            snprintf(t, sizeof t, _("El GPIO%d lo lee «%s»"), c->out_pin, o->name);
        else if (digital && (o->lo_on || o->hi_on) && o->out_pin == c->pin)
            snprintf(t, sizeof t, _("El GPIO%d es la salida de una alarma de «%s»"), c->pin, o->name);
    }
    dl_unlock();
    if (t[0]) { aos_ui_toast(t, 2200); return; }
    if (!c->name[0]) { D.name_auto = true; auto_name(); }
    if (!c->name[0]) dl_scpy(c->name, sizeof c->name, _("Lectura"));
    if (!dl_ch_put(D.idx, c)) { aos_ui_toast(_("No se pudo guardar"), 1600); return; }
    dl_lock();
    bool run = dl_cfg()->running;
    dl_unlock();
    aos_ui_toast(run ? _("Guardado") : _("Guardado. Tocá ▶ para empezar a registrar."), run ? 1000 : 2400);
    go_out();
}

static void cancel_cb(lv_event_t *e) { (void)e; go_out(); }

static void delete_yes(void)
{
    dl_ch_delete(D.idx);
    aos_ui_toast(_("Lectura borrada; su CSV queda en la tarjeta"), 2000);
    dl_go(DL_V_LIST, -1);
}

static void delete_cb(lv_event_t *e)
{
    (void)e;
    dl_confirm(_("¿Borrar esta lectura? Deja de leer; el archivo con lo que ya anotó queda en la tarjeta."), _("Borrar"), delete_yes);
}

bool dl_edit_back(void)
{
    go_out();
    return true;
}

/* ---- the sections ---- */

static lv_obj_t *chip_x(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = dl_chip(parent, text, true, cb, ud);
    lv_obj_t *l = lv_obj_get_child(c, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_right(c, 60, 0);
    lv_obj_t *g = aos_label(c, AOS_SYM_CLOSE, &aos_sym_28, lv_color_hex(0x042F2E));
    lv_obj_align(g, LV_ALIGN_RIGHT_MID, 44, 0);
    aos_make_decorative(g);
    return c;
}

static void sec_schedule(lv_obj_t *col, int32_t w)
{
    dl_section(col, _("CADA CUÁNTO LEE"));
    lv_obj_t *r = dl_chips(col, w);
    dl_chip(r, _("Cada cierto tiempo"), !D.d.sched_at, sched_cb, (void *)0);
    dl_chip(r, _("A ciertas horas"), D.d.sched_at, sched_cb, (void *)1);
    if (!D.d.sched_at) {
        char v[32];
        uint32_t n = D.d.every_s / D.unit;
        snprintf(v, sizeof v, "%u %s", (unsigned)n, D.unit == 3600 ? "h" : D.unit == 60 ? "min" : "s");
        dl_row(col, w, _("Cada"), v, every_cb, NULL);
        lv_obj_t *u = dl_chips(col, w);
        dl_chip(u, _("segundos"), D.unit == 1, unit_cb, (void *)1);
        dl_chip(u, _("minutos"), D.unit == 60, unit_cb, (void *)60);
        dl_chip(u, _("horas"), D.unit == 3600, unit_cb, (void *)3600);
        static const struct { const char *t; int s; } P[] = {
            { "1 s", 1 }, { "10 s", 10 }, { "1 min", 60 }, { "5 min", 300 }, { "15 min", 900 }, { "1 h", 3600 },
        };
        lv_obj_t *p = dl_chips(col, w);
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
            dl_chip(p, P[i].t, D.d.every_s == (uint32_t)P[i].s, preset_cb, (void *)(intptr_t)P[i].s);
        dl_caption(col, _("Lee apenas empieza a registrar y después a horario fijo: cada 5 min es a las :00, :05, :10…"), w);
    } else {
        lv_obj_t *t = dl_chips(col, w);
        for (int i = 0; i < D.d.n_at; i++) {
            char s[8];
            snprintf(s, sizeof s, "%02u:%02u", D.d.at[i] / 60u, D.d.at[i] % 60u);
            chip_x(t, s, time_del_cb, (void *)(intptr_t)i);
        }
        if (D.d.n_at < DL_MAX_AT) dl_chip(t, _("+ Hora"), false, time_add_cb, NULL);
        dl_caption(col, _("Todos los días a esas horas. Necesita la hora de la red: sin ella no lee."), w);
    }
}

static void no_sensors(lv_obj_t *col, int32_t w, const char *text)
{
    lv_obj_t *cd = dl_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cd, 22, 0);
    lv_obj_set_flex_flow(cd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cd, 14, 0);
    dl_caption(cd, text, w - 44);
    lv_obj_t *r = dl_chips(cd, w - 44);
    dl_pill(r, AOS_SYM_MAGNIFY, _("Buscar de nuevo"), AOS_C_CARD2, rescan_cb, NULL);
    dl_pill(r, AOS_SYM_EXPANSION_CARD, _("Abrir Módulos"), AOS_C_CARD2, modules_cb, NULL);
}

static void sensor_label(const aos_sensor_t *s, char *out, size_t n)
{
    const aos_io_port_t *p = aos_io_port_find(s->port);
    if (p && p->kind == AOS_PORT_GPIO) {
        const char *us = strrchr(s->name, '_');
        snprintf(out, n, "%s %s", s->chip, us ? us + 1 : s->name);
    } else if (p && p->kind == AOS_PORT_SPI) snprintf(out, n, "%s CS%u", s->chip, (unsigned)s->addr);
    else snprintf(out, n, "%s 0x%02X", s->chip, s->addr);
    if (s->declared && strcasecmp(s->name, s->chip) && !(p && p->kind == AOS_PORT_GPIO)) {
        size_t o = strlen(out);
        snprintf(out + o, n - o, " (%s)", s->name);
    }
}

/* Bus, sensor and value of a kind the sensor service reads */
static void sec_service(lv_obj_t *col, int32_t w, bool analog)
{
    s_nsens = dl_src_sensors(D.d.kind, analog, s_sens, AOS_SENSOR_MAX);
    int sel = -1;
    for (int i = 0; i < s_nsens; i++) if (!strcmp(s_sens[i].name, D.d.sensor)) sel = i;
    if (!s_nsens && !D.d.sensor[0]) {
        const char *t = analog ? _("No hay ningún ADS1115. Conectalo a i2c.ext (SDA GPIO21 pin 15, SCL GPIO22 pin 17, 3V3 y GND): se reconoce solo en unos segundos.")
                      : D.d.kind == DL_K_I2C ? _("No hay sensores I2C. Los BME280, SHT3x/4x, AHT20, INA219/226 y ADS1115 en i2c.ext (SDA GPIO21, SCL GPIO22) se reconocen solos; un BH1750 se declara en Módulos.")
                      : D.d.kind == DL_K_SPI ? _("No hay sensores SPI. Un MAX6675 se declara en Módulos (Puertos, Agregar): el puerto spi.a y su CS.")
                      : _("No hay sensores 1-Wire. Un bus de DS18B20 se declara en Módulos (Puertos, Agregar, DS18B20): se elige el pin.");
        no_sensors(col, w, t);
        return;
    }
    /* the buses there are, once each */
    if (!D.bus[0]) dl_scpy(D.bus, sizeof D.bus, sel >= 0 ? s_sens[sel].port : s_nsens ? s_sens[0].port : "");
    if (!analog) {
        dl_section(col, D.d.kind == DL_K_OW ? _("BUS 1-WIRE") : _("BUS"));
        lv_obj_t *r = dl_chips(col, w);
        for (int i = 0; i < s_nsens; i++) {
            bool seen = false;
            for (int j = 0; j < i; j++) seen |= !strcmp(s_sens[j].port, s_sens[i].port);
            if (seen) continue;
            char t[40];
            const aos_io_port_t *p = aos_io_port_find(s_sens[i].port);
            if (p && p->kind == AOS_PORT_GPIO) snprintf(t, sizeof t, "%s · GPIO%d", s_sens[i].port, p->pins[0]);
            else dl_scpy(t, sizeof t, s_sens[i].port);
            dl_chip(r, t, !strcmp(D.bus, s_sens[i].port), bus_cb, (void *)(intptr_t)i);
        }
    }
    dl_section(col, analog ? "ADS1115" : _("SENSOR"));
    lv_obj_t *r = dl_chips(col, w);
    int on_bus = 0, only = -1;
    for (int i = 0; i < s_nsens; i++) {
        if (!analog && strcmp(D.bus, s_sens[i].port)) continue;
        char t[48];
        sensor_label(&s_sens[i], t, sizeof t);
        dl_chip(r, t, i == sel, sensor_cb, (void *)(intptr_t)i);
        on_bus++;
        only = i;
    }
    if (sel < 0 && D.d.sensor[0]) {
        dl_chip(r, D.d.sensor, true, NULL, NULL);
        dl_caption(col, _("Ese sensor no está ahora: la lectura anota el error hasta que vuelva."), w);
    }
    if (sel < 0 && !D.d.sensor[0] && on_bus == 1) {
        /* one sensor on the bus: chosen already */
        dl_scpy(D.d.sensor, sizeof D.d.sensor, s_sens[only].name);
        if (s_sens[only].n == 1 || analog) dl_scpy(D.d.key, sizeof D.d.key, s_sens[only].n ? s_sens[only].v[0].key : "");
        sel = only;
        auto_name();
        lv_obj_clean(r);
        char t[48];
        sensor_label(&s_sens[only], t, sizeof t);
        dl_chip(r, t, true, sensor_cb, (void *)(intptr_t)only);
    }
    if (sel < 0) return;
    const aos_sensor_t *s = &s_sens[sel];
    dl_section(col, analog ? _("ENTRADA") : _("VARIABLE"));
    lv_obj_t *k = dl_chips(col, w);
    for (int i = 0; i < s->n && i < AOS_SENSOR_VALUES; i++) {
        char t[64], v[32];
        dl_ch_t tmp = D.d;
        dl_scpy(tmp.key, sizeof tmp.key, s->v[i].key);
        tmp.an_mode = DL_AN_VOLT;
        dl_fmt_value(&tmp, s->state == AOS_SENSOR_OK ? s->v[i].v : NAN, true, v, sizeof v);
        snprintf(t, sizeof t, "%s · %s", dl_key_name(s->v[i].key), v);
        dl_chip(k, t, !strcmp(D.d.key, s->v[i].key), key_cb, (void *)(intptr_t)(sel * AOS_SENSOR_VALUES + i));
    }
    if (s->state == AOS_SENSOR_ERR && s->err[0]) {
        char t[96];
        dl_err_text(s->err, t, sizeof t);
        lv_obj_t *l = dl_caption(col, t, w);
        lv_obj_set_style_text_color(l, AOS_C_ORANGE, 0);
    }
}

static void sec_analog(lv_obj_t *col, int32_t w)
{
    sec_service(col, w, true);
    if (!D.d.sensor[0]) return;
    int g = dl_ads_gain(D.d.sensor);
    dl_section(col, _("RANGO"));
    lv_obj_t *r = dl_chips(col, w);
    for (size_t i = 0; i < sizeof GAINS / sizeof GAINS[0]; i++) {
        char t[24], v[16];
        dl_fmt_num((float)GAINS[i] / 1000.0f, 3, v, sizeof v);
        snprintf(t, sizeof t, "±%s V", v);
        dl_chip(r, t, g == GAINS[i], gain_cb, (void *)(intptr_t)GAINS[i]);
    }
    dl_caption(col, _("Es el gain= del ADS1115 en modules.txt y vale para sus cuatro entradas; se guarda al elegirlo. Nunca más de 3,3 V en una entrada, sea cual sea el rango."), w);
    dl_section(col, _("QUÉ ANOTA"));
    lv_obj_t *m = dl_chips(col, w);
    dl_chip(m, _("Tensión (V)"), D.d.an_mode == DL_AN_VOLT, mode_cb, (void *)DL_AN_VOLT);
    dl_chip(m, _("Crudo (cuentas)"), D.d.an_mode == DL_AN_RAW, mode_cb, (void *)DL_AN_RAW);
    dl_chip(m, _("Estimado (escala)"), D.d.an_mode == DL_AN_SCALE, mode_cb, (void *)DL_AN_SCALE);
    if (D.d.an_mode == DL_AN_RAW) {
        char t[160], v[16];
        dl_fmt_num((float)g / 1000.0f, 3, v, sizeof v);
        snprintf(t, sizeof t, _("Las cuentas del convertidor, de -32768 a 32767: 32767 son %s V con este rango."), v);
        dl_caption(col, t, w);
    } else if (D.d.an_mode == DL_AN_SCALE) {
        char a[24], b[24];
        dl_fmt_num(D.d.x1, 3, a, sizeof a); strcat(a, " V");
        dl_row(col, w, _("Punto 1: tensión"), a, x1_cb, NULL);
        dl_fmt_num(D.d.y1, 3, b, sizeof b);
        dl_row(col, w, _("Punto 1: valor"), b, y1_cb, NULL);
        dl_fmt_num(D.d.x2, 3, a, sizeof a); strcat(a, " V");
        dl_row(col, w, _("Punto 2: tensión"), a, x2_cb, NULL);
        dl_fmt_num(D.d.y2, 3, b, sizeof b);
        dl_row(col, w, _("Punto 2: valor"), b, y2_cb, NULL);
        dl_row(col, w, _("Unidad"), D.d.unit[0] ? D.d.unit : _("ninguna"), unit_txt_cb, NULL);
        dl_caption(col, _("Una recta por los dos puntos: un sensor de presión que da 0,5 V a 0 bar y 4,5 V a 10 bar es 0,5 → 0 y 4,5 → 10, en bar."), w);
    }
}

static void sec_digital(lv_obj_t *col, int32_t w)
{
    dl_section(col, _("PIN"));
    int8_t pins[40];
    int n = dl_pins(pins, 40);
    lv_obj_t *r = dl_chips(col, w);
    bool shown = false;
    for (int i = 0; i < n; i++) {
        char t[32];
        dl_pin_label(pins[i], t, sizeof t);
        dl_chip(r, t, pins[i] == D.d.pin, pin_cb, (void *)(intptr_t)pins[i]);
        shown |= pins[i] == D.d.pin;
    }
    if (D.d.pin >= 0 && !shown) {
        char t[12];
        snprintf(t, sizeof t, "GPIO%d", D.d.pin);
        dl_chip(r, t, true, NULL, NULL);
    }
    if (D.d.pin >= 0) {
        const aos_io_pin_t *p = aos_io_pin_of_gpio(D.d.pin);
        char t[160];
        const char *o = dl_pin_owner(D.d.pin);
        if (o) snprintf(t, sizeof t, _("Es el pin %d del header; ahora lo tiene %s."), p ? p->pin : 0, o);
        else snprintf(t, sizeof t, _("Es el pin %d del header. Sólo 3,3 V: nunca 5 V en un GPIO."), p ? p->pin : 0);
        dl_caption(col, t, w);
    } else {
        dl_caption(col, _("Los pines que no son de un bus de sensores (i2c.ext, spi.a, un 1-Wire) ni de otra app; los de un puerto serie o un módulo, al final."), w);
    }
    dl_section(col, _("RESISTENCIA"));
    lv_obj_t *p = dl_chips(col, w);
    dl_chip(p, "Pull-up", D.d.pull == DL_PULL_UP, pull_cb, (void *)DL_PULL_UP);
    dl_chip(p, "Pull-down", D.d.pull == DL_PULL_DOWN, pull_cb, (void *)DL_PULL_DOWN);
    dl_chip(p, _("Ninguna"), D.d.pull == DL_PULL_NONE, pull_cb, (void *)DL_PULL_NONE);
    dl_caption(col, D.d.pull == DL_PULL_UP ? _("Con pull-up un contacto o botón a GND lee 0 cuando se cierra.")
                    : D.d.pull == DL_PULL_DOWN ? _("Con pull-down un contacto a 3V3 lee 1 cuando se cierra.")
                    : _("Sin resistencia: para una salida que siempre maneja el nivel (un módulo con su propio pull-up)."), w);
    dl_section(col, _("LÓGICA"));
    lv_obj_t *l = dl_chips(col, w);
    dl_chip(l, _("Normal: alto es 1"), !D.d.invert, invert_cb, (void *)0);
    dl_chip(l, _("Invertida: alto es 0"), D.d.invert, invert_cb, (void *)1);
}

static void sec_sys(lv_obj_t *col, int32_t w)
{
    dl_section(col, _("QUÉ VALOR"));
    lv_obj_t *r = dl_chips(col, w);
    for (int i = 0; i < DL_SYS_COUNT; i++) dl_chip(r, dl_sys_name(i), D.d.sys == i, sys_cb, (void *)(intptr_t)i);
    dl_caption(col, _("Valores de la placa misma, para seguir la red o la memoria a lo largo de un día."), w);
}

static void sec_alarms(lv_obj_t *col, int32_t w)
{
    dl_section(col, _("ALARMAS"));
    lv_obj_t *r = dl_chips(col, w);
    dl_chip(r, _("Baja"), D.d.lo_on, lo_on_cb, NULL);
    dl_chip(r, _("Alta"), D.d.hi_on, hi_on_cb, NULL);
    if (!D.d.lo_on && !D.d.hi_on) {
        dl_caption(col, _("Una alarma avisa cuando el valor pasa un límite: con sonido y un mensaje, o sólo con una nota en el CSV; y puede manejar un GPIO (un relé, una luz)."), w);
        return;
    }
    char v[32];
    if (D.d.lo_on) { dl_fmt_value(&D.d, D.d.lo, true, v, sizeof v); dl_row(col, w, _("Baja: menos de"), v, lo_cb, NULL); }
    if (D.d.hi_on) { dl_fmt_value(&D.d, D.d.hi, true, v, sizeof v); dl_row(col, w, _("Alta: más de"), v, hi_cb, NULL); }
    dl_fmt_value(&D.d, D.d.hyst, true, v, sizeof v);
    dl_row(col, w, _("Histéresis"), v, hyst_cb, NULL);
    dl_caption(col, _("Para que no se prenda y se apague cerca del límite: una alarma alta termina cuando el valor baja de alta menos la histéresis (y una baja, al revés)."), w);
    dl_section(col, _("AVISO"));
    lv_obj_t *a = dl_chips(col, w);
    dl_chip(a, _("Sonido y mensaje en pantalla"), D.d.sound, sound_cb, (void *)1);
    dl_chip(a, _("Sólo una nota en el CSV"), !D.d.sound, sound_cb, (void *)0);
    dl_section(col, _("SALIDA"));
    int8_t pins[40];
    int n = dl_pins(pins, 40);
    lv_obj_t *o = dl_chips(col, w);
    dl_chip(o, _("Ninguna"), D.d.out_pin < 0, out_cb, (void *)-1);
    bool shown = D.d.out_pin < 0;
    for (int i = 0; i < n; i++) {
        if (D.d.kind == DL_K_GPIO && !D.d.analog && pins[i] == D.d.pin) continue;
        char t[32];
        dl_pin_label(pins[i], t, sizeof t);
        dl_chip(o, t, pins[i] == D.d.out_pin, out_cb, (void *)(intptr_t)pins[i]);
        shown |= pins[i] == D.d.out_pin;
    }
    if (!shown) {
        char t[12];
        snprintf(t, sizeof t, "GPIO%d", D.d.out_pin);
        dl_chip(o, t, true, NULL, NULL);
    }
    if (D.d.out_pin >= 0) {
        dl_section(col, _("NIVEL CON ALARMA"));
        lv_obj_t *lv = dl_chips(col, w);
        dl_chip(lv, _("Alto (3V3)"), D.d.out_high, level_cb, (void *)1);
        dl_chip(lv, _("Bajo (GND)"), !D.d.out_high, level_cb, (void *)0);
        const aos_io_pin_t *p = aos_io_pin_of_gpio(D.d.out_pin);
        char t[220];
        snprintf(t, sizeof t, _("El GPIO%d (pin %d) queda en ese nivel mientras dure la alarma, y en el otro el resto del tiempo y en pausa. Para un módulo de relé, el nivel con el que se activa (muchos, con GND)."),
                 D.d.out_pin, p ? p->pin : 0);
        dl_caption(col, t, w);
    }
}

static void preview_refresh(void)
{
    if (!D.preview) return;
    char t[96], v[32];
    if (!dl_src_ready(&D.d)) dl_scpy(t, sizeof t, _("Elegí qué lee"));
    else if (D.d.kind == DL_K_GPIO && !D.d.analog) dl_scpy(t, sizeof t, _("El pin se lee una vez guardada la lectura"));
    else {
        float x;
        char err[64];
        if (dl_src_read(&D.d, &x, err, sizeof err)) { dl_fmt_value(&D.d, x, true, v, sizeof v); snprintf(t, sizeof t, _("Ahora: %s"), v); }
        else dl_scpy(t, sizeof t, err);
    }
    lv_label_set_text(D.preview, t);
}

static void sensors_sig(char *out, size_t n)
{
    size_t o = 0;
    out[0] = 0;
    int cnt = aos_sensor_count();
    for (int i = 0; i < cnt && o < n; i++) {
        aos_sensor_t s;
        if (aos_sensor_at(i, &s)) o += (size_t)snprintf(out + o, n - o, "%s/%d/%d;", s.name, s.n, s.state);
    }
}

static void build(void)
{
    if (!D.col) return;
    int32_t y = lv_obj_get_scroll_y(D.col);
    dl_sheet_close();
    lv_obj_clean(D.col);
    D.preview = NULL;
    int32_t w = col_w();
    sensors_sig(D.sig, sizeof D.sig);

    dl_row(D.col, w, _("Nombre"), D.d.name[0] ? D.d.name : _("(se pone solo)"), name_cb, NULL);
    sec_schedule(D.col, w);

    dl_section(D.col, _("TIPO DE SENSOR"));
    lv_obj_t *k = dl_chips(D.col, w);
    for (int i = 0; i < DL_K_COUNT; i++) dl_chip(k, dl_kind_name(i), D.d.kind == i, kind_cb, (void *)(intptr_t)i);
    if (D.d.kind == DL_K_GPIO) {
        lv_obj_t *a = dl_chips(D.col, w);
        dl_chip(a, _("Digital"), !D.d.analog, analog_cb, (void *)0);
        dl_chip(a, _("Analógica (ADS1115)"), D.d.analog, analog_cb, (void *)1);
        if (D.d.analog) sec_analog(D.col, w);
        else sec_digital(D.col, w);
    } else if (D.d.kind == DL_K_SYS) {
        sec_sys(D.col, w);
    } else {
        sec_service(D.col, w, false);
    }

    lv_obj_t *pv = dl_card(D.col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(pv, 22, 0);
    D.preview = aos_label(pv, "", aos_font_body, DL_C_MAIN);
    lv_obj_set_width(D.preview, w - 44);
    lv_label_set_long_mode(D.preview, LV_LABEL_LONG_MODE_WRAP);
    preview_refresh();

    sec_alarms(D.col, w);

    if (D.idx >= 0) {
        lv_obj_t *b = dl_box(D.col, w, 96);
        lv_obj_t *del = dl_pill(b, AOS_SYM_DELETE, _("Borrar esta lectura"), lv_color_hex(0x5C1A16), delete_cb, NULL);
        lv_obj_align(del, LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_obj_update_layout(D.col);
    lv_obj_scroll_to_y(D.col, y, LV_ANIM_OFF);
}

void dl_edit_tick(void)
{
    aos_sensors_keep();
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if ((int32_t)(now - D.prev_ms) < 1000) return;
    D.prev_ms = now;
    preview_refresh();
    char sig[sizeof D.sig];
    sensors_sig(sig, sizeof sig);
    /* a sensor came or went (a search, a module added in Módulos) */
    if (strcmp(sig, D.sig) && !U.overlay) build();
}

void dl_edit_open(int ch)
{
    if (ch != -2) {
        memset(&D, 0, sizeof D);
        D.idx = ch;
        if (ch >= 0) {
            dl_lock();
            if (ch < dl_cfg()->n) D.d = dl_cfg()->ch[ch];
            else D.idx = -1;
            dl_unlock();
        }
        if (D.idx < 0) {
            dl_cfg_defaults(&D.d);
            D.name_auto = true;
        }
        D.unit = D.d.every_s % 3600 == 0 ? 3600 : D.d.every_s % 60 == 0 ? 60 : 1;
        aos_sensors_keep();
    }
    dl_bar_clean();
    lv_obj_t *c = dl_pill(U.bar, NULL, _("Cancelar"), AOS_C_CARD2, cancel_cb, NULL);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *s = dl_pill(U.bar, AOS_SYM_CHECK, _("Guardar"), DL_C_DARK, save_cb, NULL);
    lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *t = aos_label(U.bar, D.idx >= 0 ? _("Editar") : _("Nueva lectura"), aos_font_body, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
    int32_t w = col_w(), h = U.H - U.bar_h;
    D.col = dl_column(U.content, w, h);
    lv_obj_set_style_pad_row(D.col, 14, 0);
    lv_obj_align(D.col, LV_ALIGN_TOP_MID, 0, 0);
    build();
}
