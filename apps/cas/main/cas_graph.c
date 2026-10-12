/*
 * P4OS - CAS: the grapher.
 *
 * Up to six functions of x, drawn on a canvas that takes all the room the
 * function list leaves. One finger pans, two pinch to zoom around the point
 * between them, a double tap zooms in there, a long press (or a tap on a
 * curve) traces it: a dot on the curve and its coordinates, which follow
 * the finger.
 *
 * The curves are evaluated by mx_compile()'s code, one sample per pixel
 * column, and rasterised here straight into the canvas' RGB565 buffer with
 * anti-aliased thick lines: a full redraw is a few milliseconds, so panning
 * redraws on every frame. A function the evaluator does not know (diff(),
 * a function defined in the calculator...) is handed to Giac first, and
 * what comes back, an explicit expression, is compiled instead. Every
 * function is also defined in Giac as f1(x)..f6(x), so the calculator can
 * use them.
 */
#include "cas_app.h"
#include "cas_keypad.h"
#include "cas_plot.h"
#include "mx_view.h"

#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NF 6

static const uint32_t COLORS[NF] = { 0x0A84FF, 0xFF453A, 0x30D158, 0xFF9F0A, 0xBF5AF2, 0x40C8E0 };

typedef struct {
    mx_row_t  *row;             /* as typed                                    */
    mx_prog_t  prog;
    bool       ok;              /* compiled                                    */
    bool       waiting;         /* asked Giac for an explicit form             */
    bool       on;
    char       err[48];
} fn_t;

static struct {
    fn_t        f[NF];
    bool        init;
    /* the window: centre and pixels per unit */
    float       cx, cy, scale;
    /* trace */
    int         tf;             /* -1 none */
    float       tx;
    /* widgets */
    lv_obj_t   *canvas, *box, *panel, *list, *pill, *editbox, *edview, *kp;
    cas_plot_t  plot;
    int         W, H;
    bool        dirty, shown, landscape;
    lv_timer_t *timer;
    int         editing;        /* function being edited, -1 none */
    mx_ed_t     ed;
    bool        ed_ok;
} G;

static void list_build(void);
static void save(void);

/* --- the model ----------------------------------------------------------------- */

static void define_in_cas(int i);
static void engine_cb(void *user, const char *result, bool error, uint32_t ms);

static void compile(int i)
{
    fn_t *f = &G.f[i];
    mx_prog_free(&f->prog);
    f->ok = false;
    f->err[0] = 0;
    if (mx_row_empty(f->row)) {
        return;
    }
    char *text = mx_row_text(f->row);
    const char *perr = NULL;
    mx_ast_t *t = mx_parse(text, &perr);
    if (t && mx_compile(t, "x", &f->prog)) {
        f->ok = true;
    } else if (t && cas_engine_ok()) {
        /* something only Giac knows: ask it for an explicit expression */
        char buf[600];
        snprintf(buf, sizeof buf, "simplify(%.580s)", text);
        f->waiting = cas_job(buf, engine_cb, &G.f[i]);
    } else {
        snprintf(f->err, sizeof f->err, "%s", t ? _("no se puede graficar") : _("no se entiende"));
    }
    mx_ast_free(t);
    mx_free(text);
    define_in_cas(i);
}

static void engine_cb(void *user, const char *result, bool error, uint32_t ms)
{
    (void)ms;
    fn_t *f = user;
    f->waiting = false;
    mx_ast_t *t = error ? NULL : mx_parse(result, NULL);
    if (t && mx_compile(t, "x", &f->prog)) {
        f->ok = true;
        f->err[0] = 0;
    } else {
        snprintf(f->err, sizeof f->err, "%s", _("no se puede graficar"));
    }
    mx_ast_free(t);
    G.dirty = true;
    if (G.list) {
        list_build();
    }
}

static void define_cb(void *user, const char *result, bool error, uint32_t ms)
{
    (void)user; (void)result; (void)error; (void)ms;
}

