/*
 * P4OS - Cartero: the first screen (the collections as a tree, the
 * environment in use) and the history.
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
#include <time.h>

#define TREE_OPEN_MAX  64

/* which collections and folders are open, by "slug/i/j": kept while the
 * app lives, so going into a request and back finds the tree as it was */
static char s_open[TREE_OPEN_MAX][80];
static int  s_nopen;

static void ref_key(const ct_ref_t *r, char *out, size_t n)
{
    int k = snprintf(out, n, "%.60s", r->slug);
    for (int i = 0; i < r->depth && k < (int)n - 4; i++) k += snprintf(out + k, n - (size_t)k, "/%u", r->path[i]);
}

static bool is_open(const ct_ref_t *r)
{
    char k[80];
    ref_key(r, k, sizeof k);
    for (int i = 0; i < s_nopen; i++) if (!strcmp(s_open[i], k)) return true;
    return false;
}

static void toggle_open(const ct_ref_t *r)
{
    char k[80];
    ref_key(r, k, sizeof k);
    for (int i = 0; i < s_nopen; i++) {
        if (!strcmp(s_open[i], k)) {
            memmove(s_open[i], s_open[i + 1], sizeof s_open[0] * (size_t)(s_nopen - i - 1));
            s_nopen--;
            return;
        }
    }
    if (s_nopen == TREE_OPEN_MAX) {
        memmove(s_open[0], s_open[1], sizeof s_open[0] * (TREE_OPEN_MAX - 1));
        s_nopen--;
    }
    ct_copy(s_open[s_nopen++], sizeof s_open[0], k);
}

/* the refs the rows hand to their callbacks live here until the next build */
static ct_ref_t *s_refs;
static int       s_nrefs, s_caprefs;

static ct_ref_t *keep_ref(const ct_ref_t *r)
{
    if (s_nrefs == s_caprefs) {
        int cap = s_caprefs ? s_caprefs * 2 : 64;
        ct_ref_t *n = ct_psram_realloc(s_refs, sizeof(ct_ref_t) * (size_t)cap);
        if (!n) return NULL;
        s_refs = n;
        s_caprefs = cap;
    }
    s_refs[s_nrefs] = *r;
    return &s_refs[s_nrefs++];
}

/* ---- the environment -------------------------------------------------------------------------- */

static void env_pick(int i, void *ud)
{
    (void)ud;
    if (i == CA.st.nenv + 1) {
        ct_go(CT_SCR_ENV);
        return;
    }
    CA.st.active = i == 0 ? -1 : i - 1;
    ct_state_save(&CA.st);
    ct_build();
}

static void env_cb(lv_event_t *e)
{
    (void)e;
    int n = CA.st.nenv + 2;
    const char **items = malloc(sizeof(char *) * (size_t)n);
    if (!items) return;
    items[0] = _("Sin entorno");
    for (int i = 0; i < CA.st.nenv; i++) items[i + 1] = CA.st.env[i].name;
    items[n - 1] = _("Editar los entornos…");
    ct_sheet(_("Entorno"), items, n, CA.st.active + 1, env_pick, NULL);
    free(items);
}

/* ---- the menus of a collection, a folder, a request ------------------------------------------- */

static ct_ref_t s_menu_ref;

static void new_folder_named(const char *name, void *ud)
{
    (void)ud;
    if (!name[0]) return;
    if (!ct_coll_add_folder(&CA.st, &s_menu_ref, name)) aos_ui_toast(_("No se pudo guardar"), 1500);
    else if (!is_open(&s_menu_ref)) toggle_open(&s_menu_ref);
    ct_build();
}

static void new_request_named(const char *name, void *ud)
{
    (void)ud;
    if (!name[0]) return;
    ct_arena_t a;
    ct_arena_init(&a, 1 << 20);
    ctv_t it = ct_new_request(&a, name);
    ct_ref_t out;
    bool ok = ct_coll_add(&CA.st, &s_menu_ref, it, &out);
    ct_arena_free(&a);
    if (!ok) {
        aos_ui_toast(_("No se pudo guardar"), 1500);
        return;
    }
    if (!is_open(&s_menu_ref)) toggle_open(&s_menu_ref);
    ct_open_request(&out);
}

