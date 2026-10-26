/*
 * P4OS - 3D printers: one machine, with everything its protocol allows.
 *
 * Five tabs, each shown only when the driver can do what it is for:
 *
 *   Estado    the job (a ring with the progress, the picture, the times),
 *             pause and cancel, the temperatures (a tap sets one), preheat
 *             by material, the last twenty minutes of temperature, the AMS
 *             of a Bambu, speed, flow, fan and position
 *   Control   moving the head and homing, extruding, fan, speed, flow,
 *             light, motors off, OctoPrint's serial link, emergency stop
 *   Archivos  the machine's files: print one, delete one
 *   Cámara    the machine's camera (p3d_cam.c), only while it is looked at
 *   Consola   G-code by hand, with the answers when the protocol gives them
 *
 * Nothing here waits on the network: commands go into the machine's queue
 * (p3d_send) and the screen follows what the thread publishes, four times
 * a second.
 */
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "p3d_app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define A p3d_app

enum { TAB_STATUS = 0, TAB_CONTROL, TAB_FILES, TAB_CAMERA, TAB_CONSOLE, TAB_N };
static const char *const TAB_NAMES[TAB_N] = { N_("Estado"), N_("Control"), N_("Archivos"), N_("Cámara"), N_("Consola") };

#define HEATERS   4             /* tool0, tool1, bed, chamber: p3d_cmd_t's 'a' */
#define CHART_N   P3D_HIST
#define CON_MAX   (P3D_CON_LINES * P3D_CON_LEN)
#define ARM_MS    3000

typedef struct {
    const char *name;
    int         tool, bed;
} material_t;

static const material_t MATERIALS[] = {
    { "PLA", 215, 60 }, { "PETG", 240, 80 }, { "ABS", 255, 100 }, { "ASA", 260, 105 }, { "TPU", 225, 50 },
};

static const int PRESET_TOOL[] = { 0, 180, 200, 215, 230, 240, 250, 260, 280 };
static const int PRESET_BED[] = { 0, 40, 50, 60, 70, 80, 90, 100, 110 };
static const int PRESET_CHAMBER[] = { 0, 35, 40, 45, 50, 60 };

static struct {
    int           tab;
    int           want_tab;     /* asked for before the machine said what it can do */
    uint32_t      caps;
    bool          has_cam;
    p3d_status_t  st;
    uint32_t      gen;
    lv_obj_t     *page, *body, *tabrow, *sub;
    int32_t       cw;           /* a card's width */

    /* Estado */
    lv_obj_t     *arc, *pct, *layer, *file, *t_el, *t_rem, *t_eta, *state_l, *msg;
    lv_obj_t     *btnrow, *b_pause, *b_cancel;
    lv_obj_t     *heat_row[HEATERS], *heat_cur[HEATERS], *heat_tgt[HEATERS];
    lv_obj_t     *chart;
    lv_chart_series_t *s_tool, *s_bed;
    uint64_t      chart_t;
    lv_obj_t     *info;
    lv_obj_t     *trays;
    lv_obj_t     *thumb;
    uint16_t     *thumb_px;
    int           thumb_w, thumb_h;
    uint32_t      thumb_gen;
    int           shown_state, shown_ntrays;
    uint64_t      cancel_armed, estop_armed;

    /* Control */
    float         step, ext;
    lv_obj_t     *step_row, *ext_row, *fan_l, *speed_l, *flow_l, *fan_sl, *speed_sl, *flow_sl, *light_sw;
    lv_obj_t     *lvl_row, *busy_note, *b_estop;

    /* Archivos */
    p3d_file_t   *files;
    int           nfiles, sel;
    uint32_t      files_gen;
    bool          files_busy, asked;
    lv_obj_t     *flist, *fmsg;
    uint64_t      del_armed;

    /* Cámara */
    p3d_cam_t    *cam;
    char          cam_url[200];
    uint16_t     *cam_px;
    int           cam_w, cam_h;
    uint32_t      cam_gen;
    lv_obj_t     *cam_box, *cam_img, *cam_msg;
    int32_t       cam_max_h;
    lv_timer_t   *cam_timer;    /* 25 a second: the app's timer is four */

    /* Consola */
    lv_obj_t     *con, *con_scroll, *con_in, *kb;
    uint32_t      con_gen;
    char         *con_buf;

    /* the temperature sheet */
    lv_obj_t     *sheet, *sheet_val;
    int           sheet_heater, sheet_target;
} D;

static void build_tab(void);

static void send_op(int op)
{
    p3d_cmd_t c = { .op = op };
    p3d_send(A.detail, &c);
}

static void send_temp(int heater, int t)
{
    p3d_cmd_t c = { .op = P3D_C_TEMP, .a = heater, .v = { (float)t } };
    p3d_send(A.detail, &c);
}

static const char *heater_name(int h)
{
    static const char *const N[HEATERS] = { N_("Boquilla"), N_("Boquilla 2"), N_("Cama"), N_("Recinto") };
    return _(N[h]);
}

static const p3d_temp_t *heater(const p3d_status_t *s, int h)
{
    return h == 0 ? &s->tool[0] : h == 1 ? &s->tool[1] : h == 2 ? &s->bed : &s->chamber;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *t)
{
    lv_obj_t *l = aos_label(parent, t, aos_font_caption, AOS_C_DIM);
    aos_make_decorative(l);
    return l;
}

static lv_obj_t *row_box(lv_obj_t *parent)
{
    lv_obj_t *r = p3d_box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 12, 0);
    return r;
}

/* a card that grows with what it holds, in a column */
static lv_obj_t *col_card(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *c = p3d_card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 24, 0);
    lv_obj_set_style_pad_row(c, 12, 0);
    return c;
}

/* ---- the temperature sheet ------------------------------------------------------------------ */

static void sheet_close(void)
{
    if (D.sheet) lv_obj_delete(D.sheet);
    D.sheet = NULL;
    D.sheet_val = NULL;
}

static void sheet_show_val(void)
{
    char b[24];
    if (D.sheet_target <= 0) p3d_copy(b, sizeof b, _("Apagada"));
    else snprintf(b, sizeof b, "%d°", D.sheet_target);
    p3d_set_text(D.sheet_val, b);
}

static void sheet_step_cb(lv_event_t *e)
{
    D.sheet_target += (int)(intptr_t)lv_event_get_user_data(e);
    if (D.sheet_target < 0) D.sheet_target = 0;
    if (D.sheet_target > 400) D.sheet_target = 400;
    sheet_show_val();
}

static void sheet_preset_cb(lv_event_t *e)
{
    D.sheet_target = (int)(intptr_t)lv_event_get_user_data(e);
    sheet_show_val();
}

static void sheet_ok_cb(lv_event_t *e)
{
    (void)e;
    send_temp(D.sheet_heater, D.sheet_target);
    sheet_close();
}

static void sheet_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) sheet_close();
}

static void sheet_open(int h)
{
    sheet_close();
    D.sheet_heater = h;
    const p3d_temp_t *t = heater(&D.st, h);
    D.sheet_target = (int)roundf(t->target);
    lv_obj_t *o = p3d_box(A.root, A.W, A.H);
    D.sheet = o;
    lv_obj_set_style_bg_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, sheet_bg_cb, LV_EVENT_CLICKED, NULL);
    int32_t w = A.land ? 640 : A.W - 2 * AOS_UI_PAD;
    lv_obj_t *c = col_card(o, w);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, AOS_C_CARD2, 0);
    lv_obj_set_style_pad_all(c, 28, 0);
    lv_obj_center(c);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    char b[64];
    snprintf(b, sizeof b, "%s", heater_name(h));
    aos_make_decorative(aos_label(c, b, aos_font_title, AOS_C_TEXT));
    char now[32];
    p3d_fmt_temp(now, sizeof now, t);
    snprintf(b, sizeof b, _("Ahora: %s"), now);
    caption(c, b);

    lv_obj_t *r = p3d_box(c, lv_pct(100), 110);
    D.sheet_val = aos_label(r, "", aos_font_huge, AOS_C_TEXT);
    lv_obj_center(D.sheet_val);
    aos_make_decorative(D.sheet_val);
    lv_obj_t *m10 = p3d_round_btn(r, AOS_SYM_MINUS, sheet_step_cb, (void *)(intptr_t)-5);
    lv_obj_align(m10, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *p10 = p3d_round_btn(r, AOS_SYM_PLUS, sheet_step_cb, (void *)(intptr_t)5);
    lv_obj_align(p10, LV_ALIGN_RIGHT_MID, 0, 0);
    sheet_show_val();

    lv_obj_t *pr = row_box(c);
    const int *pre = h == 2 ? PRESET_BED : h == 3 ? PRESET_CHAMBER : PRESET_TOOL;
    int np = h == 2 ? (int)(sizeof PRESET_BED / sizeof PRESET_BED[0])
           : h == 3 ? (int)(sizeof PRESET_CHAMBER / sizeof PRESET_CHAMBER[0])
                    : (int)(sizeof PRESET_TOOL / sizeof PRESET_TOOL[0]);
    for (int i = 0; i < np; i++) {
        char t2[24];
        if (pre[i] == 0) p3d_copy(t2, sizeof t2, _("Apagar"));
        else snprintf(t2, sizeof t2, "%d°", pre[i]);
        p3d_chip(pr, t2, false, sheet_preset_cb, (void *)(intptr_t)pre[i]);
    }
    lv_obj_t *br = row_box(c);
    lv_obj_set_style_margin_top(br, 10, 0);
    p3d_btn(br, AOS_SYM_CHECK, _("Aplicar"), AOS_C_ACCENT, sheet_ok_cb, NULL);
}

