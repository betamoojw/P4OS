/*
 * P4OS - Emulador: the library.
 *
 *   Emulador                       12 juegos
 *   +--------------------------------------+
 *   | [picture]  CONTINUAR                 |   the last game played, with the
 *   |            Super Something           |   picture of its automatic state
 *   |            NES · 3 h 20 min      ▶   |
 *   +--------------------------------------+
 *   (Todos 12) (NES 7) (GBA 5)     [A-Z]
 *   [■] Game one                         >
 *   [■] Game two  (USA)                  >
 *
 * The games are whatever is in each console's folder on the card (see
 * em_files.c); a game played before shows the picture it was left on.
 * Tap plays (from where it was left, when there is an automatic state);
 * a long press asks whether to start from the beginning instead.
 *
 * With a gamepad: up and down go through the cards, A plays, L and R
 * change console, START plays the last game.
 */
#include "emu.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_pad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THUMBS_MAX  32          /* pictures of played games loaded at once */
#define ROWS_MAX    600         /* rows built; past that, the search narrows */
#define FOCUS_MAX   (ROWS_MAX + 16)

typedef struct emu_lib {
    emu_games_t     games;
    int             filter;         /* -1 all, else index into emu_cores */
    bool            by_recent;
    emu_game_t      last;
    bool            has_last;

    lv_obj_t       *scr;
    lv_obj_t       *col;
    lv_obj_t       *list;
    lv_obj_t       *count;
    lv_obj_t       *ask;            /* the "from the beginning?" sheet */
    emu_game_t      ask_game;

    lv_image_dsc_t  thumb[THUMBS_MAX];
    int             nthumb;
    lv_image_dsc_t  hero_thumb;

    lv_obj_t       *focus[FOCUS_MAX];
    int             nfocus, sel;
    bool            focus_shown;
    aos_pad_t       pad;

    bool            dirty;          /* rebuild on the next tick */
    uint32_t        req_ms;         /* abrir.txt last looked for */
    bool            play_req;       /* open this game on the next tick: never from */
    bool            play_fresh;     /* inside the event of an object the library */
    emu_game_t      play_game;      /* is about to delete */
} lib_t;

static lib_t *L_of(emu_app_t *a) { return a->L; }

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, const lv_font_t *f, uint32_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}

