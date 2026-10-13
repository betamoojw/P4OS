/*
 * P4OS - Python: the aos module, what a Python app draws and reads with.
 *
 * Lua's 'aos' table (apps/lua) with the same names and the same rules, for
 * the scripts in /python/apps that are apps of their own in the launcher
 * (py_script.c is their view). A script draws on a canvas the OS scales to
 * the screen: 240x426 at x3 by default, or the size its head asks for.
 *
 *     aos.W, aos.H              the canvas
 *     aos.clear(c)              colours are 0xRRGGBB
 *     aos.pixel(x, y, c)
 *     aos.rect / aos.box(x, y, w, h, c)          filled / outline (Lua's
 *                               aos.frame(x, y, w, h, c) works too)
 *     aos.round(x, y, w, h, c, cut)              corners bitten
 *     aos.line(x0, y0, x1, y1, c)
 *     aos.disc / aos.ring(cx, cy, r, c)          filled / outline
 *     aos.ellipse(cx, cy, rx, ry, c)
 *     aos.shade(x, y, w, h, f)  darker (f < 0) or lighter, in sixteenths
 *     aos.text(x, y, s, c, scale=1)  8x8 ASCII; returns the width
 *     aos.textw(s, scale=1)
 *     aos.blit(x, y, w, h, data, key=None)  RGB565 pixels (2 bytes each, as
 *                               framebuf and the canvas keep them); key, a
 *                               0xRRGGBB colour, is not drawn
 *     aos.background()          what is drawn becomes the world: from then
 *                               on each frame starts with it restored where
 *                               the last one drew
 *     aos.frame()               shows what was drawn and waits for the next
 *                               frame (50 a second at most); the touch(),
 *                               gesture() and resize() of the script are
 *                               called from here. Returns the ms since the
 *                               last one
 *     aos.events()              what came with that frame, as tuples
 *     aos.touch(), aos.fingers(), aos.pad([mask]), aos.ms(), aos.beep(hz, ms)
 *     aos.stats()               where the last frame went
 *     aos.fb                    MicroPython's framebuf over the same canvas
 *                               (blit, scroll, poly...): what it draws is not
 *                               marked, so from then on frames go out whole
 *     aos.rgb565(c)             0xRRGGBB as framebuf's colour
 *
 * Two ways to write an app: functions the app calls (init, tick(dt),
 * draw(), touch(x, y, ev), gesture(ev, x, y, ...), resize(w, h)), as in
 * Lua, or a loop of the script's own that calls aos.frame(). A script with
 * neither shows what it drew and stays there.
 *
 * Nothing here touches LVGL: the canvas, the finger and the pad are the
 * snapshot the view leaves in pyc_t (py_canvas.h) at every frame. Named
 * mod_*.c so tools/gen_upy.sh scans it for its names; no firmware headers.
 */
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "extmod/font_petme128_8x8.h"

#include "py_canvas.h"

#include <math.h>
#include <string.h>

/* from aos_hal.h (not included, see above) */
void aos_hal_sleep_ms(uint32_t ms);

pyc_t *pyc_current;

MP_REGISTER_ROOT_POINTER(mp_obj_t p4py_aos_events);
MP_REGISTER_ROOT_POINTER(mp_obj_t p4py_aos_fb);

static uint32_t s_last_frame;

void p4py_aos_reset(void)
{
    MP_STATE_VM(p4py_aos_events) = MP_OBJ_NULL;
    MP_STATE_VM(p4py_aos_fb) = MP_OBJ_NULL;
    s_last_frame = 0;
}

static int ph_get(const int *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void ph_set(int *p, int v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static pyc_t *need(void)
{
    pyc_t *c = pyc_current;
    if (!c || !c->work) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("aos draws in a Python app: put the script in /python/apps"));
    }
    return c;
}

/* A coordinate: an int or a float (floored), kept far from int overflow.
 * The primitives clip; this keeps a width of two thousand million from
 * being a clip at all. */
static int arg_i(mp_obj_t o)
{
    mp_float_t f;
    if (mp_obj_is_small_int(o)) {
        mp_int_t v = MP_OBJ_SMALL_INT_VALUE(o);
        return v < -16384 ? -16384 : v > 16384 ? 16384 : (int)v;
    }
    f = mp_obj_get_float(o);
    if (!(f > -16384)) return -16384;           /* NaN too */
    if (f > 16384) return 16384;
    return (int)floor(f);
}

