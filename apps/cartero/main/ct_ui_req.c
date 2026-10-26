/*
 * P4OS - Cartero: a request, edited. The method and the URL on top (with
 * the URL as it will go, variables resolved, under it), Send, and five
 * tabs: the query parameters, the headers, the body, the auth and the two
 * scripts.
 *
 * The widgets are read back into the draft (CA.draft, its own arena) when
 * the tab changes, before sending or saving, and when the app is left: not
 * at every key, which would fill the arena with copies of a text being
 * typed. The structural changes (a row added or removed, a mode chosen)
 * rebuild the screen from an async call, never from inside the event of a
 * widget that the rebuild deletes.
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

enum { TAB_PARAMS, TAB_HEADERS, TAB_BODY, TAB_AUTH, TAB_SCRIPTS, TAB_N };

enum { F_QKEY, F_QVAL, F_QON, F_HKEY, F_HVAL, F_HON, F_RAW, F_FKEY, F_FVAL, F_FON, F_GQ, F_GV, F_A1, F_A2, F_A3, F_SCRIPT };

#define BIND_MAX 240

typedef struct {
    lv_obj_t *o;
    uint8_t   f;
    uint16_t  i;
} bind_t;

static struct {
    lv_obj_t *page, *url_ta, *preview, *content, *dirty_dot;
    int       tab, script_tab;
    bind_t    b[BIND_MAX];
    int       nb;
    bool      rebuild_pending;
    bool      ready;            /* the widgets hold the draft: commit reads them */
    bool      quiet;            /* a text set by the code, not typed */
} E;

static const char *const METHODS[] = { "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS" };

static void bind(lv_obj_t *o, int f, int i)
{
    if (E.nb < BIND_MAX) E.b[E.nb++] = (bind_t){ o, (uint8_t)f, (uint16_t)i };
}

static const char *ta_text(lv_obj_t *ta)
{
    return ta ? lv_textarea_get_text(ta) : "";
}

static bool on_of(lv_obj_t *o)
{
    return o && lv_obj_has_state(o, LV_STATE_CHECKED);
}