static void delete_yes(int c, void *ud)
{
    (void)ud;
    if (c != 1) return;
    bool ok;
    if (s_menu_ref.depth == 0) {
        int i;
        for (i = 0; i < CA.st.ncoll; i++) if (!strcmp(CA.st.coll[i].slug, s_menu_ref.slug)) break;
        ok = ct_coll_delete(&CA.st, i);
    } else {
        ok = ct_coll_remove(&CA.st, &s_menu_ref);
    }
    if (!ok) aos_ui_toast(_("No se pudo borrar"), 1500);
    ct_build();
}

static void duplicate(void)
{
    ctv_t it = ct_node(&CA.st, &s_menu_ref);
    if (it.t != CT_OBJ) return;
    ct_arena_t a;
    ct_arena_init(&a, 8u * 1024 * 1024);
    ctv_t copy = ct_deep_copy(&a, it);
    char name[128];
    snprintf(name, sizeof name, _("%s (copia)"), ct_item_name(it));
    ct_sets(&a, copy, "name", name);
    ct_obj_del(copy.o, "id", 2);
    ct_ref_t parent = s_menu_ref;
    parent.depth--;
    if (!ct_coll_add(&CA.st, &parent, copy, NULL)) aos_ui_toast(_("No se pudo guardar"), 1500);
    ct_arena_free(&a);
    ct_build();
}

enum { MN_RUN, MN_NEWREQ, MN_NEWFOLDER, MN_DUP, MN_DEL, MN_OPEN };

static int s_menu_map[8];

static void menu_pick(int i, void *ud)
{
    (void)ud;
    switch (s_menu_map[i]) {
    case MN_RUN: ct_run_start(&s_menu_ref, 1, 0, false); break;
    case MN_NEWREQ: ct_ask(_("Nombre del pedido"), _("Pedido nuevo"), new_request_named, NULL); break;
    case MN_NEWFOLDER: ct_ask(_("Nombre de la carpeta"), "", new_folder_named, NULL); break;
    case MN_DUP: duplicate(); break;
    case MN_OPEN: ct_open_request(&s_menu_ref); break;
    case MN_DEL: {
        char t[200];
        ctv_t it = ct_node(&CA.st, &s_menu_ref);
        const char *nm = s_menu_ref.depth ? ct_item_name(it) : CA.st.coll[s_menu_ref.coll].name;
        snprintf(t, sizeof t, _("¿Borrar «%.120s»? No se puede deshacer."), nm);
        ct_confirm(t, _("Borrar"), delete_yes, NULL);
        break;
    }
    }
}

static void menu_open(const ct_ref_t *r, bool folder)
{
    s_menu_ref = *r;
    const char *items[6];
    int n = 0;
    if (folder) {
        s_menu_map[n] = MN_RUN;
        items[n++] = r->depth ? _("Correr la carpeta") : _("Correr la colección");
        s_menu_map[n] = MN_NEWREQ;
        items[n++] = _("Pedido nuevo acá");
        s_menu_map[n] = MN_NEWFOLDER;
        items[n++] = _("Carpeta nueva acá");
    } else {
        s_menu_map[n] = MN_OPEN;
        items[n++] = _("Abrir");
        s_menu_map[n] = MN_DUP;
        items[n++] = _("Duplicar");
    }
    s_menu_map[n] = MN_DEL;
    items[n++] = _("Borrar");
    ctv_t it = ct_node(&CA.st, r);
    char title[120];
    ct_copy(title, sizeof title, r->depth ? ct_item_name(it) : CA.st.coll[r->coll].name);
    ct_sheet(title, items, n, -1, menu_pick, NULL);
}

static void row_cb(lv_event_t *e)
{
    ct_ref_t *r = lv_event_get_user_data(e);
    if (!r) return;
    ctv_t it = ct_node(&CA.st, r);
    if (r->depth == 0 || ct_is_folder(it)) {
        toggle_open(r);
        ct_build();
    } else {
        ct_open_request(r);
    }
}

static void row_long_cb(lv_event_t *e)
{
    ct_ref_t *r = lv_event_get_user_data(e);
    if (!r) return;
    ctv_t it = ct_node(&CA.st, r);
    menu_open(r, r->depth == 0 || ct_is_folder(it));
    /* the long press is not also a click */
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);
}

static void more_cb(lv_event_t *e)
{
    ct_ref_t *r = lv_event_get_user_data(e);
    if (r) menu_open(r, true);
}