static uint16_t arg_c(mp_obj_t o)
{
    return px_rgb((uint32_t)mp_obj_get_int_truncated(o));
}

static void mark(pyc_t *c, int x, int y, int w, int h)
{
    if (w > 0 && h > 0) px_dirty_add(&c->drawn, x, y, w, h);
}

/* --- drawing --------------------------------------------------------------------- */

static mp_obj_t aos_clear(size_t n_args, const mp_obj_t *args)
{
    pyc_t *c = need();
    px_fill(&c->buf, n_args ? arg_c(args[0]) : 0);
    px_dirty_all(&c->drawn);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_clear_obj, 0, 1, aos_clear);

static mp_obj_t aos_pixel(mp_obj_t x_in, mp_obj_t y_in, mp_obj_t c_in)
{
    pyc_t *c = need();
    int x = arg_i(x_in), y = arg_i(y_in);
    px_px(&c->buf, x, y, arg_c(c_in));
    mark(c, x, y, 1, 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(aos_pixel_obj, aos_pixel);

static mp_obj_t aos_rect(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]), w = arg_i(args[2]), h = arg_i(args[3]);
    px_rect(&c->buf, x, y, w, h, arg_c(args[4]));
    mark(c, x, y, w, h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_rect_obj, 5, 5, aos_rect);

static mp_obj_t aos_frame_rect(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]), w = arg_i(args[2]), h = arg_i(args[3]);
    px_frame(&c->buf, x, y, w, h, arg_c(args[4]));
    mark(c, x, y, w, h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_frame_rect_obj, 5, 5, aos_frame_rect);

static mp_obj_t aos_round(size_t n_args, const mp_obj_t *args)
{
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]), w = arg_i(args[2]), h = arg_i(args[3]);
    int cut = n_args > 5 ? arg_i(args[5]) : 2;
    if (cut < 0) cut = 0;
    px_round(&c->buf, x, y, w, h, cut, arg_c(args[4]));
    mark(c, x, y, w, h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_round_obj, 5, 6, aos_round);

static mp_obj_t aos_line(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    pyc_t *c = need();
    int x0 = arg_i(args[0]), y0 = arg_i(args[1]), x1 = arg_i(args[2]), y1 = arg_i(args[3]);
    px_line(&c->buf, x0, y0, x1, y1, arg_c(args[4]));
    int lx = x0 < x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1;
    mark(c, lx, ly, (x0 < x1 ? x1 - x0 : x0 - x1) + 1, (y0 < y1 ? y1 - y0 : y0 - y1) + 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_line_obj, 5, 5, aos_line);

static mp_obj_t aos_disc(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]), r = arg_i(args[2]);
    if (r < 0) return mp_const_none;
    px_disc(&c->buf, x, y, r, arg_c(args[3]));
    mark(c, x - r, y - r, 2 * r + 1, 2 * r + 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_disc_obj, 4, 4, aos_disc);

static mp_obj_t aos_ring(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]), r = arg_i(args[2]);
    if (r < 0) return mp_const_none;
    px_ring(&c->buf, x, y, r, arg_c(args[3]));
    mark(c, x - r, y - r, 2 * r + 1, 2 * r + 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_ring_obj, 4, 4, aos_ring);

static mp_obj_t aos_ellipse(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]), rx = arg_i(args[2]), ry = arg_i(args[3]);
    if (rx < 0 || ry < 0) return mp_const_none;
    px_ellipse(&c->buf, x, y, rx, ry, arg_c(args[4]));
    mark(c, x - rx, y - ry, 2 * rx + 1, 2 * ry + 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_ellipse_obj, 5, 5, aos_ellipse);

static mp_obj_t aos_shade(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]), w = arg_i(args[2]), h = arg_i(args[3]);
    int f = arg_i(args[4]);
    if (f < -16) f = -16;
    if (f > 16) f = 16;
    px_shade(&c->buf, x, y, w, h, f);
    mark(c, x, y, w, h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_shade_obj, 5, 5, aos_shade);