/* ---- Estado ------------------------------------------------------------------------------------ */

static void pause_cb(lv_event_t *e)
{
    (void)e;
    send_op(D.st.state == P3D_ST_PAUSED ? P3D_C_RESUME : P3D_C_PAUSE);
    aos_ui_toast(D.st.state == P3D_ST_PAUSED ? _("Reanudando...") : _("Pausando..."), 1500);
}

static void cancel_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_current_target(e);
    uint64_t now = aos_hal_uptime_ms();
    if (now - D.cancel_armed > ARM_MS) {
        D.cancel_armed = now;
        lv_obj_t *l = lv_obj_get_child(b, 1);
        if (l) lv_label_set_text(l, _("¿Cancelar? Tocá otra vez"));
        return;
    }
    D.cancel_armed = 0;
    send_op(P3D_C_CANCEL);
    aos_ui_toast(_("Cancelando la impresión"), 2000);
}

static void heater_cb(lv_event_t *e)
{
    if (!(D.caps & P3D_CAP_TEMP)) return;
    int h = (int)(intptr_t)lv_event_get_user_data(e);
    if (h == 3 && !(D.caps & P3D_CAP_CHAMBER)) return;
    sheet_open(h);
}

static void material_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0) {
        send_temp(0, 0);
        if (D.st.tool[1].on) send_temp(1, 0);
        send_temp(2, 0);
        aos_ui_toast(_("Enfriando"), 1500);
        return;
    }
    send_temp(0, MATERIALS[i].tool);
    send_temp(2, MATERIALS[i].bed);
    char b[64];
    snprintf(b, sizeof b, _("Precalentando para %s"), MATERIALS[i].name);
    aos_ui_toast(b, 1500);
}

static void to_files_cb(lv_event_t *e)
{
    (void)e;
    D.tab = TAB_FILES;
    D.want_tab = -1;
    build_tab();
}

static void status_buttons(void)
{
    if (!D.btnrow) return;
    lv_obj_clean(D.btnrow);
    D.b_pause = D.b_cancel = NULL;
    int st = D.st.state;
    bool job = st == P3D_ST_PRINTING || st == P3D_ST_PAUSED || st == P3D_ST_BUSY;
    if (job && (D.caps & P3D_CAP_PAUSE) && st != P3D_ST_BUSY) {
        bool paused = st == P3D_ST_PAUSED;
        D.b_pause = p3d_btn(D.btnrow, paused ? AOS_SYM_PLAY : AOS_SYM_PAUSE, paused ? _("Reanudar") : _("Pausar"),
                            paused ? AOS_C_GREEN : lv_color_hex(0xB45309), pause_cb, NULL);
    }
    if (job && (D.caps & P3D_CAP_CANCEL)) {
        D.b_cancel = p3d_btn(D.btnrow, AOS_SYM_STOP, _("Cancelar"), lv_color_hex(0x9B1C1C), cancel_cb, NULL);
    }
    if (!job && (D.caps & P3D_CAP_FILES) && st > P3D_ST_CONNECTING) {
        p3d_btn(D.btnrow, AOS_SYM_FOLDER_OPEN, _("Imprimir un archivo"), AOS_C_CARD2, to_files_cb, NULL);
    }
    D.shown_state = st;
}

static void trays_update(void)
{
    if (!D.trays) return;
    lv_obj_clean(D.trays);
    for (int i = 0; i < D.st.ntrays; i++) {
        const p3d_tray_t *t = &D.st.tray[i];
        lv_obj_t *c = p3d_box(D.trays, 120, 132);
        lv_obj_set_style_radius(c, 18, 0);
        lv_obj_set_style_bg_color(c, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        if (i == D.st.tray_now) {
            lv_obj_set_style_border_width(c, 4, 0);
            lv_obj_set_style_border_color(c, AOS_C_TEXT, 0);
        }
        lv_obj_t *sw = p3d_box(c, 56, 56);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(sw, t->on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(t->rgb), 0);
        lv_obj_set_style_border_width(sw, 2, 0);
        lv_obj_set_style_border_color(sw, AOS_C_DIM, 0);
        lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, 12);
        char b[24];
        lv_obj_t *l = aos_label(c, t->on ? t->type : "--", aos_font_caption, AOS_C_TEXT);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 74);
        if (t->on && t->remain >= 0) {
            snprintf(b, sizeof b, "%d%%", t->remain);
            lv_obj_t *r = aos_label(c, b, aos_font_tiny, AOS_C_DIM);
            lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 100);
        }
        aos_make_decorative(c);
    }
    D.shown_ntrays = D.st.ntrays;
}

static void chart_update(void)
{
    if (!D.chart) return;
    static int16_t tool[CHART_N], bed[CHART_N];
    int n = p3d_history(A.detail, tool, bed, CHART_N);
    int32_t *yt = lv_chart_get_series_y_array(D.chart, D.s_tool);
    int32_t *yb = lv_chart_get_series_y_array(D.chart, D.s_bed);
    int hi = 50;
    for (int i = 0; i < CHART_N; i++) {
        int k = i - (CHART_N - n);
        if (k < 0) {
            yt[i] = yb[i] = LV_CHART_POINT_NONE;
            continue;
        }
        yt[i] = tool[k] / 10;
        yb[i] = bed[k] / 10;
        if (yt[i] > hi) hi = (int)yt[i];
        if (yb[i] > hi) hi = (int)yb[i];
    }
    lv_chart_set_axis_range(D.chart, LV_CHART_AXIS_PRIMARY_Y, 0, (hi / 50 + 1) * 50);
    lv_chart_refresh(D.chart);
}

