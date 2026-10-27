/*
 * P4OS - Cartero: the environments (which one is in use, new, deleted),
 * the variables of one of them or of the globals, and the few settings of
 * the app (how long a request may take, redirects, the largest body).
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

/* ---- the list ------------------------------------------------------------------------------- */

static void active_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    CA.st.active = CA.st.active == i ? -1 : i;
    ct_state_save(&CA.st);
    ct_build();
}

static void edit_cb(lv_event_t *e)
{
    CA.vars_env = (int)(intptr_t)lv_event_get_user_data(e);
    ct_go(CT_SCR_VARS);
}

static int s_del;

static void del_yes(int c, void *ud)
{
    (void)ud;
    if (c != 1) return;
    if (!ct_env_delete(&CA.st, s_del)) aos_ui_toast(_("No se pudo borrar"), 1500);
    ct_build();
}

static void del_cb(lv_event_t *e)
{
    s_del = (int)(intptr_t)lv_event_get_user_data(e);
    char t[200];
    snprintf(t, sizeof t, _("¿Borrar el entorno «%.100s»?"), CA.st.env[s_del].name);
    ct_confirm(t, _("Borrar"), del_yes, NULL);
}

static void env_named(const char *name, void *ud)
{
    (void)ud;
    if (!name[0]) return;
    int i = ct_env_new(&CA.st, name);
    if (i < 0) {
        aos_ui_toast(_("No se pudo crear el entorno"), 1500);
        return;
    }
    CA.vars_env = i;
    ct_go(CT_SCR_VARS);
}

static void new_cb(lv_event_t *e)
{
    (void)e;
    ct_ask(_("Nombre del entorno"), "", env_named, NULL);
}

static void timeout_cb(lv_event_t *e)
{
    static const int T[] = { 10000, 30000, 60000, 120000 };
    CA.st.timeout_ms = T[(int)(intptr_t)lv_event_get_user_data(e)];
    ct_state_save(&CA.st);
    ct_build();
}

static void maxkb_cb(lv_event_t *e)
{
    static const int K[] = { 1024, 4096, 16384 };
    CA.st.max_kb = K[(int)(intptr_t)lv_event_get_user_data(e)];
    ct_state_save(&CA.st);
    ct_build();
}

static void follow_cb(lv_event_t *e)
{
    CA.st.follow = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ct_state_save(&CA.st);
}

