/*
 * P4OS - CAS: a computer algebra system on the card.
 *
 * The engine is Giac, the CAS of Xcas, as its author ported it to the
 * calculators (KhiCAS): exact arithmetic, algebra, calculus, equations,
 * matrices, statistics. It is C++ and comes prebuilt by
 * engine/build_engine.sh; cas_rt.c joins it to the firmware.
 *
 * Four views under a strip of tabs: the calculator (cas_calc.c), the
 * grapher (cas_graph.c), statistics (cas_stats.c) and finance (cas_fin.c).
 * They share the maths core (mx_*.c), the forms (cas_form.c), the plotting
 * canvas (cas_plot.c) and the engine, which takes one job at a time from
 * the queue here.
 */
#include "cas_app.h"
#include "cas_form.h"
#include "cas_plot.h"
#include "cas_rt.h"
#include "cas_save.h"
#include "mx.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

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

/* The bench: a bit of everything, timed in the log. */
static const char *const BENCH[] = {
    "1+1", "2^200", "100!", "factor(x^4-1)", "expand((x+y)^12)",
    "simplify(sin(x)^2+cos(x)^2)", "diff(sin(x)^2*exp(x),x)", "integrate(1/(x^4-1),x)",
    "integrate(x*sin(x),x,0,pi)", "limit(sin(x)/x,x,0)", "series(exp(x),x,0,6)",
    "sum(1/k^2,k,1,inf)", "solve(x^3-6*x^2+11*x-6=0,x)", "fsolve(cos(x)=x,x=0.5)",
    "det([[1,2,3],[4,5,6],[7,8,10]])", "inv([[1,2],[3,4]])", "ifactor(2^64+1)",
    "isprime(2^127-1)", "evalf(pi,60)", "desolve(y'=y,y)", "laplace(sin(t),t,s)",
    "mean([1,2,3,4,10])", "normald_cdf(0,1,1.96)",
};
#define BENCH_N (int)(sizeof BENCH / sizeof BENCH[0])

/* --- the job queue ------------------------------------------------------------- */

#define QMAX 64

typedef struct {
    char        *expr;
    cas_job_cb_t cb;
    void        *user;
} job_t;

static struct {
    aos_app_t  *app;
    job_t       q[QMAX];
    int         head, count;
    job_t       running;
    bool        busy;
    bool        started;
    lv_timer_t *timer;
    lv_obj_t   *root, *content, *status;
    int         view;           /* CAS_V_* */
    uint32_t    bench_t0;
    int         bench_left;
} A;

bool cas_job(const char *expr, cas_job_cb_t cb, void *user)
{
    if (!A.started || A.count == QMAX) {
        return false;
    }
    job_t *j = &A.q[(A.head + A.count) % QMAX];
    j->expr = mx_strdup(expr);
    j->cb = cb;
    j->user = user;
    A.count++;
    return j->expr != NULL;
}

void cas_job_cancel(void *user)
{
    for (int i = 0; i < A.count; i++) {
        job_t *j = &A.q[(A.head + i) % QMAX];
        if (j->user == user) {
            j->cb = NULL;
        }
    }
    if (A.busy && A.running.user == user) {
        A.running.cb = NULL;
    }
}

bool cas_engine_ok(void)
{
    return A.started;
}

void cas_engine_stop_job(void)
{
    cas_rt_interrupt();
}