static void run_cb(lv_event_t *e)
{
    ct_ref_t *r = lv_event_get_user_data(e);
    if (r) ct_run_start(r, 1, 0, false);
}

/* ---- the tree -------------------------------------------------------------------------------- */

static lv_obj_t *row_box(lv_obj_t *list, int depth, ct_ref_t *r)
{
    lv_obj_t *row = ct_box(list, lv_pct(100), 84);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(row, 20 + depth * 34, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_SHORT_CLICKED, r);
    lv_obj_add_event_cb(row, row_long_cb, LV_EVENT_LONG_PRESSED, r);
    return row;
}

static lv_obj_t *small_btn(lv_obj_t *row, const char *glyph, lv_color_t c, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ct_box(row, 72, 72);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = ct_label(b, glyph, &aos_sym_28, c);
    lv_obj_center(l);
    return b;
}

static int count_requests(ctv_t node)
{
    int n = 0;
    ctv_t items = ct_items(node);
    for (uint32_t i = 0; i < ct_len(items); i++) {
        if (ct_is_folder(items.a->v[i])) n += count_requests(items.a->v[i]);
        else n++;
    }
    return n;
}

static void tree(lv_obj_t *list, ctv_t node, ct_ref_t *at, int32_t w)
{
    ctv_t items = ct_items(node);
    for (uint32_t i = 0; i < ct_len(items) && at->depth < CT_DEPTH_MAX; i++) {
        ctv_t it = items.a->v[i];
        ct_ref_t r = *at;
        r.path[r.depth++] = (uint8_t)i;
        ct_ref_t *kr = keep_ref(&r);
        if (!kr) return;
        lv_obj_t *row = row_box(list, r.depth, kr);
        char safe[160];
        aos_text_safe(safe, sizeof safe, ct_item_name(it));
        int32_t avail = w - 40 - r.depth * 34;
        if (ct_is_folder(it)) {
            bool open = is_open(&r);
            ct_label(row, open ? AOS_SYM_FOLDER_OPEN : AOS_SYM_FOLDER, &aos_sym_28, AOS_C_YELLOW);
            lv_obj_t *nm = ct_label_dots(row, safe, aos_font_body, AOS_C_TEXT, avail - 40 - 90 - 30);
            lv_obj_set_flex_grow(nm, 1);
            small_btn(row, AOS_SYM_DOTS_HORIZONTAL, AOS_C_DIM, more_cb, kr);
            if (open) tree(list, it, &r, w);
        } else {
            ct_method_badge(row, ct_req_method(it), 76);
            lv_obj_t *nm = ct_label_dots(row, safe, aos_font_body, AOS_C_TEXT, avail - 90);
            lv_obj_set_flex_grow(nm, 1);
        }
    }
}

static void coll_named(const char *name, void *ud)
{
    (void)ud;
    if (!name[0]) return;
    int c = ct_coll_new(&CA.st, name);
    if (c < 0) {
        aos_ui_toast(_("No se pudo crear la colección"), 1500);
        return;
    }
    ct_ref_t r = { .coll = c };
    ct_copy(r.slug, sizeof r.slug, CA.st.coll[c].slug);
    if (!is_open(&r)) toggle_open(&r);
    ct_build();
}

static void plus_pick2(int i, void *ud)
{
    (void)ud;
    if (i == 0) ct_open_loose(ct_undef());
    else if (i == 1) ct_ask(_("Nombre de la colección"), "", coll_named, NULL);
    else if (i == 2) ct_go(CT_SCR_ENV);
}

static void plus_cb(lv_event_t *e)
{
    (void)e;
    const char *items[] = { _("Pedido nuevo (suelto)"), _("Colección nueva"), _("Entornos y variables") };
    ct_sheet(NULL, items, 3, -1, plus_pick2, NULL);
}

static void hist_cb(lv_event_t *e)
{
    (void)e;
    ct_go(CT_SCR_HIST);
}

static void envs_cb(lv_event_t *e)
{
    (void)e;
    ct_go(CT_SCR_ENV);
}

static void last_cb(lv_event_t *e)
{
    (void)e;
    if (CA.job && CA.job->kind == CT_JOB_RUN) ct_go(CT_SCR_RUN);
    else if (CA.job) {
        CA.resp_back = CT_SCR_HOME;
        ct_go(CT_SCR_RESP);
    }
}

