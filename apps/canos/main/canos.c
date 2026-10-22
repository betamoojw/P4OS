/*
 * P4OS - Caños: connect the plant's pipes before the liquid runs.
 *
 * Two ways to play, each in three difficulties:
 *
 *   Girar      the board is full of pipes turned any way: tap them to turn
 *              them until the run from the tank to the drain is closed.
 *   Completar  only some pipes are there, bolted down; the tray below offers
 *              the rest (and a few that are not needed), each as it is.
 *
 *   Fácil      no clock: you open the valve when the run looks right.
 *   Normal     the valve opens by itself when the countdown ends; the liquid
 *   Difícil    then runs and you can still fix what it has not reached.
 *              Difícil has less time and a faster liquid.
 *
 * Six worlds, each a plant with its own liquid (water, fuel, acid, crude,
 * lava, liquid nitrogen) that runs and looks its own way, ten levels each.
 * The levels are generated from their number, so they are the same every
 * time and on every board.
 *
 * How it is put together:
 *
 *   cn_game    the rules: the grid, the generator, the liquid's run
 *   cn_draw    the board's pixels: pipes, floor, liquid, the leak
 *   cn_world   the six worlds' numbers
 *   cn_snd     clicks, the valve, the liquid's own sound
 *
 * and here the screens: the menu (mode, difficulty, worlds), a world's ten
 * levels, the game with its HUD, and the pause, win and leak panels.
 *
 * The board is one lv_canvas over a PSRAM picture that cn_draw writes; each
 * frame only the cells that move are drawn and handed to LVGL as dirty
 * areas. Lying down, the upright grid is shown turned a quarter, so a level
 * plays the same in both orientations and the screen can turn mid-game (the
 * app is built again and the game, which lives in a static, comes back
 * paused).
 *
 * A USB gamepad plays it: the D-pad moves a frame over the board, A turns or
 * places, B opens the valve (and then speeds the liquid up), L/R pick a
 * piece in the tray, START pauses. Menus and panels go through aos_pad_menu.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "cn_draw.h"
#include "cn_game.h"
#include "cn_snd.h"
#include "cn_world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LVGL's, exported by the firmware but in a header the apps do not see: a
 * picture redrawn in the same buffer must leave the image cache */
extern void lv_image_cache_drop(const void *src);

#define PAD         16
#define BTN         88
#define FRAME_MS    33
#define C_GOLD      0xFFD60A
#define C_STAR_OFF  0x3A3A3C

/* A bend of pipe with water in it, and a drop falling from its end. */
static const uint8_t CANOS_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -24, -20, 40, 20, 3, AIC_C_LIT(0xC8D2DC), 255),
    AIC_RECT(AIC_CENTER, -24, -20, 40, 8, 2, AIC_C_LIT(0x3AA0FF), 255),
    AIC_ARC(AIC_CENTER, -4, 6, 52, 20, 8, 270, 360, 270, 360, 0,
            AIC_C_LIT(0xC8D2DC), 255, AIC_C_LIT(0x3AA0FF), 255),
    AIC_RECT(AIC_CENTER, 22, 16, 20, 22, 3, AIC_C_LIT(0xC8D2DC), 255),
    AIC_RECT(AIC_CENTER, 22, 16, 8, 22, 2, AIC_C_LIT(0x3AA0FF), 255),
    AIC_RECT(AIC_CENTER, 22, 36, 10, 12, AIC_CIRCLE, AIC_C_LIT(0x3AA0FF), 255),
    AIC_END
};

enum { PG_HOME = 0, PG_LEVELS, PG_PLAY };
enum { OV_NONE = 0, OV_PAUSE, OV_WIN, OV_LOSE };

/* --------------------------------------------------------------------------
 * Progress, in a file of the data folder
 * -------------------------------------------------------------------------- */

#define PROG_MAGIC   0x534F4E43u     /* "CNOS" */
#define PROG_VERSION 1

typedef struct {
    uint32_t magic;
    uint8_t  version, mode, diff, world, sound, spare[3];
    uint8_t  stars[CN_MODES][CN_DIFFS][CN_WORLDS][CN_LEVELS];
    uint32_t best[CN_MODES][CN_DIFFS][CN_WORLDS][CN_LEVELS];
} prog_t;

static prog_t s_prog;
static bool   s_unlock_all;

static void prog_path(char *out, size_t n)
{
    snprintf(out, n, "%s/canos.dat", aos_hal_path_data());
}

static void prog_load(void)
{
    char path[160];
    prog_path(path, sizeof path);
    memset(&s_prog, 0, sizeof s_prog);
    FILE *f = fopen(path, "rb");
    if (f) {
        prog_t p;
        if (fread(&p, 1, sizeof p, f) == sizeof p && p.magic == PROG_MAGIC && p.version == PROG_VERSION) {
            s_prog = p;
        }
        fclose(f);
    }
    s_prog.magic = PROG_MAGIC;
    s_prog.version = PROG_VERSION;
    if (s_prog.mode >= CN_MODES) s_prog.mode = 0;
    if (s_prog.diff >= CN_DIFFS) s_prog.diff = 0;
    if (s_prog.world >= CN_WORLDS) s_prog.world = 0;
    if (!f) s_prog.sound = 1;
}

static void prog_save(void)
{
    char path[160];
    prog_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        aos_hal_log("canos", "cannot save %s", path);
        return;
    }
    fwrite(&s_prog, 1, sizeof s_prog, f);
    fclose(f);
}

/* done in any difficulty of this mode */
static bool level_done(int mode, int world, int level)
{
    for (int d = 0; d < CN_DIFFS; d++) {
        if (s_prog.stars[mode][d][world][level]) return true;
    }
    return false;
}

static int world_done(int mode, int world)
{
    int n = 0;
    for (int l = 0; l < CN_LEVELS; l++) n += level_done(mode, world, l);
    return n;
}

static bool world_open(int mode, int world)
{
    return s_unlock_all || world == 0 || world_done(mode, world - 1) >= 5;
}

static bool level_open(int mode, int world, int level)
{
    return s_unlock_all || level == 0 || level_done(mode, world, level - 1);
}

static int world_stars(int mode, int diff, int world)
{
    int n = 0;
    for (int l = 0; l < CN_LEVELS; l++) n += s_prog.stars[mode][diff][world][l];
    return n;
}

/* --------------------------------------------------------------------------
 * The app
 * -------------------------------------------------------------------------- */

typedef struct {
    aos_app_t *self;
    lv_obj_t  *root;
    int        W, H;
    bool       land;
    int        page;

    /* menu */
    lv_obj_t  *pg_home;
    lv_obj_t  *mode_btn[CN_MODES], *diff_btn[CN_DIFFS], *diff_desc;
    lv_obj_t  *world_btn[CN_WORLDS], *world_icon[CN_WORLDS], *world_info[CN_WORLDS];
    lv_obj_t  *snd_btn, *snd_lbl;
    lv_image_dsc_t prev_dsc[CN_WORLDS];
    uint16_t  *prev_buf[CN_WORLDS];

    /* a world's levels */
    lv_obj_t  *pg_levels, *lv_title, *lv_sub, *lv_back;
    lv_obj_t  *level_btn[CN_LEVELS], *level_num[CN_LEVELS], *level_star[CN_LEVELS][3];
    int        sel_world;

    /* the game */
    lv_obj_t  *pg_play, *canvas, *cursor;
    lv_obj_t  *hud_title, *hud_sub, *hud_score, *hud_info, *bar, *bar_lbl;
    lv_obj_t  *btn_pause, *btn_restart, *btn_valve, *btn_valve_lbl, *btn_fast;
    lv_obj_t  *tray, *tray_btn[CN_TYPES], *tray_cnt[CN_TYPES], *tray_img[CN_TYPES];
    lv_image_dsc_t tray_dsc[CN_TYPES];
    uint8_t   *tray_buf[CN_TYPES];
    int        tray_px;
    int        sel_type;

    /* panels */
    lv_obj_t  *ov, *ov_card, *ov_title, *ov_text, *ov_stars, *ov_star[3], *ov_btn[3], *ov_btn_lbl[3];
    int        ov_kind;

    /* the board on screen */
    cn_draw_t  draw;
    bool       draw_ok;
    int        vrot;            /* 1: the upright grid turned a quarter (lying down) */
    int        scols, srows;
    int        C;
    float      turn[CN_MAX_ROWS][CN_MAX_COLS];     /* degrees left to turn */
    float      drop[CN_MAX_ROWS][CN_MAX_COLS];     /* > 1 while a piece drops in */
    bool       dirty[CN_MAX_ROWS][CN_MAX_COLS];    /* base to draw again */
    bool       full_redraw;
    bool       spill_on;
    float      t, phase_px;
    uint32_t   last_ms;
    int        last_score, last_secs, last_phase, last_filled;
    lv_timer_t *timer;

    /* gamepad */
    aos_pad_t      pad;
    aos_pad_menu_t pmenu;
    bool       cur_on;
    int        cur_sc, cur_sr;  /* the frame, in screen cells */

    /* measuring, and how the full cells take turns */
    uint32_t   log_ms, frames, draw_us, cells;
    float      cell_us;         /* a cell's drawing, running average */
    uint32_t   rr;              /* frame count for the turns */
    int        stride;
} cn_app_t;

/* What survives the app being built again (the screen turned). */
static cn_game_t s_game;
static struct {
    bool valid, in_game;
    int  page, sel_world, sel_type;
} s_keep;

