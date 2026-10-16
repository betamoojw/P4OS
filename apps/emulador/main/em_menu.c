/*
 * P4OS - Emulador: the pause menu.
 *
 * A sheet over the paused game: the game's name, Continue / Reset / Quit,
 * the save states (four slots by hand and the automatic one, each with the
 * picture it was saved on), and the options: the app's (scale, frame skip,
 * sound, frames a second, resume on open) and the console's own (the NES
 * palette, the GBA's BIOS...). Opened by the pad's ≡ key, the back
 * gesture, SELECT+START on a gamepad; B on the gamepad or the back gesture
 * closes it, and the gamepad's d-pad and A go through it.
 *
 * The sheet covers the picture: while it is up nothing is flipped, LVGL
 * draws as usual, and the game screen presents a whole frame when it goes.
 */
#include "emu.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOT_N      (EMU_SLOTS + 1)     /* the automatic one, then 1..4 */
#define OPT_MAX     16

typedef enum { OPT_APP, OPT_CORE } opt_kind_t;

typedef struct {
    opt_kind_t          kind;
    const char         *key;
    const char         *label;
    const char *const  *values;
    int                 def;
    lv_obj_t           *row;
    lv_obj_t           *val;
} opt_t;

typedef struct emu_menu {
    lv_obj_t      *sheet;
    lv_obj_t      *col;
    lv_obj_t      *tab_save, *tab_load;
    lv_obj_t      *slot[SLOT_N];
    lv_obj_t      *slot_img[SLOT_N];
    lv_obj_t      *slot_txt[SLOT_N];
    lv_image_dsc_t thumb[SLOT_N];
    bool           saving;              /* the slots save (true) or load */
    opt_t          opt[OPT_MAX];
    int            nopt;

    uint32_t       ticket;
    emu_cmd_t      pending;
    int            pending_slot;

    aos_pad_t      pad;
    aos_pad_menu_t pm;
    bool           closing;
} menu_t;

static const char *const V_SCALE[] = { N_("Ajustada"), N_("Entera"), N_("Estirada"), NULL };
static const char *const V_SKIP[]  = { N_("Automático"), N_("Nunca"), N_("1 de cada 2"), N_("2 de cada 3"), NULL };
static const char *const V_ONOFF[] = { N_("No"), N_("Sí"), NULL };

static menu_t *M_of(emu_app_t *a) { return a->M; }

/* ---------------------------------------------------------------------------
 * Building blocks
 * ------------------------------------------------------------------------ */

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *sym, const char *text, uint32_t bg,
                        lv_event_cb_t cb, emu_app_t *a)
{
    lv_obj_t *b = plain(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(b, 96);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A58), LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 4, 0);
    lv_obj_t *s = label(b, sym, &aos_sym_44, EMU_C_TEXT);
    (void)s;
    label(b, text, aos_font_caption, EMU_C_TEXT);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, a);
    return b;
}

static lv_obj_t *section(lv_obj_t *col, const char *text)
{
    lv_obj_t *l = label(col, text, aos_font_small, EMU_C_DIM);
    lv_obj_set_style_pad_top(l, 18, 0);
    return l;
}

/* ---------------------------------------------------------------------------
 * The slots
 * ------------------------------------------------------------------------ */

