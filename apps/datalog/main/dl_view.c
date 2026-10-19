/*
 * P4OS - Datalog: one line.
 *
 * Its latest value, a chart of its CSV over a range (an hour to all of
 * it), the minimum, maximum and mean of that range, its alarms and its
 * file. Holding a finger on the chart reads the point under it.
 *
 * The series is made by a thread of its own (dl_csv_series reads the card)
 * and handed over through the tick; it is made again when a new reading
 * comes, no more than every 10 s for the short ranges and every minute for
 * the long ones.
 */
#include "dl.h"

#include <stddef.h>

static const struct { const char *name; int32_t s; } RANGES[] = {
    { "1 h", 3600 }, { "6 h", 6 * 3600 }, { "24 h", 86400 }, { N_("7 días"), 7 * 86400 },
    { N_("30 días"), 30 * 86400 }, { N_("Todo"), 0 },
};
#define NRANGES ((int)(sizeof RANGES / sizeof RANGES[0]))

static int s_range = 2;

static struct {
    int ch;
    char id[24];
    lv_obj_t *chart, *val, *when, *x0, *x1, *readout, *stats, *loading, *chips;
    lv_chart_series_t *ser;
    lv_obj_t *g_hi, *g_lo;
    dl_series_t s;
    float scale;
    uint32_t loaded_ms, seen_reads, readout_ms;
    /* the loader */
    volatile bool busy, done, again;
    dl_series_t ld;
    char ld_id[24];
    int64_t ld_span, ld_now;
} V;

static void readout_default(void);

static void load_fn(void *arg)
{
    (void)arg;
    dl_csv_series(V.ld_id, V.ld_span, V.ld_now, &V.ld);
    V.done = true;
    V.busy = false;
}

static void load(void)
{
    if (V.busy) { V.again = true; return; }
    bool valid;
    int64_t now = dl_now(&valid);
    dl_scpy(V.ld_id, sizeof V.ld_id, V.id);
    V.ld_span = RANGES[s_range].s;
    V.ld_now = valid ? now : 0;
    V.done = false;
    V.busy = true;
    if (!aos_hal_thread_start("dl_chart", load_fn, NULL, 6144, 2)) V.busy = false;
    if (V.loading) lv_obj_remove_flag(V.loading, LV_OBJ_FLAG_HIDDEN);
}

void dl_view_leave(void)
{
    /* the loader runs this .so's code: it must be out before the app goes */
    for (int i = 0; i < 250 && V.busy; i++) aos_hal_sleep_ms(20);
    V.chart = NULL;
    V.again = false;
}

static void fmt_t(int64_t t, bool day, char *out, size_t n)
{
    int y, mo, d, hh, mi, ss;
    dl_uncivil(t, &y, &mo, &d, &hh, &mi, &ss);
    if (day) snprintf(out, n, "%02d/%02d %02d:%02d", d, mo, hh, mi);
    else snprintf(out, n, "%02d:%02d", hh, mi);
}

static void get_ch(dl_ch_t *c)
{
    dl_lock();
    if (V.ch >= 0 && V.ch < dl_cfg()->n) *c = dl_cfg()->ch[V.ch];
    else dl_cfg_defaults(c);
    dl_unlock();
}