static void define_in_cas(int i)
{
    if (!cas_engine_ok()) {
        return;
    }
    char head[24];
    snprintf(head, sizeof head, "f%d(x):=", (i + 1) % 10);
    char *text = mx_row_empty(G.f[i].row) ? NULL : mx_row_text(G.f[i].row);
    mx_str_t b = { 0 };
    mx_str_add(&b, head);
    mx_str_add(&b, text ? text : "undef");
    cas_job(b.s, define_cb, NULL);
    mx_str_free(&b);
    mx_free(text);
}

/* --- kept between openings -----------------------------------------------------
 * data/cas_graph.txt: "W cx cy scale", then "F i on text" per function. */

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/cas_graph.txt", aos_hal_path_data());
}

static void save(void)
{
    if (!G.init) {
        return;
    }
    char path[200];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "W %g %g %g\n", (double)G.cx, (double)G.cy, (double)G.scale);
    for (int i = 0; i < NF; i++) {
        if (mx_row_empty(G.f[i].row)) {
            continue;
        }
        char *t = mx_row_text(G.f[i].row);
        fprintf(f, "F %d %d %s\n", i, G.f[i].on ? 1 : 0, t);
        mx_free(t);
    }
    fclose(f);
}

static void load(void)
{
    char path[200];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    static char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'W') {
            float cx, cy, sc;
            if (sscanf(line + 1, "%f %f %f", &cx, &cy, &sc) == 3 && sc > 0) {
                G.cx = cx;
                G.cy = cy;
                G.scale = sc;
            }
        } else if (line[0] == 'F') {
            int i = line[2] - '0', on = line[4] - '0';
            if (i < 0 || i >= NF || line[1] != ' ' || line[3] != ' ' || line[5] != ' ') {
                continue;
            }
            mx_row_free(G.f[i].row);
            G.f[i].row = mx_text_to_row(line + 6);
            G.f[i].on = on == 1;
            compile(i);
        }
    }
    fclose(f);
}

static void model_init(void)
{
    if (G.init) {
        return;
    }
    G.init = true;
    for (int i = 0; i < NF; i++) {
        G.f[i].row = mx_row_new();
        G.f[i].on = true;
    }
    G.cx = 0;
    G.cy = 0;
    G.scale = 60;
    G.tf = -1;
    G.editing = -1;
    load();
}

/* --- the window ----------------------------------------------------------------- */

static float sx_to_x(float sx) { return G.cx + (sx - G.W / 2.0f) / G.scale; }
static float x_to_sx(float x)  { return G.W / 2.0f + (x - G.cx) * G.scale; }
static float y_to_sy(float y)  { return G.H / 2.0f - (y - G.cy) * G.scale; }

/* how long a redraw takes, in the log every few seconds of use: the number
 * that says whether panning keeps up with the finger */
static struct {
    uint32_t n, sum_us, max_us, since;
} s_perf;

static void perf_note(uint32_t us)
{
    s_perf.n++;
    s_perf.sum_us += us;
    if (us > s_perf.max_us) s_perf.max_us = us;
    uint32_t now = aos_hal_uptime_ms();
    if (now - s_perf.since >= 3000) {
        if (s_perf.n > 1) {
            aos_hal_log("cas", "graph: %u redraws, %u.%u ms avg, %u.%u ms max (%dx%d)",
                        (unsigned)s_perf.n, (unsigned)(s_perf.sum_us / s_perf.n / 1000),
                        (unsigned)(s_perf.sum_us / s_perf.n / 100 % 10),
                        (unsigned)(s_perf.max_us / 1000), (unsigned)(s_perf.max_us / 100 % 10), G.W, G.H);
        }
        memset(&s_perf, 0, sizeof s_perf);
        s_perf.since = now;
    }
}