static void played_text(uint32_t s, char *out, size_t n)
{
    if (s < 60) snprintf(out, n, "%s", _("menos de 1 min"));
    else if (s < 3600) snprintf(out, n, "%u min", (unsigned)(s / 60));
    else snprintf(out, n, "%u h %u min", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
}

/* A console's chip: its short name on its colour. */
static lv_obj_t *badge(lv_obj_t *parent, const emu_core_t *core, int w, int h)
{
    const emu_skin_t *s = emu_skin(core);
    lv_obj_t *b = plain(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(s->badge), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(s->body2), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = text(b, core->short_name ? core->short_name : core->name, aos_font_label, 0xFFFFFF);
    lv_obj_center(l);
    return b;
}

/* ---------------------------------------------------------------------------
 * Focus, for a gamepad
 * ------------------------------------------------------------------------ */

static void focus_mark(lib_t *l, int i, bool on)
{
    if (i < 0 || i >= l->nfocus || !l->focus[i]) return;
    lv_obj_t *o = l->focus[i];
    lv_obj_set_style_outline_width(o, on ? 5 : 0, 0);
    lv_obj_set_style_outline_pad(o, on ? 3 : 0, 0);
    lv_obj_set_style_outline_color(o, lv_color_white(), 0);
}

static void focus_add(lib_t *l, lv_obj_t *o)
{
    if (l->nfocus < FOCUS_MAX) l->focus[l->nfocus++] = o;
}

static void focus_move(lib_t *l, int d)
{
    if (!l->nfocus) return;
    if (!l->focus_shown) {
        l->focus_shown = true;
        if (l->sel >= l->nfocus) l->sel = 0;
    } else {
        focus_mark(l, l->sel, false);
        l->sel += d;
        if (l->sel < 0) l->sel = 0;
        if (l->sel >= l->nfocus) l->sel = l->nfocus - 1;
    }
    focus_mark(l, l->sel, true);
    lv_obj_scroll_to_view_recursive(l->focus[l->sel], LV_ANIM_ON);
}

/* ---------------------------------------------------------------------------
 * Playing
 * ------------------------------------------------------------------------ */

static void play_now(emu_app_t *a, const emu_game_t *g, bool fresh)
{
    emu_game_t copy = *g;
    if (fresh) {
        /* from the beginning: the automatic state goes */
        char st[EMU_PATH_MAX], th[EMU_PATH_MAX];
        emu_game_state(&copy, 0, st, sizeof st);
        emu_game_thumb(&copy, 0, th, sizeof th);
        remove(st);
        remove(th);
        copy.has_auto = false;
    }
    emu_lib_hide(a);
    if (!emu_game_open(a, &copy)) emu_lib_show(a);
}

static void play(emu_app_t *a, const emu_game_t *g, bool fresh)
{
    lib_t *l = L_of(a);
    l->play_game = *g;
    l->play_fresh = fresh;
    l->play_req = true;
}

static void ask_close(lib_t *l)
{
    if (l->ask) lv_obj_delete_async(l->ask);
    l->ask = NULL;
}

static void ask_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    lib_t *l = L_of(a);
    intptr_t fresh = (intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    emu_game_t g = l->ask_game;
    ask_close(l);
    if (fresh >= 0) play(a, &g, fresh != 0);
}

static void ask_fresh(emu_app_t *a, const emu_game_t *g)
{
    lib_t *l = L_of(a);
    if (l->ask) return;
    l->ask_game = *g;
    l->ask = plain(l->scr);
    lv_obj_set_size(l->ask, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(l->ask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(l->ask, 170, 0);
    lv_obj_add_flag(l->ask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(l->ask, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(l->ask, ask_cb, LV_EVENT_CLICKED, a);

    lv_obj_t *card = plain(l->ask);
    lv_obj_set_size(card, 600, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(EMU_C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_t *t = text(card, g->title, aos_font_title, EMU_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_t *s = text(card, _("Empezar desde el principio borra el estado automático. "
                               "Las partidas guardadas por el juego y las ranuras quedan."),
                       aos_font_small, EMU_C_DIM);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s, LV_PCT(100));
    static const char *const T[2] = { N_("Seguir donde quedó"), N_("Desde el principio") };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = plain(card);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(b, LV_PCT(100), 80);
        lv_obj_set_style_radius(b, 40, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i == 0 ? EMU_C_ACCENT : EMU_C_CARD2), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_t *bl = text(b, _(T[i]), aos_font_body, EMU_C_TEXT);
        lv_obj_center(bl);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, ask_cb, LV_EVENT_CLICKED, a);
    }
}

static void row_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    lib_t *l = L_of(a);
    lv_obj_t *o = lv_event_get_current_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(o);
    if (i < 0 || i >= l->games.n) return;
    const emu_game_t *g = &l->games.v[i];
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        if (g->has_auto) ask_fresh(a, g);
        return;
    }
    play(a, g, false);
}

static void hero_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    lib_t *l = L_of(a);
    if (!l->has_last) return;
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        if (l->last.has_auto) ask_fresh(a, &l->last);
        return;
    }
    play(a, &l->last, false);
}

static void chip_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    lib_t *l = L_of(a);
    l->filter = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    emu_pref_set("lib.filter", l->filter >= 0 ? emu_core_code(emu_cores[l->filter]) : 0);
    l->dirty = true;
}

static void sort_cb(lv_event_t *e)
{
    emu_app_t *a = lv_event_get_user_data(e);
    lib_t *l = L_of(a);
    l->by_recent = !l->by_recent;
    emu_pref_set("lib.recent", l->by_recent);
    l->dirty = true;
}

/* ---------------------------------------------------------------------------
 * Building
 * ------------------------------------------------------------------------ */

static void thumbs_free(lib_t *l)
{
    for (int i = 0; i < l->nthumb; i++) emu_thumb_free(&l->thumb[i]);
    l->nthumb = 0;
    emu_thumb_free(&l->hero_thumb);
}

/* A console's picture width for a height: its shape as shown, the wide
 * pixels of an Atari counted, never narrower than square. */
static int pic_w(const emu_core_t *c, int h)
{
    float pa = c->pixel_aspect > 0.01f ? c->pixel_aspect : 1.0f;
    int w = (int)(h * c->max_w * pa / c->max_h);
    return w < h ? h : w;
}

static lv_obj_t *picture(lv_obj_t *parent, const emu_game_t *g, lv_image_dsc_t *dsc, int w, int h)
{
    lv_obj_t *well = plain(parent);
    lv_obj_set_size(well, w, h);
    lv_obj_set_style_radius(well, 14, 0);
    lv_obj_set_style_clip_corner(well, true, 0);
    lv_obj_set_style_bg_color(well, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
    lv_obj_remove_flag(well, LV_OBJ_FLAG_CLICKABLE);
    char tp[EMU_PATH_MAX];
    emu_game_thumb(g, 0, tp, sizeof tp);
    if (g->has_auto && dsc && emu_thumb_read(tp, dsc)) {
        lv_obj_t *img = lv_image_create(well);
        lv_image_set_src(img, dsc);
        lv_obj_set_size(img, w, h);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_delete(well);
        well = badge(parent, g->core, w, h);
    }
    return well;
}

static void build_hero(emu_app_t *a, lib_t *l, lv_obj_t *col, int cw)
{
    if (!l->has_last) return;
    const emu_game_t *g = &l->last;
    lv_obj_t *c = plain(col);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(c, cw, 232);
    lv_obj_set_style_radius(c, 28, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(EMU_C_CARD2), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(emu_skin(g->core)->body2), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_add_event_cb(c, hero_cb, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(c, hero_cb, LV_EVENT_LONG_PRESSED, a);
    focus_add(l, c);

    int ph = 192, pw = pic_w(g->core, ph);
    lv_obj_t *pic = picture(c, g, &l->hero_thumb, pw, ph);
    lv_obj_align(pic, LV_ALIGN_LEFT_MID, 0, 0);

    int tx = pw + 24, tw = cw - 40 - tx - 90;
    lv_obj_t *k = text(c, l->last.has_auto ? _("CONTINUAR") : _("JUGAR DE NUEVO"), aos_font_label, EMU_C_ACCENT);
    lv_obj_set_pos(k, tx, 10);
    lv_obj_t *t = text(c, g->title, aos_font_title, EMU_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, tw);
    lv_obj_set_height(t, lv_font_get_line_height(aos_font_title));
    lv_obj_set_pos(t, tx, 44);
    char p[64], sub[120];
    played_text(g->played_s, p, sizeof p);
    snprintf(sub, sizeof sub, "%s  ·  %s", g->core->short_name ? g->core->short_name : g->core->name, p);
    lv_obj_t *s = text(c, sub, aos_font_small, EMU_C_DIM);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s, tw);
    lv_obj_set_height(s, lv_font_get_line_height(aos_font_small));
    lv_obj_set_pos(s, tx, 96);

    lv_obj_t *go = plain(c);
    lv_obj_set_size(go, 80, 80);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(go, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(go, lv_color_hex(EMU_C_ACCENT), 0);
    lv_obj_set_style_bg_opa(go, LV_OPA_COVER, 0);
    lv_obj_remove_flag(go, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *gl = text(go, AOS_SYM_PLAY, &aos_sym_44, 0xFFFFFF);
    lv_obj_align(gl, LV_ALIGN_CENTER, 3, 0);
}

static lv_obj_t *chip(emu_app_t *a, lv_obj_t *parent, const char *t, bool on, uint32_t color, intptr_t ud,
                      lv_event_cb_t cb)
{
    lv_obj_t *c = plain(parent);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(c, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_pad_hor(c, 24, 0);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(on ? color : EMU_C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, on ? 0 : 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(EMU_C_LINE), 0);
    lv_obj_t *l = text(c, t, aos_font_small, on ? 0xFFFFFF : EMU_C_DIM);
    lv_obj_center(l);
    lv_obj_set_user_data(c, (void *)ud);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, a);
    return c;
}

static void build_chips(emu_app_t *a, lib_t *l, lv_obj_t *col)
{
    lv_obj_t *row = plain(col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_row(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    int total = l->games.n;
    char t[48];
    snprintf(t, sizeof t, "%s  %d", _("Todos"), total);
    focus_add(l, chip(a, row, t, l->filter < 0, EMU_C_ACCENT2, -1, chip_cb));
    for (int i = 0; emu_cores[i]; i++) {
        int n = 0;
        for (int j = 0; j < l->games.n; j++) n += l->games.v[j].core == emu_cores[i];
        /* only the consoles that have games, and the one chosen */
        if (!n && l->filter != i) continue;
        snprintf(t, sizeof t, "%s  %d", emu_cores[i]->short_name ? emu_cores[i]->short_name : emu_cores[i]->name, n);
        focus_add(l, chip(a, row, t, l->filter == i, emu_skin(emu_cores[i])->badge, i, chip_cb));
    }
    focus_add(l, chip(a, row, l->by_recent ? _("Recientes") : _("A-Z"), false, EMU_C_CARD2, 0, sort_cb));
}

static void build_row(emu_app_t *a, lib_t *l, lv_obj_t *list, int gi, int w)
{
    const emu_game_t *g = &l->games.v[gi];
    lv_obj_t *r = plain(list);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, w, 112);
    lv_obj_set_style_radius(r, 20, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(EMU_C_CARD), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(EMU_C_CARD2), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(r, 14, 0);
    lv_obj_set_user_data(r, (void *)(intptr_t)gi);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_LONG_PRESSED, a);
    focus_add(l, r);

    int ph = 84, pw = pic_w(g->core, ph);
    lv_image_dsc_t *dsc = g->has_auto && l->nthumb < THUMBS_MAX ? &l->thumb[l->nthumb] : NULL;
    lv_obj_t *pic = picture(r, g, dsc, pw, ph);
    if (dsc && dsc->data) l->nthumb++;
    lv_obj_align(pic, LV_ALIGN_LEFT_MID, 0, 0);

    int tx = pw + 18, tw = w - 28 - tx - 30;
    lv_obj_t *t = text(r, g->title, aos_font_body, EMU_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, tw);
    lv_obj_set_height(t, lv_font_get_line_height(aos_font_body));
    lv_obj_set_pos(t, tx, 18);
    /* the console (when all are listed), the tags, the time played: what
     * there is, joined by dots */
    char sub[160] = "", p[48] = "";
    const char *parts[3] = { l->filter < 0 ? (g->core->short_name ? g->core->short_name : g->core->name) : NULL,
                             g->tags[0] ? g->tags : NULL, NULL };
    if (g->last) {
        played_text(g->played_s, p, sizeof p);
        parts[2] = p;
    }
    for (int i = 0; i < 3; i++) {
        if (!parts[i]) continue;
        size_t n = strlen(sub);
        snprintf(sub + n, sizeof sub - n, "%s%s", n ? "  ·  " : "", parts[i]);
    }
    if (sub[0]) {
        lv_obj_t *s = text(r, sub, aos_font_caption, EMU_C_DIM);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s, tw);
        lv_obj_set_height(s, lv_font_get_line_height(aos_font_caption));
        lv_obj_set_pos(s, tx, 60);
    } else {
        lv_obj_align(t, LV_ALIGN_LEFT_MID, tx, 0);
    }
    lv_obj_t *ch = text(r, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, EMU_C_DIM);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void build_empty(lib_t *l, lv_obj_t *col)
{
    lv_obj_t *c = plain(col);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, 28, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(EMU_C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c, 32, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_t *t = text(c, _("Todavía no hay juegos"), aos_font_title, EMU_C_TEXT);
    (void)t;
    char body[600], dirs[300] = "";
    for (int i = 0; emu_cores[i]; i++) {
        if (l->filter >= 0 && l->filter != i) continue;
        size_t n = strlen(dirs);
        snprintf(dirs + n, sizeof dirs - n, "\n%s/%s   (%s)", emu_root(), emu_cores[i]->id, emu_cores[i]->exts);
    }
    snprintf(body, sizeof body, "%s%s\n\n%s", _("Copiá los juegos a la tarjeta, una carpeta por consola:"), dirs,
             _("También se pueden subir desde el portal web de la placa, en la página Emulador. "
               "Los archivos de sistema (como la BIOS de la GBA) van en la carpeta bios de cada consola."));
    lv_obj_t *b = text(c, body, aos_font_small, EMU_C_DIM);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(b, LV_PCT(100));
}

static void build(emu_app_t *a)
{
    lib_t *l = L_of(a);
    thumbs_free(l);
    l->nfocus = 0;
    l->focus_shown = false;
    l->sel = 0;
    if (l->scr) lv_obj_clean(l->scr);
    l->ask = NULL;

    int W = lv_obj_get_width(a->root);
    bool land = W > lv_obj_get_height(a->root);
    int cw = land ? 1100 : W - 40;
    if (cw > W - 40) cw = W - 40;

    l->col = lv_obj_create(l->scr);
    lv_obj_remove_style_all(l->col);
    lv_obj_set_size(l->col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(l->col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(l->col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(l->col, 16, 0);
    lv_obj_set_style_pad_top(l->col, 28, 0);
    lv_obj_set_style_pad_bottom(l->col, 60, 0);
    lv_obj_add_flag(l->col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(l->col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l->col, LV_SCROLLBAR_MODE_ACTIVE);

    /* the title */
    lv_obj_t *head = plain(l->col);
    lv_obj_set_size(head, cw, 64);
    lv_obj_t *ti = text(head, _("Emulador"), aos_font_large, EMU_C_TEXT);
    lv_obj_align(ti, LV_ALIGN_LEFT_MID, 0, 0);
    l->count = text(head, "", aos_font_small, EMU_C_DIM);
    lv_obj_align(l->count, LV_ALIGN_RIGHT_MID, 0, 6);

    l->has_last = emu_recent_last(&l->last);
    build_hero(a, l, l->col, cw);

    lv_obj_t *cc = plain(l->col);
    lv_obj_set_size(cc, cw, LV_SIZE_CONTENT);
    lv_obj_add_flag(cc, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    build_chips(a, l, cc);

    /* the games */
    int shown = 0;
    l->list = plain(l->col);
    lv_obj_set_size(l->list, cw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(l->list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(l->list, 14, 0);
    lv_obj_set_style_pad_row(l->list, 12, 0);
    lv_obj_add_flag(l->list, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    int rw = land ? (cw - 14) / 2 : cw;
    emu_games_sort(&l->games, l->by_recent);
    for (int i = 0; i < l->games.n && shown < ROWS_MAX; i++) {
        if (l->filter >= 0 && l->games.v[i].core != emu_cores[l->filter]) continue;
        build_row(a, l, l->list, i, rw);
        shown++;
    }
    if (!shown) build_empty(l, l->list);

    char t[64];
    snprintf(t, sizeof t, l->games.n == 1 ? _("%d juego") : _("%d juegos"), l->games.n);
    lv_label_set_text(l->count, t);
}

/* ---------------------------------------------------------------------------
 * From the app
 * ------------------------------------------------------------------------ */

void emu_lib_show(emu_app_t *a)
{
    if (!a->L) {
        a->L = lv_malloc_zeroed(sizeof(lib_t));
        if (!a->L) return;
        a->L->filter = -1;
    }
    lib_t *l = a->L;
    /* the console chosen, kept by its stable code (0: all) */
    int code = emu_pref("lib.filter", 0);
    l->filter = -1;
    for (int i = 0; code && emu_cores[i]; i++) {
        if (emu_core_code(emu_cores[i]) == code) l->filter = i;
    }
    l->by_recent = emu_pref("lib.recent", 1) != 0;

    if (!l->scr) {
        l->scr = plain(a->root);
        lv_obj_set_size(l->scr, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(l->scr, lv_color_hex(EMU_C_BG), 0);
        lv_obj_set_style_bg_opa(l->scr, LV_OPA_COVER, 0);
    }
    lv_obj_remove_flag(l->scr, LV_OBJ_FLAG_HIDDEN);
    emu_games_free(&l->games);
    uint64_t t0 = aos_hal_uptime_us();
    emu_games_scan(NULL, &l->games);
    aos_hal_log(EMU_TAG, "library: %d games in %u ms", l->games.n,
                (unsigned)((aos_hal_uptime_us() - t0) / 1000));
    build(a);
    aos_pad_reset(&l->pad, lv_tick_get());
}

void emu_lib_hide(emu_app_t *a)
{
    lib_t *l = a->L;
    if (!l) return;
    if (l->scr) {
        lv_obj_delete(l->scr);
        l->scr = NULL;
    }
    thumbs_free(l);
    emu_games_free(&l->games);
    l->nfocus = 0;
}

void emu_lib_resize(emu_app_t *a)
{
    lib_t *l = a->L;
    if (!l || !l->scr) return;
    build(a);
}

void emu_lib_destroy(emu_app_t *a)
{
    emu_lib_hide(a);
    lv_free(a->L);
    a->L = NULL;
}

/* A game asked for from outside (the portal's "Jugar en la placa", a
 * test): emulador/abrir.txt holds its key, "<console>/<path>". Looked for
 * about once a second while the library is up; taken and deleted. */
static bool open_request(emu_app_t *a, lib_t *l)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - l->req_ms < 1000) return false;
    l->req_ms = now;
    char path[EMU_PATH_MAX], key[EMU_PATH_MAX] = "";
    snprintf(path, sizeof path, "%s/abrir.txt", emu_root());
    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(key, sizeof key, f)) key[0] = 0;
    fclose(f);
    remove(path);
    key[strcspn(key, "\r\n")] = 0;
    for (int i = 0; i < l->games.n; i++) {
        char k[EMU_PATH_MAX];
        emu_game_key(&l->games.v[i], k, sizeof k);
        if (!strcmp(k, key)) {
            aos_hal_log(EMU_TAG, "asked to open %s", key);
            play(a, &l->games.v[i], false);
            return true;
        }
    }
    aos_hal_log(EMU_TAG, "asked to open %s, which is not in the library", key);
    return false;
}

void emu_lib_tick(emu_app_t *a)
{
    lib_t *l = a->L;
    if (!l || !l->scr) return;
    open_request(a, l);
    if (l->play_req) {
        l->play_req = false;
        emu_game_t g = l->play_game;
        play_now(a, &g, l->play_fresh);
        return;
    }
    if (l->dirty) {
        l->dirty = false;
        build(a);
        return;
    }
    aos_pad_update(&l->pad, lv_tick_get());
    const aos_pad_t *p = &l->pad;
    if (l->ask) {
        if (aos_pad_pressed(p, AOS_PAD_A)) {
            emu_game_t g = l->ask_game;
            ask_close(l);
            play(a, &g, false);
        } else if (aos_pad_pressed(p, AOS_PAD_B)) {
            ask_close(l);
        }
        return;
    }
    if (aos_pad_repeat(p, AOS_PAD_UP | AOS_PAD_LEFT)) focus_move(l, -1);
    if (aos_pad_repeat(p, AOS_PAD_DOWN | AOS_PAD_RIGHT)) focus_move(l, 1);
    if (aos_pad_pressed(p, AOS_PAD_A) && l->focus_shown && l->sel < l->nfocus) {
        lv_obj_send_event(l->focus[l->sel], LV_EVENT_CLICKED, NULL);
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_START) && l->has_last) {
        play(a, &l->last, false);
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_L | AOS_PAD_R)) {
        int nc = 0;
        while (emu_cores[nc]) nc++;
        int f = l->filter + (aos_pad_pressed(p, AOS_PAD_R) ? 1 : -1);
        if (f < -1) f = nc - 1;
        if (f >= nc) f = -1;
        l->filter = f;
        emu_pref_set("lib.filter", f >= 0 ? emu_core_code(emu_cores[f]) : 0);
        l->dirty = true;
    }
}