static void apply(void)
{
    if (!V.chart) return;
    dl_ch_t c;
    get_ch(&c);
    const dl_series_t *s = &V.s;
    if (V.loading) lv_obj_add_flag(V.loading, LV_OBJ_FLAG_HIDDEN);
    float lo = s->lo, hi = s->hi;
    if (!s->ok || !s->cnt) {
        lv_chart_set_all_values(V.chart, V.ser, LV_CHART_POINT_NONE);
        lv_obj_add_flag(V.g_hi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(V.g_lo, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(V.stats, s->ok ? _("No hay lecturas en este rango.") : _("Todavía no hay lecturas en el archivo."));
        lv_label_set_text(V.x0, "");
        lv_label_set_text(V.x1, "");
        lv_chart_refresh(V.chart);
        return;
    }
    /* the alarm lines, when they are not far from the readings */
    float span = hi - lo;
    if (c.hi_on && c.hi <= hi + span * 2 + 1e-3f && c.hi >= lo - span) { if (c.hi > hi) hi = c.hi; if (c.hi < lo) lo = c.hi; }
    if (c.lo_on && c.lo >= lo - span * 2 - 1e-3f && c.lo <= hi + span) { if (c.lo < lo) lo = c.lo; if (c.lo > hi) hi = c.lo; }
    span = hi - lo;
    float m = (hi + lo) / 2, min = fabsf(m) * 0.02f + 0.01f;
    if (span < min) { lo = m - min / 2; hi = m + min / 2; }
    else { lo -= span * 0.08f; hi += span * 0.08f; }
    V.scale = fabsf(lo) > 1e6f || fabsf(hi) > 1e6f ? 1.0f : 1000.0f;
    int32_t *y = lv_chart_get_series_y_array(V.chart, V.ser);
    int pts = 0;
    for (int i = 0; i < DL_CHART_N; i++) {
        y[i] = isnan(s->v[i]) ? LV_CHART_POINT_NONE : (int32_t)roundf(s->v[i] * V.scale);
        pts += !isnan(s->v[i]);
    }
    /* the chart draws a line only between neighbours: join the points that
     * are apart only because the slices are shorter than the interval, and
     * leave real holes (the logger stopped) as holes */
    float every = c.sched_at ? 86400.0f / (float)(c.n_at ? c.n_at : 1) : (float)(c.every_s ? c.every_s : 1);
    float slice = (float)(int32_t)(s->t1 - s->t0) / (float)DL_CHART_N;
    int maxgap = slice > 0 ? (int)ceilf(every * 2.5f / slice) : 1;
    for (int i = 0, prev = -1; i < DL_CHART_N; i++) {
        if (y[i] == LV_CHART_POINT_NONE) continue;
        if (prev >= 0 && i - prev > 1 && i - prev <= maxgap)
            for (int k = prev + 1; k < i; k++) y[k] = y[prev] + (int32_t)((int64_t)(y[i] - y[prev]) * (k - prev) / (i - prev));
        prev = i;
    }
    /* a few readings are dots, many a line */
    lv_obj_set_style_size(V.chart, pts < 40 ? 10 : 0, pts < 40 ? 10 : 0, LV_PART_INDICATOR);
    lv_chart_set_axis_range(V.chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)floorf(lo * V.scale), (int32_t)ceilf(hi * V.scale));
    lv_chart_refresh(V.chart);
    /* the alarm limits, as guides across */
    lv_obj_update_layout(V.chart);
    int32_t ch = lv_obj_get_content_height(V.chart);
    for (int k = 0; k < 2; k++) {
        lv_obj_t *g = k ? V.g_lo : V.g_hi;
        bool on = k ? c.lo_on : c.hi_on;
        float v = k ? c.lo : c.hi;
        if (!on || v < lo || v > hi) { lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(g, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(g, (int32_t)((hi - v) / (hi - lo) * (float)ch) - 1);
    }

    bool day = s->t1 - s->t0 > 86400;
    char a[32], b[32];
    fmt_t(s->t0, day, a, sizeof a);
    lv_label_set_text(V.x0, a);
    fmt_t(s->t1, day, b, sizeof b);
    lv_label_set_text(V.x1, b);

    char mn[32], mx[32], av[32], t[200];
    dl_fmt_value(&c, s->lo, true, mn, sizeof mn);
    dl_fmt_value(&c, s->hi, true, mx, sizeof mx);
    dl_fmt_value(&c, s->sum / (float)s->cnt, true, av, sizeof av);
    if (s->sampled) snprintf(t, sizeof t, _("mín %s · máx %s · promedio %s\nde %d lecturas tomadas a lo largo del archivo"), mn, mx, av, s->cnt);
    else snprintf(t, sizeof t, _("mín %s · máx %s · promedio %s\n%d lecturas en el rango"), mn, mx, av, s->cnt);
    lv_label_set_text(V.stats, t);
}

static void head_refresh(void)
{
    if (!V.val) return;
    dl_ch_t c;
    get_ch(&c);
    static dl_rt_t r;
    dl_lock();
    if (V.ch >= 0 && V.ch < dl_cfg()->n) r = *dl_rt(V.ch);
    dl_unlock();
    char t[160];
    dl_fmt_value(&c, r.have ? r.v : NAN, true, t, sizeof t);
    lv_label_set_text(V.val, t);
    lv_obj_set_style_text_color(V.val, r.alarm == DL_AL_HIGH ? DL_C_HIGH : r.alarm == DL_AL_LOW ? DL_C_LOW : AOS_C_TEXT, 0);
    if (!r.ok && r.err[0]) snprintf(t, sizeof t, "%s · %s", r.when, r.err);
    else if (r.when[0]) snprintf(t, sizeof t, _("la última: %s"), r.when);
    else dl_scpy(t, sizeof t, _("todavía sin leer"));
    lv_label_set_text(V.when, t);
    lv_obj_set_style_text_color(V.when, !r.ok && r.err[0] ? AOS_C_ORANGE : AOS_C_DIM, 0);
}

void dl_view_refresh(void)
{
    head_refresh();
    dl_lock();
    uint32_t reads = V.ch >= 0 && V.ch < dl_cfg()->n ? dl_rt(V.ch)->reads + dl_rt(V.ch)->errors : 0;
    dl_unlock();
    if (reads != V.seen_reads) {
        V.seen_reads = reads;
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        uint32_t every = RANGES[s_range].s && RANGES[s_range].s <= 86400 ? 10000 : 60000;
        if ((int32_t)(now - V.loaded_ms) >= (int32_t)every) load();
    }
}

void dl_view_tick(void)
{
    if (V.readout_ms && (int32_t)((uint32_t)aos_hal_uptime_ms() - V.readout_ms) >= 0) {
        V.readout_ms = 0;
        readout_default();
    }
    if (!V.done) return;
    V.done = false;
    /* a series of the line seen before this one */
    if (strcmp(V.ld_id, V.id)) { load(); return; }
    V.s = V.ld;
    V.loaded_ms = (uint32_t)aos_hal_uptime_ms();
    apply();
    if (V.again) { V.again = false; load(); }
}

/* ---- interaction ---- */

static void readout_default(void)
{
    if (V.readout) lv_label_set_text(V.readout, _("mantené apretado para leer"));
}

static void chart_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        /* the reading stays a moment, to be read with the finger off */
        V.readout_ms = (uint32_t)aos_hal_uptime_ms() + 4000;
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED) return;
    uint32_t id = lv_chart_get_pressed_point(V.chart);
    if (id == LV_CHART_POINT_NONE || id >= DL_CHART_N || !V.s.ok) return;
    dl_ch_t c;
    get_ch(&c);
    int64_t span = V.s.t1 - V.s.t0;
    int64_t t = V.s.t0 + (span * (2 * (int64_t)id + 1)) / (2 * DL_CHART_N);
    char w[32], v[32], out[72];
    fmt_t(t, span > 86400, w, sizeof w);
    dl_fmt_value(&c, V.s.v[id], true, v, sizeof v);
    snprintf(out, sizeof out, "%s · %s", w, isnan(V.s.v[id]) ? _("sin lecturas") : v);
    lv_label_set_text(V.readout, out);
}

static void chips_build(void);

static void range_cb(lv_event_t *e)
{
    s_range = (int)(intptr_t)lv_event_get_user_data(e);
    chips_build();
    load();
}

static void chips_build(void)
{
    if (!V.chips) return;
    lv_obj_clean(V.chips);
    for (int i = 0; i < NRANGES; i++) dl_chip(V.chips, dl_tr(RANGES[i].name), i == s_range, range_cb, (void *)(intptr_t)i);
}

static void back_cb(lv_event_t *e) { (void)e; dl_go(DL_V_LIST, -1); }
static void edit_cb(lv_event_t *e) { (void)e; dl_go(DL_V_EDIT, V.ch); }

static void clear_yes(void)
{
    dl_lock();
    bool ok = dl_csv_clear(V.id);
    if (V.ch >= 0 && V.ch < dl_cfg()->n) {
        dl_rt_t *r = dl_rt(V.ch);
        r->hn = r->hhead = 0;
        r->reads = r->errors = 0;
    }
    dl_unlock();
    aos_ui_toast(ok ? _("Datos borrados") : _("No había datos"), 1400);
    dl_go(DL_V_DETAIL, V.ch);
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    char t[160];
    snprintf(t, sizeof t, _("¿Borrar %s.csv con todas sus lecturas? La línea sigue y empieza un archivo nuevo."), V.id);
    dl_confirm(t, _("Borrar"), clear_yes);
}

/* ---- the screen ---- */

static void alarm_card(lv_obj_t *col, const dl_ch_t *c, int32_t w)
{
    dl_section(col, _("ALARMAS"));
    lv_obj_t *cd = dl_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cd, 22, 0);
    lv_obj_set_flex_flow(cd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cd, 8, 0);
    char t[240], a[32];
    if (!c->lo_on && !c->hi_on) {
        dl_caption(cd, _("Sin alarmas. Se agregan desde Editar."), w - 44);
        return;
    }
    if (c->hi_on) {
        dl_fmt_value(c, c->hi, true, a, sizeof a);
        snprintf(t, sizeof t, _("Alta: más de %s"), a);
        aos_label(cd, t, aos_font_small, DL_C_HIGH);
    }
    if (c->lo_on) {
        dl_fmt_value(c, c->lo, true, a, sizeof a);
        snprintf(t, sizeof t, _("Baja: menos de %s"), a);
        aos_label(cd, t, aos_font_small, DL_C_LOW);
    }
    size_t o = 0;
    if (c->hyst > 0) {
        dl_fmt_value(c, c->hyst, true, a, sizeof a);
        o += (size_t)snprintf(t + o, sizeof t - o, _("Histéresis %s. "), a);
    }
    o += (size_t)snprintf(t + o, sizeof t - o, "%s", c->sound ? _("Avisa con sonido y en pantalla, y lo anota en el CSV.") : _("Sólo lo anota en el CSV."));
    if (c->out_pin >= 0 && o < sizeof t)
        snprintf(t + o, sizeof t - o, _(" El GPIO%d va a %s mientras dure."), c->out_pin, c->out_high ? "3V3" : "GND");
    dl_caption(cd, t, w - 44);
}

