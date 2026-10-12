/*
 * P4OS - CAS: the calculator view.
 *
 * The history on top, each entry the input as typed (dim) and the result
 * the way a book prints it; the editor under it; the keypad at the bottom
 * (on the right, lying down). Tapping an input puts it back in the editor;
 * tapping a result inserts it where the cursor is.
 *
 * The history is a model (entries[]) apart from its widgets, so the view
 * can be rebuilt when the screen turns without losing anything.
 */
#include "cas_app.h"
#include "cas_keypad.h"
#include "cas_save.h"
#include "mx_view.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

#define HIST_MAX 60

typedef struct {
    mx_row_t *in;               /* what was typed                 */
    char     *res;              /* Giac's text, NULL while pending */
    bool      err;
    uint32_t  ms;
    lv_obj_t *box;              /* the entry's widgets, if built  */
    lv_obj_t *res_box;
} entry_t;

static struct {
    entry_t  *e[HIST_MAX];       /* each on its own: a pending job holds the pointer */
    int       n;
    mx_ed_t   ed;
    bool      ed_ok;
    char     *ans;              /* the last result's text          */
    bool      loaded;
    unsigned  ver;              /* changes with the history: the portal's page follows it */
    lv_obj_t *list, *edbox, *edview;
} C;

/* --- the history ------------------------------------------------------------ */

static void free_entry(entry_t *e)
{
    if (!e) {
        return;
    }
    cas_job_cancel(e);
    mx_row_free(e->in);
    mx_free(e->res);
    mx_free(e);
}

static void edit_changed(void);
void cas_calc_save(void);

static void input_tap(lv_obj_t *view, void *user)
{
    (void)view;
    entry_t *e = user;
    if (e->in) {
        mx_ed_set_row(&C.ed, mx_row_copy(e->in));
        edit_changed();
    }
}

static void result_tap(lv_obj_t *view, void *user)
{
    (void)user;
    const mx_row_t *r = mx_view_row(view);
    if (r) {
        mx_ed_insert_row(&C.ed, r);
        edit_changed();
    }
}

static void fill_result(entry_t *e)
{
    if (!e->res_box) {
        return;
    }
    lv_obj_clean(e->res_box);
    if (!e->res) {
        lv_obj_t *l = lv_label_create(e->res_box);
        lv_label_set_text(l, "\xE2\x80\xA6");
        lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
        lv_obj_set_style_text_font(l, mx_font(32), 0);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        return;
    }
    if (e->err) {
        lv_obj_t *l = lv_label_create(e->res_box);
        lv_label_set_text(l, e->res);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_color(l, AOS_C_RED, 0);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        return;
    }
    lv_obj_t *v = mx_view_create(e->res_box, 32);
    mx_view_set_row(v, mx_text_to_row(e->res));
    mx_view_on_tap(v, result_tap, e);
    /* right-aligned when it fits; when it does not, it scrolls sideways */
    lv_obj_update_layout(e->res_box);
    int cw = lv_obj_get_content_width(e->res_box);
    if (lv_obj_get_width(v) < cw) {
        lv_obj_set_width(v, cw);
        mx_view_set_align_right(v, true);
    }
}

