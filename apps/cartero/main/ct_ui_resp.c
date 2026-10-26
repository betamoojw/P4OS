/*
 * P4OS - Cartero: a response looked at. The status, how long each part
 * took, the size; then the body (a JSON tree that opens by levels, laid
 * out, or raw), the headers, the tests, the console and what was sent.
 *
 * A value of the tree can go into a variable of the environment or the
 * globals with two taps: the way a token or an id is taken from one
 * answer to use in the next request.
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

#define SHOW_TEXT   (32 * 1024)      /* what a label holds of a body */
#define ROWS_MAX    500
#define KIDS_MAX    150

enum { RT_BODY, RT_HEADERS, RT_TESTS, RT_CONSOLE, RT_SENT, RT_N };

typedef struct {
    ctv_t v;
    char  path[200];
    char  key[64];
} jrow_t;

static struct {
    int       tab, body_mode;        /* 0 tree, 1 laid out, 2 raw */
    lv_obj_t *wait_label;
    bool      was_running;
    void    **open;                  /* the containers of the tree that are open */
    int       nopen, capopen;
    jrow_t   *rows;
    int       nrows;
    const ct_job_t *for_job;
    int       for_exec;
    jrow_t    pick;
} R;

static const ct_exec_t *cur_exec(void)
{
    if (!CA.job || CA.view_exec < 0 || CA.view_exec >= CA.job->nexec) return NULL;
    return CA.job->ex[CA.view_exec];
}

static void forget_tree_if_new(void)
{
    if (R.for_job == CA.job && R.for_exec == CA.view_exec) return;
    R.for_job = CA.job;
    R.for_exec = CA.view_exec;
    R.nopen = 0;
    R.body_mode = 0;
}

static bool opened(const void *p)
{
    for (int i = 0; i < R.nopen; i++) if (R.open[i] == p) return true;
    return false;
}

static void toggle(const void *p)
{
    for (int i = 0; i < R.nopen; i++) {
        if (R.open[i] == p) {
            R.open[i] = R.open[--R.nopen];
            return;
        }
    }
    if (R.nopen == R.capopen) {
        int cap = R.capopen ? R.capopen * 2 : 32;
        void **n = ct_psram_realloc(R.open, sizeof(void *) * (size_t)cap);
        if (!n) return;
        R.open = n;
        R.capopen = cap;
    }
    R.open[R.nopen++] = (void *)p;
}

static const void *cont_id(ctv_t v)
{
    return v.t == CT_ARR ? (const void *)v.a : v.t == CT_OBJ ? (const void *)v.o : NULL;
}

/* ---- the header and the times ------------------------------------------------------------------- */

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    if (CA.job) CA.job->stop = true;
}

