/*
 * P4OS - 3D printers: control and follow every 3D printer in the house.
 *
 * The list shows each machine with what matters at a glance (its state,
 * the temperatures, the progress, what is left and when it ends); a machine
 * opens into its detail (p3d_detail.c) with the controls its protocol
 * allows. Eight protocols, one driver each (drv_*.c):
 *
 *   OctoPrint, PrusaLink (Prusa Connect's printers, on the LAN), Bambu Lab
 *   (LAN mode, MQTT), Klipper (Moonraker), Duet (RepRapFirmware), Elegoo
 *   (SDCP), FlashForge (port 8898, or G-code on 8899) and Creality (its
 *   WebSocket).
 *
 * The machines are saved in impresoras.txt (p3d_cfg.c), which the portal's
 * page (web/impresoras.js) edits too; the app looks at the file's date
 * every two seconds. The page also reads the status live (aos_hal_live_*)
 * and sends commands through it.
 *
 * The app lives in the background (BACKGROUND): it keeps asking, slowly,
 * so that a print that ends, fails or pauses says so wherever the user is,
 * with a sound, a notice and a number on the icon.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "p3d_app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define TIMER_MS      250
#define CFG_POLL_MS   2000
#define LIVE_MS       1000
#define POLL_LIST     2000
#define POLL_DETAIL   1000
#define POLL_OTHERS   5000
#define POLL_HIDDEN   15000
#define STOP_WAIT_MS  6000

/* A printer: the frame, the gantry, the head over a part on the bed. */
static const uint8_t P3D_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, -31, 74, 7, 3, AIC_C_TEXT, 255),            /* top      */
    AIC_RECT(AIC_CENTER, -34, 0, 7, 68, 3, AIC_C_TEXT, 255),            /* posts    */
    AIC_RECT(AIC_CENTER, 34, 0, 7, 68, 3, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 0, 32, 80, 8, 4, AIC_C_TEXT, 255),             /* base     */
    AIC_RECT(AIC_CENTER, 0, -12, 64, 5, 2, AIC_C_TEXT, 210),            /* gantry   */
    AIC_RECT(AIC_CENTER, 8, -8, 18, 16, 4, AIC_C_LIT(0x1C1C1E), 255),   /* head     */
    AIC_RECT(AIC_CENTER, 8, 3, 6, 7, 1, AIC_C_LIT(0x1C1C1E), 255),      /* nozzle   */
    AIC_RECT(AIC_CENTER, 0, 24, 54, 5, 2, AIC_C_LIT(0x3A3A3C), 255),    /* bed      */
    AIC_RECT(AIC_CENTER, 8, 15, 22, 13, 3, AIC_C_LIT(0x30D158), 255),   /* the part */
    AIC_GRAD(AIC_C_LIT(0x1E8E3E), AIC_GRAD_VER),
    AIC_END
};

p3d_app_t p3d_app;
#define A p3d_app

typedef struct {
    lv_obj_t *card, *dot, *name, *state, *sub, *tool, *bed, *bar, *pct, *file, *time;
    uint32_t  gen;
} row_t;

static struct {
    lv_timer_t  *timer;
    row_t        rows[P3D_MAX];
    lv_obj_t    *summary;
    uint64_t     cfg_poll, live_t;
    int          last_state[P3D_MAX];
    bool         seen[P3D_MAX];
    int          badge;
    bool         icon_on;
} L;

/* ---- the list --------------------------------------------------------------------------- */

static void open_cb(lv_event_t *e)
{
    p3d_go_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    if (A.cfg.count >= P3D_MAX) {
        aos_ui_toast(_("No entran más impresoras"), 2000);
        return;
    }
    p3d_go_edit(-1);
}

static lv_obj_t *dim_label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c, int32_t w)
{
    lv_obj_t *l = aos_label(parent, t, f, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));
    aos_make_decorative(l);
    return l;
}

