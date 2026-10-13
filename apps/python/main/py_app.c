/*
 * P4OS - Python: the app (for now, the feasibility test).
 *
 * MicroPython 1.29 embedded (main/upy/, made by tools/gen_upy.sh) on a
 * thread of its own (p4py_rt.c). This first version opens, boots the
 * interpreter, runs a bench and shows what it prints, with the times in
 * the log: whether MicroPython works as a .so on the board, and how fast.
 */
#include "p4py_rt.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define HEAP_BYTES  (12u * 1024 * 1024)
#define SHOWN_MAX   (24 * 1024)

/* Python's logo: two snakes, the same shape turned half a turn about the
 * centre, blue above and yellow below, each with its eye; a thin line of
 * the background between them. Percent of the icon, from its centre. */
static const uint8_t PY_ICON[] = {
    AIC_HEADER,
    /* blue: the head on top, the body to the left */
    AIC_RECT(AIC_CENTER,  -2, -21, 34, 18, 8, AIC_C_LIT(0x3776AB), 255),
    AIC_GRAD(AIC_C_LIT(0x4B8BBE), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER, -14,  -2, 32, 20, 8, AIC_C_LIT(0x3776AB), 255),
    AIC_GRAD(AIC_C_LIT(0x306998), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER,  -4, -11, 14, 12, 0, AIC_C_LIT(0x3776AB), 255),
    AIC_RECT(AIC_CENTER, -11, -25,  5,  5, AIC_CIRCLE, AIC_C_TEXT, 255),
    /* yellow: the same, turned */
    AIC_RECT(AIC_CENTER,  14,   2, 32, 20, 8, AIC_C_LIT(0xFFD43B), 255),
    AIC_GRAD(AIC_C_LIT(0xFFE873), AIC_GRAD_VER),
    AIC_BORDER(2, AIC_C_LIT(0xF5F7FA), 255),
    AIC_RECT(AIC_CENTER,   2,  21, 34, 18, 8, AIC_C_LIT(0xFFD43B), 255),
    AIC_RECT(AIC_CENTER,   4,  11, 14, 12, 0, AIC_C_LIT(0xFFD43B), 255),
    AIC_RECT(AIC_CENTER,  11,  25,  5,  5, AIC_CIRCLE, AIC_C_TEXT, 255),
    AIC_END
};

static const char BENCH[] =
    "import sys, time, gc, math, json, re, random\n"
    "print('MicroPython', sys.version, 'on', sys.platform)\n"
    "def t(name, f):\n"
    "    t0 = time.ticks_ms(); r = f(); dt = time.ticks_diff(time.ticks_ms(), t0)\n"
    "    print('%-18s %6d ms  %s' % (name, dt, r))\n"
    "def fib(n):\n"
    "    return n if n < 2 else fib(n - 1) + fib(n - 2)\n"
    "def loop():\n"
    "    s = 0\n"
    "    for i in range(300000):\n"
    "        s += i * i\n"
    "    return s\n"
    "t('fib(24)', lambda: fib(24))\n"
    "t('for 300k', loop)\n"
    "t('2**10000 digits', lambda: len(str(2 ** 10000)))\n"
    "t('sin 100k', lambda: round(sum(math.sin(i / 1000) for i in range(100000)), 6))\n"
    "t('list comp 100k', lambda: len([x * 2 for x in range(100000)]))\n"
    "t('dict 50k', lambda: len({i: str(i) for i in range(50000)}))\n"
    "t('json', lambda: len(json.dumps({'a': [1, 2.5, 'x'] * 1000})))\n"
    "t('re', lambda: re.sub(r'(\\d+)', 'N', 'a1b22c333' * 100)[:12])\n"
    "t('sort 50k', lambda: sorted(random.random() for _ in range(50000))[0] >= 0)\n"
    "print(f'{math.pi:.15f}', 1 / 3, 2 ** 0.5, (1 + 2j) ** 2, 10 ** 30 // 7)\n"
    "try:\n"
    "    1 / 0\n"
    "except ZeroDivisionError as e:\n"
    "    print('caught', repr(e))\n"
    "gc.collect()\n"
    "print('heap free', gc.mem_free(), 'used', gc.mem_alloc())\n";

