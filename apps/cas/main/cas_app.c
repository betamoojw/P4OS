/*
 * P4OS - CAS: a computer algebra system on the card.
 *
 * The engine is Giac, the CAS of Xcas, as its author ported it to the
 * calculators (KhiCAS): exact arithmetic, algebra, calculus, equations,
 * matrices, statistics. It is C++ and comes prebuilt by
 * engine/build_engine.sh; cas_rt.c is what joins it to the firmware.
 *
 * This first version is the feasibility test: a line to type into, the
 * history, and a bench that runs a fixed list of computations and logs how
 * long each took, so the board can be measured from the portal's log.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "cas_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A function plotted on its axes: one period of a sine, drawn as two half
 * circles (AIC has arcs, not paths), with the traced point on its maximum. */
static const uint8_t CAS_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   2,  6, 82, AIC_DIV(24), 0, AIC_C_TEXT, 130),       /* x axis */
    AIC_RECT(AIC_CENTER, -36,  0, AIC_DIV(24), 72, 0, AIC_C_TEXT, 130),       /* y axis */
    AIC_ARC(AIC_CENTER, -18, 6, 36, 0, 9, 0, 0, 180, 360, 0,                 /* up     */
            AIC_C_BG, 0, AIC_C_YELLOW, 255),
    AIC_ARC(AIC_CENTER,  18, 6, 36, 0, 9, 0, 0, 0, 180, 0,                   /* down   */
            AIC_C_BG, 0, AIC_C_YELLOW, 255),
    AIC_RECT(AIC_CENTER, -18, -10, 12, 12, AIC_CIRCLE, AIC_C_TEXT, 255),      /* point  */
    AIC_END
};

/* What the bench runs, in order: a bit of everything the app will lean on. */
static const char *const BENCH[] = {
    "1+1",
    "2^200",
    "100!",
    "factor(x^4-1)",
    "expand((x+y)^12)",
    "simplify(sin(x)^2+cos(x)^2)",
    "diff(sin(x)^2*exp(x),x)",
    "integrate(1/(x^4-1),x)",
    "integrate(x*sin(x),x,0,pi)",
    "limit(sin(x)/x,x,0)",
    "series(exp(x),x,0,6)",
    "sum(1/k^2,k,1,inf)",
    "solve(x^3-6*x^2+11*x-6=0,x)",
    "fsolve(cos(x)=x,x=0.5)",
    "det([[1,2,3],[4,5,6],[7,8,10]])",
    "inv([[1,2],[3,4]])",
    "ifactor(2^64+1)",
    "isprime(2^127-1)",
    "evalf(pi,60)",
    "desolve(y'=y,y)",
    "laplace(sin(t),t,s)",
    "mean([1,2,3,4,10])",
    "normald_cdf(0,1,1.96)",
};
#define BENCH_N (int)(sizeof BENCH / sizeof BENCH[0])
#define HIST_MAX 80

typedef struct {
    lv_obj_t   *status;
    lv_obj_t   *list;
    lv_obj_t   *ta;
    lv_obj_t   *kb;
    lv_timer_t *timer;
    bool        started;
    int         bench;          /* next bench line; -1 = not running */
    uint32_t    bench_t0;
    char        pending[160];   /* what is being computed */
} cas_t;

static void set_status(cas_t *c)
{
    char buf[120];
    cas_mem_t m;
    cas_rt_mem(&m);
    const char *state = !c->started    ? _("Motor apagado")
                      : !cas_rt_ready() ? _("Iniciando el motor...")
                      : cas_rt_busy()   ? _("Calculando...")
                      :                   _("Listo");
    snprintf(buf, sizeof buf, "%s  ·  %u / %u KB", state,
             (unsigned)(m.used / 1024), (unsigned)(m.arena / 1024));
    lv_label_set_text(c->status, buf);
}

static void add_line(cas_t *c, const char *text, lv_color_t color, bool right)
{
    while (lv_obj_get_child_count(c->list) >= HIST_MAX) {
        lv_obj_delete(lv_obj_get_child(c->list, 0));
    }
    lv_obj_t *l = lv_label_create(c->list);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_set_style_text_align(l, right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_scroll_to_view(l, LV_ANIM_OFF);
}

static bool submit(cas_t *c, const char *expr)
{
    if (!cas_rt_submit(expr)) {
        return false;
    }
    snprintf(c->pending, sizeof c->pending, "%s", expr);
    add_line(c, expr, AOS_C_DIM, false);
    set_status(c);
    return true;
}

static void poll_cb(lv_timer_t *t)
{
    cas_t *c = lv_timer_get_user_data(t);
    int err = 0;
    uint32_t ms = 0;
    char *r = cas_rt_take(&err, &ms);
    if (r) {
        size_t n = strlen(r) + 32;
        char *line = malloc(n);
        if (line) {
            snprintf(line, n, "%s   (%u ms)", r, (unsigned)ms);
            add_line(c, line, err ? AOS_C_RED : AOS_C_TEXT, true);
            free(line);
        }
        cas_mem_t m;
        cas_rt_mem(&m);
        aos_hal_log("cas", "%s -> %.80s%s | %u ms | arena %u/%u KB peak %u KB%s",
                    c->pending, r, strlen(r) > 80 ? "..." : "", (unsigned)ms,
                    (unsigned)(m.used / 1024), (unsigned)(m.arena / 1024),
                    (unsigned)(m.peak / 1024), err ? " | ERROR" : "");
        free(r);
        if (c->bench >= 0) {
            if (c->bench < BENCH_N) {
                submit(c, BENCH[c->bench++]);
            } else {
                aos_hal_log("cas", "bench done, %d lines in %u ms", BENCH_N,
                            (unsigned)(aos_hal_uptime_ms() - c->bench_t0));
                c->bench = -1;
            }
        }
    }
    static uint32_t last;
    if (aos_hal_uptime_ms() - last > 500) {
        last = aos_hal_uptime_ms();
        set_status(c);
    }
}

static void go_cb(lv_event_t *e)
{
    cas_t *c = lv_event_get_user_data(e);
    const char *text = lv_textarea_get_text(c->ta);
    if (!text[0]) {
        return;
    }
    if (submit(c, text)) {
        lv_textarea_set_text(c->ta, "");
    } else {
        aos_ui_toast(cas_rt_ready() ? _("Esperá a que termine") : _("El motor todavía arranca"), 1500);
    }
}

static void kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        go_cb(e);
    }
}

