/*
 * PINBALL - tables in 3D, upright: the app
 *
 * The tables are packs on the card (/apps/pinball_<id>_p4.pak, made by
 * tools/pack_p4.py from a table module and its Blender renders): the app
 * lists them in a carousel, and the one chosen is loaded whole, played,
 * and freed when the player goes back to choose another. A new table is a
 * new pack: nothing here knows any table by name.
 *
 * Everything the player sees once a table is chosen is that table's: the
 * DMD's colour, the launch and pause buttons, the panels' colours and the
 * table's logo on them, its sounds and music.
 *
 * Touch: the lower half of the screen is the flippers (left half, right
 * half; both at once); the launch button shows when a ball waits on the
 * plunger (a tap launches, held it charges); a quick swipe on the upper
 * table nudges it; the button in the DMD's corner pauses. A USB gamepad
 * plays it too (aos_pad.h): L or left, R or right are the flippers, A the
 * plunger, up the nudge, START the pause.
 *
 * Frames: the physics, the rules and the picture run in an LVGL timer;
 * only the rectangles that changed are drawn (pb_render.c), and on the
 * board they go out by page flipping, the rows each panel buffer lacks
 * copied in by the DMA (aos_hal_display_back / back_age / blit_into).
 * Files: pb_pak (the pack), pb_table (the definition), pb_phys, pb_game
 * (the rules), pb_dmd, pb_render, pb_audio.
 */
#include "pb_audio.h"
#include "pb_dmd.h"
#include "pb_game.h"
#include "pb_pak.h"
#include "pb_phys.h"
#include "pb_render.h"
#include "pb_table.h"

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CARDS   16
#define FRAME_MS    14
#define ZONE_Y      600             /* below this row the screen is the flippers */
#define THUMB_W     432
#define THUMB_H     768
#define NUDGE_PX    70
#define NUDGE_MS    320
#define PERF_MS     5000
#define HIST        8
#define ROW_TILE    8
#define ROW_WORDS   ((PB_SH / ROW_TILE + 31) / 32)

enum { ST_CAROUSEL, ST_LOADING, ST_PLAY };
enum { PN_NONE, PN_PAUSE, PN_OVER, PN_TUTORIAL, PN_SETTINGS };
enum { FR_NONE, FR_LEFT, FR_RIGHT, FR_LAUNCH, FR_PAUSE, FR_NUDGE };

typedef struct {
    char          path[160];
    pb_table_t   *head;
    uint8_t      *thumb;
    lv_image_dsc_t thumb_dsc;
    uint8_t      *logo;
    lv_image_dsc_t logo_dsc;
    uint8_t      *icon;
    lv_image_dsc_t icon_dsc;
    uint32_t      best;
} card_t;

typedef struct {
    bool     on;
    uint8_t  id;
    uint8_t  role;
    bool     nudged;
    int16_t  x0, y0, x, y;
    uint32_t t0;
} finger_t;

typedef struct {
    uint16_t *buf;
    uint32_t  flip;             /* our flip that showed it last, 0 none    */
} pbuf_t;

typedef struct {
    aos_app_t *self;
    lv_obj_t  *root;
    int        state;
    bool       sfx, music;
    lv_timer_t *timer, *load_timer;
    uint64_t   last_us;
    aos_pad_t  pad;
    aos_pad_menu_t menu;

    /* the carousel */
    card_t     card[MAX_CARDS];
    int        ncard, sel;
    lv_obj_t  *c_scroll, *c_name, *c_desc, *c_play, *c_play_icon, *c_dots, *c_gear, *c_title, *c_empty;
    lv_obj_t  *c_card[MAX_CARDS];

    /* the game */
    int        cur;             /* the card being played                   */
    pb_pak_t   pak;
    pb_table_t *t;
    pb_phys_t  ph;
    pb_game_t  g;
    pb_render_t rd;
    bool       loaded;
    lv_obj_t  *canvas;
    lv_obj_t  *panel;
    int        panel_kind;
    lv_obj_t  *loading;
    finger_t   fin[2];
    bool       held[2];
    bool       launch_down;
    float      charge;
    float      launch_held_s;
    bool       pad_launch;
    bool       over_seen;
    bool       toast_seen;
    bool       debug;
    bool       button_pause;
    bool       saved_best;
    bool       no_memory;

    /* page flipping */
    bool       flip_ok;
    uint32_t   flips;
    pbuf_t     pb[3];
    uint32_t   hist[HIST][ROW_WORDS];

    /* the log's line */
    uint32_t   perf_frames, perf_rows, perf_t0;
    uint64_t   perf_us, perf_draw_us, perf_push_us;
} app_t;

static app_t *s_app;

static void carousel_build(app_t *a);
static void carousel_free(app_t *a);
static void game_quit(app_t *a);
static void panel_close(app_t *a);
static void pause_show(app_t *a);
static void tutorial_show(app_t *a);
static void carousel_pad(app_t *a);

