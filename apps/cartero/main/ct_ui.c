/*
 * P4OS - Cartero: the pieces every screen is made of. Boxes, labels,
 * buttons and chips in the system's look; the keyboard, one per screen,
 * that shrinks the content above it; and the sheets that come up from the
 * bottom (a list of choices, a question with a text, a confirmation).
 *
 * A sheet is never deleted inside its own event: the choice hides it and an
 * async call deletes it and then calls back, so the callback may rebuild
 * the whole screen (HANDOFF-APPS, the double delete of a sheet).
 */
#include "ct_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- boxes and labels ---------------------------------------------------------------------- */

lv_obj_t *ct_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    /* not clickable unless it is made so: a box inside a row must not take
     * the row's tap (the rows, buttons and scrolled lists add the flag) */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ct_label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = aos_label(parent, t, f, c);
    aos_make_decorative(l);
    return l;
}

lv_obj_t *ct_label_dots(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c, int32_t w)
{
    lv_obj_t *l = ct_label(parent, t, f, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));
    return l;
}

lv_obj_t *ct_label_wrap(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = ct_label(parent, t, f, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

void ct_set_text(lv_obj_t *l, const char *t)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t)) lv_label_set_text(l, t);
}

lv_obj_t *ct_page(void)
{
    lv_obj_t *page = ct_box(CA.root, CA.W, CA.H);
    lv_obj_set_style_bg_color(page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    return page;
}

lv_obj_t *ct_round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ct_box(parent, AOS_UI_TAP_MIN, AOS_UI_TAP_MIN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = ct_label(b, glyph, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    return b;
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    /* the same path as the system's back gesture */
    if (CA.self && CA.self->back) CA.self->back(CA.self, &CA);
}

/* a title row: back (if asked), the title cut to fit, and room on the
 * right for the screen's own buttons (returned: a row, filled right to left) */
lv_obj_t *ct_header(lv_obj_t *page, const char *title, bool back)
{
    int32_t x = AOS_UI_PAD;
    if (back) {
        lv_obj_t *b = ct_round_btn(page, AOS_SYM_CHEVRON_LEFT, back_cb, NULL);
        lv_obj_set_pos(b, AOS_UI_PAD - 8, 16);
        x = AOS_UI_PAD + AOS_UI_TAP_MIN;
    }
    lv_obj_t *right = ct_box(page, LV_SIZE_CONTENT, AOS_UI_TAP_MIN);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 12, 0);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD + 8, 16);
    lv_obj_t *t = ct_label(page, title, aos_font_title, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    /* the right side is measured once its buttons are in: the title is
     * narrowed in ct_header_fit() */
    lv_obj_set_width(t, CA.W - x - AOS_UI_PAD - 3 * (AOS_UI_TAP_MIN + 12));
    lv_obj_set_pos(t, x, 34);
    lv_obj_set_user_data(right, t);
    return right;
}

lv_obj_t *ct_scroll(lv_obj_t *page, int32_t top)
{
    lv_obj_t *s = ct_box(page, CA.W, CA.H - top);
    lv_obj_set_pos(s, 0, top);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(s, 6, 0);
    lv_obj_set_style_pad_bottom(s, 60, 0);
    lv_obj_set_style_pad_row(s, 14, 0);
    return s;
}

lv_obj_t *ct_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ct_box(parent, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) ct_label(b, glyph, &aos_sym_28, AOS_C_TEXT);
    if (text) ct_label(b, text, aos_font_body, AOS_C_TEXT);
    return b;
}

lv_obj_t *ct_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = ct_box(parent, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_pad_hor(c, 20, 0);
    lv_obj_set_style_bg_color(c, on ? AOS_C_TEXT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = ct_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT);
    lv_obj_center(l);
    return c;
}

lv_obj_t *ct_card(lv_obj_t *parent)
{
    lv_obj_t *c = ct_box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    return c;
}

lv_obj_t *ct_textarea(lv_obj_t *parent, const char *text, bool one_line, bool mono)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, one_line);
    lv_textarea_set_text(ta, text ? text : "");
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, mono ? &aos_mono_22 : aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_border_width(ta, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, AOS_C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, 18, 0);
    lv_obj_set_style_pad_hor(ta, 20, 0);
    lv_obj_set_style_pad_ver(ta, 14, 0);
    lv_obj_set_style_text_color(ta, AOS_C_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_cursor_pos(ta, 0);
    ct_kb_watch(ta);
    return ta;
}

/* ---- colours and numbers ------------------------------------------------------------------- */

