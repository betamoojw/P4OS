/*
 * P4OS - Cartero: a collection or a folder run in order (the runner). The
 * requests come in as they are sent, each with its status, its time and its
 * tests; one opens into the response screen. When it ends, it can run
 * again with more iterations, a pause between requests, or stopping at the
 * first failure. The results also go to /data/cartero/corridas/.
 */
#include "ct_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>

static struct {
    lv_obj_t *list, *summary, *bar;
    int       shown;
    bool      was_running;
    int       iterations, delay_idx;
    bool      stop_on_fail;
} U = { .iterations = 1 };

static const int DELAYS[] = { 0, 250, 1000 };

static void open_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!CA.job || i >= CA.job->nexec) return;
    CA.view_exec = i;
    CA.resp_back = CT_SCR_RUN;
    ct_go(CT_SCR_RESP);
}

static void add_row(int i)
{
    const ct_exec_t *e = CA.job->ex[i];
    bool err = e->pre_err[0] || e->test_err[0] || (e->sent && !e->resp.ok);
    bool bad = err || e->nfail;
    lv_obj_t *row = ct_box(U.list, lv_pct(100), 88);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    ct_label(row, e->skipped ? AOS_SYM_MINUS_CIRCLE : bad ? AOS_SYM_CLOSE : AOS_SYM_CHECK, &aos_sym_28,
             e->skipped ? AOS_C_DIM : bad ? AOS_C_RED : AOS_C_GREEN);
    ct_method_badge(row, e->method[0] ? e->method : "GET", 76);
    char safe[160], name[140];
    if (CA.job->iterations > 1) snprintf(name, sizeof name, "%d · %.120s", e->iteration + 1, e->name);
    else ct_copy(name, sizeof name, e->name);
    aos_text_safe(safe, sizeof safe, name);
    lv_obj_t *nl = ct_label_dots(row, safe, aos_font_small, AOS_C_TEXT, CA.W - 2 * AOS_UI_PAD - 420);
    lv_obj_set_flex_grow(nl, 1);
    char t[64], ms[24];
    ct_fmt_ms(ms, sizeof ms, e->resp.t_total);
    if (e->sent && e->resp.ok) snprintf(t, sizeof t, "%d · %s", e->resp.status, ms);
    else if (e->skipped) ct_copy(t, sizeof t, _("salteado"));
    else ct_copy(t, sizeof t, _("error"));
    ct_label(row, t, aos_font_caption, e->resp.ok ? ct_status_color(e->resp.status) : AOS_C_RED);
    if (e->npass + e->nfail) {
        snprintf(t, sizeof t, "%d/%d", e->npass, e->npass + e->nfail);
        ct_label(row, t, aos_font_caption, e->nfail ? AOS_C_RED : AOS_C_GREEN);
    }
}

static void summary_update(void)
{
    const ct_job_t *j = CA.job;
    if (!j || !U.summary) return;
    char t[200], ms[24];
    int ms_now = j->done ? j->ms : 0;
    ct_fmt_ms(ms, sizeof ms, ms_now);
    int reqs = j->nexec;
    if (j->done) {
        snprintf(t, sizeof t, _("%d pedidos · %d de %d tests pasaron · %d con error · %s"), reqs, j->passed, j->tests,
                 j->errors, ms);
    } else {
        snprintf(t, sizeof t, _("%d de %d pedidos · %d de %d tests pasaron"), reqs, j->planned, j->passed, j->tests);
    }
    ct_set_text(U.summary, t);
    lv_obj_set_style_text_color(U.summary, !j->done ? AOS_C_TEXT : (j->failed || j->errors) ? AOS_C_ORANGE : AOS_C_GREEN, 0);
    if (U.bar) {
        lv_bar_set_range(U.bar, 0, j->planned > 0 ? j->planned : 1);
        lv_bar_set_value(U.bar, j->done ? (j->planned > 0 ? j->planned : 1) : reqs, LV_ANIM_OFF);
    }
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    if (CA.job) CA.job->stop = true;
}

static void again_cb(lv_event_t *e)
{
    (void)e;
    ct_run_start(&CA.run_ref, U.iterations, DELAYS[U.delay_idx], U.stop_on_fail);
}