static lv_obj_t *env_row(lv_obj_t *list, const char *name, const char *sub, bool active, int idx)
{
    lv_obj_t *row = ct_box(list, lv_pct(100), 100);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    if (idx >= 0) {
        lv_obj_t *r = ct_box(row, 72, 72);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, active_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_center(ct_label(r, active ? AOS_SYM_CHECK : AOS_SYM_CIRCLE_OUTLINE, &aos_sym_28,
                               active ? AOS_C_ACCENT : AOS_C_DIM));
    } else {
        lv_obj_t *r = ct_box(row, 72, 72);
        lv_obj_center(ct_label(r, AOS_SYM_EARTH, &aos_sym_28, AOS_C_TEAL));
    }
    lv_obj_t *col = ct_box(row, CA.W - 2 * AOS_UI_PAD - 260, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(col, edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    char safe[160];
    aos_text_safe(safe, sizeof safe, name);
    ct_label_dots(col, safe, aos_font_body, active ? AOS_C_ACCENT : AOS_C_TEXT, CA.W - 2 * AOS_UI_PAD - 260);
    ct_label_dots(col, sub, aos_font_caption, AOS_C_DIM, CA.W - 2 * AOS_UI_PAD - 260);
    lv_obj_t *ed = ct_round_btn(row, AOS_SYM_PENCIL, edit_cb, (void *)(intptr_t)idx);
    (void)ed;
    if (idx >= 0) ct_round_btn(row, AOS_SYM_DELETE, del_cb, (void *)(intptr_t)idx);
    return row;
}

void ct_env_build(void)
{
    lv_obj_t *page = ct_page();
    lv_obj_t *right = ct_header(page, _("Entornos"), true);
    ct_round_btn(right, AOS_SYM_PLUS, new_cb, NULL);
    ct_header_fit(right);
    lv_obj_t *list = ct_scroll(page, 120);
    lv_obj_set_style_pad_row(list, 10, 0);
    ct_label_wrap(list, _("El entorno en uso cambia los {{valores}} de los pedidos: el mismo pedido va a desarrollo o a producción sin tocarlo. Tocá el círculo para usarlo."),
                  aos_font_caption, AOS_C_DIM);
    for (int i = 0; i < CA.st.nenv; i++) {
        ct_doc_t *d = &CA.st.env[i];
        char sub[96];
        if (d->bad) snprintf(sub, sizeof sub, _("No se pudo leer: %.60s"), d->err);
        else {
            int n = (int)ct_len(ct_get(d->root, "values"));
            snprintf(sub, sizeof sub, n == 1 ? _("%d variable") : _("%d variables"), n);
        }
        env_row(list, d->name, sub, i == CA.st.active, i);
    }
    char sub[96];
    int ng = (int)ct_len(ct_get(CA.st.glob.root, "values"));
    snprintf(sub, sizeof sub, _("Valen en todos los entornos: %d"), ng);
    env_row(list, _("Globales"), sub, false, -1);

    lv_obj_t *card = ct_card(list);
    lv_obj_set_style_margin_top(card, 16, 0);
    ct_label(card, _("Ajustes"), aos_font_small, AOS_C_TEXT);
    ct_label(card, _("Tiempo máximo de un pedido"), aos_font_caption, AOS_C_DIM);
    static const char *const T[] = { "10 s", "30 s", "60 s", "120 s" };
    int ts = CA.st.timeout_ms <= 10000 ? 0 : CA.st.timeout_ms <= 30000 ? 1 : CA.st.timeout_ms <= 60000 ? 2 : 3;
    ct_seg(card, T, 4, ts, timeout_cb);
    ct_label(card, _("Cuerpo más grande que se guarda"), aos_font_caption, AOS_C_DIM);
    static const char *const K[] = { "1 MB", "4 MB", "16 MB" };
    int ks = CA.st.max_kb <= 1024 ? 0 : CA.st.max_kb <= 4096 ? 1 : 2;
    ct_seg(card, K, 3, ks, maxkb_cb);
    lv_obj_t *row = ct_box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ct_label(row, _("Seguir las redirecciones"), aos_font_body, AOS_C_TEXT);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 96, 52);
    if (CA.st.follow) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, AOS_C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, follow_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- the variables of one --------------------------------------------------------------------- */

#define VROWS 120

static struct {
    lv_obj_t *key[VROWS], *val[VROWS], *on[VROWS];
    int       n;
    bool      dirty;
    lv_obj_t *page;
} V;

static ct_doc_t *vars_doc(const char **dir)
{
    if (CA.vars_env >= 0 && CA.vars_env < CA.st.nenv) {
        *dir = "entornos";
        return &CA.st.env[CA.vars_env];
    }
    *dir = NULL;
    return &CA.st.glob;
}

static void vchanged_cb(lv_event_t *e)
{
    (void)e;
    V.dirty = true;
}

/* the rows into the document's values, and the file written; the entries
 * keep their other fields (type: secret) */
static bool vars_commit(void)
{
    if (!V.dirty || !V.page) return true;
    const char *dir;
    ct_doc_t *d = vars_doc(&dir);
    if (d->bad || d->root.t != CT_OBJ) return false;
    ctv_t old = ct_get(d->root, "values");
    ctv_t list = ct_arr_new(&d->ar, (uint32_t)V.n);
    for (int i = 0; i < V.n; i++) {
        const char *k = lv_textarea_get_text(V.key[i]), *v = lv_textarea_get_text(V.val[i]);
        if (!k[0]) continue;
        ctv_t prev = ct_at(old, (uint32_t)i);
        ctv_t e = prev.t == CT_OBJ ? prev : ct_obj_new(&d->ar, 4);
        ct_sets(&d->ar, e, "key", k);
        ct_sets(&d->ar, e, "value", v);
        if (ct_get(e, "type").t == CT_UNDEF) ct_sets(&d->ar, e, "type", "default");
        ct_setv(&d->ar, e, "enabled", ct_bool(lv_obj_has_state(V.on[i], LV_STATE_CHECKED)));
        ct_arr_push(&d->ar, list.a, e);
    }
    ct_setv(&d->ar, d->root, "values", list);
    bool ok = ct_doc_save(d, dir);
    V.dirty = false;
    if (!ok) aos_ui_toast(_("No se pudo guardar"), 1500);
    return ok;
}

static void vsave_cb(lv_event_t *e)
{
    (void)e;
    ct_kb_hide();
    if (vars_commit()) aos_ui_toast(_("Guardado"), 1000);
}

static void vadd_cb(lv_event_t *e)
{
    (void)e;
    vars_commit();
    const char *dir;
    ct_doc_t *d = vars_doc(&dir);
    if (d->root.t != CT_OBJ) return;
    ctv_t vals = ct_get(d->root, "values");
    if (vals.t != CT_ARR) {
        vals = ct_arr_new(&d->ar, 4);
        ct_setv(&d->ar, d->root, "values", vals);
    }
    ctv_t x = ct_obj_new(&d->ar, 4);
    ct_sets(&d->ar, x, "key", "");
    ct_sets(&d->ar, x, "value", "");
    ct_sets(&d->ar, x, "type", "default");
    ct_setv(&d->ar, x, "enabled", ct_bool(true));
    ct_arr_push(&d->ar, vals.a, x);
    ct_build();
}

static void vdel_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= V.n) return;
    lv_textarea_set_text(V.key[i], "");
    V.dirty = true;
    vars_commit();
    ct_build();
}

