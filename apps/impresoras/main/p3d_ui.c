/*
 * P4OS - 3D printers: UI pieces shared by the list, the detail and the
 * editor, and the words and colours of each state.
 */
#include "p3d.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"

#include <stdio.h>
#include <time.h>

lv_obj_t *p3d_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *p3d_round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = p3d_box(parent, AOS_UI_TAP_MIN, AOS_UI_TAP_MIN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, glyph, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return b;
}

lv_obj_t *p3d_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = p3d_box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? AOS_C_TEXT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return c;
}

lv_obj_t *p3d_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = p3d_box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

/* a wide button: a glyph and a word, on a colour */
lv_obj_t *p3d_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = p3d_box(parent, LV_SIZE_CONTENT, 80);
    lv_obj_set_style_radius(b, 40, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_28, AOS_C_TEXT));
    if (text) aos_make_decorative(aos_label(b, text, aos_font_body, AOS_C_TEXT));
    return b;
}

void p3d_set_text(lv_obj_t *l, const char *t)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t)) lv_label_set_text(l, t);
}

static const char *const STATE_NAMES[P3D_ST_N] = {
    [P3D_ST_OFFLINE]    = N_("Sin conexión"),
    [P3D_ST_CONNECTING] = N_("Conectando"),
    [P3D_ST_IDLE]       = N_("Lista"),
    [P3D_ST_BUSY]       = N_("Preparando"),
    [P3D_ST_PRINTING]   = N_("Imprimiendo"),
    [P3D_ST_PAUSED]     = N_("En pausa"),
    [P3D_ST_DONE]       = N_("Terminó"),
    [P3D_ST_CANCELLED]  = N_("Cancelada"),
    [P3D_ST_ERROR]      = N_("Error"),
};

const char *p3d_state_name(int st)
{
    return _(STATE_NAMES[st >= 0 && st < P3D_ST_N ? st : 0]);
}

lv_color_t p3d_state_color(int st)
{
    switch (st) {
    case P3D_ST_PRINTING: return AOS_C_ACCENT;
    case P3D_ST_BUSY: return AOS_C_TEAL;
    case P3D_ST_PAUSED: return AOS_C_ORANGE;
    case P3D_ST_DONE: return AOS_C_GREEN;
    case P3D_ST_ERROR: return AOS_C_RED;
    case P3D_ST_IDLE: return lv_color_hex(0xD1D1D6);
    case P3D_ST_CANCELLED: return AOS_C_YELLOW;
    default: return AOS_C_DIM;
    }
}

bool p3d_active(int st)
{
    return st == P3D_ST_PRINTING || st == P3D_ST_PAUSED || st == P3D_ST_BUSY;
}

void p3d_fmt_dur(char *out, size_t n, int32_t s)
{
    if (s < 0) {
        p3d_copy(out, n, "--");
        return;
    }
    int h = (int)(s / 3600), m = (int)(s % 3600 / 60);
    if (h >= 24) snprintf(out, n, "%d d %d h", h / 24, h % 24);
    else if (h) snprintf(out, n, "%d h %02d min", h, m);
    else if (m) snprintf(out, n, "%d min", m);
    else snprintf(out, n, "%d s", (int)s);
}

void p3d_fmt_eta(char *out, size_t n, int32_t remaining_s)
{
    out[0] = '\0';
    if (remaining_s <= 0 || !aos_hal_time_is_valid()) return;
    time_t t = time(NULL) + remaining_s;
    struct tm lt;
    localtime_r(&t, &lt);
    time_t now = time(NULL);
    struct tm ln;
    localtime_r(&now, &ln);
    if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
    else snprintf(out, n, "%s %02d:%02d", aos_day_name(lt.tm_wday), lt.tm_hour, lt.tm_min);
}

void p3d_fmt_pct(char *out, size_t n, float progress)
{
    /* a long print spends an hour under 1 %: a tenth shows that it moves.
     * Cut, never rounded up: 99.96 is not 100 yet. */
    if (progress < 0) {
        p3d_copy(out, n, "--");
    } else if (progress < 10) {
        int t = (int)(progress * 10);
        const char *lang = aos_i18n_current();
        char dot = lang && !strncmp(lang, "en", 2) ? '.' : ',';
        snprintf(out, n, "%d%c%d%%", t / 10, dot, t % 10);
    } else {
        snprintf(out, n, "%d%%", (int)progress);
    }
}

void p3d_fmt_temp(char *out, size_t n, const p3d_temp_t *t)
{
    if (!t->on) {
        p3d_copy(out, n, "--");
        return;
    }
    if (t->target > 0.5f) snprintf(out, n, "%d / %d°", (int)(t->cur + 0.5f), (int)(t->target + 0.5f));
    else snprintf(out, n, "%d°", (int)(t->cur + 0.5f));
}
