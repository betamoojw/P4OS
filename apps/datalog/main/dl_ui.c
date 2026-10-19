/*
 * P4OS - Datalog: small widgets, in Módulos' look, and the sheets that ask
 * for a text, a number, a time of day or a yes.
 *
 * A sheet is a dimmed layer over the whole app (U.overlay) with a card on
 * it; a tap on the dim closes it. One at a time.
 */
#include "dl.h"

/* strncpy that always terminates, and that GCC's format-truncation check
 * leaves alone */
void dl_scpy(char *d, size_t n, const char *s)
{
    if (!n) return;
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

lv_obj_t *dl_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *dl_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = dl_box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

/* A scrolling column; clickable, or a finger could not drag it */
lv_obj_t *dl_column(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 32, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

lv_obj_t *dl_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = dl_box(parent, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 12, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_28, lv_color_white()));
    if (text) aos_make_decorative(aos_label(b, text, aos_font_body, lv_color_white()));
    return b;
}

/* A round button with a glyph, for the bars */
lv_obj_t *dl_round(lv_obj_t *parent, const char *glyph, int32_t d, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = dl_box(parent, d, d);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 10);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *g = aos_label(b, glyph, d >= 80 ? &aos_sym_44 : &aos_sym_28, lv_color_white());
    lv_obj_center(g);
    aos_make_decorative(g);
    return b;
}

lv_obj_t *dl_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = dl_box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? DL_C_MAIN : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? lv_color_hex(0x042F2E) : AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return c;
}

lv_obj_t *dl_chips(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *r = dl_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    return r;
}

lv_obj_t *dl_caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

lv_obj_t *dl_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 10, 0);
    return t;
}

/* A settings-style row: the label, its value on the right and a chevron */
lv_obj_t *dl_row(lv_obj_t *parent, int32_t w, const char *label, const char *value, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = dl_card(parent, w, 88);
    lv_obj_set_style_pad_hor(r, 24, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *l = aos_label(r, label, aos_font_body, AOS_C_TEXT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = aos_label(r, value, aos_font_body, AOS_C_DIM);
    lv_obj_set_width(v, w / 2);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, cb ? -40 : 0, 0);
    if (cb) {
        lv_obj_t *g = aos_label(r, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(g, LV_ALIGN_RIGHT_MID, 6, 0);
    }
    aos_make_decorative(l);
    aos_make_decorative(v);
    return r;
}

/* ---- sheets ---- */

void dl_sheet_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
}

static void dim_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == U.overlay) dl_sheet_close();
}

static lv_obj_t *overlay(void)
{
    dl_sheet_close();
    U.overlay = dl_box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_70, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.overlay, dim_cb, LV_EVENT_CLICKED, NULL);
    return U.overlay;
}

lv_obj_t *dl_sheet_open(const char *title, int32_t w)
{
    lv_obj_t *ov = overlay();
    lv_obj_t *sh = dl_card(ov, w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(sh, U.H - 40, 0);
    lv_obj_add_flag(sh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(sh, LV_DIR_VER);
    lv_obj_set_style_pad_all(sh, 28, 0);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sh, 16, 0);
    lv_obj_center(sh);
    if (title) aos_label(sh, title, aos_font_title, AOS_C_TEXT);
    return sh;
}

static int32_t sheet_w(void) { return U.land ? 760 : U.W - 2 * AOS_UI_PAD; }

/* text and numbers: a field on top and the system's keyboard below */
static struct {
    lv_obj_t *ta;
    void (*text_done)(const char *);
    void (*num_done)(float);
} T;

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) { dl_sheet_close(); return; }
    if (code != LV_EVENT_READY || !T.ta) return;
    char buf[64];
    dl_scpy(buf, sizeof buf, lv_textarea_get_text(T.ta));
    void (*td)(const char *) = T.text_done;
    void (*nd)(float) = T.num_done;
    if (nd) {
        float v;
        if (!dl_parse_num(buf, &v)) { aos_ui_toast(_("No es un número"), 1500); return; }
        dl_sheet_close();
        T.ta = NULL;
        nd(v);
        return;
    }
    dl_sheet_close();
    T.ta = NULL;
    if (td) td(buf);
}