lv_color_t ct_method_color(const char *m)
{
    if (!strcasecmp(m, "GET")) return AOS_C_GREEN;
    if (!strcasecmp(m, "POST")) return AOS_C_ORANGE;
    if (!strcasecmp(m, "PUT")) return lv_color_hex(0x0A84FF);
    if (!strcasecmp(m, "PATCH")) return AOS_C_PURPLE;
    if (!strcasecmp(m, "DELETE")) return AOS_C_RED;
    if (!strcasecmp(m, "HEAD")) return AOS_C_TEAL;
    return AOS_C_DIM;
}

lv_color_t ct_status_color(int s)
{
    if (s >= 200 && s < 300) return AOS_C_GREEN;
    if (s >= 300 && s < 400) return AOS_C_TEAL;
    if (s >= 400 && s < 500) return AOS_C_ORANGE;
    if (s >= 500) return AOS_C_RED;
    return AOS_C_DIM;
}

lv_obj_t *ct_method_badge(lv_obj_t *parent, const char *m, int32_t w)
{
    char t[12];
    ct_copy(t, sizeof t, !strcasecmp(m, "DELETE") ? "DEL" : !strcasecmp(m, "OPTIONS") ? "OPT" : m);
    lv_obj_t *l = ct_label(parent, t, &aos_mono_22, ct_method_color(m));
    lv_obj_set_width(l, w);
    return l;
}

void ct_fmt_size(char *out, size_t n, long b)
{
    const char *lang = aos_i18n_current();
    char dot = lang && !strncmp(lang, "en", 2) ? '.' : ',';
    if (b < 1024) snprintf(out, n, "%ld B", b);
    else if (b < 1024 * 1024) {
        long t = b * 10 / 1024;
        if (t < 100) snprintf(out, n, "%ld%c%ld KB", t / 10, dot, t % 10);
        else snprintf(out, n, "%ld KB", b / 1024);
    } else {
        long t = b * 10 / (1024 * 1024);
        snprintf(out, n, "%ld%c%ld MB", t / 10, dot, t % 10);
    }
}

void ct_fmt_ms(char *out, size_t n, int ms)
{
    const char *lang = aos_i18n_current();
    char dot = lang && !strncmp(lang, "en", 2) ? '.' : ',';
    if (ms < 1000) snprintf(out, n, "%d ms", ms);
    else if (ms < 60000) snprintf(out, n, "%d%c%d s", ms / 1000, dot, ms % 1000 / 100);
    else snprintf(out, n, "%d min %d s", ms / 60000, ms % 60000 / 1000);
}

const char *ct_err_text(const ct_resp_t *r)
{
    switch (r->code) {
    case AOS_HTTP_ERR_DNS: return _("No se encontró el servidor (DNS)");
    case AOS_HTTP_ERR_CONNECT: return _("No se pudo conectar");
    case AOS_HTTP_ERR_SEND: return _("No se pudo enviar el pedido");
    case AOS_HTTP_ERR_RECV: return _("La respuesta se cortó");
    case AOS_HTTP_ERR_PROTO: return _("La respuesta no es HTTP");
    case AOS_HTTP_ERR_MEM: return _("No hay memoria");
    case AOS_HTTP_ERR_SIN_HORA: return _("La placa todavía no tiene hora: no puede verificar el certificado");
    case AOS_HTTP_ERR_TLS: return _("El certificado del servidor no se pudo verificar");
    case CT_ERR_URL: return _("La dirección no es http:// ni https://");
    case CT_ERR_TIMEOUT: return _("Se acabó el tiempo de espera");
    case CT_ERR_STOPPED: return _("Cancelado");
    case CT_ERR_TOOMANY: return _("Demasiadas redirecciones");
    default: return _("Falló el pedido");
    }
}

/* a row of chips where one is chosen; cb gets the index as user data */
lv_obj_t *ct_seg(lv_obj_t *parent, const char *const *labels, int n, int sel, lv_event_cb_t cb)
{
    lv_obj_t *row = ct_box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 10, 0);
    for (int i = 0; i < n; i++) ct_chip(row, labels[i], i == sel, cb, (void *)(intptr_t)i);
    return row;
}

/* ---- the keyboard ---------------------------------------------------------------------------- */

static struct {
    lv_obj_t *page, *content, *kb, *ta;
    int32_t   content_h;
} KB;

static void kb_done_cb(lv_event_t *e)
{
    (void)e;
    ct_kb_hide();
}

