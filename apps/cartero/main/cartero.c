/*
 * P4OS - Cartero: an API client in the manner of Postman (README).
 *
 * Requests by hand, their responses looked at in detail, tests in
 * JavaScript, collections that run in order and environments that change
 * the server, on the board's screen and in the portal (web/cartero.js).
 * Both work on the same files, Postman's own formats, in the card's
 * /data/cartero (ct_store.c).
 *
 * This file is the app: its life cycle, the screens it moves between, the
 * jobs it starts (ct_job.c) and what it does when one ends, and the live
 * channel through which the portal's page sends requests from the board.
 * The app lives in the background, so the page can do that while another
 * app is in front.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "ct_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TIMER_MS      250
#define POLL_MS       2000
#define LIVE_MS       1000
#define STOP_WAIT_MS  6000

/* An envelope, its flap, and a little arrow going out. */
static const uint8_t CT_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -4, 4, 62, 44, 7, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, -15, 10, 38, 5, 2, AIC_C_LIT(0x3634A3), 255),
    AIC_ROT(330),
    AIC_RECT(AIC_TOP_MID, 15, 10, 38, 5, 2, AIC_C_LIT(0x3634A3), 255),
    AIC_ROT(-330),
    AIC_OUT,
    AIC_RECT(AIC_CENTER, 30, -26, 26, 26, AIC_CIRCLE, AIC_C_ORANGE, 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, 0, 0, 12, 4, 2, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 3, -3, 8, 4, 2, AIC_C_TEXT, 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, 3, 3, 8, 4, 2, AIC_C_TEXT, 255),
    AIC_ROT(-450),
    AIC_OUT,
    AIC_GRAD(AIC_C_LIT(0x2C2A8C), AIC_GRAD_VER),
    AIC_END
};

ct_app_t ct_app;

static struct {
    lv_timer_t *timer;
    bool        job_seen;       /* the last job's end was handled */
    int         last_web_n;
    bool        web_busy_said;
    uint64_t    estado_t;
    char        estado[96];
} M;

/* ---- screens --------------------------------------------------------------------------------- */

static void close_screen(void)
{
    if (CA.screen == CT_SCR_REQ) ct_req_close();
}