static void slot_paint(emu_app_t *a, int i)
{
    menu_t *m = M_of(a);
    const emu_game_t *g = emu_game_current(a);
    if (!g || !m->slot[i]) return;
    char path[EMU_PATH_MAX];
    emu_game_state(g, i, path, sizeof path);
    uint32_t mt = 0;
    bool exists = emu_file_exists(path, &mt);

    emu_thumb_free(&m->thumb[i]);
    char tp[EMU_PATH_MAX];
    emu_game_thumb(g, i, tp, sizeof tp);
    if (exists && emu_thumb_read(tp, &m->thumb[i])) {
        lv_image_set_src(m->slot_img[i], &m->thumb[i]);
        lv_obj_remove_flag(m->slot_img[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(m->slot_img[i], LV_OBJ_FLAG_HIDDEN);
    }

    char t[64];
    const char *name = i == 0 ? _("Automático") : NULL;
    char num[24];
    if (!name) {
        snprintf(num, sizeof num, _("Ranura %d"), i);
        name = num;
    }
    if (!exists) {
        snprintf(t, sizeof t, "%s\n%s", name, _("vacía"));
    } else if (aos_hal_time_is_valid() && mt > 1600000000u) {
        time_t tt = (time_t)mt;
        struct tm tm;
        localtime_r(&tt, &tm);
        snprintf(t, sizeof t, "%s\n%02d/%02d %02d:%02d", name, tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    } else {
        snprintf(t, sizeof t, "%s\n%s", name, _("guardado"));
    }
    lv_label_set_text(m->slot_txt[i], t);

    /* loading: an empty slot does nothing; saving: the automatic one is
     * the app's */
    bool usable = m->saving ? i > 0 : exists;
    if (usable) lv_obj_remove_state(m->slot[i], LV_STATE_DISABLED);
    else lv_obj_add_state(m->slot[i], LV_STATE_DISABLED);
    lv_obj_set_style_opa(m->slot[i], usable ? LV_OPA_COVER : 110, 0);
    if (i == 0) {
        if (m->saving) lv_obj_add_flag(m->slot[0], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(m->slot[0], LV_OBJ_FLAG_HIDDEN);
    }
}

static void tabs_paint(menu_t *m)
{
    lv_obj_set_style_bg_color(m->tab_save, lv_color_hex(m->saving ? EMU_C_ACCENT : EMU_C_CARD2), 0);
    lv_obj_set_style_bg_color(m->tab_load, lv_color_hex(!m->saving ? EMU_C_ACCENT2 : EMU_C_CARD2), 0);
}

static void slots_paint(emu_app_t *a)
{
    menu_t *m = M_of(a);
    tabs_paint(m);
    for (int i = 0; i < SLOT_N; i++) slot_paint(a, i);
}

static void tab_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    menu_t *m = M_of(a);
    m->saving = lv_event_get_target(e) == m->tab_save;
    slots_paint(a);
}

static void slot_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    menu_t *m = M_of(a);
    if (m->pending != EMU_CMD_NONE) return;
    lv_obj_t *t = lv_event_get_target(e);
    int i = 0;
    while (i < SLOT_N && m->slot[i] != t) i++;
    if (i == SLOT_N) return;
    m->pending = m->saving ? EMU_CMD_SAVE : EMU_CMD_LOAD;
    m->pending_slot = i;
    m->ticket = emu_run_command(m->pending, i, NULL);
    aos_ui_toast(m->saving ? _("Guardando...") : _("Cargando..."), 600);
}

static lv_obj_t *slot_card(emu_app_t *a, lv_obj_t *parent, int i, int w)
{
    menu_t *m = M_of(a);
    const emu_game_t *g = emu_game_current(a);
    int tw = g->core->max_w / 2, th = g->core->max_h / 2;
    int ih = w * th / tw;
    lv_obj_t *c = plain(parent);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(c, w, ih + 76);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(EMU_C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(EMU_C_CARD2), LV_STATE_PRESSED);
    lv_obj_set_style_clip_corner(c, true, 0);

    lv_obj_t *well = plain(c);
    lv_obj_set_size(well, w, ih);
    lv_obj_set_style_bg_color(well, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
    lv_obj_remove_flag(well, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *img = lv_image_create(well);
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(img, w, ih);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    m->slot_img[i] = img;

    lv_obj_t *t = label(c, "", aos_font_caption, EMU_C_TEXT);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(t, w - 8);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -8);
    m->slot_txt[i] = t;
    m->slot[i] = c;
    lv_obj_add_event_cb(c, slot_cb, LV_EVENT_CLICKED, a);
    return c;
}

/* ---------------------------------------------------------------------------
 * Options
 * ------------------------------------------------------------------------ */

static int opt_get(emu_app_t *a, const opt_t *o)
{
    const emu_game_t *g = emu_game_current(a);
    return o->kind == OPT_APP ? emu_pref(o->key, o->def) : emu_core_pref(g->core, o->key, o->def);
}

static int opt_count(const opt_t *o)
{
    int n = 0;
    while (o->values[n]) n++;
    return n;
}

static void opt_paint(emu_app_t *a, opt_t *o)
{
    int v = opt_get(a, o);
    int n = opt_count(o);
    if (v < 0 || v >= n) v = o->def;
    char t[80];
    snprintf(t, sizeof t, "%s", _(o->values[v]));
    lv_label_set_text(o->val, t);
}

static void opt_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    menu_t *m = M_of(a);
    if (m->pending != EMU_CMD_NONE) return;
    lv_obj_t *t = lv_event_get_target(e);
    opt_t *o = NULL;
    for (int i = 0; i < m->nopt; i++) if (m->opt[i].row == t) o = &m->opt[i];
    if (!o) return;
    int n = opt_count(o);
    int v = (opt_get(a, o) + 1) % n;
    const emu_game_t *g = emu_game_current(a);
    if (o->kind == OPT_APP) {
        emu_pref_set(o->key, v);
        if (!strcmp(o->key, "sound")) emu_run_sound(v != 0);
        if (!strcmp(o->key, "skip")) emu_run_skip(v);
        if (!strcmp(o->key, "scale")) emu_game_relayout(a);
    } else {
        emu_core_pref_set(g->core, o->key, v);
        m->pending = EMU_CMD_OPTION;
        m->ticket = emu_run_command(EMU_CMD_OPTION, v, o->key);
    }
    opt_paint(a, o);
}

static void opt_row(emu_app_t *a, lv_obj_t *col, opt_kind_t kind, const char *key, const char *lbl,
                    const char *const *values, int def)
{
    menu_t *m = M_of(a);
    if (m->nopt == OPT_MAX) return;
    opt_t *o = &m->opt[m->nopt++];
    o->kind = kind;
    o->key = key;
    o->label = lbl;
    o->values = values;
    o->def = def;
    lv_obj_t *r = plain(col);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, LV_PCT(100), 76);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(EMU_C_CARD), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(EMU_C_CARD2), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(r, 22, 0);
    lv_obj_t *l = label(r, _(lbl), aos_font_body, EMU_C_TEXT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *ch = label(r, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, EMU_C_DIM);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
    o->val = label(r, "", aos_font_body, EMU_C_ACCENT);
    lv_obj_align(o->val, LV_ALIGN_RIGHT_MID, -38, 0);
    o->row = r;
    lv_obj_add_event_cb(r, opt_cb, LV_EVENT_CLICKED, a);
    opt_paint(a, o);
}

/* ---------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------ */

static void continue_cb(lv_event_t *e)
{
    emu_menu_close(lv_event_get_user_data(e));
}

static void reset_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    menu_t *m = M_of(a);
    if (m->pending != EMU_CMD_NONE) return;
    m->pending = EMU_CMD_RESET;
    m->ticket = emu_run_command(EMU_CMD_RESET, 0, NULL);
}

static void quit_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    /* closing deletes this sheet: not from inside its own event */
    M_of(a)->closing = true;
}