static void set_status(void)
{
    if (!A.status) {
        return;
    }
    cas_mem_t m;
    cas_rt_mem(&m);
    const char *state = !A.started      ? _("Motor apagado")
                      : !cas_rt_ready() ? _("Iniciando...")
                      : A.busy          ? _("Calculando...")
                      :                   "";
    (void)m;
    /* a pill over the view, only while there is something to say */
    if (strcmp(lv_label_get_text(A.status), state)) {
        lv_label_set_text(A.status, state);
    }
    if (state[0]) {
        lv_obj_remove_flag(A.status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(A.status, LV_OBJ_FLAG_HIDDEN);
    }
}

/* --- the portal's page (apps/cas/web/cas.js) ------------------------------------
 * It sends one line at a time on the live channel:
 *   calc <expression>     typed into the calculator, as if on the board
 *   graph <1..6> <expr>   a function of the grapher ("" clears it)
 *   stats                 the lists file was written: read it again
 *   view <n>              show a view (CAS_V_*)
 * and reads, while it looks: "state" (the view, the engine, where the plot
 * on screen is), "hist" (the history) and "graph" (the functions), each
 * put again only when it changed. */

#define LIVE_ID "aos.cas"

static void show_view(int v);

static void live_take(void)
{
    char msg[AOS_LIVE_MSG_MAX + 1];
    int n;
    while ((n = aos_hal_live_take(LIVE_ID, msg, sizeof msg - 1)) > 0) {
        msg[n] = 0;
        msg[strcspn(msg, "\r\n")] = 0;
        if (!strncmp(msg, "calc ", 5)) {
            if (A.view != CAS_V_CALC) show_view(CAS_V_CALC);
            cas_calc_submit_text(msg + 5);
        } else if (!strncmp(msg, "graph ", 6) && msg[6] >= '1' && msg[6] <= '6') {
            cas_graph_set(msg[6] - '1', msg[7] == ' ' ? msg + 8 : "");
        } else if (!strcmp(msg, "stats")) {
            cas_stats_reload();
        } else if (!strncmp(msg, "view ", 5)) {
            int v = atoi(msg + 5);
            if (v >= 0 && v < CAS_V_COUNT && v != A.view) show_view(v);
        }
    }
}

static void live_put(void)
{
    static uint32_t last_state;
    static unsigned hist_ver = ~0u, graph_ver = ~0u;
    if (aos_hal_live_idle_ms(LIVE_ID) > 5000) {
        hist_ver = graph_ver = ~0u;     /* the next page to look gets everything */
        return;
    }
    uint32_t now = aos_hal_uptime_ms();
    if (now - last_state >= 400) {
        last_state = now;
        char j[200];
        lv_area_t a;
        int k = snprintf(j, sizeof j, "{\"view\":%d,\"ready\":%s,\"busy\":%s,\"plot\":", A.view,
                         cas_rt_ready() ? "true" : "false", A.busy || A.count ? "true" : "false");
        if (cas_plot_on_screen(&a)) {
            snprintf(j + k, sizeof j - (size_t)k, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}}",
                     (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
        } else {
            snprintf(j + k, sizeof j - (size_t)k, "null}");
        }
        aos_hal_live_put(LIVE_ID, "state", "application/json", j, strlen(j));
    }
    if (cas_calc_version() != hist_ver) {
        hist_ver = cas_calc_version();
        mx_str_t b = { 0 };
        cas_calc_json(&b);
        if (b.s) aos_hal_live_put(LIVE_ID, "hist", "application/json", b.s, b.n);
        mx_str_free(&b);
    }
    if (cas_graph_version() != graph_ver) {
        graph_ver = cas_graph_version();
        mx_str_t b = { 0 };
        cas_graph_json(&b);
        if (b.s) aos_hal_live_put(LIVE_ID, "graph", "application/json", b.s, b.n);
        mx_str_free(&b);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (A.busy) {
        int err = 0;
        uint32_t ms = 0;
        char *r = cas_rt_take(&err, &ms);
        if (r) {
            A.busy = false;
            aos_hal_log("cas", "%.60s -> %.80s%s | %u ms%s", A.running.expr, r,
                        strlen(r) > 80 ? "..." : "", (unsigned)ms, err ? " | ERROR" : "");
            if (A.running.cb) {
                A.running.cb(A.running.user, r, err != 0, ms);
            }
            free(r);
            mx_free(A.running.expr);
            A.running.expr = NULL;
            if (A.bench_left > 0 && --A.bench_left == 0) {
                aos_hal_log("cas", "bench done, %d lines in %u ms", BENCH_N,
                            (unsigned)(aos_hal_uptime_ms() - A.bench_t0));
            }
        }
    }
    if (!A.busy && A.count && cas_rt_ready()) {
        job_t *j = &A.q[A.head];
        A.head = (A.head + 1) % QMAX;
        A.count--;
        if (!j->cb) {
            mx_free(j->expr);
        } else if (cas_rt_submit(j->expr)) {
            A.running = *j;
            A.busy = true;
        } else {
            j->cb(j->user, _("El motor no aceptó el cálculo"), true, 0);
            mx_free(j->expr);
        }
    }
    static uint32_t last;
    if (aos_hal_uptime_ms() - last > 500) {
        last = aos_hal_uptime_ms();
        set_status();
    }
    live_take();
    live_put();
}

/* --- the shell ----------------------------------------------------------------- */

static void build_views(void);

static void show_view(int v)
{
    A.view = v;
    build_views();
}

static void show_async(void *v)
{
    if (A.root && (int)(intptr_t)v != A.view) {
        show_view((int)(intptr_t)v);
    }
}

void cas_app_show(int view)
{
    /* the caller is usually a button of the view going away: switch after
     * its event is over */
    lv_async_call(show_async, (void *)(intptr_t)view);
}

static void tab_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v != A.view) {
        show_view(v);
    }
}