void ct_build(void)
{
    ct_sheet_forget();
    ct_kb_forget();
    lv_obj_clean(CA.root);
    CA.W = lv_obj_get_width(CA.root);
    CA.H = lv_obj_get_height(CA.root);
    CA.land = CA.W > CA.H;
    lv_obj_set_style_bg_color(CA.root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(CA.root, LV_OPA_COVER, 0);
    switch (CA.screen) {
    case CT_SCR_REQ: ct_req_build(); break;
    case CT_SCR_RESP: ct_resp_build(); break;
    case CT_SCR_ENV: ct_env_build(); break;
    case CT_SCR_VARS: ct_vars_build(); break;
    case CT_SCR_HIST: ct_hist_build(); break;
    case CT_SCR_RUN: ct_run_build(); break;
    default: ct_home_build(); break;
    }
}

void ct_go(ct_screen_t s)
{
    if (CA.screen == CT_SCR_REQ && s != CT_SCR_REQ) ct_req_commit();
    close_screen();
    CA.screen = s;
    ct_build();
}

/* a URL kept as plain text (or as raw alone) gets its parts, so the
 * parameters tab has its rows; the file only changes if it is saved */
static void draft_normalize(void)
{
    ct_req_ensure(&CA.dar, CA.draft);
    ctv_t u = ct_get(ct_get(CA.draft, "request"), "url");
    if (u.t == CT_STR || (u.t == CT_OBJ && ct_get(u, "host").t == CT_UNDEF && ct_get(u, "raw").t == CT_STR)) {
        char *raw = ct_alloc(&CA.dar, strlen(ct_req_url(&CA.dar, CA.draft)) + 1);
        if (raw) {
            strcpy(raw, ct_req_url(&CA.dar, CA.draft));
            ct_req_set_url(&CA.dar, CA.draft, raw);
        }
    }
}

static void draft_reset(void)
{
    ct_arena_free(&CA.dar);
    ct_arena_init(&CA.dar, 16u * 1024 * 1024);
    CA.draft = ct_undef();
    CA.has_draft = false;
    CA.dirty = false;
}

void ct_open_request(const ct_ref_t *r)
{
    ctv_t it = ct_node(&CA.st, r);
    if (it.t != CT_OBJ) {
        aos_ui_toast(_("Ese pedido ya no está en la colección"), 2000);
        return;
    }
    draft_reset();
    CA.draft = ct_deep_copy(&CA.dar, it);
    draft_normalize();
    CA.dorig = ct_deep_copy(&CA.dar, CA.draft);
    CA.has_draft = true;
    CA.loose = false;
    CA.dref = *r;
    ct_copy(CA.dname, sizeof CA.dname, ct_item_name(it));
    ct_go(CT_SCR_REQ);
}

void ct_open_loose(ctv_t req_like)
{
    draft_reset();
    if (ct_get(req_like, "request").t != CT_UNDEF) {
        CA.draft = ct_deep_copy(&CA.dar, req_like);
    } else if (req_like.t == CT_OBJ) {
        /* a bare request (the history's): wrapped as an item */
        CA.draft = ct_obj_new(&CA.dar, 4);
        ct_sets(&CA.dar, CA.draft, "name", _("Pedido nuevo"));
        ct_setv(&CA.dar, CA.draft, "request", ct_deep_copy(&CA.dar, req_like));
    } else {
        CA.draft = ct_new_request(&CA.dar, _("Pedido nuevo"));
    }
    draft_normalize();
    CA.dorig = ct_deep_copy(&CA.dar, CA.draft);
    CA.has_draft = true;
    CA.loose = true;
    memset(&CA.dref, 0, sizeof CA.dref);
    CA.dref.coll = -1;
    ct_copy(CA.dname, sizeof CA.dname, ct_item_name(CA.draft));
    ct_go(CT_SCR_REQ);
}

/* ---- jobs -------------------------------------------------------------------------------------- */

static int coll_of(const ct_ref_t *r)
{
    if (r->coll >= 0 && r->coll < CA.st.ncoll && !strcmp(CA.st.coll[r->coll].slug, r->slug)) return r->coll;
    for (int i = 0; i < CA.st.ncoll; i++) if (!strcmp(CA.st.coll[i].slug, r->slug)) return i;
    return -1;
}

/* the variables of the moment, copied into the job */
static void job_vars(ct_job_t *j, int coll)
{
    ct_store_t *st = &CA.st;
    if (st->active >= 0 && st->active < st->nenv) {
        ct_doc_refresh(&st->env[st->active], "entornos");
        j->env = ct_vars_obj(&j->ar, ct_get(st->env[st->active].root, "values"));
        ct_copy(j->env_name, sizeof j->env_name, st->env[st->active].name);
    }
    ct_doc_refresh(&st->glob, NULL);
    j->glob = ct_vars_obj(&j->ar, ct_get(st->glob.root, "values"));
    if (coll >= 0) j->collvars = ct_vars_obj(&j->ar, ct_get(st->coll[coll].root, "variable"));
    j->timeout_ms = st->timeout_ms;
    j->follow = st->follow;
    j->max_body = st->max_kb * 1024;
}

static bool job_busy(void)
{
    return CA.job && !CA.job->done;
}

static void job_replace(ct_job_t *j)
{
    if (CA.job) ct_job_free(CA.job);
    CA.job = j;
    M.job_seen = false;
    CA.view_exec = 0;
}

void ct_send(void)
{
    if (job_busy()) {
        aos_ui_toast(_("Ya hay un pedido en curso"), 1500);
        return;
    }
    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        aos_ui_toast(_("No hay red"), 1500);
        return;
    }
    ct_req_commit();
    ct_job_t *j = ct_job_new(CT_JOB_SEND);
    if (!j) {
        aos_ui_toast(_("No hay memoria"), 1500);
        return;
    }
    int c = CA.loose ? -1 : coll_of(&CA.dref);
    if (c >= 0) {
        ct_doc_refresh(&CA.st.coll[c], "colecciones");
        j->coll = ct_deep_copy(&j->ar, CA.st.coll[c].root);
        j->depth = CA.dref.depth;
        memcpy(j->path, CA.dref.path, sizeof j->path);
    }
    j->item = ct_deep_copy(&j->ar, CA.draft);
    job_vars(j, c);
    ct_copy(j->title, sizeof j->title, ct_item_name(CA.draft));
    if (!ct_job_start(j)) {
        ct_job_free(j);
        aos_ui_toast(_("No se pudo empezar el pedido"), 1500);
        return;
    }
    job_replace(j);
    CA.resp_back = CT_SCR_REQ;
    ct_go(CT_SCR_RESP);
}