void ct_home_build(void)
{
    s_nrefs = 0;
    lv_obj_t *page = ct_page();
    lv_obj_t *right = ct_header(page, _("Cartero"), false);
    ct_round_btn(right, AOS_SYM_CLOCK_OUTLINE, hist_cb, NULL);
    ct_round_btn(right, AOS_SYM_EARTH, envs_cb, NULL);
    ct_round_btn(right, AOS_SYM_PLUS, plus_cb, NULL);
    ct_header_fit(right);

    /* the environment in use, as a chip */
    lv_obj_t *bar = ct_box(page, CA.W - 2 * AOS_UI_PAD, 64);
    lv_obj_set_pos(bar, AOS_UI_PAD, 118);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);
    char t[160];
    const char *en = ct_env_name(&CA.st);
    if (en[0]) snprintf(t, sizeof t, "%s  " LV_SYMBOL_DOWN, en);
    else snprintf(t, sizeof t, "%s  " LV_SYMBOL_DOWN, _("Sin entorno"));
    lv_obj_t *chip = ct_chip(bar, t, false, env_cb, NULL);
    lv_obj_t *cl = lv_obj_get_child(chip, 0);
    if (cl) lv_obj_set_style_text_color(cl, en[0] ? AOS_C_ACCENT : AOS_C_DIM, 0);
    if (CA.job) {
        const char *what = CA.job->kind == CT_JOB_RUN ? _("Última corrida") : _("Última respuesta");
        ct_chip(bar, what, false, last_cb, NULL);
    }

    lv_obj_t *list = ct_scroll(page, 196);
    lv_obj_set_style_pad_row(list, 8, 0);
    int32_t w = CA.W - 2 * AOS_UI_PAD;
    for (int c = 0; c < CA.st.ncoll; c++) {
        ct_doc_t *d = &CA.st.coll[c];
        ct_ref_t r = { .coll = c };
        ct_copy(r.slug, sizeof r.slug, d->slug);
        ct_ref_t *kr = keep_ref(&r);
        if (!kr) break;
        bool open = is_open(&r);
        lv_obj_t *row = row_box(list, 0, kr);
        lv_obj_set_height(row, 96);
        lv_obj_set_style_margin_top(row, c ? 10 : 0, 0);
        ct_label(row, open ? AOS_SYM_CHEVRON_DOWN : AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
        lv_obj_t *col = ct_box(row, w - 40 - 40 - 2 * 86, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        char safe[160];
        aos_text_safe(safe, sizeof safe, d->name);
        ct_label_dots(col, safe, aos_font_body, AOS_C_TEXT, w - 40 - 40 - 2 * 86);
        char sub[96];
        if (d->bad) {
            snprintf(sub, sizeof sub, _("No se pudo leer: %.60s"), d->err);
        } else {
            int n = count_requests(d->root);
            snprintf(sub, sizeof sub, n == 1 ? _("%d pedido") : _("%d pedidos"), n);
        }
        ct_label_dots(col, sub, aos_font_caption, d->bad ? AOS_C_RED : AOS_C_DIM, w - 40 - 40 - 2 * 86);
        if (!d->bad) {
            small_btn(row, AOS_SYM_PLAY, AOS_C_GREEN, run_cb, kr);
            small_btn(row, AOS_SYM_DOTS_HORIZONTAL, AOS_C_DIM, more_cb, kr);
        }
        if (open && !d->bad) {
            tree(list, d->root, &r, w);
            if (!ct_len(ct_items(d->root))) {
                lv_obj_t *l = ct_label_wrap(list, _("Vacía. Agregá un pedido desde el menú ⋯ de la colección."),
                                            aos_font_caption, AOS_C_DIM);
                lv_obj_set_style_pad_left(l, 40, 0);
            }
        }
    }
    if (!CA.st.ncoll) {
        lv_obj_t *l = ct_label_wrap(list, _("Todavía no hay colecciones.\n\nCreá un pedido suelto o una colección con +, o importá una colección de Postman desde la página Cartero del portal (los archivos son los de Postman)."),
                                    aos_font_body, AOS_C_DIM);
        lv_obj_set_style_margin_top(l, 20, 0);
        lv_obj_t *row = ct_box(list, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(row, 14, 0);
        lv_obj_set_style_margin_top(row, 16, 0);
        ct_btn(row, AOS_SYM_PLUS, _("Crear"), AOS_C_ACCENT, plus_cb, NULL);
    }
}

/* ---- the history ------------------------------------------------------------------------------- */

static void hist_open_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ctv_t h = ct_at(CA.st.hist.root, (uint32_t)i);
    if (h.t != CT_OBJ) return;
    ct_arena_t a;
    ct_arena_init(&a, 8u * 1024 * 1024);
    ctv_t it = ct_obj_new(&a, 3);
    const char *nm = ct_s(ct_get(h, "name"));
    ct_sets(&a, it, "name", nm[0] ? nm : _("Pedido nuevo"));
    ct_setv(&a, it, "request", ct_deep_copy(&a, ct_get(h, "request")));
    ct_open_loose(it);
    ct_arena_free(&a);
}

static void hist_clear_yes(int c, void *ud)
{
    (void)ud;
    if (c != 1) return;
    ct_hist_clear(&CA.st);
    ct_build();
}

static void hist_clear_cb(lv_event_t *e)
{
    (void)e;
    ct_confirm(_("¿Borrar todo el historial?"), _("Borrar"), hist_clear_yes, NULL);
}

static void ago(char *out, size_t n, double t)
{
    if (t <= 0 || !aos_hal_time_is_valid()) {
        out[0] = '\0';
        return;
    }
    long d = (long)(time(NULL) - (time_t)t);
    if (d < 60) ct_copy(out, n, _("recién"));
    else if (d < 3600) snprintf(out, n, _("hace %ld min"), d / 60);
    else if (d < 86400) snprintf(out, n, _("hace %ld h"), d / 3600);
    else snprintf(out, n, _("hace %ld d"), d / 86400);
}

void ct_hist_build(void)
{
    lv_obj_t *page = ct_page();
    lv_obj_t *right = ct_header(page, _("Historial"), true);
    if (ct_len(CA.st.hist.root)) ct_round_btn(right, AOS_SYM_DELETE, hist_clear_cb, NULL);
    ct_header_fit(right);
    lv_obj_t *list = ct_scroll(page, 120);
    lv_obj_set_style_pad_row(list, 8, 0);
    int32_t w = CA.W - 2 * AOS_UI_PAD;
    ctv_t hs = CA.st.hist.root;
    for (uint32_t i = 0; i < ct_len(hs); i++) {
        ctv_t h = ct_at(hs, i);
        lv_obj_t *row = ct_box(list, lv_pct(100), 100);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, hist_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *m = ct_method_badge(row, ct_s(ct_get(h, "method")), 80);
        lv_obj_set_pos(m, 18, 16);
        char safe[300];
        const char *nm = ct_s(ct_get(h, "name"));
        aos_text_safe(safe, sizeof safe, nm[0] ? nm : ct_s(ct_get(h, "url")));
        lv_obj_t *l = ct_label_dots(row, safe, aos_font_small, AOS_C_TEXT, w - 18 - 90 - 170);
        lv_obj_set_pos(l, 108, 12);
        aos_text_safe(safe, sizeof safe, ct_s(ct_get(h, "url")));
        l = ct_label_dots(row, safe, aos_font_caption, AOS_C_DIM, w - 18 - 90 - 170);
        lv_obj_set_pos(l, 108, 56);
        int st = (int)ct_to_num(ct_get(h, "status"));
        char b[64], ms[24], when[32];
        ct_fmt_ms(ms, sizeof ms, (int)ct_to_num(ct_get(h, "ms")));
        ago(when, sizeof when, ct_to_num(ct_get(h, "t")));
        if (st) snprintf(b, sizeof b, "%d · %s", st, ms);
        else snprintf(b, sizeof b, "%s", _("sin respuesta"));
        l = ct_label(row, b, aos_font_caption, ct_status_color(st));
        lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -18, 14);
        l = ct_label(row, when, aos_font_caption, AOS_C_DIM);
        lv_obj_align(l, LV_ALIGN_BOTTOM_RIGHT, -18, -14);
    }
    if (!ct_len(hs)) {
        ct_label_wrap(list, _("Los pedidos que mandes van a aparecer acá, del más nuevo al más viejo (los últimos 50)."),
                      aos_font_body, AOS_C_DIM);
    }
}
