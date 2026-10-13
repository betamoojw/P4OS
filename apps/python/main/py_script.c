/*
 * P4OS - Python: the scripts in /python/apps, each an app of its own.
 *
 * Every .py in the card's /python/apps (but those whose name starts with
 * "_", which are modules for the others) is an entry in the launcher, with
 * its name, a colour and an icon, beside the apps written in C: the same
 * arrangement as Lua's scripts (apps/lua). The script says about itself, in
 * comments on its first lines:
 *
 *     # @name Gatitos
 *     # @canvas 360x640             its own canvas (x2 here); without it the
 *                                   screen at x3, 240x426 upright
 *     # @orientation portrait       or landscape: the screen turns for it
 *
 * and draws with the aos module (mod_aos.c), on a canvas the OS scales
 * (aos_retro.h). An .aic beside it (gatitos.aic) is its icon.
 *
 * This file is the view, all of it in the LVGL task: the canvas, the finger
 * and the gestures, the gamepad, what the script printed, its error. The
 * script runs on the interpreter's thread (p4py_rt.c); the two meet in
 * pyc_t (py_canvas.h), handed over at every aos.frame(). There is one
 * interpreter for all the apps of python.so -the loader gives them one copy
 * of the module-, and the app in front has it.
 *
 * A broken script is a message with its line, over its canvas, never a
 * reboot; tapping the message (or A on a pad) runs it again, and so does
 * saving the file: the app watches it. sys.exit() closes the app. Leaving
 * stops the script even if it catches KeyboardInterrupt (p4py_rt.c).
 */
#include "p4py_rt.h"
#include "py_canvas.h"

#include "aos_app.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_pad.h"
#include "aos_retro.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PYS_DIR         "/python/apps"
#define PYS_MAX_APPS    128             /* launcher entries, of the firmware's 224 */
#define PYS_MAX_SOURCE  (256 * 1024)
#define PYS_HEAD        512             /* where the @ lines are looked for */
#define PYS_FPS         50
#define PYS_HEAP        (2u * 1024 * 1024)      /* to start: it grows */
#define PYS_FILL_DIV    3               /* the default canvas: the screen over this, x this */
#define PYS_MIN_SIDE    16
#define PYS_MAX_SIDE    1280
#define PYS_MAX_PX      (720 * 1280)
#define PYS_EDGE_BOTTOM 36              /* the home swipe's strip */
#define PYS_TAIL        4096            /* what the script printed last */
#define PYS_LOOP        "\n__import__('aos')._loop()\n"

_Static_assert(AOS_PAD_A == PYC_PAD_A && AOS_PAD_START == PYC_PAD_START && AOS_PAD_UP == PYC_PAD_UP,
               "py_canvas.h's pad roles are aos_pad.h's");

typedef struct {
    char     name[48];
    int16_t  w, h;              /* the canvas it asked for; 0 = fill        */
    uint32_t orient;            /* AOS_APP_FLAG_PORTRAIT / _LANDSCAPE or 0  */
} pys_head_t;

typedef struct {
    aos_app_t *self;
    lv_obj_t  *root, *surface, *message;
    const aos_retro_t *r;
    char       file[48];        /* "gatitos.py"                             */
    pys_head_t head;
    pyc_t      c;               /* shared with the script's thread          */

    bool       started;         /* the interpreter is up (or booting)       */
    bool       posted;          /* the script was handed to it              */
    bool       done;            /* it ended                                 */
    bool       want_reload;
    uint32_t   last_post;

    /* the finger and the gestures, waiting for the next frame */
    pyc_ev_t   pend[PYC_MAX_EVENTS];
    int        n_pend;
    int16_t    t_x, t_y, last_x, last_y;
    bool       t_down;

    aos_pad_t   pad;
    uint16_t    pad_pend;
    lv_timer_t *pad_timer;

    char       tail[PYS_TAIL + 1];
    size_t     tail_n;
    uint32_t   tail_seq, tail_put_seq, tail_put_ms;
    bool       tail_watched;

    uint32_t   watch_at;
    time_t     watch_mtime;
    long       watch_size;
} pys_t;

static uint32_t now_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