/* --- text: framebuf's 8x8 font, ASCII ------------------------------------------- */

/* The next character of UTF-8 text as one the font has: Spanish letters lose
 * their accent (á -> a, ñ -> n), anything else outside ASCII is '?'. */
static int next_char(const byte **p, const byte *end)
{
    const byte *s = *p;
    int ch = *s++;
    if (ch >= 0x80) {
        unsigned cp = 0;
        int more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
        cp = ch & (0x3F >> more);
        while (more-- > 0 && s < end && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
        static const char *const FROM = "\xe1\xe9\xed\xf3\xfa\xfc\xf1\xc1\xc9\xcd\xd3\xda\xdc\xd1\xbf\xa1";
        static const char *const TO = "aeiouunAEIOUUN?!";
        ch = '?';
        for (int i = 0; FROM[i]; i++) {
            if ((byte)FROM[i] == cp) {
                ch = TO[i];
                break;
            }
        }
    }
    *p = s;
    return ch;
}

static int text_draw(pyc_t *c, int x, int y, const byte *s, size_t n, uint16_t col, int k, bool draw)
{
    const byte *end = s + n;
    int cx = x, w = 0, lines = 1;
    while (s < end) {
        int ch = next_char(&s, end);
        if (ch == '\n') {
            cx = x;
            y += 9 * k;
            lines++;
            continue;
        }
        if (ch < 32 || ch > 127) ch = '?';
        if (draw) {
            const uint8_t *g = &font_petme128_8x8[(ch - 32) * 8];
            for (int i = 0; i < 8; i++) {
                for (int j = 0, bits = g[i]; bits; j++, bits >>= 1) {
                    if (bits & 1) {
                        if (k == 1) px_px(&c->buf, cx + i, y + j, col);
                        else px_rect(&c->buf, cx + i * k, y + j * k, k, k, col);
                    }
                }
            }
        }
        cx += 8 * k;
        if (cx - x > w) w = cx - x;
    }
    (void)lines;
    return w;
}

static int arg_scale(size_t n_args, const mp_obj_t *args, size_t i)
{
    int k = n_args > i ? arg_i(args[i]) : 1;
    return k < 1 ? 1 : k > 32 ? 32 : k;
}

static mp_obj_t aos_text(size_t n_args, const mp_obj_t *args)
{
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]);
    mp_obj_t s_obj = mp_obj_is_str(args[2]) ? args[2] : mp_obj_str_make_new(&mp_type_str, 1, 0, &args[2]);
    size_t n;
    const byte *s = (const byte *)mp_obj_str_get_data(s_obj, &n);
    int k = arg_scale(n_args, args, 4);
    int lines = 1;
    for (size_t i = 0; i < n; i++) lines += s[i] == '\n';
    int w = text_draw(c, x, y, s, n, arg_c(args[3]), k, true);
    mark(c, x, y, w, (9 * lines - 1) * k);
    return MP_OBJ_NEW_SMALL_INT(w);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_text_obj, 4, 5, aos_text);

static mp_obj_t aos_textw(size_t n_args, const mp_obj_t *args)
{
    mp_obj_t s_obj = mp_obj_is_str(args[0]) ? args[0] : mp_obj_str_make_new(&mp_type_str, 1, 0, &args[0]);
    size_t n;
    const byte *s = (const byte *)mp_obj_str_get_data(s_obj, &n);
    return MP_OBJ_NEW_SMALL_INT(text_draw(NULL, 0, 0, s, n, 0, arg_scale(n_args, args, 1), false));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_textw_obj, 1, 2, aos_textw);

/* --- pixels from elsewhere ------------------------------------------------------- */