void ct_run_start(const ct_ref_t *r, int iterations, int delay_ms, bool stop_on_fail)
{
    if (job_busy()) {
        aos_ui_toast(_("Ya hay un pedido en curso"), 1500);
        return;
    }
    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        aos_ui_toast(_("No hay red"), 1500);
        return;
    }
    int c = coll_of(r);
    if (c < 0) return;
    ct_job_t *j = ct_job_new(CT_JOB_RUN);
    if (!j) return;
    ct_doc_refresh(&CA.st.coll[c], "colecciones");
    j->coll = ct_deep_copy(&j->ar, CA.st.coll[c].root);
    j->depth = r->depth;
    memcpy(j->path, r->path, sizeof j->path);
    j->iterations = iterations < 1 ? 1 : iterations;
    j->delay_ms = delay_ms;
    j->stop_on_fail = stop_on_fail;
    job_vars(j, c);
    ctv_t node = ct_node(&CA.st, r);
    ct_copy(j->title, sizeof j->title, r->depth ? ct_item_name(node) : CA.st.coll[c].name);
    if (!ct_job_start(j)) {
        ct_job_free(j);
        return;
    }
    job_replace(j);
    CA.run_ref = *r;
    ct_go(CT_SCR_RUN);
}

/* a job ended: what its scripts changed goes to the files, the send to the
 * history, the run to its results file */