static int ph_get(const int *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void ph_set(int *p, int v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void path_of(const char *file, char *out, size_t n)
{
    const char *sd = aos_hal_path_sd_root();
    snprintf(out, n, "%s" PYS_DIR "/%s", sd ? sd : "/sdcard", file);
}

/* ==========================================================================
 * What a script says about itself
 * ========================================================================== */

static bool head_tag(const char *text, const char *tag, char *out, size_t out_len)
{
    const char *p = strstr(text, tag);
    if (!p) return false;
    p += strlen(tag);
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (p[n] && p[n] != '\n' && p[n] != '\r' && n < out_len - 1) n++;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n == 0) return false;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

/* The name in the launcher, the canvas and the orientation; without @name
 * the file's, without its extension. A canvas that makes no sense is
 * ignored: the script still runs, on the default one. */
static void head_read(const char *file, pys_head_t *h)
{
    memset(h, 0, sizeof *h);
    snprintf(h->name, sizeof h->name, "%s", file);
    char *dot = strrchr(h->name, '.');
    if (dot) *dot = 0;
    char path[192];
    path_of(file, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char text[PYS_HEAD];
    size_t got = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[got] = 0;
    char v[48];
    if (head_tag(text, "@name", v, sizeof v)) snprintf(h->name, sizeof h->name, "%s", v);
    if (head_tag(text, "@canvas", v, sizeof v)) {
        int w = 0, hh = 0;
        if (sscanf(v, "%dx%d", &w, &hh) == 2 && w >= PYS_MIN_SIDE && hh >= PYS_MIN_SIDE &&
            w <= PYS_MAX_SIDE && hh <= PYS_MAX_SIDE && w * hh <= PYS_MAX_PX) {
            h->w = (int16_t)w;
            h->h = (int16_t)hh;
        }
    }
    if (head_tag(text, "@orientation", v, sizeof v)) {
        if (!strcasecmp(v, "portrait")) h->orient = AOS_APP_FLAG_PORTRAIT;
        else if (!strcasecmp(v, "landscape")) h->orient = AOS_APP_FLAG_LANDSCAPE;
    }
}

/* ==========================================================================
 * What the script printed, and its error
 * ========================================================================== */

/* The newest PYS_TAIL bytes of what it printed: the error is in there, and
 * the page in the portal reads it (key "con" on the live channel). */
static void tail_add(pys_t *s, const char *b, size_t n)
{
    if (n >= PYS_TAIL) {
        b += n - PYS_TAIL;
        n = PYS_TAIL;
    }
    if (s->tail_n + n > PYS_TAIL) {
        size_t drop = s->tail_n + n - PYS_TAIL;
        memmove(s->tail, s->tail + drop, s->tail_n - drop);
        s->tail_n -= drop;
    }
    memcpy(s->tail + s->tail_n, b, n);
    s->tail_n += n;
    s->tail[s->tail_n] = 0;
    s->tail_seq++;
}

static void output_drain(pys_t *s)
{
    char buf[1024];
    size_t n;
    while ((n = p4py_take_output(buf, sizeof buf)) > 0) tail_add(s, buf, n);
    uint32_t now = now_ms();
    bool watched = aos_hal_live_idle_ms(s->self->desc.id) <= 10000;
    bool fresh = watched && !s->tail_watched;     /* a page that just started looking */
    s->tail_watched = watched;
    if (watched && (fresh || (s->tail_seq != s->tail_put_seq && now - s->tail_put_ms > 200))) {
        aos_hal_live_put(s->self->desc.id, "con", "text/plain; charset=utf-8", s->tail, s->tail_n);
        s->tail_put_seq = s->tail_seq;
        s->tail_put_ms = now;
    }
}

/* the traceback: the last stretch between the runtime's 0x01 and 0x02 */
static void last_error(const pys_t *s, char *out, size_t n)
{
    const char *a = strrchr(s->tail, '\x01');
    if (!a) {
        snprintf(out, n, "%s", s->tail_n ? s->tail : "?");
        return;
    }
    a++;
    const char *b = strchr(a, '\x02');
    size_t len = b ? (size_t)(b - a) : strlen(a);
    while (len > 0 && (a[len - 1] == '\n' || a[len - 1] == ' ')) len--;
    if (len >= n) {
        a += len - (n - 1);         /* the end of it says what went wrong */
        len = n - 1;
    }
    memcpy(out, a, len);
    out[len] = 0;
}

static void reload(pys_t *s);

static void reload_cb(lv_event_t *e)
{
    pys_t *s = lv_event_get_user_data(e);
    s->want_reload = true;
}

static void show_error(pys_t *s, const char *what)
{
    if (!s->message) {
        int32_t w = lv_obj_get_width(s->root) - 2 * AOS_UI_PAD;
        if (w > 680) w = 680;
        s->message = lv_obj_create(s->root);
        lv_obj_remove_style_all(s->message);
        lv_obj_set_width(s->message, w);
        lv_obj_set_height(s->message, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s->message, lv_color_hex(0x300000), 0);
        lv_obj_set_style_bg_opa(s->message, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s->message, 20, 0);
        lv_obj_set_style_pad_row(s->message, 12, 0);
        lv_obj_set_style_radius(s->message, AOS_UI_RADIUS / 2, 0);
        lv_obj_set_flex_flow(s->message, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(s->message, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s->message, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s->message, reload_cb, LV_EVENT_CLICKED, s);
        lv_obj_align(s->message, LV_ALIGN_BOTTOM_MID, 0, -(PYS_EDGE_BOTTOM + AOS_UI_PAD));

        lv_obj_t *text = lv_label_create(s->message);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(text, LV_PCT(100));
        lv_obj_set_style_text_color(text, lv_color_hex(0xFF9F9F), 0);
        lv_obj_set_style_text_font(text, aos_font_small, 0);

        lv_obj_t *hint = lv_label_create(s->message);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_text(hint, _("Tocá para correrlo de nuevo"));
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_text_font(hint, aos_font_caption, 0);
        aos_make_decorative(text);
        aos_make_decorative(hint);
    }
    lv_label_set_text(lv_obj_get_child(s->message, 0), what);
    lv_obj_remove_flag(s->message, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s->message);
    aos_hal_beep(220, 120);
}

/* ==========================================================================
 * The frame
 * ========================================================================== */

static int rows_of(const px_dirty_t *d)
{
    if (d->all) return d->h;
    int rows = 0;
    for (int y = 0; y < d->h; y++) {
        for (int i = 0; i < d->n; i++) {
            if (y >= d->r[i].y0 && y < d->r[i].y1) {
                rows++;
                break;
            }
        }
    }
    return rows;
}

/* What the script handed over goes onto the OS's canvas, and its beeps
 * sound. With the thread waiting in aos.frame(), or done. */
static void harvest(pys_t *s, bool all)
{
    const aos_retro_t *r = s->r;
    pyc_t *c = &s->c;
    if (r && c->work && r->w == c->w && r->h == c->h) {
        if (all || c->dirty.all) {
            memcpy(r->px, c->work, (size_t)c->w * c->h * 2);
            aos_retro_present();
            c->rows = (uint16_t)c->h;
            c->px = (uint32_t)c->w * c->h;
        } else {
            for (int i = 0; i < c->dirty.n; i++) {
                const px_rect_t *d = &c->dirty.r[i];
                px_restore(r->px, c->work, c->w, d);
                aos_retro_present_rect(d->x0, d->y0, d->x1 - d->x0, d->y1 - d->y0);
            }
            c->rows = (uint16_t)rows_of(&c->dirty);
            c->px = (uint32_t)px_dirty_area(&c->dirty);
        }
    }
    for (int i = 0; i < c->n_beeps; i++) aos_hal_beep(c->beeps[i].hz, c->beeps[i].ms);
    c->n_beeps = 0;
}

/* the finger, in the script's pixels */
static int16_t to_canvas(int v, int origin, int k)
{
    int d = v - origin;
    return (int16_t)(d >= 0 ? d / k : -((-d + k - 1) / k));
}

/* The finger and the pad as they are now, and the events since the last
 * frame, into the snapshot the script reads. */
static void snapshot(pys_t *s)
{
    pyc_t *c = &s->c;
    uint32_t now = now_ms();
    uint32_t dt = now - s->last_post;
    s->last_post = now;
    c->ms_frame = (uint16_t)(dt > 0xFFFF ? 0xFFFF : dt);
    aos_retro_stats_t st;
    aos_retro_stats(&st);
    c->ms_screen = (uint16_t)(st.refresh_us_avg / 1000);
    c->touch_x = s->t_x;
    c->touch_y = s->t_y;
    c->touch_down = s->t_down;
    c->pad_held = (uint16_t)s->pad.held;
    c->pad_pressed = s->pad_pend;
    c->pad_x = s->pad.x;
    c->pad_y = s->pad.y;
    c->pad_on = s->pad.connected;
    s->pad_pend = 0;
    c->fingers = 0;
    if (s->surface && s->r) {
        aos_touch_point_t pts[2];
        aos_touch_points(pts);
        lv_area_t a;
        lv_obj_get_coords(s->surface, &a);
        for (int i = 0; i < 2; i++) {
            if (!pts[i].down) continue;
            int n = c->fingers++;
            c->finger_id[n] = pts[i].id;
            c->finger_x[n] = to_canvas((int)pts[i].x, a.x1, s->r->scale);
            c->finger_y[n] = to_canvas((int)pts[i].y, a.y1, s->r->scale);
        }
    }
    memcpy(c->ev, s->pend, sizeof(pyc_ev_t) * (size_t)s->n_pend);
    c->n_ev = s->n_pend;
    s->n_pend = 0;
}

static void close_async(void *arg)
{
    pys_t *s = arg;
    aos_ui_close(s->self->desc.id);
}

/* the script, to the interpreter, once it has booted */
static void try_start(pys_t *s)
{
    if (!p4py_ready()) return;
    char path[192];
    path_of(s->file, path, sizeof path);
    FILE *f = fopen(path, "rb");
    long len = -1;
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        rewind(f);
    }
    char *src = (len >= 0 && len <= PYS_MAX_SOURCE) ? malloc((size_t)len + sizeof PYS_LOOP) : NULL;
    if (!src) {
        if (f) fclose(f);
        s->posted = s->done = true;
        show_error(s, len > PYS_MAX_SOURCE ? "the file is too big" : path);
        return;
    }
    size_t got = fread(src, 1, (size_t)len, f);
    fclose(f);
    /* after its top level, the frames of a script made of functions */
    memcpy(src + got, PYS_LOOP, sizeof PYS_LOOP);

    pyc_t *c = &s->c;
    c->t0 = c->t_script = now_ms();
    c->n_ev = 0;
    c->n_beeps = 0;
    c->framed = c->has_resize = false;
    p4py_aos_reset();
    pyc_current = c;
    ph_set(&c->phase, PYC_SCRIPT);
    s->last_post = now_ms();
    s->posted = p4py_run(src, s->file, P4PY_FILE);
    free(src);
}

/* the run ended: what it drew stays on screen, with its error if it had one */
static void finished(pys_t *s)
{
    s->done = true;
    output_drain(s);
    harvest(s, true);
    if (p4py_last_error()) {
        char err[600];
        last_error(s, err, sizeof err);
        show_error(s, err);
    } else if (p4py_exited()) {
        lv_async_call(close_async, s);
    }
}

/* The frame timer (aos_retro_run, LVGL task). If the script is still on
 * the last frame, nothing: it gets the frames it can make. */
static void pump(void *user)
{
    pys_t *s = user;
    output_drain(s);
    if (!s->started || s->want_reload) return;      /* a reload is the tick's */
    if (!s->posted) {
        try_start(s);
        return;
    }
    if (s->done) return;
    if (!p4py_busy()) {
        finished(s);
        return;
    }
    if (ph_get(&s->c.phase) != PYC_SHOW) return;
    harvest(s, false);
    snapshot(s);
    ph_set(&s->c.phase, PYC_SCRIPT);
}

/* ==========================================================================
 * The finger, the gestures, the pad
 * ========================================================================== */

static void queue(pys_t *s, const pyc_ev_t *e)
{
    /* a move replaces the move before it: where the finger is, not every
     * sample on the way */
    if (e->kind == PYC_EV_MOVE && s->n_pend > 0 && s->pend[s->n_pend - 1].kind == PYC_EV_MOVE) {
        s->pend[s->n_pend - 1] = *e;
        return;
    }
    if (s->n_pend < PYC_MAX_EVENTS) s->pend[s->n_pend++] = *e;
}

static void touch_cb(lv_event_t *e)
{
    pys_t *s = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (!s->r || !s->surface || s->done) return;
    lv_point_t p = { 0, 0 };
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(s->surface, &a);
    int k = s->r->scale;
    s->t_x = to_canvas(p.x, a.x1, k);
    s->t_y = to_canvas(p.y, a.y1, k);
    s->t_down = code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING;
    pyc_ev_t ev = { 0 };
    ev.x = s->t_x;
    ev.y = s->t_y;
    if (code == LV_EVENT_PRESSED) {
        ev.kind = PYC_EV_DOWN;
    } else if (code == LV_EVENT_PRESSING) {
        if (ev.x == s->last_x && ev.y == s->last_y) return;
        ev.kind = PYC_EV_MOVE;
    } else {
        ev.kind = PYC_EV_UP;
    }
    s->last_x = ev.x;
    s->last_y = ev.y;
    queue(s, &ev);
}

static void gesture_cb(const aos_gesture_event_t *g, void *user)
{
    pys_t *s = user;
    if (s->done || !s->r || !s->surface) return;
    pyc_ev_t ev = { 0 };
    ev.kind = PYC_EV_GESTURE;
    switch (g->type) {
    case AOS_GESTURE_TAP:         ev.name = "tap";        break;
    case AOS_GESTURE_DOUBLE_TAP:  ev.name = "double";     break;
    case AOS_GESTURE_LONG_PRESS:  ev.name = "long";       break;
    case AOS_GESTURE_DRAG:        ev.name = "drag";       ev.nargs = 2; break;
    case AOS_GESTURE_DRAG_END:    ev.name = "release";    ev.nargs = 2; break;
    case AOS_GESTURE_PINCH_BEGIN: ev.name = "pinchstart"; break;
    case AOS_GESTURE_PINCH:       ev.name = "pinch";      ev.nargs = 3; break;
    case AOS_GESTURE_PINCH_END:   ev.name = "pinchend";   break;
    default: return;
    }
    lv_area_t a;
    lv_obj_get_coords(s->surface, &a);
    float k = (float)s->r->scale;
    ev.x = to_canvas((int)g->x, a.x1, s->r->scale);
    ev.y = to_canvas((int)g->y, a.y1, s->r->scale);
    if (g->type == AOS_GESTURE_DRAG) {
        ev.a = g->dx / k;
        ev.b = g->dy / k;
    } else if (g->type == AOS_GESTURE_DRAG_END) {
        ev.a = g->vx / k;
        ev.b = g->vy / k;
    } else if (g->type == AOS_GESTURE_PINCH) {
        ev.a = g->scale;
        ev.b = g->dx / k;
        ev.c = g->dy / k;
    }
    /* drags and pinches within one frame add up into one */
    if (s->n_pend > 0) {
        pyc_ev_t *last = &s->pend[s->n_pend - 1];
        if (last->kind == PYC_EV_GESTURE && last->name == ev.name) {
            if (g->type == AOS_GESTURE_DRAG) {
                last->a += ev.a;
                last->b += ev.b;
                last->x = ev.x;
                last->y = ev.y;
                return;
            }
            if (g->type == AOS_GESTURE_PINCH) {
                last->a *= ev.a;
                last->b += ev.b;
                last->c += ev.c;
                last->x = ev.x;
                last->y = ev.y;
                return;
            }
        }
    }
    queue(s, &ev);
}

/* Every 20 ms: one read of the pad. The presses gather until the next frame
 * (a short one during a slow frame still counts once); on the error
 * message, A runs the script again. */
static void pad_cb(lv_timer_t *t)
{
    pys_t *s = lv_timer_get_user_data(t);
    aos_pad_update(&s->pad, lv_tick_get());
    s->pad_pend = (uint16_t)(s->pad_pend | s->pad.pressed);
    if (s->message && !lv_obj_has_flag(s->message, LV_OBJ_FLAG_HIDDEN) && aos_pad_pressed(&s->pad, AOS_PAD_A)) {
        s->want_reload = true;
    }
}

/* ==========================================================================
 * The canvas
 * ========================================================================== */

/* The canvas for the root's shape and the script's head, and the surface
 * over it that takes the finger. 'keep' carries the pixels over (a canvas of
 * the script's own size, placed again after a turn). The buffers are made
 * once, for the largest the canvas may be in either orientation: a turn
 * never moves them under a framebuf the script holds. */
static bool canvas_setup(pys_t *s, bool keep)
{
    lv_obj_t *root = s->root;
    pyc_t *c = &s->c;
    lv_obj_update_layout(root);
    int W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    int w = s->head.w, h = s->head.h;
    if (w <= 0 || h <= 0) {
        w = W / PYS_FILL_DIV;
        h = H / PYS_FILL_DIV;
    }
    if (!c->work) {
        size_t cap = (size_t)w * h, turned = (size_t)(H / PYS_FILL_DIV) * (W / PYS_FILL_DIV);
        if (s->head.w <= 0 && turned > cap) cap = turned;
        /* malloc: big means PSRAM, where a canvas belongs (docs/MEMORY.md) */
        c->work = calloc(cap, 2);
        c->back = calloc(cap, 2);
        if (!c->work || !c->back) {
            free(c->work);
            free(c->back);
            c->work = c->back = NULL;
            return false;
        }
        c->cap_px = cap;
    }
    if ((size_t)w * h > c->cap_px) return false;

    if (s->surface) {
        lv_obj_delete(s->surface);
        s->surface = NULL;
    }
    s->r = aos_retro_begin(root, w, h, 0, 0);
    if (!s->r) return false;

    bool same = keep && c->w == w && c->h == h;
    if (!same) {
        memset(c->work, 0, (size_t)w * h * 2);
        c->back_on = false;
        c->w = (int16_t)w;
        c->h = (int16_t)h;
        px_buf_init(&c->buf, c->work, w, h);
        px_dirty_init(&c->drawn, w, h);
        px_dirty_init(&c->dirty, w, h);
        px_dirty_init(&c->prev, w, h);
    }
    memcpy(s->r->px, c->work, (size_t)w * h * 2);
    aos_retro_present();

    s->surface = lv_obj_create(root);
    lv_obj_remove_style_all(s->surface);
    lv_obj_set_pos(s->surface, s->r->x, s->r->y);
    lv_obj_set_size(s->surface, s->r->w * s->r->scale, s->r->h * s->r->scale);
    lv_obj_remove_flag(s->surface, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s->surface, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->surface, touch_cb, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(s->surface, touch_cb, LV_EVENT_PRESSING, s);
    lv_obj_add_event_cb(s->surface, touch_cb, LV_EVENT_RELEASED, s);
    aos_gesture_attach(s->surface, 0, gesture_cb, s);
    if (s->message) lv_obj_move_foreground(s->message);

    aos_retro_run(PYS_FPS, NULL, pump, s);
    aos_hal_log("python", "%s: canvas %dx%d x%d", s->file, w, h, s->r->scale);
    return true;
}

static void watch_mark(pys_t *s)
{
    char path[192];
    path_of(s->file, path, sizeof path);
    struct stat st;
    if (stat(path, &st) == 0) {
        s->watch_mtime = st.st_mtime;
        s->watch_size = (long)st.st_size;
    }
    s->watch_at = now_ms();
}

/* From the start: a new interpreter, the head read again (the canvas made
 * again if it asks for another), the screen black. */
static void reload(pys_t *s)
{
    s->want_reload = false;
    if (s->started) {
        if (!p4py_stop()) {
            show_error(s, "the script did not stop");
            s->started = false;
            return;
        }
    }
    pyc_current = NULL;
    s->started = s->posted = s->done = false;
    s->tail_n = 0;
    s->tail[0] = 0;
    s->tail_seq++;                              /* the page sees it empty again */
    s->n_pend = 0;
    if (s->message) lv_obj_add_flag(s->message, LV_OBJ_FLAG_HIDDEN);
    watch_mark(s);

    pys_head_t old = s->head;
    head_read(s->file, &s->head);
    pyc_t *c = &s->c;
    c->fb_used = false;
    if (old.w != s->head.w || old.h != s->head.h || !s->r) {
        if (!canvas_setup(s, false)) {
            show_error(s, "no memory for the canvas");
            return;
        }
    } else {
        memset(c->work, 0, (size_t)c->w * c->h * 2);
        c->back_on = false;
        px_dirty_reset(&c->drawn);
        px_dirty_reset(&c->prev);
        harvest(s, true);
    }
    aos_pad_reset(&s->pad, lv_tick_get());
    s->pad_pend = 0;
    s->started = p4py_start(PYS_HEAP);
    if (!s->started) show_error(s, "no memory for the interpreter");
}

/* ==========================================================================
 * The app
 * ========================================================================== */

/* "py.gatitos" -> "gatitos.py" */
static void file_of(const char *id, char *out, size_t n)
{
    const char *base = id && !strncmp(id, "py.", 3) ? id + 3 : id;
    snprintf(out, n, "%s.py", base ? base : "");
}

static void *pys_create(aos_app_t *self, lv_obj_t *root)
{
    pys_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->self = self;
    s->root = root;
    file_of(self->desc.id, s->file, sizeof s->file);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    aos_pad_reset(&s->pad, lv_tick_get());
    s->pad_timer = lv_timer_create(pad_cb, 20, s);
    reload(s);
    return s;
}

static void pys_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    pys_t *s = inst;
    if (!s) return;
    lv_async_call_cancel(close_async, s);
    if (s->pad_timer) lv_timer_delete(s->pad_timer);
    aos_retro_stop();
    bool stopped = !s->started || p4py_stop();
    aos_retro_end();
    aos_hal_live_clear(s->self->desc.id);
    if (!stopped) {
        /* its thread may still write into the canvas: a leak beats that */
        aos_hal_log("python", "%s: the script did not stop, its memory is left", s->file);
        return;
    }
    pyc_current = NULL;
    free(s->c.work);
    free(s->c.back);
    free(s);
}

/* ~5 times a second: a reload asked for (the error tapped, the file saved). */
static void pys_tick(aos_app_t *self, void *inst)
{
    (void)self;
    pys_t *s = inst;
    if (!s) return;
    if (s->want_reload) {
        reload(s);
        return;
    }
    if (now_ms() - s->watch_at < 1000) return;
    s->watch_at = now_ms();
    char path[192];
    path_of(s->file, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) return;           /* deleted while running: leave it */
    if (st.st_mtime == s->watch_mtime && (long)st.st_size == s->watch_size) return;
    reload(s);
}

/* The screen turned. A canvas of the script's own size is placed again,
 * pixels and all. The default canvas takes the new shape, black: a script
 * that defines resize(w, h) is told and goes on, any other starts again. */
static bool pys_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)root;
    pys_t *s = inst;
    if (!s) return false;
    if (s->head.w > 0) return canvas_setup(s, true);
    if (!s->posted || s->done || !s->started) {
        s->head.w = -1;                         /* reload(): a canvas for the new shape */
        reload(s);
        return true;
    }
    /* the script has to be waiting in aos.frame() to be told */
    uint32_t t0 = now_ms();
    while (ph_get(&s->c.phase) != PYC_SHOW && p4py_busy()) {
        if (now_ms() - t0 > 1000) return false; /* the runtime makes the app again */
        aos_hal_sleep_ms(1);
    }
    if (!p4py_busy() || !s->c.has_resize) {
        s->head.w = -1;
        reload(s);
        return true;
    }
    harvest(s, false);
    if (!canvas_setup(s, false)) return false;
    pyc_ev_t ev = { .kind = PYC_EV_RESIZE, .x = s->c.w, .y = s->c.h };
    s->n_pend = 0;
    queue(s, &ev);
    return true;
}

/* ==========================================================================
 * One app per script
 *
 * As lua.so does (apps/lua/main/lua_app.c, the long comment there): the
 * module says how many apps it brings and describes each; what an app IS
 * comes from its id ("py.gatitos"), which the runtime keeps, not from its
 * index, which moves when a file is added. Index 0 is the console.
 * ========================================================================== */

bool py_console_describe(aos_app_t *app);       /* py_app.c */

static char s_files[PYS_MAX_APPS][44];
static int  s_count;

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

static void apps_scan(void)
{
    s_count = 0;
    const char *sd = aos_hal_path_sd_root();
    if (!sd) return;
    char dir[128];
    snprintf(dir, sizeof dir, "%s" PYS_DIR, sd);
    DIR *d = opendir(dir);
    if (!d) {
        /* made, so the folder is there for the first script */
        char py[128];
        snprintf(py, sizeof py, "%s/python", sd);
        mkdir(py, 0777);
        mkdir(dir, 0777);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < PYS_MAX_APPS) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".py") || e->d_name[0] == '.' || e->d_name[0] == '_') continue;
        /* "py." and the name, without ".py", inside the loader's 40-byte id:
         * a truncated id is an app that opens onto a file that is not there */
        size_t n = strlen(e->d_name);
        if (n >= sizeof s_files[0] || n >= 40) continue;
        memcpy(s_files[s_count], e->d_name, n + 1);
        s_count++;
    }
    closedir(d);
    qsort(s_files, (size_t)s_count, sizeof s_files[0], by_name);
    aos_hal_log("python", "%s: %d app%s for the launcher", dir, s_count, s_count == 1 ? "" : "s");
}