static void build_entry(entry_t *e)
{
    lv_obj_t *box = lv_obj_create(C.list);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(box, 6, 0);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, AOS_C_CARD2, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    e->box = box;

    lv_obj_t *in_box = lv_obj_create(box);
    lv_obj_remove_style_all(in_box);
    lv_obj_set_size(in_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_scroll_dir(in_box, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(in_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *iv = mx_view_create(in_box, 28);
    mx_view_set_row(iv, mx_row_copy(e->in));
    mx_view_set_color(iv, AOS_C_DIM);
    mx_view_on_tap(iv, input_tap, e);

    e->res_box = lv_obj_create(box);
    lv_obj_remove_style_all(e->res_box);
    lv_obj_set_size(e->res_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_scroll_dir(e->res_box, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(e->res_box, LV_SCROLLBAR_MODE_OFF);
    fill_result(e);
}

static void job_done(void *user, const char *result, bool error, uint32_t ms)
{
    entry_t *e = user;
    mx_free(e->res);
    e->res = mx_strdup(result && *result ? result : (error ? _("Error") : ""));
    e->err = error;
    e->ms = ms;
    if (!error && result && *result) {
        mx_free(C.ans);
        C.ans = mx_strdup(result);
    }
    fill_result(e);
    if (C.list) {
        lv_obj_scroll_to_y(C.list, LV_COORD_MAX, LV_ANIM_ON);
    }
    C.ver++;
    cas_calc_save();
}

static void load(void);

static void submit_row(mx_row_t *in)
{
    if (!C.loaded) {
        load();                 /* the page can type before the view was ever shown */
    }
    C.ver++;
    char *text = mx_row_text(in);
    if (!text || !*text) {
        mx_free(text);
        mx_row_free(in);
        return;
    }
    if (C.n == HIST_MAX) {
        if (C.e[0]->box) {
            lv_obj_delete(C.e[0]->box);
        }
        free_entry(C.e[0]);
        memmove(&C.e[0], &C.e[1], sizeof(C.e[0]) * (HIST_MAX - 1));
        C.n--;
    }
    entry_t *e = mx_alloc(sizeof *e);
    if (!e) {
        mx_free(text);
        mx_row_free(in);
        return;
    }
    C.e[C.n++] = e;
    e->in = in;
    if (C.list) {
        build_entry(e);
        lv_obj_scroll_to_y(C.list, LV_COORD_MAX, LV_ANIM_ON);
    }
    if (!cas_job(text, job_done, e)) {
        job_done(e, _("El motor no está listo"), true, 0);
    }
    mx_free(text);
}

void cas_calc_submit_text(const char *expr)
{
    submit_row(mx_text_to_row(expr));
}

void cas_calc_clear(void)
{
    for (int i = 0; i < C.n; i++) {
        free_entry(C.e[i]);
        C.e[i] = NULL;
    }
    C.n = 0;
    C.ver++;
    if (C.list) {
        lv_obj_clean(C.list);
    }
    cas_calc_save();
}

unsigned cas_calc_version(void)
{
    return C.ver;
}

void cas_calc_json(mx_str_t *b)
{
    mx_str_addc(b, '[');
    for (int i = 0; i < C.n; i++) {
        entry_t *e = C.e[i];
        char *in = mx_row_text(e->in);
        mx_str_add(b, i ? ",{\"in\":" : "{\"in\":");
        cas_str_json(b, in);
        mx_str_add(b, ",\"out\":");
        cas_str_json(b, e->res);
        cas_str_printf(b, ",\"err\":%s,\"ms\":%u}", e->err ? "true" : "false", (unsigned)e->ms);
        mx_free(in);
    }
    mx_str_addc(b, ']');
}

/* --- the editor ---------------------------------------------------------------- */

static void edit_changed(void)
{
    if (!C.edview) {
        return;
    }
    mx_view_refresh(C.edview);
    lv_obj_update_layout(C.edbox);
    int bw = lv_obj_get_content_width(C.edbox);
    int cx = mx_view_cursor_x(C.edview);
    int sx = lv_obj_get_scroll_x(C.edbox);
    if (cx - sx > bw - 40) {
        lv_obj_scroll_to_x(C.edbox, cx - bw + 40, LV_ANIM_OFF);
    } else if (cx - sx < 20) {
        lv_obj_scroll_to_x(C.edbox, cx > 20 ? cx - 20 : 0, LV_ANIM_OFF);
    }
}

static void key_cb(const char *key, void *user)
{
    (void)user;
    if (!strcmp(key, "/enter")) {
        if (mx_row_empty(C.ed.root)) {
            return;
        }
        mx_row_t *in = C.ed.root;
        C.ed.root = NULL;
        mx_ed_set_row(&C.ed, NULL);
        submit_row(in);
    } else if (!strcmp(key, "/ans")) {
        if (C.ans) {
            mx_row_t *r = mx_text_to_row(C.ans);
            if (r) {
                mx_ed_insert_row(&C.ed, r);
                mx_row_free(r);
            }
        }
    } else {
        mx_ed_key(&C.ed, key);
    }
    edit_changed();
}

static void edbox_click(lv_event_t *e)
{
    /* a tap right of the expression: the cursor to the end */
    if (lv_event_get_target(e) == C.edbox) {
        mx_ed_key(&C.ed, "/end");
        edit_changed();
    }
}

static bool hw_key(uint32_t key, uint8_t mods)
{
    char k[8];
    if (!cas_hwkey_str(key, mods, k)) {
        return false;
    }
    key_cb(k, NULL);
    return true;
}

/* --- kept between openings ------------------------------------------------------
 * data/cas_hist.txt, one entry a line: input <TAB> result <TAB> error. The
 * input is kept as the text Giac read, and drawn again from it. */

static void hist_path(char *out, size_t n)
{
    snprintf(out, n, "%s/cas_hist.txt", aos_hal_path_data());
}

static void put_field(mx_str_t *b, const char *s)
{
    for (; s && *s; s++) {
        mx_str_addc(b, *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
    }
}

void cas_calc_save(void)
{
    char path[200];
    hist_path(path, sizeof path);
    mx_str_t b = { 0 };
    for (int i = 0; i < C.n; i++) {
        entry_t *e = C.e[i];
        if (!e->res) {
            continue;           /* still computing: not worth keeping */
        }
        char *in = mx_row_text(e->in);
        put_field(&b, in);
        mx_str_addc(&b, '\t');
        put_field(&b, e->res);
        mx_str_add(&b, e->err ? "\t1\n" : "\t0\n");
        mx_free(in);
    }
    cas_save_text(path, &b);
}

static void load(void)
{
    C.loaded = true;
    char path[200];
    hist_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    static char line[2048];
    while (C.n < HIST_MAX && fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2) {
            continue;
        }
        *t1 = *t2 = 0;
        entry_t *e = mx_alloc(sizeof *e);
        if (!e) {
            break;
        }
        e->in = mx_text_to_row(line);
        e->res = mx_strdup(t1 + 1);
        e->err = t2[1] == '1';
        C.e[C.n++] = e;
        if (!e->err) {
            mx_free(C.ans);
            C.ans = mx_strdup(e->res);
        }
    }
    fclose(f);
}

void cas_calc_build(lv_obj_t *parent, bool landscape)
{
    if (!C.loaded) {
        load();
    }
    if (!C.ed_ok) {
        mx_ed_init(&C.ed);
        C.ed_ok = true;
    }
    for (int i = 0; i < C.n; i++) {
        C.e[i]->box = C.e[i]->res_box = NULL;
    }
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(root, 10, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = root;
    if (landscape) {
        left = lv_obj_create(root);
        lv_obj_remove_style_all(left);
        lv_obj_set_height(left, LV_PCT(100));
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 10, 0);
        lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    }

    C.list = lv_obj_create(left);
    lv_obj_remove_style_all(C.list);
    lv_obj_set_width(C.list, LV_PCT(100));
    lv_obj_set_flex_grow(C.list, 1);
    lv_obj_set_flex_flow(C.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(C.list, 6, 0);
    lv_obj_set_scroll_dir(C.list, LV_DIR_VER);
    lv_obj_add_flag(C.list, LV_OBJ_FLAG_CLICKABLE);

    C.edbox = lv_obj_create(left);
    lv_obj_remove_style_all(C.edbox);
    lv_obj_set_size(C.edbox, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(C.edbox, 76, 0);
    lv_obj_set_style_max_height(C.edbox, 260, 0);
    lv_obj_set_style_bg_color(C.edbox, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(C.edbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(C.edbox, 16, 0);
    lv_obj_set_style_pad_all(C.edbox, 6, 0);
    lv_obj_set_style_border_width(C.edbox, 2, 0);
    lv_obj_set_style_border_color(C.edbox, AOS_C_ACCENT, 0);
    lv_obj_set_scrollbar_mode(C.edbox, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(C.edbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(C.edbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(C.edbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(C.edbox, edbox_click, LV_EVENT_CLICKED, NULL);
    C.edview = mx_view_create(C.edbox, 32);
    mx_view_set_editor(C.edview, &C.ed);

    lv_obj_t *kp = cas_keypad_create(root, key_cb, NULL);
    if (landscape) {
        lv_obj_set_size(kp, LV_PCT(46), LV_PCT(100));
    } else {
        lv_obj_set_size(kp, LV_PCT(100), LV_PCT(45));
    }

    for (int i = 0; i < C.n; i++) {
        build_entry(C.e[i]);
    }
    lv_obj_update_layout(root);
    lv_obj_scroll_to_y(C.list, LV_COORD_MAX, LV_ANIM_OFF);
    edit_changed();
    aos_ui_hwkbd_handler(hw_key);
}

void cas_calc_free(void)
{
    cas_calc_save();
    aos_ui_hwkbd_handler(NULL);
    cas_calc_clear();
    C.loaded = false;
    if (C.ed_ok) {
        mx_ed_free(&C.ed);
        C.ed_ok = false;
    }
    mx_free(C.ans);
    C.ans = NULL;
    C.list = C.edbox = C.edview = NULL;
}