static mp_obj_t aos_blit(size_t n_args, const mp_obj_t *args)
{
    pyc_t *c = need();
    int x = arg_i(args[0]), y = arg_i(args[1]), w = arg_i(args[2]), h = arg_i(args[3]);
    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[4], &bi, MP_BUFFER_READ);
    if (w <= 0 || h <= 0) return mp_const_none;
    if ((size_t)w * h * 2 > bi.len) mp_raise_ValueError(MP_ERROR_TEXT("blit: the data is shorter than w*h*2"));
    bool keyed = n_args > 5 && args[5] != mp_const_none;
    uint16_t key = keyed ? arg_c(args[5]) : 0;
    const uint16_t *src = bi.buf;
    const px_buf_t *b = &c->buf;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < b->cy0 || yy >= b->cy1) continue;
        const uint16_t *row = src + (size_t)j * w;
        uint16_t *dst = b->px + (size_t)yy * b->w;
        int i0 = x < b->cx0 ? b->cx0 - x : 0, i1 = x + w > b->cx1 ? b->cx1 - x : w;
        if (!keyed) {
            if (i1 > i0) memcpy(dst + x + i0, row + i0, (size_t)(i1 - i0) * 2);
        } else {
            for (int i = i0; i < i1; i++) {
                if (row[i] != key) dst[x + i] = row[i];
            }
        }
    }
    mark(c, x, y, w, h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_blit_obj, 5, 6, aos_blit);

static mp_obj_t aos_background(void)
{
    pyc_t *c = need();
    memcpy(c->back, c->work, (size_t)c->w * c->h * 2);
    c->back_on = true;
    px_dirty_reset(&c->prev);       /* nothing of the old world to undo */
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(aos_background_obj, aos_background);

static mp_obj_t aos_rgb565(mp_obj_t c_in)
{
    return MP_OBJ_NEW_SMALL_INT(arg_c(c_in));
}
static MP_DEFINE_CONST_FUN_OBJ_1(aos_rgb565_obj, aos_rgb565);

/* --- the frame ------------------------------------------------------------------- */

/* A function the script defined at the top, or NULL. */
static mp_obj_t main_fn(qstr name)
{
    mp_map_elem_t *e = mp_map_lookup(&MP_STATE_VM(dict_main).map, MP_OBJ_NEW_QSTR(name), MP_MAP_LOOKUP);
    return e && mp_obj_is_callable(e->value) ? e->value : MP_OBJ_NULL;
}

static mp_obj_t ev_tuple(const pyc_ev_t *e)
{
    static const qstr TOUCH[] = { MP_QSTR_down, MP_QSTR_move, MP_QSTR_up };
    mp_obj_t t[6];
    size_t n = 0;
    if (e->kind == PYC_EV_RESIZE) {
        t[n++] = MP_OBJ_NEW_QSTR(MP_QSTR_resize);
    } else if (e->kind == PYC_EV_GESTURE) {
        t[n++] = mp_obj_new_str_from_cstr(e->name);
    } else {
        t[n++] = MP_OBJ_NEW_QSTR(TOUCH[e->kind]);
    }
    t[n++] = MP_OBJ_NEW_SMALL_INT(e->x);
    t[n++] = MP_OBJ_NEW_SMALL_INT(e->y);
    if (e->kind == PYC_EV_GESTURE) {
        if (e->nargs > 0) t[n++] = mp_obj_new_float(e->a);
        if (e->nargs > 1) t[n++] = mp_obj_new_float(e->b);
        if (e->nargs > 2) t[n++] = mp_obj_new_float(e->c);
    }
    return mp_obj_new_tuple(n, t);
}

/* The script's own functions for what came with the frame: touch(x, y, ev),
 * gesture(ev, x, y, ...) and resize(w, h), whichever it defines. */
static void deliver(mp_obj_t list)
{
    size_t n;
    mp_obj_t *items;
    mp_obj_list_get(list, &n, &items);
    for (size_t i = 0; i < n; i++) {
        mp_obj_t *t;
        size_t tn;
        mp_obj_tuple_get(items[i], &tn, &t);
        qstr kind = mp_obj_is_qstr(t[0]) ? MP_OBJ_QSTR_VALUE(t[0]) : MP_QSTRnull;
        if (kind == MP_QSTR_down || kind == MP_QSTR_move || kind == MP_QSTR_up) {
            mp_obj_t f = main_fn(MP_QSTR_touch);
            if (f) {
                mp_obj_t a[3] = { t[1], t[2], t[0] };
                mp_call_function_n_kw(f, 3, 0, a);
            }
        } else if (kind == MP_QSTR_resize) {
            mp_obj_t f = main_fn(MP_QSTR_resize);
            if (f) mp_call_function_2(f, t[1], t[2]);
        } else {
            mp_obj_t f = main_fn(MP_QSTR_gesture);
            if (f) mp_call_function_n_kw(f, tn, 0, t);
        }
    }
}

static mp_obj_t aos_frame(void)
{
    pyc_t *c = need();
    uint32_t now = mp_hal_ticks_ms();
    c->ms_script = (uint16_t)(now - c->t_script);

    /* what goes out: what was undone at the start of this frame (already in
     * 'dirty') and what the script drew; through framebuf, everything */
    if (c->fb_used || c->drawn.all) {
        px_dirty_all(&c->dirty);
    } else {
        for (int i = 0; i < c->drawn.n; i++) {
            const px_rect_t *r = &c->drawn.r[i];
            px_dirty_add(&c->dirty, r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0);
        }
    }
    c->prev = c->drawn;
    px_dirty_reset(&c->drawn);
    c->has_resize = main_fn(MP_QSTR_resize) != MP_OBJ_NULL;
    c->framed = true;

    /* the LVGL task's turn: it shows this and hands back the next frame */
    ph_set(&c->phase, PYC_SHOW);
    while (ph_get(&c->phase) != PYC_SCRIPT) {
        mp_handle_pending(true);            /* Stop, leaving the app */
        aos_hal_sleep_ms(1);
    }

    /* the next frame starts with the background back where the last one drew */
    px_dirty_reset(&c->dirty);
    if (c->back_on) {
        if (c->prev.all) {
            memcpy(c->work, c->back, (size_t)c->w * c->h * 2);
            px_dirty_all(&c->dirty);
        } else {
            for (int i = 0; i < c->prev.n; i++) {
                const px_rect_t *r = &c->prev.r[i];
                px_restore(c->work, c->back, c->w, r);
                px_dirty_add(&c->dirty, r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0);
            }
        }
    }
    uint32_t back_at = mp_hal_ticks_ms();
    uint32_t dt = s_last_frame ? back_at - s_last_frame : 0;
    s_last_frame = back_at;
    c->t_script = back_at;

    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < c->n_ev; i++) mp_obj_list_append(list, ev_tuple(&c->ev[i]));
    c->n_ev = 0;
    MP_STATE_VM(p4py_aos_events) = list;
    deliver(list);
    return MP_OBJ_NEW_SMALL_INT(dt);
}