static void scrim_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        lv_obj_delete_async(lv_event_get_current_target(e));
    }
}

static void menu_cb(lv_event_t *e)
{
    lv_obj_t *mb = lv_event_get_current_target(e);
    const char *what = lv_obj_get_user_data(mb);
    lv_obj_delete_async(lv_obj_get_parent(lv_obj_get_parent(mb)));
    if (!strcmp(what, "bench")) {
        if (A.view != CAS_V_CALC) {
            show_view(CAS_V_CALC);
        }
        aos_hal_log("cas", "bench start (engine booted in %u ms)", (unsigned)cas_rt_boot_ms());
        A.bench_t0 = aos_hal_uptime_ms();
        A.bench_left = BENCH_N;
        for (int i = 0; i < BENCH_N; i++) {
            cas_calc_submit_text(BENCH[i]);
        }
    } else if (!strcmp(what, "clear")) {
        cas_calc_clear();
    } else if (!strcmp(what, "stop")) {
        cas_rt_interrupt();
    }
}

static void menu_open(lv_event_t *e)
{
    (void)e;
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(scrim);
    lv_obj_set_size(box, 420, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_RIGHT, -16, 90);
    lv_obj_set_style_bg_color(box, AOS_C_CARD2, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    cas_mem_t m;
    cas_rt_mem(&m);
    if (m.arena) {
        char mb[64];
        snprintf(mb, sizeof mb, _("Memoria del motor: %u KB"), (unsigned)(m.used / 1024));
        lv_obj_t *ml = lv_label_create(box);
        lv_label_set_text(ml, mb);
        lv_obj_set_style_text_color(ml, AOS_C_DIM, 0);
        lv_obj_set_style_text_font(ml, aos_font_small, 0);
        lv_obj_set_style_pad_all(ml, 8, 0);
    }
    static const char *const ITEMS[][2] = {
        { "stop", N_("Parar el cálculo") },
        { "clear", N_("Borrar el historial") },
        { "bench", N_("Banco de pruebas") },
    };
    for (size_t i = 0; i < sizeof ITEMS / sizeof ITEMS[0]; i++) {
        lv_obj_t *b = lv_button_create(box);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_pad_ver(b, 16, 0);
        lv_obj_set_user_data(b, (void *)ITEMS[i][0]);
        lv_obj_add_event_cb(b, menu_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, _(ITEMS[i][1]));
        lv_obj_set_style_text_font(l, aos_font_body, 0);
    }
}

static lv_obj_t *tab_button(lv_obj_t *bar, const char *text, int v)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_set_height(b, 52);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, v == A.view ? AOS_C_ACCENT : AOS_C_CARD, 0);
    lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, mx_font(20), 0);      /* five tabs and the menu in 720 px */
    lv_obj_center(l);
    return b;
}

/* the view that is leaving lets go of what it holds outside its widgets */
static void leave_view(void)
{
    cas_graph_shown(false);
    cas_geo_shown(false);
    cas_stats_hidden();
    cas_fin_hidden();
    cas_sci_hidden();
}