static void bench_cb(lv_event_t *e)
{
    cas_t *c = lv_event_get_user_data(e);
    if (c->bench >= 0 || cas_rt_busy()) {
        return;
    }
    aos_hal_log("cas", "bench start (engine booted in %u ms)", (unsigned)cas_rt_boot_ms());
    c->bench = 0;
    c->bench_t0 = aos_hal_uptime_ms();
    submit(c, BENCH[c->bench++]);
}

static void stop_cb(lv_event_t *e)
{
    cas_t *c = lv_event_get_user_data(e);
    c->bench = -1;
    cas_rt_interrupt();
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, cas_t *c)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_set_style_pad_ver(b, 12, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, c);
    return b;
}

static void build(cas_t *c, lv_obj_t *root)
{
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 16, 0);
    lv_obj_set_style_pad_row(root, 10, 0);

    c->status = lv_label_create(root);
    lv_obj_set_style_text_color(c->status, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(c->status, aos_font_small, 0);

    c->list = lv_obj_create(root);
    lv_obj_set_width(c->list, lv_pct(100));
    lv_obj_set_flex_grow(c->list, 1);
    lv_obj_set_flex_flow(c->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(c->list, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(c->list, 0, 0);
    lv_obj_set_style_radius(c->list, 18, 0);
    lv_obj_set_style_pad_all(c->list, 14, 0);
    lv_obj_set_style_pad_row(c->list, 6, 0);

    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    c->ta = lv_textarea_create(row);
    lv_textarea_set_one_line(c->ta, true);
    lv_textarea_set_placeholder_text(c->ta, "factor(x^4-1)");
    lv_obj_set_flex_grow(c->ta, 1);
    lv_obj_set_style_text_font(c->ta, aos_font_body, 0);
    button(row, "=", AOS_C_ACCENT, go_cb, c);

    lv_obj_t *row2 = lv_obj_create(root);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row2, 10, 0);
    button(row2, _("Banco de pruebas"), AOS_C_CARD2, bench_cb, c);
    button(row2, _("Parar"), AOS_C_RED, stop_cb, c);

    c->kb = lv_keyboard_create(root);
    lv_obj_set_size(c->kb, lv_pct(100), lv_pct(32));
    lv_keyboard_set_textarea(c->kb, c->ta);
    lv_obj_add_event_cb(c->kb, kb_cb, LV_EVENT_READY, c);

    set_status(c);
}

static void *cas_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    cas_t *c = lv_malloc_zeroed(sizeof(cas_t));
    if (!c) {
        return NULL;
    }
    c->bench = -1;
    build(c, root);
    c->started = cas_rt_start();
    if (!c->started) {
        add_line(c, _("No se pudo arrancar el motor. Cerrá la app y volvé a abrirla."),
                 AOS_C_RED, false);
    }
    c->timer = lv_timer_create(poll_cb, 40, c);
    return c;
}

static bool cas_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    cas_t *c = inst;
    /* the history is LVGL objects: rebuilt empty, the engine keeps its state */
    build(c, root);
    return true;
}

static void cas_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    cas_t *c = inst;
    lv_timer_delete(c->timer);
    cas_rt_stop();
    lv_free(c);
}

static bool cas_init(aos_app_t *app)
{
    app->desc.id       = "aos.cas";              /* before the icon: it is keyed by id */
    app->desc.name     = "CAS";
    app->desc.icon     = LV_SYMBOL_EDIT;         /* the fallback, if the blob were refused */
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x5856D6;
    app->desc.color_b  = 0x2B2A7A;
    app->desc.order    = 150;
    app->desc.flags    = AOS_APP_FLAG_KEEP;
    aos_icon_set_ops(app, CAS_ICON, sizeof CAS_ICON);

    app->create  = cas_create;
    app->destroy = cas_destroy;
    app->resize  = cas_resize;
    return true;
}

AOS_APP_ENTRY(cas_init);