/* aos.frame(): the frame. aos.frame(x, y, w, h, c): an outline, as Lua's
 * aos.frame is (aos.box says only that). */
static mp_obj_t aos_frame_var(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0) return aos_frame();
    if (n_args != 5) mp_raise_TypeError(MP_ERROR_TEXT("aos.frame() or aos.frame(x, y, w, h, c)"));
    return aos_frame_rect(n_args, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_frame_obj, 0, 5, aos_frame_var);

static mp_obj_t aos_events(void)
{
    mp_obj_t l = MP_STATE_VM(p4py_aos_events);
    return l ? l : mp_obj_new_list(0, NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_0(aos_events_obj, aos_events);

/* After the script's top level (py_script.c runs it last): the frames, for a
 * script made of functions. One with only init() or none at all has
 * nothing to do every frame and ends here, its drawing on screen. */
static mp_obj_t aos_loop(void)
{
    need();
    mp_obj_t init = main_fn(MP_QSTR_init);
    if (!main_fn(MP_QSTR_tick) && !main_fn(MP_QSTR_draw) && !main_fn(MP_QSTR_touch) &&
        !main_fn(MP_QSTR_gesture)) {
        if (init) mp_call_function_0(init);
        return mp_const_none;
    }
    if (init) mp_call_function_0(init);
    mp_obj_t dt = MP_OBJ_NEW_SMALL_INT(0);
    for (;;) {
        /* looked up every frame: a script may define them as it goes */
        mp_obj_t tick = main_fn(MP_QSTR_tick), draw = main_fn(MP_QSTR_draw);
        if (tick) mp_call_function_1(tick, dt);
        if (draw) mp_call_function_0(draw);
        dt = aos_frame();
    }
}
static MP_DEFINE_CONST_FUN_OBJ_0(aos_loop_obj, aos_loop);

/* --- what comes from outside: the view's snapshot -------------------------------- */

static mp_obj_t aos_touch(void)
{
    pyc_t *c = need();
    mp_obj_t t[3] = { MP_OBJ_NEW_SMALL_INT(c->touch_x), MP_OBJ_NEW_SMALL_INT(c->touch_y),
                      mp_obj_new_bool(c->touch_down) };
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(aos_touch_obj, aos_touch);

static mp_obj_t aos_fingers(void)
{
    pyc_t *c = need();
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int i = 0; i < c->fingers && i < 2; i++) {
        mp_obj_t t[3] = { MP_OBJ_NEW_SMALL_INT(c->finger_id[i]), MP_OBJ_NEW_SMALL_INT(c->finger_x[i]),
                          MP_OBJ_NEW_SMALL_INT(c->finger_y[i]) };
        mp_obj_list_append(l, mp_obj_new_tuple(3, t));
    }
    return l;
}
static MP_DEFINE_CONST_FUN_OBJ_0(aos_fingers_obj, aos_fingers);

/* aos.pad(): held, pressed, x, y, on. aos.pad(mask): isdown, waspressed. */
static mp_obj_t aos_pad(size_t n_args, const mp_obj_t *args)
{
    pyc_t *c = need();
    if (n_args) {
        uint32_t m = (uint32_t)mp_obj_get_int(args[0]);
        mp_obj_t t[2] = { mp_obj_new_bool(c->pad_held & m), mp_obj_new_bool(c->pad_pressed & m) };
        return mp_obj_new_tuple(2, t);
    }
    mp_obj_t t[5] = { MP_OBJ_NEW_SMALL_INT(c->pad_held), MP_OBJ_NEW_SMALL_INT(c->pad_pressed),
                      MP_OBJ_NEW_SMALL_INT(c->pad_x), MP_OBJ_NEW_SMALL_INT(c->pad_y),
                      mp_obj_new_bool(c->pad_on) };
    return mp_obj_new_tuple(5, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aos_pad_obj, 0, 1, aos_pad);

static mp_obj_t aos_ms(void)
{
    pyc_t *c = need();
    return mp_obj_new_int_from_uint(mp_hal_ticks_ms() - c->t0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(aos_ms_obj, aos_ms);

/* queued: the view plays them when the frame comes back */
static mp_obj_t aos_beep(mp_obj_t hz_in, mp_obj_t ms_in)
{
    pyc_t *c = need();
    int hz = arg_i(hz_in), ms = arg_i(ms_in);
    if (c->n_beeps < PYC_MAX_BEEPS && hz > 0 && ms > 0) {
        c->beeps[c->n_beeps].hz = (uint16_t)(hz > 20000 ? 20000 : hz);
        c->beeps[c->n_beeps].ms = (uint16_t)(ms > 5000 ? 5000 : ms);
        c->n_beeps++;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(aos_beep_obj, aos_beep);

static mp_obj_t aos_stats(void)
{
    pyc_t *c = need();
    mp_obj_t d = mp_obj_new_dict(5);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_script_ms), MP_OBJ_NEW_SMALL_INT(c->ms_script));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_screen_ms), MP_OBJ_NEW_SMALL_INT(c->ms_screen));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_frame_ms), MP_OBJ_NEW_SMALL_INT(c->ms_frame));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rows), MP_OBJ_NEW_SMALL_INT(c->rows));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_pixels), mp_obj_new_int_from_uint(c->px));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(aos_stats_obj, aos_stats);