static void build_views(void)
{
    lv_obj_t *root = A.root;
    leave_view();
    lv_obj_clean(root);
    bool landscape = lv_obj_get_width(root) > lv_obj_get_height(root);
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 12, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 56);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* five tabs: in German or on a narrow screen they slide */
    lv_obj_t *tabs = lv_obj_create(bar);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_height(tabs, 56);
    lv_obj_set_flex_grow(tabs, 1);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, 6, 0);
    lv_obj_set_scroll_dir(tabs, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tabs, LV_SCROLLBAR_MODE_OFF);
    static const char *const TABS[CAS_V_COUNT] = { N_("Cálculo"), N_("Gráfico"), N_("Geometría"), N_("Estadística"), N_("Finanzas"), N_("Ciencias") };
    lv_obj_t *lit = NULL;
    for (int v = 0; v < CAS_V_COUNT; v++) {
        lv_obj_t *b = tab_button(tabs, _(TABS[v]), v);
        if (v == A.view) lit = b;
    }
    lv_obj_t *menu = lv_button_create(bar);
    lv_obj_set_size(menu, 56, 52);
    lv_obj_set_style_radius(menu, 26, 0);
    lv_obj_set_style_shadow_width(menu, 0, 0);
    lv_obj_set_style_bg_color(menu, AOS_C_CARD, 0);
    lv_obj_add_event_cb(menu, menu_open, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(menu);
    lv_label_set_text(ml, LV_SYMBOL_LIST);
    lv_obj_center(ml);
    lv_obj_update_layout(bar);
    if (lit) {
        lv_obj_scroll_to_view(lit, LV_ANIM_OFF);
    }

    A.content = lv_obj_create(root);
    lv_obj_remove_style_all(A.content);
    lv_obj_set_width(A.content, LV_PCT(100));
    lv_obj_set_flex_grow(A.content, 1);
    lv_obj_remove_flag(A.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(root);

    switch (A.view) {
    case CAS_V_CALC:
        cas_calc_build(A.content, landscape);
        break;
    case CAS_V_GRAPH:
        cas_graph_build(A.content, landscape);
        cas_graph_shown(true);
        break;
    case CAS_V_GEO:
        cas_geo_build(A.content, landscape);
        cas_geo_shown(true);
        break;
    case CAS_V_STATS:
        cas_stats_build(A.content, landscape);
        break;
    case CAS_V_FIN:
        cas_fin_build(A.content, landscape);
        break;
    default:
        cas_sci_build(A.content, landscape);
        break;
    }

    /* the engine's state, over the view while it has something to say */
    A.status = lv_label_create(root);
    lv_obj_add_flag(A.status, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(A.status, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(A.status, LV_OPA_90, 0);
    lv_obj_set_style_radius(A.status, 16, 0);
    lv_obj_set_style_pad_hor(A.status, 14, 0);
    lv_obj_set_style_pad_ver(A.status, 6, 0);
    lv_obj_set_style_text_color(A.status, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(A.status, aos_font_small, 0);
    lv_label_set_text(A.status, "");
    lv_obj_align(A.status, LV_ALIGN_TOP_RIGHT, -76, 70);
    lv_obj_add_flag(A.status, LV_OBJ_FLAG_HIDDEN);
    set_status();
}

/* The grapher needs the screen's edges for its fingers: the back gesture
 * is off while it is shown (aos_app.h, desc.flags can change live). */
void cas_app_set_gestures(bool graph)
{
    if (!A.app) {
        return;
    }
    if (graph) {
        A.app->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    } else {
        A.app->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
    }
}

static void *cas_create(aos_app_t *self, lv_obj_t *root)
{
    A.app = self;
    A.root = root;
    A.started = cas_rt_start();
    if (!A.started) {
        aos_ui_toast(_("No se pudo arrancar el motor. Cerrá la app y volvé a abrirla."), 3000);
    }
    build_views();
    A.timer = lv_timer_create(poll_cb, 30, NULL);
    return &A;
}

static bool cas_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    A.root = root;
    build_views();
    return true;
}

static void cas_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    lv_timer_delete(A.timer);
    A.timer = NULL;
    aos_hal_live_clear(LIVE_ID);
    for (int v = 0; v < CAS_V_COUNT; v++) {
        lv_async_call_cancel(show_async, (void *)(intptr_t)v);
    }
    leave_view();
    cas_entry_free();
    cas_text_free();
    cas_calc_free();
    cas_graph_free();
    cas_geo_free();
    cas_stats_free();
    cas_fin_free();
    cas_sci_free();
    for (int i = 0; i < A.count; i++) {
        mx_free(A.q[(A.head + i) % QMAX].expr);
    }
    A.count = 0;
    A.head = 0;
    mx_free(A.running.expr);
    A.running.expr = NULL;
    A.busy = false;
    cas_rt_stop();
    cas_save_wait();
    A.status = A.content = A.root = NULL;
    A.app = NULL;
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