static void file_card(lv_obj_t *col, int32_t w)
{
    dl_section(col, _("ARCHIVO"));
    lv_obj_t *cd = dl_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cd, 22, 0);
    lv_obj_set_flex_flow(cd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cd, 14, 0);
    long sz = dl_csv_size(V.id);
    char t[160], s[32];
    if (sz < 0) dl_scpy(s, sizeof s, _("todavía no existe"));
    else if (sz < 1024) snprintf(s, sizeof s, "%ld B", sz);
    else if (sz < 1024 * 1024) snprintf(s, sizeof s, "%ld KB", sz / 1024);
    else { snprintf(s, sizeof s, "%.1f MB", (double)sz / (1024.0 * 1024.0)); for (char *p = s; *p; p++) if (*p == '.') *p = ','; }
    snprintf(t, sizeof t, "datalog/%s.csv · %s", V.id, s);
    aos_label(cd, t, aos_font_small, AOS_C_TEXT);
    dl_caption(cd, _("Se baja desde el portal (Datalog) o se abre en Archivos, que también lo grafica."), w - 44);
    if (sz > 0) dl_pill(cd, AOS_SYM_DELETE, _("Borrar los datos"), AOS_C_CARD2, clear_cb, NULL);
}

static lv_obj_t *chart_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *cd = dl_card(parent, w, h);
    lv_obj_set_style_pad_all(cd, 20, 0);
    int32_t iw = w - 40;
    V.val = aos_label(cd, "--", aos_font_large, AOS_C_TEXT);
    lv_obj_set_pos(V.val, 0, 0);
    V.when = aos_label(cd, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(V.when, iw);
    lv_label_set_long_mode(V.when, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(V.when, 0, 60);
    V.readout = aos_label(cd, "", aos_font_caption, DL_C_MAIN);
    lv_obj_align(V.readout, LV_ALIGN_TOP_RIGHT, 0, 14);
    V.chart = lv_chart_create(cd);
    lv_obj_set_size(V.chart, iw, h - 40 - 96 - 34);
    lv_obj_set_pos(V.chart, 0, 96);
    lv_obj_set_style_bg_opa(V.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(V.chart, 0, 0);
    lv_obj_set_style_pad_all(V.chart, 4, 0);
    lv_obj_set_style_line_color(V.chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_type(V.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(V.chart, DL_CHART_N);
    lv_chart_set_div_line_count(V.chart, 4, 6);
    lv_obj_set_style_size(V.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(V.chart, 4, LV_PART_ITEMS);
    V.ser = lv_chart_add_series(V.chart, DL_C_MAIN, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(V.chart, V.ser, LV_CHART_POINT_NONE);
    for (int k = 0; k < 2; k++) {
        lv_obj_t *g = dl_box(V.chart, LV_PCT(100), 3);
        lv_obj_set_style_bg_color(g, k ? DL_C_LOW : DL_C_HIGH, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_60, 0);
        lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
        aos_make_decorative(g);
        if (k) V.g_lo = g; else V.g_hi = g;
    }
    lv_obj_add_event_cb(V.chart, chart_cb, LV_EVENT_ALL, NULL);
    lv_obj_remove_flag(V.chart, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    V.loading = aos_label(cd, _("Cargando…"), aos_font_caption, AOS_C_DIM);
    lv_obj_align(V.loading, LV_ALIGN_CENTER, 0, 40);
    V.x0 = aos_label(cd, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(V.x0, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    V.x1 = aos_label(cd, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(V.x1, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    readout_default();
    return cd;
}

void dl_view_build(int ch)
{
    dl_bar_clean();
    bool keep = V.busy;
    memset(&V, 0, offsetof(typeof(V), busy));
    V.busy = keep;
    V.done = V.again = false;
    V.ch = ch;
    dl_ch_t c;
    get_ch(&c);
    dl_scpy(V.id, sizeof V.id, c.id);
    /* the range that shows some thirty readings, at least */
    float every = c.sched_at ? 86400.0f / (float)(c.n_at ? c.n_at : 1) : (float)(c.every_s ? c.every_s : 1);
    s_range = NRANGES - 1;
    for (int i = 0; i < NRANGES - 1; i++)
        if ((float)RANGES[i].s >= every * 30.0f) { s_range = i; break; }

    lv_obj_t *b = dl_round(U.bar, AOS_SYM_CHEVRON_LEFT, 76, AOS_C_CARD2, back_cb, NULL);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *e = dl_round(U.bar, AOS_SYM_PENCIL, 76, DL_C_DARK, edit_cb, NULL);
    lv_obj_align(e, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *t = aos_label(U.bar, c.name, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, U.W - 2 * AOS_UI_PAD - 2 * 96);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 96, -16);
    char src[96], sched[64], sub[180];
    dl_src_desc(&c, src, sizeof src);
    dl_sched_text(&c, sched, sizeof sched);
    snprintf(sub, sizeof sub, "%s · %s", src, sched);
    lv_obj_t *st = aos_label(U.bar, sub, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(st, U.W - 2 * AOS_UI_PAD - 2 * 96);
    lv_label_set_long_mode(st, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(st, LV_ALIGN_LEFT_MID, 96, 24);

    int32_t w = U.W - 2 * AOS_UI_PAD, h = U.H - U.bar_h;
    lv_obj_t *left, *right;
    int32_t lw, rw;
    if (U.land) {
        lw = w * 60 / 100;
        rw = w - lw - AOS_UI_PAD;
        left = dl_column(U.content, lw, h);
        right = dl_column(U.content, rw, h);
        lv_obj_set_x(right, lw + AOS_UI_PAD);
        chart_card(left, lw, h - 40);
    } else {
        lw = rw = w;
        left = right = dl_column(U.content, w, h);
        chart_card(left, w, 560);
    }
    V.chips = dl_chips(right, rw);
    chips_build();
    lv_obj_t *sc = dl_card(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sc, 22, 0);
    V.stats = aos_label(sc, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(V.stats, rw - 44);
    lv_label_set_long_mode(V.stats, LV_LABEL_LONG_MODE_WRAP);
    alarm_card(right, &c, rw);
    file_card(right, rw);

    dl_lock();
    V.seen_reads = ch >= 0 && ch < dl_cfg()->n ? dl_rt(ch)->reads + dl_rt(ch)->errors : 0;
    dl_unlock();
    head_refresh();
    load();
}
