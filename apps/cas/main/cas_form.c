/*
 * P4OS - CAS: forms.
 *
 * The entry sheet floats on the top layer, over whichever view opened it,
 * with the same editor and keypad as the calculator: a field takes a value
 * written any way the keypad allows, and mx_text_value() reduces it to a
 * number without the engine.
 */
#include "cas_form.h"
#include "cas_keypad.h"
#include "mx_view.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- numbers -------------------------------------------------------------------- */

void cas_fmt(char *b, size_t n, double v, int digits)
{
    if (isnan(v)) {
        snprintf(b, n, "\xE2\x80\x94");
        return;
    }
    if (isinf(v)) {
        snprintf(b, n, "%s\xE2\x88\x9E", v < 0 ? "\xE2\x88\x92" : "");
        return;
    }
    char raw[48];
    double a = fabs(v);
    if (a != 0 && (a >= 1e12 || a < 1e-5)) {
        snprintf(raw, sizeof raw, "%.*g", digits, a);
    } else {
        /* fixed, with as many decimals as the significant digits allow */
        int intd = a >= 1 ? (int)floor(log10(a)) + 1 : 1;
        int dec = digits - intd;
        if (dec < 0) dec = 0;
        if (dec > 10) dec = 10;
        snprintf(raw, sizeof raw, "%.*f", dec, a);
        /* no trailing zeros after the point */
        if (strchr(raw, '.')) {
            char *e = raw + strlen(raw) - 1;
            while (*e == '0') *e-- = 0;
            if (*e == '.') *e = 0;
        }
    }
    size_t o = 0;
    if (v < 0 && n > 4) {
        memcpy(b, "\xE2\x88\x92", 3);
        o = 3;
    }
    /* spaces between thousands of the integer part */
    const char *dot = strchr(raw, '.');
    const char *e = strchr(raw, 'e');
    size_t ilen = dot ? (size_t)(dot - raw) : e ? (size_t)(e - raw) : strlen(raw);
    for (size_t i = 0; raw[i] && o + 4 < n; i++) {
        if (i < ilen && i > 0 && (ilen - i) % 3 == 0 && ilen > 4) {
            memcpy(b + o, "\xC2\xA0", 2);     /* a no-break space: the font has no thin one */
            o += 2;
        }
        b[o++] = raw[i];
    }
    b[o] = 0;
}

void cas_fmt_money(char *b, size_t n, double v)
{
    if (!isfinite(v)) {
        cas_fmt(b, n, v, 10);
        return;
    }
    char raw[48];
    snprintf(raw, sizeof raw, "%.2f", fabs(v));
    size_t o = 0;
    if (v < -0.004 && n > 4) {
        memcpy(b, "\xE2\x88\x92", 3);
        o = 3;
    }
    size_t ilen = (size_t)(strchr(raw, '.') - raw);
    for (size_t i = 0; raw[i] && o + 4 < n; i++) {
        if (i < ilen && i > 0 && (ilen - i) % 3 == 0) {
            memcpy(b + o, "\xC2\xA0", 2);     /* a no-break space: the font has no thin one */
            o += 2;
        }
        b[o++] = raw[i];
    }
    b[o] = 0;
}

/* --- the entry sheet ----------------------------------------------------------------- */

static struct {
    lv_obj_t      *scrim, *title, *edbox, *edview;
    mx_ed_t        ed;
    bool           ed_ok;
    bool           land;            /* the orientation it was built for */
    bool           shown;
    cas_entry_cb_t cb;
    void          *user;
} E;

static void entry_changed(void)
{
    if (E.edview) {
        mx_view_refresh(E.edview);
    }
}

static void entry_commit(void)
{
    if (mx_row_empty(E.ed.root)) {
        cas_entry_close();
        return;
    }
    char *text = mx_row_text(E.ed.root);
    double v;
    if (!mx_text_value(text, &v)) {
        aos_ui_toast(_("Eso no es un número"), 1500);
        mx_free(text);
        return;
    }
    bool keep = E.cb ? E.cb(E.user, text, v) : false;
    mx_free(text);
    if (keep) {
        mx_ed_clear(&E.ed);
        entry_changed();
    } else {
        cas_entry_close();
    }
}

static void entry_key(const char *key, void *user)
{
    (void)user;
    if (!strcmp(key, "/enter")) {
        entry_commit();
        return;
    }
    if (!strcmp(key, "/ans")) {
        return;
    }
    mx_ed_key(&E.ed, key);
    entry_changed();
}