static void row_update(int i, const p3d_status_t *s)
{
    row_t *r = &L.rows[i];
    if (!r->card) return;
    const p3d_cfg_t *c = &A.cfg.m[i];
    lv_color_t col = p3d_state_color(s->state);
    lv_obj_set_style_bg_color(r->dot, col, 0);
    lv_obj_set_style_text_color(r->state, col, 0);
    char b[160], t1[48], t2[48];
    if (s->state == P3D_ST_PRINTING && s->progress >= 0) {
        p3d_fmt_pct(t1, sizeof t1, s->progress);
        snprintf(b, sizeof b, "%s %s", p3d_state_name(s->state), t1);
    } else {
        p3d_copy(b, sizeof b, p3d_state_name(s->state));
    }
    p3d_set_text(r->state, b);
    if ((s->state == P3D_ST_OFFLINE || s->state == P3D_ST_ERROR) && s->msg[0]) {
        aos_text_safe(b, sizeof b, s->msg);
        lv_obj_set_style_text_color(r->sub, s->state == P3D_ST_ERROR ? AOS_C_RED : AOS_C_ORANGE, 0);
    } else {
        snprintf(b, sizeof b, "%s%s%.47s", p3d_driver(c->kind)->label, s->model[0] ? "  ·  " : "", s->model);
        lv_obj_set_style_text_color(r->sub, AOS_C_DIM, 0);
    }
    p3d_set_text(r->sub, b);
    p3d_fmt_temp(t1, sizeof t1, &s->tool[0]);
    p3d_fmt_temp(t2, sizeof t2, &s->bed);
    p3d_set_text(r->tool, t1);
    p3d_set_text(r->bed, t2);
    bool job = p3d_active(s->state) || s->state == P3D_ST_DONE;
    if (job && s->progress >= 0) {
        lv_obj_remove_flag(r->bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(r->bar, (int32_t)(s->progress * 10), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(r->bar, col, LV_PART_INDICATOR);
        p3d_fmt_pct(b, sizeof b, s->progress);
        p3d_set_text(r->pct, b);
    } else {
        lv_obj_add_flag(r->bar, LV_OBJ_FLAG_HIDDEN);
        p3d_set_text(r->pct, "");
    }
    if (job && s->file[0]) {
        aos_text_safe(b, sizeof b, s->file);
        p3d_set_text(r->file, b);
    } else {
        p3d_set_text(r->file, s->state == P3D_ST_IDLE ? _("Sin trabajo") : "");
    }
    b[0] = '\0';
    if (p3d_active(s->state) && s->remaining_s >= 0) {
        p3d_fmt_dur(t1, sizeof t1, s->remaining_s);
        p3d_fmt_eta(t2, sizeof t2, s->remaining_s);
        if (t2[0]) snprintf(b, sizeof b, _("faltan %s · %s"), t1, t2);
        else snprintf(b, sizeof b, _("faltan %s"), t1);
    } else if (s->layers > 0 && p3d_active(s->state)) {
        snprintf(b, sizeof b, _("capa %d de %d"), s->layer, s->layers);
    }
    p3d_set_text(r->time, b);
}

static void summary_update(void)
{
    if (!L.summary) return;
    int n[P3D_ST_N] = { 0 };
    for (int i = 0; i < A.cfg.count; i++) {
        p3d_status_t s;
        if (p3d_snapshot(i, &s, NULL)) n[s.state]++;
    }
    char b[160];
    int k = 0;
    b[0] = '\0';
    /* the words of a count: "2 imprimiendo", "1 lista", "3 listas" */
    static const struct { int st; const char *one, *many; } W[] = {
        { P3D_ST_PRINTING, N_("imprimiendo"), N_("imprimiendo") },
        { P3D_ST_PAUSED, N_("en pausa"), N_("en pausa") },
        { P3D_ST_BUSY, N_("preparando"), N_("preparando") },
        { P3D_ST_DONE, N_("terminada"), N_("terminadas") },
        { P3D_ST_ERROR, N_("con error"), N_("con error") },
        { P3D_ST_IDLE, N_("lista"), N_("listas") },
        { P3D_ST_OFFLINE, N_("sin conexión"), N_("sin conexión") },
    };
    for (size_t j = 0; j < sizeof W / sizeof W[0]; j++) {
        int c = n[W[j].st];
        if (W[j].st == P3D_ST_DONE) c += n[P3D_ST_CANCELLED];
        if (!c) continue;
        k += snprintf(b + k, sizeof b - (size_t)k, "%s%d %s", k ? "  ·  " : "", c, _(c == 1 ? W[j].one : W[j].many));
        if (k >= (int)sizeof b) break;
    }
    if (!A.cfg.count) p3d_copy(b, sizeof b, "");
    p3d_set_text(L.summary, b);
}

static void build_list(void)
{
    memset(L.rows, 0, sizeof L.rows);
    lv_obj_t *page = p3d_box(A.root, A.W, A.H);
    lv_obj_set_style_bg_color(page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

    lv_obj_t *title = aos_label(page, _("Impresoras 3D"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 26);
    aos_make_decorative(title);
    L.summary = dim_label(page, "", aos_font_caption, AOS_C_DIM, A.W - 2 * AOS_UI_PAD - AOS_UI_TAP_MIN - 20);
    lv_obj_align(L.summary, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 76);
    lv_obj_t *add = p3d_round_btn(page, AOS_SYM_PLUS, add_cb, NULL);
    lv_obj_align(add, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 18);

    const int32_t top = 120;
    lv_obj_t *list = p3d_box(page, A.W, A.H - top);
    lv_obj_set_pos(list, 0, top);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(list, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(list, 8, 0);
    lv_obj_set_style_pad_bottom(list, 40, 0);
    lv_obj_set_style_pad_gap(list, 16, 0);

    int cols = A.land ? 2 : 1;
    int32_t cw = (A.W - 2 * AOS_UI_PAD - (cols - 1) * 16) / cols;
    const int32_t ch = 236, in = 24;
    for (int i = 0; i < A.cfg.count; i++) {
        row_t *r = &L.rows[i];
        r->card = p3d_card(list, cw, ch);
        lv_obj_set_style_bg_color(r->card, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_flag(r->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r->card, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        r->dot = p3d_box(r->card, 18, 18);
        lv_obj_set_style_radius(r->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r->dot, AOS_C_DIM, 0);
        lv_obj_set_pos(r->dot, in, 30);
        aos_make_decorative(r->dot);

        char safe[96];
        aos_text_safe(safe, sizeof safe, A.cfg.m[i].name);
        int32_t state_w = 220;
        r->name = dim_label(r->card, safe, aos_font_body, AOS_C_TEXT, cw - in * 2 - 30 - state_w);
        lv_obj_set_pos(r->name, in + 30, 20);
        r->state = aos_label(r->card, "", aos_font_small, AOS_C_DIM);
        lv_obj_set_style_text_align(r->state, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(r->state, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(r->state, state_w, lv_font_get_line_height(aos_font_small));
        lv_obj_align(r->state, LV_ALIGN_TOP_RIGHT, -in, 24);
        aos_make_decorative(r->state);
        r->sub = dim_label(r->card, "", aos_font_caption, AOS_C_DIM, cw - in * 2);
        lv_obj_set_pos(r->sub, in, 62);

        /* temperatures */
        lv_obj_t *g = aos_label(r->card, AOS_SYM_FIRE, &aos_sym_28, AOS_C_ORANGE);
        lv_obj_set_pos(g, in - 4, 100);
        aos_make_decorative(g);
        r->tool = dim_label(r->card, "--", aos_font_small, AOS_C_TEXT, 170);
        lv_obj_set_pos(r->tool, in + 30, 101);
        g = aos_label(r->card, AOS_SYM_RADIATOR, &aos_sym_28, AOS_C_RED);
        lv_obj_set_pos(g, in + 210, 100);
        aos_make_decorative(g);
        r->bed = dim_label(r->card, "--", aos_font_small, AOS_C_TEXT, 150);
        lv_obj_set_pos(r->bed, in + 246, 101);

        /* the job */
        r->bar = lv_bar_create(r->card);
        lv_bar_set_range(r->bar, 0, 1000);
        lv_obj_set_size(r->bar, cw - in * 2 - 96, 14);
        lv_obj_set_pos(r->bar, in, 152);
        lv_obj_set_style_bg_color(r->bar, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(r->bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r->bar, 7, 0);
        lv_obj_set_style_radius(r->bar, 7, LV_PART_INDICATOR);
        aos_make_decorative(r->bar);
        lv_obj_add_flag(r->bar, LV_OBJ_FLAG_HIDDEN);
        r->pct = aos_label(r->card, "", aos_font_small, AOS_C_TEXT);
        lv_obj_set_style_text_align(r->pct, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(r->pct, 84, lv_font_get_line_height(aos_font_small));
        lv_obj_align(r->pct, LV_ALIGN_TOP_RIGHT, -in, 142);
        aos_make_decorative(r->pct);
        int32_t half = (cw - in * 2) / 2;
        r->file = dim_label(r->card, "", aos_font_caption, AOS_C_DIM, half + 40);
        lv_obj_set_pos(r->file, in, 186);
        r->time = aos_label(r->card, "", aos_font_caption, AOS_C_TEXT);
        lv_obj_set_style_text_align(r->time, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(r->time, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(r->time, half - 48, lv_font_get_line_height(aos_font_caption));
        lv_obj_align(r->time, LV_ALIGN_TOP_RIGHT, -in, 186);
        aos_make_decorative(r->time);

        p3d_status_t s;
        r->gen = 0;
        if (p3d_snapshot(i, &s, &r->gen)) row_update(i, &s);
    }
    if (!A.cfg.count) {
        lv_obj_t *l = aos_label(list, _("Ninguna impresora todavía.\n\nAgregala con + o desde la página Impresoras 3D del portal. Andan OctoPrint, PrusaLink (Prusa), Bambu Lab, Klipper, Duet, Elegoo, FlashForge y Creality."),
                                aos_font_body, AOS_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, A.W - 2 * AOS_UI_PAD);
        aos_make_decorative(l);
        lv_obj_t *b = p3d_btn(list, AOS_SYM_PLUS, _("Agregar una impresora"), AOS_C_ACCENT, add_cb, NULL);
        lv_obj_set_style_margin_top(b, 20, 0);
    }
    summary_update();
}

static void list_tick(void)
{
    for (int i = 0; i < A.cfg.count; i++) {
        p3d_status_t s;
        if (p3d_snapshot(i, &s, &L.rows[i].gen)) row_update(i, &s);
    }
    summary_update();
}

/* ---- screens ------------------------------------------------------------------------------ */

void p3d_build(void)
{
    lv_obj_clean(A.root);
    A.W = lv_obj_get_width(A.root);
    A.H = lv_obj_get_height(A.root);
    A.land = A.W > A.H;
    L.summary = NULL;
    memset(L.rows, 0, sizeof L.rows);
    lv_obj_set_style_bg_color(A.root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(A.root, LV_OPA_COVER, 0);
    if (A.screen == P3D_SCR_DETAIL) p3d_detail_build();
    else if (A.screen == P3D_SCR_EDIT) p3d_edit_build();
    else build_list();
}

static void set_polls(void)
{
    if (A.hidden) p3d_set_poll(POLL_HIDDEN, -1, POLL_HIDDEN);
    else if (A.screen == P3D_SCR_DETAIL) p3d_set_poll(POLL_DETAIL, A.detail, POLL_OTHERS);
    else p3d_set_poll(POLL_LIST, -1, POLL_LIST);
}

void p3d_go_list(void)
{
    if (A.screen == P3D_SCR_DETAIL) p3d_detail_close();
    if (A.screen == P3D_SCR_EDIT) p3d_edit_close();
    A.screen = P3D_SCR_LIST;
    set_polls();
    p3d_build();
}

void p3d_go_detail(int i)
{
    if (i < 0 || i >= A.cfg.count) return;
    if (A.screen == P3D_SCR_DETAIL) p3d_detail_close();
    if (A.screen == P3D_SCR_EDIT) p3d_edit_close();
    A.screen = P3D_SCR_DETAIL;
    A.detail = i;
    set_polls();
    p3d_build();
}

void p3d_go_edit(int i)
{
    if (A.screen == P3D_SCR_DETAIL) p3d_detail_close();
    p3d_edit_open(i);
    A.screen = P3D_SCR_EDIT;
    set_polls();
    p3d_build();
}

bool p3d_cfg_commit(void)
{
    bool ok = p3d_cfg_save(&A.cfg);
    A.cfg_stamp = p3d_cfg_stamp();
    p3d_core_sync(&A.cfg);
    for (int i = 0; i < P3D_MAX; i++) L.seen[i] = false;
    set_polls();
    return ok;
}

/* ---- notices ------------------------------------------------------------------------------- */

static void notices(void)
{
    bool printing = false;
    for (int i = 0; i < A.cfg.count; i++) {
        p3d_status_t s;
        if (!p3d_snapshot(i, &s, NULL)) continue;
        int was = L.last_state[i];
        L.last_state[i] = s.state;
        if (s.state == P3D_ST_PRINTING || s.state == P3D_ST_PAUSED) printing = true;
        if (!L.seen[i]) {
            /* the first state of a machine is not news */
            if (s.state > P3D_ST_CONNECTING) L.seen[i] = true;
            continue;
        }
        if (was == s.state) continue;
        const char *what = NULL;
        bool was_job = was == P3D_ST_PRINTING || was == P3D_ST_PAUSED || was == P3D_ST_BUSY;
        if (s.state == P3D_ST_DONE && was_job) what = _("terminó de imprimir");
        else if (s.state == P3D_ST_ERROR && was != P3D_ST_OFFLINE) what = _("tiene un error");
        else if (s.state == P3D_ST_PAUSED && was == P3D_ST_PRINTING &&
                 (A.hidden || A.screen != P3D_SCR_DETAIL || A.detail != i)) {
            what = _("se pausó");           /* filament, a crash: not the user's own tap */
        }
        else if (s.state == P3D_ST_CANCELLED && was_job) what = _("canceló la impresión");
        if (!what) continue;
        char b[160], name[64];
        aos_text_safe(name, sizeof name, A.cfg.m[i].name);
        snprintf(b, sizeof b, "%s %s", name, what);
        aos_ui_toast(b, 4000);
        aos_ui_sound(AOS_SND_NOTIF);
        aos_hal_log(P3D_TAG, "%s", b);
        if (A.hidden) {
            L.badge++;
            aos_ui_set_badge(P3D_APP_ID, L.badge);
        }
    }
    bool want = printing && A.hidden;
    if (want != L.icon_on) {
        aos_ui_status_icon_set(P3D_APP_ID, want ? AOS_SYM_TIMER_SAND : NULL, 0);
        L.icon_on = want;
    }
}

/* ---- the portal's page --------------------------------------------------------------------- */

static int jesc(char *o, size_t n, const char *s)
{
    return (int)p3d_json_esc(o, n, s);
}

static void live_put(void)
{
    size_t cap = 32 * 1024;
    char *j = p3d_psram(cap);
    if (!j) return;
    int k = snprintf(j, cap, "{\"v\":1,\"machines\":[");
    for (int i = 0; i < A.cfg.count && k < (int)cap - 2048; i++) {
        p3d_status_t s;
        if (!p3d_snapshot(i, &s, NULL)) continue;
        char name[100], file[260], msg[260], raw[64], model[100], cam[400];
        jesc(name, sizeof name, A.cfg.m[i].name);
        jesc(file, sizeof file, s.file);
        jesc(msg, sizeof msg, s.msg);
        jesc(raw, sizeof raw, s.raw);
        jesc(model, sizeof model, s.model);
        char url[200];
        p3d_cam_url(&A.cfg.m[i], &s, url, sizeof url);
        jesc(cam, sizeof cam, strncmp(url, "http", 4) ? "" : url);
        k += snprintf(j + k, cap - (size_t)k,
                      "%s{\"i\":%d,\"name\":\"%s\",\"type\":\"%s\",\"state\":%d,\"raw\":\"%s\",\"msg\":\"%s\","
                      "\"model\":\"%s\",\"tool\":[%.1f,%.1f,%d],\"tool1\":[%.1f,%.1f,%d],\"bed\":[%.1f,%.1f,%d],"
                      "\"chamber\":[%.1f,%.1f,%d],\"progress\":%.1f,\"elapsed\":%ld,\"remaining\":%ld,\"file\":\"%s\","
                      "\"layer\":%d,\"layers\":%d,\"fan\":%d,\"speed\":%d,\"flow\":%d,\"light\":%d,\"caps\":%lu,"
                      "\"cam\":\"%s\",\"age\":%lu}",
                      i ? "," : "", i, name, p3d_driver(A.cfg.m[i].kind)->word, s.state, raw, msg, model,
                      (double)s.tool[0].cur, (double)s.tool[0].target, s.tool[0].on, (double)s.tool[1].cur,
                      (double)s.tool[1].target, s.tool[1].on, (double)s.bed.cur, (double)s.bed.target, s.bed.on,
                      (double)s.chamber.cur, (double)s.chamber.target, s.chamber.on, (double)s.progress,
                      (long)s.elapsed_s, (long)s.remaining_s, file, s.layer, s.layers, s.fan, s.speed, s.flow,
                      s.light, (unsigned long)s.caps, cam,
                      (unsigned long)((uint32_t)aos_hal_uptime_ms() - s.updated_ms));
    }
    k += snprintf(j + k, cap - (size_t)k, "]}");
    aos_hal_live_put(P3D_APP_ID, "status", "application/json", j, (size_t)k);
    free(j);
}

/* "pause 0", "temp 0 2 60", "gcode 0 G28", "start 0 local/x.gcode", "reload" */
static void live_take(void)
{
    char msg[AOS_LIVE_MSG_MAX + 1];
    int n;
    while ((n = aos_hal_live_take(P3D_APP_ID, msg, sizeof msg - 1)) > 0) {
        msg[n] = '\0';
        char op[16] = "";
        int idx = -1, used = 0;
        if (sscanf(msg, "%15s %d %n", op, &idx, &used) < 1) continue;
        if (!strcmp(op, "reload")) {
            A.cfg_stamp = 0;
            continue;
        }
        if (idx < 0 || idx >= A.cfg.count) continue;
        const char *rest = used > 0 ? msg + used : "";
        p3d_cmd_t c = { 0 };
        float a = 0, b = 0, d = 0;
        sscanf(rest, "%f %f %f", &a, &b, &d);
        static const struct { const char *w; int op; } OPS[] = {
            { "pause", P3D_C_PAUSE }, { "resume", P3D_C_RESUME }, { "cancel", P3D_C_CANCEL },
            { "temp", P3D_C_TEMP }, { "jog", P3D_C_JOG }, { "home", P3D_C_HOME }, { "extrude", P3D_C_EXTRUDE },
            { "fan", P3D_C_FAN }, { "speed", P3D_C_SPEED }, { "level", P3D_C_SPEED_LVL }, { "flow", P3D_C_FLOW },
            { "gcode", P3D_C_GCODE }, { "start", P3D_C_START }, { "light", P3D_C_LIGHT }, { "connect", P3D_C_CONNECT },
            { "estop", P3D_C_ESTOP }, { "motors", P3D_C_MOTORS_OFF },
        };
        for (size_t k = 0; k < sizeof OPS / sizeof OPS[0]; k++) {
            if (strcmp(op, OPS[k].w)) continue;
            c.op = OPS[k].op;
            switch (c.op) {
            case P3D_C_TEMP: c.a = (int)a; c.v[0] = b; break;
            case P3D_C_JOG: c.v[0] = a; c.v[1] = b; c.v[2] = d; break;
            case P3D_C_HOME: case P3D_C_SPEED_LVL: case P3D_C_LIGHT: case P3D_C_CONNECT: c.a = (int)a; break;
            case P3D_C_GCODE: case P3D_C_START: p3d_copy(c.text, sizeof c.text, rest); break;
            default: c.v[0] = a; break;
            }
            p3d_send(idx, &c);
        }
    }
}

/* ---- the timer and the life cycle ----------------------------------------------------------- */

static void cfg_check(void)
{
    uint32_t stamp = p3d_cfg_stamp();
    if (stamp == A.cfg_stamp) return;
    A.cfg_stamp = stamp;
    p3d_cfg_load(&A.cfg);
    p3d_core_sync(&A.cfg);
    for (int i = 0; i < P3D_MAX; i++) L.seen[i] = false;
    set_polls();
    if (A.screen == P3D_SCR_DETAIL && A.detail >= A.cfg.count) A.screen = P3D_SCR_LIST;
    if (!A.hidden && A.screen != P3D_SCR_EDIT) {
        if (A.screen == P3D_SCR_DETAIL) p3d_detail_close();
        p3d_build();
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (A.hidden) return;
    uint64_t now = aos_hal_uptime_ms();
    if (now - L.cfg_poll >= CFG_POLL_MS) {
        L.cfg_poll = now;
        cfg_check();
    }
    if (A.screen == P3D_SCR_LIST) list_tick();
    else if (A.screen == P3D_SCR_DETAIL) p3d_detail_tick();
    else p3d_edit_tick();
}

static void tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    notices();
    uint64_t now = aos_hal_uptime_ms();
    if (now - L.live_t >= LIVE_MS) {
        L.live_t = now;
        if (A.hidden && now - L.cfg_poll >= CFG_POLL_MS * 5) {
            L.cfg_poll = now;
            cfg_check();
        }
        if (aos_hal_live_idle_ms(P3D_APP_ID) < 6000) live_put();
        live_take();
    }
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (A.screen == P3D_SCR_DETAIL) {
        if (!p3d_detail_back()) p3d_go_list();
        return true;
    }
    if (A.screen == P3D_SCR_EDIT) {
        if (!p3d_edit_back()) p3d_go_list();
        return true;
    }
    return false;
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A.hidden = true;
    if (A.screen == P3D_SCR_DETAIL) p3d_detail_close();
    set_polls();
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A.hidden = false;
    L.badge = 0;
    aos_ui_set_badge(P3D_APP_ID, 0);
    if (L.icon_on) {
        aos_ui_status_icon_set(P3D_APP_ID, NULL, 0);
        L.icon_on = false;
    }
    set_polls();
    if (A.screen == P3D_SCR_DETAIL) p3d_build();      /* the camera comes back */
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    A.root = root;
    if (A.screen == P3D_SCR_EDIT) p3d_edit_keep();
    if (A.screen == P3D_SCR_DETAIL) p3d_detail_close();
    p3d_build();
    return true;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&A, 0, sizeof A);
    memset(&L, 0, sizeof L);
    A.self = self;
    A.root = root;
    p3d_core_init();
    p3d_cfg_load(&A.cfg);
    A.cfg_stamp = p3d_cfg_stamp();
    p3d_core_sync(&A.cfg);
    L.cfg_poll = aos_hal_uptime_ms();
    A.screen = P3D_SCR_LIST;
    set_polls();
    p3d_build();
    L.timer = lv_timer_create(timer_cb, TIMER_MS, NULL);

    /* Development: P3D_OPEN=<n> opens the n-th machine at once. */
    const char *open = getenv("P3D_OPEN");
    if (open && open[0] >= '0' && open[0] <= '9') p3d_go_detail(open[0] - '0');
    return &A;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (L.timer) {
        lv_timer_delete(L.timer);
        L.timer = NULL;
    }
    if (A.screen == P3D_SCR_DETAIL) p3d_detail_close();
    if (A.screen == P3D_SCR_EDIT) p3d_edit_close();
    lv_obj_clean(A.root);
    /* the loader unloads the .so on the next tick: no thread may be left in it */
    p3d_core_stop_all(STOP_WAIT_MS);
    for (int i = 0; i < 200 && (p3d_cam_threads() > 0 || p3d_edit_threads() > 0); i++) aos_hal_sleep_ms(20);
    p3d_trust_free();
    aos_hal_live_clear(P3D_APP_ID);
    if (L.icon_on) aos_ui_status_icon_set(P3D_APP_ID, NULL, 0);
}

static bool p3d_init(aos_app_t *app)
{
    app->desc.id       = "aos.impresoras";
    app->desc.name     = "Impresoras 3D";
    app->desc.icon     = AOS_SYM_PACKAGE_VARIANT;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xF97316;
    app->desc.color_b  = 0x9A3412;
    app->desc.order    = 132;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND;
    aos_icon_set_ops(app, P3D_ICON, sizeof P3D_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->show    = show;
    app->hide    = hide;
    app->tick    = tick;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(p3d_init);