/* a colour per script, from its name: fifteen identical tiles are fifteen
 * tiles to read one by one */
static uint32_t app_hue(const char *name, bool second)
{
    static const uint32_t PAIRS[][2] = {
        { 0x3776AB, 0x1E4A75 }, { 0xFFD43B, 0xB39400 }, { 0x30D158, 0x1A7F36 }, { 0xFF9F0A, 0xB36A00 },
        { 0xFF375F, 0xA61E3A }, { 0xBF5AF2, 0x7A2FA0 }, { 0x64D2FF, 0x2E8FB0 }, { 0x5E5CE6, 0x3A38A0 },
    };
    uint32_t h = 2166136261u;
    for (const char *c = name; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    return PAIRS[h % (sizeof PAIRS / sizeof PAIRS[0])][second ? 1 : 0];
}

/* an .aic beside the script is its icon (tools/aic.py); /sdcard/icons/<id>.aic
 * still wins, that is the firmware's own override */
static void app_icon(aos_app_t *app, const char *file)
{
    char path[200];
    path_of(file, path, sizeof path);
    char *dot = strrchr(path, '.');
    if (!dot) return;
    snprintf(dot, sizeof path - (size_t)(dot - path), ".aic");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len > 0 && len <= 2048) {
        uint8_t *blob = malloc((size_t)len);
        if (blob && fread(blob, 1, (size_t)len, f) == (size_t)len) aos_icon_set_ops(app, blob, (size_t)len);
        free(blob);
    }
    fclose(f);
}