static bool entry_hw(uint32_t key, uint8_t mods)
{
    char k[8];
    if (key == AOS_KEY_ESC) {
        cas_entry_close();
        return true;
    }
    if (!cas_hwkey_str(key, mods, k)) {
        return false;
    }
    entry_key(k, NULL);
    return true;
}

static void scrim_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        cas_entry_close();
    }
}

static void ok_cb(lv_event_t *e)
{
    (void)e;
    entry_commit();
}

/* The sheet is built once and then hidden and shown: building its keypad
 * took long enough on the board for the first taps after opening to be
 * lost. It is rebuilt only when the screen turned. */
static void build_sheet(bool land)
{
    if (E.scrim) {
        lv_obj_delete(E.scrim);
    }
    E.land = land;
    lv_obj_t *top = lv_layer_top();
    E.scrim = lv_obj_create(top);
    lv_obj_remove_style_all(E.scrim);
    lv_obj_set_size(E.scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(E.scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(E.scrim, LV_OPA_50, 0);
    lv_obj_add_flag(E.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(E.scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(E.scrim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, land ? 720 : LV_PCT(100), land ? LV_PCT(100) : LV_PCT(62));
    lv_obj_align(card, land ? LV_ALIGN_RIGHT_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111114), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, AOS_C_CARD2, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    E.title = lv_label_create(hdr);
    lv_obj_set_flex_grow(E.title, 1);
    lv_label_set_long_mode(E.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(E.title, aos_font_body, 0);
    lv_obj_set_style_text_color(E.title, AOS_C_TEXT, 0);
    lv_obj_t *ok = lv_button_create(hdr);
    lv_obj_set_style_bg_color(ok, AOS_C_ACCENT, 0);
    lv_obj_set_style_radius(ok, 14, 0);
    lv_obj_set_style_shadow_width(ok, 0, 0);
    lv_obj_set_style_pad_hor(ok, 22, 0);
    lv_obj_set_height(ok, 56);
    lv_obj_add_event_cb(ok, ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "OK");
    lv_obj_set_style_text_font(okl, aos_font_body, 0);
    lv_obj_set_style_text_color(okl, AOS_C_TEXT, 0);
    lv_obj_center(okl);

    E.edbox = lv_obj_create(card);
    lv_obj_remove_style_all(E.edbox);
    lv_obj_set_size(E.edbox, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(E.edbox, 76, 0);
    lv_obj_set_style_bg_color(E.edbox, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(E.edbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(E.edbox, 16, 0);
    lv_obj_set_style_border_width(E.edbox, 2, 0);
    lv_obj_set_style_border_color(E.edbox, AOS_C_ACCENT, 0);
    lv_obj_set_scrollbar_mode(E.edbox, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(E.edbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(E.edbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    E.edview = mx_view_create(E.edbox, 32);
    mx_view_set_editor(E.edview, &E.ed);

    lv_obj_t *kp = cas_keypad_create(card, entry_key, NULL);
    lv_obj_set_width(kp, LV_PCT(100));
    lv_obj_set_flex_grow(kp, 1);
}

void cas_entry_open(const char *title, double initial, bool has_initial, cas_entry_cb_t cb, void *user)
{
    if (!E.ed_ok) {
        mx_ed_init(&E.ed);
        E.ed_ok = true;
    }
    mx_ed_clear(&E.ed);
    if (has_initial && isfinite(initial)) {
        char b[40];
        snprintf(b, sizeof b, "%.12g", initial);
        mx_ed_set_row(&E.ed, mx_text_to_row(b));
    }
    E.cb = cb;
    E.user = user;
    lv_obj_t *top = lv_layer_top();
    bool land = lv_obj_get_width(top) > lv_obj_get_height(top);
    if (!E.scrim || land != E.land) {
        build_sheet(land);
    }
    lv_label_set_text(E.title, title);
    mx_view_refresh(E.edview);
    lv_obj_remove_flag(E.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(E.scrim);
    E.shown = true;
    aos_ui_hwkbd_handler(entry_hw);
}

void cas_entry_title(const char *title)
{
    if (E.title && E.shown) {
        lv_label_set_text(E.title, title);
    }
}

void cas_entry_close(void)
{
    if (E.scrim && E.shown) {
        lv_obj_add_flag(E.scrim, LV_OBJ_FLAG_HIDDEN);
        E.shown = false;
        E.cb = NULL;
        aos_ui_hwkbd_handler(NULL);
    }
}

void cas_entry_free(void)
{
    if (E.scrim) {
        lv_obj_delete(E.scrim);
    }
    E.scrim = E.title = E.edbox = E.edview = NULL;
    E.shown = false;
    E.cb = NULL;
    if (E.ed_ok) {
        mx_ed_free(&E.ed);
        E.ed_ok = false;
    }
}

/* --- the text sheet ---------------------------------------------------------------------- */

static struct {
    lv_obj_t     *scrim, *title, *ta, *kb;
    cas_text_cb_t cb;
    void         *user;
} T;

static void text_commit(void)
{
    cas_text_cb_t cb = T.cb;
    void *user = T.user;
    char *text = mx_strdup(T.ta ? lv_textarea_get_text(T.ta) : "");
    cas_text_close();
    if (cb && text) cb(user, text);
    mx_free(text);
}

static void text_close_async(void *u)
{
    (void)u;
    cas_text_close();
}

static void text_commit_async(void *u)
{
    (void)u;
    text_commit();
}

static void text_kb_cb(lv_event_t *e)
{
    /* the keyboard's own OK and close keys: after its event is over */
    if (lv_event_get_code(e) == LV_EVENT_READY) lv_async_call(text_commit_async, NULL);
    else lv_async_call(text_close_async, NULL);
}

static void text_ok_cb(lv_event_t *e)
{
    (void)e;
    lv_async_call(text_commit_async, NULL);
}

static void text_scrim_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) lv_async_call(text_close_async, NULL);
}

void cas_text_open(const char *title, const char *initial, bool multiline, cas_text_cb_t cb, void *user)
{
    cas_text_close();
    T.cb = cb;
    T.user = user;
    lv_obj_t *top = lv_layer_top();
    bool land = lv_obj_get_width(top) > lv_obj_get_height(top);
    T.scrim = lv_obj_create(top);
    lv_obj_remove_style_all(T.scrim);
    lv_obj_set_size(T.scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(T.scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(T.scrim, LV_OPA_50, 0);
    lv_obj_add_flag(T.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(T.scrim, text_scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(T.scrim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, land ? 860 : LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111114), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, AOS_C_CARD2, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    T.title = lv_label_create(hdr);
    lv_obj_set_flex_grow(T.title, 1);
    lv_label_set_long_mode(T.title, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(T.title, title);
    lv_obj_set_style_text_font(T.title, aos_font_body, 0);
    lv_obj_set_style_text_color(T.title, AOS_C_TEXT, 0);
    lv_obj_t *ok = lv_button_create(hdr);
    lv_obj_set_style_bg_color(ok, AOS_C_ACCENT, 0);
    lv_obj_set_style_radius(ok, 14, 0);
    lv_obj_set_style_shadow_width(ok, 0, 0);
    lv_obj_set_style_pad_hor(ok, 22, 0);
    lv_obj_set_height(ok, 56);
    lv_obj_add_event_cb(ok, text_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "OK");
    lv_obj_set_style_text_font(okl, aos_font_body, 0);
    lv_obj_center(okl);

    T.ta = lv_textarea_create(card);
    lv_obj_set_width(T.ta, LV_PCT(100));
    lv_textarea_set_one_line(T.ta, !multiline);
    if (multiline) lv_obj_set_height(T.ta, 160);
    lv_textarea_set_text(T.ta, initial ? initial : "");
    lv_obj_set_style_text_font(T.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(T.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(T.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_color(T.ta, AOS_C_ACCENT, 0);
    lv_obj_set_style_border_width(T.ta, 2, 0);
    lv_obj_set_style_radius(T.ta, 14, 0);
    lv_obj_add_state(T.ta, LV_STATE_FOCUSED);       /* the cursor shows */

    T.kb = lv_keyboard_create(card);
    lv_obj_set_size(T.kb, LV_PCT(100), land ? 300 : 380);
    lv_keyboard_set_mode(T.kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_textarea(T.kb, T.ta);
    lv_obj_set_style_text_font(T.kb, aos_font_body, 0);
    lv_obj_set_style_bg_color(T.kb, lv_color_hex(0x111114), 0);
    lv_obj_set_style_bg_color(T.kb, AOS_C_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(T.kb, AOS_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_width(T.kb, 0, 0);
    lv_obj_set_style_border_width(T.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(T.kb, 10, LV_PART_ITEMS);
    lv_obj_add_event_cb(T.kb, text_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(T.kb, text_kb_cb, LV_EVENT_CANCEL, NULL);
}

void cas_text_close(void)
{
    if (T.scrim) {
        lv_obj_delete(T.scrim);
    }
    T.scrim = T.title = T.ta = T.kb = NULL;
    T.cb = NULL;
}

void cas_text_free(void)
{
    lv_async_call_cancel(text_commit_async, NULL);
    lv_async_call_cancel(text_close_async, NULL);
    cas_text_close();
}

/* --- fields ---------------------------------------------------------------------------- */

typedef struct {
    double        *value;
    int            id;
    cas_field_cb_t cb;
    void          *user;
    lv_obj_t      *val;
    char           title[64];
} field_t;

static bool field_entered(void *user, const char *text, double v)
{
    (void)text;
    field_t *f = user;
    *f->value = v;
    char b[64];
    cas_fmt(b, sizeof b, v, 10);
    lv_label_set_text(f->val, b);
    if (f->cb) {
        f->cb(f->user, f->id);
    }
    return false;
}

static void field_click(lv_event_t *e)
{
    field_t *f = lv_event_get_user_data(e);
    cas_entry_open(f->title, *f->value, true, field_entered, f);
}

static void field_delete(lv_event_t *e)
{
    mx_free(lv_event_get_user_data(e));
}

static lv_obj_t *row_box(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 58, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *cas_field(lv_obj_t *parent, const char *label, double *value, int id,
                    cas_field_cb_t cb, void *user)
{
    field_t *f = mx_alloc(sizeof *f);
    lv_obj_t *row = row_box(parent);
    if (!f) {
        return row;
    }
    f->value = value;
    f->id = id;
    f->cb = cb;
    f->user = user;
    snprintf(f->title, sizeof f->title, "%s", label);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(l, aos_font_small, 0);

    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_set_style_min_width(b, 190, 0);
    lv_obj_add_event_cb(b, field_click, LV_EVENT_CLICKED, f);
    lv_obj_add_event_cb(row, field_delete, LV_EVENT_DELETE, f);
    f->val = lv_label_create(b);
    lv_obj_set_style_text_font(f->val, mx_font(28), 0);
    lv_obj_align(f->val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_user_data(row, f);
    cas_field_refresh(row);
    return row;
}

void cas_field_refresh(lv_obj_t *row)
{
    field_t *f = lv_obj_get_user_data(row);
    if (f) {
        char b[64];
        cas_fmt(b, sizeof b, *f->value, 10);
        lv_label_set_text(f->val, b);
    }
}

void cas_field_solve_button(lv_obj_t *row, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, 64, 50);
    lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, "=?");
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, AOS_C_ACCENT, 0);
    lv_obj_center(l);
}

/* --- segmented choice --------------------------------------------------------------------- */

typedef struct {
    cas_seg_cb_t cb;
    void        *user;
    int          n;
    lv_obj_t    *b[8];
} seg_t;

static void seg_paint(seg_t *s, int sel)
{
    for (int i = 0; i < s->n; i++) {
        lv_obj_set_style_bg_color(s->b[i], i == sel ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    }
}

static void seg_click(lv_event_t *e)
{
    seg_t *s = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_current_target(e);
    for (int i = 0; i < s->n; i++) {
        if (s->b[i] == b) {
            seg_paint(s, i);
            if (s->cb) s->cb(s->user, i);
            return;
        }
    }
}

lv_obj_t *cas_seg(lv_obj_t *parent, const char *const *labels, int n, int sel, cas_seg_cb_t cb, void *user)
{
    seg_t *s = mx_alloc(sizeof *s);
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (!s) {
        return row;
    }
    s->cb = cb;
    s->user = user;
    s->n = n > 8 ? 8 : n;
    lv_obj_add_event_cb(row, field_delete, LV_EVENT_DELETE, s);
    for (int i = 0; i < s->n; i++) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 50);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_add_event_cb(b, seg_click, LV_EVENT_CLICKED, s);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_font(l, mx_font(24), 0);
        lv_obj_center(l);
        s->b[i] = b;
    }
    seg_paint(s, sel);
    return row;
}

/* --- cards and results ----------------------------------------------------------------------- */

lv_obj_t *cas_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, aos_font_body, 0);
        lv_obj_set_style_text_color(t, AOS_C_TEXT, 0);
    }
    return c;
}

lv_obj_t *cas_result(lv_obj_t *parent, const char *label, lv_obj_t **value)
{
    lv_obj_t *row = row_box(parent);
    lv_obj_set_style_min_height(row, 44, 0);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "\xE2\x80\x94");
    lv_obj_set_style_text_font(v, mx_font(28), 0);
    if (value) {
        *value = v;
    }
    return row;
}