static void secret_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= V.n) return;
    lv_textarea_set_password_mode(V.val[i], !lv_textarea_get_password_mode(V.val[i]));
}

void ct_vars_build(void)
{
    const char *dir;
    ct_doc_t *d = vars_doc(&dir);
    ct_doc_refresh(d, dir);
    memset(&V, 0, sizeof V);
    lv_obj_t *page = ct_page();
    V.page = page;
    char safe[160];
    aos_text_safe(safe, sizeof safe, dir ? d->name : _("Globales"));
    lv_obj_t *right = ct_header(page, safe, true);
    ct_round_btn(right, AOS_SYM_PLUS, vadd_cb, NULL);
    ct_round_btn(right, AOS_SYM_CHECK, vsave_cb, NULL);
    ct_header_fit(right);
    lv_obj_t *list = ct_scroll(page, 120);
    lv_obj_set_style_pad_row(list, 10, 0);
    ctv_t vals = ct_get(d->root, "values");
    int32_t w = CA.W - 2 * AOS_UI_PAD;
    int32_t kw = w * 32 / 100, vw = w - kw - 64 - 2 * 72 - 40;
    for (uint32_t i = 0; i < ct_len(vals) && V.n < VROWS; i++) {
        ctv_t x = ct_at(vals, i);
        lv_obj_t *row = ct_box(list, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_t *c = lv_checkbox_create(row);
        lv_checkbox_set_text(c, "");
        ctv_t en = ct_get(x, "enabled");
        if (!(en.t == CT_BOOL && !en.b) && !ct_truthy(ct_get(x, "disabled"))) lv_obj_add_state(c, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(c, AOS_C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_pad_all(c, 10, LV_PART_INDICATOR);
        lv_obj_add_event_cb(c, vchanged_cb, LV_EVENT_VALUE_CHANGED, NULL);
        V.on[V.n] = c;
        lv_obj_t *k = ct_textarea(row, ct_s(ct_get(x, "key")), true, true);
        lv_obj_set_width(k, kw);
        lv_textarea_set_placeholder_text(k, _("nombre"));
        lv_obj_add_event_cb(k, vchanged_cb, LV_EVENT_VALUE_CHANGED, NULL);
        V.key[V.n] = k;
        ct_str_t *vs = ct_to_str(&d->ar, ct_get(x, "value"));
        bool undef = ct_get(x, "value").t == CT_UNDEF || ct_get(x, "value").t == CT_NULL;
        lv_obj_t *v = ct_textarea(row, undef || !vs ? "" : vs->p, true, true);
        lv_obj_set_width(v, vw);
        lv_textarea_set_placeholder_text(v, _("valor"));
        if (!strcmp(ct_s(ct_get(x, "type")), "secret")) lv_textarea_set_password_mode(v, true);
        lv_obj_add_event_cb(v, vchanged_cb, LV_EVENT_VALUE_CHANGED, NULL);
        V.val[V.n] = v;
        ct_round_btn(row, AOS_SYM_EYE_OUTLINE, secret_cb, (void *)(intptr_t)V.n);
        ct_round_btn(row, AOS_SYM_CLOSE, vdel_cb, (void *)(intptr_t)V.n);
        V.n++;
    }
    if (!V.n) ct_label_wrap(list, _("Sin variables. Agregá una con +, o dejá que un test la cree con pm.environment.set(...)."),
                            aos_font_body, AOS_C_DIM);
    ct_label_wrap(list, _("Se guardan en la tarjeta tal cual, también los secretos (sólo se tapan en la pantalla)."),
                  aos_font_caption, AOS_C_DIM);
    ct_kb_attach(page, list);
}

bool ct_vars_back(void)
{
    vars_commit();
    V.page = NULL;
    return false;
}