static void thumb_update(void)
{
    if (!D.thumb) return;
    if (!p3d_thumb_take(A.detail, &D.thumb_px, &D.thumb_w, &D.thumb_h, &D.thumb_gen)) return;
    if (!D.thumb_px) {
        lv_obj_add_flag(lv_obj_get_parent(D.thumb), LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* a canvas: set_buffer drops LVGL's cached copy of the old one */
    lv_canvas_set_buffer(D.thumb, D.thumb_px, D.thumb_w, D.thumb_h, LV_COLOR_FORMAT_RGB565);
    lv_image_set_pivot(D.thumb, D.thumb_w / 2, D.thumb_h / 2);
    int big = D.thumb_w > D.thumb_h ? D.thumb_w : D.thumb_h;
    lv_image_set_scale(D.thumb, (uint32_t)(256 * 104 / (big > 0 ? big : 1)));
    lv_obj_remove_flag(lv_obj_get_parent(D.thumb), LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(D.thumb);
}

static void status_update(void)
{
    const p3d_status_t *s = &D.st;
    char b[200], t[48], t2[48];
    lv_color_t col = p3d_state_color(s->state);
    bool job = p3d_active(s->state) || s->state == P3D_ST_DONE;
    if (D.arc) {
        lv_arc_set_value(D.arc, job && s->progress >= 0 ? (int32_t)(s->progress * 10) : 0);
        lv_obj_set_style_arc_color(D.arc, col, LV_PART_INDICATOR);
        p3d_fmt_pct(b, sizeof b, job ? s->progress : -1);
        p3d_set_text(D.pct, b);
        if (s->layers > 0 && job) snprintf(b, sizeof b, _("capa %d/%d"), s->layer, s->layers);
        else b[0] = '\0';
        p3d_set_text(D.layer, b);
        p3d_set_text(D.state_l, p3d_state_name(s->state));
        lv_obj_set_style_text_color(D.state_l, col, 0);
        if (job && s->file[0]) aos_text_safe(b, sizeof b, s->file);
        else p3d_copy(b, sizeof b, s->state == P3D_ST_IDLE ? _("Sin trabajo") : "");
        p3d_set_text(D.file, b);
        if (job) {
            p3d_fmt_dur(t, sizeof t, s->elapsed_s);
            snprintf(b, sizeof b, _("Lleva %s"), t);
            p3d_set_text(D.t_el, s->elapsed_s >= 0 ? b : "");
            p3d_fmt_dur(t, sizeof t, s->remaining_s);
            snprintf(b, sizeof b, _("Faltan %s"), t);
            p3d_set_text(D.t_rem, s->remaining_s >= 0 && s->state != P3D_ST_DONE ? b : "");
            p3d_fmt_eta(t2, sizeof t2, s->remaining_s);
            snprintf(b, sizeof b, _("Termina %s"), t2);
            p3d_set_text(D.t_eta, t2[0] && s->state != P3D_ST_DONE ? b : "");
        } else {
            p3d_set_text(D.t_el, "");
            p3d_set_text(D.t_rem, "");
            p3d_set_text(D.t_eta, "");
        }
        if (s->msg[0]) {
            aos_text_safe(b, sizeof b, s->msg);
            p3d_set_text(D.msg, b);
            lv_obj_remove_flag(D.msg, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(D.msg, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (D.shown_state != s->state) status_buttons();
    for (int h = 0; h < HEATERS; h++) {
        if (!D.heat_row[h]) continue;
        const p3d_temp_t *ht = heater(s, h);
        if (!ht->on) {
            lv_obj_add_flag(D.heat_row[h], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(D.heat_row[h], LV_OBJ_FLAG_HIDDEN);
        snprintf(b, sizeof b, "%.1f°", (double)ht->cur);
        p3d_set_text(D.heat_cur[h], b);
        if (ht->target > 0.5f) snprintf(b, sizeof b, _("objetivo %d°"), (int)roundf(ht->target));
        else if (h == 3 && !(D.caps & P3D_CAP_CHAMBER)) p3d_copy(b, sizeof b, _("sensor"));
        else p3d_copy(b, sizeof b, _("apagado"));
        p3d_set_text(D.heat_tgt[h], b);
    }
    if (D.info) {
        int k = 0;
        b[0] = '\0';
        if (s->speed >= 0) k += snprintf(b + k, sizeof b - (size_t)k, _("Velocidad %d%%"), s->speed);
        if (s->speed_lvl > 0) {
            static const char *const LV[] = { "", N_("silencioso"), N_("estándar"), N_("deportivo"), N_("a fondo") };
            k += snprintf(b + k, sizeof b - (size_t)k, " (%s)", _(LV[s->speed_lvl > 4 ? 4 : s->speed_lvl]));
        }
        if (s->flow >= 0) k += snprintf(b + k, sizeof b - (size_t)k, "%s%s %d%%", k ? "  ·  " : "", _("Flujo"), s->flow);
        if (s->fan >= 0) k += snprintf(b + k, sizeof b - (size_t)k, "%s%s %d%%", k ? "  ·  " : "", _("Ventilador"), s->fan);
        if (s->has_pos && k < (int)sizeof b - 40) {
            k += snprintf(b + k, sizeof b - (size_t)k, "\nX %.1f  Y %.1f  Z %.2f", (double)s->pos[0], (double)s->pos[1],
                          (double)s->pos[2]);
        }
        if ((s->fw[0] || s->raw[0]) && k < (int)sizeof b - 60) {
            k += snprintf(b + k, sizeof b - (size_t)k, "\n%.40s%s%.24s", s->fw, s->fw[0] && s->raw[0] ? "  ·  " : "", s->raw);
        }
        p3d_set_text(D.info, b);
    }
    if (D.trays && D.shown_ntrays != s->ntrays) trays_update();
    else if (D.trays && s->ntrays) trays_update();
    uint64_t now = aos_hal_uptime_ms();
    if (D.chart && now - D.chart_t > 2000) {
        D.chart_t = now;
        chart_update();
    }
    thumb_update();
}

static lv_obj_t *heater_row(lv_obj_t *card, int h)
{
    lv_obj_t *r = p3d_box(card, lv_pct(100), 84);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_add_event_cb(r, heater_cb, LV_EVENT_CLICKED, (void *)(intptr_t)h);
    static const char *const G[HEATERS] = { AOS_SYM_FIRE, AOS_SYM_FIRE, AOS_SYM_RADIATOR, AOS_SYM_THERMOMETER };
    lv_obj_t *g = aos_label(r, G[h], &aos_sym_44, h >= 2 ? AOS_C_RED : AOS_C_ORANGE);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *n = aos_label(r, heater_name(h), aos_font_body, AOS_C_TEXT);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 64, 6);
    D.heat_tgt[h] = aos_label(r, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(D.heat_tgt[h], LV_ALIGN_TOP_LEFT, 64, 46);
    D.heat_cur[h] = aos_label(r, "--", aos_font_large, AOS_C_TEXT);
    lv_obj_align(D.heat_cur[h], LV_ALIGN_RIGHT_MID, (D.caps & P3D_CAP_TEMP) ? -40 : 0, 0);
    if ((D.caps & P3D_CAP_TEMP) && (h != 3 || (D.caps & P3D_CAP_CHAMBER))) {
        lv_obj_t *c = aos_label(r, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++) aos_make_decorative(lv_obj_get_child(r, (int32_t)i));
    D.heat_row[h] = r;
    return r;
}

static void build_status(lv_obj_t *body)
{
    int32_t w = D.cw;
    /* the job */
    lv_obj_t *job = p3d_card(body, w, 340);
    int32_t ad = 250;
    D.arc = lv_arc_create(job);
    lv_obj_set_size(D.arc, ad, ad);
    lv_obj_align(D.arc, LV_ALIGN_LEFT_MID, 24, 0);
    lv_arc_set_range(D.arc, 0, 1000);
    lv_arc_set_bg_angles(D.arc, 135, 45);
    lv_arc_set_value(D.arc, 0);
    lv_obj_remove_style(D.arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(D.arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(D.arc, 22, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(D.arc, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(D.arc, true, LV_PART_INDICATOR);
    aos_make_decorative(D.arc);
    D.pct = aos_label(D.arc, "--", aos_font_large, AOS_C_TEXT);
    lv_obj_align(D.pct, LV_ALIGN_CENTER, 0, -14);
    D.layer = aos_label(D.arc, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(D.layer, LV_ALIGN_CENTER, 0, 34);
    D.state_l = aos_label(D.arc, "", aos_font_small, AOS_C_DIM);
    lv_obj_align(D.state_l, LV_ALIGN_BOTTOM_MID, 0, -6);

    int32_t tx = 24 + ad + 24, tw = w - tx - 20;
    lv_obj_t *col = p3d_box(job, tw, 316);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, tx, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_t *tb = p3d_box(col, 104, 104);
    D.thumb = lv_canvas_create(tb);
    lv_obj_center(D.thumb);
    lv_obj_add_flag(tb, LV_OBJ_FLAG_HIDDEN);
    D.thumb_gen = 0;
    D.file = aos_label(col, "", aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(D.file, LV_LABEL_LONG_MODE_DOTS);
    /* two lines: a slicer's names are long ("Domestika Lamp - Lamp Shade - ...") */
    lv_obj_set_size(D.file, tw, 2 * lv_font_get_line_height(aos_font_body));
    D.t_el = aos_label(col, "", aos_font_small, AOS_C_DIM);
    D.t_rem = aos_label(col, "", aos_font_small, AOS_C_TEXT);
    D.t_eta = aos_label(col, "", aos_font_small, AOS_C_DIM);
    aos_make_decorative(job);

    D.msg = aos_label(body, "", aos_font_small, AOS_C_ORANGE);
    lv_label_set_long_mode(D.msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(D.msg, w);
    lv_obj_add_flag(D.msg, LV_OBJ_FLAG_HIDDEN);
    aos_make_decorative(D.msg);

    D.btnrow = p3d_box(body, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(D.btnrow, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(D.btnrow, 14, 0);
    D.shown_state = -1;

    /* temperatures */
    lv_obj_t *tc = col_card(body, w);
    lv_obj_set_style_pad_row(tc, 4, 0);
    caption(tc, (D.caps & P3D_CAP_TEMP) ? _("Temperaturas (tocá para cambiar)") : _("Temperaturas"));
    for (int h = 0; h < HEATERS; h++) heater_row(tc, h);
    if (D.caps & P3D_CAP_TEMP) {
        lv_obj_t *mr = row_box(tc);
        lv_obj_set_style_margin_top(mr, 10, 0);
        for (int i = 0; i < (int)(sizeof MATERIALS / sizeof MATERIALS[0]); i++) {
            p3d_chip(mr, MATERIALS[i].name, false, material_cb, (void *)(intptr_t)i);
        }
        p3d_chip(mr, _("Enfriar"), false, material_cb, (void *)(intptr_t)-1);
    }

    /* the last twenty minutes */
    lv_obj_t *cc = col_card(body, w);
    caption(cc, _("Últimos 20 minutos"));
    D.chart = lv_chart_create(cc);
    lv_obj_set_size(D.chart, lv_pct(100), 220);
    lv_chart_set_type(D.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(D.chart, CHART_N);
    lv_chart_set_div_line_count(D.chart, 4, 0);
    lv_obj_set_style_bg_opa(D.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(D.chart, 0, 0);
    lv_obj_set_style_line_color(D.chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_size(D.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(D.chart, 4, LV_PART_ITEMS);
    D.s_tool = lv_chart_add_series(D.chart, AOS_C_ORANGE, LV_CHART_AXIS_PRIMARY_Y);
    D.s_bed = lv_chart_add_series(D.chart, AOS_C_RED, LV_CHART_AXIS_PRIMARY_Y);
    aos_make_decorative(D.chart);
    lv_obj_t *lg = row_box(cc);
    lv_obj_t *l1 = aos_label(lg, heater_name(0), aos_font_caption, AOS_C_ORANGE);
    lv_obj_t *l2 = aos_label(lg, heater_name(2), aos_font_caption, AOS_C_RED);
    aos_make_decorative(l1);
    aos_make_decorative(l2);
    D.chart_t = 0;

    /* the AMS */
    if (D.st.ntrays > 0) {
        lv_obj_t *ac = col_card(body, w);
        caption(ac, "AMS");
        D.trays = row_box(ac);
        D.shown_ntrays = -1;
    }

    lv_obj_t *ic = col_card(body, w);
    D.info = aos_label(ic, "", aos_font_small, AOS_C_TEXT);
    lv_label_set_long_mode(D.info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(D.info, lv_pct(100));
    aos_make_decorative(D.info);
}

/* ---- Control ------------------------------------------------------------------------------------- */

static void control_rows(void);

static void step_cb(lv_event_t *e)
{
    D.step = (float)(intptr_t)lv_event_get_user_data(e) / 10.0f;
    control_rows();
}

static void ext_cb(lv_event_t *e)
{
    D.ext = (float)(intptr_t)lv_event_get_user_data(e);
    control_rows();
}

static bool moving_ok(void)
{
    if (D.st.state == P3D_ST_PRINTING || D.st.state == P3D_ST_BUSY) {
        aos_ui_toast(_("Mientras imprime no se mueve a mano"), 2000);
        return false;
    }
    return true;
}

static void jog_cb(lv_event_t *e)
{
    if (!moving_ok()) return;
    int d = (int)(intptr_t)lv_event_get_user_data(e);    /* axis * 2 + (negative) */
    p3d_cmd_t c = { .op = P3D_C_JOG };
    c.v[d / 2] = d % 2 ? -D.step : D.step;
    p3d_send(A.detail, &c);
}

static void home_cb(lv_event_t *e)
{
    if (!moving_ok()) return;
    p3d_cmd_t c = { .op = P3D_C_HOME, .a = (int)(intptr_t)lv_event_get_user_data(e) };
    p3d_send(A.detail, &c);
    aos_ui_toast(_("Haciendo home"), 1200);
}

static void extrude_cb(lv_event_t *e)
{
    if (!moving_ok()) return;
    if (D.st.tool[0].on && D.st.tool[0].cur < 170) {
        aos_ui_toast(_("La boquilla está fría: calentala antes de extruir"), 2500);
        return;
    }
    int sign = (int)(intptr_t)lv_event_get_user_data(e);
    p3d_cmd_t c = { .op = P3D_C_EXTRUDE, .v = { sign * D.ext } };
    p3d_send(A.detail, &c);
}

static void slider_label(void)
{
    char b[48];
    if (D.fan_l) {
        snprintf(b, sizeof b, "%s  %d%%", _("Ventilador"), (int)lv_slider_get_value(D.fan_sl));
        p3d_set_text(D.fan_l, b);
    }
    if (D.speed_l) {
        snprintf(b, sizeof b, "%s  %d%%", _("Velocidad"), (int)lv_slider_get_value(D.speed_sl));
        p3d_set_text(D.speed_l, b);
    }
    if (D.flow_l) {
        snprintf(b, sizeof b, "%s  %d%%", _("Flujo"), (int)lv_slider_get_value(D.flow_sl));
        p3d_set_text(D.flow_l, b);
    }
}

static void slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    slider_label();
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    p3d_cmd_t c = { .op = (int)(intptr_t)lv_event_get_user_data(e), .v = { (float)lv_slider_get_value(sl) } };
    p3d_send(A.detail, &c);
}

static void level_cb(lv_event_t *e)
{
    p3d_cmd_t c = { .op = P3D_C_SPEED_LVL, .a = (int)(intptr_t)lv_event_get_user_data(e) };
    p3d_send(A.detail, &c);
    D.st.speed_lvl = c.a;
    control_rows();
}

static void light_cb(lv_event_t *e)
{
    p3d_cmd_t c = { .op = P3D_C_LIGHT, .a = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0 };
    p3d_send(A.detail, &c);
}

static void simple_cb(lv_event_t *e)
{
    int op = (int)(intptr_t)lv_event_get_user_data(e);
    if (op == P3D_C_MOTORS_OFF && !moving_ok()) return;
    send_op(op);
    aos_ui_toast(_("Enviado"), 1000);
}

static void connect_cb(lv_event_t *e)
{
    p3d_cmd_t c = { .op = P3D_C_CONNECT, .a = (int)(intptr_t)lv_event_get_user_data(e) };
    p3d_send(A.detail, &c);
    aos_ui_toast(c.a ? _("Conectando OctoPrint con la impresora") : _("Desconectando"), 1500);
}

static void estop_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_current_target(e);
    uint64_t now = aos_hal_uptime_ms();
    if (now - D.estop_armed > ARM_MS) {
        D.estop_armed = now;
        lv_obj_t *l = lv_obj_get_child(b, 1);
        if (l) lv_label_set_text(l, _("¿Parar todo? Tocá otra vez"));
        return;
    }
    D.estop_armed = 0;
    send_op(P3D_C_ESTOP);
    aos_ui_toast(_("Parada de emergencia"), 2000);
}

static void control_rows(void)
{
    if (D.step_row) {
        lv_obj_clean(D.step_row);
        static const int STEPS[] = { 1, 10, 100, 500 };     /* tenths of a mm */
        for (int i = 0; i < 4; i++) {
            char b[24];
            if (STEPS[i] < 10) snprintf(b, sizeof b, "0.%d mm", STEPS[i]);
            else snprintf(b, sizeof b, "%d mm", STEPS[i] / 10);
            p3d_chip(D.step_row, b, fabsf(D.step * 10 - (float)STEPS[i]) < 0.5f, step_cb, (void *)(intptr_t)STEPS[i]);
        }
    }
    if (D.ext_row) {
        lv_obj_clean(D.ext_row);
        static const int EXT[] = { 1, 5, 10, 50 };
        for (int i = 0; i < 4; i++) {
            char b[24];
            snprintf(b, sizeof b, "%d mm", EXT[i]);
            p3d_chip(D.ext_row, b, (int)D.ext == EXT[i], ext_cb, (void *)(intptr_t)EXT[i]);
        }
    }
    if (D.lvl_row) {
        lv_obj_clean(D.lvl_row);
        static const char *const LV[] = { N_("Silencioso"), N_("Estándar"), N_("Deportivo"), N_("A fondo") };
        for (int i = 0; i < 4; i++) p3d_chip(D.lvl_row, _(LV[i]), D.st.speed_lvl == i + 1, level_cb, (void *)(intptr_t)(i + 1));
    }
}

static lv_obj_t *jog_btn(lv_obj_t *parent, const char *glyph, int d)
{
    lv_obj_t *b = p3d_round_btn(parent, glyph, jog_cb, (void *)(intptr_t)d);
    lv_obj_set_size(b, 104, 104);
    return b;
}

static lv_obj_t *slider(lv_obj_t *card, lv_obj_t **label, lv_obj_t **sl, int op, int lo, int hi, int val)
{
    *label = aos_label(card, "", aos_font_small, AOS_C_TEXT);
    aos_make_decorative(*label);
    *sl = lv_slider_create(card);
    lv_obj_set_width(*sl, lv_pct(96));
    lv_obj_set_height(*sl, 18);
    lv_obj_set_style_margin_ver(*sl, 14, 0);
    lv_obj_set_style_margin_left(*sl, 12, 0);
    lv_slider_set_range(*sl, lo, hi);
    lv_slider_set_value(*sl, val < lo ? lo : val > hi ? hi : val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(*sl, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(*sl, AOS_C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(*sl, AOS_C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(*sl, 10, LV_PART_KNOB);
    lv_obj_add_event_cb(*sl, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)op);
    lv_obj_add_event_cb(*sl, slider_cb, LV_EVENT_RELEASED, (void *)(intptr_t)op);
    return *sl;
}

static void build_control(lv_obj_t *body)
{
    int32_t w = D.cw;
    uint32_t c = D.caps;
    if (D.step <= 0) D.step = 10;
    if (D.ext <= 0) D.ext = 5;
    if (c & (P3D_CAP_JOG | P3D_CAP_HOME)) {
        lv_obj_t *card = col_card(body, w);
        caption(card, _("Mover el cabezal"));
        lv_obj_t *pad = p3d_box(card, lv_pct(100), 340);
        if (c & P3D_CAP_JOG) {
            /* the XY cross on the left, Z on the right */
            int32_t cx = 170;
            lv_obj_t *b;
            b = jog_btn(pad, AOS_SYM_ARROW_UP_BOLD, 2);         /* +Y */
            lv_obj_align(b, LV_ALIGN_TOP_LEFT, cx - 52, 0);
            b = jog_btn(pad, AOS_SYM_ARROW_DOWN_BOLD, 3);       /* -Y */
            lv_obj_align(b, LV_ALIGN_TOP_LEFT, cx - 52, 232);
            b = jog_btn(pad, AOS_SYM_ARROW_LEFT_BOLD, 1);       /* -X */
            lv_obj_align(b, LV_ALIGN_TOP_LEFT, cx - 168, 116);
            b = jog_btn(pad, AOS_SYM_ARROW_RIGHT_BOLD, 0);      /* +X */
            lv_obj_align(b, LV_ALIGN_TOP_LEFT, cx + 64, 116);
            lv_obj_t *h = p3d_round_btn(pad, AOS_SYM_AXIS_ARROW, home_cb, (void *)(intptr_t)3);
            lv_obj_set_size(h, 104, 104);
            lv_obj_align(h, LV_ALIGN_TOP_LEFT, cx - 52, 116);
            lv_obj_t *xy = aos_label(h, "XY", aos_font_tiny, AOS_C_DIM);
            lv_obj_align(xy, LV_ALIGN_BOTTOM_MID, 0, -8);
            b = jog_btn(pad, AOS_SYM_CHEVRON_UP, 4);            /* +Z */
            lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -20, 40);
            b = jog_btn(pad, AOS_SYM_CHEVRON_DOWN, 5);          /* -Z */
            lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -20, 196);
            lv_obj_t *z = aos_label(pad, "Z", aos_font_body, AOS_C_DIM);
            lv_obj_align(z, LV_ALIGN_TOP_RIGHT, -60, 155);
            aos_make_decorative(z);
        } else {
            lv_obj_set_height(pad, 0);
        }
        if (c & P3D_CAP_JOG) {
            D.step_row = row_box(card);
        }
        if (c & P3D_CAP_HOME) {
            caption(card, _("Home"));
            lv_obj_t *hr = row_box(card);
            p3d_chip(hr, _("Todos"), false, home_cb, (void *)(intptr_t)7);
            p3d_chip(hr, "X", false, home_cb, (void *)(intptr_t)1);
            p3d_chip(hr, "Y", false, home_cb, (void *)(intptr_t)2);
            p3d_chip(hr, "Z", false, home_cb, (void *)(intptr_t)4);
        }
        D.busy_note = caption(card, _("Mientras imprime, sólo velocidad, flujo, ventilador y luz."));
    }
    if (c & P3D_CAP_EXTRUDE) {
        lv_obj_t *card = col_card(body, w);
        caption(card, _("Extrusor"));
        D.ext_row = row_box(card);
        lv_obj_t *br = row_box(card);
        p3d_btn(br, AOS_SYM_ARROW_DOWN_BOLD, _("Extruir"), AOS_C_CARD2, extrude_cb, (void *)(intptr_t)1);
        p3d_btn(br, AOS_SYM_ARROW_UP_BOLD, _("Retraer"), AOS_C_CARD2, extrude_cb, (void *)(intptr_t)-1);
    }
    if (c & (P3D_CAP_FAN | P3D_CAP_SPEED | P3D_CAP_FLOW | P3D_CAP_SPEED_LVL)) {
        lv_obj_t *card = col_card(body, w);
        if (c & P3D_CAP_FAN) slider(card, &D.fan_l, &D.fan_sl, P3D_C_FAN, 0, 100, D.st.fan >= 0 ? D.st.fan : 0);
        if (c & P3D_CAP_SPEED) slider(card, &D.speed_l, &D.speed_sl, P3D_C_SPEED, 10, 300, D.st.speed > 0 ? D.st.speed : 100);
        if (c & P3D_CAP_FLOW) slider(card, &D.flow_l, &D.flow_sl, P3D_C_FLOW, 50, 150, D.st.flow > 0 ? D.st.flow : 100);
        if (c & P3D_CAP_SPEED_LVL) {
            caption(card, _("Velocidad"));
            D.lvl_row = row_box(card);
        }
        slider_label();
    }
    if (c & (P3D_CAP_LIGHT | P3D_CAP_MOTORS_OFF | P3D_CAP_CONNECT | P3D_CAP_ESTOP)) {
        lv_obj_t *card = col_card(body, w);
        if (c & P3D_CAP_LIGHT) {
            lv_obj_t *r = p3d_box(card, lv_pct(100), 72);
            lv_obj_t *l = aos_label(r, _("Luz"), aos_font_body, AOS_C_TEXT);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
            aos_make_decorative(l);
            D.light_sw = lv_switch_create(r);
            lv_obj_set_size(D.light_sw, 96, 54);
            lv_obj_align(D.light_sw, LV_ALIGN_RIGHT_MID, 0, 0);
            if (D.st.light == 1) lv_obj_add_state(D.light_sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(D.light_sw, light_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
        lv_obj_t *br = row_box(card);
        if (c & P3D_CAP_MOTORS_OFF) p3d_btn(br, AOS_SYM_ENGINE, _("Soltar motores"), AOS_C_CARD2, simple_cb, (void *)(intptr_t)P3D_C_MOTORS_OFF);
        if (c & P3D_CAP_CONNECT) {
            bool linked = D.st.state != P3D_ST_OFFLINE;
            p3d_btn(br, linked ? AOS_SYM_LAN_DISCONNECT : AOS_SYM_LAN_CONNECT, linked ? _("Desconectar") : _("Conectar"),
                    AOS_C_CARD2, connect_cb, (void *)(intptr_t)(linked ? 0 : 1));
        }
        if (c & P3D_CAP_ESTOP) D.b_estop = p3d_btn(br, AOS_SYM_ALERT_OUTLINE, _("Parada de emergencia"), lv_color_hex(0x9B1C1C), estop_cb, NULL);
    }
    control_rows();
}

static void control_update(void)
{
    if (D.busy_note) {
        if (D.st.state == P3D_ST_PRINTING || D.st.state == P3D_ST_BUSY) lv_obj_remove_flag(D.busy_note, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(D.busy_note, LV_OBJ_FLAG_HIDDEN);
    }
    /* the machine's values, unless a finger is on the slider */
    lv_indev_t *in = lv_indev_active();
    lv_obj_t *held = in ? lv_indev_get_active_obj() : NULL;
    if (D.fan_sl && held != D.fan_sl && D.st.fan >= 0) lv_slider_set_value(D.fan_sl, D.st.fan, LV_ANIM_OFF);
    if (D.speed_sl && held != D.speed_sl && D.st.speed > 0) lv_slider_set_value(D.speed_sl, D.st.speed, LV_ANIM_OFF);
    if (D.flow_sl && held != D.flow_sl && D.st.flow > 0) lv_slider_set_value(D.flow_sl, D.st.flow, LV_ANIM_OFF);
    slider_label();
    if (D.light_sw && D.st.light >= 0 && held != D.light_sw) {
        if (D.st.light) lv_obj_add_state(D.light_sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(D.light_sw, LV_STATE_CHECKED);
    }
}

/* ---- Archivos ------------------------------------------------------------------------------------- */

static void files_show(void);

static void file_sel_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    D.sel = D.sel == i ? -1 : i;
    D.del_armed = 0;
    files_show();
}

static void file_print_cb(lv_event_t *e)
{
    (void)e;
    if (D.sel < 0 || D.sel >= D.nfiles) return;
    if (p3d_active(D.st.state)) {
        aos_ui_toast(_("Ya está imprimiendo"), 2000);
        return;
    }
    p3d_cmd_t c = { .op = P3D_C_START };
    p3d_copy(c.text, sizeof c.text, D.files[D.sel].path);
    p3d_send(A.detail, &c);
    aos_ui_toast(_("Empezando a imprimir"), 2000);
    D.sel = -1;
    D.tab = TAB_STATUS;
    D.want_tab = -1;
    build_tab();
}

static void file_del_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_current_target(e);
    if (D.sel < 0 || D.sel >= D.nfiles) return;
    uint64_t now = aos_hal_uptime_ms();
    if (now - D.del_armed > ARM_MS) {
        D.del_armed = now;
        lv_obj_t *l = lv_obj_get_child(b, 1);
        if (l) lv_label_set_text(l, _("Tocá otra vez para borrar"));
        return;
    }
    p3d_cmd_t c = { .op = P3D_C_DELETE };
    p3d_copy(c.text, sizeof c.text, D.files[D.sel].path);
    p3d_send(A.detail, &c);
    D.sel = -1;
}

static void files_refresh_cb(lv_event_t *e)
{
    (void)e;
    send_op(P3D_C_FILES);
}

static void fmt_size(char *out, size_t n, uint32_t b)
{
    if (b >= 1024 * 1024) snprintf(out, n, "%.1f MB", (double)b / (1024.0 * 1024.0));
    else if (b >= 1024) snprintf(out, n, "%u KB", (unsigned)(b / 1024));
    else if (b) snprintf(out, n, "%u B", (unsigned)b);
    else out[0] = '\0';
}

static void files_show(void)
{
    if (!D.flist) return;
    lv_obj_clean(D.flist);
    char err[96] = "";
    bool busy = false;
    int n = p3d_files(A.detail, NULL, 0, NULL, &busy, err, sizeof err);
    (void)n;
    if (busy && !D.nfiles) {
        caption(D.flist, _("Leyendo los archivos..."));
        return;
    }
    if (err[0]) {
        lv_obj_t *l = aos_label(D.flist, err, aos_font_body, AOS_C_ORANGE);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, lv_pct(100));
    }
    if (!D.nfiles && !busy && !err[0]) caption(D.flist, D.asked ? _("No hay archivos") : "");
    for (int i = 0; i < D.nfiles; i++) {
        const p3d_file_t *f = &D.files[i];
        bool sel = i == D.sel;
        lv_obj_t *r = p3d_box(D.flist, lv_pct(100), sel ? 190 : 100);
        lv_obj_set_style_radius(r, 18, 0);
        lv_obj_set_style_bg_color(r, sel ? AOS_C_CARD2 : AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, file_sel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char b[160], sz[24], est[32] = "";
        lv_obj_t *g = aos_label(r, AOS_SYM_FILE_DOCUMENT_OUTLINE, &aos_sym_44, AOS_C_ACCENT);
        lv_obj_set_pos(g, 16, 22);
        aos_make_decorative(g);
        aos_text_safe(b, sizeof b, f->name);
        lv_obj_t *nm = aos_label(r, b, aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(nm, D.cw - 120, lv_font_get_line_height(aos_font_body));
        lv_obj_set_pos(nm, 80, 14);
        aos_make_decorative(nm);
        fmt_size(sz, sizeof sz, f->size);
        if (f->est_s > 0) p3d_fmt_dur(est, sizeof est, f->est_s);
        snprintf(b, sizeof b, "%s%s%s", sz, sz[0] && est[0] ? "  ·  " : "", est);
        lv_obj_t *sub = aos_label(r, b, aos_font_caption, AOS_C_DIM);
        lv_obj_set_pos(sub, 80, 58);
        aos_make_decorative(sub);
        if (sel) {
            lv_obj_t *br = p3d_box(r, lv_pct(100), 84);
            lv_obj_set_pos(br, 16, 98);
            lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(br, 14, 0);
            p3d_btn(br, AOS_SYM_PLAY, _("Imprimir"), AOS_C_ACCENT, file_print_cb, NULL);
            if (D.caps & P3D_CAP_DELETE) p3d_btn(br, AOS_SYM_DELETE, _("Borrar"), lv_color_hex(0x9B1C1C), file_del_cb, NULL);
        }
    }
}

static void files_update(void)
{
    uint32_t gen;
    bool busy;
    p3d_files(A.detail, NULL, 0, &gen, &busy, NULL, 0);
    if (gen == D.files_gen) return;
    D.files_gen = gen;
    if (!D.files) D.files = p3d_psram(sizeof(p3d_file_t) * P3D_FILES_MAX);
    if (!D.files) return;
    D.nfiles = p3d_files(A.detail, D.files, P3D_FILES_MAX, NULL, &D.files_busy, NULL, 0);
    if (D.sel >= D.nfiles) D.sel = -1;
    files_show();
}

static void build_files(lv_obj_t *body)
{
    lv_obj_t *hr = p3d_box(body, D.cw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    caption(hr, _("Tocá un archivo para imprimirlo"));
    p3d_round_btn(hr, AOS_SYM_RESTART, files_refresh_cb, NULL);
    D.flist = p3d_box(body, D.cw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(D.flist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(D.flist, 10, 0);
    D.sel = -1;
    if (!D.asked) {
        D.asked = true;
        send_op(P3D_C_FILES);
    }
    D.files_gen = (uint32_t)-1;
    files_update();
    files_show();
}

/* ---- Cámara ---------------------------------------------------------------------------------------- */

static void cam_update(void);

static void cam_timer_cb(lv_timer_t *t)
{
    (void)t;
    cam_update();
}

static void cam_stop(void)
{
    if (D.cam_timer) lv_timer_delete(D.cam_timer);
    D.cam_timer = NULL;
    if (D.cam_img) lv_obj_add_flag(D.cam_img, LV_OBJ_FLAG_HIDDEN);
    if (D.cam) p3d_cam_stop(D.cam);
    D.cam = NULL;
    free(D.cam_px);
    D.cam_px = NULL;
    D.cam_gen = 0;
    D.cam_w = D.cam_h = 0;
}

static void cam_update(void)
{
    if (!D.cam || !D.cam_img) return;
    if (p3d_cam_take(D.cam, &D.cam_px, &D.cam_w, &D.cam_h, &D.cam_gen)) {
        lv_canvas_set_buffer(D.cam_img, D.cam_px, D.cam_w, D.cam_h, LV_COLOR_FORMAT_RGB565);
        lv_image_set_pivot(D.cam_img, D.cam_w / 2, D.cam_h / 2);
        int32_t bw = lv_obj_get_width(D.cam_box), bh = lv_obj_get_height(D.cam_box);
        /* the box takes the frame's shape (a 16:9 camera in a 4:3 box had bars),
         * within the room there is */
        int32_t want = bw * D.cam_h / D.cam_w;
        if (want > D.cam_max_h) want = D.cam_max_h;
        if (want != bh && want > 0) {
            lv_obj_set_height(D.cam_box, want);
            lv_obj_center(D.cam_img);
            bh = want;
        }
        uint32_t sx = (uint32_t)(256 * bw / D.cam_w), sy = (uint32_t)(256 * bh / D.cam_h);
        lv_image_set_scale(D.cam_img, sx < sy ? sx : sy);
        lv_obj_invalidate(D.cam_img);
    }
    char st[96];
    float fps;
    p3d_cam_status(D.cam, st, sizeof st, &fps);
    if (!st[0] && D.cam_px) snprintf(st, sizeof st, "%dx%d  ·  %.1f fps", D.cam_w, D.cam_h, (double)fps);
    p3d_set_text(D.cam_msg, st);
}

static void build_camera(lv_obj_t *body)
{
    p3d_cam_url(&A.cfg.m[A.detail], &D.st, D.cam_url, sizeof D.cam_url);
    int32_t w = A.land ? A.W - 2 * AOS_UI_PAD : D.cw;
    int32_t h = A.land ? A.H - 230 : w * 3 / 4;
    D.cam_max_h = A.land ? A.H - 230 : A.H - 330;
    D.cam_box = p3d_box(body, w, h);
    lv_obj_set_style_bg_color(D.cam_box, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(D.cam_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(D.cam_box, AOS_UI_RADIUS, 0);
    lv_obj_set_style_clip_corner(D.cam_box, true, 0);
    D.cam_img = lv_canvas_create(D.cam_box);
    lv_obj_center(D.cam_img);
    aos_make_decorative(D.cam_img);
    D.cam_msg = aos_label(body, "", aos_font_small, AOS_C_DIM);
    lv_label_set_long_mode(D.cam_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(D.cam_msg, w);
    if (!D.cam_url[0]) {
        p3d_set_text(D.cam_msg, _("Esta impresora no tiene cámara conocida. Se puede poner su dirección en el editor (Cámara)."));
        return;
    }
    D.cam = p3d_cam_start(D.cam_url, &A.cfg.m[A.detail], w, h);
    if (D.cam) D.cam_timer = lv_timer_create(cam_timer_cb, 40, NULL);
}

/* ---- Consola ---------------------------------------------------------------------------------------- */

static void con_send(const char *g)
{
    if (!g || !g[0]) return;
    p3d_cmd_t c = { .op = P3D_C_GCODE };
    p3d_copy(c.text, sizeof c.text, g);
    p3d_send(A.detail, &c);
}

#define BODY_TOP 196

static void con_kb_show(bool on)
{
    if (!D.kb) return;
    if (on) {
        lv_obj_remove_flag(D.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(D.body, A.H - BODY_TOP - lv_obj_get_height(D.kb));
        if (D.con_in) lv_obj_scroll_to_view(lv_obj_get_parent(D.con_in), LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(D.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(D.body, A.H - BODY_TOP);
    }
    if (D.con_scroll) lv_obj_scroll_to_y(D.con_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static void con_focus_cb(lv_event_t *e)
{
    (void)e;
    con_kb_show(true);
}

static void con_ready_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        con_send(lv_textarea_get_text(D.con_in));
        lv_textarea_set_text(D.con_in, "");
        return;
    }
    con_kb_show(false);
}

static void con_send_cb(lv_event_t *e)
{
    (void)e;
    con_send(lv_textarea_get_text(D.con_in));
    lv_textarea_set_text(D.con_in, "");
}

static void con_quick_cb(lv_event_t *e)
{
    con_send((const char *)lv_event_get_user_data(e));
}

static void con_update(void)
{
    if (!D.con) return;
    uint32_t gen;
    if (!D.con_buf) D.con_buf = p3d_psram(CON_MAX + 1);
    if (!D.con_buf) return;
    p3d_console(A.detail, D.con_buf, CON_MAX + 1, &gen);
    if (gen == D.con_gen && lv_label_get_text(D.con)[0]) return;
    D.con_gen = gen;
    char *safe = p3d_psram(CON_MAX + 64);
    if (!safe) return;
    aos_text_safe(safe, CON_MAX + 64, D.con_buf[0] ? D.con_buf : _("Lo que mandes aparece acá."));
    lv_label_set_text(D.con, safe);
    free(safe);
    lv_obj_update_layout(D.con_scroll);
    lv_obj_scroll_to_y(D.con_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static void build_console(lv_obj_t *body)
{
    int32_t w = D.cw;
    if (!(D.caps & P3D_CAP_REPLIES)) caption(body, _("Esta impresora no devuelve las respuestas: sólo se ve lo que se manda."));
    D.con_scroll = p3d_card(body, w, A.land ? A.H - 420 : 520);
    lv_obj_add_flag(D.con_scroll, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(D.con_scroll, LV_DIR_VER);
    lv_obj_set_style_pad_all(D.con_scroll, 20, 0);
    D.con = aos_label(D.con_scroll, "", &aos_mono_22, lv_color_hex(0xD1D1D6));
    lv_label_set_long_mode(D.con, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(D.con, lv_pct(100));
    aos_make_decorative(D.con);
    D.con_gen = (uint32_t)-1;

    lv_obj_t *ir = p3d_box(body, w, 84);
    D.con_in = lv_textarea_create(ir);
    lv_textarea_set_one_line(D.con_in, true);
    lv_textarea_set_max_length(D.con_in, 190);
    lv_textarea_set_placeholder_text(D.con_in, "G28");
    lv_obj_set_size(D.con_in, w - 110, 80);
    lv_obj_align(D.con_in, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(D.con_in, &aos_mono_22, 0);
    lv_obj_set_style_bg_color(D.con_in, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(D.con_in, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(D.con_in, 0, 0);
    lv_obj_set_style_radius(D.con_in, 20, 0);
    lv_obj_set_style_pad_hor(D.con_in, 20, 0);
    lv_obj_set_style_pad_ver(D.con_in, 22, 0);
    lv_obj_add_event_cb(D.con_in, con_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(D.con_in, con_focus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sb = p3d_round_btn(ir, AOS_SYM_SEND, con_send_cb, NULL);
    lv_obj_align(sb, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *qr = row_box(body);
    lv_obj_set_width(qr, w);
    static const char *const QUICK[] = { "G28", "M114", "M105", "M503", "M84", "M106 S255", "M107", "G29" };
    for (size_t i = 0; i < sizeof QUICK / sizeof QUICK[0]; i++) p3d_chip(qr, QUICK[i], false, con_quick_cb, (void *)QUICK[i]);

    D.kb = lv_keyboard_create(A.root);
    aos_keyboard_style(D.kb, aos_font_body);
    lv_obj_set_size(D.kb, A.W, A.land ? A.H * 45 / 100 : A.H * 32 / 100);
    lv_obj_align(D.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(D.kb, D.con_in);
    lv_keyboard_set_mode(D.kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_add_event_cb(D.kb, con_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(D.kb, con_ready_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(D.kb, LV_OBJ_FLAG_HIDDEN);
    p3d_want_console(A.detail, true);
    con_update();
}

/* ---- the tabs and the page ------------------------------------------------------------------------- */

static bool tab_on(int t)
{
    switch (t) {
    case TAB_STATUS: return true;
    case TAB_CONTROL:
        return (D.caps & (P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_EXTRUDE | P3D_CAP_FAN | P3D_CAP_SPEED | P3D_CAP_FLOW |
                          P3D_CAP_LIGHT | P3D_CAP_SPEED_LVL | P3D_CAP_CONNECT | P3D_CAP_MOTORS_OFF)) != 0;
    case TAB_FILES: return (D.caps & P3D_CAP_FILES) != 0;
    case TAB_CAMERA: return D.has_cam;
    case TAB_CONSOLE: return (D.caps & P3D_CAP_GCODE) != 0;
    }
    return false;
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == D.tab) return;
    D.tab = t;
    D.want_tab = -1;
    build_tab();
}

static void tabs_draw(void)
{
    lv_obj_clean(D.tabrow);
    for (int t = 0; t < TAB_N; t++) {
        if (!tab_on(t)) continue;
        lv_obj_t *c = p3d_chip(D.tabrow, _(TAB_NAMES[t]), t == D.tab, tab_cb, (void *)(intptr_t)t);
        lv_obj_set_style_pad_hor(c, 18, 0);
    }
}

static void clear_tab(void)
{
    sheet_close();
    cam_stop();
    if (D.tab != TAB_CONSOLE) p3d_want_console(A.detail, false);
    if (D.kb) lv_obj_delete(D.kb);
    D.kb = NULL;
    D.arc = D.pct = D.layer = D.file = D.t_el = D.t_rem = D.t_eta = D.state_l = D.msg = NULL;
    D.btnrow = D.b_pause = D.b_cancel = NULL;
    memset(D.heat_row, 0, sizeof D.heat_row);
    D.chart = NULL;
    D.info = D.trays = D.thumb = NULL;
    D.step_row = D.ext_row = D.fan_l = D.speed_l = D.flow_l = D.fan_sl = D.speed_sl = D.flow_sl = D.light_sw = NULL;
    D.lvl_row = D.busy_note = D.b_estop = NULL;
    D.flist = D.fmsg = NULL;
    D.cam_box = D.cam_img = D.cam_msg = NULL;
    D.con = D.con_scroll = D.con_in = NULL;
    if (D.body) lv_obj_set_height(D.body, A.H - BODY_TOP);
}

static void build_tab(void)
{
    clear_tab();
    if (D.want_tab >= 0 && tab_on(D.want_tab)) {
        D.tab = D.want_tab;
        D.want_tab = -1;
    }
    if (!tab_on(D.tab)) D.tab = TAB_STATUS;
    tabs_draw();
    lv_obj_clean(D.body);
    lv_obj_scroll_to_y(D.body, 0, LV_ANIM_OFF);
    switch (D.tab) {
    case TAB_CONTROL: build_control(D.body); control_update(); break;
    case TAB_FILES: build_files(D.body); break;
    case TAB_CAMERA: build_camera(D.body); break;
    case TAB_CONSOLE:
        p3d_want_console(A.detail, true);
        build_console(D.body);
        break;
    default: build_status(D.body); status_update(); break;
    }
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    p3d_go_list();
}

static void edit_cb(lv_event_t *e)
{
    (void)e;
    p3d_go_edit(A.detail);
}

static void header_update(void)
{
    char b[200];
    const p3d_cfg_t *c = &A.cfg.m[A.detail];
    snprintf(b, sizeof b, "%s  ·  %s%s%.40s", p3d_state_name(D.st.state), p3d_driver(c->kind)->label,
             D.st.model[0] ? "  ·  " : "", D.st.model);
    char safe[220];
    aos_text_safe(safe, sizeof safe, b);
    p3d_set_text(D.sub, safe);
    lv_obj_set_style_text_color(D.sub, p3d_state_color(D.st.state), 0);
}

void p3d_detail_build(void)
{
    int keep_tab = D.tab;
    bool asked = D.asked;
    p3d_file_t *files = D.files;
    char *con_buf = D.con_buf;
    memset(&D, 0, sizeof D);
    D.tab = keep_tab;
    D.want_tab = keep_tab;
    D.asked = asked;
    D.files = files;
    D.con_buf = con_buf;
    D.sel = -1;
    p3d_status_reset(&D.st);
    p3d_snapshot(A.detail, &D.st, &D.gen);
    D.caps = D.st.caps;
    char url[200];
    p3d_cam_url(&A.cfg.m[A.detail], &D.st, url, sizeof url);
    D.has_cam = url[0] != '\0';

    D.page = p3d_box(A.root, A.W, A.H);
    lv_obj_set_style_bg_color(D.page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(D.page, LV_OPA_COVER, 0);
    lv_obj_t *bk = p3d_round_btn(D.page, AOS_SYM_CHEVRON_LEFT, back_cb, NULL);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 14);
    lv_obj_t *ed = p3d_round_btn(D.page, AOS_SYM_PENCIL, edit_cb, NULL);
    lv_obj_align(ed, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 14);
    char safe[96];
    aos_text_safe(safe, sizeof safe, A.cfg.m[A.detail].name);
    int32_t tw = A.W - 2 * (AOS_UI_PAD + AOS_UI_TAP_MIN + 16);
    lv_obj_t *t = aos_label(D.page, safe, aos_font_title, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(t, tw, lv_font_get_line_height(aos_font_title));
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 16, 14);
    aos_make_decorative(t);
    D.sub = aos_label(D.page, "", aos_font_caption, AOS_C_DIM);
    lv_label_set_long_mode(D.sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(D.sub, tw, lv_font_get_line_height(aos_font_caption));
    lv_obj_align(D.sub, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 16, 64);
    aos_make_decorative(D.sub);

    D.tabrow = p3d_box(D.page, A.W, LV_SIZE_CONTENT);
    lv_obj_set_pos(D.tabrow, 0, 116);
    lv_obj_set_flex_flow(D.tabrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(D.tabrow, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_column(D.tabrow, 10, 0);
    lv_obj_add_flag(D.tabrow, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(D.tabrow, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(D.tabrow, LV_SCROLLBAR_MODE_OFF);

    D.body = p3d_box(D.page, A.W, A.H - BODY_TOP);
    lv_obj_set_pos(D.body, 0, BODY_TOP);
    lv_obj_add_flag(D.body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(D.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(D.body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(D.body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(D.body, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(D.body, 40, 0);
    lv_obj_set_style_pad_gap(D.body, 16, 0);
    int cols = A.land ? 2 : 1;
    D.cw = (A.W - 2 * AOS_UI_PAD - (cols - 1) * 16) / cols;

    header_update();
    build_tab();

    const char *tab = getenv("P3D_TAB");     /* the simulator's tests */
    if (tab && tab[0] >= '0' && tab[0] <= '4' && D.tab != tab[0] - '0') {
        D.want_tab = tab[0] - '0';
        build_tab();
    }
}

void p3d_detail_tick(void)
{
    if (!D.page) return;
    if (p3d_snapshot(A.detail, &D.st, &D.gen)) {
        char url[200];
        p3d_cam_url(&A.cfg.m[A.detail], &D.st, url, sizeof url);
        bool cam = url[0] != '\0';
        if (D.st.caps != D.caps || cam != D.has_cam) {
            /* what the machine can do is known now: other tabs */
            D.caps = D.st.caps;
            D.has_cam = cam;
            if (!D.sheet && D.tab != TAB_CONSOLE) build_tab();
            else tabs_draw();
        }
        header_update();
        if (D.tab == TAB_STATUS) status_update();
        else if (D.tab == TAB_CONTROL) control_update();
    }
    if (D.tab == TAB_STATUS && D.chart && aos_hal_uptime_ms() - D.chart_t > 5000) {
        D.chart_t = aos_hal_uptime_ms();
        chart_update();
    }
    if (D.tab == TAB_FILES) files_update();
    if (D.tab == TAB_CAMERA && !D.cam_timer) cam_update();
    if (D.tab == TAB_CONSOLE) con_update();
}

void p3d_detail_close(void)
{
    sheet_close();
    cam_stop();
    p3d_want_console(A.detail, false);
    if (D.kb) lv_obj_delete(D.kb);
    D.kb = NULL;
    if (D.thumb) lv_obj_add_flag(D.thumb, LV_OBJ_FLAG_HIDDEN);
    free(D.thumb_px);
    D.thumb_px = NULL;
    D.thumb = NULL;
    D.page = NULL;
    D.asked = false;
    D.nfiles = 0;
    D.tab = TAB_STATUS;
}

bool p3d_detail_back(void)
{
    if (D.sheet) {
        sheet_close();
        return true;
    }
    if (D.kb && !lv_obj_has_flag(D.kb, LV_OBJ_FLAG_HIDDEN)) {
        con_kb_show(false);
        return true;
    }
    if (D.tab != TAB_STATUS) {
        D.tab = TAB_STATUS;
        D.want_tab = -1;
        build_tab();
        return true;
    }
    return false;
}
