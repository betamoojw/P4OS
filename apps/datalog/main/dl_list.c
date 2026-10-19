/*
 * P4OS - Datalog: the main screen.
 *
 * The bar: the time, medium sized, with the date and whether it is logging
 * under it; play/pause and + on the right. Below, one card per line with
 * its latest value, a sparkline of the last readings, when it reads and how
 * the last one went; a tap opens the line. At the end, the app's settings.
 */
#include "dl.h"

static struct {
    uint32_t sig, seq;
    int n;
    bool running;
} L;

static uint32_t cfg_sig(void)
{
    uint32_t h = 2166136261u;
    dl_lock();
    dl_cfg_t *c = dl_cfg();
    h = (h ^ (uint32_t)c->n) * 16777619u;
    for (int i = 0; i < c->n; i++) {
        const unsigned char *p = (const unsigned char *)&c->ch[i];
        for (size_t k = 0; k < sizeof c->ch[i]; k++) h = (h ^ p[k]) * 16777619u;
    }
    dl_unlock();
    return h;
}

/* ---- the bar ---- */

static void play_cb(lv_event_t *e)
{
    (void)e;
    dl_lock();
    bool on = !dl_cfg()->running;
    int n = dl_cfg()->n;
    dl_unlock();
    if (on && !n) { aos_ui_toast(_("Primero agregá una lectura con +"), 1800); return; }
    dl_set_running(on);
    if (on && !aos_hal_time_is_valid())
        aos_ui_toast(_("Todavía no hay hora de la red: anota el tiempo desde el arranque"), 2600);
    else aos_ui_toast(on ? _("Registrando") : _("En pausa"), 1000);
    dl_list_refresh();
}

static void plus_cb(lv_event_t *e)
{
    (void)e;
    dl_lock();
    int n = dl_cfg()->n;
    dl_unlock();
    if (n >= DL_MAX_CH) { aos_ui_toast(_("Ya hay 16 lecturas: es el máximo"), 1800); return; }
    dl_go(DL_V_EDIT, -1);
}

static void build_bar(void)
{
    U.clock = aos_label(U.bar, "--:--:--", aos_font_large, AOS_C_TEXT);
    lv_obj_align(U.clock, LV_ALIGN_LEFT_MID, 0, U.land ? -14 : -16);
    U.date = aos_label(U.bar, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.date, LV_ALIGN_LEFT_MID, 2, U.land ? 32 : 36);
    lv_obj_t *plus = dl_round(U.bar, AOS_SYM_PLUS, 84, DL_C_DARK, plus_cb, NULL);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
    U.play = dl_round(U.bar, AOS_SYM_PLAY, 84, AOS_C_CARD2, play_cb, NULL);
    lv_obj_align(U.play, LV_ALIGN_RIGHT_MID, -104, 0);
    U.status = aos_label(U.bar, "", aos_font_small, AOS_C_DIM);
    lv_obj_align(U.status, LV_ALIGN_RIGHT_MID, -212, 0);
}