void ct_job_finished(ct_job_t *j)
{
    int c = -1;
    if (j->kind == CT_JOB_SEND && !CA.loose) c = coll_of(&CA.dref);
    if (j->kind == CT_JOB_RUN) c = coll_of(&CA.run_ref);
    int dropped = ct_store_apply(&CA.st, c, j->changes);
    if (dropped && CA.st.active < 0) {
        aos_ui_toast(_("Los scripts cambiaron variables del entorno, pero no hay ninguno activo"), 3000);
    }
    if (j->kind == CT_JOB_SEND && j->nexec > 0 && j->ex[0]->sent) {
        ct_arena_t a;
        ct_arena_init(&a, 4u * 1024 * 1024);
        ctv_t h = ct_exec_hist(&a, j->ex[0], j->item);
        ct_hist_add(&CA.st, h);
        ct_arena_free(&a);
    }
    if (j->kind == CT_JOB_RUN) {
        ct_sb_t b = { 0 };
        const char *cname = c >= 0 ? CA.st.coll[c].name : "";
        ct_job_results(j, &b, cname, CA.run_ref.depth ? j->title : "");
        char rel[200], path[320], date[32] = "sin-hora", slug[64];
        if (aos_hal_time_is_valid()) {
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(date, sizeof date, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else {
            snprintf(date, sizeof date, "t%lu", (unsigned long)(aos_hal_uptime_ms() / 1000));
        }
        ct_slug(slug, sizeof slug, j->title);
        snprintf(rel, sizeof rel, "corridas/%s-%s.json", date, slug);
        ct_path(path, sizeof path, rel);
        if (!b.oom) ct_write_file(path, b.p, b.n);
        ct_sb_free(&b);
        aos_hal_log(CT_TAG, "run %s: %d requests, %d/%d tests passed, %d ms", j->title, j->nexec, j->passed,
                    j->tests, j->ms);
    }
    if (CA.hidden && j->kind == CT_JOB_RUN) aos_ui_sound(AOS_SND_NOTIF);
}

/* ---- the portal's page: requests through the board --------------------------------------------- */

static void estado_put(bool force)
{
    char b[96];
    bool busy = CA.web && !CA.web->done;
    snprintf(b, sizeof b, "{\"v\":1,\"busy\":%s,\"n\":%d}", busy ? "true" : "false", M.last_web_n);
    uint64_t now = aos_hal_uptime_ms();
    if (!force && !strcmp(b, M.estado) && now - M.estado_t < 5000) return;
    ct_copy(M.estado, sizeof M.estado, b);
    M.estado_t = now;
    aos_hal_live_put(CT_APP_ID, "estado", "application/json", b, strlen(b));
}

static void web_refuse(int n, const char *why)
{
    char b[600];
    char esc[300];
    size_t k = 0;
    for (const char *p = why; *p && k + 2 < sizeof esc; p++) {
        if (*p == '"' || *p == '\\') esc[k++] = '\\';
        esc[k++] = *p;
    }
    esc[k] = '\0';
    snprintf(b, sizeof b, "{\"n\":%d,\"ok\":false,\"status\":0,\"reason\":\"\",\"ms\":0,\"headers\":[],\"size\":0,"
                          "\"body\":\"\",\"file\":\"\",\"error\":\"%s\"}", n, esc);
    aos_hal_live_put(CT_APP_ID, "respuesta", "application/json", b, strlen(b));
}

static void live_take(void)
{
    char msg[AOS_LIVE_MSG_MAX + 1];
    int n;
    while ((n = aos_hal_live_take(CT_APP_ID, msg, sizeof msg - 1)) > 0) {
        msg[n] = '\0';
        if (!strncmp(msg, "send", 4)) {
            int num = atoi(msg + 4);
            if (CA.web && !CA.web->done) {
                web_refuse(num, "la placa todavía está con el pedido anterior");
                continue;
            }
            if (CA.web) {
                ct_job_free(CA.web);
                CA.web = NULL;
            }
            char path[300];
            ct_path(path, sizeof path, "web/pedido.json");
            size_t len = 0;
            char *t = ct_read_file(path, 8u * 1024 * 1024, &len);
            if (!t) {
                web_refuse(num, "no se encontró /data/cartero/web/pedido.json");
                continue;
            }
            char err[160];
            ct_job_t *j = ct_job_web(t, len, err, sizeof err);
            free(t);
            if (!j) {
                web_refuse(num, err);
                continue;
            }
            j->timeout_ms = j->timeout_ms ? j->timeout_ms : CA.st.timeout_ms;
            j->max_body = CA.st.max_kb * 1024;
            if (!ct_job_start(j)) {
                ct_job_free(j);
                web_refuse(num, "no se pudo empezar el pedido");
                continue;
            }
            CA.web = j;
            M.last_web_n = j->web_n;
            estado_put(true);
        } else if (!strncmp(msg, "cancel", 6)) {
            if (CA.web && !CA.web->done) CA.web->stop = true;
        } else if (!strncmp(msg, "reload", 6)) {
            CA.poll_t = 0;
        }
    }
}

/* ---- the timer and the life cycle -------------------------------------------------------------- */

static void jobs_check(void)
{
    if (CA.job && CA.job->done && !M.job_seen) {
        M.job_seen = true;
        ct_job_finished(CA.job);
    }
    if (CA.web && CA.web->done) {
        ct_job_free(CA.web);
        CA.web = NULL;
        estado_put(true);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    jobs_check();
    if (CA.hidden) return;
    uint64_t now = aos_hal_uptime_ms();
    if (now - CA.poll_t >= POLL_MS) {
        CA.poll_t = now;
        bool editing = ct_sheet_open() || ct_kb_visible();
        if (!editing && ct_store_poll(&CA.st)) {
            /* not the variables being edited: what was typed would go */
            if (CA.screen == CT_SCR_HOME || CA.screen == CT_SCR_ENV || CA.screen == CT_SCR_HIST) {
                ct_build();
            }
        }
    }
    if (CA.screen == CT_SCR_RESP) ct_resp_tick();
    else if (CA.screen == CT_SCR_RUN) ct_run_tick();
}

static void tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    jobs_check();
    uint64_t now = aos_hal_uptime_ms();
    /* quick while the page looks: a request through the board waits on this */
    bool looking = aos_hal_live_idle_ms(CT_APP_ID) < 6000;
    if (now - CA.live_t >= (CA.web || looking ? 200 : LIVE_MS)) {
        CA.live_t = now;
        live_take();
        if (looking) estado_put(false);
    }
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (ct_sheet_open()) {
        ct_sheet_close();
        return true;
    }
    if (ct_kb_visible()) {
        ct_kb_hide();
        return true;
    }
    switch (CA.screen) {
    case CT_SCR_REQ:
        if (!ct_req_back()) ct_go(CT_SCR_HOME);
        return true;
    case CT_SCR_RESP:
        if (!ct_resp_back()) ct_go(CA.resp_back == CT_SCR_RUN ? CT_SCR_RUN : CA.has_draft ? CT_SCR_REQ : CT_SCR_HOME);
        return true;
    case CT_SCR_VARS:
        if (!ct_vars_back()) ct_go(CT_SCR_ENV);
        return true;
    case CT_SCR_RUN:
        if (!ct_run_back()) ct_go(CT_SCR_HOME);
        return true;
    case CT_SCR_ENV: case CT_SCR_HIST:
        ct_go(CT_SCR_HOME);
        return true;
    default:
        return false;
    }
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    CA.hidden = true;
    if (CA.screen == CT_SCR_REQ) ct_req_commit();
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    CA.hidden = false;
    CA.poll_t = 0;
    if (CA.screen == CT_SCR_RESP || CA.screen == CT_SCR_RUN) ct_build();
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    CA.root = root;
    if (CA.screen == CT_SCR_REQ) ct_req_commit();
    close_screen();
    ct_build();
    return true;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&CA, 0, sizeof CA);
    memset(&M, 0, sizeof M);
    CA.self = self;
    CA.root = root;
    CA.vars_env = -1;
    ct_arena_init(&CA.dar, 16u * 1024 * 1024);
    CA.st.active = -1;
    ct_store_load(&CA.st);
    CA.poll_t = aos_hal_uptime_ms();
    CA.screen = CT_SCR_HOME;
    ct_build();
    M.timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
    estado_put(true);

    /* Development: CT_OPEN=<collection>/<i>[/<j>] opens a request at once,
     * CT_SCREEN=env|hist|new a screen. */
    const char *open = getenv("CT_OPEN");
    if (open && open[0]) {
        ct_ref_t r = { .coll = -1 };
        const char *slash = strchr(open, '/');
        size_t sl = slash ? (size_t)(slash - open) : strlen(open);
        if (sl < sizeof r.slug) {
            memcpy(r.slug, open, sl);
            r.slug[sl] = '\0';
        }
        while (slash && r.depth < CT_DEPTH_MAX) {
            r.path[r.depth++] = (uint8_t)atoi(slash + 1);
            slash = strchr(slash + 1, '/');
        }
        ct_open_request(&r);
    }
    const char *scr = getenv("CT_SCREEN");
    if (scr && !strcmp(scr, "env")) ct_go(CT_SCR_ENV);
    if (scr && !strcmp(scr, "hist")) ct_go(CT_SCR_HIST);
    if (scr && !strcmp(scr, "new")) ct_open_loose(ct_undef());
    return &CA;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (M.timer) {
        lv_timer_delete(M.timer);
        M.timer = NULL;
    }
    if (CA.screen == CT_SCR_REQ) ct_req_commit();
    close_screen();
    ct_sheet_cancel_async();
    ct_kb_forget();
    lv_obj_clean(CA.root);
    /* the loader unloads the .so on the next tick: no thread may be left in it */
    if (CA.job) CA.job->stop = true;
    if (CA.web) CA.web->stop = true;
    for (int i = 0; i < STOP_WAIT_MS / 20 && ct_job_threads() > 0; i++) aos_hal_sleep_ms(20);
    if (ct_job_threads() > 0) aos_hal_log(CT_TAG, "a job did not stop in time");
    if (CA.job && CA.job->done) ct_job_free(CA.job);
    if (CA.web && CA.web->done) ct_job_free(CA.web);
    CA.job = CA.web = NULL;
    ct_arena_free(&CA.dar);
    ct_store_free(&CA.st);
    aos_hal_live_clear(CT_APP_ID);
}

static bool ct_init(aos_app_t *app)
{
    app->desc.id       = "aos.cartero";
    app->desc.name     = "Cartero";
    app->desc.icon     = AOS_SYM_SEND;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x5E5CE6;
    app->desc.color_b  = 0x2C2A8C;
    app->desc.order    = 134;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND;
    aos_icon_set_ops(app, CT_ICON, sizeof CT_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->show    = show;
    app->hide    = hide;
    app->tick    = tick;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(ct_init);