/* --------------------------------------------------------------------------
 * UI helpers
 * -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, int32_t w, int32_t h, uint32_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, h >= BTN ? 26 : 18, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = button(parent, BTN, BTN, 0x2C2C2E, cb, ud);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *l = text(b, sym, &aos_sym_44, 0xFFFFFF);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *text_btn(lv_obj_t *parent, const char *txt, int32_t w, uint32_t bg, uint32_t fg,
                          lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = button(parent, w, BTN, bg, cb, ud);
    lv_obj_t *l = text(b, txt, aos_font_body, fg);
    lv_obj_center(l);
    return b;
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static const char *mode_name(int m)
{
    return m == CN_MODE_ROTATE ? _("Girar") : _("Completar");
}

static const char *diff_name(int d)
{
    return d == CN_EASY ? _("Fácil") : d == CN_NORMAL ? _("Normal") : _("Difícil");
}

/* the pad menu's outline must be in view in a column that scrolls */
static void pmenu_follow(cn_app_t *a)
{
    lv_obj_t *o = aos_pad_menu_selected(&a->pmenu);
    if (o && a->pmenu.shown) lv_obj_scroll_to_view_recursive(o, LV_ANIM_ON);
}

/* --------------------------------------------------------------------------
 * The menu
 * -------------------------------------------------------------------------- */

static void show_page(cn_app_t *a, int page);
static void show_levels(cn_app_t *a, int world);
static void start_level(cn_app_t *a, int mode, int diff, int world, int level, bool fresh);
static void build_pmenu_home(cn_app_t *a);

static const char *DIFF_DESC[CN_DIFFS] = {
    N_("Sin reloj: abrís la llave cuando el recorrido esté listo."),
    N_("Cuando se acaba el tiempo, la llave se abre sola."),
    N_("Menos tiempo, y el líquido corre más rápido."),
};

static void refresh_home(cn_app_t *a)
{
    for (int m = 0; m < CN_MODES; m++) {
        bool on = m == s_prog.mode;
        lv_obj_set_style_border_width(a->mode_btn[m], on ? 4 : 0, 0);
        lv_obj_set_style_bg_color(a->mode_btn[m], lv_color_hex(on ? 0x24324A : 0x1C1C1E), 0);
    }
    for (int d = 0; d < CN_DIFFS; d++) {
        bool on = d == s_prog.diff;
        lv_obj_set_style_bg_color(a->diff_btn[d], lv_color_hex(on ? 0x0A84FF : 0x2C2C2E), 0);
    }
    lv_label_set_text(a->diff_desc, _(DIFF_DESC[s_prog.diff]));
    for (int w = 0; w < CN_WORLDS; w++) {
        bool open = world_open(s_prog.mode, w);
        lv_obj_set_style_opa(a->world_btn[w], open ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_label_set_text(a->world_icon[w], open ? AOS_SYM_STAR : AOS_SYM_LOCK);
        lv_obj_set_style_text_color(a->world_icon[w], lv_color_hex(open ? C_GOLD : 0x8E8E93), 0);
        if (open) {
            lv_label_set_text_fmt(a->world_info[w], "%d/%d", world_stars(s_prog.mode, s_prog.diff, w),
                                  CN_LEVELS * 3);
        } else {
            lv_label_set_text(a->world_info[w], _("5 niveles del anterior"));
        }
    }
    lv_label_set_text(a->snd_lbl, s_prog.sound ? AOS_SYM_VOLUME_HIGH : AOS_SYM_VOLUME_OFF);
}

static void cb_mode(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_current_target(e);
    for (int m = 0; m < CN_MODES; m++) {
        if (t == a->mode_btn[m]) s_prog.mode = (uint8_t)m;
    }
    cn_snd(CN_SND_CLICK);
    refresh_home(a);
}

static void cb_diff(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_current_target(e);
    for (int d = 0; d < CN_DIFFS; d++) {
        if (t == a->diff_btn[d]) s_prog.diff = (uint8_t)d;
    }
    cn_snd(CN_SND_CLICK);
    refresh_home(a);
}

static void cb_world(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_current_target(e);
    for (int w = 0; w < CN_WORLDS; w++) {
        if (t != a->world_btn[w]) continue;
        if (!world_open(s_prog.mode, w)) {
            cn_snd(CN_SND_DENY);
            aos_ui_toast(_("Completá 5 niveles del mundo anterior"), 1800);
            return;
        }
        cn_snd(CN_SND_CLICK);
        s_prog.world = (uint8_t)w;
        show_levels(a, w);
    }
}

static void cb_sound(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    s_prog.sound = !s_prog.sound;
    cn_snd_enable(s_prog.sound);
    prog_save();
    refresh_home(a);
}

static lv_obj_t *section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *l = text(parent, title, aos_font_small, 0x8E8E93);
    lv_obj_set_style_pad_top(l, 10, 0);
    return l;
}