static void bar_refresh(void)
{
    if (!U.clock) return;
    bool valid = aos_hal_time_is_valid();
    struct tm tm;
    aos_hal_time_now(&tm);
    if (tm.tm_sec != U.last_sec || !valid) {
        U.last_sec = tm.tm_sec;
        char t[48];
        if (valid) {
            snprintf(t, sizeof t, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            lv_label_set_text(U.clock, t);
            snprintf(t, sizeof t, "%s %d %s", aos_day_name(tm.tm_wday), tm.tm_mday, aos_month_name(tm.tm_mon));
            lv_label_set_text(U.date, t);
        } else {
            lv_label_set_text(U.clock, "--:--:--");
            lv_label_set_text(U.date, _("sin hora de la red"));
        }
    }
    dl_lock();
    bool run = dl_cfg()->running;
    dl_unlock();
    if (run != L.running || !lv_label_get_text(U.status)[0]) {
        L.running = run;
        lv_label_set_text(U.status, run ? _("Registrando") : _("En pausa"));
        lv_obj_set_style_text_color(U.status, run ? AOS_C_GREEN : AOS_C_ORANGE, 0);
        lv_obj_set_style_bg_color(U.play, run ? lv_color_hex(0x8A4B00) : lv_color_hex(0x1E6B34), 0);
        lv_label_set_text(lv_obj_get_child(U.play, 0), run ? AOS_SYM_PAUSE : AOS_SYM_PLAY);
    }
}

/* ---- the cards ---- */

static void card_cb(lv_event_t *e)
{
    dl_go(DL_V_DETAIL, (int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *line_card(lv_obj_t *parent, int i, const dl_ch_t *c, int32_t w)
{
    const int32_t pad = 20, iw = w - 2 * pad;
    lv_obj_t *cd = dl_card(parent, w, 232);
    lv_obj_set_style_pad_all(cd, pad, 0);
    lv_obj_add_flag(cd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(cd, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(cd, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *g = aos_label(cd, dl_src_glyph(c), &aos_sym_28, DL_C_MAIN);
    lv_obj_set_pos(g, 0, 2);
    lv_obj_t *nm = aos_label(cd, c->name, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(nm, iw - 40 - 120);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(nm, 40, 0);

    lv_obj_t *tag = dl_box(cd, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_radius(tag, 20, 0);
    lv_obj_set_style_pad_hor(tag, 16, 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    lv_obj_t *tl = aos_label(tag, "", aos_font_caption, lv_color_white());
    lv_obj_center(tl);
    lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
    U.c_tag[i] = tag;

    char src[96];
    dl_src_desc(c, src, sizeof src);
    lv_obj_t *sl = aos_label(cd, src, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(sl, iw);
    lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(sl, 0, 42);

    int32_t chw = iw * 45 / 100;
    U.c_val[i] = aos_label(cd, "--", aos_font_large, AOS_C_TEXT);
    lv_obj_set_width(U.c_val[i], iw - chw - 12);
    lv_label_set_long_mode(U.c_val[i], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(U.c_val[i], 0, 78);

    lv_obj_t *ch = lv_chart_create(cd);
    lv_obj_set_size(ch, chw, 84);
    lv_obj_align(ch, LV_ALIGN_TOP_RIGHT, 0, 72);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 2, 0);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(ch, 0, 0);
    lv_chart_set_point_count(ch, 2);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ch, 3, LV_PART_ITEMS);
    U.c_ser[i] = lv_chart_add_series(ch, DL_C_MAIN, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(ch, U.c_ser[i], LV_CHART_POINT_NONE);
    U.c_chart[i] = ch;

    U.c_when[i] = aos_label(cd, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.c_when[i], iw);
    lv_label_set_long_mode(U.c_when[i], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.c_when[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    aos_make_decorative(g);
    aos_make_decorative(nm);
    aos_make_decorative(sl);
    aos_make_decorative(tag);
    aos_make_decorative(ch);
    return cd;
}

static void spark(int i, const dl_rt_t *r)
{
    lv_obj_t *ch = U.c_chart[i];
    if (!ch) return;
    int32_t *y = lv_chart_get_series_y_array(ch, U.c_ser[i]);
    float lo = INFINITY, hi = -INFINITY;
    for (int k = 0; k < r->hn; k++) {
        float v = r->hist[(r->hhead - r->hn + k + DL_HIST) % DL_HIST];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    float scale = fabsf(lo) > 1e6f || fabsf(hi) > 1e6f ? 1.0f : 1000.0f;
    /* as many points as readings: the line goes across from the first one */
    int np = r->hn > 2 ? r->hn : 2;
    if ((int)lv_chart_get_point_count(ch) != np) lv_chart_set_point_count(ch, (uint32_t)np);
    y = lv_chart_get_series_y_array(ch, U.c_ser[i]);
    for (int k = 0; k < np; k++) {
        int j = k - (np - r->hn);
        y[k] = j < 0 ? LV_CHART_POINT_NONE
                     : (int32_t)roundf(r->hist[(r->hhead - r->hn + j + DL_HIST) % DL_HIST] * scale);
    }
    if (lo <= hi) {
        float span = hi - lo, m = (hi + lo) / 2;
        float min = fabsf(m) * 0.02f + 0.001f;
        if (span < min) { lo = m - min / 2; hi = m + min / 2; }
        else { lo -= span * 0.1f; hi += span * 0.1f; }
        lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, (int32_t)floorf(lo * scale), (int32_t)ceilf(hi * scale));
    }
    lv_chart_refresh(ch);
}

/* ---- settings ---- */

static void fmt_pick_cb(lv_event_t *e)
{
    dl_set_semicolon(lv_event_get_user_data(e) != NULL);
    dl_sheet_close();
    dl_list_build();
}

static void fmt_cb(lv_event_t *e)
{
    (void)e;
    dl_lock();
    bool semi = dl_cfg()->semicolon;
    dl_unlock();
    lv_obj_t *sh = dl_sheet_open(_("Formato del CSV"), U.land ? 760 : U.W - 2 * AOS_UI_PAD);
    lv_obj_t *r = dl_chips(sh, LV_PCT(100));
    dl_chip(r, _("Coma y punto decimal"), !semi, fmt_pick_cb, NULL);
    dl_chip(r, _("Punto y coma y coma decimal"), semi, fmt_pick_cb, (void *)1);
    dl_caption(sh, _("Coma y punto es el estándar (LibreOffice, Google Sheets, Python). Punto y coma con coma decimal es lo que espera Excel en castellano. Vale para las líneas nuevas: un archivo que ya existe sigue con lo suyo hasta que se borre."),
               (U.land ? 760 : U.W - 2 * AOS_UI_PAD) - 56);
}

static void modules_cb(lv_event_t *e)
{
    (void)e;
    aos_ui_open("aos.modules");
}

static void settings(lv_obj_t *col, int32_t w)
{
    dl_lock();
    bool semi = dl_cfg()->semicolon;
    dl_unlock();
    dl_section(col, _("AJUSTES"));
    dl_row(col, w, _("Formato del CSV"), semi ? _("Punto y coma") : _("Coma"), fmt_cb, NULL);
    dl_row(col, w, _("Sensores y puertos"), _("Módulos"), modules_cb, NULL);
    char dir[220], t[400];
    dl_csv_dir(dir, sizeof dir);
    const char *root = aos_hal_path_sd_root();
    const char *shown = dir[0] && root ? dir + strlen(root) : "/datalog";
    snprintf(t, sizeof t, _("Cada lectura va a su archivo en %s de la tarjeta (se bajan desde el portal, en Datalog). Mientras registra, la app sigue con la pantalla en otra cosa y arranca sola con la placa; en pausa no."), shown);
    dl_caption(col, t, w);
}

/* ---- the screen ---- */

void dl_list_build(void)
{
    dl_bar_clean();
    build_bar();
    int32_t w = U.W - 2 * AOS_UI_PAD, h = U.H - U.bar_h;
    lv_obj_t *col = dl_column(U.content, w, h);
    lv_obj_set_style_pad_top(col, 4, 0);
    int cols = U.land ? 2 : 1;
    int32_t cw = (w - (cols - 1) * 16) / cols;
    lv_obj_t *grid = dl_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 16, 0);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);

    static dl_cfg_t cfg;
    dl_lock();
    cfg = *dl_cfg();
    dl_unlock();
    for (int i = 0; i < cfg.n; i++) line_card(grid, i, &cfg.ch[i], cw);
    if (!cfg.n) {
        lv_obj_t *e = dl_card(grid, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(e, 28, 0);
        lv_obj_set_flex_flow(e, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(e, 14, 0);
        aos_label(e, _("Todavía no hay lecturas"), aos_font_title, AOS_C_TEXT);
        dl_caption(e, _("Cada lectura es un valor que se anota en un CSV con la fecha y la hora: un sensor (I2C, SPI, 1-Wire, de pines o por puerto serie), una entrada del ADS1115 o un pin del header, cada tanto o a ciertas horas, con alarmas si querés."), w - 56);
        dl_pill(e, AOS_SYM_PLUS, _("Agregar una lectura"), DL_C_DARK, plus_cb, NULL);
    }
    settings(col, w);
    L.sig = cfg_sig();
    L.n = cfg.n;
    L.seq = 0;
    L.running = !cfg.running;     /* the first refresh paints the bar */
    dl_list_refresh();
}

void dl_list_refresh(void)
{
    bar_refresh();
    uint32_t seq = dl_seq();
    if (seq == L.seq) return;
    L.seq = seq;
    if (cfg_sig() != L.sig) {
        /* the lines changed (the portal): build again where the list was */
        lv_obj_t *col = lv_obj_get_child(U.content, 0);
        int32_t y = col ? lv_obj_get_scroll_y(col) : 0;
        if (U.overlay) return;
        dl_list_build();
        col = lv_obj_get_child(U.content, 0);
        if (col) { lv_obj_update_layout(col); lv_obj_scroll_to_y(col, y, LV_ANIM_OFF); }
        return;
    }
    static dl_rt_t rt;
    static dl_ch_t ch;
    for (int i = 0; i < L.n; i++) {
        if (!U.c_val[i]) continue;
        dl_lock();
        rt = *dl_rt(i);
        ch = dl_cfg()->ch[i];
        dl_unlock();
        char t[160], sched[64];
        dl_fmt_value(&ch, rt.have ? rt.v : NAN, true, t, sizeof t);
        lv_label_set_text(U.c_val[i], t);
        lv_obj_set_style_text_color(U.c_val[i], rt.alarm == DL_AL_HIGH ? DL_C_HIGH : rt.alarm == DL_AL_LOW ? DL_C_LOW
                                                 : rt.ok || !rt.when[0] ? AOS_C_TEXT : AOS_C_DIM, 0);
        dl_sched_text(&ch, sched, sizeof sched);
        if (!rt.ok && rt.err[0]) {
            snprintf(t, sizeof t, "%s · %s", rt.when, rt.err);
            lv_obj_set_style_text_color(U.c_when[i], AOS_C_ORANGE, 0);
        } else if (rt.err[0]) {
            snprintf(t, sizeof t, "%s · %s", sched, rt.err);
            lv_obj_set_style_text_color(U.c_when[i], AOS_C_ORANGE, 0);
        } else {
            if (rt.when[0]) snprintf(t, sizeof t, "%s · %s", sched, rt.when);
            else snprintf(t, sizeof t, "%s", sched);
            lv_obj_set_style_text_color(U.c_when[i], AOS_C_DIM, 0);
        }
        lv_label_set_text(U.c_when[i], t);
        lv_obj_t *tag = U.c_tag[i];
        if (rt.alarm == DL_AL_NONE) lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_remove_flag(tag, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(tag, rt.alarm == DL_AL_HIGH ? lv_color_hex(0xB3261E) : lv_color_hex(0x0B5394), 0);
            lv_label_set_text(lv_obj_get_child(tag, 0), rt.alarm == DL_AL_HIGH ? _("ALTA") : _("BAJA"));
        }
        spark(i, &rt);
    }
}