static void redraw(void)
{
    if (!G.canvas || !G.plot.db) {
        return;
    }
    G.dirty = false;
    uint64_t t0 = aos_hal_uptime_us();
    cas_plot_t *p = &G.plot;
    p->x0 = sx_to_x(0);
    p->x1 = sx_to_x((float)G.W);
    p->y0 = G.cy - G.H / 2.0f / G.scale;
    p->y1 = G.cy + G.H / 2.0f / G.scale;
    cas_plot_clear(p, 0x000000);
    cas_plot_grid(p);

    /* curves */
    for (int i = 0; i < NF; i++) {
        fn_t *f = &G.f[i];
        if (!f->ok || !f->on) continue;
        float px = 0, py = 0;
        bool have = false;
        for (int s = 0; s <= G.W; s++) {
            float x = sx_to_x((float)s);
            float y = mx_eval(&f->prog, x);
            if (!isfinite(y)) {
                have = false;
                continue;
            }
            float sy = y_to_sy(y);
            if (sy < -4 * G.H) sy = -4.0f * G.H;
            if (sy > 5 * G.H) sy = 5.0f * G.H;
            if (have) {
                /* an asymptote: a jump across the whole screen is not a line */
                bool jump = fabsf(sy - py) > G.H && (sy < 0 || sy > G.H || py < 0 || py > G.H);
                if (!jump) cas_plot_line(p, px, py, (float)s, sy, COLORS[i], 3.2f);
            }
            px = (float)s;
            py = sy;
            have = true;
        }
    }

    /* the traced point */
    char pill[96] = "";
    if (G.tf >= 0 && G.f[G.tf].ok) {
        float y = mx_eval(&G.f[G.tf].prog, G.tx);
        if (isfinite(y)) {
            float sx = x_to_sx(G.tx), sy = y_to_sy(y);
            cas_plot_disc(p, sx, sy, 9, 0xFFFFFF);
            cas_plot_disc(p, sx, sy, 6, COLORS[G.tf]);
            snprintf(pill, sizeof pill, "f%d   x = %.5g   y = %.5g", (G.tf + 1) % 10, (double)G.tx, (double)y);
        }
    }

    lv_layer_t layer;
    lv_canvas_init_layer(G.canvas, &layer);
    cas_plot_numbers(p, &layer);
    lv_canvas_finish_layer(G.canvas, &layer);
    cas_plot_done(p);
    perf_note((uint32_t)(aos_hal_uptime_us() - t0));

    if (G.pill) {
        if (pill[0]) {
            lv_label_set_text(G.pill, pill);
            lv_obj_remove_flag(G.pill, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(G.pill, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (G.dirty && G.shown) {
        redraw();
    }
}

/* --- gestures ------------------------------------------------------------------------ */

static int nearest_curve(float sx, float sy, float maxd)
{
    int best = -1;
    float bd = maxd;
    float x = sx_to_x(sx);
    for (int i = 0; i < NF; i++) {
        if (!G.f[i].ok || !G.f[i].on) continue;
        float y = mx_eval(&G.f[i].prog, x);
        if (!isfinite(y)) continue;
        float d = fabsf(y_to_sy(y) - sy);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void local(const aos_gesture_event_t *ev, float *x, float *y)
{
    lv_area_t a;
    lv_obj_get_coords(G.canvas, &a);
    *x = ev->x - a.x1;
    *y = ev->y - a.y1;
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    float x, y;
    switch (ev->type) {
    case AOS_GESTURE_DRAG:
        if (ev->after_long && G.tf >= 0) {
            local(ev, &x, &y);
            G.tx = sx_to_x(x);
        } else {
            G.cx -= ev->dx / G.scale;
            G.cy += ev->dy / G.scale;
        }
        G.dirty = true;
        break;
    case AOS_GESTURE_PINCH: {
        local(ev, &x, &y);
        float mx = sx_to_x(x), my = G.cy + (G.H / 2.0f - y) / G.scale;
        float s = G.scale * ev->scale;
        if (s < 1e-3f) s = 1e-3f;
        if (s > 1e6f) s = 1e6f;
        G.scale = s;
        /* the point between the fingers stays under them, and follows them */
        G.cx = mx - (x + ev->dx - G.W / 2.0f) / G.scale;
        G.cy = my - (G.H / 2.0f - (y + ev->dy)) / G.scale;
        G.dirty = true;
        break;
    }
    case AOS_GESTURE_DOUBLE_TAP: {
        local(ev, &x, &y);
        float mx = sx_to_x(x), my = G.cy + (G.H / 2.0f - y) / G.scale;
        G.scale *= 2;
        G.cx = mx - (x - G.W / 2.0f) / G.scale;
        G.cy = my - (G.H / 2.0f - y) / G.scale;
        G.dirty = true;
        break;
    }
    case AOS_GESTURE_LONG_PRESS:
    case AOS_GESTURE_TAP: {
        local(ev, &x, &y);
        int f = nearest_curve(x, y, ev->type == AOS_GESTURE_TAP ? 40 : 4000);
        if (f >= 0) {
            G.tf = f;
            G.tx = sx_to_x(x);
        } else if (ev->type == AOS_GESTURE_TAP) {
            G.tf = -1;
        }
        G.dirty = true;
        break;
    }
    default:
        break;
    }
}

/* --- the function list and its editor ------------------------------------------------- */

static void edit_open(int i);

static void row_tap(lv_obj_t *view, void *user)
{
    (void)view;
    edit_open((int)(intptr_t)user);
}

static void dot_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    G.f[i].on = !G.f[i].on;
    G.dirty = true;
    save();
    list_build();
}

static void row_click(lv_event_t *e)
{
    edit_open((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *fn_label(lv_obj_t *parent, int i)
{
    char b[24];
    snprintf(b, sizeof b, "f%d(x) =", (i + 1) % 10);
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, b);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLORS[i]), 0);
    return l;
}

static void list_build(void)
{
    if (!G.list) {
        return;
    }
    lv_obj_clean(G.list);
    int last = 0;
    for (int i = 0; i < NF; i++) {
        if (!mx_row_empty(G.f[i].row)) last = i + 1;
    }
    int rows = last < NF ? last + 1 : NF;
    for (int i = 0; i < rows; i++) {
        fn_t *f = &G.f[i];
        lv_obj_t *row = lv_obj_create(G.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 64, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 34, 34);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 3, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(COLORS[i]), 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COLORS[i]), 0);
        lv_obj_set_style_bg_opa(dot, f->on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_ext_click_area(dot, 12);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(dot, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        fn_label(row, i);
        if (mx_row_empty(f->row)) {
            lv_obj_t *hint = lv_label_create(row);
            lv_label_set_text(hint, _("tocá para escribir"));
            lv_obj_set_style_text_color(hint, AOS_C_DIM, 0);
            lv_obj_set_style_text_font(hint, aos_font_small, 0);
        } else {
            lv_obj_t *v = mx_view_create(row, 28);
            mx_view_set_row(v, mx_row_copy(f->row));
            mx_view_on_tap(v, row_tap, (void *)(intptr_t)i);
            if (f->err[0] || f->waiting) {
                lv_obj_t *e = lv_label_create(row);
                lv_label_set_text(e, f->waiting ? "\xE2\x80\xA6" : f->err);
                lv_obj_set_style_text_color(e, f->waiting ? AOS_C_DIM : AOS_C_RED, 0);
                lv_obj_set_style_text_font(e, aos_font_small, 0);
            }
        }
    }
}

static void edit_changed(void)
{
    if (G.edview) {
        mx_view_refresh(G.edview);
    }
}

static void build(void);

static void edit_close(bool commit)
{
    if (G.editing < 0) {
        return;
    }
    int i = G.editing;
    if (commit) {
        mx_row_free(G.f[i].row);
        G.f[i].row = G.ed.root;
        G.ed.root = NULL;
        mx_ed_set_row(&G.ed, NULL);
        compile(i);
        G.dirty = true;
        save();
    }
    G.editing = -1;
    build();
}

static void key_cb(const char *key, void *user)
{
    (void)user;
    if (!strcmp(key, "/enter")) {
        edit_close(true);
        return;
    }
    if (!strcmp(key, "/ans")) {
        return;
    }
    mx_ed_key(&G.ed, key);
    edit_changed();
}

static bool hw_key(uint32_t key, uint8_t mods)
{
    char k[8];
    if (G.editing < 0 || !cas_hwkey_str(key, mods, k)) {
        return false;
    }
    key_cb(k, NULL);
    return true;
}

static void done_cb(lv_event_t *e)
{
    (void)e;
    edit_close(true);
}

static void edit_open(int i)
{
    if (!G.ed_ok) {
        mx_ed_init(&G.ed);
        G.ed_ok = true;
    }
    mx_ed_set_row(&G.ed, mx_row_copy(G.f[i].row));
    G.editing = i;
    build();
}

/* --- the view ---------------------------------------------------------------------------- */

static void home_cb(lv_event_t *e)
{
    (void)e;
    G.cx = G.cy = 0;
    G.scale = 60;
    G.dirty = true;
}

static void fn_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (G.panel) {
        if (lv_obj_has_flag(G.panel, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(G.panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(G.panel, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_update_layout(G.box);
        G.W = 0;            /* the canvas is re-sized on the next build */
        build();
    }
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 64, 64);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, mx_font(28), 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *s_parent;
static bool      s_panel_hidden;

static void canvas_free(void)
{
    cas_plot_free(&G.plot);
}

static void build(void)
{
    lv_obj_t *parent = s_parent;
    if (!parent) {
        return;
    }
    if (G.panel) {
        s_panel_hidden = lv_obj_has_flag(G.panel, LV_OBJ_FLAG_HIDDEN) && G.editing < 0;
    }
    lv_obj_clean(parent);
    G.canvas = G.list = G.panel = G.pill = G.editbox = G.edview = G.kp = NULL;
    canvas_free();

    bool land = G.landscape;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(root, 10, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    G.box = lv_obj_create(root);
    lv_obj_remove_style_all(G.box);
    lv_obj_set_flex_grow(G.box, 1);
    if (land) lv_obj_set_height(G.box, LV_PCT(100));
    else lv_obj_set_width(G.box, LV_PCT(100));
    lv_obj_set_style_radius(G.box, 16, 0);
    lv_obj_set_style_clip_corner(G.box, true, 0);
    lv_obj_remove_flag(G.box, LV_OBJ_FLAG_SCROLLABLE);

    G.panel = lv_obj_create(root);
    lv_obj_remove_style_all(G.panel);
    lv_obj_set_flex_flow(G.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(G.panel, 8, 0);
    lv_obj_remove_flag(G.panel, LV_OBJ_FLAG_SCROLLABLE);
    if (land) {
        lv_obj_set_size(G.panel, LV_PCT(46), LV_PCT(100));
    } else {
        lv_obj_set_size(G.panel, LV_PCT(100), G.editing >= 0 ? LV_PCT(56) : LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(G.panel, LV_PCT(40), 0);
        if (G.editing >= 0) lv_obj_set_style_max_height(G.panel, LV_PCT(60), 0);
    }

    if (G.editing >= 0) {
        int i = G.editing;
        lv_obj_t *hdr = lv_obj_create(G.panel);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(hdr, 10, 0);
        lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
        fn_label(hdr, i);
        G.editbox = lv_obj_create(hdr);
        lv_obj_remove_style_all(G.editbox);
        lv_obj_set_flex_grow(G.editbox, 1);
        lv_obj_set_height(G.editbox, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(G.editbox, 64, 0);
        lv_obj_set_style_bg_color(G.editbox, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(G.editbox, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(G.editbox, 14, 0);
        lv_obj_set_style_border_width(G.editbox, 2, 0);
        lv_obj_set_style_border_color(G.editbox, lv_color_hex(COLORS[i]), 0);
        lv_obj_set_scrollbar_mode(G.editbox, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(G.editbox, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(G.editbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        G.edview = mx_view_create(G.editbox, 28);
        mx_view_set_editor(G.edview, &G.ed);
        lv_obj_t *done = lv_button_create(hdr);
        lv_obj_set_style_bg_color(done, AOS_C_ACCENT, 0);
        lv_obj_set_style_radius(done, 14, 0);
        lv_obj_set_style_shadow_width(done, 0, 0);
        lv_obj_set_height(done, 64);
        lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *dl = lv_label_create(done);
        lv_label_set_text(dl, _("Listo"));
        lv_obj_set_style_text_font(dl, aos_font_body, 0);
        lv_obj_center(dl);
        G.kp = cas_keypad_create(G.panel, key_cb, NULL);
        lv_obj_set_width(G.kp, LV_PCT(100));
        lv_obj_set_flex_grow(G.kp, 1);
        aos_ui_hwkbd_handler(hw_key);
    } else {
        G.list = lv_obj_create(G.panel);
        lv_obj_remove_style_all(G.list);
        lv_obj_set_size(G.list, LV_PCT(100), LV_SIZE_CONTENT);
        if (land) {
            lv_obj_set_flex_grow(G.list, 1);
        }
        lv_obj_set_style_max_height(G.list, land ? LV_PCT(100) : 300, 0);
        lv_obj_set_flex_flow(G.list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(G.list, 8, 0);
        lv_obj_set_scroll_dir(G.list, LV_DIR_VER);
        list_build();
        aos_ui_hwkbd_handler(NULL);
        if (s_panel_hidden) {
            lv_obj_add_flag(G.panel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_update_layout(root);
    G.W = lv_obj_get_width(G.box);
    G.H = lv_obj_get_height(G.box);
    if (G.W < 16) G.W = 16;
    if (G.H < 16) G.H = 16;
    if (!cas_plot_create(&G.plot, G.box, G.W, G.H)) {
        lv_obj_t *l = lv_label_create(G.box);
        lv_label_set_text(l, _("Sin memoria para el gráfico"));
        lv_obj_center(l);
        return;
    }
    G.canvas = G.plot.canvas;
    aos_gesture_attach(G.canvas, 0, gesture_cb, NULL);

    G.pill = lv_label_create(G.box);
    lv_obj_set_style_bg_color(G.pill, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(G.pill, LV_OPA_90, 0);
    lv_obj_set_style_radius(G.pill, 18, 0);
    lv_obj_set_style_pad_hor(G.pill, 16, 0);
    lv_obj_set_style_pad_ver(G.pill, 8, 0);
    lv_obj_set_style_text_font(G.pill, aos_font_small, 0);
    lv_obj_align(G.pill, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_add_flag(G.pill, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *fb = round_button(G.box, "f(x)");
    lv_obj_align(fb, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_add_event_cb(fb, fn_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hb = round_button(G.box, LV_SYMBOL_HOME);
    lv_obj_align(hb, LV_ALIGN_BOTTOM_LEFT, 88, -12);
    lv_obj_add_event_cb(hb, home_cb, LV_EVENT_CLICKED, NULL);

    redraw();
}

void cas_graph_build(lv_obj_t *parent, bool landscape)
{
    model_init();
    s_parent = parent;
    G.landscape = landscape;
    G.panel = NULL;
    if (!G.timer) {
        G.timer = lv_timer_create(timer_cb, 30, NULL);
    }
    build();
}

int cas_graph_put(const char *expr)
{
    model_init();
    for (int i = 0; i < NF; i++) {
        if (mx_row_empty(G.f[i].row)) {
            mx_row_free(G.f[i].row);
            G.f[i].row = mx_text_to_row(expr);
            G.f[i].on = true;
            compile(i);
            G.dirty = true;
            save();
            return i;
        }
    }
    return -1;
}

void cas_graph_shown(bool shown)
{
    if (G.shown && !shown) {
        save();             /* the window as it was left */
    }
    G.shown = shown;
    cas_app_set_gestures(shown);
    if (!shown) {
        /* the widgets go with the view: forget them */
        if (G.editing >= 0) {
            G.editing = -1;
        }
        G.canvas = G.list = G.panel = G.pill = G.editbox = G.edview = G.kp = NULL;
        canvas_free();
        s_parent = NULL;
    } else {
        G.dirty = true;
    }
}

void cas_graph_free(void)
{
    save();
    if (G.timer) {
        lv_timer_delete(G.timer);
        G.timer = NULL;
    }
    canvas_free();
    for (int i = 0; i < NF; i++) {
        cas_job_cancel(&G.f[i]);
        mx_prog_free(&G.f[i].prog);
        mx_row_free(G.f[i].row);
        G.f[i].row = NULL;
    }
    if (G.ed_ok) {
        mx_ed_free(&G.ed);
        G.ed_ok = false;
    }
    G.init = false;
    s_parent = NULL;
}