/* ---- small things ------------------------------------------------------------ */

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static uint32_t mix(uint32_t a, uint32_t b, int k)
{
    int r = (int)(((a >> 16) & 255) * (256 - k) + ((b >> 16) & 255) * k) >> 8;
    int g = (int)(((a >> 8) & 255) * (256 - k) + ((b >> 8) & 255) * k) >> 8;
    int bl = (int)((a & 255) * (256 - k) + (b & 255) * k) >> 8;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static const char *tname(const pb_table_t *t)
{
    int l = pb_lang();
    return t->name[l][0] ? t->name[l] : t->name[0];
}

static const char *tdesc(const pb_table_t *t)
{
    int l = pb_lang();
    return t->desc[l][0] ? t->desc[l] : t->desc[0];
}

static void pref_key(char *k, size_t n, const char *id)
{
    snprintf(k, n, "pb_b_%.9s", id);
}

static uint32_t best_get(const char *id)
{
    char k[16];
    int32_t v = 0;
    pref_key(k, sizeof k, id);
    if (!aos_hal_pref_get_i32(k, &v) || v < 0) v = 0;
    return (uint32_t)v;
}

static void best_set(const char *id, uint32_t v)
{
    char k[16];
    pref_key(k, sizeof k, id);
    aos_hal_pref_set_i32(k, (int32_t)(v > 0x7FFFFFFF ? 0x7FFFFFFF : v));
}

static bool pref_bool(const char *k, bool def)
{
    int32_t v;
    return aos_hal_pref_get_i32(k, &v) ? v != 0 : def;
}

/* an image of the pack as LVGL wants it: RGB565 or RGB565 + alpha plane */
static uint8_t *pak_image(pb_pak_t *pak, const char *name, lv_image_dsc_t *d)
{
    const pb_entry_t *e = pb_pak_find(pak, name);
    if (!e || (e->type != PK_RGB565 && e->type != PK_SPRITE)) return NULL;
    uint8_t *raw = (uint8_t *)pb_pak_load(pak, e);
    if (!raw) return NULL;
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = e->type == PK_SPRITE ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    d->header.w = e->w;
    d->header.h = e->h;
    d->header.stride = (uint32_t)e->w * 2;
    d->data = raw;
    d->data_size = e->rawlen;
    return raw;
}

static void style_button(lv_obj_t *b, const pb_table_t *t, bool primary)
{
    uint32_t acc = t ? t->c_accent : 0x0A84FF;
    uint32_t pan = t ? t->c_panel : 0x2C2C2E;
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, C(primary ? acc : mix(pan, 0xFFFFFF, 24)), 0);
    lv_obj_set_style_bg_grad_color(b, C(primary ? mix(acc, 0x000000, 90) : pan), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_border_color(b, C(primary ? mix(acc, 0xFFFFFF, 110) : acc), 0);
    lv_obj_set_style_shadow_width(b, primary ? 24 : 0, 0);
    lv_obj_set_style_shadow_color(b, C(acc), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_text_color(b, C(primary ? 0x000000 : (t ? t->c_text : 0xFFFFFF)), 0);
    if (primary) {
        /* dark text on a light accent, light on a dark one */
        int lum = (int)(((acc >> 16) & 255) * 3 + ((acc >> 8) & 255) * 6 + (acc & 255)) / 10;
        lv_obj_set_style_text_color(b, C(lum > 140 ? 0x101010 : 0xFFFFFF), 0);
    }
    lv_obj_set_style_text_font(b, aos_font_body, 0);
    lv_obj_set_style_bg_color(b, C(mix(primary ? acc : pan, 0xFFFFFF, 60)), LV_STATE_PRESSED);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, const pb_table_t *t, bool primary, int w, int h,
                        lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    style_button(b, t, primary);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* ---- the pack list --------------------------------------------------------------- */

typedef struct {
    app_t *a;
} scan_ctx_t;

static bool scan_cb(const aos_dir_entry_t *e, void *ctx)
{
    app_t *a = ((scan_ctx_t *)ctx)->a;
    size_t n = strlen(e->name);
    if (e->dir || e->name[0] == '.' || a->ncard >= MAX_CARDS) return true;
    if (strncmp(e->name, "pinball_", 8) || n < 16 || strcmp(e->name + n - 7, "_p4.pak")) return true;
    card_t *c = &a->card[a->ncard];
    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s/%s", aos_hal_path_apps(), e->name);
    a->ncard++;
    return true;
}

static int card_cmp(const void *x, const void *y)
{
    const card_t *a = (const card_t *)x, *b = (const card_t *)y;
    if (a->head->order != b->head->order) return a->head->order - b->head->order;
    return strcmp(a->head->id, b->head->id);
}

static void cards_load(app_t *a)
{
    a->ncard = 0;
    scan_ctx_t ctx = { a };
    if (aos_hal_path_apps()) aos_hal_dir_scan(aos_hal_path_apps(), scan_cb, &ctx);
    int lang = pb_lang();
    static const char *const LOGO[PB_LANGS] = { "logo_es", "logo_en", "logo_de" };
    int k = 0;
    for (int i = 0; i < a->ncard; i++) {
        card_t c = a->card[i];
        pb_pak_t pak;
        bool ok = false;
        if (pb_pak_open(&pak, c.path)) {
            const pb_entry_t *e = pb_pak_find(&pak, "table");
            char *txt = e ? (char *)pb_pak_load(&pak, e) : NULL;
            c.head = (pb_table_t *)pb_calloc(sizeof(pb_table_t));
            if (txt && c.head && pb_table_parse(c.head, txt, true)) {
                ok = true;
                c.thumb = pak_image(&pak, "thumb", &c.thumb_dsc);
                c.logo = pak_image(&pak, LOGO[lang], &c.logo_dsc);
                if (!c.logo) c.logo = pak_image(&pak, LOGO[0], &c.logo_dsc);
                c.icon = pak_image(&pak, "icon", &c.icon_dsc);
                c.best = best_get(c.head->id);
            }
            pb_free(txt);
            pb_pak_close(&pak);
        }
        if (ok) {
            a->card[k++] = c;
        } else {
            aos_hal_log("pinball", "%s is not a table", c.path);
            pb_free(c.head);
        }
    }
    a->ncard = k;
    for (int i = k; i < MAX_CARDS; i++) memset(&a->card[i], 0, sizeof a->card[i]);
    if (a->ncard > 1) qsort(a->card, (size_t)a->ncard, sizeof(card_t), card_cmp);
}

static void cards_free(app_t *a)
{
    for (int i = 0; i < a->ncard; i++) {
        card_t *c = &a->card[i];
        pb_free(c->head);
        pb_free(c->thumb);
        pb_free(c->logo);
        pb_free(c->icon);
        memset(c, 0, sizeof *c);
    }
    a->ncard = 0;
}

/* ---- page flipping and pushing ------------------------------------------------- */

static void rows_of(const pb_render_t *r, uint32_t *m)
{
    memset(m, 0, sizeof(uint32_t) * ROW_WORDS);
    for (int i = 0; i < r->ndirty; i++) {
        int t0 = r->dirty[i].y0 / ROW_TILE, t1 = (r->dirty[i].y1 - 1) / ROW_TILE;
        for (int k = t0; k <= t1 && k < PB_SH / ROW_TILE; k++) m[k >> 5] |= 1u << (k & 31);
    }
}

static pbuf_t *pbuf(app_t *a, uint16_t *b)
{
    for (int i = 0; i < 3; i++) {
        if (a->pb[i].buf == b) return &a->pb[i];
    }
    for (int i = 0; i < 3; i++) {
        if (!a->pb[i].buf) {
            a->pb[i].buf = b;
            a->pb[i].flip = 0;
            return &a->pb[i];
        }
    }
    return NULL;
}

/* copies the rows in m from the frame into the panel's buffer b */
static int copy_rows(app_t *a, uint16_t *b, const uint32_t *m)
{
    int rows = 0;
    int n = PB_SH / ROW_TILE;
    for (int k = 0; k < n;) {
        if (!(m[k >> 5] & (1u << (k & 31)))) {
            k++;
            continue;
        }
        int k0 = k;
        while (k < n && (m[k >> 5] & (1u << (k & 31)))) k++;
        int y = k0 * ROW_TILE, h = (k - k0) * ROW_TILE;
        rows += h;
        /* in pieces of 64 rows (90 KB): one DMA copy of a whole frame wants
         * more link-list items than the driver has, and fails loudly */
        while (h > 0) {
            int part = h > 64 ? 64 : h;
            if (!aos_hal_display_blit_into(b, 0, y, PB_SW, part, a->rd.fb + (size_t)y * PB_SW)) {
                for (int j = y; j < y + part; j++)
                    memcpy(b + (size_t)j * PB_SW, a->rd.fb + (size_t)j * PB_SW, PB_SW * 2);
            }
            y += part;
            h -= part;
        }
    }
    return rows;
}

static void push(app_t *a, bool toast)
{
    pb_render_t *r = &a->rd;
    if (!r->ndirty) return;
    uint32_t m[ROW_WORDS];
    rows_of(r, m);
    if (a->flip_ok && !toast) {
        uint16_t *b = aos_hal_display_back();
        if (b) {
            pbuf_t *p = pbuf(a, b);
            uint32_t since = 0;
            bool touched = true;
            bool known = p && p->flip && aos_hal_display_back_age(b, &since, &touched) && !touched &&
                         since != UINT32_MAX && since < HIST - 1 && since == a->flips - p->flip;
            uint32_t need[ROW_WORDS];
            if (!known) {
                memset(need, 0xFF, sizeof need);
            } else {
                memcpy(need, m, sizeof need);
                for (uint32_t f = p->flip + 1; f <= a->flips; f++) {
                    for (int w = 0; w < ROW_WORDS; w++) need[w] |= a->hist[f % HIST][w];
                }
            }
            a->perf_rows += (uint32_t)copy_rows(a, b, need);
            if (aos_hal_display_flip(b)) {
                a->flips++;
                memcpy(a->hist[a->flips % HIST], m, sizeof m);
                if (p) p->flip = a->flips;
                return;
            }
        }
    }
    /* no flipping: the rows to the buffer on screen, or the canvas */
    a->flips++;
    memset(a->hist[a->flips % HIST], 0xFF, sizeof a->hist[0]);
    bool blit = !toast;
    for (int i = 0; i < r->ndirty && blit; i++) {
        const pb_rect_t *d = &r->dirty[i];
        if (!aos_hal_display_blit_scaled(0, d->y0, PB_SW, d->y1 - d->y0, r->fb + (size_t)d->y0 * PB_SW, 1, false))
            blit = false;
        else
            a->perf_rows += (uint32_t)(d->y1 - d->y0);
    }
    if (!blit && a->canvas) {
        lv_area_t c;
        lv_obj_get_coords(a->canvas, &c);
        for (int i = 0; i < r->ndirty; i++) {
            const pb_rect_t *d = &r->dirty[i];
            lv_area_t ar = { c.x1 + d->x0, c.y1 + d->y0, c.x1 + d->x1 - 1, c.y1 + d->y1 - 1 };
            lv_obj_invalidate_area(a->canvas, &ar);
        }
    }
}

/* ---- touch and the gamepad, in a game --------------------------------------------- */

static void launch_now(app_t *a, float power)
{
    if (!pb_game_wants_launch(&a->g)) return;
    pb_phys_launch(&a->ph, power);
}

static void finger_down(app_t *a, finger_t *f, int x, int y)
{
    f->on = true;
    f->x0 = f->x = (int16_t)x;
    f->y0 = f->y = (int16_t)y;
    f->t0 = lv_tick_get();
    f->nudged = false;
    if (pb_render_inside(&a->rd.pause_rc, x, y)) {
        f->role = FR_PAUSE;
    } else if (pb_game_wants_launch(&a->g) && pb_render_inside(&a->rd.launch_rc, x, y)) {
        f->role = FR_LAUNCH;
        a->launch_down = true;
        a->charge = 0;
        a->launch_held_s = 0;
    } else if (y >= ZONE_Y) {
        f->role = x < PB_SW / 2 ? FR_LEFT : FR_RIGHT;
    } else if (y >= PB_TOP) {
        f->role = FR_NUDGE;
    } else {
        f->role = FR_NONE;
    }
}

static void finger_up(app_t *a, finger_t *f)
{
    if (!f->on) return;
    f->on = false;
    int dx = f->x - f->x0, dy = f->y - f->y0;
    if (f->role == FR_PAUSE && dx * dx + dy * dy < 50 * 50) {
        pause_show(a);
    } else if (f->role == FR_LAUNCH) {
        /* a tap is a full launch; held, the charge decides */
        float p = a->launch_held_s < 0.22f ? 0.88f : a->charge;
        a->launch_down = false;
        launch_now(a, p);
    }
}

static void nudge(app_t *a, float dx, float dy)
{
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1) return;
    if (pb_game_nudge(&a->g)) {
        pb_phys_nudge(&a->ph, dx / l * 420.0f, dy / l * 420.0f);
        pb_audio_play(SND_WALL, 200);
    }
}

static void input(app_t *a, float dt)
{
    aos_touch_point_t pt[2];
    aos_touch_points(pt);
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);
    bool seen[2] = { false, false };
    for (int s = 0; s < 2; s++) {
        if (!pt[s].down) continue;
        int x = (int)pt[s].x - rc.x1, y = (int)pt[s].y - rc.y1;
        int k = -1;
        for (int i = 0; i < 2; i++) {
            if (a->fin[i].on && a->fin[i].id == pt[s].id) k = i;
        }
        if (k < 0) {
            for (int i = 0; i < 2; i++) {
                if (!a->fin[i].on && !seen[i]) {
                    k = i;
                    break;
                }
            }
            if (k < 0) continue;
            a->fin[k].id = pt[s].id;
            finger_down(a, &a->fin[k], x, y);
        }
        seen[k] = true;
        finger_t *f = &a->fin[k];
        f->x = (int16_t)x;
        f->y = (int16_t)y;
        if (f->role == FR_NUDGE && !f->nudged) {
            int dx = f->x - f->x0, dy = f->y - f->y0;
            if (dx * dx + dy * dy > NUDGE_PX * NUDGE_PX) {
                f->nudged = true;
                if (lv_tick_get() - f->t0 < NUDGE_MS) nudge(a, (float)dx, (float)dy);
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        if (a->fin[i].on && !seen[i]) finger_up(a, &a->fin[i]);
    }

    aos_pad_t *p = &a->pad;
    bool held[2];
    held[0] = aos_pad_held(p, AOS_PAD_L | AOS_PAD_LEFT);
    held[1] = aos_pad_held(p, AOS_PAD_R | AOS_PAD_RIGHT);
    for (int i = 0; i < 2; i++) {
        if (a->fin[i].on && a->fin[i].role == FR_LEFT) held[0] = true;
        if (a->fin[i].on && a->fin[i].role == FR_RIGHT) held[1] = true;
    }
    for (int s = 0; s < 2; s++) {
        if (held[s] && !a->held[s]) pb_game_flipper(&a->g, s);
        a->held[s] = held[s];
        pb_phys_flipper(&a->ph, s, held[s]);
    }
    if (aos_pad_pressed(p, AOS_PAD_UP)) nudge(a, 0, -1);
    if (aos_pad_pressed(p, AOS_PAD_START) || a->button_pause) {
        a->button_pause = false;
        pause_show(a);
        return;
    }
    /* the plunger from the pad: A held charges, let go launches */
    if (pb_game_wants_launch(&a->g)) {
        if (aos_pad_pressed(p, AOS_PAD_A)) {
            a->pad_launch = true;
            a->launch_down = true;
            a->charge = 0;
            a->launch_held_s = 0;
        }
        if (a->pad_launch && aos_pad_held(p, AOS_PAD_A) == false) {
            a->pad_launch = false;
            a->launch_down = false;
            launch_now(a, a->launch_held_s < 0.22f ? 0.88f : a->charge);
        }
    } else {
        a->pad_launch = false;
        bool finger_launch = false;
        for (int i = 0; i < 2; i++) finger_launch |= a->fin[i].on && a->fin[i].role == FR_LAUNCH;
        if (!finger_launch) a->launch_down = false;
    }
    if (a->launch_down) {
        a->launch_held_s += dt;
        if (a->launch_held_s > 0.22f) {
            /* up and down while held, so the player picks a strength */
            float ph = (a->launch_held_s - 0.22f) / 1.1f;
            ph = ph - floorf(ph / 2.0f) * 2.0f;
            a->charge = ph < 1 ? ph : 2 - ph;
        }
    }
}

/* ---- panels over the game ------------------------------------------------------------ */

static lv_obj_t *panel_open(app_t *a, int kind, int w, int h)
{
    panel_close(a);
    const pb_table_t *t = a->t;
    lv_obj_t *dim = lv_obj_create(a->root);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim, C(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_50, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *p = lv_obj_create(dim);
    lv_obj_set_size(p, w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(p, h, 0);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, 36, 0);
    lv_obj_set_style_bg_color(p, C(t ? t->c_panel : 0x1C1C1E), 0);
    lv_obj_set_style_bg_grad_color(p, C(t ? mix(t->c_panel, 0x000000, 140) : 0x000000), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 4, 0);
    lv_obj_set_style_border_color(p, C(t ? t->c_accent : 0x0A84FF), 0);
    lv_obj_set_style_shadow_width(p, 60, 0);
    lv_obj_set_style_shadow_color(p, C(t ? t->c_accent : 0x0A84FF), 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(p, 28, 0);
    lv_obj_set_style_pad_row(p, 18, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    a->panel = dim;
    a->panel_kind = kind;
    pb_audio_pause(true);
    aos_pad_reset(&a->pad, lv_tick_get());
    return p;
}

static void panel_logo(app_t *a, lv_obj_t *p)
{
    card_t *c = a->cur >= 0 && a->cur < a->ncard ? &a->card[a->cur] : NULL;
    if (c && c->logo) {
        lv_obj_t *img = lv_image_create(p);
        lv_image_set_src(img, &c->logo_dsc);
        int w = (int)c->logo_dsc.header.w;
        if (w > 520) lv_image_set_scale(img, (uint32_t)(256 * 520 / w));
    } else if (a->t) {
        lv_obj_t *l = lv_label_create(p);
        lv_label_set_text(l, tname(a->t));
        lv_obj_set_style_text_font(l, aos_font_title, 0);
        lv_obj_set_style_text_color(l, C(a->t->c_accent), 0);
    }
}

static lv_obj_t *panel_label(app_t *a, lv_obj_t *p, const char *text, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, C(col ? col : (a->t ? a->t->c_text : 0xFFFFFF)), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static void panel_close(app_t *a)
{
    if (!a->panel) return;
    aos_pad_menu_clear(&a->menu);
    lv_obj_delete(a->panel);
    a->panel = NULL;
    a->panel_kind = PN_NONE;
    pb_audio_pause(false);
    aos_pad_reset(&a->pad, lv_tick_get());
    memset(a->fin, 0, sizeof a->fin);
    a->last_us = aos_hal_uptime_us();
    if (a->loaded) pb_render_full(&a->rd);
}

static void ev_resume(lv_event_t *e)
{
    panel_close((app_t *)lv_event_get_user_data(e));
}

static void new_game(app_t *a)
{
    panel_close(a);
    a->saved_best = false;
    pb_game_start(&a->g);
    pb_render_full(&a->rd);
}

static void ev_restart(lv_event_t *e)
{
    new_game((app_t *)lv_event_get_user_data(e));
}

static void ev_tables(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    panel_close(a);
    game_quit(a);
    carousel_build(a);
}

static void ev_tutorial(lv_event_t *e)
{
    tutorial_show((app_t *)lv_event_get_user_data(e));
}

static void ev_sfx(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->sfx = !a->sfx;
    aos_hal_pref_set_i32("pb_sfx", a->sfx);
    pb_audio_set(a->sfx, a->music);
    lv_obj_t *l = lv_obj_get_child(lv_event_get_target(e), 0);
    lv_label_set_text(l, a->sfx ? _("Efectos: sí") : _("Efectos: no"));
}

static void ev_music(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->music = !a->music;
    aos_hal_pref_set_i32("pb_mus", a->music);
    pb_audio_set(a->sfx, a->music);
    lv_obj_t *l = lv_obj_get_child(lv_event_get_target(e), 0);
    lv_label_set_text(l, a->music ? _("Música: sí") : _("Música: no"));
}

static void pad_menu_from(app_t *a, lv_obj_t *p)
{
    lv_obj_t *items[16];
    int n = 0;
    uint32_t cnt = lv_obj_get_child_count(p);
    for (uint32_t i = 0; i < cnt && n < 16; i++) {
        lv_obj_t *c = lv_obj_get_child(p, (int32_t)i);
        if (lv_obj_check_type(c, &lv_button_class)) items[n++] = c;
    }
    aos_pad_menu_set(&a->menu, items, n, 0);
}

static void pause_show(app_t *a)
{
    if (!a->loaded || a->panel) return;
    lv_obj_t *p = panel_open(a, PN_PAUSE, 600, 1000);
    panel_logo(a, p);
    panel_label(a, p, _("Pausa"), aos_font_title, 0);
    const pb_table_t *t = a->t;
    button(p, _("Seguir"), t, true, 480, 96, ev_resume, a);
    button(p, _("Empezar de nuevo"), t, false, 480, 84, ev_restart, a);
    button(p, _("Cómo se juega"), t, false, 480, 84, ev_tutorial, a);
    button(p, a->sfx ? _("Efectos: sí") : _("Efectos: no"), t, false, 480, 84, ev_sfx, a);
    button(p, a->music ? _("Música: sí") : _("Música: no"), t, false, 480, 84, ev_music, a);
    button(p, _("Elegir otra mesa"), t, false, 480, 84, ev_tables, a);
    pad_menu_from(a, p);
}

static void over_show(app_t *a)
{
    if (a->panel) return;
    lv_obj_t *p = panel_open(a, PN_OVER, 600, 860);
    panel_logo(a, p);
    panel_label(a, p, _("Fin del juego"), aos_font_title, 0);
    char num[24], line[64];
    dmd_number(num, sizeof num, a->g.score);
    panel_label(a, p, num, aos_font_huge, a->t->c_accent);
    if (a->g.new_best) {
        panel_label(a, p, _("¡Récord nuevo!"), aos_font_body, a->t->c_dmd);
    } else {
        dmd_number(num, sizeof num, a->g.best);
        snprintf(line, sizeof line, _("Récord: %s"), num);
        panel_label(a, p, line, aos_font_body, 0);
    }
    button(p, _("Otra vez"), a->t, true, 480, 96, ev_restart, a);
    button(p, _("Elegir otra mesa"), a->t, false, 480, 84, ev_tables, a);
    pad_menu_from(a, p);
}

/* the tutorial: the zones drawn over the table itself */
static lv_obj_t *zone(lv_obj_t *parent, int x, int y, int w, int h, uint32_t col, const char *text, bool round,
                      bool top)
{
    lv_obj_t *z = lv_obj_create(parent);
    lv_obj_remove_style_all(z);
    lv_obj_set_pos(z, x, y);
    lv_obj_set_size(z, w, h);
    lv_obj_set_style_radius(z, round ? LV_RADIUS_CIRCLE : 28, 0);
    lv_obj_set_style_border_width(z, 5, 0);
    lv_obj_set_style_border_color(z, C(col), 0);
    lv_obj_set_style_bg_color(z, C(col), 0);
    lv_obj_set_style_bg_opa(z, LV_OPA_20, 0);
    lv_obj_remove_flag(z, LV_OBJ_FLAG_CLICKABLE);
    if (text) {
        lv_obj_t *l = lv_label_create(z);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, C(0xFFFFFF), 0);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, w - 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        if (top) lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 50);
        else lv_obj_center(l);
    }
    return z;
}

static void ev_tut_done(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    aos_hal_pref_set_i32("pb_tut", 1);
    panel_close(a);
}

static void tutorial_show(app_t *a)
{
    if (!a->loaded) return;
    panel_close(a);
    const pb_table_t *t = a->t;
    lv_obj_t *dim = lv_obj_create(a->root);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim, C(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    a->panel = dim;
    a->panel_kind = PN_TUTORIAL;
    pb_audio_pause(true);
    aos_pad_reset(&a->pad, lv_tick_get());
    uint32_t acc = t->c_accent, alt = t->c_dmd;
    zone(dim, 10, ZONE_Y, PB_SW / 2 - 16, PB_SH - ZONE_Y - 10, acc, _("Tocá acá:\npaleta izquierda"), false, true);
    zone(dim, PB_SW / 2 + 6, ZONE_Y, PB_SW / 2 - 16, PB_SH - ZONE_Y - 10, acc, _("Tocá acá:\npaleta derecha"),
         false, true);
    zone(dim, 10, PB_TOP + 10, PB_SW - 20, ZONE_Y - PB_TOP - 30, alt,
         _("Deslizá rápido el dedo por acá para sacudir la mesa.\n¡Si la sacudís mucho, TILT!"), false, false);
    pb_rect_t lr = a->rd.launch_rc;
    lv_obj_t *lz = zone(dim, lr.x0, lr.y0, lr.x1 - lr.x0, lr.y1 - lr.y0, 0xFFFFFF, NULL, true, false);
    card_t *c = a->cur >= 0 ? &a->card[a->cur] : NULL;
    if (c && c->icon) {
        lv_obj_t *img = lv_image_create(lz);
        lv_image_set_src(img, &c->icon_dsc);
        lv_obj_center(img);
    }
    lv_obj_t *ll = lv_label_create(dim);
    lv_label_set_text(ll, _("Cuando aparece este botón, tocalo para lanzar la bola. Mantenelo apretado para elegir la fuerza."));
    lv_obj_set_style_text_color(ll, C(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ll, aos_font_small, 0);
    lv_obj_set_style_text_align(ll, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_color(ll, C(0x000000), 0);
    lv_obj_set_style_bg_opa(ll, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(ll, 10, 0);
    lv_obj_set_style_radius(ll, 12, 0);
    lv_obj_set_width(ll, 420);
    lv_label_set_long_mode(ll, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(ll, PB_SW - 430, lr.y0 - 200);
    pb_rect_t pr = a->rd.pause_rc;
    zone(dim, pr.x0 - 4, pr.y0 - 4, pr.x1 - pr.x0 + 8, pr.y1 - pr.y0 + 8, 0xFFFFFF, NULL, false, false);
    lv_obj_t *pl = lv_label_create(dim);
    lv_label_set_text(pl, _("Pausa"));
    lv_obj_set_style_text_color(pl, C(0xFFFFFF), 0);
    lv_obj_set_style_text_font(pl, aos_font_body, 0);
    lv_obj_set_pos(pl, pr.x1 + 16, (pr.y0 + pr.y1) / 2 - 16);
    lv_obj_t *b = button(dim, _("¡A jugar!"), t, true, 360, 96, ev_tut_done, a);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, ZONE_Y - 120);
    lv_obj_t *items[1] = { b };
    aos_pad_menu_set(&a->menu, items, 1, 0);
}

/* ---- the game --------------------------------------------------------------- */

static void game_quit(app_t *a)
{
    if (a->loaded && a->g.score > best_get(a->t->id)) best_set(a->t->id, a->g.score);
    pb_audio_close();
    pb_render_free(&a->rd);
    pb_phys_free(&a->ph);
    pb_pak_close(&a->pak);
    pb_free(a->t);
    a->t = NULL;
    a->loaded = false;
    if (a->canvas) {
        lv_obj_delete(a->canvas);
        a->canvas = NULL;
    }
    memset(a->pb, 0, sizeof a->pb);
    a->state = ST_CAROUSEL;
}

static void perf(app_t *a, uint64_t us)
{
    a->perf_frames++;
    a->perf_us += us;
    uint32_t now = lv_tick_get();
    if (!a->perf_t0) a->perf_t0 = now;
    if (now - a->perf_t0 < PERF_MS) return;
    float s = (float)(now - a->perf_t0) / 1000.0f;
    double n = a->perf_frames ? a->perf_frames : 1;
    aos_hal_log("pinball", "%.1f fps, %.2f ms a frame (draw %.2f, push %.2f), %u rows/frame pushed, flip %d, %u KB art",
                (double)(a->perf_frames / s), (double)(a->perf_us / 1000.0 / n), (double)(a->perf_draw_us / 1000.0 / n),
                (double)(a->perf_push_us / 1000.0 / n), (unsigned)(a->perf_rows / n), a->flip_ok,
                (unsigned)(a->rd.bytes / 1024));
    a->perf_frames = a->perf_rows = 0;
    a->perf_us = a->perf_draw_us = a->perf_push_us = 0;
    a->perf_t0 = now;
}

static void game_frame(app_t *a, float dt)
{
    uint32_t over = aos_ui_overlay();
    bool toast = (over & AOS_UI_OVER_TOAST) != 0;
    if (over & ~(uint32_t)AOS_UI_OVER_TOAST) {
        /* the system is over us: the game waits until it goes, and under a
         * panel, the switcher or the lock it waits paused (the zoom from
         * the icon and a banner only hold it) */
        const uint32_t PAUSING = AOS_UI_OVER_PANEL | AOS_UI_OVER_SWITCHER | AOS_UI_OVER_GESTURE |
                                 AOS_UI_OVER_NOT_FRONT | AOS_UI_OVER_LOCK;
        if ((over & PAUSING) && !a->panel && a->g.state == GS_PLAY) pause_show(a);
        a->over_seen = true;
        a->last_us = aos_hal_uptime_us();
        return;
    }
    if (a->over_seen || (a->toast_seen && !toast)) {
        a->over_seen = false;
        pb_render_full(&a->rd);
    }
    a->toast_seen = toast;
    if (a->panel) {
        if (aos_pad_pressed(&a->pad, AOS_PAD_B | AOS_PAD_START) && a->panel_kind == PN_PAUSE) {
            panel_close(a);
            return;
        }
        aos_pad_menu_step(&a->menu, &a->pad);
        pb_audio_pump();
        return;
    }
    uint64_t t0 = aos_hal_uptime_us();
    input(a, dt);
    if (a->panel) return;
    pb_phys_step(&a->ph, dt);
    pb_game_update(&a->g, dt);
    for (int i = 0; i < a->g.nsnd; i++) pb_audio_play(a->g.snd[i], 230);
    a->g.nsnd = 0;
    pb_audio_pump();
    if (a->g.state == GS_OVER) {
        if (!a->saved_best) {
            a->saved_best = true;
            if (a->g.score > best_get(a->t->id)) best_set(a->t->id, a->g.score);
            if (a->cur >= 0 && a->cur < a->ncard) a->card[a->cur].best = a->g.best;
        }
        if (a->g.end_t > 2.5f) over_show(a);
    }
    pb_ui_t ui = { 0 };
    ui.launch_show = pb_game_wants_launch(&a->g);
    ui.launch_down = a->launch_down && ui.launch_show;
    ui.charge = a->charge;
    ui.debug = a->debug;
    uint64_t t1 = aos_hal_uptime_us();
    pb_render_frame(&a->rd, &a->g, &a->ph, &ui);
    uint64_t t2 = aos_hal_uptime_us();
    push(a, toast);
    a->rd.ndirty = 0;
    uint64_t t3 = aos_hal_uptime_us();
    a->perf_draw_us += t2 - t1;
    a->perf_push_us += t3 - t2;
    perf(a, t3 - t0);
}

static void timer_cb(lv_timer_t *tm)
{
    app_t *a = (app_t *)lv_timer_get_user_data(tm);
    uint64_t now = aos_hal_uptime_us();
    float dt = a->last_us ? (float)(now - a->last_us) / 1e6f : 0.016f;
    a->last_us = now;
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0) dt = 0;
    aos_pad_update(&a->pad, lv_tick_get());
    if (a->state == ST_PLAY && a->loaded) game_frame(a, dt);
    else if (a->state == ST_CAROUSEL) carousel_pad(a);
}

static bool game_load(app_t *a, int idx)
{
    card_t *c = &a->card[idx];
    if (!pb_pak_open(&a->pak, c->path)) return false;
    const pb_entry_t *e = pb_pak_find(&a->pak, "table");
    char *txt = e ? (char *)pb_pak_load(&a->pak, e) : NULL;
    a->t = (pb_table_t *)pb_calloc(sizeof(pb_table_t));
    bool ok = txt && a->t && pb_table_parse(a->t, txt, false);
    pb_free(txt);
    if (!ok) goto fail;
    if (!pb_phys_init(&a->ph, a->t)) goto fail;
    if (!pb_render_init(&a->rd, a->t, &a->pak)) {
        pb_phys_free(&a->ph);
        goto fail;
    }
    if (a->pak.fails || !a->rd.nflip[0] || !a->rd.nflip[1]) {
        /* half a table is no table: everything it loaded goes back */
        aos_hal_log("pinball", "%s: %d entries did not fit in PSRAM", a->t->id, a->pak.fails);
        pb_render_free(&a->rd);
        pb_phys_free(&a->ph);
        a->no_memory = true;
        goto fail;
    }
    pb_audio_set(a->sfx, a->music);
    pb_audio_open(&a->pak);
    pb_game_init(&a->g, a->t, &a->ph, best_get(a->t->id));
    a->cur = idx;
    a->loaded = true;
    aos_hal_log("pinball", "%s loaded, %u KB of pictures", a->t->id, (unsigned)(a->rd.bytes / 1024));
    return true;
fail:
    aos_hal_log("pinball", "cannot load %s", c->path);
    pb_free(a->t);
    a->t = NULL;
    pb_pak_close(&a->pak);
    return false;
}

static void load_cb(lv_timer_t *tm)
{
    app_t *a = (app_t *)lv_timer_get_user_data(tm);
    lv_timer_delete(tm);
    a->load_timer = NULL;
    int idx = a->sel;
    /* the carousel's cards (4 MB) go before the table comes in */
    if (a->c_scroll) {
        /* everything but the loading screen */
        for (int32_t i = (int32_t)lv_obj_get_child_count(a->root) - 1; i >= 0; i--) {
            lv_obj_t *c = lv_obj_get_child(a->root, i);
            if (c != a->loading) lv_obj_delete(c);
        }
        a->c_scroll = NULL;
        a->c_play = NULL;
    }
    for (int i = 0; i < a->ncard; i++) {
        pb_free(a->card[i].thumb);
        a->card[i].thumb = NULL;
    }
    a->no_memory = false;
    if (!game_load(a, idx)) {
        a->state = ST_CAROUSEL;
        a->loading = NULL;
        carousel_build(a);
        aos_ui_toast(a->no_memory ? _("No hay memoria para la mesa: cerrá otras apps y probá de nuevo")
                                  : _("No se pudo cargar la mesa"), 4000);
        return;
    }
    if (a->loading) lv_obj_delete(a->loading);
    a->loading = NULL;
    lv_obj_set_style_bg_color(a->root, C(0x000000), 0);
    a->canvas = lv_canvas_create(a->root);
    lv_canvas_set_buffer(a->canvas, a->rd.fb, PB_SW, PB_SH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    a->flip_ok = aos_hal_display_back() != NULL;
    a->state = ST_PLAY;
    a->saved_best = false;
    pb_game_start(&a->g);
    pb_render_full(&a->rd);
    /* the first frame now, so whatever opens over it has the table under it */
    pb_ui_t ui = { 0 };
    pb_render_frame(&a->rd, &a->g, &a->ph, &ui);
    a->rd.ndirty = 0;
    pb_render_full(&a->rd);
    lv_obj_invalidate(a->canvas);
    a->last_us = aos_hal_uptime_us();
    if (!pref_bool("pb_tut", false)) tutorial_show(a);
}

static void play(app_t *a, int idx)
{
    if (a->state != ST_CAROUSEL || idx < 0 || idx >= a->ncard) return;
    a->sel = idx;
    a->state = ST_LOADING;
    const pb_table_t *t = a->card[idx].head;
    a->loading = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->loading);
    lv_obj_set_size(a->loading, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(a->loading, C(t->c_bg), 0);
    lv_obj_set_style_bg_opa(a->loading, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(a->loading);
    lv_label_set_text(l, _("Preparando la mesa..."));
    lv_obj_set_style_text_font(l, aos_font_title, 0);
    lv_obj_set_style_text_color(l, C(t->c_accent), 0);
    lv_obj_center(l);
    a->load_timer = lv_timer_create(load_cb, 60, a);
}

/* ---- the carousel -------------------------------------------------------------- */

static void carousel_theme(app_t *a)
{
    if (!a->ncard || !a->c_play) return;
    const pb_table_t *t = a->card[a->sel].head;
    lv_obj_set_style_bg_color(a->root, C(mix(t->c_bg, t->c_panel, 90)), 0);
    lv_obj_set_style_bg_grad_color(a->root, C(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(a->root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(a->root, LV_OPA_COVER, 0);
    lv_label_set_text(a->c_name, tname(t));
    lv_obj_set_style_text_color(a->c_name, C(t->c_accent), 0);
    char line[200], num[24];
    dmd_number(num, sizeof num, a->card[a->sel].best);
    if (a->card[a->sel].best) snprintf(line, sizeof line, "%s\n%s %s", tdesc(t), _("Récord:"), num);
    else snprintf(line, sizeof line, "%s", tdesc(t));
    lv_label_set_text(a->c_desc, line);
    lv_obj_set_style_text_color(a->c_desc, C(t->c_text), 0);
    style_button(a->c_play, t, true);
    if (a->c_play_icon) {
        if (a->card[a->sel].icon) {
            lv_image_set_src(a->c_play_icon, &a->card[a->sel].icon_dsc);
            lv_obj_remove_flag(a->c_play_icon, LV_OBJ_FLAG_HIDDEN);
            int w = (int)a->card[a->sel].icon_dsc.header.w;
            if (w > 0) lv_image_set_scale(a->c_play_icon, (uint32_t)(256 * 72 / w));
        } else {
            lv_obj_add_flag(a->c_play_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < a->ncard; i++) {
        lv_obj_t *d = lv_obj_get_child(a->c_dots, i);
        if (!d) continue;
        lv_obj_set_style_bg_color(d, C(i == a->sel ? t->c_accent : 0x606060), 0);
        lv_obj_set_size(d, i == a->sel ? 34 : 14, 14);
    }
}

static void ev_scroll_end(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->c_scroll) return;
    int32_t sx = lv_obj_get_scroll_x(a->c_scroll);
    int pitch = THUMB_W + 36;
    int i = (int)((sx + pitch / 2) / pitch);
    if (i < 0) i = 0;
    if (i >= a->ncard) i = a->ncard - 1;
    if (i != a->sel) {
        a->sel = i;
        carousel_theme(a);
    }
}

static void ev_card(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_target(e);
    for (int i = 0; i < a->ncard; i++) {
        if (a->c_card[i] == o) {
            if (i == a->sel) play(a, i);
            else lv_obj_scroll_to_view(o, LV_ANIM_ON);
        }
    }
}

static void ev_play(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    play(a, a->sel);
}

static void settings_close(app_t *a)
{
    if (a->panel) {
        lv_obj_delete(a->panel);
        a->panel = NULL;
        a->panel_kind = PN_NONE;
        aos_pad_reset(&a->pad, lv_tick_get());
        aos_pad_menu_clear(&a->menu);
    }
}

static void ev_set_close(lv_event_t *e)
{
    settings_close((app_t *)lv_event_get_user_data(e));
}

static void ev_set_tut(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    aos_hal_pref_set_i32("pb_tut", 0);
    aos_ui_toast(_("El tutorial sale en la próxima partida"), 2000);
    settings_close(a);
}

static void ev_set_reset(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    for (int i = 0; i < a->ncard; i++) {
        best_set(a->card[i].head->id, 0);
        a->card[i].best = 0;
    }
    aos_ui_toast(_("Récords borrados"), 1500);
    settings_close(a);
    carousel_theme(a);
}

static void ev_settings(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->panel || !a->ncard) return;
    pb_table_t *save = a->t;
    a->t = a->card[a->sel].head;
    lv_obj_t *p = panel_open(a, PN_SETTINGS, 600, 760);
    pb_audio_pause(false);
    panel_label(a, p, _("Ajustes"), aos_font_title, 0);
    button(p, a->sfx ? _("Efectos: sí") : _("Efectos: no"), a->t, false, 480, 84, ev_sfx, a);
    button(p, a->music ? _("Música: sí") : _("Música: no"), a->t, false, 480, 84, ev_music, a);
    button(p, _("Ver el tutorial otra vez"), a->t, false, 480, 84, ev_set_tut, a);
    button(p, _("Borrar los récords"), a->t, false, 480, 84, ev_set_reset, a);
    button(p, _("Listo"), a->t, true, 480, 96, ev_set_close, a);
    pad_menu_from(a, p);
    a->t = save;
}

static void carousel_free(app_t *a)
{
    aos_pad_menu_clear(&a->menu);
    a->c_scroll = NULL;
    a->c_play = NULL;
}

static void carousel_build(app_t *a)
{
    lv_obj_clean(a->root);
    a->panel = NULL;
    a->c_scroll = a->c_play = a->c_play_icon = NULL;
    a->state = ST_CAROUSEL;
    cards_free(a);
    cards_load(a);
    if (a->sel >= a->ncard) a->sel = 0;
    lv_obj_t *r = a->root;
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);

    a->c_title = lv_label_create(r);
    lv_label_set_text(a->c_title, _("Pinball 3D"));
    lv_obj_set_style_text_font(a->c_title, aos_font_title, 0);
    lv_obj_set_style_text_color(a->c_title, C(0xFFFFFF), 0);
    lv_obj_align(a->c_title, LV_ALIGN_TOP_MID, 0, 34);
    a->c_gear = lv_button_create(r);
    lv_obj_set_size(a->c_gear, 84, 84);
    lv_obj_align(a->c_gear, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_radius(a->c_gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(a->c_gear, C(0x303036), 0);
    lv_obj_t *gl = lv_label_create(a->c_gear);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gl, aos_font_title, 0);
    lv_obj_center(gl);
    lv_obj_add_event_cb(a->c_gear, ev_settings, LV_EVENT_CLICKED, a);

    if (!a->ncard) {
        a->c_empty = lv_label_create(r);
        lv_label_set_text(a->c_empty, _("No hay mesas en la tarjeta.\n\nCopiá los archivos pinball_*_p4.pak a la carpeta apps de la tarjeta."));
        lv_obj_set_style_text_font(a->c_empty, aos_font_body, 0);
        lv_obj_set_style_text_color(a->c_empty, C(0xC0C0C0), 0);
        lv_obj_set_style_text_align(a->c_empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(a->c_empty, 600);
        lv_label_set_long_mode(a->c_empty, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_center(a->c_empty);
        lv_obj_set_style_bg_color(r, C(0x101014), 0);
        return;
    }

    lv_obj_t *s = lv_obj_create(r);
    a->c_scroll = s;
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, PB_SW, THUMB_H + 40);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s, 36, 0);
    lv_obj_set_style_pad_left(s, (PB_SW - THUMB_W) / 2, 0);
    lv_obj_set_style_pad_right(s, (PB_SW - THUMB_W) / 2, 0);
    lv_obj_set_scroll_snap_x(s, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scroll_dir(s, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLL_ONE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s, ev_scroll_end, LV_EVENT_SCROLL_END, a);
    for (int i = 0; i < a->ncard; i++) {
        card_t *c = &a->card[i];
        lv_obj_t *k = lv_obj_create(s);
        a->c_card[i] = k;
        lv_obj_remove_style_all(k);
        lv_obj_set_size(k, THUMB_W, THUMB_H);
        lv_obj_set_style_radius(k, 30, 0);
        lv_obj_set_style_clip_corner(k, true, 0);
        lv_obj_set_style_bg_color(k, C(c->head->c_bg), 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(k, 4, 0);
        lv_obj_set_style_border_color(k, C(c->head->c_accent), 0);
        lv_obj_set_style_shadow_width(k, 40, 0);
        lv_obj_set_style_shadow_color(k, C(c->head->c_accent), 0);
        lv_obj_set_style_shadow_opa(k, LV_OPA_40, 0);
        lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(k, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(k, ev_card, LV_EVENT_SHORT_CLICKED, a);
        if (c->thumb) {
            lv_obj_t *img = lv_image_create(k);
            lv_image_set_src(img, &c->thumb_dsc);
            lv_obj_center(img);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        }
        if (c->logo) {
            lv_obj_t *img = lv_image_create(k);
            lv_image_set_src(img, &c->logo_dsc);
            int w = (int)c->logo_dsc.header.w;
            if (w > THUMB_W - 24) lv_image_set_scale(img, (uint32_t)(256 * (THUMB_W - 24) / w));
            lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 120);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t *l = lv_label_create(k);
            lv_label_set_text(l, tname(c->head));
            lv_obj_set_style_text_font(l, aos_font_title, 0);
            lv_obj_set_style_text_color(l, C(c->head->c_accent), 0);
            lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 160);
        }
    }

    a->c_dots = lv_obj_create(r);
    lv_obj_remove_style_all(a->c_dots);
    lv_obj_set_size(a->c_dots, PB_SW, 24);
    lv_obj_align(a->c_dots, LV_ALIGN_TOP_MID, 0, 130 + THUMB_H + 50);
    lv_obj_set_flex_flow(a->c_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->c_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(a->c_dots, 12, 0);
    for (int i = 0; i < a->ncard; i++) {
        lv_obj_t *d = lv_obj_create(a->c_dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 14, 14);
        lv_obj_set_style_radius(d, 7, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }
    a->c_name = lv_label_create(r);
    lv_obj_set_style_text_font(a->c_name, aos_font_title, 0);
    lv_obj_align(a->c_name, LV_ALIGN_TOP_MID, 0, 130 + THUMB_H + 86);
    a->c_desc = lv_label_create(r);
    lv_obj_set_style_text_font(a->c_desc, aos_font_small, 0);
    lv_obj_set_style_text_align(a->c_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(a->c_desc, 640);
    lv_label_set_long_mode(a->c_desc, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(a->c_desc, LV_ALIGN_TOP_MID, 0, 130 + THUMB_H + 136);
    a->c_play = lv_button_create(r);
    lv_obj_set_size(a->c_play, 420, 104);
    lv_obj_align(a->c_play, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_flex_flow(a->c_play, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->c_play, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(a->c_play, 16, 0);
    a->c_play_icon = lv_image_create(a->c_play);
    lv_obj_t *pl = lv_label_create(a->c_play);
    lv_label_set_text(pl, _("Jugar"));
    lv_obj_add_event_cb(a->c_play, ev_play, LV_EVENT_CLICKED, a);
    lv_obj_add_flag(a->c_play, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    carousel_theme(a);
    lv_obj_update_layout(s);
    lv_obj_scroll_to_view(a->c_card[a->sel], LV_ANIM_OFF);
    aos_pad_menu_clear(&a->menu);
}

static void carousel_pad(app_t *a)
{
    aos_pad_t *p = &a->pad;
    if (a->panel) {
        if (aos_pad_pressed(p, AOS_PAD_B)) settings_close(a);
        else aos_pad_menu_step(&a->menu, p);
        return;
    }
    if (!a->ncard || !a->c_scroll) return;
    int d = aos_pad_repeat(p, AOS_PAD_LEFT | AOS_PAD_L) ? -1 : aos_pad_repeat(p, AOS_PAD_RIGHT | AOS_PAD_R) ? 1 : 0;
    if (d) {
        int i = a->sel + d;
        if (i >= 0 && i < a->ncard) {
            a->sel = i;
            lv_obj_scroll_to_view(a->c_card[i], LV_ANIM_ON);
            carousel_theme(a);
        }
    }
    if (aos_pad_pressed(p, AOS_PAD_START | AOS_PAD_A)) {
        play(a, a->sel);
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_UP)) {
        /* the gear: the settings */
        lv_obj_send_event(a->c_gear, LV_EVENT_CLICKED, NULL);
    }
}

/* ---- life cycle ------------------------------------------------------------------ */

static void *pinball_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)pb_calloc(sizeof(app_t));
    if (!a) return NULL;
    s_app = a;
    a->self = self;
    a->root = root;
    a->cur = -1;
    a->sfx = pref_bool("pb_sfx", true);
    a->music = pref_bool("pb_mus", true);
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    a->debug = getenv("PB_DEBUG") != NULL;
#endif
    lv_obj_set_style_bg_color(root, C(0x000000), 0);
    carousel_build(a);
    a->timer = lv_timer_create(timer_cb, FRAME_MS, a);
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    /* PB_TABLE=<n> opens that table straight away (scripts) */
    const char *auto_t = getenv("PB_TABLE");
    if (auto_t && a->ncard) {
        if (getenv("PB_NOTUT")) aos_hal_pref_set_i32("pb_tut", 1);
        play(a, atoi(auto_t) % a->ncard);
    }
#endif
    return a;
}

static void pinball_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    if (a->load_timer) lv_timer_delete(a->load_timer);
    a->timer = a->load_timer = NULL;
    game_quit(a);
    cards_free(a);
    carousel_free(a);
    if (s_app == a) s_app = NULL;
    pb_free(a);
}

static void pinball_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a && a->state == ST_PLAY && !a->panel) pause_show(a);
    pb_audio_pause(true);
}

static bool pinball_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    if (a->state == ST_LOADING) return true;
    if (a->state == ST_CAROUSEL) {
        if (a->panel) {
            settings_close(a);
            return true;
        }
        return false;
    }
    if (a->panel_kind == PN_PAUSE) panel_close(a);
    else if (!a->panel) pause_show(a);
    return true;
}

static bool pinball_button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || a->state != ST_PLAY || action != AOS_BUTTON_CLICK) return false;
    a->button_pause = true;
    return true;
}

/* a ball between two flippers, under two bumpers */
static const uint8_t PINBALL_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -20, -24, 24, 24, AIC_CIRCLE, AIC_C_LIT(0xFF3D6E), 255),
    AIC_BORDER(3, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 22, -30, 18, 18, AIC_CIRCLE, AIC_C_LIT(0x38D8FF), 255),
    AIC_BORDER(3, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -17, 27, 34, 11, 6, AIC_C_LIT(0xFFD23C), 255),
    AIC_ROT(220),
    AIC_RECT(AIC_CENTER, 17, 27, 34, 11, 6, AIC_C_LIT(0xFFD23C), 255),
    AIC_ROT(-220),
    AIC_RECT(AIC_CENTER, 3, 3, 18, 18, AIC_CIRCLE, AIC_C_LIT(0xF0F2F8), 255),
    AIC_GRAD(AIC_C_LIT(0x6A7280), AIC_GRAD_VER),
    AIC_END
};

static bool pinball_init(aos_app_t *app)
{
    app->desc.id       = "demo.pinball";
    app->desc.name     = "Pinball 3D";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x5A1E8C;
    app->desc.color_b  = 0x10082A;
    app->desc.order    = 158;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_PORTRAIT |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    aos_icon_set_ops(app, PINBALL_ICON, sizeof PINBALL_ICON);
    app->create  = pinball_create;
    app->destroy = pinball_destroy;
    app->hide    = pinball_hide;
    app->back    = pinball_back;
    app->button  = pinball_button;
    return true;
}

AOS_APP_ENTRY(pinball_init);