static void mark_dirty(void)
{
    if (E.quiet) return;
    if (!CA.dirty) {
        CA.dirty = true;
        if (E.dirty_dot) lv_obj_remove_flag(E.dirty_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void changed_cb(lv_event_t *e)
{
    (void)e;
    mark_dirty();
}

static void rebuild_cb(void *unused)
{
    (void)unused;
    E.rebuild_pending = false;
    if (CA.screen == CT_SCR_REQ) ct_build();
}

static void rebuild_later(void)
{
    if (E.rebuild_pending) return;
    E.rebuild_pending = true;
    lv_async_call(rebuild_cb, NULL);
}

void ct_req_close(void)
{
    if (E.rebuild_pending) lv_async_call_cancel(rebuild_cb, NULL);
    E.rebuild_pending = false;
    E.ready = false;
    E.page = E.url_ta = E.preview = E.content = E.dirty_dot = NULL;
    E.nb = 0;
}

/* ---- the variables of the moment, for the preview --------------------------------------------- */

static void preview_update(void)
{
    if (!E.preview || !E.url_ta) return;
    ct_arena_t a;
    ct_arena_init(&a, 2u * 1024 * 1024);
    ct_script_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ar = &a;
    ct_store_t *st = &CA.st;
    if (st->active >= 0 && st->active < st->nenv) ctx.env = ct_vars_obj(&a, ct_get(st->env[st->active].root, "values"));
    ctx.glob = ct_vars_obj(&a, ct_get(st->glob.root, "values"));
    if (!CA.loose) {
        for (int i = 0; i < st->ncoll; i++) {
            if (!strcmp(st->coll[i].slug, CA.dref.slug)) ctx.coll = ct_vars_obj(&a, ct_get(st->coll[i].root, "variable"));
        }
    }
    const char *raw = ta_text(E.url_ta);
    char *r = ct_resolve(&a, &ctx, raw);
    char out[700];
    if (!r || !raw[0]) {
        ct_copy(out, sizeof out, "");
    } else {
        /* the names left unresolved, said apart */
        char miss[200] = "";
        size_t mk = 0;
        for (const char *p = r; (p = strstr(p, "{{")); p += 2) {
            const char *c = strstr(p + 2, "}}");
            if (!c) break;
            if (mk + (size_t)(c - p) + 4 < sizeof miss) {
                mk += (size_t)snprintf(miss + mk, sizeof miss - mk, "%s%.*s", mk ? ", " : "", (int)(c - p - 2), p + 2);
            }
        }
        if (strcmp(r, raw) || miss[0]) {
            if (miss[0]) snprintf(out, sizeof out, "%.480s\n%s %s", r, _("Sin definir:"), miss);
            else snprintf(out, sizeof out, "%.600s", r);
        } else {
            out[0] = '\0';
        }
        lv_obj_set_style_text_color(E.preview, miss[0] ? AOS_C_ORANGE : AOS_C_DIM, 0);
    }
    char safe[760];
    aos_text_safe(safe, sizeof safe, out);
    ct_set_text(E.preview, safe);
    if (out[0]) lv_obj_remove_flag(E.preview, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(E.preview, LV_OBJ_FLAG_HIDDEN);
    ct_arena_free(&a);
}

static void url_changed_cb(lv_event_t *e)
{
    (void)e;
    mark_dirty();
    preview_update();
}

/* ---- reading the widgets back --------------------------------------------------------------------- */

static ctv_t req(void)
{
    return ct_req_ensure(&CA.dar, CA.draft);
}

static ctv_t body_obj(void)
{
    ctv_t r = req();
    ctv_t b = ct_get(r, "body");
    if (b.t != CT_OBJ) {
        b = ct_obj_new(&CA.dar, 3);
        ct_setv(&CA.dar, r, "body", b);
    }
    return b;
}

static ctv_t auth_obj(bool make)
{
    ctv_t r = req();
    ctv_t a = ct_get(r, "auth");
    if (a.t != CT_OBJ && make) {
        a = ct_obj_new(&CA.dar, 2);
        ct_setv(&CA.dar, r, "auth", a);
    }
    return a;
}

/* the URL without its query, and the query made again from the rows */
static void commit_params(void)
{
    const char *raw = ta_text(E.url_ta);
    const char *q = strchr(raw, '?');
    const char *hash = strchr(raw, '#');
    size_t base = q ? (size_t)(q - raw) : hash ? (size_t)(hash - raw) : strlen(raw);
    ct_sb_t b = { 0 };
    ct_sb_add(&b, raw, base);
    ctv_t query = ct_arr_new(&CA.dar, 4);
    int k = 0;
    for (int i = 0; i < E.nb; i++) {
        if (E.b[i].f != F_QKEY) continue;
        lv_obj_t *kt = E.b[i].o, *vt = NULL, *on = NULL;
        for (int j = 0; j < E.nb; j++) {
            if (E.b[j].i != E.b[i].i) continue;
            if (E.b[j].f == F_QVAL) vt = E.b[j].o;
            if (E.b[j].f == F_QON) on = E.b[j].o;
        }
        const char *key = ta_text(kt), *val = ta_text(vt);
        if (!key[0] && !val[0]) continue;
        bool enabled = on ? on_of(on) : true;
        ctv_t e = ct_obj_new(&CA.dar, 3);
        ct_sets(&CA.dar, e, "key", key);
        ct_sets(&CA.dar, e, "value", val);
        if (!enabled) ct_setv(&CA.dar, e, "disabled", ct_bool(true));
        ct_arr_push(&CA.dar, query.a, e);
        if (!enabled) continue;
        ct_sb_addc(&b, k++ ? '&' : '?');
        ct_sb_adds(&b, key);
        if (val[0]) {
            ct_sb_addc(&b, '=');
            ct_sb_adds(&b, val);
        }
    }
    if (hash) ct_sb_adds(&b, hash);
    bool same = b.p && !strcmp(b.p, raw);
    /* the disabled ones survive ct_req_set_url through the old query */
    ctv_t u = ct_get(req(), "url");
    if (u.t != CT_OBJ) {
        u = ct_obj_new(&CA.dar, 2);
        ct_setv(&CA.dar, req(), "url", u);
    }
    ctv_t only_off = ct_arr_new(&CA.dar, 2);
    for (uint32_t i = 0; i < ct_len(query); i++) {
        if (ct_truthy(ct_get(ct_at(query, i), "disabled"))) ct_arr_push(&CA.dar, only_off.a, ct_at(query, i));
    }
    ct_setv(&CA.dar, u, "query", only_off);
    ct_req_set_url(&CA.dar, CA.draft, b.p ? b.p : "");
    ct_sb_free(&b);
    if (E.url_ta && !same) {
        E.quiet = true;
        lv_textarea_set_text(E.url_ta, ct_req_url(&CA.dar, CA.draft));
        E.quiet = false;
    }
}

static void commit_kv(int fk, int fv, int fon, ctv_t old, ctv_t *out)
{
    ctv_t list = ct_arr_new(&CA.dar, 4);
    for (int i = 0; i < E.nb; i++) {
        if (E.b[i].f != fk) continue;
        int row = E.b[i].i;
        lv_obj_t *vt = NULL, *on = NULL;
        for (int j = 0; j < E.nb; j++) {
            if (E.b[j].i != row) continue;
            if (E.b[j].f == fv) vt = E.b[j].o;
            if (E.b[j].f == fon) on = E.b[j].o;
        }
        const char *key = ta_text(E.b[i].o), *val = ta_text(vt);
        if (!key[0] && !val[0]) continue;
        /* the entry it was, so its other fields (type, description) stay */
        ctv_t prev = ct_at(old, (uint32_t)row);
        ctv_t e = prev.t == CT_OBJ ? ct_deep_copy(&CA.dar, prev) : ct_obj_new(&CA.dar, 3);
        ct_sets(&CA.dar, e, "key", key);
        ct_sets(&CA.dar, e, "value", val);
        if (on && !on_of(on)) ct_setv(&CA.dar, e, "disabled", ct_bool(true));
        else ct_obj_del(e.o, "disabled", 8);
        ct_arr_push(&CA.dar, list.a, e);
    }
    *out = list;
}

void ct_req_commit(void)
{
    if (!E.ready || !CA.has_draft) return;
    const char *url = ta_text(E.url_ta);
    if (strcmp(url, ct_req_url(&CA.dar, CA.draft))) ct_req_set_url(&CA.dar, CA.draft, url);
    switch (E.tab) {
    case TAB_PARAMS:
        commit_params();
        break;
    case TAB_HEADERS: {
        ctv_t list;
        commit_kv(F_HKEY, F_HVAL, F_HON, ct_req_headers(CA.draft), &list);
        ct_setv(&CA.dar, req(), "header", list);
        break;
    }
    case TAB_BODY: {
        ctv_t body = ct_req_body(CA.draft);
        const char *mode = ct_s(ct_get(body, "mode"));
        for (int i = 0; i < E.nb; i++) {
            if (E.b[i].f == F_RAW) ct_sets(&CA.dar, body_obj(), "raw", ta_text(E.b[i].o));
        }
        if (!strcmp(mode, "urlencoded")) {
            ctv_t list;
            commit_kv(F_FKEY, F_FVAL, F_FON, ct_get(body, "urlencoded"), &list);
            ct_setv(&CA.dar, body_obj(), "urlencoded", list);
        }
        if (!strcmp(mode, "graphql")) {
            ctv_t g = ct_obj_new(&CA.dar, 2);
            for (int i = 0; i < E.nb; i++) {
                if (E.b[i].f == F_GQ) ct_sets(&CA.dar, g, "query", ta_text(E.b[i].o));
                if (E.b[i].f == F_GV) ct_sets(&CA.dar, g, "variables", ta_text(E.b[i].o));
            }
            ct_setv(&CA.dar, body_obj(), "graphql", g);
        }
        break;
    }
    case TAB_AUTH: {
        ctv_t a = auth_obj(false);
        const char *type = ct_s(ct_get(a, "type"));
        const char *v[3] = { "", "", "" };
        for (int i = 0; i < E.nb; i++) {
            if (E.b[i].f >= F_A1 && E.b[i].f <= F_A3) v[E.b[i].f - F_A1] = ta_text(E.b[i].o);
        }
        static const char *const KEYS[][3] = { { "token" }, { "username", "password" }, { "key", "value" } };
        int k = !strcmp(type, "bearer") ? 0 : !strcmp(type, "basic") ? 1 : !strcmp(type, "apikey") ? 2 : -1;
        if (k < 0) break;
        ctv_t list = ct_get(a, type);
        ctv_t nl = ct_arr_new(&CA.dar, 3);
        /* the parameters this tab does not show (apikey's 'in') stay */
        for (uint32_t i = 0; i < ct_len(list); i++) {
            const char *key = ct_s(ct_get(ct_at(list, i), "key"));
            bool shown = false;
            for (int f = 0; f < 3; f++) if (KEYS[k][f] && !strcmp(KEYS[k][f], key)) shown = true;
            if (!shown) ct_arr_push(&CA.dar, nl.a, ct_at(list, i));
        }
        for (int f = 0; f < 3; f++) {
            if (!KEYS[k][f]) continue;
            ctv_t e = ct_obj_new(&CA.dar, 3);
            ct_sets(&CA.dar, e, "key", KEYS[k][f]);
            ct_sets(&CA.dar, e, "value", v[f]);
            ct_sets(&CA.dar, e, "type", "string");
            ct_arr_push(&CA.dar, nl.a, e);
        }
        ct_setv(&CA.dar, a, type, nl);
        break;
    }
    case TAB_SCRIPTS:
        for (int i = 0; i < E.nb; i++) {
            if (E.b[i].f == F_SCRIPT) {
                ct_req_set_script(&CA.dar, CA.draft, E.script_tab ? "test" : "prerequest", ta_text(E.b[i].o));
            }
        }
        break;
    }
}

/* ---- the method, the name, saving ------------------------------------------------------------------ */

static void method_pick(int i, void *ud)
{
    (void)ud;
    ct_req_commit();
    ct_sets(&CA.dar, req(), "method", METHODS[i]);
    CA.dirty = true;
    ct_build();
}

static void method_cb(lv_event_t *e)
{
    (void)e;
    const char *m = ct_req_method(CA.draft);
    int sel = -1;
    for (int i = 0; i < 7; i++) if (!strcmp(m, METHODS[i])) sel = i;
    ct_sheet(_("Método"), METHODS, 7, sel, method_pick, NULL);
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    ct_kb_hide();
    ct_send();
}

static void renamed(const char *name, void *ud)
{
    (void)ud;
    if (!name[0]) return;
    ct_req_commit();
    ct_sets(&CA.dar, CA.draft, "name", name);
    CA.dirty = true;
    ct_build();
}

/* the places a loose request may go: each collection and its folders */
static ct_ref_t s_dest[48];
static int s_ndest;

static void dest_collect(ctv_t node, ct_ref_t *at, const char *prefix, const char **names, char (*buf)[120])
{
    if (s_ndest >= 48) return;
    s_dest[s_ndest] = *at;
    snprintf(buf[s_ndest], 120, "%s", prefix);
    names[s_ndest] = buf[s_ndest];
    s_ndest++;
    if (at->depth >= 2) return;
    ctv_t items = ct_items(node);
    for (uint32_t i = 0; i < ct_len(items) && s_ndest < 48; i++) {
        if (!ct_is_folder(items.a->v[i])) continue;
        ct_ref_t r = *at;
        r.path[r.depth++] = (uint8_t)i;
        char p[120];
        snprintf(p, sizeof p, "%.50s / %.60s", prefix, ct_item_name(items.a->v[i]));
        dest_collect(items.a->v[i], &r, p, names, buf);
    }
}

static void saved_toast(bool ok)
{
    aos_ui_toast(ok ? _("Guardado") : _("No se pudo guardar"), 1200);
}

static void dest_pick(int i, void *ud)
{
    (void)ud;
    if (i < 0 || i >= s_ndest) return;
    ct_ref_t out;
    bool ok = ct_coll_add(&CA.st, &s_dest[i], CA.draft, &out);
    saved_toast(ok);
    if (ok) {
        CA.loose = false;
        CA.dref = out;
        CA.dirty = false;
        CA.dorig = ct_deep_copy(&CA.dar, CA.draft);
        ct_copy(CA.dname, sizeof CA.dname, ct_item_name(CA.draft));
        ct_build();
    }
}

static void save_as(void)
{
    static const char *names[48];
    static char buf[48][120];
    s_ndest = 0;
    for (int c = 0; c < CA.st.ncoll && s_ndest < 48; c++) {
        if (CA.st.coll[c].bad) continue;
        ct_ref_t r = { .coll = c };
        ct_copy(r.slug, sizeof r.slug, CA.st.coll[c].slug);
        dest_collect(CA.st.coll[c].root, &r, CA.st.coll[c].name, names, buf);
    }
    if (!s_ndest) {
        aos_ui_toast(_("Primero creá una colección (+ en el inicio)"), 2500);
        return;
    }
    ct_sheet(_("Guardar en"), names, s_ndest, -1, dest_pick, NULL);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    ct_kb_hide();
    ct_req_commit();
    if (CA.loose) {
        save_as();
        return;
    }
    bool ok = ct_coll_put(&CA.st, &CA.dref, CA.draft, CA.dname);
    saved_toast(ok);
    if (ok) {
        CA.dirty = false;
        CA.dorig = ct_deep_copy(&CA.dar, CA.draft);
        ct_copy(CA.dname, sizeof CA.dname, ct_item_name(CA.draft));
        if (E.dirty_dot) lv_obj_add_flag(E.dirty_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void more_pick(int i, void *ud)
{
    (void)ud;
    if (i == 0) ct_ask(_("Nombre del pedido"), ct_item_name(CA.draft), renamed, NULL);
    else if (i == 1) save_as();
    else if (i == 2 && CA.job) {
        CA.resp_back = CT_SCR_REQ;
        ct_go(CT_SCR_RESP);
    }
}

static void more_cb(lv_event_t *e)
{
    (void)e;
    ct_req_commit();
    const char *items[3] = { _("Cambiar el nombre"), _("Guardar como…"), _("Ver la última respuesta") };
    ct_sheet(ct_item_name(CA.draft), items, CA.job && CA.job->kind == CT_JOB_SEND ? 3 : 2, -1, more_pick, NULL);
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == E.tab) return;
    ct_req_commit();
    E.tab = t;
    rebuild_later();
}

/* ---- rows of key and value ------------------------------------------------------------------------ */

static lv_obj_t *check(lv_obj_t *row, bool on, int f, int i)
{
    lv_obj_t *c = lv_checkbox_create(row);
    lv_checkbox_set_text(c, "");
    if (on) lv_obj_add_state(c, LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(c, AOS_C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(c, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(c, 10, LV_PART_INDICATOR);
    lv_obj_set_style_radius(c, 8, LV_PART_INDICATOR);
    lv_obj_add_event_cb(c, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    bind(c, f, i);
    return c;
}

static lv_obj_t *small_ta(lv_obj_t *row, const char *text, const char *ph, int32_t w, int f, int i)
{
    lv_obj_t *ta = ct_textarea(row, text, true, true);
    lv_obj_set_width(ta, w);
    lv_obj_set_style_pad_ver(ta, 10, 0);
    lv_obj_set_style_pad_hor(ta, 14, 0);
    lv_textarea_set_placeholder_text(ta, ph);
    lv_obj_add_event_cb(ta, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    bind(ta, f, i);
    return ta;
}

static int s_kv_kind;            /* the list the + and x act on: 0 query, 1 headers, 2 form */

static void kv_del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ct_req_commit();
    ctv_t list = s_kv_kind == 0 ? ct_get(ct_get(req(), "url"), "query")
                 : s_kv_kind == 1 ? ct_req_headers(CA.draft) : ct_get(body_obj(), "urlencoded");
    if (list.t == CT_ARR && (uint32_t)i < list.a->n) ct_arr_remove(list.a, (uint32_t)i);
    if (s_kv_kind == 0) {
        /* the URL follows the list */
        ct_sb_t b = { 0 };
        const char *raw = ct_req_url(&CA.dar, CA.draft);
        const char *q = strchr(raw, '?');
        ct_sb_add(&b, raw, q ? (size_t)(q - raw) : strlen(raw));
        int k = 0;
        for (uint32_t j = 0; j < ct_len(list); j++) {
            ctv_t x = ct_at(list, j);
            if (ct_truthy(ct_get(x, "disabled"))) continue;
            ct_sb_addc(&b, k++ ? '&' : '?');
            ct_sb_adds(&b, ct_s(ct_get(x, "key")));
            const char *v = ct_s(ct_get(x, "value"));
            if (v[0]) {
                ct_sb_addc(&b, '=');
                ct_sb_adds(&b, v);
            }
        }
        ctv_t off = ct_arr_new(&CA.dar, 2);
        for (uint32_t j = 0; j < ct_len(list); j++) {
            if (ct_truthy(ct_get(ct_at(list, j), "disabled"))) ct_arr_push(&CA.dar, off.a, ct_at(list, j));
        }
        ct_setv(&CA.dar, ct_get(req(), "url"), "query", off);
        ct_req_set_url(&CA.dar, CA.draft, b.p ? b.p : "");
        ct_sb_free(&b);
    }
    CA.dirty = true;
    rebuild_later();
}

static void kv_add(const char *k, const char *v)
{
    ct_req_commit();
    ctv_t list;
    if (s_kv_kind == 1) {
        list = ct_req_headers(CA.draft);
    } else if (s_kv_kind == 2) {
        list = ct_get(body_obj(), "urlencoded");
        if (list.t != CT_ARR) {
            list = ct_arr_new(&CA.dar, 4);
            ct_setv(&CA.dar, body_obj(), "urlencoded", list);
        }
    } else {
        ctv_t u = ct_get(req(), "url");
        if (u.t != CT_OBJ) {
            ct_req_set_url(&CA.dar, CA.draft, ct_req_url(&CA.dar, CA.draft));
            u = ct_get(req(), "url");
        }
        list = ct_get(u, "query");
        if (list.t != CT_ARR) {
            list = ct_arr_new(&CA.dar, 4);
            ct_setv(&CA.dar, u, "query", list);
        }
    }
    ctv_t e = ct_obj_new(&CA.dar, 3);
    ct_sets(&CA.dar, e, "key", k);
    ct_sets(&CA.dar, e, "value", v);
    ct_arr_push(&CA.dar, list.a, e);
    CA.dirty = true;
    rebuild_later();
}

static void kv_add_cb(lv_event_t *e)
{
    (void)e;
    kv_add("", "");
}

static void kv_rows(lv_obj_t *content, ctv_t list, int fk, int fv, int fon, const char *kph, const char *vph)
{
    int32_t w = CA.W - 2 * AOS_UI_PAD;
    int32_t kw = CA.land ? w * 32 / 100 : w * 34 / 100;
    int32_t vw = w - kw - 64 - 72 - 30;
    for (uint32_t i = 0; i < ct_len(list); i++) {
        ctv_t x = ct_at(list, i);
        lv_obj_t *row = ct_box(content, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        check(row, !ct_truthy(ct_get(x, "disabled")), fon, (int)i);
        small_ta(row, ct_s(ct_get(x, "key")), kph, kw, fk, (int)i);
        ct_str_t *vs = ct_to_str(&CA.dar, ct_get(x, "value"));
        small_ta(row, ct_get(x, "value").t == CT_UNDEF || ct_get(x, "value").t == CT_NULL ? "" : (vs ? vs->p : ""),
                 vph, vw, fv, (int)i);
        lv_obj_t *d = ct_box(row, 64, 64);
        lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(d, 32, 0);
        lv_obj_set_style_bg_color(d, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(d, kv_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(ct_label(d, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_DIM));
    }
    lv_obj_t *add = ct_btn(content, AOS_SYM_PLUS, _("Agregar"), AOS_C_CARD2, kv_add_cb, NULL);
    lv_obj_set_style_margin_top(add, 6, 0);
}

/* ---- the tabs ------------------------------------------------------------------------------------- */

static void tab_params(lv_obj_t *c)
{
    /* the rows are the URL's query (ct_req_commit parsed the text again if
     * it was typed in the URL) */
    s_kv_kind = 0;
    ctv_t q = ct_get(ct_get(req(), "url"), "query");
    if (!ct_len(q)) ct_label_wrap(c, _("Los parámetros de la dirección (?clave=valor) se editan acá o en la dirección misma."),
                                  aos_font_caption, AOS_C_DIM);
    kv_rows(c, q, F_QKEY, F_QVAL, F_QON, _("clave"), _("valor"));
}

static void quick_hdr_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    static const char *const Q[][2] = {
        { "Content-Type", "application/json" }, { "Accept", "application/json" },
        { "Authorization", "Bearer {{token}}" }, { "X-Api-Key", "{{api_key}}" },
    };
    s_kv_kind = 1;
    kv_add(Q[i][0], Q[i][1]);
}

static void tab_headers(lv_obj_t *c)
{
    s_kv_kind = 1;
    kv_rows(c, ct_req_headers(CA.draft), F_HKEY, F_HVAL, F_HON, _("encabezado"), _("valor"));
    ct_label_wrap(c, _("Rápidos:"), aos_font_caption, AOS_C_DIM);
    lv_obj_t *row = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 10, 0);
    static const char *const L[] = { "Content-Type: JSON", "Accept: JSON", "Bearer", "X-Api-Key" };
    for (int i = 0; i < 4; i++) ct_chip(row, L[i], false, quick_hdr_cb, (void *)(intptr_t)i);
    ct_label_wrap(c, _("La placa agrega Host, User-Agent, Accept, Accept-Encoding: identity, Connection y Content-Length si no están."),
                  aos_font_caption, AOS_C_DIM);
}

static const char *const BODY_MODES[] = { N_("Ninguno"), "JSON", N_("Texto"), "XML", N_("Formulario"), "GraphQL" };

static int body_mode_index(ctv_t body)
{
    const char *m = ct_s(ct_get(body, "mode"));
    if (!strcmp(m, "raw")) {
        const char *l = ct_s(ct_get(ct_get(ct_get(body, "options"), "raw"), "language"));
        if (!strcmp(l, "json")) return 1;
        if (!strcmp(l, "xml")) return 3;
        return 2;
    }
    if (!strcmp(m, "urlencoded")) return 4;
    if (!strcmp(m, "graphql")) return 5;
    return 0;
}

static void body_mode_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ct_req_commit();
    ctv_t b = body_obj();
    static const char *const MODE[] = { "none", "raw", "raw", "raw", "urlencoded", "graphql" };
    static const char *const LANG[] = { NULL, "json", "text", "xml", NULL, NULL };
    ct_sets(&CA.dar, b, "mode", MODE[i]);
    if (LANG[i]) {
        ctv_t o = ct_obj_new(&CA.dar, 1), r = ct_obj_new(&CA.dar, 1);
        ct_sets(&CA.dar, r, "language", LANG[i]);
        ct_setv(&CA.dar, o, "raw", r);
        ct_setv(&CA.dar, b, "options", o);
        if (ct_get(b, "raw").t != CT_STR) ct_sets(&CA.dar, b, "raw", "");
    }
    CA.dirty = true;
    rebuild_later();
}

static lv_obj_t *s_json_state;

static void json_check(lv_obj_t *ta)
{
    if (!s_json_state) return;
    const char *t = ta_text(ta);
    const char *p = t;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (!*p) {
        ct_set_text(s_json_state, "");
        return;
    }
    /* variables make it not JSON yet: they are resolved when it goes */
    if (strstr(t, "{{")) {
        ct_set_text(s_json_state, _("Con variables: se resuelven al enviar"));
        lv_obj_set_style_text_color(s_json_state, AOS_C_DIM, 0);
        return;
    }
    ct_arena_t a;
    ct_arena_init(&a, 8u * 1024 * 1024);
    ctv_t v;
    char err[120];
    bool ok = ct_json_parse(&a, t, strlen(t), &v, err, sizeof err);
    ct_arena_free(&a);
    if (ok) {
        ct_set_text(s_json_state, _("JSON válido"));
        lv_obj_set_style_text_color(s_json_state, AOS_C_GREEN, 0);
    } else {
        ct_set_text(s_json_state, err);
        lv_obj_set_style_text_color(s_json_state, AOS_C_RED, 0);
    }
}

static void raw_changed_cb(lv_event_t *e)
{
    mark_dirty();
    json_check(lv_event_get_target(e));
}

static void format_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_user_data(e);
    const char *t = ta_text(ta);
    ct_sb_t b = { 0 };
    if (ct_json_pretty(t, strlen(t), &b, 2) && b.p) {
        lv_textarea_set_text(ta, b.p);
        mark_dirty();
    } else {
        aos_ui_toast(_("No es JSON (o tiene variables sin comillas)"), 1500);
    }
    ct_sb_free(&b);
}

static lv_obj_t *big_ta(lv_obj_t *c, const char *text, int32_t h, int f)
{
    lv_obj_t *ta = ct_textarea(c, text, false, true);
    lv_obj_set_height(ta, h);
    lv_obj_add_event_cb(ta, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    bind(ta, f, 0);
    return ta;
}

static void tab_body(lv_obj_t *c)
{
    ctv_t body = ct_req_body(CA.draft);
    int m = body_mode_index(body);
    const char *labels[6];
    for (int i = 0; i < 6; i++) labels[i] = (i == 0 || i == 2 || i == 4) ? _(BODY_MODES[i]) : BODY_MODES[i];
    ct_seg(c, labels, 6, m, body_mode_cb);
    int32_t h = CA.land ? CA.H * 45 / 100 : CA.H * 36 / 100;
    s_json_state = NULL;
    if (m >= 1 && m <= 3) {
        lv_obj_t *ta = big_ta(c, ct_s(ct_get(body, "raw")), h, F_RAW);
        if (m == 1) {
            lv_obj_add_event_cb(ta, raw_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
            lv_obj_t *row = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 14, 0);
            ct_btn(row, AOS_SYM_AUTO_FIX, _("Formatear"), AOS_C_CARD2, format_cb, ta);
            s_json_state = ct_label_dots(row, "", aos_font_caption, AOS_C_DIM, CA.W - 2 * AOS_UI_PAD - 260);
            json_check(ta);
        }
    } else if (m == 4) {
        s_kv_kind = 2;
        kv_rows(c, ct_get(body, "urlencoded"), F_FKEY, F_FVAL, F_FON, _("campo"), _("valor"));
    } else if (m == 5) {
        ctv_t g = ct_get(body, "graphql");
        ct_label(c, "query", aos_font_caption, AOS_C_DIM);
        big_ta(c, ct_s(ct_get(g, "query")), h * 2 / 3, F_GQ);
        ct_label(c, "variables (JSON)", aos_font_caption, AOS_C_DIM);
        big_ta(c, ct_s(ct_get(g, "variables")), h / 2, F_GV);
    } else {
        ct_label_wrap(c, _("El pedido va sin cuerpo."), aos_font_caption, AOS_C_DIM);
    }
}

static const char *const AUTH_TYPES[] = { "inherit", "noauth", "bearer", "basic", "apikey" };
static const char *const AUTH_LABELS[] = { N_("Heredada"), N_("Ninguna"), "Bearer", "Basic", "API key" };

static void auth_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ct_req_commit();
    if (i == 0) {
        ct_obj_del(req().o, "auth", 4);
    } else {
        ctv_t a = auth_obj(true);
        ct_sets(&CA.dar, a, "type", AUTH_TYPES[i]);
    }
    CA.dirty = true;
    rebuild_later();
}

static void apikey_in_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ct_req_commit();
    ctv_t a = auth_obj(true);
    ctv_t list = ct_get(a, "apikey");
    if (list.t != CT_ARR) {
        list = ct_arr_new(&CA.dar, 3);
        ct_setv(&CA.dar, a, "apikey", list);
    }
    ct_vars_unset(list, "in");
    ctv_t x = ct_obj_new(&CA.dar, 3);
    ct_sets(&CA.dar, x, "key", "in");
    ct_sets(&CA.dar, x, "value", i ? "query" : "header");
    ct_sets(&CA.dar, x, "type", "string");
    ct_arr_push(&CA.dar, list.a, x);
    CA.dirty = true;
    rebuild_later();
}

static const char *aparam(ctv_t a, const char *type, const char *key)
{
    ctv_t list = ct_get(a, type);
    if (list.t == CT_OBJ) return ct_s(ct_get(list, key));
    for (uint32_t i = 0; i < ct_len(list); i++) {
        if (!strcmp(ct_s(ct_get(ct_at(list, i), "key")), key)) return ct_s(ct_get(ct_at(list, i), "value"));
    }
    return "";
}

static void field(lv_obj_t *c, const char *label, const char *value, int f, bool secret)
{
    ct_label(c, label, aos_font_caption, AOS_C_DIM);
    lv_obj_t *ta = ct_textarea(c, value, true, true);
    if (secret) lv_textarea_set_password_mode(ta, true);
    lv_obj_add_event_cb(ta, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    bind(ta, f, 0);
}

/* where an inherited auth comes from, said in words */
static void inherited(lv_obj_t *c)
{
    char t[200] = "";
    if (!CA.loose) {
        ct_ref_t r = CA.dref;
        while (r.depth > 0) {
            r.depth--;
            ctv_t n = ct_node(&CA.st, &r);
            ctv_t a = ct_get(n, "auth");
            if (a.t == CT_OBJ && strcmp(ct_s(ct_get(a, "type")), "inherit")) {
                const char *type = ct_s(ct_get(a, "type"));
                snprintf(t, sizeof t, _("Usa la de «%.80s»: %s"), ct_item_name(n), type);
                break;
            }
        }
    }
    if (!t[0]) ct_copy(t, sizeof t, CA.loose ? _("Suelto, no hereda nada: va sin auth.") : _("Ni la carpeta ni la colección tienen: va sin auth."));
    ct_label_wrap(c, t, aos_font_caption, AOS_C_DIM);
}

static void tab_auth(lv_obj_t *c)
{
    ctv_t a = ct_req_auth(CA.draft);
    const char *type = a.t == CT_OBJ ? ct_s(ct_get(a, "type")) : "inherit";
    int sel = 0;
    for (int i = 0; i < 5; i++) if (!strcmp(type, AUTH_TYPES[i])) sel = i;
    const char *labels[5];
    for (int i = 0; i < 5; i++) labels[i] = i < 2 ? _(AUTH_LABELS[i]) : AUTH_LABELS[i];
    ct_seg(c, labels, 5, sel, auth_cb);
    switch (sel) {
    case 0: inherited(c); break;
    case 1: ct_label_wrap(c, _("Va sin auth, aunque la carpeta o la colección tengan."), aos_font_caption, AOS_C_DIM); break;
    case 2: field(c, _("Token"), aparam(a, "bearer", "token"), F_A1, false); break;
    case 3:
        field(c, _("Usuario"), aparam(a, "basic", "username"), F_A1, false);
        field(c, _("Contraseña"), aparam(a, "basic", "password"), F_A2, true);
        break;
    case 4: {
        field(c, _("Nombre (encabezado o parámetro)"), aparam(a, "apikey", "key"), F_A1, false);
        field(c, _("Valor"), aparam(a, "apikey", "value"), F_A2, false);
        static const char *const IN[] = { N_("En un encabezado"), N_("En la dirección") };
        const char *in[2] = { _(IN[0]), _(IN[1]) };
        ct_seg(c, in, 2, !strcmp(aparam(a, "apikey", "in"), "query") ? 1 : 0, apikey_in_cb);
        break;
    }
    }
    if (sel >= 2) {
        ct_label_wrap(c, _("Se puede usar {{variables}}. Queda guardado en la colección, en la tarjeta, tal cual: mejor una variable del entorno."),
                      aos_font_caption, AOS_C_DIM);
    }
}

static void script_tab_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == E.script_tab) return;
    ct_req_commit();
    E.script_tab = i;
    rebuild_later();
}

static const char *const SNIP_TEST[][2] = {
    { N_("Estado 200"), "pm.test(\"Estado 200\", () => pm.response.to.have.status(200));" },
    { N_("Tiempo < 500 ms"), "pm.test(\"Responde en menos de 500 ms\", () => pm.expect(pm.response.responseTime).to.be.below(500));" },
    { N_("Es JSON"), "pm.test(\"La respuesta es JSON\", () => pm.response.to.be.json);" },
    { N_("Tiene una propiedad"), "pm.test(\"Tiene id\", () => pm.expect(pm.response.json()).to.have.property(\"id\"));" },
    { N_("Guardar en el entorno"), "pm.environment.set(\"id\", pm.response.json().id);" },
    { N_("Encabezado"), "pm.test(\"Content-Type\", () => pm.response.to.have.header(\"Content-Type\"));" },
};

static const char *const SNIP_PRE[][2] = {
    { N_("Variable con la hora"), "pm.variables.set(\"ahora\", Date.now());" },
    { N_("Agregar encabezado"), "pm.request.headers.add({ key: \"X-Id\", value: \"{{$guid}}\" });" },
    { N_("Registrar la URL"), "console.log(pm.request.url.toString());" },
    { N_("Saltear si no hay token"), "if (!pm.environment.get(\"token\")) pm.execution.skipRequest();" },
};

static lv_obj_t *s_script_ta;

static void snippet_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_script_ta) return;
    const char *code = E.script_tab ? SNIP_TEST[i][1] : SNIP_PRE[i][1];
    const char *cur = ta_text(s_script_ta);
    lv_textarea_set_cursor_pos(s_script_ta, LV_TEXTAREA_CURSOR_LAST);
    if (cur[0] && cur[strlen(cur) - 1] != '\n') lv_textarea_add_char(s_script_ta, '\n');
    lv_textarea_add_text(s_script_ta, code);
    lv_textarea_add_char(s_script_ta, '\n');
    mark_dirty();
}

static void tab_scripts(lv_obj_t *c)
{
    const char *tabs[2] = { _("Antes (pre-request)"), _("Tests") };
    ct_seg(c, tabs, 2, E.script_tab, script_tab_cb);
    char *src = ct_req_script(&CA.dar, CA.draft, E.script_tab ? "test" : "prerequest");
    int32_t h = CA.land ? CA.H * 42 / 100 : CA.H * 34 / 100;
    s_script_ta = big_ta(c, src ? src : "", h, F_SCRIPT);
    lv_textarea_set_placeholder_text(s_script_ta, E.script_tab ? "pm.test(\"...\", () => ...);" : "pm.variables.set(...);");
    lv_obj_t *row = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 10, 0);
    int n = E.script_tab ? (int)(sizeof SNIP_TEST / sizeof SNIP_TEST[0]) : (int)(sizeof SNIP_PRE / sizeof SNIP_PRE[0]);
    for (int i = 0; i < n; i++) {
        ct_chip(row, _(E.script_tab ? SNIP_TEST[i][0] : SNIP_PRE[i][0]), false, snippet_cb, (void *)(intptr_t)i);
    }
    ct_label_wrap(c, _("JavaScript con pm, como en Postman. La placa corre lo común (sin expresiones regulares ni clases); el portal, todo."),
                  aos_font_caption, AOS_C_DIM);
}

/* ---- the screen ------------------------------------------------------------------------------------ */

static void name_cb(lv_event_t *e)
{
    (void)e;
    ct_req_commit();
    ct_ask(_("Nombre del pedido"), ct_item_name(CA.draft), renamed, NULL);
}

void ct_req_build(void)
{
    if (!CA.has_draft) {
        CA.screen = CT_SCR_HOME;
        ct_home_build();
        return;
    }
    E.nb = 0;
    E.ready = false;
    s_script_ta = NULL;
    s_json_state = NULL;
    lv_obj_t *page = ct_page();
    E.page = page;
    char title[160];
    aos_text_safe(title, sizeof title, ct_item_name(CA.draft));
    lv_obj_t *right = ct_header(page, title, true);
    ct_round_btn(right, AOS_SYM_DOTS_HORIZONTAL, more_cb, NULL);
    ct_round_btn(right, AOS_SYM_CHECK, save_cb, NULL);
    ct_header_fit(right);
    lv_obj_t *tl = lv_obj_get_user_data(right);
    if (tl) {
        lv_obj_add_flag(tl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tl, name_cb, LV_EVENT_CLICKED, NULL);
    }
    /* where it lives */
    char where[200];
    if (CA.loose) {
        ct_copy(where, sizeof where, _("Suelto: sin guardar en una colección"));
    } else {
        int c = -1;
        for (int i = 0; i < CA.st.ncoll; i++) if (!strcmp(CA.st.coll[i].slug, CA.dref.slug)) c = i;
        ct_copy(where, sizeof where, c >= 0 ? CA.st.coll[c].name : "");
    }
    lv_obj_t *wl = ct_label_dots(page, where, aos_font_caption, AOS_C_DIM, CA.W - 2 * AOS_UI_PAD - 40);
    lv_obj_set_pos(wl, AOS_UI_PAD + AOS_UI_TAP_MIN, 86);
    E.dirty_dot = ct_box(page, 14, 14);
    lv_obj_set_style_radius(E.dirty_dot, 7, 0);
    lv_obj_set_style_bg_color(E.dirty_dot, AOS_C_ORANGE, 0);
    lv_obj_set_style_bg_opa(E.dirty_dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(E.dirty_dot, AOS_UI_PAD + AOS_UI_TAP_MIN - 22, 94);
    if (!CA.dirty && !CA.loose) lv_obj_add_flag(E.dirty_dot, LV_OBJ_FLAG_HIDDEN);

    /* everything below scrolls; the keyboard shrinks it */
    lv_obj_t *c = ct_scroll(page, 120);
    E.content = c;
    lv_obj_t *row = ct_box(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    const char *m = ct_req_method(CA.draft);
    lv_obj_t *mc = ct_chip(row, m, false, method_cb, NULL);
    lv_obj_t *ml = lv_obj_get_child(mc, 0);
    if (ml) {
        lv_obj_set_style_text_color(ml, ct_method_color(m), 0);
        lv_obj_set_style_text_font(ml, &aos_mono_22, 0);
    }
    lv_obj_t *sp = ct_box(row, 10, 10);
    lv_obj_set_flex_grow(sp, 1);
    ct_btn(row, AOS_SYM_SEND, _("Enviar"), AOS_C_ACCENT, send_cb, NULL);

    E.url_ta = ct_textarea(c, ct_req_url(&CA.dar, CA.draft), true, true);
    lv_textarea_set_placeholder_text(E.url_ta, "https://api.ejemplo.com/v1/usuarios  o  {{base}}/usuarios");
    lv_obj_add_event_cb(E.url_ta, url_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    E.preview = ct_label_wrap(c, "", &aos_mono_18, AOS_C_DIM);
    lv_obj_set_style_pad_hor(E.preview, 8, 0);

    static const char *const TABS[TAB_N] = { N_("Parámetros"), N_("Encabezados"), N_("Cuerpo"), N_("Auth"), N_("Scripts") };
    const char *labels[TAB_N];
    char counts[TAB_N][48];
    int nq = (int)ct_len(ct_get(ct_get(req(), "url"), "query")), nh = (int)ct_len(ct_req_headers(CA.draft));
    for (int i = 0; i < TAB_N; i++) {
        int n = i == TAB_PARAMS ? nq : i == TAB_HEADERS ? nh : 0;
        if (n) snprintf(counts[i], sizeof counts[i], "%s %d", _(TABS[i]), n);
        else ct_copy(counts[i], sizeof counts[i], _(TABS[i]));
        if (i == TAB_BODY && body_mode_index(ct_req_body(CA.draft))) snprintf(counts[i], sizeof counts[i], "%s •", _(TABS[i]));
        if (i == TAB_SCRIPTS) {
            char *pre = ct_req_script(&CA.dar, CA.draft, "prerequest"), *tst = ct_req_script(&CA.dar, CA.draft, "test");
            if ((pre && pre[0]) || (tst && tst[0])) snprintf(counts[i], sizeof counts[i], "%s •", _(TABS[i]));
        }
        labels[i] = counts[i];
    }
    lv_obj_t *tabs = ct_seg(c, labels, TAB_N, E.tab, tab_cb);
    lv_obj_set_style_margin_top(tabs, 8, 0);

    switch (E.tab) {
    case TAB_PARAMS: tab_params(c); break;
    case TAB_HEADERS: tab_headers(c); break;
    case TAB_BODY: tab_body(c); break;
    case TAB_AUTH: tab_auth(c); break;
    default: tab_scripts(c); break;
    }
    ct_kb_attach(page, c);
    E.ready = true;
    preview_update();
}

static void discard_pick(int i, void *ud)
{
    (void)ud;
    if (i == 0) {
        save_cb(NULL);
        if (CA.dirty) return;               /* not saved (a loose one asks where) */
    }
    if (i == 0 || i == 1) {
        CA.dirty = false;
        ct_go(CT_SCR_HOME);
    }
}

bool ct_req_back(void)
{
    ct_req_commit();
    /* what counts is a real difference, not a widget that was touched */
    CA.dirty = !ct_deep_equal(CA.draft, CA.dorig);
    if (CA.loose && !ct_req_url(&CA.dar, CA.draft)[0]) CA.dirty = false;
    if (CA.dirty) {
        const char *items[] = { _("Guardar"), _("Descartar los cambios"), _("Seguir editando") };
        ct_sheet(CA.loose ? _("El pedido suelto no está guardado") : _("El pedido tiene cambios sin guardar"), items, 3,
                 -1, discard_pick, NULL);
        return true;
    }
    return false;
}