static void times_bar(lv_obj_t *c, const ct_resp_t *r)
{
    int total = r->t_connect + r->t_send + r->t_wait + r->t_download;
    if (total <= 0) return;
    int32_t w = CA.W - 2 * AOS_UI_PAD - 40;
    lv_obj_t *bar = ct_box(c, w, 16);
    lv_obj_set_style_radius(bar, 8, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_set_style_bg_color(bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    const int part[4] = { r->t_connect, r->t_send, r->t_wait, r->t_download };
    const lv_color_t col[4] = { AOS_C_TEAL, AOS_C_PURPLE, AOS_C_ORANGE, AOS_C_GREEN };
    static const char *const NAME[4] = { N_("Conexión"), N_("Envío"), N_("Espera"), N_("Descarga") };
    for (int i = 0; i < 4; i++) {
        int32_t pw = (int32_t)((int64_t)w * part[i] / total);
        if (part[i] > 0 && pw < 3) pw = 3;
        lv_obj_t *seg = ct_box(bar, pw, 16);
        lv_obj_set_style_bg_color(seg, col[i], 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }
    lv_obj_t *leg = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(leg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(leg, 14, 0);
    for (int i = 0; i < 4; i++) {
        char t[64], ms[24];
        ct_fmt_ms(ms, sizeof ms, part[i]);
        snprintf(t, sizeof t, "%s %s", _(NAME[i]), ms);
        lv_obj_t *dot = ct_box(leg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(dot, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(dot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(dot, 6, 0);
        lv_obj_t *d = ct_box(dot, 12, 12);
        lv_obj_set_style_radius(d, 6, 0);
        lv_obj_set_style_bg_color(d, col[i], 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        ct_label(dot, t, aos_font_caption, AOS_C_DIM);
    }
}

static void status_card(lv_obj_t *c, const ct_exec_t *e)
{
    const ct_resp_t *r = &e->resp;
    lv_obj_t *card = ct_card(c);
    char t[160];
    if (e->skipped) {
        ct_label_wrap(card, _("El script de antes salteó el pedido (skipRequest)."), aos_font_body, AOS_C_DIM);
        return;
    }
    if (!e->sent) {
        ct_label_wrap(card, _("No se envió: falló el script de antes."), aos_font_body, AOS_C_RED);
        return;
    }
    if (!r->ok) {
        ct_label_wrap(card, ct_err_text(r), aos_font_body, AOS_C_RED);
        if (r->err[0]) {
            char safe[260];
            aos_text_safe(safe, sizeof safe, r->err);
            ct_label_wrap(card, safe, aos_font_caption, AOS_C_DIM);
        }
        if (r->t_total) {
            char ms[24];
            ct_fmt_ms(ms, sizeof ms, r->t_total);
            snprintf(t, sizeof t, _("después de %s"), ms);
            ct_label(card, t, aos_font_caption, AOS_C_DIM);
        }
        return;
    }
    lv_obj_t *row = ct_box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 22, 0);
    snprintf(t, sizeof t, "%d %.40s", r->status, r->reason);
    ct_label(row, t, aos_font_title, ct_status_color(r->status));
    char ms[24], sz[24];
    ct_fmt_ms(ms, sizeof ms, r->t_total);
    ct_fmt_size(sz, sizeof sz, r->len);
    ct_label(row, ms, aos_font_body, AOS_C_TEXT);
    snprintf(t, sizeof t, "%s%s", sz, r->cut ? _(" (cortado)") : "");
    ct_label(row, t, aos_font_body, r->cut ? AOS_C_ORANGE : AOS_C_TEXT);
    times_bar(card, r);
    if (r->redirects) {
        char safe[600];
        aos_text_safe(safe, sizeof safe, r->final_url);
        snprintf(t, sizeof t, r->redirects == 1 ? _("%d redirección, terminó en:") : _("%d redirecciones, terminó en:"),
                 r->redirects);
        ct_label_wrap(card, t, aos_font_caption, AOS_C_DIM);
        ct_label_wrap(card, safe, &aos_mono_18, AOS_C_DIM);
    }
}

/* ---- the JSON tree ---------------------------------------------------------------------------- */

static void tree_toggle_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= R.nrows) return;
    toggle(cont_id(R.rows[i].v));
    ct_build();
}

static void var_named(const char *name, void *ud)
{
    if (!name[0]) return;
    bool glob = ud != NULL;
    if (!glob && CA.st.active < 0) {
        aos_ui_toast(_("No hay un entorno activo: elegilo en el inicio"), 2000);
        return;
    }
    ct_arena_t a;
    ct_arena_init(&a, 4u * 1024 * 1024);
    ctv_t ch = ct_arr_new(&a, 1), one = ct_arr_new(&a, 3);
    ct_arr_push(&a, one.a, ct_cstr(&a, glob ? "globals" : "environment"));
    ct_arr_push(&a, one.a, ct_cstr(&a, name));
    ctv_t v = R.pick.v;
    if (v.t != CT_STR && v.t != CT_NUM && v.t != CT_BOOL) {
        ct_sb_t b = { 0 };
        ct_json_write(&b, v, 0);
        v = ct_strv(&a, b.p ? b.p : "", b.n);
        ct_sb_free(&b);
    }
    ct_arr_push(&a, one.a, ct_deep_copy(&a, v));
    ct_arr_push(&a, ch.a, one);
    ct_store_apply(&CA.st, -1, ch);
    ct_arena_free(&a);
    char t[160];
    snprintf(t, sizeof t, glob ? _("Guardado en las globales: %.80s") : _("Guardado en el entorno: %.80s"), name);
    aos_ui_toast(t, 1800);
}

static void leaf_pick(int i, void *ud)
{
    (void)ud;
    const char *def = R.pick.key[0] && R.pick.key[0] != '[' ? R.pick.key : "valor";
    if (i == 0) ct_ask(_("Nombre de la variable del entorno"), def, var_named, NULL);
    else if (i == 1) ct_ask(_("Nombre de la variable global"), def, var_named, (void *)1);
}

static void leaf_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= R.nrows) return;
    R.pick = R.rows[i];
    char title[480];
    ct_sb_t b = { 0 };
    if (R.pick.v.t == CT_STR) ct_sb_add(&b, R.pick.v.s->p, R.pick.v.s->len > 200 ? 200 : R.pick.v.s->len);
    else ct_json_write(&b, R.pick.v, 0);
    char val[220];
    aos_text_safe(val, sizeof val, b.p ? b.p : "");
    ct_sb_free(&b);
    snprintf(title, sizeof title, "pm.response.json()%s\n= %.200s", R.pick.path, val);
    const char *items[2] = { _("Guardar en una variable del entorno"), _("Guardar en una variable global") };
    ct_sheet(title, items, 2, -1, leaf_pick, NULL);
}

static void value_text(ctv_t v, char *out, size_t n, lv_color_t *c)
{
    switch (v.t) {
    case CT_STR: {
        char tmp[200];
        size_t k = v.s->len < sizeof tmp - 1 ? v.s->len : sizeof tmp - 1;
        memcpy(tmp, v.s->p, k);
        tmp[k] = '\0';
        for (char *p = tmp; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        snprintf(out, n, "\"%s\"", tmp);
        *c = AOS_C_GREEN;
        break;
    }
    case CT_NUM: ct_num_fmt(v.n, out); *c = lv_color_hex(0x64D2FF); break;
    case CT_BOOL: ct_copy(out, n, v.b ? "true" : "false"); *c = AOS_C_PURPLE; break;
    case CT_NULL: ct_copy(out, n, "null"); *c = AOS_C_DIM; break;
    case CT_ARR: snprintf(out, n, "[ %u ]", (unsigned)v.a->n); *c = AOS_C_DIM; break;
    case CT_OBJ: snprintf(out, n, "{ %u }", (unsigned)v.o->n); *c = AOS_C_DIM; break;
    default: ct_copy(out, n, ""); *c = AOS_C_DIM; break;
    }
}

static void tree_rows(lv_obj_t *list, ctv_t v, const char *key, const char *path, int depth)
{
    if (R.nrows >= ROWS_MAX) return;
    int i = R.nrows++;
    jrow_t *jr = &R.rows[i];
    jr->v = v;
    ct_copy(jr->path, sizeof jr->path, path);
    ct_copy(jr->key, sizeof jr->key, key);
    bool cont = v.t == CT_ARR || v.t == CT_OBJ;
    bool open = cont && opened(cont_id(v));
    lv_obj_t *row = ct_box(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 52, 0);
    lv_obj_set_style_pad_left(row, 6 + depth * 26, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cont ? tree_toggle_cb : leaf_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (cont) ct_label(row, open ? AOS_SYM_CHEVRON_DOWN : AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
    else ct_box(row, 28, 28);
    int32_t avail = CA.W - 2 * AOS_UI_PAD - 50 - depth * 26;
    if (key[0]) {
        char k[90], safe[100];
        snprintf(k, sizeof k, "%s:", key);
        aos_text_safe(safe, sizeof safe, k);
        lv_obj_t *kl = ct_label(row, safe, &aos_mono_22, lv_color_hex(0xD1D1D6));
        lv_obj_set_style_max_width(kl, avail / 2, 0);
        lv_label_set_long_mode(kl, LV_LABEL_LONG_MODE_DOTS);
    }
    char val[240], safe[260];
    lv_color_t col;
    value_text(v, val, sizeof val, &col);
    aos_text_safe(safe, sizeof safe, val);
    lv_obj_t *vl = ct_label(row, safe, &aos_mono_22, col);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(vl, 1);
    lv_obj_set_height(vl, lv_font_get_line_height(&aos_mono_22));
    if (!open) return;
    uint32_t n = v.t == CT_ARR ? v.a->n : v.o->n;
    for (uint32_t k = 0; k < n && k < KIDS_MAX && R.nrows < ROWS_MAX; k++) {
        char ck[64], cp[200];
        ctv_t child;
        if (v.t == CT_ARR) {
            snprintf(ck, sizeof ck, "[%u]", (unsigned)k);
            snprintf(cp, sizeof cp, "%.180s[%u]", path, (unsigned)k);
            child = v.a->v[k];
        } else {
            ct_str_t *ks = v.o->p[k].k;
            snprintf(ck, sizeof ck, "%.60s", ks->p);
            bool plain = ks->len > 0;
            for (uint32_t q = 0; q < ks->len && plain; q++) {
                char ch = ks->p[q];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9' && q) || ch == '_' ||
                      ch == '$')) {
                    plain = false;
                }
            }
            if (plain) snprintf(cp, sizeof cp, "%.150s.%.45s", path, ks->p);
            else snprintf(cp, sizeof cp, "%.150s[\"%.40s\"]", path, ks->p);
            child = v.o->p[k].v;
        }
        tree_rows(list, child, ck, cp, depth + 1);
    }
    if (n > KIDS_MAX) {
        char t[120];
        snprintf(t, sizeof t, _("… y %u más (el resto, en Crudo o en el portal)"), (unsigned)(n - KIDS_MAX));
        lv_obj_t *l = ct_label(list, t, aos_font_caption, AOS_C_DIM);
        lv_obj_set_style_pad_left(l, 40 + depth * 26, 0);
    }
}

/* ---- the body as text ------------------------------------------------------------------------- */

static void text_block(lv_obj_t *c, const char *text, size_t n)
{
    size_t show = n < SHOW_TEXT ? n : SHOW_TEXT;
    char *tmp = ct_psram(show + 1);
    char *safe = ct_psram(show * 2 + 16);
    if (!tmp || !safe) {
        free(tmp);
        free(safe);
        return;
    }
    memcpy(tmp, text, show);
    tmp[show] = '\0';
    for (size_t i = 0; i < show; i++) {
        unsigned char ch = (unsigned char)tmp[i];
        if (ch < 0x20 && ch != '\n' && ch != '\t') tmp[i] = ch == '\r' ? ' ' : '.';
        if (ch == '\t') tmp[i] = ' ';
    }
    aos_text_safe(safe, show * 2 + 16, tmp);
    lv_obj_t *box = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_t *l = ct_label(box, safe, &aos_mono_18, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    free(tmp);
    free(safe);
    if (n > SHOW_TEXT) {
        char t[160], a[24], b[24];
        ct_fmt_size(a, sizeof a, SHOW_TEXT);
        ct_fmt_size(b, sizeof b, (long)n);
        snprintf(t, sizeof t, _("Se muestran %s de %s: el resto, guardándolo o en el portal."), a, b);
        ct_label_wrap(c, t, aos_font_caption, AOS_C_DIM);
    }
}

static void body_mode_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    const ct_exec_t *x = cur_exec();
    /* the chips are the ones there are: map back to the mode */
    if (x && !x->json_ok) m += 1;
    R.body_mode = m;
    ct_build();
}

static void tab_body(lv_obj_t *c, const ct_exec_t *e)
{
    const ct_resp_t *r = &e->resp;
    if (!r->ok) return;
    if (!r->len || !r->body) {
        ct_label_wrap(c, _("La respuesta no trae cuerpo."), aos_font_body, AOS_C_DIM);
        return;
    }
    bool text = ct_body_is_text(r);
    const char *ct = ct_resp_header(r, "Content-Type");
    if (!text) {
        char t[200], sz[24];
        ct_fmt_size(sz, sizeof sz, r->len);
        snprintf(t, sizeof t, _("Cuerpo binario (%.60s, %s). Guardalo en la tarjeta (⬇ arriba) para abrirlo, o miralo en el portal."),
                 ct ? ct : "?", sz);
        ct_label_wrap(c, t, aos_font_body, AOS_C_DIM);
        return;
    }
    const char *modes[3];
    int nm = 0, sel;
    if (e->json_ok) modes[nm++] = _("Árbol");
    bool looks_json = e->json_ok || (r->body[0] == '{' || r->body[0] == '[');
    if (looks_json) modes[nm++] = _("Ordenado");
    modes[nm++] = _("Crudo");
    if (!e->json_ok && R.body_mode == 0) R.body_mode = looks_json ? 1 : 2;
    if (!looks_json) R.body_mode = 2;
    sel = e->json_ok ? R.body_mode : R.body_mode - 1;
    if (sel >= nm) sel = nm - 1;
    ct_seg(c, modes, nm, sel, body_mode_cb);
    if (R.body_mode == 0 && e->json_ok) {
        if (!R.rows) R.rows = ct_psram(sizeof(jrow_t) * ROWS_MAX);
        if (!R.rows) return;
        R.nrows = 0;
        /* the first look opens the top, and a lone object or array under it */
        if (!R.nopen) {
            toggle(cont_id(e->json));
            if (e->json.t == CT_OBJ && e->json.o->n <= 3) {
                for (uint32_t i = 0; i < e->json.o->n; i++) {
                    ctv_t ch = e->json.o->p[i].v;
                    if ((ch.t == CT_ARR || ch.t == CT_OBJ) && !opened(cont_id(ch))) toggle(cont_id(ch));
                }
            }
        }
        lv_obj_t *box = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(box, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 18, 0);
        lv_obj_set_style_pad_all(box, 10, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        tree_rows(box, e->json, "", "", 0);
        ct_label_wrap(c, _("Tocá un valor para guardarlo en una variable."), aos_font_caption, AOS_C_DIM);
        return;
    }
    if (R.body_mode == 1) {
        ct_sb_t b = { 0 };
        size_t n = (size_t)r->len < 2 * SHOW_TEXT ? (size_t)r->len : 2 * SHOW_TEXT;
        if (ct_json_pretty((const char *)r->body, (size_t)r->len, &b, 2) && b.p) {
            text_block(c, b.p, b.n);
        } else {
            text_block(c, (const char *)r->body, n);
        }
        ct_sb_free(&b);
        return;
    }
    text_block(c, (const char *)r->body, (size_t)r->len);
}

/* ---- the other tabs ----------------------------------------------------------------------------- */

static void kv_line(lv_obj_t *c, const char *k, const char *v)
{
    lv_obj_t *box = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(box, 6, 0);
    char safe[600];
    aos_text_safe(safe, sizeof safe, k);
    ct_label_wrap(box, safe, &aos_mono_18, lv_color_hex(0xD1D1D6));
    aos_text_safe(safe, sizeof safe, v);
    ct_label_wrap(box, safe, &aos_mono_22, AOS_C_TEXT);
}

static void tab_headers(lv_obj_t *c, const ct_exec_t *e)
{
    const ct_resp_t *r = &e->resp;
    if (!r->ok) return;
    lv_obj_t *card = ct_card(c);
    char t[120];
    snprintf(t, sizeof t, "%s %d %s", r->version, r->status, r->reason);
    ct_label(card, t, &aos_mono_18, AOS_C_DIM);
    for (int i = 0; i < r->nhdr; i++) kv_line(card, r->hdr[i].k, r->hdr[i].v);
}

static void tab_tests(lv_obj_t *c, const ct_exec_t *e)
{
    char t[200];
    if (e->pre_err[0]) {
        lv_obj_t *card = ct_card(c);
        ct_label(card, _("Script de antes"), aos_font_caption, AOS_C_DIM);
        char safe[300];
        aos_text_safe(safe, sizeof safe, e->pre_err);
        ct_label_wrap(card, safe, &aos_mono_18, e->sent ? AOS_C_ORANGE : AOS_C_RED);
    }
    if (e->test_err[0]) {
        lv_obj_t *card = ct_card(c);
        ct_label(card, _("Script de tests"), aos_font_caption, AOS_C_DIM);
        char safe[300];
        aos_text_safe(safe, sizeof safe, e->test_err);
        ct_label_wrap(card, safe, &aos_mono_18, AOS_C_RED);
    }
    int n = (int)ct_len(e->tests);
    if (!n) {
        ct_label_wrap(c, _("Este pedido no tiene tests. Se escriben en Scripts, Tests: pm.test(\"nombre\", () => ...)."),
                      aos_font_body, AOS_C_DIM);
        return;
    }
    snprintf(t, sizeof t, _("%d de %d pasaron"), e->npass, e->npass + e->nfail);
    ct_label(c, t, aos_font_body, e->nfail ? AOS_C_ORANGE : AOS_C_GREEN);
    for (int i = 0; i < n; i++) {
        ctv_t x = ct_at(e->tests, (uint32_t)i);
        bool ok = ct_truthy(ct_get(x, "ok")), skip = ct_truthy(ct_get(x, "skipped"));
        lv_obj_t *row = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_pad_all(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 12, 0);
        ct_label(row, skip ? AOS_SYM_MINUS_CIRCLE : ok ? AOS_SYM_CHECK : AOS_SYM_CLOSE, &aos_sym_28,
                 skip ? AOS_C_DIM : ok ? AOS_C_GREEN : AOS_C_RED);
        lv_obj_t *col = ct_box(row, CA.W - 2 * AOS_UI_PAD - 90, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        char safe[400];
        aos_text_safe(safe, sizeof safe, ct_s(ct_get(x, "name")));
        ct_label_wrap(col, safe, aos_font_small, AOS_C_TEXT);
        const char *err = ct_s(ct_get(x, "error"));
        if (err[0] && !ok) {
            aos_text_safe(safe, sizeof safe, err);
            ct_label_wrap(col, safe, &aos_mono_18, AOS_C_RED);
        }
    }
}

static void tab_console(lv_obj_t *c, const ct_exec_t *e)
{
    if (!e->console || !e->console[0]) {
        ct_label_wrap(c, _("Nada en la consola. Los scripts escriben con console.log(...)."), aos_font_body, AOS_C_DIM);
        return;
    }
    lv_obj_t *card = ct_card(c);
    int lines = 0;
    for (const char *p = e->console; *p && lines < 300; lines++) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[500];
        size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, k);
        line[k] = '\0';
        lv_color_t col = line[0] == 'w' ? AOS_C_ORANGE : line[0] == 'e' ? AOS_C_RED : line[0] == 'i' ? AOS_C_TEAL : AOS_C_TEXT;
        char safe[600];
        aos_text_safe(safe, sizeof safe, k > 2 ? line + 2 : "");
        ct_label_wrap(card, safe, &aos_mono_18, col);
        p = nl ? nl + 1 : p + n;
    }
}

static void tab_sent(lv_obj_t *c, const ct_exec_t *e)
{
    if (!e->sent) return;
    lv_obj_t *card = ct_card(c);
    char safe[1200];
    char t[1100];
    snprintf(t, sizeof t, "%s %.1000s", e->method, e->url ? e->url : "");
    aos_text_safe(safe, sizeof safe, t);
    ct_label_wrap(card, safe, &aos_mono_22, ct_method_color(e->method));
    for (uint32_t i = 0; i < ct_len(e->sent_hdr); i++) {
        ctv_t h = ct_at(e->sent_hdr, i);
        kv_line(card, ct_s(ct_at(h, 0)), ct_s(ct_at(h, 1)));
    }
    if (e->sent_len > 0 && e->sent_body) {
        ct_label(c, _("Cuerpo enviado"), aos_font_caption, AOS_C_DIM);
        text_block(c, e->sent_body, (size_t)e->sent_len);
    }
}

/* ---- saving the body ------------------------------------------------------------------------------ */

static void save_named(const char *name, void *ud)
{
    (void)ud;
    const ct_exec_t *e = cur_exec();
    if (!e || !name[0] || !e->resp.body) return;
    char slug[64], rel[160], path[320];
    ct_slug(slug, sizeof slug, name);
    const char *ct = ct_resp_header(&e->resp, "Content-Type");
    const char *ext = !ct ? "txt" : strstr(ct, "json") ? "json" : strstr(ct, "xml") ? "xml" : strstr(ct, "html") ? "html"
                      : strstr(ct, "png") ? "png" : strstr(ct, "jpeg") ? "jpg" : strstr(ct, "gif") ? "gif"
                      : strstr(ct, "pdf") ? "pdf" : strstr(ct, "csv") ? "csv" : !strncmp(ct, "text/", 5) ? "txt" : "bin";
    snprintf(rel, sizeof rel, "respuestas/%s.%s", slug, ext);
    ct_path(path, sizeof path, rel);
    bool ok = ct_write_file(path, e->resp.body, (size_t)e->resp.len);
    char t[200];
    snprintf(t, sizeof t, ok ? _("Guardado en /data/cartero/%s") : _("No se pudo guardar %s"), rel);
    aos_ui_toast(t, 2500);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    const ct_exec_t *x = cur_exec();
    if (!x || !x->resp.ok || !x->resp.len) {
        aos_ui_toast(_("No hay cuerpo para guardar"), 1500);
        return;
    }
    ct_ask(_("Nombre del archivo"), x->name, save_named, NULL);
}

static void tab_cb(lv_event_t *e)
{
    R.tab = (int)(intptr_t)lv_event_get_user_data(e);
    ct_build();
}

void ct_resp_build(void)
{
    forget_tree_if_new();
    lv_obj_t *page = ct_page();
    const ct_exec_t *e = cur_exec();
    bool running = CA.job && !CA.job->done && CA.job->kind == CT_JOB_SEND;
    R.was_running = running;
    char title[160];
    aos_text_safe(title, sizeof title, e ? e->name : CA.job ? CA.job->title : _("Respuesta"));
    lv_obj_t *right = ct_header(page, title, true);
    if (e && e->resp.ok && e->resp.len) ct_round_btn(right, AOS_SYM_DOWNLOAD, save_cb, NULL);
    ct_header_fit(right);
    lv_obj_t *c = ct_scroll(page, 120);
    R.wait_label = NULL;
    if (running || !e) {
        lv_obj_t *card = ct_card(c);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (!CA.job) {
            ct_label_wrap(card, _("Todavía no se mandó nada."), aos_font_body, AOS_C_DIM);
            return;
        }
        lv_obj_t *sp = lv_spinner_create(card);
        lv_obj_set_size(sp, 80, 80);
        lv_obj_set_style_arc_color(sp, AOS_C_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(sp, 8, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(sp, 8, 0);
        R.wait_label = ct_label(card, _("Enviando…"), aos_font_body, AOS_C_TEXT);
        ct_btn(card, AOS_SYM_CLOSE, _("Cancelar"), AOS_C_CARD2, cancel_cb, NULL);
        return;
    }
    status_card(c, e);
    const char *tabs[RT_N];
    char lab[RT_N][48];
    static const char *const TABS[RT_N] = { N_("Cuerpo"), N_("Encabezados"), N_("Tests"), N_("Consola"), N_("Enviado") };
    for (int i = 0; i < RT_N; i++) {
        if (i == RT_HEADERS && e->resp.ok) snprintf(lab[i], sizeof lab[i], "%s %d", _(TABS[i]), e->resp.nhdr);
        else if (i == RT_TESTS && (e->npass + e->nfail)) snprintf(lab[i], sizeof lab[i], "%s %d/%d", _(TABS[i]), e->npass, e->npass + e->nfail);
        else if (i == RT_TESTS && (e->pre_err[0] || e->test_err[0])) snprintf(lab[i], sizeof lab[i], "%s !", _(TABS[i]));
        else ct_copy(lab[i], sizeof lab[i], _(TABS[i]));
        tabs[i] = lab[i];
    }
    if ((e->nfail || e->test_err[0] || (e->pre_err[0] && !e->sent)) && R.tab == RT_BODY && !e->resp.ok) R.tab = RT_TESTS;
    lv_obj_t *seg = ct_seg(c, tabs, RT_N, R.tab, tab_cb);
    if (e->nfail || e->test_err[0]) {
        lv_obj_t *tc = lv_obj_get_child(seg, RT_TESTS);
        lv_obj_t *tl = tc ? lv_obj_get_child(tc, 0) : NULL;
        if (tl && R.tab != RT_TESTS) lv_obj_set_style_text_color(tl, AOS_C_RED, 0);
    }
    switch (R.tab) {
    case RT_HEADERS: tab_headers(c, e); break;
    case RT_TESTS: tab_tests(c, e); break;
    case RT_CONSOLE: tab_console(c, e); break;
    case RT_SENT: tab_sent(c, e); break;
    default: tab_body(c, e); break;
    }
}

void ct_resp_tick(void)
{
    bool running = CA.job && !CA.job->done && CA.job->kind == CT_JOB_SEND;
    if (R.was_running && !running) {
        R.tab = RT_BODY;
        R.nopen = 0;
        R.for_job = NULL;
        forget_tree_if_new();
        ct_build();
        return;
    }
    if (running && R.wait_label) {
        static uint64_t t0;
        static const ct_job_t *for_job;
        if (for_job != CA.job) {
            for_job = CA.job;
            t0 = aos_hal_uptime_ms();
        }
        char t[64], ms[24];
        ct_fmt_ms(ms, sizeof ms, (int)(aos_hal_uptime_ms() - t0));
        snprintf(t, sizeof t, "%s %s", _("Enviando…"), ms);
        ct_set_text(R.wait_label, t);
    }
}

bool ct_resp_back(void)
{
    return false;
}