static void build_home(cn_app_t *a)
{
    lv_obj_t *pg = box(a->root);
    a->pg_home = pg;
    lv_obj_set_size(pg, a->W, a->H);

    /* upright one column that scrolls; lying down the choices on the left
     * and the worlds on the right */
    lv_obj_t *left, *right;
    int32_t lw, rw;
    if (a->land) {
        lw = a->W * 44 / 100;
        rw = a->W - lw;
        left = box(pg);
        lv_obj_set_size(left, lw, a->H);
        right = box(pg);
        lv_obj_set_size(right, rw, a->H);
        lv_obj_set_pos(right, lw, 0);
    } else {
        lw = rw = a->W;
        left = right = box(pg);
        lv_obj_set_size(left, a->W, a->H);
    }
    lv_obj_t *cols[2] = { left, right };
    for (int i = 0; i < (a->land ? 2 : 1); i++) {
        lv_obj_t *c = cols[i];
        lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(c, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, PAD + 8, 0);
        lv_obj_set_style_pad_row(c, 14, 0);
    }
    int32_t inner_l = lw - 2 * (PAD + 8), inner_r = rw - 2 * (PAD + 8);

    /* title and the sound */
    lv_obj_t *head = box(left);
    lv_obj_set_size(head, inner_l, 110);
    lv_obj_t *t = text(head, _("Caños"), aos_font_huge, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, -14);
    lv_obj_t *st = text(head, _("Armá el recorrido antes de que corra el líquido"), aos_font_caption, 0x8E8E93);
    lv_obj_set_width(st, inner_l - BTN - 12);
    lv_label_set_long_mode(st, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(st, LV_ALIGN_BOTTOM_LEFT, 0, 4);
    a->snd_btn = round_btn(head, AOS_SYM_VOLUME_HIGH, cb_sound, a);
    a->snd_lbl = lv_obj_get_child(a->snd_btn, 0);
    lv_obj_align(a->snd_btn, LV_ALIGN_TOP_RIGHT, 0, 0);

    section(left, _("Modo"));
    lv_obj_t *mrow = box(left);
    lv_obj_set_size(mrow, inner_l, 170);
    static const char *MODE_DESC[CN_MODES] = {
        N_("Tocá los caños para girarlos hasta unir el tanque con el desagüe."),
        N_("Faltan caños: poné los que ofrece la bandeja, tal como vienen."),
    };
    int32_t mw = (inner_l - 14) / 2;
    for (int m = 0; m < CN_MODES; m++) {
        lv_obj_t *b = button(mrow, mw, 170, 0x1C1C1E, cb_mode, a);
        lv_obj_set_pos(b, m * (mw + 14), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x0A84FF), 0);
        lv_obj_set_style_pad_all(b, 16, 0);
        lv_obj_t *n = text(b, mode_name(m), aos_font_title, 0xFFFFFF);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *dsc = text(b, _(MODE_DESC[m]), aos_font_tiny, 0xAEAEB2);
        lv_obj_set_width(dsc, mw - 32);
        lv_label_set_long_mode(dsc, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align(dsc, LV_ALIGN_TOP_LEFT, 0, 48);
        a->mode_btn[m] = b;
    }

    section(left, _("Dificultad"));
    lv_obj_t *drow = box(left);
    lv_obj_set_size(drow, inner_l, BTN);
    int32_t dw = (inner_l - 2 * 12) / 3;
    for (int d = 0; d < CN_DIFFS; d++) {
        lv_obj_t *b = text_btn(drow, diff_name(d), dw, 0x2C2C2E, 0xFFFFFF, cb_diff, a);
        lv_obj_set_pos(b, d * (dw + 12), 0);
        a->diff_btn[d] = b;
    }
    a->diff_desc = text(left, "", aos_font_caption, 0xAEAEB2);
    lv_obj_set_width(a->diff_desc, inner_l);
    lv_label_set_long_mode(a->diff_desc, LV_LABEL_LONG_MODE_WRAP);

    section(right, _("Mundos"));
    int32_t ph = 72, pw = a->land ? inner_r * 36 / 100 : 210;
    for (int w = 0; w < CN_WORLDS; w++) {
        const cn_world_t *wd = cn_world(w);
        /* the world's colour as a stripe on the left: a gradient across a
         * dark card bands in 16 bits */
        lv_obj_t *b = button(right, inner_r, 112, 0x1C1C1E, cb_world, a);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(b, 8, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(wd->ui_a), 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        a->world_btn[w] = b;
        if (!a->prev_buf[w]) {
            a->prev_buf[w] = cn_draw_alloc((size_t)pw * ph * 2);
            if (a->prev_buf[w] && !cn_draw_preview(w, pw, ph, 0.6f, a->prev_buf[w])) {
                free(a->prev_buf[w]);
                a->prev_buf[w] = NULL;
            }
        }
        if (a->prev_buf[w]) {
            lv_image_dsc_t *ds = &a->prev_dsc[w];
            memset(ds, 0, sizeof *ds);
            ds->header.magic = LV_IMAGE_HEADER_MAGIC;
            ds->header.cf = LV_COLOR_FORMAT_RGB565;
            ds->header.w = (uint32_t)pw;
            ds->header.h = (uint32_t)ph;
            ds->header.stride = (uint32_t)pw * 2;
            ds->data = (const uint8_t *)a->prev_buf[w];
            ds->data_size = (uint32_t)pw * ph * 2;
            lv_obj_t *img = lv_image_create(b);
            lv_image_set_src(img, ds);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(img, 14, 0);
            lv_obj_set_style_clip_corner(img, true, 0);
            lv_obj_align(img, LV_ALIGN_LEFT_MID, 20, 0);
        }
        int32_t tx = pw + 40;
        lv_obj_t *n = text(b, _(wd->place), aos_font_body, 0xFFFFFF);
        lv_obj_set_width(n, inner_r - tx - 12);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, tx, 14);
        lv_obj_t *f = text(b, _(wd->fluid), aos_font_small, 0xFFFFFF);
        lv_obj_set_style_text_color(f, lv_color_mix(lv_color_hex(wd->c_light), lv_color_white(), 150), 0);
        lv_obj_align(f, LV_ALIGN_LEFT_MID, tx, 4);
        a->world_icon[w] = text(b, AOS_SYM_STAR, &aos_sym_28, C_GOLD);
        lv_obj_align(a->world_icon[w], LV_ALIGN_BOTTOM_LEFT, tx - 2, -10);
        a->world_info[w] = text(b, "", aos_font_caption, 0xFFFFFF);
        lv_obj_align(a->world_info[w], LV_ALIGN_BOTTOM_LEFT, tx + 36, -12);
    }
    refresh_home(a);
}

static void build_pmenu_home(cn_app_t *a)
{
    lv_obj_t *items[16];
    int n = 0;
    items[n++] = a->snd_btn;
    for (int m = 0; m < CN_MODES; m++) items[n++] = a->mode_btn[m];
    for (int d = 0; d < CN_DIFFS; d++) items[n++] = a->diff_btn[d];
    for (int w = 0; w < CN_WORLDS; w++) items[n++] = a->world_btn[w];
    aos_pad_menu_set(&a->pmenu, items, n, 3 + s_prog.diff + 1);
}

/* --------------------------------------------------------------------------
 * A world's levels
 * -------------------------------------------------------------------------- */

static void cb_level(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_current_target(e);
    for (int l = 0; l < CN_LEVELS; l++) {
        if (t != a->level_btn[l]) continue;
        if (!level_open(s_prog.mode, a->sel_world, l)) {
            cn_snd(CN_SND_DENY);
            aos_ui_toast(_("Primero pasá el nivel anterior"), 1600);
            return;
        }
        cn_snd(CN_SND_CLICK);
        start_level(a, s_prog.mode, s_prog.diff, a->sel_world, l, true);
    }
}

static void cb_levels_back(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    cn_snd(CN_SND_CLICK);
    show_page(a, PG_HOME);
}

static void build_levels(cn_app_t *a)
{
    lv_obj_t *pg = box(a->root);
    a->pg_levels = pg;
    lv_obj_set_size(pg, a->W, a->H);

    a->lv_back = round_btn(pg, AOS_SYM_CHEVRON_LEFT, cb_levels_back, a);
    lv_obj_set_pos(a->lv_back, PAD + 8, PAD);
    a->lv_title = text(pg, "", aos_font_title, 0xFFFFFF);
    lv_obj_set_pos(a->lv_title, PAD + 8 + BTN + 20, PAD + 4);
    a->lv_sub = text(pg, "", aos_font_small, 0x8E8E93);
    lv_obj_set_pos(a->lv_sub, PAD + 8 + BTN + 20, PAD + 50);

    int cols = a->land ? 5 : 2, rows = CN_LEVELS / cols;
    int32_t top = PAD + BTN + 30;
    int32_t gw = a->W - 2 * (PAD + 8), gh = a->H - top - PAD - 8;
    int32_t gap = 16;
    int32_t bw = (gw - (cols - 1) * gap) / cols, bh = (gh - (rows - 1) * gap) / rows;
    if (bh > 190) bh = 190;
    for (int l = 0; l < CN_LEVELS; l++) {
        lv_obj_t *b = button(pg, bw, bh, 0x1C1C1E, cb_level, a);
        lv_obj_set_pos(b, PAD + 8 + (l % cols) * (bw + gap), top + (l / cols) * (bh + gap));
        a->level_btn[l] = b;
        a->level_num[l] = text(b, "", aos_font_large, 0xFFFFFF);
        lv_obj_align(a->level_num[l], LV_ALIGN_CENTER, 0, -18);
        for (int k = 0; k < 3; k++) {
            a->level_star[l][k] = text(b, AOS_SYM_STAR, &aos_sym_28, C_STAR_OFF);
            lv_obj_align(a->level_star[l][k], LV_ALIGN_BOTTOM_MID, (k - 1) * 34, -12);
        }
    }
}

static void refresh_levels(cn_app_t *a)
{
    int w = a->sel_world;
    const cn_world_t *wd = cn_world(w);
    lv_label_set_text(a->lv_title, _(wd->place));
    lv_label_set_text_fmt(a->lv_sub, "%s · %s · %s", _(wd->fluid), mode_name(s_prog.mode), diff_name(s_prog.diff));
    for (int l = 0; l < CN_LEVELS; l++) {
        bool open = level_open(s_prog.mode, w, l);
        int st = s_prog.stars[s_prog.mode][s_prog.diff][w][l];
        lv_obj_set_style_bg_color(a->level_btn[l], lv_color_hex(open ? 0x1C1C1E : 0x111113), 0);
        lv_obj_set_style_border_side(a->level_btn[l], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(a->level_btn[l], open ? 6 : 0, 0);
        lv_obj_set_style_border_color(a->level_btn[l], lv_color_hex(wd->ui_a), 0);
        if (open) {
            lv_obj_set_style_text_font(a->level_num[l], aos_font_large, 0);
            lv_label_set_text_fmt(a->level_num[l], "%d", l + 1);
        } else {
            lv_obj_set_style_text_font(a->level_num[l], &aos_sym_44, 0);
            lv_label_set_text(a->level_num[l], AOS_SYM_LOCK);
        }
        for (int k = 0; k < 3; k++) {
            set_hidden(a->level_star[l][k], !open);
            lv_obj_set_style_text_color(a->level_star[l][k], lv_color_hex(k < st ? C_GOLD : C_STAR_OFF), 0);
        }
    }
}

static void show_levels(cn_app_t *a, int world)
{
    a->sel_world = world;
    refresh_levels(a);
    show_page(a, PG_LEVELS);
}

/* --------------------------------------------------------------------------
 * The board on screen: the upright grid, turned when lying down
 * -------------------------------------------------------------------------- */

static void to_screen_cell(const cn_app_t *a, int c, int r, int *sc, int *sr)
{
    if (a->vrot) {
        *sc = s_game.lv.rows - 1 - r;
        *sr = c;
    } else {
        *sc = c;
        *sr = r;
    }
}

static void to_grid_cell(const cn_app_t *a, int sc, int sr, int *c, int *r)
{
    if (a->vrot) {
        *c = sr;
        *r = s_game.lv.rows - 1 - sc;
    } else {
        *c = sc;
        *r = sr;
    }
}

static const uint8_t SHAPE_OF[CN_KINDS] = {
    [CN_EMPTY] = CN_SH_NONE, [CN_STRAIGHT] = CN_SH_STRAIGHT, [CN_BEND] = CN_SH_BEND,
    [CN_CROSS] = CN_SH_CROSS, [CN_SOURCE] = CN_SH_SOURCE, [CN_DRAIN] = CN_SH_DRAIN,
    [CN_BLOCK] = CN_SH_BLOCK,
};

/* the screen turn of a piece */
static uint8_t screen_rot(const cn_app_t *a, uint8_t kind, uint8_t rot)
{
    switch (kind) {
    case CN_STRAIGHT: return (uint8_t)((rot + a->vrot) & 1);
    case CN_BEND:
    case CN_SOURCE:
    case CN_DRAIN:    return (uint8_t)((rot + a->vrot) & 3);
    default:          return 0;
    }
}

static void tile_of(const cn_app_t *a, int c, int r, cn_tile_t *t)
{
    const cn_game_t *g = &s_game;
    const cn_cell_t *cell = &g->lv.cell[r][c];
    memset(t, 0, sizeof *t);
    t->shape = SHAPE_OF[cell->kind < CN_KINDS ? cell->kind : 0];
    t->srot = screen_rot(a, cell->kind, cell->rot);
    t->fixed = (cell->flags & CN_F_FIXED) &&
               (cell->kind == CN_STRAIGHT || cell->kind == CN_BEND || cell->kind == CN_CROSS);
    t->turn = a->turn[r][c];
    t->scale = a->drop[r][c] > 1.0f ? a->drop[r][c] : 1.0f;
    for (int k = 0; k < 2; k++) {
        int si = g->segi[r][c][k];
        if (si < 0) continue;
        const cn_seg_t *s = &g->seg[si];
        int sk = cell->kind == CN_CROSS ? (k ^ a->vrot) : 0;
        t->ch[sk].on = 1;
        t->ch[sk].start_px = s->start * (float)a->C;
        if (cell->kind == CN_STRAIGHT || cell->kind == CN_BEND || cell->kind == CN_CROSS) {
            int entry = cell->kind == CN_CROSS ? sk : t->srot;   /* the channel's own entry side */
            int in = (s->in + a->vrot) & 3;
            t->ch[sk].rev = (uint8_t)(in != entry);
        }
    }
    if (cell->kind == CN_SOURCE) {
        float v = g->phase == CN_PH_EDIT ? 0 : g->phase == CN_PH_OPENING ? g->valve : 1.0f;
        t->valve = v * 1.5f;
        t->flowing = g->phase >= CN_PH_FLOW && g->phase <= CN_PH_SPILL;
    }
    if (cell->kind == CN_DRAIN) {
        t->drain = g->phase == CN_PH_DRAIN ? g->end_t / 1.2f : g->phase == CN_PH_WON ? 1.0f : 0;
    }
}

static void invalidate_cell(cn_app_t *a, int sc, int sr)
{
    lv_area_t cc;
    lv_obj_get_coords(a->canvas, &cc);
    lv_area_t ar = { cc.x1 + sc * a->C, cc.y1 + sr * a->C, cc.x1 + (sc + 1) * a->C - 1, cc.y1 + (sr + 1) * a->C - 1 };
    lv_obj_invalidate_area(a->canvas, &ar);
}

static bool flowing(void)
{
    return s_game.phase >= CN_PH_FLOW && s_game.phase <= CN_PH_SPILL;
}

/* The liquid's front is in this cell, or has just left it. */
static bool at_front(const cn_game_t *g, int c, int r)
{
    for (int k = 0; k < 2; k++) {
        int si = g->segi[r][c][k];
        if (si >= 0 && g->seg[si].start + g->seg[si].len > g->front - 0.3f) return true;
    }
    return false;
}

/* Draws what moved this frame (or everything). The cells the front is in,
 * the ones turning and the spill are drawn every frame; the full ones,
 * whose streaks only drift, take turns when there are more than the frame's
 * budget (14 ms at the measured cost of a cell): on the board a long run is
 * thirty-odd cells of about a millisecond each. */
static void render(cn_app_t *a, bool all)
{
    if (!a->draw_ok) return;
    const cn_game_t *g = &s_game;
    uint32_t t0 = (uint32_t)aos_hal_uptime_us();
    a->rr++;
    int stride = a->stride < 1 ? 1 : a->stride;
    float C = (float)a->C;
    float front = g->front * C;
    if (g->phase >= CN_PH_DRAIN) front += cn_world(g->lv.world)->curv * a->draw.ri_px + C * 0.3f;
    cn_draw_begin(&a->draw, a->t, a->phase_px, front, g->speed * C);

    bool live = flowing() && a->ov_kind == OV_NONE;
    bool spill_live = a->spill_on && (a->ov_kind == OV_NONE || a->ov_kind == OV_LOSE);
    int sx0 = 0, sy0 = 0, sx1 = -1, sy1 = -1;
    if (spill_live) {
        if (!cn_draw_spill_rect(&a->draw, &sx0, &sy0, &sx1, &sy1)) sx1 = -1;
    }
    int n = 0;
    for (int r = 0; r < g->lv.rows; r++) {
        for (int c = 0; c < g->lv.cols; c++) {
            const cn_cell_t *cell = &g->lv.cell[r][c];
            int sc, sr;
            to_screen_cell(a, c, r, &sc, &sr);
            bool need = all || a->dirty[r][c] || a->turn[r][c] != 0 || a->drop[r][c] > 1.0f;
            if (!need && live && cn_game_wet(g, c, r)) {
                need = at_front(g, c, r) || g->phase != CN_PH_FLOW ||
                       ((uint32_t)(r * 7 + c * 3) + a->rr) % (uint32_t)stride == 0;
            }
            if (!need && cell->kind == CN_SOURCE && a->ov_kind == OV_NONE) need = true;
            if (!need && cell->kind == CN_DRAIN && g->phase == CN_PH_DRAIN) need = true;
            if (!need && sx1 >= sx0) {
                int x0 = sc * a->C, y0 = sr * a->C;
                need = x0 <= sx1 && x0 + a->C > sx0 && y0 <= sy1 && y0 + a->C > sy0;
            }
            if (!need) continue;
            cn_tile_t t;
            tile_of(a, c, r, &t);
            if (all || a->dirty[r][c]) {
                cn_tile_t still = t;
                still.turn = 0;
                still.scale = 1;
                cn_draw_base(&a->draw, sc, sr, &still);
                a->dirty[r][c] = false;
            }
            cn_draw_tile(&a->draw, sc, sr, &t);
            if (!all) invalidate_cell(a, sc, sr);
            n++;
        }
    }
    if (all) lv_obj_invalidate(a->canvas);
    uint32_t us = (uint32_t)aos_hal_uptime_us() - t0;
    a->draw_us += us;
    a->cells += (uint32_t)n;
    if (n >= 3 && !all) {
        float per = (float)us / (float)n;
        a->cell_us = a->cell_us > 0 ? a->cell_us * 0.9f + per * 0.1f : per;
        int wet = 0;
        for (int r = 0; r < g->lv.rows; r++) {
            for (int c = 0; c < g->lv.cols; c++) wet += cn_game_wet(g, c, r);
        }
        float fit = 14000.0f / a->cell_us;     /* cells a frame can afford */
        int st = fit >= (float)wet ? 1 : (int)((float)wet / (fit > 2.0f ? fit - 1.0f : 1.0f)) + 1;
        a->stride = st > 6 ? 6 : st;
    }
}

/* --------------------------------------------------------------------------
 * The game's screen
 * -------------------------------------------------------------------------- */

static void show_overlay(cn_app_t *a, int kind);
static void update_hud(cn_app_t *a, bool force);

static void cb_pause(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    if (s_game.phase >= CN_PH_WON) return;
    cn_snd(CN_SND_CLICK);
    show_overlay(a, OV_PAUSE);
}

static void restart(cn_app_t *a)
{
    start_level(a, s_game.lv.mode, s_game.lv.diff, s_game.lv.world, s_game.lv.level, true);
}

static void cb_restart(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    cn_snd(CN_SND_CLICK);
    restart(a);
}

static void open_valve(cn_app_t *a)
{
    if (cn_game_open(&s_game)) {
        cn_snd(CN_SND_VALVE);
        update_hud(a, true);
    }
}

static void cb_valve(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    if (s_game.phase == CN_PH_EDIT) {
        open_valve(a);
    } else if (s_game.phase == CN_PH_FLOW) {
        cn_game_set_fast(&s_game, !s_game.fast);
        cn_snd(CN_SND_CLICK);
        update_hud(a, true);
    }
}

static void refresh_tray(cn_app_t *a)
{
    if (s_game.lv.mode != CN_MODE_FILL) return;
    for (int i = 0; i < CN_TYPES; i++) {
        int n = s_game.lv.tray[i];
        lv_label_set_text_fmt(a->tray_cnt[i], "%d", n);
        bool sel = i == a->sel_type;
        lv_obj_set_style_border_width(a->tray_btn[i], sel ? 4 : 0, 0);
        lv_obj_set_style_opa(a->tray_img[i], n ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_bg_color(a->tray_btn[i], lv_color_hex(sel ? 0x24324A : 0x1C1C1E), 0);
    }
}

static void cb_tray(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_current_target(e);
    for (int i = 0; i < CN_TYPES; i++) {
        if (t == a->tray_btn[i]) {
            a->sel_type = i;
            cn_snd(CN_SND_CLICK);
        }
    }
    refresh_tray(a);
}

/* A tap (or the pad's A) on a cell of the grid. */
static void act_on(cn_app_t *a, int c, int r)
{
    cn_game_t *g = &s_game;
    if (c < 0 || r < 0 || c >= g->lv.cols || r >= g->lv.rows) return;
    if (g->phase >= CN_PH_DRAIN) return;
    const cn_cell_t *cell = &g->lv.cell[r][c];
    if (cell->kind == CN_SOURCE) {
        if (g->phase == CN_PH_EDIT) open_valve(a);
        return;
    }
    if (g->lv.mode == CN_MODE_ROTATE) {
        if (cn_game_rotate(g, c, r)) {
            a->turn[r][c] = -90.0f;
            a->dirty[r][c] = true;
            cn_snd(CN_SND_TURN);
        } else if (cell->kind != CN_CROSS) {
            cn_snd(CN_SND_DENY);
        }
    } else {
        if (cell->flags & CN_F_PLACED) {
            int t = cn_game_take(g, c, r);
            if (t >= 0) {
                a->sel_type = t;
                a->dirty[r][c] = true;
                cn_snd(CN_SND_TAKE);
                refresh_tray(a);
            } else {
                cn_snd(CN_SND_DENY);
            }
        } else if (cell->kind == CN_EMPTY && cn_game_editable(g, c, r)) {
            if (a->sel_type < 0 || !g->lv.tray[a->sel_type]) {
                cn_snd(CN_SND_DENY);
                aos_ui_toast(_("Elegí un caño de la bandeja"), 1400);
                return;
            }
            if (cn_game_place(g, c, r, a->sel_type)) {
                a->drop[r][c] = 1.3f;
                a->dirty[r][c] = true;
                cn_snd(CN_SND_PLACE);
                refresh_tray(a);
            }
        } else {
            cn_snd(CN_SND_DENY);
        }
    }
    update_hud(a, true);
}

static void cursor_place(cn_app_t *a)
{
    if (!a->cursor) return;
    set_hidden(a->cursor, !a->cur_on);
    lv_obj_set_pos(a->cursor, lv_obj_get_x(a->canvas) + a->cur_sc * a->C - 4,
                   lv_obj_get_y(a->canvas) + a->cur_sr * a->C - 4);
}

static void cb_board(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    if (a->ov_kind != OV_NONE) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t cc;
    lv_obj_get_coords(a->canvas, &cc);
    if (p.x < cc.x1 || p.y < cc.y1) return;
    int sc = (p.x - cc.x1) / a->C, sr = (p.y - cc.y1) / a->C;
    if (sc >= a->scols || sr >= a->srows) return;
    if (a->cur_on) {
        a->cur_on = false;
        cursor_place(a);
    }
    int c, r;
    to_grid_cell(a, sc, sr, &c, &r);
    act_on(a, c, r);
}

static void update_hud(cn_app_t *a, bool force)
{
    const cn_game_t *g = &s_game;
    if (force || g->score != a->last_score) {
        lv_label_set_text_fmt(a->hud_score, "%d", g->score);
        a->last_score = g->score;
    }
    int secs = (int)(g->countdown + 0.999f);
    int ph = g->phase | (g->fast << 8);
    if (!force && secs == a->last_secs && ph == a->last_phase && g->filled == a->last_filled) return;
    a->last_secs = secs;
    a->last_filled = g->filled;
    a->last_phase = ph;

    bool timed = g->lv.diff != CN_EASY;
    if (g->phase == CN_PH_EDIT) {
        if (timed) {
            set_hidden(a->bar, false);
            lv_bar_set_range(a->bar, 0, (int32_t)(g->lv.countdown * 10.0f));
            lv_bar_set_value(a->bar, (int32_t)(g->countdown * 10.0f), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(a->bar, lv_color_hex(secs <= 5 ? 0xFF453A : secs <= 12 ? 0xFF9F0A : 0x30D158),
                                      LV_PART_INDICATOR);
            lv_label_set_text_fmt(a->bar_lbl, "%d:%02d", secs / 60, secs % 60);
            lv_label_set_text(a->hud_info, "");
        } else {
            set_hidden(a->bar, true);
            lv_label_set_text(a->bar_lbl, "");
            lv_label_set_text(a->hud_info, g->lv.mode == CN_MODE_ROTATE
                              ? _("Girá los caños y abrí la llave") : _("Poné los caños y abrí la llave"));
        }
        lv_label_set_text(a->btn_valve_lbl, _("Abrir llave"));
        lv_obj_set_style_bg_color(a->btn_valve, lv_color_hex(0x0A84FF), 0);
        lv_obj_remove_state(a->btn_valve, LV_STATE_DISABLED);
    } else {
        set_hidden(a->bar, true);
        lv_label_set_text(a->bar_lbl, "");
        if (g->phase == CN_PH_OPENING) {
            lv_label_set_text(a->hud_info, _("Abriendo la llave..."));
        } else if (g->phase == CN_PH_FLOW) {
            lv_label_set_text_fmt(a->hud_info, _("Corre el líquido: %d caños"), g->filled);
        } else if (g->phase == CN_PH_DRAIN || g->phase == CN_PH_WON) {
            lv_label_set_text(a->hud_info, _("¡Llegó al desagüe!"));
        } else {
            lv_label_set_text(a->hud_info, _("¡Fuga!"));
        }
        bool can_fast = g->phase == CN_PH_FLOW;
        bool fast = can_fast && g->fast;
        lv_label_set_text(a->btn_valve_lbl, fast ? _("Más despacio") : _("Acelerar"));
        lv_obj_set_style_bg_color(a->btn_valve, lv_color_hex(fast ? 0xFF9F0A : 0x2C2C2E), 0);
        if (can_fast) lv_obj_remove_state(a->btn_valve, LV_STATE_DISABLED);
        else lv_obj_add_state(a->btn_valve, LV_STATE_DISABLED);
    }
}

/* The cell size that fits, and the canvas and the cursor for it. */
static bool setup_board(cn_app_t *a)
{
    const cn_game_t *g = &s_game;
    a->vrot = a->land ? 1 : 0;
    a->scols = a->vrot ? g->lv.rows : g->lv.cols;
    a->srows = a->vrot ? g->lv.cols : g->lv.rows;

    int32_t ax, ay, aw, ah;
    bool fill = g->lv.mode == CN_MODE_FILL;
    if (a->land) {
        int32_t side = 400;
        ax = PAD;
        ay = PAD;
        aw = a->W - side - 2 * PAD;
        ah = a->H - 2 * PAD;
    } else {
        int32_t top = 8 + BTN + 8 + 40;
        int32_t bottom = 8 + BTN + 12 + (fill ? a->tray_px + 16 : 0);
        ax = PAD / 2;
        ay = top;
        aw = a->W - PAD;
        ah = a->H - top - bottom;
    }
    int C = (int)(aw / a->scols);
    int Ch = (int)(ah / a->srows);
    if (Ch < C) C = Ch;
    if (C > 140) C = 140;
    C &= ~1;
    if (a->draw_ok) {
        cn_draw_free(&a->draw);
        a->draw_ok = false;
    }
    a->C = C;
    if (!cn_draw_init(&a->draw, g->lv.world, a->scols, a->srows, C)) {
        aos_hal_log("canos", "no memory for a %dx%d board of %d px", a->scols, a->srows, C);
        return false;
    }
    a->draw_ok = true;
    lv_canvas_set_buffer(a->canvas, a->draw.fb, a->draw.W, a->draw.H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(a->canvas, a->draw.W, a->draw.H);
    lv_obj_set_pos(a->canvas, ax + (aw - a->draw.W) / 2, ay + (ah - a->draw.H) / 2);
    lv_obj_set_size(a->cursor, C + 8, C + 8);
    lv_obj_update_layout(a->pg_play);
    return true;
}

static void make_tray_icons(cn_app_t *a)
{
    int sz = a->tray_px - 14;
    for (int i = 0; i < CN_TYPES; i++) {
        uint8_t kind, rot;
        cn_type_piece(i, &kind, &rot);
        if (!a->tray_buf[i]) a->tray_buf[i] = cn_draw_alloc((size_t)sz * sz * 3);
        if (!a->tray_buf[i]) continue;
        cn_draw_icon(s_game.lv.world, SHAPE_OF[kind], screen_rot(a, kind, rot), sz, a->tray_buf[i]);
        lv_image_dsc_t *ds = &a->tray_dsc[i];
        memset(ds, 0, sizeof *ds);
        ds->header.magic = LV_IMAGE_HEADER_MAGIC;
        ds->header.cf = LV_COLOR_FORMAT_RGB565A8;
        ds->header.w = (uint32_t)sz;
        ds->header.h = (uint32_t)sz;
        ds->header.stride = (uint32_t)sz * 2;
        ds->data = a->tray_buf[i];
        ds->data_size = (uint32_t)sz * sz * 3;
        lv_image_cache_drop(ds);
        lv_image_set_src(a->tray_img[i], ds);
    }
}

static void build_play(cn_app_t *a)
{
    lv_obj_t *pg = box(a->root);
    a->pg_play = pg;
    lv_obj_set_size(pg, a->W, a->H);

    a->canvas = lv_canvas_create(pg);
    lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->canvas, cb_board, LV_EVENT_PRESSED, a);

    a->cursor = box(pg);
    lv_obj_set_style_border_color(a->cursor, lv_color_white(), 0);
    lv_obj_set_style_border_width(a->cursor, 5, 0);
    lv_obj_set_style_radius(a->cursor, 14, 0);
    lv_obj_add_flag(a->cursor, LV_OBJ_FLAG_HIDDEN);

    /* the HUD: upright across the top, lying down a column on the right */
    int32_t hx = PAD, hy = 8, hw = a->W - 2 * PAD;
    if (a->land) {
        hx = a->W - 400 + PAD;
        hw = 400 - 2 * PAD;
    }
    a->btn_pause = round_btn(pg, AOS_SYM_PAUSE, cb_pause, a);
    lv_obj_set_pos(a->btn_pause, hx, hy);
    a->hud_title = text(pg, "", aos_font_body, 0xFFFFFF);
    lv_obj_set_pos(a->hud_title, hx + BTN + 16, hy + 6);
    lv_obj_set_width(a->hud_title, hw - BTN - 16 - (a->land ? 0 : 150));
    lv_label_set_long_mode(a->hud_title, LV_LABEL_LONG_MODE_DOTS);
    a->hud_sub = text(pg, "", aos_font_caption, 0x8E8E93);
    lv_obj_set_pos(a->hud_sub, hx + BTN + 16, hy + 48);
    a->hud_score = text(pg, "0", aos_font_title, 0xFFD60A);
    if (a->land) lv_obj_set_pos(a->hud_score, hx, hy + BTN + 14);
    else lv_obj_align(a->hud_score, LV_ALIGN_TOP_RIGHT, -PAD, hy + 22);

    int32_t iy = a->land ? hy + BTN + 70 : hy + BTN + 6;
    a->bar = lv_bar_create(pg);
    lv_obj_set_size(a->bar, hw - 150, 16);
    lv_obj_set_pos(a->bar, hx, iy + 12);
    lv_obj_set_style_bg_color(a->bar, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(a->bar, 8, 0);
    lv_obj_set_style_radius(a->bar, 8, LV_PART_INDICATOR);
    a->bar_lbl = text(pg, "", aos_font_body, 0xFFFFFF);
    lv_obj_set_style_text_font(a->bar_lbl, aos_font_body, 0);
    lv_obj_set_pos(a->bar_lbl, hx + hw - 136, iy);
    a->hud_info = text(pg, "", aos_font_caption, 0xAEAEB2);
    lv_obj_set_pos(a->hud_info, hx, iy + 6);
    lv_obj_set_width(a->hud_info, hw);
    lv_label_set_long_mode(a->hud_info, LV_LABEL_LONG_MODE_WRAP);

    /* the buttons: restart, the valve (then fast-forward), and upright the
     * tray above them */
    int32_t by = a->land ? iy + 100 : a->H - BTN - 12;
    a->btn_restart = round_btn(pg, AOS_SYM_RESTART, cb_restart, a);
    lv_obj_set_pos(a->btn_restart, hx, by);
    a->btn_valve = button(pg, hw - BTN - 16, BTN, 0x0A84FF, cb_valve, a);
    lv_obj_set_pos(a->btn_valve, hx + BTN + 16, by);
    lv_obj_set_style_bg_opa(a->btn_valve, LV_OPA_40, LV_STATE_DISABLED);
    a->btn_valve_lbl = text(a->btn_valve, "", aos_font_body, 0xFFFFFF);
    lv_obj_center(a->btn_valve_lbl);
    a->btn_fast = NULL;

    int cols = a->land ? 4 : CN_TYPES;
    int32_t gap = 8;
    a->tray_px = (hw - (cols - 1) * gap) / cols;
    if (a->tray_px > 104) a->tray_px = 104;
    a->tray = box(pg);
    int rows = (CN_TYPES + cols - 1) / cols;
    int32_t tw = cols * a->tray_px + (cols - 1) * gap, th = rows * a->tray_px + (rows - 1) * gap;
    lv_obj_set_size(a->tray, tw + 24, th + 24);
    lv_obj_set_style_pad_all(a->tray, 12, 0);
    if (a->land) lv_obj_set_pos(a->tray, hx - 12, by + BTN + 24 - 12);
    else lv_obj_set_pos(a->tray, (a->W - tw) / 2 - 12, by - th - 16 - 12);
    for (int i = 0; i < CN_TYPES; i++) {
        lv_obj_t *b = button(a->tray, a->tray_px, a->tray_px, 0x1C1C1E, cb_tray, a);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x0A84FF), 0);
        lv_obj_set_pos(b, (i % cols) * (a->tray_px + gap), (i / cols) * (a->tray_px + gap));
        a->tray_btn[i] = b;
        a->tray_img[i] = lv_image_create(b);
        lv_obj_remove_flag(a->tray_img[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(a->tray_img[i]);
        lv_obj_t *cnt = text(b, "0", aos_font_caption, 0xFFFFFF);
        lv_obj_set_style_bg_color(cnt, lv_color_hex(0x0A84FF), 0);
        lv_obj_set_style_bg_opa(cnt, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cnt, 12, 0);
        lv_obj_set_style_pad_hor(cnt, 7, 0);
        lv_obj_align(cnt, LV_ALIGN_TOP_RIGHT, -2, 2);
        a->tray_cnt[i] = cnt;
    }
}

/* --------------------------------------------------------------------------
 * Panels: pause, won, leaked
 * -------------------------------------------------------------------------- */

static void cb_ov(lv_event_t *e)
{
    cn_app_t *a = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_current_target(e);
    int i = 0;
    while (i < 3 && t != a->ov_btn[i]) i++;
    cn_snd(CN_SND_CLICK);
    const cn_level_t *lv = &s_game.lv;
    switch (a->ov_kind) {
    case OV_PAUSE:
        if (i == 0) show_overlay(a, OV_NONE);
        else if (i == 1) restart(a);
        else show_levels(a, lv->world);
        break;
    case OV_WIN:
        if (i == 0) {
            if (lv->level + 1 < CN_LEVELS) start_level(a, lv->mode, lv->diff, lv->world, lv->level + 1, true);
            else if (lv->world + 1 < CN_WORLDS && world_open(lv->mode, lv->world + 1)) start_level(a, lv->mode, lv->diff, lv->world + 1, 0, true);
            else show_levels(a, lv->world);
        } else if (i == 1) {
            restart(a);
        } else {
            show_levels(a, lv->world);
        }
        break;
    case OV_LOSE:
        if (i == 0) restart(a);
        else show_levels(a, lv->world);
        break;
    default:
        break;
    }
}

static void build_overlay(cn_app_t *a)
{
    a->ov = box(a->root);
    lv_obj_set_size(a->ov, a->W, a->H);
    lv_obj_add_flag(a->ov, LV_OBJ_FLAG_CLICKABLE);   /* eats the taps under it */
    lv_obj_set_style_bg_color(a->ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a->ov, LV_OPA_40, 0);
    a->ov_card = box(a->ov);
    int32_t cw = a->land ? 560 : a->W - 2 * (PAD + 24);
    lv_obj_set_width(a->ov_card, cw);
    lv_obj_set_height(a->ov_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(a->ov_card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(a->ov_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a->ov_card, 32, 0);
    lv_obj_set_style_pad_all(a->ov_card, 28, 0);
    lv_obj_set_style_pad_row(a->ov_card, 16, 0);
    lv_obj_set_flex_flow(a->ov_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->ov_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(a->ov_card);
    a->ov_title = text(a->ov_card, "", aos_font_title, 0xFFFFFF);
    a->ov_stars = box(a->ov_card);
    lv_obj_set_size(a->ov_stars, 3 * 84, 84);
    for (int i = 0; i < 3; i++) {
        a->ov_star[i] = text(a->ov_stars, AOS_SYM_STAR, &aos_sym_72, C_STAR_OFF);
        lv_obj_set_pos(a->ov_star[i], i * 84 + 4, 2);
    }
    a->ov_text = text(a->ov_card, "", aos_font_small, 0xAEAEB2);
    lv_obj_set_width(a->ov_text, cw - 56);
    lv_label_set_long_mode(a->ov_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(a->ov_text, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < 3; i++) {
        a->ov_btn[i] = text_btn(a->ov_card, "", cw - 56, i == 0 ? 0x0A84FF : 0x2C2C2E, 0xFFFFFF, cb_ov, a);
        a->ov_btn_lbl[i] = lv_obj_get_child(a->ov_btn[i], 0);
    }
    set_hidden(a->ov, true);
}

static void show_overlay(cn_app_t *a, int kind)
{
    a->ov_kind = kind;
    if (kind == OV_NONE) {
        set_hidden(a->ov, true);
        aos_pad_menu_clear(&a->pmenu);
        a->last_ms = lv_tick_get();
        cursor_place(a);
        return;
    }
    const cn_game_t *g = &s_game;
    const cn_world_t *wd = cn_world(g->lv.world);
    const char *b[3] = { NULL, NULL, NULL };
    set_hidden(a->ov_stars, kind != OV_WIN);
    switch (kind) {
    case OV_PAUSE:
        lv_label_set_text(a->ov_title, _("Pausa"));
        lv_label_set_text_fmt(a->ov_text, "%s · %s %d\n%s · %s", _(wd->place), _("nivel"), g->lv.level + 1,
                              mode_name(g->lv.mode), diff_name(g->lv.diff));
        b[0] = _("Seguir");
        b[1] = _("Otra vez");
        b[2] = _("Niveles");
        break;
    case OV_WIN: {
        lv_label_set_text(a->ov_title, _("¡Llegó al desagüe!"));
        for (int i = 0; i < 3; i++) {
            lv_obj_set_style_text_color(a->ov_star[i], lv_color_hex(i < g->stars ? C_GOLD : C_STAR_OFF), 0);
        }
        char buf[200];
        if (g->lv.mode == CN_MODE_ROTATE) {
            snprintf(buf, sizeof buf, "%s: %d\n%s: %d · %s: %d (%d)", _("Puntos"), g->score, _("Caños llenos"),
                     g->filled, _("Giros"), g->moves, g->lv.par_moves);
        } else {
            snprintf(buf, sizeof buf, "%s: %d\n%s: %d · %s: %d (%d)", _("Puntos"), g->score, _("Caños llenos"),
                     g->filled, _("Piezas"), g->moves, g->lv.gaps);
        }
        lv_label_set_text(a->ov_text, buf);
        bool more = g->lv.level + 1 < CN_LEVELS ||
                    (g->lv.world + 1 < CN_WORLDS && world_open(g->lv.mode, g->lv.world + 1));
        b[0] = more ? _("Siguiente") : _("Niveles");
        b[1] = _("Otra vez");
        b[2] = more ? _("Niveles") : NULL;
        break;
    }
    default:
        lv_label_set_text(a->ov_title, _("¡Fuga!"));
        lv_label_set_text(a->ov_text, _(wd->leak));
        b[0] = _("Otra vez");
        b[1] = _("Niveles");
        break;
    }
    lv_obj_t *items[3];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        set_hidden(a->ov_btn[i], !b[i]);
        if (b[i]) {
            lv_label_set_text(a->ov_btn_lbl[i], b[i]);
            items[n++] = a->ov_btn[i];
        }
    }
    set_hidden(a->ov, false);
    lv_obj_move_foreground(a->ov);
    aos_pad_menu_set(&a->pmenu, items, n, 0);
    aos_pad_reset(&a->pad, lv_tick_get());
}

/* --------------------------------------------------------------------------
 * Pages and levels
 * -------------------------------------------------------------------------- */

static void show_page(cn_app_t *a, int page)
{
    if (a->page == PG_PLAY && page != PG_PLAY) {
        cn_snd_flow(0, 0);
        if (a->draw_ok) {
            cn_draw_free(&a->draw);
            a->draw_ok = false;
        }
        s_keep.in_game = false;
    }
    a->page = page;
    set_hidden(a->pg_home, page != PG_HOME);
    set_hidden(a->pg_levels, page != PG_LEVELS);
    set_hidden(a->pg_play, page != PG_PLAY);
    if (page != PG_PLAY) set_hidden(a->ov, true);
    a->ov_kind = page == PG_PLAY ? a->ov_kind : OV_NONE;
    if (page == PG_HOME) {
        refresh_home(a);
        build_pmenu_home(a);
        prog_save();
    } else if (page == PG_LEVELS) {
        refresh_levels(a);
        lv_obj_t *items[CN_LEVELS + 1];
        int n = 0, first = 1;
        items[n++] = a->lv_back;
        for (int l = 0; l < CN_LEVELS; l++) {
            items[n++] = a->level_btn[l];
            if (level_open(s_prog.mode, a->sel_world, l)) first = l + 1;
        }
        aos_pad_menu_set(&a->pmenu, items, n, first);
    } else {
        aos_pad_menu_clear(&a->pmenu);
    }
    aos_pad_reset(&a->pad, lv_tick_get());
}

static void start_level(cn_app_t *a, int mode, int diff, int world, int level, bool fresh)
{
    if (fresh) cn_game_start(&s_game, mode, diff, world, level);
    a->page = PG_PLAY;
    set_hidden(a->pg_home, true);
    set_hidden(a->pg_levels, true);
    set_hidden(a->pg_play, false);
    memset(a->turn, 0, sizeof a->turn);
    memset(a->drop, 0, sizeof a->drop);
    memset(a->dirty, 0, sizeof a->dirty);
    a->spill_on = false;
    a->phase_px = 0;
    a->t = 0;
    a->last_ms = lv_tick_get();
    a->last_score = -1;
    a->last_secs = -1;
    a->last_phase = -1;
    s_keep.in_game = true;

    const cn_level_t *lv = &s_game.lv;
    bool fill = lv->mode == CN_MODE_FILL;
    set_hidden(a->tray, !fill);
    a->sel_type = -1;
    if (fill) {
        for (int i = 0; i < CN_TYPES && a->sel_type < 0; i++) {
            if (lv->tray[i]) a->sel_type = i;
        }
    }
    if (!setup_board(a)) {
        aos_ui_toast(_("No hay memoria para el tablero"), 2000);
        show_page(a, PG_LEVELS);
        return;
    }
    if (fill) make_tray_icons(a);
    refresh_tray(a);
    const cn_world_t *wd = cn_world(lv->world);
    lv_label_set_text_fmt(a->hud_title, "%s · %d", _(wd->place), lv->level + 1);
    lv_label_set_text_fmt(a->hud_sub, "%s · %s · %s", _(wd->fluid), mode_name(lv->mode), diff_name(lv->diff));
    lv_obj_set_style_text_color(a->hud_sub, lv_color_mix(lv_color_hex(wd->c_light), lv_color_white(), 150), 0);

    /* where the pad's frame starts: on the source */
    to_screen_cell(a, lv->src_c, lv->src_r, &a->cur_sc, &a->cur_sr);
    cursor_place(a);

    /* a game brought back after a turn: the leak and the drain where they were */
    if (!fresh && s_game.phase == CN_PH_SPILL) s_game.phase = CN_PH_LOST;
    if (!fresh && s_game.phase == CN_PH_DRAIN) {
        s_game.end_t = 2.0f;
        cn_game_step(&s_game, 0.01f);
    }
    render(a, true);
    update_hud(a, true);
    show_overlay(a, OV_NONE);
    if (!fresh) {
        if (s_game.phase == CN_PH_WON) show_overlay(a, OV_WIN);
        else if (s_game.phase == CN_PH_LOST) show_overlay(a, OV_LOSE);
        else show_overlay(a, OV_PAUSE);
    }
    aos_hal_log("canos", "level %d-%d mode %d diff %d: %dx%d, cell %d px, path %d, board %d KB",
                lv->world + 1, lv->level + 1, lv->mode, lv->diff, lv->cols, lv->rows, a->C, lv->path_len,
                (a->draw.W * a->draw.H * 7) / 1024);
}

static void level_won(cn_app_t *a)
{
    const cn_level_t *lv = &s_game.lv;
    uint8_t *st = &s_prog.stars[lv->mode][lv->diff][lv->world][lv->level];
    if (s_game.stars > *st) *st = (uint8_t)s_game.stars;
    uint32_t *best = &s_prog.best[lv->mode][lv->diff][lv->world][lv->level];
    if ((uint32_t)s_game.score > *best) *best = (uint32_t)s_game.score;
    prog_save();
    cn_snd(CN_SND_WIN);
    show_overlay(a, OV_WIN);
}

/* --------------------------------------------------------------------------
 * The frame
 * -------------------------------------------------------------------------- */

static void leak_start(cn_app_t *a)
{
    const cn_game_t *g = &s_game;
    int sc, sr;
    to_screen_cell(a, g->leak_c, g->leak_r, &sc, &sr);
    int sdir = (g->leak_dir + a->vrot) & 3;
    static const int DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };
    float x = ((float)sc + 0.5f + 0.5f * (float)DX[sdir]) * (float)a->C;
    float y = ((float)sr + 0.5f + 0.5f * (float)DY[sdir]) * (float)a->C;
    int nsc = sc + DX[sdir], nsr = sr + DY[sdir];
    bool edge = nsc < 0 || nsr < 0 || nsc >= a->scols || nsr >= a->srows;
    cn_draw_spill_clear(&a->draw);
    cn_draw_spill_start(&a->draw, x, y, sdir, edge);
    a->spill_on = true;
}

static void pad_play(cn_app_t *a)
{
    const aos_pad_t *p = &a->pad;
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        if (s_game.phase < CN_PH_WON) show_overlay(a, OV_PAUSE);
        return;
    }
    uint32_t dirs = p->repeat & AOS_PAD_DIRS;
    if (dirs || aos_pad_pressed(p, AOS_PAD_A)) {
        if (!a->cur_on) {
            a->cur_on = true;
            cursor_place(a);
            return;
        }
    }
    if (dirs) {
        if (dirs & AOS_PAD_UP && a->cur_sr > 0) a->cur_sr--;
        if (dirs & AOS_PAD_DOWN && a->cur_sr < a->srows - 1) a->cur_sr++;
        if (dirs & AOS_PAD_LEFT && a->cur_sc > 0) a->cur_sc--;
        if (dirs & AOS_PAD_RIGHT && a->cur_sc < a->scols - 1) a->cur_sc++;
        cursor_place(a);
    }
    if (aos_pad_pressed(p, AOS_PAD_A)) {
        int c, r;
        to_grid_cell(a, a->cur_sc, a->cur_sr, &c, &r);
        act_on(a, c, r);
    }
    if (aos_pad_pressed(p, AOS_PAD_B)) lv_obj_send_event(a->btn_valve, LV_EVENT_CLICKED, NULL);
    if (s_game.lv.mode == CN_MODE_FILL && aos_pad_pressed(p, AOS_PAD_L | AOS_PAD_R)) {
        int step = aos_pad_pressed(p, AOS_PAD_R) ? 1 : CN_TYPES - 1;
        int t = a->sel_type < 0 ? 0 : a->sel_type;
        for (int i = 0; i < CN_TYPES; i++) {
            t = (t + step) % CN_TYPES;
            if (s_game.lv.tray[t]) break;
        }
        a->sel_type = t;
        cn_snd(CN_SND_CLICK);
        refresh_tray(a);
    }
}

static void frame_cb(lv_timer_t *timer)
{
    cn_app_t *a = lv_timer_get_user_data(timer);
    uint32_t now = lv_tick_get();
    float dt = (float)(now - a->last_ms) / 1000.0f;
    a->last_ms = now;
    if (dt > 0.1f) dt = 0.1f;

    aos_pad_update(&a->pad, now);
    if (a->pad.pressed || a->pad.repeat) {
        if (a->page != PG_PLAY || a->ov_kind != OV_NONE) {
            if (a->page == PG_LEVELS && aos_pad_pressed(&a->pad, AOS_PAD_B)) {
                show_page(a, PG_HOME);
            } else if (a->ov_kind == OV_PAUSE && aos_pad_pressed(&a->pad, AOS_PAD_START | AOS_PAD_B)) {
                show_overlay(a, OV_NONE);
            } else if (aos_pad_menu_step(&a->pmenu, &a->pad)) {
                pmenu_follow(a);
            }
        } else {
            pad_play(a);
        }
    }

    if (a->page != PG_PLAY || !a->draw_ok) {
        cn_snd_tick();
        return;
    }
    uint32_t over = aos_ui_overlay();
    bool covered = over & (AOS_UI_OVER_PANEL | AOS_UI_OVER_SWITCHER | AOS_UI_OVER_GESTURE |
                           AOS_UI_OVER_CURTAIN | AOS_UI_OVER_NOT_FRONT | AOS_UI_OVER_LOCK);
    bool running = a->ov_kind == OV_NONE && !covered;
    cn_game_t *g = &s_game;
    const cn_world_t *wd = cn_world(g->lv.world);

    if (running) {
        a->t += dt;
        uint32_t ev = cn_game_step(g, dt);
        if (ev & CN_EV_OPEN) {
            cn_snd(CN_SND_VALVE);
            if (g->auto_opened) aos_ui_toast(_("¡Se abrió la llave!"), 1400);
        }
        if (ev & CN_EV_FLOW) cn_snd(CN_SND_GUSH);
        if ((ev & CN_EV_SECOND) && g->countdown <= 5.0f) cn_snd(CN_SND_TICK);
        if (ev & CN_EV_CROSS2) cn_snd(CN_SND_CROSS);
        if (ev & CN_EV_LEAK) {
            cn_snd(CN_SND_LEAK);
            leak_start(a);
        }
        if (ev & CN_EV_DRAIN) cn_snd(CN_SND_DRAIN);
        if (ev & CN_EV_WON) level_won(a);
        if (ev & CN_EV_LOST) {
            cn_snd(CN_SND_LOSE);
            show_overlay(a, OV_LOSE);
        }
        if (flowing()) {
            float sp = (g->phase == CN_PH_FLOW ? g->speed : cn_game_base_speed(g)) * (float)a->C;
            a->phase_px += sp * dt;
        }
        for (int r = 0; r < g->lv.rows; r++) {
            for (int c = 0; c < g->lv.cols; c++) {
                if (a->turn[r][c] < 0) {
                    a->turn[r][c] += dt * 900.0f;
                    if (a->turn[r][c] >= 0) {
                        a->turn[r][c] = 0;
                        a->dirty[r][c] = true;
                    }
                }
                if (a->drop[r][c] > 1.0f) {
                    a->drop[r][c] -= dt * 3.0f;
                    if (a->drop[r][c] <= 1.0f) {
                        a->drop[r][c] = 0;
                        a->dirty[r][c] = true;
                    }
                }
            }
        }
        float level = 0;
        if (g->phase == CN_PH_FLOW) level = g->fast ? 1.0f : 0.7f;
        else if (g->phase == CN_PH_DRAIN) level = 0.8f * (1.0f - g->end_t / 1.6f);
        else if (g->phase == CN_PH_SPILL) level = 1.0f - g->end_t / 3.2f;
        cn_snd_flow(g->lv.world, level);
    } else {
        cn_snd_flow(g->lv.world, 0);
    }
    if (a->spill_on && (running || a->ov_kind == OV_LOSE)) {
        float flow = g->phase == CN_PH_SPILL ? 1.0f - g->end_t / 3.2f : 0;
        if (a->ov_kind == OV_LOSE) {
            a->t += dt;
            a->phase_px += cn_game_base_speed(g) * (float)a->C * dt * 0.3f;
        }
        cn_draw_spill_step(&a->draw, dt, flow);
    }
    if (running || a->ov_kind == OV_LOSE) render(a, false);
    if (running) update_hud(a, false);
    cn_snd_tick();

    a->frames++;
    if (now - a->log_ms >= 5000) {
        if (a->frames && running && flowing()) {
            aos_hal_log("canos", "%s: %u fps, draw %u ms a frame, %u cells a frame, %u us a cell, turns of %d",
                        wd->fluid, (unsigned)(a->frames * 1000 / (now - a->log_ms)),
                        (unsigned)(a->draw_us / 1000 / a->frames), (unsigned)(a->cells / a->frames),
                        (unsigned)a->cell_us, a->stride);
        }
        a->log_ms = now;
        a->frames = a->draw_us = a->cells = 0;
    }
}

/* --------------------------------------------------------------------------
 * The app's life cycle
 * -------------------------------------------------------------------------- */

static bool cn_back(aos_app_t *self, void *inst)
{
    (void)self;
    cn_app_t *a = inst;
    if (a->page == PG_PLAY) {
        if (a->ov_kind == OV_PAUSE) show_overlay(a, OV_NONE);
        else if (a->ov_kind == OV_NONE && s_game.phase < CN_PH_WON) show_overlay(a, OV_PAUSE);
        else show_levels(a, s_game.lv.world);
        return true;
    }
    if (a->page == PG_LEVELS) {
        show_page(a, PG_HOME);
        return true;
    }
    return false;
}

static void cn_tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    aos_ui_take_gesture();
}

static void cn_show(aos_app_t *self, void *inst)
{
    (void)self;
    cn_app_t *a = inst;
    if (s_prog.sound) cn_snd_open();
    a->last_ms = lv_tick_get();
}

static void cn_hide(aos_app_t *self, void *inst)
{
    (void)self;
    cn_app_t *a = inst;
    if (a->page == PG_PLAY && a->ov_kind == OV_NONE && s_game.phase < CN_PH_WON) show_overlay(a, OV_PAUSE);
    cn_snd_flow(0, 0);
    cn_snd_close();
}

/* Development switches (on the board getenv() returns NULL):
 *   CANOS_PLAY=m,d,w,l   straight into a level (0-based numbers)
 *   CANOS_SOLVE=1        with the solution on the board; =2 also opens the valve
 *   CANOS_UNLOCK=1       every world and level open
 *   CANOS_LEAK=1         opens the valve at once (on a scrambled board: a leak) */
static void dev_switches(cn_app_t *a)
{
    static bool done;
    if (done) return;   /* once: not again when the screen turns */
    done = true;
    const char *u = getenv("CANOS_UNLOCK");
    if (u && u[0] == '1') s_unlock_all = true;
    const char *p = getenv("CANOS_PLAY");
    if (!p || !p[0]) return;
    int m = 0, d = 0, w = 0, l = 0;
    if (sscanf(p, "%d,%d,%d,%d", &m, &d, &w, &l) < 4) return;
    s_prog.mode = (uint8_t)(m % CN_MODES);
    s_prog.diff = (uint8_t)(d % CN_DIFFS);
    a->sel_world = w % CN_WORLDS;
    start_level(a, s_prog.mode, s_prog.diff, a->sel_world, l, true);
    const char *s = getenv("CANOS_SOLVE");
    if (s && s[0]) {
        cn_game_solve(&s_game);
        memset(a->dirty, 1, sizeof a->dirty);
        refresh_tray(a);
        if (s[0] == '2') cn_game_open(&s_game);
        render(a, true);
    }
    const char *lk = getenv("CANOS_LEAK");
    if (lk && lk[0]) cn_game_open(&s_game);
    const char *bench = getenv("CANOS_BENCH");
    if (bench && bench[0]) {
        /* the whole run full, then the frame drawn 100 times */
        cn_game_solve(&s_game);
        cn_game_open(&s_game);
        for (int i = 0; i < 4000 && s_game.phase < CN_PH_DRAIN; i++) cn_game_step(&s_game, 0.05f);
        s_game.phase = CN_PH_FLOW;
        memset(a->dirty, 1, sizeof a->dirty);
        render(a, true);
        a->draw_us = a->cells = 0;
        uint32_t t0 = (uint32_t)aos_hal_uptime_us();
        for (int i = 0; i < 100; i++) {
            a->t += 0.033f;
            a->phase_px += 3.0f;
            render(a, false);
        }
        uint32_t us = (uint32_t)aos_hal_uptime_us() - t0;
        aos_hal_log("canos", "bench: %u us a frame, %u cells a frame, %u us a cell", (unsigned)(us / 100),
                    (unsigned)(a->cells / 100), (unsigned)(a->cells ? us / a->cells : 0));
    }
}

static void *cn_create(aos_app_t *self, lv_obj_t *root)
{
    /* in PSRAM: the board's drawing state rides in it (its noise, tables
     * and particles are some 40 KB) */
    cn_app_t *a = cn_draw_alloc(sizeof(cn_app_t));
    if (!a) return NULL;
    memset(a, 0, sizeof *a);
    a->self = self;
    a->root = root;
    a->W = lv_obj_get_width(root);
    a->H = lv_obj_get_height(root);
    a->land = a->W > a->H;
    a->sel_type = -1;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    prog_load();
    cn_snd_enable(s_prog.sound);
    if (s_prog.sound) cn_snd_open();

    build_home(a);
    build_levels(a);
    build_play(a);
    build_overlay(a);
    a->page = -1;

    if (s_keep.valid && s_keep.in_game) {
        a->sel_world = s_game.lv.world;
        start_level(a, s_game.lv.mode, s_game.lv.diff, s_game.lv.world, s_game.lv.level, false);
        a->sel_type = s_keep.sel_type;
        refresh_tray(a);
    } else if (s_keep.valid && s_keep.page == PG_LEVELS) {
        show_levels(a, s_keep.sel_world);
    } else {
        a->sel_world = s_prog.world;
        show_page(a, PG_HOME);
    }
    dev_switches(a);
    aos_pad_reset(&a->pad, lv_tick_get());
    a->last_ms = a->log_ms = lv_tick_get();
    a->timer = lv_timer_create(frame_cb, FRAME_MS, a);
    return a;
}

static void cn_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    cn_app_t *a = inst;
    if (a->timer) lv_timer_delete(a->timer);
    s_keep.valid = true;
    s_keep.page = a->page;
    s_keep.sel_world = a->sel_world;
    s_keep.sel_type = a->sel_type;
    s_keep.in_game = a->page == PG_PLAY;
    prog_save();
    cn_snd_flow(0, 0);
    cn_snd_close();
    if (a->draw_ok) cn_draw_free(&a->draw);
    for (int w = 0; w < CN_WORLDS; w++) free(a->prev_buf[w]);
    for (int i = 0; i < CN_TYPES; i++) free(a->tray_buf[i]);
    free(a);
}

static bool cn_init(aos_app_t *app)
{
    app->desc.id       = "demo.canos";
    app->desc.name     = "Caños";
    app->desc.icon     = "C";
    app->desc.icon_vec = AOS_ICON_GAMEPAD;
    app->desc.color_a  = 0x2A6E9A;
    app->desc.color_b  = 0x0C2238;
    app->desc.order    = 152;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE;
    aos_icon_set_ops(app, CANOS_ICON, sizeof CANOS_ICON);

    app->create  = cn_create;
    app->destroy = cn_destroy;
    app->show    = cn_show;
    app->hide    = cn_hide;
    app->back    = cn_back;
    app->tick    = cn_tick;
    return true;
}

AOS_APP_ENTRY(cn_init);
