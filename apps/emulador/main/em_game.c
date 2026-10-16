/*
 * P4OS - Emulador: the game screen.
 *
 * The picture and the console's pad, in either orientation:
 *
 *   upright (720 wide)              lying down (1280 wide)
 *   +--------------------+          +------+------------------+------+
 *   |                    |          | L  ≡ |                  |   R  |
 *   |      picture       |          |      |                  |      |
 *   |                    |          |  ✚   |     picture      |  B A |
 *   +--------------------+          |      |                  |      |
 *   | L    ≡  »       R  |          |SELECT|                  |START |
 *   |                    |          +------+------------------+------+
 *   |  ✚           B  A  |
 *   |   SELECT START     |
 *   +--------------------+
 *
 * The picture fills the width upright (GBA 240x160 x3 = 720x480, the NES
 * 256x224 x2.8125 = 720x630) and the middle lying down, between two bands
 * of at least BAND_MIN for the thumbs. The scale is always a whole number
 * of sixteenths, the PPA's step, so it comes out exact.
 *
 * Fingers (aos_touch_points, two of them): each finger owns what it landed
 * on until it lifts. The d-pad follows the finger's angle from its centre,
 * eight ways, and keeps it when the thumb slides off; A and B are one
 * cluster, so a thumb between them presses both and a thumb rolling from
 * one to the other changes button without lifting.
 *
 * USB gamepads: pad i is player i+1, the first one together with the
 * screen. The buttons as a SNES-style pad numbers them (X=1, A=2, B=3,
 * Y=4, L=5, R=6, SELECT=9, START=10), with Y also B and X also A for
 * consoles without them; SELECT+START together open the menu.
 *
 * The picture goes up as Duke's and Doom's do (present_flip): the PPA
 * scales the frame into the panel's free buffer, what LVGL drew around it
 * (the pad) is brought up to date in that buffer from the one on screen,
 * and the buffer is flipped to. Nothing LVGL draws may overlap the
 * picture's box. In the simulator, which has no frame buffers, a canvas.
 */
#include "emu.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_gesture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS         8
#define BAND_MIN        220         /* lying down: each thumb's band, at least */
#define PAD_SETTLE_MS   60
#define FB_N            3
#define DIRTY_N         6
#define MENU_COMBO_MS   400         /* SELECT+START held this long: the menu */
#define LOG_MS          5000

/* The keys on screen. */
enum {
    PK_DPAD = 0, PK_A, PK_B, PK_X, PK_Y, PK_L, PK_R, PK_SELECT, PK_START,
    PK_MENU, PK_TURBO,
    PK_N
};

enum { OWN_NONE = -1, OWN_FACE = 100 };

typedef struct { int x, y, w, h; } rect_t;

typedef struct {
    uint16_t   *fb;
    bool        whole;
    int         nd;
    rect_t      d[DIRTY_N];
} fbrec_t;

typedef struct {
    rect_t      r;                  /* in root coordinates */
    bool        round;
    bool        used;
    lv_obj_t   *o;
    lv_obj_t   *lbl;
    uint32_t    btn;                /* EMU_BTN_* (0: MENU, TURBO) */
} pkey_t;

typedef struct emu_game {
    emu_game_t          game;
    const emu_core_t   *core;
    const emu_skin_t   *skin;
    bool                open;
    bool                running;    /* the worker has the game */
    bool                failed;

    lv_obj_t   *scr;                /* everything of the game screen */
    lv_obj_t   *msg;
    lv_obj_t   *fps;
    lv_obj_t   *arm[4];             /* the d-pad's lit arms: up, down, left, right */
    pkey_t      key[PK_N];

    int         w, h, ox, oy;       /* root, and its place on the screen */
    bool        land;
    rect_t      pic;                /* where the picture goes (root coordinates) */
    int         fw, fh;             /* the last frame's size */

    /* fingers */
    uint8_t     fid[2];
    int         fown[2];
    uint32_t    touch_btn;
    uint32_t    lit;                /* what is drawn lit */

    /* gamepads */
    uint32_t    pad_btn[EMU_MAX_PLAYERS];
    uint32_t    combo_ms;
    bool        combo_fired;

    bool        turbo;
    bool        was_up, pad_up;         /* after an error: waiting for a fresh tap */
    bool        blocked;
    uint32_t    over;
    bool        held;

    /* presenting */
    fbrec_t     fbr[FB_N];
    bool        redo;
    bool        pad_refr;
    uint32_t    pad_ms;
    uint16_t   *cv_buf;             /* the simulator's canvas */
    lv_obj_t   *cv;
    int         cv_w, cv_h;

    uint32_t    t_open;             /* when the game started, for the time played */
    uint32_t    log_ms;
    uint32_t    shown, shown_log;
    uint32_t    flips, directs;
} game_t;

static game_t *G_of(emu_app_t *a) { return a->G; }

/* ---------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */

static int frame_w(const game_t *g) { return g->fw ? g->fw : g->core->max_w; }
static int frame_h(const game_t *g)
{
    if (g->fh) return g->fh;
    /* the NES crops 8 lines top and bottom by default */
    return !strcmp(g->core->id, "nes") ? 224 : g->core->max_h;
}

/* The picture for a w x h frame in a bw x bh box, centred, by the scale
 * setting. Sixteenths: the PPA's step. */
static rect_t fit(int w, int h, int bx, int by, int bw, int bh)
{
    int mode = emu_pref("scale", EMU_SCALE_FIT);
    int pw, ph;
    if (mode == EMU_SCALE_FILL) {
        pw = bw;
        ph = bh;
    } else {
        int s16 = bw * 16 / w;
        if (bh * 16 / h < s16) s16 = bh * 16 / h;
        if (mode == EMU_SCALE_INT && s16 >= 16) s16 -= s16 % 16;
        if (s16 < 4) s16 = 4;
        pw = w * s16 / 16;
        ph = h * s16 / 16;
    }
    pw &= ~1;
    return (rect_t){ bx + (bw - pw) / 2, by + (bh - ph) / 2, pw, ph };
}