void ct_kb_attach(lv_obj_t *page, lv_obj_t *content)
{
    KB.page = page;
    KB.content = content;
    KB.content_h = content ? lv_obj_get_style_height(content, 0) : 0;
    KB.kb = lv_keyboard_create(page);
    aos_keyboard_style(KB.kb, aos_font_body);
    lv_obj_set_size(KB.kb, CA.W, CA.land ? CA.H * 46 / 100 : CA.H * 32 / 100);
    lv_obj_align(KB.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(KB.kb, kb_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(KB.kb, kb_done_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(KB.kb, LV_OBJ_FLAG_HIDDEN);
    KB.ta = NULL;
}

static void kb_show(lv_obj_t *ta)
{
    if (!KB.kb || !ta) return;
    KB.ta = ta;
    lv_keyboard_set_textarea(KB.kb, ta);
    lv_keyboard_set_mode(KB.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(KB.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(KB.kb);
    if (KB.content) {
        int32_t y = lv_obj_get_y(KB.content);
        lv_obj_set_height(KB.content, CA.H - y - lv_obj_get_height(KB.kb));
        lv_obj_update_layout(KB.content);
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    kb_show(lv_event_get_target(e));
}

void ct_kb_watch(lv_obj_t *ta)
{
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
}

void ct_kb_hide(void)
{
    if (!KB.kb) return;
    lv_obj_add_flag(KB.kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(KB.kb, NULL);
    if (KB.ta) lv_obj_remove_state(KB.ta, LV_STATE_FOCUSED);
    KB.ta = NULL;
    if (KB.content) lv_obj_set_height(KB.content, CA.H - lv_obj_get_y(KB.content));
}

bool ct_kb_visible(void)
{
    return KB.kb && !lv_obj_has_flag(KB.kb, LV_OBJ_FLAG_HIDDEN);
}

void ct_kb_forget(void)
{
    memset(&KB, 0, sizeof KB);
}

/* ---- sheets ------------------------------------------------------------------------------------ */

static struct {
    lv_obj_t    *ov, *ta, *kb;
    ct_pick_cb_t pick;
    ct_ask_cb_t  ask;
    void        *ud;
    int          choice;
    char        *text;
    bool         pending;
} SH;

static void fire(void *unused)
{
    (void)unused;
    SH.pending = false;
    lv_obj_t *ov = SH.ov;
    SH.ov = NULL;
    SH.ta = SH.kb = NULL;
    if (ov) lv_obj_delete(ov);
    ct_pick_cb_t pick = SH.pick;
    ct_ask_cb_t ask = SH.ask;
    char *text = SH.text;
    SH.text = NULL;
    SH.pick = NULL;
    SH.ask = NULL;
    if (ask && text) ask(text, SH.ud);
    else if (pick && SH.choice >= 0) pick(SH.choice, SH.ud);
    free(text);
}

static void finish(int choice)
{
    if (SH.pending) return;
    SH.choice = choice;
    if (SH.ov) lv_obj_add_flag(SH.ov, LV_OBJ_FLAG_HIDDEN);
    SH.pending = true;
    lv_async_call(fire, NULL);
}

void ct_sheet_close(void)
{
    if (!SH.ov || SH.pending) return;
    SH.pick = NULL;
    SH.ask = NULL;
    finish(-1);
}

bool ct_sheet_open(void)
{
    return SH.ov && !SH.pending;
}

/* the screen is rebuilt: the sheet went with it */
void ct_sheet_forget(void);
void ct_sheet_forget(void)
{
    SH.ov = SH.ta = SH.kb = NULL;
}

void ct_sheet_cancel_async(void);
void ct_sheet_cancel_async(void)
{
    if (SH.pending) lv_async_call_cancel(fire, NULL);
    SH.pending = false;
    free(SH.text);
    memset(&SH, 0, sizeof SH);
}

static void outside_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    SH.pick = NULL;
    SH.ask = NULL;
    finish(-1);
}

static lv_obj_t *overlay(void)
{
    if (SH.ov) lv_obj_delete(SH.ov);
    SH.ov = ct_box(CA.root, CA.W, CA.H);
    lv_obj_set_style_bg_color(SH.ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(SH.ov, LV_OPA_60, 0);
    lv_obj_add_flag(SH.ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(SH.ov, outside_cb, LV_EVENT_CLICKED, NULL);
    return SH.ov;
}

static lv_obj_t *panel(lv_obj_t *ov, int32_t max_h)
{
    int32_t w = CA.land ? CA.W * 60 / 100 : CA.W;
    lv_obj_t *p = ct_box(ov, w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(p, max_h, 0);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(p, 24, 0);
    lv_obj_set_style_pad_bottom(p, 40, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, AOS_UI_RADIUS);
    return p;
}

static void item_cb(lv_event_t *e)
{
    finish((int)(intptr_t)lv_event_get_user_data(e));
}

void ct_sheet(const char *title, const char *const *items, int n, int sel, ct_pick_cb_t cb, void *ud)
{
    if (SH.pending) return;
    ct_kb_hide();
    lv_obj_t *ov = overlay();
    lv_obj_t *p = panel(ov, CA.H * 80 / 100);
    SH.pick = cb;
    SH.ask = NULL;
    SH.ud = ud;
    if (title) ct_label_wrap(p, title, aos_font_small, AOS_C_DIM);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = ct_box(p, lv_pct(100), 84);
        lv_obj_set_style_radius(row, 20, 0);
        lv_obj_set_style_bg_color(row, i == sel ? AOS_C_CARD2 : AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        /* the panel's width is not laid out yet: the one it was given */
        int32_t pw = CA.land ? CA.W * 60 / 100 : CA.W;
        lv_obj_t *l = ct_label_dots(row, items[i], aos_font_body, i == sel ? AOS_C_ACCENT : AOS_C_TEXT, pw - 48 - 100);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 20, 0);
        if (i == sel) {
            lv_obj_t *c = ct_label(row, AOS_SYM_CHECK, &aos_sym_28, AOS_C_ACCENT);
            lv_obj_align(c, LV_ALIGN_RIGHT_MID, -20, 0);
        }
    }
}

static void ask_ok_cb(lv_event_t *e)
{
    (void)e;
    if (!SH.ta) return;
    const char *t = lv_textarea_get_text(SH.ta);
    while (*t == ' ') t++;
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') n--;
    free(SH.text);
    SH.text = malloc(n + 1);
    if (SH.text) {
        memcpy(SH.text, t, n);
        SH.text[n] = '\0';
    }
    finish(0);
}

static void ask_cancel_cb(lv_event_t *e)
{
    (void)e;
    SH.ask = NULL;
    finish(-1);
}

static void ask_kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) ask_ok_cb(e);
    else ask_cancel_cb(e);
}

void ct_ask(const char *title, const char *initial, ct_ask_cb_t cb, void *ud)
{
    if (SH.pending) return;
    ct_kb_hide();
    lv_obj_t *ov = overlay();
    SH.ask = cb;
    SH.pick = NULL;
    SH.ud = ud;
    int32_t kbh = CA.land ? CA.H * 50 / 100 : CA.H * 32 / 100;
    lv_obj_t *p = ct_box(ov, CA.land ? CA.W * 70 / 100 : CA.W - 2 * AOS_UI_PAD, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(p, 24, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 14, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    ct_label_wrap(p, title, aos_font_body, AOS_C_TEXT);
    SH.ta = lv_textarea_create(p);
    lv_textarea_set_one_line(SH.ta, true);
    lv_textarea_set_text(SH.ta, initial ? initial : "");
    lv_obj_set_width(SH.ta, lv_pct(100));
    lv_obj_set_style_text_font(SH.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(SH.ta, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(SH.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(SH.ta, 0, 0);
    lv_obj_set_style_radius(SH.ta, 16, 0);
    lv_obj_set_style_pad_all(SH.ta, 14, 0);
    lv_obj_add_state(SH.ta, LV_STATE_FOCUSED);
    lv_obj_t *row = ct_box(p, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    ct_btn(row, NULL, _("Cancelar"), AOS_C_CARD2, ask_cancel_cb, NULL);
    ct_btn(row, AOS_SYM_CHECK, _("Listo"), AOS_C_ACCENT, ask_ok_cb, NULL);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, CA.land ? 20 : 120);
    SH.kb = lv_keyboard_create(ov);
    aos_keyboard_style(SH.kb, aos_font_body);
    lv_obj_set_size(SH.kb, CA.W, kbh);
    lv_obj_align(SH.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(SH.kb, SH.ta);
    lv_obj_add_event_cb(SH.kb, ask_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(SH.kb, ask_kb_cb, LV_EVENT_CANCEL, NULL);
}

static void confirm_cb(lv_event_t *e)
{
    finish((int)(intptr_t)lv_event_get_user_data(e));
}

void ct_confirm(const char *title, const char *yes, ct_pick_cb_t cb, void *ud)
{
    if (SH.pending) return;
    ct_kb_hide();
    lv_obj_t *ov = overlay();
    lv_obj_t *p = panel(ov, CA.H * 60 / 100);
    SH.pick = cb;
    SH.ask = NULL;
    SH.ud = ud;
    ct_label_wrap(p, title, aos_font_body, AOS_C_TEXT);
    lv_obj_t *row = ct_box(p, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_margin_top(row, 10, 0);
    ct_btn(row, NULL, _("Cancelar"), AOS_C_CARD2, confirm_cb, (void *)(intptr_t)-1);
    ct_btn(row, AOS_SYM_DELETE, yes, lv_color_hex(0x9B1C1C), confirm_cb, (void *)(intptr_t)1);
}

/* the title of a header narrowed to the room its buttons left */
void ct_header_fit(lv_obj_t *right)
{
    lv_obj_t *t = lv_obj_get_user_data(right);
    if (!t) return;
    lv_obj_update_layout(right);
    int32_t w = lv_obj_get_x(right) - lv_obj_get_x(t) - 12;
    if (w > 40) lv_obj_set_width(t, w);
}