static void iter_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    U.iterations += d;
    if (U.iterations < 1) U.iterations = 1;
    if (U.iterations > 50) U.iterations = 50;
    ct_build();
}

static void delay_cb(lv_event_t *e)
{
    U.delay_idx = (int)(intptr_t)lv_event_get_user_data(e);
    ct_build();
}

static void sof_cb(lv_event_t *e)
{
    U.stop_on_fail = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void options(lv_obj_t *c)
{
    lv_obj_t *card = ct_card(c);
    lv_obj_t *row = ct_box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    ct_label(row, _("Vueltas"), aos_font_body, AOS_C_TEXT);
    ct_round_btn(row, AOS_SYM_MINUS, iter_cb, (void *)(intptr_t)-1);
    char t[16];
    snprintf(t, sizeof t, "%d", U.iterations);
    ct_label(row, t, aos_font_title, AOS_C_TEXT);
    ct_round_btn(row, AOS_SYM_PLUS, iter_cb, (void *)(intptr_t)1);
    ct_label(card, _("Pausa entre pedidos"), aos_font_caption, AOS_C_DIM);
    static const char *const D[] = { "0", "250 ms", "1 s" };
    ct_seg(card, D, 3, U.delay_idx, delay_cb);
    lv_obj_t *r2 = ct_box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ct_label(r2, _("Parar en la primera falla"), aos_font_body, AOS_C_TEXT);
    lv_obj_t *sw = lv_switch_create(r2);
    lv_obj_set_size(sw, 96, 52);
    if (U.stop_on_fail) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, AOS_C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, sof_cb, LV_EVENT_VALUE_CHANGED, NULL);
    ct_btn(card, AOS_SYM_PLAY, _("Correr otra vez"), AOS_C_ACCENT, again_cb, NULL);
}

void ct_run_build(void)
{
    U.list = NULL;
    U.summary = U.bar = NULL;
    U.shown = 0;
    lv_obj_t *page = ct_page();
    const ct_job_t *j = CA.job && CA.job->kind == CT_JOB_RUN ? CA.job : NULL;
    char title[160];
    aos_text_safe(title, sizeof title, j ? j->title : _("Corrida"));
    lv_obj_t *right = ct_header(page, title, true);
    bool running = j && !j->done;
    U.was_running = running;
    if (running) ct_round_btn(right, AOS_SYM_STOP, stop_cb, NULL);
    ct_header_fit(right);
    lv_obj_t *c = ct_scroll(page, 120);
    lv_obj_set_style_pad_row(c, 8, 0);
    if (!j) {
        ct_label_wrap(c, _("No hay ninguna corrida. Se empieza con ▶ en una colección o en el menú de una carpeta."),
                      aos_font_body, AOS_C_DIM);
        return;
    }
    if (j->env_name[0]) {
        char t[140];
        snprintf(t, sizeof t, _("Entorno: %.100s"), j->env_name);
        ct_label(c, t, aos_font_caption, AOS_C_DIM);
    }
    U.summary = ct_label_wrap(c, "", aos_font_small, AOS_C_TEXT);
    U.bar = lv_bar_create(c);
    lv_obj_set_size(U.bar, lv_pct(100), 12);
    lv_obj_set_style_bg_color(U.bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(U.bar, AOS_C_ACCENT, LV_PART_INDICATOR);
    if (!running) lv_obj_add_flag(U.bar, LV_OBJ_FLAG_HIDDEN);
    if (j->note[0] && j->done) ct_label_wrap(c, j->note, aos_font_caption, AOS_C_ORANGE);
    if (!running) options(c);
    U.list = c;
    for (int i = 0; i < j->nexec; i++) add_row(i);
    U.shown = j->nexec;
    summary_update();
}

void ct_run_tick(void)
{
    const ct_job_t *j = CA.job && CA.job->kind == CT_JOB_RUN ? CA.job : NULL;
    if (!j || !U.list) return;
    if (U.was_running && j->done) {
        ct_build();
        return;
    }
    while (U.shown < j->nexec) add_row(U.shown++);
    summary_update();
}

bool ct_run_back(void)
{
    return false;
}