static const char ERROR_TEST[] =
    "def f(x):\n"
    "    return g(x) + 1\n"
    "def g(x):\n"
    "    return x / 0\n"
    "f(3)\n";

typedef struct {
    lv_obj_t   *out, *status;
    lv_timer_t *timer;
    char       *shown;
    size_t      shown_n;
    int         step;
} py_t;

static void show(py_t *p, const char *s, size_t n)
{
    if (p->shown_n + n > SHOWN_MAX - 1) {
        size_t drop = p->shown_n + n - (SHOWN_MAX - 1);
        if (drop > p->shown_n) drop = p->shown_n;
        memmove(p->shown, p->shown + drop, p->shown_n - drop);
        p->shown_n -= drop;
    }
    memcpy(p->shown + p->shown_n, s, n);
    p->shown_n += n;
    p->shown[p->shown_n] = 0;
    lv_label_set_text(p->out, p->shown);
    lv_obj_scroll_to_y(lv_obj_get_parent(p->out), LV_COORD_MAX, LV_ANIM_OFF);
}

static void tick(lv_timer_t *t)
{
    py_t *p = lv_timer_get_user_data(t);
    static char buf[4096];
    size_t n;
    while ((n = p4py_take_output(buf, sizeof buf)) > 0) {
        show(p, buf, n);
        aos_hal_log("python", "%s", buf);
    }
    char st[96];
    size_t used, freeb;
    p4py_mem(&used, &freeb);
    snprintf(st, sizeof st, "%s  ·  %u KB / %u KB", p4py_busy() ? "running" : "ready",
             (unsigned)(used / 1024), (unsigned)((used + freeb) / 1024));
    lv_label_set_text(p->status, st);
    if (!p4py_ready()) return;
    if (p->step == 0) {
        aos_hal_log("python", "booted in %u ms; bench", (unsigned)p4py_boot_ms());
        /* a test of the moment, if the card has one: /python/_test.py */
        const char *sd = aos_hal_path_sd_root();
        char path[200];
        snprintf(path, sizeof path, "%s/python/_test.py", sd ? sd : "/sdcard");
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            p4py_run("import _test", "<test>", P4PY_FILE);
            p->step = 3;
            return;
        }
        p4py_run(BENCH, "bench.py", P4PY_FILE);
        p->step = 1;
    } else if (p->step == 1) {
        aos_hal_log("python", "bench: %u ms", (unsigned)p4py_last_ms());
        p4py_run(ERROR_TEST, "error.py", P4PY_FILE);
        p->step = 2;
    } else if (p->step == 2) {
        p4py_run("[x * x for x in range(8)]", "<stdin>", P4PY_SINGLE);
        p->step = 3;
    }
}

static void *py_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    py_t *p = lv_malloc_zeroed(sizeof *p);
    p->shown = lv_malloc(SHOWN_MAX);
    p->shown[0] = 0;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 12, 0);
    lv_obj_set_style_pad_row(root, 8, 0);
    p->status = lv_label_create(root);
    lv_obj_set_style_text_color(p->status, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(p->status, aos_font_small, 0);
    lv_obj_t *box = lv_obj_create(root);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    p->out = lv_label_create(box);
    lv_obj_set_width(p->out, LV_PCT(100));
    lv_label_set_long_mode(p->out, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(p->out, aos_font_small, 0);
    lv_obj_set_style_text_color(p->out, AOS_C_TEXT, 0);
    lv_label_set_text(p->out, "");
    if (!p4py_start(HEAP_BYTES)) {
        lv_label_set_text(p->out, "MicroPython did not start (memory?)");
    }
    p->timer = lv_timer_create(tick, 50, p);
    return p;
}

static void py_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    py_t *p = inst;
    lv_timer_delete(p->timer);
    p4py_stop();
    lv_free(p->shown);
    lv_free(p);
}

static bool py_init(aos_app_t *app)
{
    app->desc.id       = "aos.python";
    app->desc.name     = "Python";
    app->desc.icon     = LV_SYMBOL_EDIT;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xF5F7FA;
    app->desc.color_b  = 0xD3DCE6;
    app->desc.order    = 205;
    aos_icon_set_ops(app, PY_ICON, sizeof PY_ICON);
    app->create  = py_create;
    app->destroy = py_destroy;
    return true;
}

AOS_APP_ENTRY(py_init);