static void key_set(game_t *g, int k, int x, int y, int w, int h, bool round)
{
    g->key[k].r = (rect_t){ x, y, w, h };
    g->key[k].round = round;
    g->key[k].used = true;
}

static bool has(const game_t *g, uint32_t btn) { return (g->core->buttons & btn) != 0; }

static void layout(game_t *g)
{
    int W = g->w, H = g->h;
    g->land = W > H;
    for (int i = 0; i < PK_N; i++) g->key[i].used = false;
    bool lr = has(g, EMU_BTN_L) || has(g, EMU_BTN_R);
    bool xy = has(g, EMU_BTN_X) || has(g, EMU_BTN_Y);
    int fw = frame_w(g), fh = frame_h(g);

    if (!g->land) {
        int maxh = H * 52 / 100;
        g->pic = fit(fw, fh, 0, 16, W, maxh);
        if (g->pic.y > 16) g->pic.y = 16;
        int cy = g->pic.y + g->pic.h + 16;

        /* the top row: L, the menu and fast forward, R */
        int ty = cy + 10;
        if (lr) {
            key_set(g, PK_L, 24, ty, 190, 62, false);
            key_set(g, PK_R, W - 24 - 190, ty, 190, 62, false);
        }
        key_set(g, PK_MENU, W / 2 - 112, ty + 4, 104, 54, false);
        key_set(g, PK_TURBO, W / 2 + 8, ty + 4, 104, 54, false);

        /* the bottom row: SELECT and START */
        int by = H - 86;
        key_set(g, PK_SELECT, W / 2 - 150, by, 130, 50, false);
        key_set(g, PK_START, W / 2 + 20, by, 130, 50, false);

        /* the middle: the d-pad and the face buttons */
        int top = ty + 62 + 16, bot = by - 16;
        int my = (top + bot) / 2;
        int S = bot - top;
        if (S > 300) S = 300;
        if (S < 200) S = 200;
        key_set(g, PK_DPAD, 36, my - S / 2, S, S, false);
        int d = S * 44 / 100;
        if (d > 128) d = 128;
        int fx = W - 36 - d / 2 - 8;            /* the rightmost button's centre */
        if (xy) {
            int c = fx - d * 3 / 4;
            key_set(g, PK_A, c + d * 3 / 4 - d / 2, my - d / 2, d, d, true);
            key_set(g, PK_B, c - d / 2, my + d * 3 / 4 - d / 2, d, d, true);
            key_set(g, PK_X, c - d / 2, my - d * 3 / 4 - d / 2, d, d, true);
            key_set(g, PK_Y, c - d * 3 / 4 - d / 2, my - d / 2, d, d, true);
        } else if (!strcmp(g->core->id, "nes")) {
            /* side by side, as on the NES pad */
            key_set(g, PK_A, fx - d / 2, my - d / 2 + 10, d, d, true);
            key_set(g, PK_B, fx - d - 30 - d / 2, my - d / 2 + 10, d, d, true);
        } else {
            /* the diagonal of a Game Boy */
            key_set(g, PK_A, fx - d / 2, my - d / 2 - d * 2 / 5, d, d, true);
            key_set(g, PK_B, fx - d - 22 - d / 2, my - d / 2 + d * 2 / 5, d, d, true);
        }
    } else {
        g->pic = fit(fw, fh, BAND_MIN, 0, W - 2 * BAND_MIN, H);
        int band = g->pic.x;
        int rx = g->pic.x + g->pic.w;
        int rband = W - rx;
        if (lr) {
            key_set(g, PK_L, 16, 16, band - 32, 60, false);
            key_set(g, PK_R, rx + 16, 16, rband - 32, 60, false);
        }
        int my0 = lr ? 92 : 20;
        int half = (band - 40) / 2;
        key_set(g, PK_MENU, 16, my0, half, 52, false);
        key_set(g, PK_TURBO, 24 + half, my0, half, 52, false);

        key_set(g, PK_SELECT, band / 2 - 64, H - 74, 128, 50, false);
        key_set(g, PK_START, rx + rband / 2 - 64, H - 74, 128, 50, false);

        int top = my0 + 52 + 12, bot = H - 74 - 12;
        int my = (top + bot) / 2;
        int S = band - 28;
        if (S > bot - top) S = bot - top;
        if (S > 280) S = 280;
        key_set(g, PK_DPAD, band / 2 - S / 2, my - S / 2, S, S, false);

        int cx = rx + rband / 2;
        int d = rband * 42 / 100;
        if (d > 118) d = 118;
        int o = d * 62 / 100;
        if (xy) {
            key_set(g, PK_A, cx + o - d / 2, my - d / 2, d, d, true);
            key_set(g, PK_B, cx - d / 2, my + o - d / 2, d, d, true);
            key_set(g, PK_X, cx - d / 2, my - o - d / 2, d, d, true);
            key_set(g, PK_Y, cx - o - d / 2, my - d / 2, d, d, true);
        } else {
            int o2 = d * 52 / 100;
            key_set(g, PK_A, cx + o2 - d / 2, my - o2 - d / 2 + 6, d, d, true);
            key_set(g, PK_B, cx - o2 - d / 2, my + o2 - d / 2 - 6, d, d, true);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The pad, drawn by LVGL
 * ------------------------------------------------------------------------ */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static uint32_t key_color(const game_t *g, int k, bool on)
{
    const emu_skin_t *s = g->skin;
    switch (k) {
    case PK_A: return on ? 0xFFFFFF : s->a;
    case PK_B: return on ? 0xFFFFFF : s->b;
    case PK_X: case PK_Y: return on ? 0xFFFFFF : s->key_lit;
    case PK_TURBO: return on ? EMU_C_ACCENT : s->key;
    default: return on ? s->key_lit : s->key;
    }
}

static void key_paint(game_t *g, int k, bool on)
{
    pkey_t *p = &g->key[k];
    if (!p->o) return;
    lv_obj_set_style_bg_color(p->o, lv_color_hex(key_color(g, k, on)), 0);
    if (p->lbl) {
        bool face = k == PK_A || k == PK_B || k == PK_X || k == PK_Y;
        uint32_t lc = face && on ? g->skin->a : g->skin->label;
        if (face && on && k == PK_B) lc = g->skin->b;
        lv_obj_set_style_text_color(p->lbl, lv_color_hex(lc), 0);
    }
}

static void pad_mark(game_t *g, rect_t r)
{
    r.x -= 4; r.y -= 4; r.w += 8; r.h += 8;
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > g->w) r.w = g->w - r.x;
    if (r.y + r.h > g->h) r.h = g->h - r.y;
    if (r.w <= 0 || r.h <= 0) return;
    for (int i = 0; i < FB_N; i++) {
        fbrec_t *f = &g->fbr[i];
        if (f->nd < DIRTY_N) {
            f->d[f->nd++] = r;
            continue;
        }
        /* full: merge into the last one */
        rect_t *d = &f->d[DIRTY_N - 1];
        int x1 = d->x + d->w > r.x + r.w ? d->x + d->w : r.x + r.w;
        int y1 = d->y + d->h > r.y + r.h ? d->y + d->h : r.y + r.h;
        d->x = d->x < r.x ? d->x : r.x;
        d->y = d->y < r.y ? d->y : r.y;
        d->w = x1 - d->x;
        d->h = y1 - d->y;
    }
    g->pad_ms = (uint32_t)aos_hal_uptime_ms();
    g->pad_refr = true;
}

static void lit_update(game_t *g, uint32_t want_btn)
{
    /* what each key shows: pressed by a finger or a pad */
    uint32_t lit = want_btn;
    if (g->turbo) lit |= 1u << 31;
    uint32_t ch = lit ^ g->lit;
    if (!ch) return;
    g->lit = lit;
    static const struct { int k; uint32_t b; } MAP[] = {
        { PK_A, EMU_BTN_A }, { PK_B, EMU_BTN_B }, { PK_X, EMU_BTN_X }, { PK_Y, EMU_BTN_Y },
        { PK_L, EMU_BTN_L }, { PK_R, EMU_BTN_R }, { PK_SELECT, EMU_BTN_SELECT },
        { PK_START, EMU_BTN_START },
    };
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
        if ((ch & MAP[i].b) && g->key[MAP[i].k].used) {
            key_paint(g, MAP[i].k, (lit & MAP[i].b) != 0);
            pad_mark(g, g->key[MAP[i].k].r);
        }
    }
    if (ch & (1u << 31)) {
        key_paint(g, PK_TURBO, g->turbo);
        pad_mark(g, g->key[PK_TURBO].r);
    }
    if (ch & EMU_BTN_DPAD) {
        static const uint32_t DIRS[4] = { EMU_BTN_UP, EMU_BTN_DOWN, EMU_BTN_LEFT, EMU_BTN_RIGHT };
        for (int i = 0; i < 4; i++) {
            if (!g->arm[i]) continue;
            if (lit & DIRS[i]) lv_obj_remove_flag(g->arm[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(g->arm[i], LV_OBJ_FLAG_HIDDEN);
        }
        pad_mark(g, g->key[PK_DPAD].r);
    }
}

static void build_dpad(game_t *g, lv_obj_t *parent, rect_t r)
{
    const emu_skin_t *s = g->skin;
    int S = r.w, t = S * 34 / 100;
    lv_obj_t *base = box(parent);
    lv_obj_set_pos(base, r.x, r.y);
    lv_obj_set_size(base, S, S);
    g->key[PK_DPAD].o = base;

    /* a soft well under the cross */
    lv_obj_t *well = box(base);
    lv_obj_set_size(well, S, S);
    lv_obj_set_style_radius(well, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(well, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(well, 60, 0);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *bar = box(base);
        if (i == 0) {
            lv_obj_set_size(bar, t, S - 12);
        } else {
            lv_obj_set_size(bar, S - 12, t);
        }
        lv_obj_center(bar);
        lv_obj_set_style_radius(bar, 14, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(s->key), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(bar, 10, 0);
        lv_obj_set_style_shadow_color(bar, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(bar, 120, 0);
        lv_obj_set_style_shadow_offset_y(bar, 4, 0);
    }
    /* the arms lit when pressed */
    int arm = (S - 12 - t) / 2;
    const struct { int x, y, w, h; } A[4] = {
        { (S - t) / 2, 6, t, arm }, { (S - t) / 2, S - 6 - arm, t, arm },
        { 6, (S - t) / 2, arm, t }, { S - 6 - arm, (S - t) / 2, arm, t },
    };
    static const char *const SYM[4] = { AOS_SYM_ARROW_UP_BOLD, AOS_SYM_ARROW_DOWN_BOLD,
                                        AOS_SYM_ARROW_LEFT_BOLD, AOS_SYM_ARROW_RIGHT_BOLD };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *l = box(base);
        lv_obj_set_pos(l, A[i].x, A[i].y);
        lv_obj_set_size(l, A[i].w, A[i].h);
        lv_obj_set_style_radius(l, 12, 0);
        lv_obj_set_style_bg_color(l, lv_color_hex(s->key_lit), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        g->arm[i] = l;

        lv_obj_t *ar = lv_label_create(base);
        lv_label_set_text(ar, SYM[i]);
        lv_obj_set_style_text_font(ar, &aos_sym_28, 0);
        lv_obj_set_style_text_color(ar, lv_color_hex(s->label), 0);
        lv_obj_set_style_text_opa(ar, 150, 0);
        lv_obj_update_layout(ar);
        int lw = lv_obj_get_width(ar), lh = lv_obj_get_height(ar);
        lv_obj_set_pos(ar, A[i].x + (A[i].w - lw) / 2, A[i].y + (A[i].h - lh) / 2);
    }
    lv_obj_t *hub = box(base);
    lv_obj_set_size(hub, t * 2 / 3, t * 2 / 3);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(hub, 50, 0);
}

static void build_key(game_t *g, lv_obj_t *parent, int k, const char *text, const lv_font_t *font)
{
    pkey_t *p = &g->key[k];
    lv_obj_t *o = box(parent);
    lv_obj_set_pos(o, p->r.x, p->r.y);
    lv_obj_set_size(o, p->r.w, p->r.h);
    lv_obj_set_style_radius(o, p->round ? LV_RADIUS_CIRCLE : p->r.h / 2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(o, 12, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(o, 130, 0);
    lv_obj_set_style_shadow_offset_y(o, 4, 0);
    if (p->round) {
        lv_obj_set_style_border_width(o, 3, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_opa(o, 70, 0);
    }
    p->o = o;
    p->lbl = lv_label_create(o);
    lv_label_set_text(p->lbl, text);
    lv_obj_set_style_text_font(p->lbl, font, 0);
    lv_obj_center(p->lbl);
    key_paint(g, k, false);
}

static void pad_build(game_t *g)
{
    lv_obj_t *s = g->scr;
    lv_obj_clean(s);
    for (int i = 0; i < PK_N; i++) {
        g->key[i].o = NULL;
        g->key[i].lbl = NULL;
    }
    for (int i = 0; i < 4; i++) g->arm[i] = NULL;

    /* the console's body behind everything but the picture: one colour,
     * since a gradient this big bands (LVGL renders it in strips) */
    lv_obj_set_style_bg_color(s, lv_color_hex(g->skin->body), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);

    /* the screen's well, black, the size of the picture: under the
     * picture LVGL never draws anything else */
    lv_obj_t *well = box(s);
    lv_obj_set_pos(well, g->pic.x, g->pic.y);
    lv_obj_set_size(well, g->pic.w, g->pic.h);
    lv_obj_set_style_bg_color(well, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);

    if (g->key[PK_DPAD].used) build_dpad(g, s, g->key[PK_DPAD].r);
    const lv_font_t *big = aos_font_title, *small = aos_font_caption;
    if (g->key[PK_A].used) build_key(g, s, PK_A, "A", big);
    if (g->key[PK_B].used) build_key(g, s, PK_B, "B", big);
    if (g->key[PK_X].used) build_key(g, s, PK_X, "X", big);
    if (g->key[PK_Y].used) build_key(g, s, PK_Y, "Y", big);
    if (g->key[PK_L].used) build_key(g, s, PK_L, "L", aos_font_body);
    if (g->key[PK_R].used) build_key(g, s, PK_R, "R", aos_font_body);
    if (g->key[PK_SELECT].used) build_key(g, s, PK_SELECT, "SELECT", small);
    if (g->key[PK_START].used) build_key(g, s, PK_START, "START", small);
    if (g->key[PK_MENU].used) build_key(g, s, PK_MENU, AOS_SYM_PAUSE, &aos_sym_28);
    if (g->key[PK_TURBO].used) build_key(g, s, PK_TURBO, AOS_SYM_SKIP_NEXT, &aos_sym_28);

    /* the frames a second, when asked for: beside the menu */
    g->fps = lv_label_create(s);
    lv_obj_set_style_text_font(g->fps, aos_font_tiny, 0);
    lv_obj_set_style_text_color(g->fps, lv_color_hex(g->skin->label), 0);
    lv_label_set_text(g->fps, "");
    if (g->land) {
        lv_obj_set_pos(g->fps, 16, g->key[PK_MENU].r.y + g->key[PK_MENU].r.h + 6);
    } else {
        lv_obj_set_pos(g->fps, 24, g->pic.y + g->pic.h + 2);
    }
    if (!emu_pref("fps", 0)) lv_obj_add_flag(g->fps, LV_OBJ_FLAG_HIDDEN);

    /* messages over the picture (loading, errors) */
    g->msg = lv_label_create(s);
    lv_label_set_long_mode(g->msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(g->msg, g->pic.w - 60);
    lv_obj_set_style_text_align(g->msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g->msg, lv_color_hex(EMU_C_TEXT), 0);
    lv_obj_set_style_text_font(g->msg, aos_font_body, 0);
    lv_obj_set_pos(g->msg, g->pic.x + 30, g->pic.y + g->pic.h / 2 - 60);
    lv_obj_add_flag(g->msg, LV_OBJ_FLAG_HIDDEN);

    g->lit = 0;
    lit_update(g, 0);
    g->cv = NULL;               /* lv_obj_clean took it */
    g->redo = true;
}

static void show_msg(game_t *g, const char *text)
{
    if (!g->msg) return;
    if (text) {
        lv_label_set_text(g->msg, text);
        lv_obj_remove_flag(g->msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g->msg, LV_OBJ_FLAG_HIDDEN);
    }
    g->redo = true;
}

/* ---------------------------------------------------------------------------
 * Fingers
 * ------------------------------------------------------------------------ */

static bool in_rect(rect_t r, int x, int y, int grow)
{
    return x >= r.x - grow && x < r.x + r.w + grow && y >= r.y - grow && y < r.y + r.h + grow;
}

static int claim(game_t *g, int x, int y)
{
    if (in_rect(g->pic, x, y, 0)) return OWN_NONE;
    /* the face cluster: the box around its buttons, a little bigger */
    rect_t fb = { 0 };
    bool any = false;
    for (int k = PK_A; k <= PK_Y; k++) {
        if (!g->key[k].used) continue;
        rect_t r = g->key[k].r;
        if (!any) {
            fb = r;
            any = true;
        } else {
            int x1 = fb.x + fb.w > r.x + r.w ? fb.x + fb.w : r.x + r.w;
            int y1 = fb.y + fb.h > r.y + r.h ? fb.y + fb.h : r.y + r.h;
            fb.x = fb.x < r.x ? fb.x : r.x;
            fb.y = fb.y < r.y ? fb.y : r.y;
            fb.w = x1 - fb.x;
            fb.h = y1 - fb.y;
        }
    }
    if (any && in_rect(fb, x, y, 30)) return OWN_FACE;
    if (g->key[PK_DPAD].used && in_rect(g->key[PK_DPAD].r, x, y, 40)) return PK_DPAD;
    for (int k = PK_L; k < PK_N; k++) {
        if (g->key[k].used && in_rect(g->key[k].r, x, y, 14)) return k;
    }
    return OWN_NONE;
}

static uint32_t dpad_dirs(game_t *g, int x, int y)
{
    rect_t r = g->key[PK_DPAD].r;
    int dx = x - (r.x + r.w / 2), dy = y - (r.y + r.h / 2);
    int dead = r.w * 12 / 100;
    if (dx * dx + dy * dy < dead * dead) return 0;
    /* eight ways: a direction counts when the other axis is less than
     * tan(67.5°) ~ 2.414 times it */
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    uint32_t b = 0;
    if (ax * 1000 > ay * 414) b |= dx < 0 ? EMU_BTN_LEFT : EMU_BTN_RIGHT;
    if (ay * 1000 > ax * 414) b |= dy < 0 ? EMU_BTN_UP : EMU_BTN_DOWN;
    return b;
}

static uint32_t face_btns(game_t *g, int x, int y)
{
    static const struct { int k; uint32_t b; } F[] = {
        { PK_A, EMU_BTN_A }, { PK_B, EMU_BTN_B }, { PK_X, EMU_BTN_X }, { PK_Y, EMU_BTN_Y },
    };
    uint32_t b = 0;
    int best = -1, bd = 0;
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        const pkey_t *p = &g->key[F[i].k];
        if (!p->used) continue;
        int cx = p->r.x + p->r.w / 2, cy = p->r.y + p->r.h / 2;
        int rad = p->r.w * 62 / 100;            /* a bit past the edge: between two is both */
        int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        if (d2 <= rad * rad) b |= F[i].b;
        if (best < 0 || d2 < bd) {
            best = (int)i;
            bd = d2;
        }
    }
    if (!b && best >= 0) b = F[best].b;
    return b;
}

static void open_menu(emu_app_t *a);

static void touch_poll(emu_app_t *a, game_t *g)
{
    aos_touch_point_t pts[2];
    aos_touch_points(pts);
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);
    uint32_t b = 0;
    for (int i = 0; i < 2; i++) {
        uint8_t id = pts[i].down ? pts[i].id : 0;
        int x = (int)pts[i].x - rc.x1, y = (int)pts[i].y - rc.y1;
        if (id != g->fid[i]) {
            g->fid[i] = id;
            g->fown[i] = id ? claim(g, x, y) : OWN_NONE;
            if (id && g->fown[i] == PK_MENU) {
                open_menu(a);
                g->fown[i] = OWN_NONE;
                return;
            }
            if (id && g->fown[i] == PK_TURBO) {
                g->turbo = !g->turbo;
                emu_run_turbo(g->turbo);
                g->fown[i] = OWN_NONE;
            }
        }
        if (!id) continue;
        switch (g->fown[i]) {
        case PK_DPAD:   b |= dpad_dirs(g, x, y); break;
        case OWN_FACE:  b |= face_btns(g, x, y); break;
        case PK_L:      b |= EMU_BTN_L; break;
        case PK_R:      b |= EMU_BTN_R; break;
        case PK_SELECT: b |= EMU_BTN_SELECT; break;
        case PK_START:  b |= EMU_BTN_START; break;
        default: break;
        }
    }
    g->touch_btn = b;
}

static void touch_release(game_t *g)
{
    g->fid[0] = g->fid[1] = 0;
    g->fown[0] = g->fown[1] = OWN_NONE;
    g->touch_btn = 0;
}

/* ---------------------------------------------------------------------------
 * Gamepads
 * ------------------------------------------------------------------------ */

static uint32_t pad_read(int i, bool *menu_combo)
{
    aos_gamepad_t p;
    if (!aos_hal_hid_gamepad_get(i, &p) || !p.connected) return 0;
    uint32_t b = 0;
    uint8_t d = aos_hal_hid_gamepad_dpad(&p);
    if (d & AOS_DPAD_UP) b |= EMU_BTN_UP;
    if (d & AOS_DPAD_DOWN) b |= EMU_BTN_DOWN;
    if (d & AOS_DPAD_LEFT) b |= EMU_BTN_LEFT;
    if (d & AOS_DPAD_RIGHT) b |= EMU_BTN_RIGHT;
    uint32_t r = p.buttons;
    /* SNES-style numbering, 1-based: X=1 A=2 B=3 Y=4 L=5 R=6 L2=7 R2=8 SELECT=9 START=10 */
    if (r & (1u << 0)) b |= EMU_BTN_X;
    if (r & (1u << 1)) b |= EMU_BTN_A;
    if (r & (1u << 2)) b |= EMU_BTN_B;
    if (r & (1u << 3)) b |= EMU_BTN_Y;
    if (r & (1u << 4)) b |= EMU_BTN_L;
    if (r & (1u << 5)) b |= EMU_BTN_R;
    if (r & (1u << 6)) b |= EMU_BTN_L2;
    if (r & (1u << 7)) b |= EMU_BTN_R2;
    if (r & (1u << 8)) b |= EMU_BTN_SELECT;
    if (r & (1u << 9)) b |= EMU_BTN_START;
    if (r & (1u << 12)) *menu_combo = true;     /* a "home" button, where there is one */
    if ((b & (EMU_BTN_SELECT | EMU_BTN_START)) == (EMU_BTN_SELECT | EMU_BTN_START)) *menu_combo = true;
    return b;
}

/* The pad's buttons as the console has them: X and Y are A and B on a
 * console without them, L2/R2 are L/R. */
static uint32_t fold(const emu_core_t *core, uint32_t b)
{
    if (!(core->buttons & EMU_BTN_X) && (b & EMU_BTN_X)) b = (b & ~EMU_BTN_X) | EMU_BTN_A;
    if (!(core->buttons & EMU_BTN_Y) && (b & EMU_BTN_Y)) b = (b & ~EMU_BTN_Y) | EMU_BTN_B;
    if (!(core->buttons & EMU_BTN_L2) && (b & EMU_BTN_L2)) b = (b & ~EMU_BTN_L2) | EMU_BTN_L;
    if (!(core->buttons & EMU_BTN_R2) && (b & EMU_BTN_R2)) b = (b & ~EMU_BTN_R2) | EMU_BTN_R;
    return b & core->buttons;
}

static void input_poll(emu_app_t *a, game_t *g)
{
    touch_poll(a, g);
    if (emu_menu_is_open(a)) return;
    bool combo = false;
    uint32_t pads[EMU_MAX_PLAYERS] = { 0 };
    for (int i = 0; i < EMU_MAX_PLAYERS && i < AOS_GAMEPAD_MAX; i++) {
        pads[i] = pad_read(i, &combo);
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (combo) {
        if (!g->combo_ms) g->combo_ms = now;
        if (!g->combo_fired && now - g->combo_ms >= MENU_COMBO_MS) {
            g->combo_fired = true;
            open_menu(a);
            return;
        }
        /* while the combination is held, the game does not see it */
        for (int i = 0; i < EMU_MAX_PLAYERS; i++) pads[i] &= ~(EMU_BTN_SELECT | EMU_BTN_START);
    } else {
        g->combo_ms = 0;
        g->combo_fired = false;
    }
    pads[0] |= g->touch_btn;
    for (int i = 0; i < EMU_MAX_PLAYERS; i++) pads[i] = fold(g->core, pads[i]);
    for (int i = g->core->players; i < EMU_MAX_PLAYERS; i++) pads[i] = 0;
    emu_run_pads(pads);
    lit_update(g, pads[0]);
}

/* ---------------------------------------------------------------------------
 * Presenting
 * ------------------------------------------------------------------------ */

static rect_t to_panel(rect_t r, int rot)
{
    switch (rot) {
    case 90:  return (rect_t){ AOS_PANEL_W - (r.y + r.h), r.x, r.h, r.w };
    case 180: return (rect_t){ AOS_PANEL_W - (r.x + r.w), AOS_PANEL_H - (r.y + r.h), r.w, r.h };
    case 270: return (rect_t){ r.y, AOS_PANEL_H - (r.x + r.w), r.h, r.w };
    default:  return r;
    }
}

static void copy_box(game_t *g, uint16_t *dst, const uint16_t *src, rect_t s, int rot)
{
    s.x += g->ox;
    s.y += g->oy;
    rect_t p = to_panel(s, rot);
    if (p.w <= 0 || p.h <= 0) return;
    if (p.x < 0 || p.y < 0 || p.x + p.w > AOS_PANEL_W || p.y + p.h > AOS_PANEL_H) return;
    for (int y = 0; y < p.h; y++) {
        size_t o = (size_t)(p.y + y) * AOS_PANEL_W + p.x;
        memcpy(dst + o, src + o, (size_t)p.w * 2);
    }
}

static fbrec_t *fb_rec(game_t *g, uint16_t *fb)
{
    for (int i = 0; i < FB_N; i++)
        if (g->fbr[i].fb == fb) return &g->fbr[i];
    for (int i = 0; i < FB_N; i++) {
        if (!g->fbr[i].fb) {
            g->fbr[i].fb = fb;
            g->fbr[i].whole = false;
            g->fbr[i].nd = 0;
            return &g->fbr[i];
        }
    }
    return NULL;
}

static bool present_flip(game_t *g, const uint16_t *f, int fw, int fh, int stride)
{
    uint16_t *fb = aos_hal_display_back();
    fbrec_t *r = fb ? fb_rec(g, fb) : NULL;
    uint32_t age = 0;
    if (!r || !aos_hal_display_back_age(fb, &age, NULL)) return false;
    if (g->redo) {
        g->redo = false;
        for (int i = 0; i < FB_N; i++) g->fbr[i].whole = false;
    }
    if (age == UINT32_MAX) r->whole = false;
    if (g->pad_refr || !r->whole) {
        g->pad_refr = false;
        lv_refr_now(NULL);
    }
    int rot = aos_hal_display_get_rotation();
    rect_t p = g->pic;
    if (!aos_hal_display_blit_into_fit(fb, p.x + g->ox, p.y + g->oy, p.w, p.h, f, fw, fh, stride, false)) {
        return false;
    }
    const uint16_t *front = NULL;
    int sw, sh;
    if (!r->whole || r->nd) {
        if (!aos_hal_display_fb(&front, &sw, &sh)) front = NULL;
    }
    if (!r->whole && front) {
        /* everything around the picture, from the buffer on screen */
        copy_box(g, fb, front, (rect_t){ 0, 0, g->w, p.y }, rot);
        copy_box(g, fb, front, (rect_t){ 0, p.y + p.h, g->w, g->h - p.y - p.h }, rot);
        copy_box(g, fb, front, (rect_t){ 0, p.y, p.x, p.h }, rot);
        copy_box(g, fb, front, (rect_t){ p.x + p.w, p.y, g->w - p.x - p.w, p.h }, rot);
        r->whole = true;
        r->nd = 0;
    } else if (front && r->nd) {
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        for (int i = 0; i < r->nd; i++) copy_box(g, fb, front, r->d[i], rot);
        /* a key just lit may still be drawing: keep copying for a moment */
        if (now - g->pad_ms >= PAD_SETTLE_MS) r->nd = 0;
    }
    if (!aos_hal_display_flip(fb)) {
        r->whole = false;
        return false;
    }
    g->flips++;
    return true;
}

/* The simulator: a canvas the size of the picture, scaled on the CPU. */
static void present_canvas(emu_app_t *a, game_t *g, const uint16_t *f, int fw, int fh, int stride)
{
    int pw = g->pic.w, ph = g->pic.h;
    if (!g->cv || g->cv_w != pw || g->cv_h != ph) {
        if (g->cv) lv_obj_delete(g->cv);
        g->cv = NULL;
        free(g->cv_buf);
        g->cv_buf = emu_psram((size_t)pw * ph * 2);
        if (!g->cv_buf) return;
        g->cv_w = pw;
        g->cv_h = ph;
        g->cv = lv_canvas_create(g->scr);
        lv_canvas_set_buffer(g->cv, g->cv_buf, pw, ph, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(g->cv, g->pic.x, g->pic.y);
        lv_obj_remove_flag(g->cv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(g->cv, 1);
        (void)a;
    }
    for (int y = 0; y < ph; y++) {
        const uint16_t *s = f + (size_t)(y * fh / ph) * stride;
        uint16_t *d = g->cv_buf + (size_t)y * pw;
        for (int x = 0; x < pw; x++) d[x] = s[x * fw / pw];
    }
    lv_obj_invalidate(g->cv);
}

static void push_frame(emu_app_t *a, game_t *g)
{
    int fw, fh, stride;
    const uint16_t *f = emu_frame_take(&fw, &fh, &stride);
    if (!f) return;
    if (fw != g->fw || fh != g->fh) {
        /* the console changed its picture's size (the first frame, the NES
         * overscan option): lay the screen out again */
        g->fw = fw;
        g->fh = fh;
        layout(g);
        pad_build(g);
        aos_hal_log(EMU_TAG, "picture %dx%d -> %dx%d at %d,%d", fw, fh, g->pic.w, g->pic.h, g->pic.x, g->pic.y);
    }
    bool ok = !(g->over & AOS_UI_OVER_TOAST) && present_flip(g, f, fw, fh, stride);
    if (!ok) {
        ok = aos_hal_display_blit_fit(g->pic.x + g->ox, g->pic.y + g->oy, g->pic.w, g->pic.h, f, fw, fh,
                                      stride, false);
        if (ok) {
            g->directs++;
            g->redo = true;
        } else {
            present_canvas(a, g, f, fw, fh, stride);
        }
    }
    emu_frame_release();
    g->shown++;
}

/* ---------------------------------------------------------------------------
 * The frame timer
 * ------------------------------------------------------------------------ */

static void gestures_block(game_t *g, bool on)
{
    if (g->blocked != on) {
        g->blocked = on;
        aos_ui_block_gestures(on);
    }
}

static void log_rates(game_t *g, uint32_t now)
{
    if (!g->log_ms) {
        g->log_ms = now;
        g->shown_log = g->shown;
        return;
    }
    uint32_t dt = now - g->log_ms;
    emu_run_stats_t s;
    emu_run_stats(&s);
    float shown = dt ? (g->shown - g->shown_log) * 1000.0f / dt : 0;
    if (g->fps && !lv_obj_has_flag(g->fps, LV_OBJ_FLAG_HIDDEN) && dt >= 500) {
        char t[64];
        snprintf(t, sizeof t, "%.0f fps  %.1f ms", (double)s.fps_emu, (double)s.run_ms);
        lv_label_set_text(g->fps, t);
        lv_obj_t *l = g->fps;
        lv_area_t c;
        lv_obj_get_coords(l, &c);
        pad_mark(g, (rect_t){ c.x1 - 0, c.y1, 160, c.y2 - c.y1 + 1 });
    }
    if (dt < LOG_MS) return;
    const char *cs = g->core->stats ? g->core->stats() : NULL;
    aos_hal_log(EMU_TAG, "%s: %.1f fps run, %.1f drawn, %.1f shown | core %.1f ms (max %.1f) | skipped %u, "
                "underruns %u | %u flips, %u direct%s%s",
                g->core->id, (double)s.fps_emu, (double)s.fps_drawn, (double)shown, (double)s.run_ms,
                (double)s.run_ms_max, (unsigned)s.skipped, (unsigned)s.underruns, (unsigned)g->flips,
                (unsigned)g->directs, cs ? " | " : "", cs ? cs : "");
    g->log_ms = now;
    g->shown_log = g->shown;
    g->flips = g->directs = 0;
}

static void game_frame(emu_app_t *a, game_t *g)
{
    emu_run_state_t st = emu_run_state();
    if (st == EMU_RUN_ERROR && !g->failed) {
        g->failed = true;
        char t[300];
        snprintf(t, sizeof t, "%s\n\n%s\n\n%s", g->game.title, emu_run_error(), _("Tocá para volver"));
        show_msg(g, t);
        gestures_block(g, false);
        return;
    }
    if (g->failed) {
        /* a tap (down after up, so not the finger that opened the game) or
         * A / START on a pad goes back to the library */
        aos_touch_point_t pts[2];
        bool down = aos_touch_points(pts) > 0;
        bool pressed = down && g->was_up;
        g->was_up = !down;
        bool combo = false;
        uint32_t pb = pad_read(0, &combo);
        if (pb & (EMU_BTN_A | EMU_BTN_START)) {
            if (g->pad_up) pressed = true;
        } else {
            g->pad_up = true;
        }
        if (pressed) emu_game_close(a, false);
        return;
    }
    if (st == EMU_RUN_LOADING) return;
    if (st != EMU_RUN_RUNNING) return;
    if (!g->running) {
        g->running = true;
        show_msg(g, NULL);
        emu_recent_touch(&g->game, 0);
    }

    g->over = aos_ui_overlay();
    const char *cur = aos_ui_current_app();
    if (!cur || strcmp(cur, a->self->desc.id) != 0) g->over |= AOS_UI_OVER_NOT_FRONT;
    bool menu = emu_menu_is_open(a);
    bool hold = (g->over & ~(uint32_t)AOS_UI_OVER_TOAST) != 0 || menu;
    if (hold != g->held) {
        g->held = hold;
        emu_run_pause(hold);
        if (hold) {
            touch_release(g);
            uint32_t none[EMU_MAX_PLAYERS] = { 0 };
            emu_run_pads(none);
            lit_update(g, 0);
        }
        gestures_block(g, !hold);
        g->redo = true;
    }
    if (menu) {
        emu_menu_step(a);
        return;
    }
    if (hold) return;
    input_poll(a, g);
    if (emu_menu_is_open(a)) return;
    push_frame(a, g);
    log_rates(g, (uint32_t)aos_hal_uptime_ms());
}

/* ---------------------------------------------------------------------------
 * From the app
 * ------------------------------------------------------------------------ */

static void open_menu(emu_app_t *a)
{
    game_t *g = G_of(a);
    if (!g || !g->running || emu_menu_is_open(a)) return;
    touch_release(g);
    if (g->turbo) {
        g->turbo = false;
        emu_run_turbo(false);
    }
    emu_run_pause(true);
    g->held = true;
    gestures_block(g, false);
    emu_menu_open(a);
}

static void geometry(emu_app_t *a, game_t *g)
{
    g->w = lv_obj_get_width(a->root);
    g->h = lv_obj_get_height(a->root);
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);
    g->ox = rc.x1;
    g->oy = rc.y1;
}

bool emu_game_open(emu_app_t *a, const emu_game_t *gm)
{
    game_t *g = a->G;
    if (!g) {
        g = lv_malloc_zeroed(sizeof *g);
        if (!g) return false;
        a->G = g;
    }
    memset(g, 0, sizeof *g);
    g->game = *gm;
    g->core = gm->core;
    g->skin = emu_skin(gm->core);
    g->fown[0] = g->fown[1] = OWN_NONE;
    g->open = true;
    g->t_open = (uint32_t)aos_hal_uptime_ms();

    g->scr = box(a->root);
    lv_obj_set_size(g->scr, LV_PCT(100), LV_PCT(100));
    lv_obj_update_layout(a->root);
    geometry(a, g);
    layout(g);
    pad_build(g);

    char t[200];
    snprintf(t, sizeof t, "%s\n%s", g->game.title, _("Cargando..."));
    show_msg(g, t);

    emu_run_sound(emu_pref("sound", 1) != 0);
    emu_run_skip(emu_pref("skip", EMU_SKIP_AUTO));
    if (!emu_run_start(g->core, &g->game)) {
        g->failed = true;
        snprintf(t, sizeof t, "%s\n\n%s\n\n%s", g->game.title, emu_run_error(), _("Tocá para volver"));
        show_msg(g, t);
        return true;
    }
    aos_hal_log(EMU_TAG, "opening %s (%s)", g->game.rel, g->core->name);
    gestures_block(g, true);
    return true;
}

void emu_game_close(emu_app_t *a, bool save_auto)
{
    game_t *g = a->G;
    if (!g || !g->open) return;
    emu_menu_close(a);
    gestures_block(g, false);
    bool played = g->running;
    uint32_t secs = ((uint32_t)aos_hal_uptime_ms() - g->t_open) / 1000;
    emu_run_stop(save_auto);
    if (played) emu_recent_touch(&g->game, secs);
    if (g->scr) lv_obj_delete(g->scr);
    g->scr = NULL;
    g->cv = NULL;
    free(g->cv_buf);
    g->cv_buf = NULL;
    g->open = false;
    g->running = false;
    emu_lib_show(a);
}

bool emu_game_is_open(emu_app_t *a)
{
    return a->G && a->G->open;
}

void emu_game_resize(emu_app_t *a)
{
    game_t *g = a->G;
    if (!g || !g->open) return;
    touch_release(g);
    geometry(a, g);
    layout(g);
    pad_build(g);
    if (!g->running && !g->failed) show_msg(g, _("Cargando..."));
    for (int i = 0; i < FB_N; i++) g->fbr[i] = (fbrec_t){ 0 };
    emu_menu_resize(a);
}

bool emu_game_back(emu_app_t *a)
{
    game_t *g = a->G;
    if (!g || !g->open) return false;
    if (emu_menu_is_open(a)) {
        emu_menu_close(a);
        return true;
    }
    if (g->failed || !g->running) {
        emu_game_close(a, false);
        return true;
    }
    open_menu(a);
    return true;
}

void emu_game_destroy(emu_app_t *a)
{
    game_t *g = a->G;
    if (!g) return;
    if (g->open) {
        gestures_block(g, false);
        uint32_t secs = ((uint32_t)aos_hal_uptime_ms() - g->t_open) / 1000;
        bool played = g->running;
        emu_run_stop(true);
        if (played) emu_recent_touch(&g->game, secs);
    }
    free(g->cv_buf);
    lv_free(g);
    a->G = NULL;
}

/* Called by the app's timer. */
void emu_game_tick(emu_app_t *a)
{
    game_t *g = a->G;
    if (g && g->open) game_frame(a, g);
}

/* For the menu. */
const emu_game_t *emu_game_current(emu_app_t *a)
{
    return a->G && a->G->open ? &a->G->game : NULL;
}

void emu_game_resume(emu_app_t *a)
{
    game_t *g = a->G;
    if (!g || !g->open) return;
    touch_release(g);
    g->held = false;
    g->redo = true;
    emu_run_pause(false);
    gestures_block(g, true);
    /* a setting may have changed the picture or the pad */
    if (g->fps) {
        if (emu_pref("fps", 0)) lv_obj_remove_flag(g->fps, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g->fps, LV_OBJ_FLAG_HIDDEN);
    }
}

void emu_game_relayout(emu_app_t *a)
{
    game_t *g = a->G;
    if (!g || !g->open) return;
    layout(g);
    pad_build(g);
    for (int i = 0; i < FB_N; i++) g->fbr[i] = (fbrec_t){ 0 };
}