/* the strings of a descriptor outlive this call, one buffer per app (the
 * simulator keeps the pointers: lua_app.c tells how that showed) */
static char s_ids[PYS_MAX_APPS][48];
static char s_names[PYS_MAX_APPS][48];

static uint32_t py_count(void)
{
    apps_scan();
    return 1u + (uint32_t)s_count;
}

static bool py_describe(aos_app_t *app, uint32_t index)
{
    if (index == 0) return py_console_describe(app);
    if (s_count == 0) apps_scan();              /* reopened on the board */
    uint32_t i = index - 1;
    if (i >= (uint32_t)s_count) return false;
    const char *file = s_files[i];
    snprintf(s_ids[i], sizeof s_ids[0], "py.%s", file);
    char *dot = strrchr(s_ids[i], '.');
    if (dot) *dot = 0;
    pys_head_t head;
    head_read(file, &head);
    snprintf(s_names[i], sizeof s_names[0], "%s", head.name);

    app->desc.id       = s_ids[i];
    app->desc.name     = s_names[i];
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = app_hue(file, false);
    app->desc.color_b  = app_hue(file, true);
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | head.orient;
    app->desc.order    = 1200 + (int32_t)i;
    app->create  = pys_create;
    app->destroy = pys_destroy;
    app->tick    = pys_tick;
    app->resize  = pys_resize;
    app_icon(app, file);
    return true;
}

AOS_APP_ENTRY_MANY(py_count, py_describe);