static void entry(const char *title, const char *value, int max, lv_keyboard_mode_t mode)
{
    lv_obj_t *ov = overlay();
    int32_t kb_h = U.land ? U.H / 2 : U.H * 2 / 5;
    lv_obj_t *card = dl_card(ov, sheet_w(), LV_SIZE_CONTENT);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, U.land ? 20 : 60);
    aos_label(card, title, aos_font_title, AOS_C_TEXT);
    T.ta = lv_textarea_create(card);
    lv_textarea_set_one_line(T.ta, true);
    lv_textarea_set_max_length(T.ta, (uint32_t)max);
    lv_textarea_set_text(T.ta, value);
    lv_obj_set_size(T.ta, LV_PCT(100), 88);
    lv_obj_set_style_text_font(T.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(T.ta, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(T.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(T.ta, 0, 0);
    lv_obj_set_style_radius(T.ta, 20, 0);
    lv_obj_set_style_pad_all(T.ta, 22, 0);
    lv_obj_add_state(T.ta, LV_STATE_FOCUSED);
    lv_obj_t *kb = lv_keyboard_create(ov);
    lv_obj_set_size(kb, U.W, kb_h);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(kb, aos_font_body);
    lv_keyboard_set_mode(kb, mode);
    lv_keyboard_set_textarea(kb, T.ta);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_ALL, NULL);
}

void dl_text_entry(const char *title, const char *value, int max, void (*done)(const char *))
{
    T.text_done = done;
    T.num_done = NULL;
    entry(title, value, max, LV_KEYBOARD_MODE_TEXT_LOWER);
}

void dl_num_entry(const char *title, float value, int dec, void (*done)(float))
{
    T.text_done = NULL;
    T.num_done = done;
    char t[32];
    dl_fmt_num(value, dec, t, sizeof t);
    if (!strcmp(t, "--")) t[0] = 0;
    entry(title, t, 14, LV_KEYBOARD_MODE_NUMBER);
}

/* a time of day: two rollers */
static struct {
    lv_obj_t *h, *m;
    void (*done)(int);
} R;

static void time_ok_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)lv_roller_get_selected(R.h) * 60 + (int)lv_roller_get_selected(R.m);
    void (*d)(int) = R.done;
    dl_sheet_close();
    if (d) d(v);
}

static lv_obj_t *roller(lv_obj_t *parent, int n, int sel)
{
    static char opts[60 * 3 + 1];
    size_t o = 0;
    for (int i = 0; i < n; i++) o += (size_t)snprintf(opts + o, sizeof opts - o, i ? "\n%02d" : "%02d", i);
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 4);
    lv_roller_set_selected(r, (uint32_t)sel, LV_ANIM_OFF);
    lv_obj_set_width(r, 150);
    lv_obj_set_style_text_font(r, aos_font_title, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(r, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_bg_color(r, DL_C_DARK, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, AOS_C_TEXT, LV_PART_SELECTED);
    return r;
}

void dl_time_entry(const char *title, int minutes, void (*done)(int))
{
    lv_obj_t *sh = dl_sheet_open(title, U.land ? 560 : U.W - 2 * AOS_UI_PAD);
    lv_obj_set_flex_align(sh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    R.done = done;
    lv_obj_t *row = dl_box(sh, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    R.h = roller(row, 24, minutes / 60 % 24);
    aos_label(row, ":", aos_font_title, AOS_C_TEXT);
    R.m = roller(row, 60, minutes % 60);
    dl_pill(sh, AOS_SYM_CHECK, _("Listo"), DL_C_DARK, time_ok_cb, NULL);
}

/* yes or no */
static void (*s_confirm)(void);

static void confirm_yes_cb(lv_event_t *e)
{
    (void)e;
    void (*d)(void) = s_confirm;
    dl_sheet_close();
    if (d) d();
}

static void confirm_no_cb(lv_event_t *e) { (void)e; dl_sheet_close(); }

void dl_confirm(const char *text, const char *yes, void (*done)(void))
{
    lv_obj_t *sh = dl_sheet_open(NULL, U.land ? 640 : U.W - 2 * AOS_UI_PAD);
    s_confirm = done;
    lv_obj_t *l = aos_label(sh, text, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *row = dl_box(sh, LV_PCT(100), 80);
    lv_obj_t *no = dl_pill(row, NULL, _("Cancelar"), AOS_C_CARD2, confirm_no_cb, NULL);
    lv_obj_align(no, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *ok = dl_pill(row, NULL, yes, lv_color_hex(0xB3261E), confirm_yes_cb, NULL);
    lv_obj_align(ok, LV_ALIGN_RIGHT_MID, 0, 0);
}