/* aos.W, aos.H and aos.fb change with the canvas: asked for every time */
static mp_obj_t aos_getattr(mp_obj_t attr)
{
    qstr q = mp_obj_str_get_qstr(attr);
    if (q == MP_QSTR_W) return MP_OBJ_NEW_SMALL_INT(need()->w);
    if (q == MP_QSTR_H) return MP_OBJ_NEW_SMALL_INT(need()->h);
    if (q == MP_QSTR_fb) {
        pyc_t *c = need();
        c->fb_used = true;
        mp_obj_t fb = MP_STATE_VM(p4py_aos_fb);
        if (fb) {
            /* the same buffer always; a new FrameBuffer when the shape changed */
            mp_obj_t w = mp_load_attr(fb, MP_QSTR_width);
            if (mp_obj_get_int(w) == c->w) return fb;
        }
        mp_obj_t mod = mp_import_name(MP_QSTR_framebuf, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
        mp_obj_t a[4] = {
            mp_obj_new_bytearray_by_ref((size_t)c->w * c->h * 2, c->work),
            MP_OBJ_NEW_SMALL_INT(c->w), MP_OBJ_NEW_SMALL_INT(c->h),
            mp_load_attr(mod, MP_QSTR_RGB565),
        };
        fb = mp_call_function_n_kw(mp_load_attr(mod, MP_QSTR_FrameBuffer), 4, 0, a);
        MP_STATE_VM(p4py_aos_fb) = fb;
        return fb;
    }
    /* not found: the caller says so (an import only looks for __init__) */
    return MP_OBJ_NULL;
}
static MP_DEFINE_CONST_FUN_OBJ_1(aos_getattr_obj, aos_getattr);

static const mp_rom_map_elem_t aos_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_aos) },
    { MP_ROM_QSTR(MP_QSTR___getattr__), MP_ROM_PTR(&aos_getattr_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&aos_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&aos_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&aos_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame), MP_ROM_PTR(&aos_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_box), MP_ROM_PTR(&aos_frame_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_round), MP_ROM_PTR(&aos_round_obj) },
    { MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&aos_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_disc), MP_ROM_PTR(&aos_disc_obj) },
    { MP_ROM_QSTR(MP_QSTR_ring), MP_ROM_PTR(&aos_ring_obj) },
    { MP_ROM_QSTR(MP_QSTR_ellipse), MP_ROM_PTR(&aos_ellipse_obj) },
    { MP_ROM_QSTR(MP_QSTR_shade), MP_ROM_PTR(&aos_shade_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&aos_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_textw), MP_ROM_PTR(&aos_textw_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit), MP_ROM_PTR(&aos_blit_obj) },
    { MP_ROM_QSTR(MP_QSTR_background), MP_ROM_PTR(&aos_background_obj) },
    { MP_ROM_QSTR(MP_QSTR_rgb565), MP_ROM_PTR(&aos_rgb565_obj) },
    { MP_ROM_QSTR(MP_QSTR_events), MP_ROM_PTR(&aos_events_obj) },
    { MP_ROM_QSTR(MP_QSTR__loop), MP_ROM_PTR(&aos_loop_obj) },
    { MP_ROM_QSTR(MP_QSTR_touch), MP_ROM_PTR(&aos_touch_obj) },
    { MP_ROM_QSTR(MP_QSTR_fingers), MP_ROM_PTR(&aos_fingers_obj) },
    { MP_ROM_QSTR(MP_QSTR_pad), MP_ROM_PTR(&aos_pad_obj) },
    { MP_ROM_QSTR(MP_QSTR_ms), MP_ROM_PTR(&aos_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_beep), MP_ROM_PTR(&aos_beep_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&aos_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_PAD_UP), MP_ROM_INT(PYC_PAD_UP) },
    { MP_ROM_QSTR(MP_QSTR_PAD_DOWN), MP_ROM_INT(PYC_PAD_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_PAD_LEFT), MP_ROM_INT(PYC_PAD_LEFT) },
    { MP_ROM_QSTR(MP_QSTR_PAD_RIGHT), MP_ROM_INT(PYC_PAD_RIGHT) },
    { MP_ROM_QSTR(MP_QSTR_PAD_A), MP_ROM_INT(PYC_PAD_A) },
    { MP_ROM_QSTR(MP_QSTR_PAD_B), MP_ROM_INT(PYC_PAD_B) },
    { MP_ROM_QSTR(MP_QSTR_PAD_L), MP_ROM_INT(PYC_PAD_L) },
    { MP_ROM_QSTR(MP_QSTR_PAD_R), MP_ROM_INT(PYC_PAD_R) },
    { MP_ROM_QSTR(MP_QSTR_PAD_START), MP_ROM_INT(PYC_PAD_START) },
};
static MP_DEFINE_CONST_DICT(aos_globals, aos_globals_table);

const mp_obj_module_t p4py_aos_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&aos_globals,
};

MP_REGISTER_MODULE(MP_QSTR_aos, p4py_aos_module);