static void pad_items(emu_app_t *a, lv_obj_t **items, int *n)
{
    menu_t *m = M_of(a);
    *n = 0;
    lv_obj_t *row = lv_obj_get_child(m->col, 1);       /* the actions */
    for (uint32_t i = 0; row && i < lv_obj_get_child_count(row); i++) items[(*n)++] = lv_obj_get_child(row, (int32_t)i);
    items[(*n)++] = m->tab_save;
    items[(*n)++] = m->tab_load;
    for (int i = 0; i < SLOT_N; i++) items[(*n)++] = m->slot[i];
    for (int i = 0; i < m->nopt && *n < AOS_PAD_MENU_MAX; i++) items[(*n)++] = m->opt[i].row;
}

/* ---------------------------------------------------------------------------
 * The sheet
 * ------------------------------------------------------------------------ */

static void build(emu_app_t *a)
{
    menu_t *m = M_of(a);
    const emu_game_t *g = emu_game_current(a);
    int W = lv_obj_get_width(a->root), H = lv_obj_get_height(a->root);
    bool land = W > H;

    m->sheet = plain(a->root);
    lv_obj_set_size(m->sheet, W, H);
    lv_obj_set_style_bg_color(m->sheet, lv_color_hex(EMU_C_BG), 0);
    lv_obj_set_style_bg_opa(m->sheet, 244, 0);
    lv_obj_add_flag(m->sheet, LV_OBJ_FLAG_CLICKABLE);

    int cw = land ? 760 : W - 48;
    m->col = lv_obj_create(m->sheet);
    lv_obj_remove_style_all(m->col);
    lv_obj_set_size(m->col, cw, H);
    lv_obj_align(m->col, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(m->col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m->col, 12, 0);
    lv_obj_set_style_pad_top(m->col, 36, 0);
    lv_obj_set_style_pad_bottom(m->col, 60, 0);
    lv_obj_set_style_pad_hor(m->col, 12, 0);
    lv_obj_add_flag(m->col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(m->col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(m->col, LV_SCROLLBAR_MODE_OFF);

    /* the game */
    lv_obj_t *head = plain(m->col);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *tl = label(head, g->title, aos_font_title, EMU_C_TEXT);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(tl, LV_PCT(100));
    char sub[96];
    snprintf(sub, sizeof sub, "%s%s%s", g->core->name, g->tags[0] ? "  ·  " : "", g->tags);
    lv_obj_t *sl = label(head, sub, aos_font_small, EMU_C_DIM);
    lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(sl, LV_PCT(100));

    /* continue, reset, quit */
    lv_obj_t *row = plain(m->col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    button(row, AOS_SYM_PLAY, _("Continuar"), EMU_C_ACCENT, continue_cb, a);
    button(row, AOS_SYM_RESTART, _("Reiniciar"), EMU_C_CARD2, reset_cb, a);
    button(row, AOS_SYM_CLOSE, _("Salir"), EMU_C_CARD2, quit_cb, a);

    /* the states */
    section(m->col, _("Estados"));
    lv_obj_t *tabs = plain(m->col);
    lv_obj_set_size(tabs, LV_PCT(100), 64);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 10, 0);
    lv_obj_add_flag(tabs, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    const char *names[2] = { _("Guardar"), _("Cargar") };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *t = plain(tabs);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_height(t, 64);
        lv_obj_set_flex_grow(t, 1);
        lv_obj_set_style_radius(t, 32, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_t *l = label(t, names[i], aos_font_body, EMU_C_TEXT);
        lv_obj_center(l);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, a);
        if (i == 0) m->tab_save = t;
        else m->tab_load = t;
    }

    lv_obj_t *grid = plain(m->col);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    int per = 5;
    int sw = (cw - 24 - 12 * (per - 1)) / per;
    for (int i = 0; i < SLOT_N; i++) slot_card(a, grid, i, sw);

    /* options */
    section(m->col, _("Opciones"));
    opt_row(a, m->col, OPT_APP, "scale", N_("Tamaño de la imagen"), V_SCALE, EMU_SCALE_FIT);
    opt_row(a, m->col, OPT_APP, "skip", N_("Saltear cuadros"), V_SKIP, EMU_SKIP_AUTO);
    opt_row(a, m->col, OPT_APP, "sound", N_("Sonido"), V_ONOFF, 1);
    opt_row(a, m->col, OPT_APP, "fps", N_("Mostrar cuadros por segundo"), V_ONOFF, 0);
    opt_row(a, m->col, OPT_APP, "resume", N_("Seguir donde quedó al abrir"), V_ONOFF, 1);
    if (g->core->n_options) {
        char t[64];
        snprintf(t, sizeof t, "%s", g->core->name);
        section(m->col, t);
        for (int i = 0; i < g->core->n_options; i++) {
            const emu_option_t *o = &g->core->options[i];
            opt_row(a, m->col, OPT_CORE, o->key, o->label, o->values, o->def);
        }
    }

    m->saving = true;
    slots_paint(a);

    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int n;
    pad_items(a, items, &n);
    aos_pad_menu_set(&m->pm, items, n, 0);
    aos_pad_reset(&m->pad, lv_tick_get());
}

void emu_menu_open(emu_app_t *a)
{
    if (!a->M) {
        a->M = lv_malloc_zeroed(sizeof(menu_t));
        if (!a->M) return;
    }
    menu_t *m = a->M;
    if (m->sheet) return;
    memset(m, 0, sizeof *m);
    build(a);
}

static void free_thumbs(menu_t *m)
{
    for (int i = 0; i < SLOT_N; i++) emu_thumb_free(&m->thumb[i]);
}

void emu_menu_close(emu_app_t *a)
{
    menu_t *m = a->M;
    if (!m || !m->sheet) return;
    aos_pad_menu_clear(&m->pm);
    lv_obj_delete(m->sheet);
    m->sheet = NULL;
    free_thumbs(m);
    emu_game_resume(a);
}

bool emu_menu_is_open(emu_app_t *a)
{
    return a->M && a->M->sheet;
}

void emu_menu_resize(emu_app_t *a)
{
    menu_t *m = a->M;
    if (!m || !m->sheet) return;
    bool saving = m->saving;
    aos_pad_menu_clear(&m->pm);
    lv_obj_delete(m->sheet);
    m->sheet = NULL;
    free_thumbs(m);
    memset(m, 0, sizeof *m);
    build(a);
    m->saving = saving;
    slots_paint(a);
}

void emu_menu_step(emu_app_t *a)
{
    menu_t *m = a->M;
    if (!m || !m->sheet) return;

    if (m->closing) {
        m->closing = false;
        emu_game_close(a, true);
        return;
    }

    if (m->pending != EMU_CMD_NONE) {
        bool ok = false;
        if (!emu_run_done(m->ticket, &ok)) return;
        emu_cmd_t c = m->pending;
        int slot = m->pending_slot;
        m->pending = EMU_CMD_NONE;
        switch (c) {
        case EMU_CMD_SAVE:
            aos_ui_toast(ok ? _("Estado guardado") : _("No se pudo guardar el estado"), 1500);
            slot_paint(a, slot);
            break;
        case EMU_CMD_LOAD:
            if (ok) {
                emu_menu_close(a);
                return;
            }
            aos_ui_toast(_("No se pudo cargar el estado"), 1800);
            break;
        case EMU_CMD_RESET:
            emu_menu_close(a);
            return;
        default:
            break;
        }
    }

    aos_pad_update(&m->pad, lv_tick_get());
    if (aos_pad_pressed(&m->pad, AOS_PAD_B | AOS_PAD_START)) {
        emu_menu_close(a);
        return;
    }
    if (aos_pad_pressed(&m->pad, AOS_PAD_L | AOS_PAD_R)) {
        m->saving = aos_pad_pressed(&m->pad, AOS_PAD_L);
        slots_paint(a);
    }
    uint8_t before = m->pm.sel;
    aos_pad_menu_step(&m->pm, &m->pad);
    if (m->pm.sel != before || m->pm.shown) {
        lv_obj_t *s = aos_pad_menu_selected(&m->pm);
        if (s && m->pm.sel != before) lv_obj_scroll_to_view_recursive(s, LV_ANIM_ON);
    }
}
