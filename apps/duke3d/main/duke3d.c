/*
 * P4OS - Duke Nukem 3D.
 *
 * Chocolate Duke3D (the Build engine and the game, port/ for the board),
 * running in the app's worker on core 0. This file is only the OS's side:
 * the picture, blitted from LVGL's task, and the pad around it. It is
 * Doom's app (apps/doom/main/doom.c) with Duke's buttons and a picture
 * whose size follows the game's video mode.
 *
 * Always in landscape: AOS_APP_FLAG_LANDSCAPE makes the runtime turn the
 * screen while Duke is in front and back when it leaves. On the 1280x720
 * screen:
 *
 *     x  160..1119, y 60..659   the picture: a 16:10 mode (320x200,
 *                               640x400...) fills the 960x600 box, a 4:3
 *                               one (640x480) is 800x600 in its middle.
 *                               The PPA scales and turns it. Pressing it
 *                               fires, and so do the strips around it.
 *     x    0..159               MENU, MAP, ITEM and NEXT at the top, then
 *                               the stick for the left thumb, which appears
 *                               where the thumb lands.
 *     x 1120..1279              WEAPON and STRAFE at the top, then JUMP,
 *                               USE, FIRE and CROUCH for the right thumb.
 *
 * Two fingers (aos_touch_points): each finger owns whatever it landed on
 * until it lifts - the stick, a button or the picture - so the left thumb
 * walks while the right one shoots or jumps. The stick and FIRE sit level
 * with each other, where the thumbs rest.
 *
 * Nothing LVGL draws may overlap the picture's box: LVGL's next redraw over
 * it wins until the next frame, which would be a flicker.
 *
 * The picture goes up by page flipping, exactly as Doom's (present_flip):
 * the PPA scales it into the panel's free buffer, the pad is brought up to
 * date in that buffer from the one on screen, and the buffer is flipped to.
 * Under a panel, the switcher, a banner, the zoom or a gesture the game
 * pauses; under a toast it plays on.
 *
 * A USB gamepad plays too (aos_pad.h), through the same buttons: the stick
 * or the d-pad walks and turns (the arrows in a menu), A fires (Enter in a
 * menu), B uses (back), L jumps, R crouches, L and R together change
 * weapon, START is MENU and, held, leaves.
 *
 * The GRP is not in the app: the card's duke3d/ folder holds DUKE3D.GRP,
 * the shareware one or the full game's (port/dk_system.c). The config and
 * the saves go in the same folder.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_gesture.h"
#include "aos_pad.h"
#include "lvgl.h"

#include "duke_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS     8
#define BOX_W       960             /* the picture's box */
#define BOX_H       600

/* The CPU clock the mixer's cycles are divided by, for the log: the rev1.3
 * board runs at 360 MHz (a rev3 chip would say 400). */
#define CPU_MHZ     360

/* The stick, in the left band, below the four small buttons. */
#define STICK_TOP       136         /* above: MENU, MAP, ITEM, NEXT           */
#define STICK_R         60          /* the thumb this far away: full push     */
#define STICK_BASE      70          /* the base's radius, drawn               */
#define STICK_KNOB      28
#define STICK_TRAVEL    (STICK_BASE - STICK_KNOB - 2)
#define STICK_REST_Y    470         /* level with FIRE                        */
#define EDGE_KEEP       8

#define MIN_PRESS_MS    70          /* a quick tap still lasts two game tics  */
#define MENU_HOLD_MS    2000        /* MENU held this long: out of Duke       */

#define PAD_SETTLE_MS   60
#define FB_N            3           /* the panel's buffers */

typedef struct {
    int x, y, w, h;                 /* screen box */
    bool round;
} box_t;

typedef struct {
    int x, y, w, h;
} rect_t;

typedef struct {
    uint16_t   *fb;
    bool        whole;
    bool        dirty[2];
    rect_t      d[2];
} fbrec_t;

static struct {
    int   w, h;                     /* the screen                             */
    int   bx, by;                   /* the picture box's top-left corner      */
    int   band;                     /* each side band's width                 */
    bool  ok;                       /* landscape, and the box fits            */
    box_t box[DK_BTN_N];
} L;

/* The right band, top to bottom: a finger belongs to the row it lands in. */
static const int RIGHT_ROWS[] = {
    DK_BTN_WEAPON, DK_BTN_STRAFE, DK_BTN_JUMP, DK_BTN_USE, DK_BTN_FIRE, DK_BTN_CROUCH,
};
#define RIGHT_N ((int)(sizeof RIGHT_ROWS / sizeof RIGHT_ROWS[0]))

enum {
    OWN_NONE = -1,
    OWN_STICK = 100,
    OWN_PICTURE = 101,
};

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    lv_obj_t   *touch;
    lv_obj_t   *msg;
    lv_obj_t   *btn[DK_BTN_N];
    lv_obj_t   *base, *knob;
    lv_timer_t *timer;

    bool        running;
    bool        sound;
    bool        shown_any;
    bool        want_exit;
    bool        exit_on_tap;
    bool        blocked;
    bool        paused_msg;

    /* the picture as last presented */
    int         fw, fh;             /* the frame */
    rect_t      pic;                /* where it goes on the screen */

    int         cx, cy;
    int         sbx, sby;           /* the stick's base, drawn */

    uint8_t     fid[2];
    int         fown[2];
    uint32_t    fdown[2];
    bool        strafe_on;
    bool        menu_tap;
    bool        btn_on[DK_BTN_ALL];
    uint32_t    btn_ms[DK_BTN_ALL];

    aos_pad_t   pad;
    uint32_t    pad_want;
    uint32_t    pad_start_ms;
    bool        pad_stick;

    int         dbx, dby;

    fbrec_t     fbr[FB_N];
    uint32_t    pad_ms[2];
    bool        pad_refr;
    bool        redo;
    uint32_t    over;
    bool        held;

    uint32_t    fps_ms, fps_frames, fps_blits, blits;
    uint32_t    cyc_music, cyc_total;
    uint64_t    cost_us;
    uint32_t    cost_n;
    uint64_t    blit_us;
    uint32_t    blit_n;
    uint32_t    flips, directs, wholes;
    uint64_t    pad_us;
    uint32_t    pad_kb;
} app_t;

/* --------------------------------------------------------------------------
 * The layout
 * -------------------------------------------------------------------------- */

static void layout_compute(void)
{
    L.w = AOS_SCREEN_W;
    L.h = AOS_SCREEN_H;
    L.band = (L.w - BOX_W) / 2;
    L.bx = L.band;
    L.by = (L.h - BOX_H) / 2;
    L.ok = aos_hal_landscape() && L.band >= 2 * STICK_BASE && L.by >= 0;

    int half = (L.band - 28) / 2;           /* two small buttons side by side */
    int rx = L.w - L.band;
    int rc = rx + L.band / 2;
    int bw = L.band - 24;
    L.box[DK_BTN_MENU]     = (box_t){ 12,            16, half, 48, false };
    L.box[DK_BTN_MAP]      = (box_t){ 16 + half,     16, half, 48, false };
    L.box[DK_BTN_INV]      = (box_t){ 12,            72, half, 48, false };
    L.box[DK_BTN_INV_NEXT] = (box_t){ 16 + half,     72, half, 48, false };
    L.box[DK_BTN_WEAPON]   = (box_t){ rx + 12,       16, bw, 48, false };
    L.box[DK_BTN_STRAFE]   = (box_t){ rx + 12,       72, bw, 48, false };
    L.box[DK_BTN_JUMP]     = (box_t){ rc - 46,      140, 92, 92, true };
    L.box[DK_BTN_USE]      = (box_t){ rc - 46,      250, 92, 92, true };
    L.box[DK_BTN_FIRE]     = (box_t){ rc - 68, STICK_REST_Y - 68, 136, 136, true };
    L.box[DK_BTN_CROUCH]   = (box_t){ rc - 50,      566, 100, 100, true };
}

static int right_row(int y)
{
    for (int i = 0; i < RIGHT_N - 1; i++) {
        const box_t *b = &L.box[RIGHT_ROWS[i]], *n = &L.box[RIGHT_ROWS[i + 1]];
        if (y < (b->y + b->h + n->y) / 2) return RIGHT_ROWS[i];
    }
    return RIGHT_ROWS[RIGHT_N - 1];
}

/* The four small buttons of the left band, by where the finger landed. */
static int left_small(int x, int y)
{
    bool right = x >= L.box[DK_BTN_MAP].x - 2;
    if (y < (L.box[DK_BTN_MENU].y + L.box[DK_BTN_MENU].h + L.box[DK_BTN_INV].y) / 2)
        return right ? DK_BTN_MAP : DK_BTN_MENU;
    return right ? DK_BTN_INV_NEXT : DK_BTN_INV;
}

/* Where a frame of w x h goes: its shape kept, as big as the box allows. */
static rect_t pic_rect(int w, int h)
{
    int pw = BOX_W, ph = BOX_W * h / w;
    if (ph > BOX_H) {
        ph = BOX_H;
        pw = BOX_H * w / h;
    }
    pw &= ~1;
    return (rect_t){ L.bx + (BOX_W - pw) / 2, L.by + (BOX_H - ph) / 2, pw, ph };
}

/* --------------------------------------------------------------------------
 * The pad
 * -------------------------------------------------------------------------- */

static lv_obj_t *pad_button(lv_obj_t *root, const char *text)
{
    lv_obj_t *o = lv_obj_create(root);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x2A2208), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0x9A7A18), 0);
    lv_obj_set_style_border_width(o, 3, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xF8E8B0), 0);
    lv_obj_set_style_text_font(l, aos_font_caption, 0);
    lv_obj_center(l);
    return o;
}

static void pad_mark(app_t *a, int x, int y, int w, int h)
{
    int band = x + w / 2 < L.w / 2 ? 0 : 1;
    int bx0 = band ? L.w - L.band : 0, bx1 = band ? L.w : L.band;
    x -= 2; y -= 2; w += 4; h += 4;
    if (x < bx0) { w -= bx0 - x; x = bx0; }
    if (x + w > bx1) w = bx1 - x;
    if (y < 0) { h += y; y = 0; }
    if (y + h > L.h) h = L.h - y;
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < FB_N; i++) {
        fbrec_t *r = &a->fbr[i];
        if (!r->dirty[band]) {
            r->d[band] = (rect_t){ x, y, w, h };
            r->dirty[band] = true;
            continue;
        }
        rect_t *d = &r->d[band];
        int x1 = d->x + d->w > x + w ? d->x + d->w : x + w;
        int y1 = d->y + d->h > y + h ? d->y + d->h : y + h;
        d->x = d->x < x ? d->x : x;
        d->y = d->y < y ? d->y : y;
        d->w = x1 - d->x;
        d->h = y1 - d->y;
    }
    a->pad_ms[band] = (uint32_t)aos_hal_uptime_ms();
    a->pad_refr = true;
}

static void pad_lit(app_t *a, int btn, bool on)
{
    if (btn < 0 || btn >= DK_BTN_N || !a->btn[btn]) return;
    lv_obj_set_style_bg_color(a->btn[btn], lv_color_hex(on ? 0xC08A10 : 0x2A2208), 0);
    const box_t *b = &L.box[btn];
    pad_mark(a, b->x, b->y, b->w, b->h);
}

static void stick_draw(app_t *a, int kx, int ky)
{
    if (a->dbx || a->dby) pad_mark(a, a->dbx - STICK_BASE, a->dby - STICK_BASE, 2 * STICK_BASE, 2 * STICK_BASE);
    if (a->dbx != a->sbx || a->dby != a->sby) pad_mark(a, a->sbx - STICK_BASE, a->sby - STICK_BASE, 2 * STICK_BASE, 2 * STICK_BASE);
    a->dbx = a->sbx;
    a->dby = a->sby;
    lv_obj_set_pos(a->base, a->sbx - STICK_BASE, a->sby - STICK_BASE);
    lv_obj_set_pos(a->knob, a->sbx + kx - STICK_KNOB, a->sby + ky - STICK_KNOB);
}

static void stick_show(app_t *a, bool on)
{
    a->sbx = L.band / 2;
    if (on) {
        int lo = STICK_TOP + 4 + STICK_BASE;
        int hi = L.h - EDGE_KEEP - STICK_BASE;
        a->sby = a->cy < lo ? lo : a->cy > hi ? hi : a->cy;
    } else {
        a->sby = STICK_REST_Y;
    }
    stick_draw(a, 0, 0);
}

static void stick_update(app_t *a, int x, int y)
{
    int dx = x - a->cx, dy = y - a->cy;
    int d2 = dx * dx + dy * dy;
    if (d2 > STICK_R * STICK_R) {
        int d = 1;
        while (d * d < d2) d++;
        dx = dx * STICK_R / d;
        dy = dy * STICK_R / d;
    }
    stick_draw(a, dx * STICK_TRAVEL / STICK_R, dy * STICK_TRAVEL / STICK_R);
    dk_stick(dx * 100 / STICK_R, dy * 100 / STICK_R);
}

static void pad_place(app_t *a)
{
    for (int i = 0; i < DK_BTN_N; i++) {
        const box_t *b = &L.box[i];
        if (!a->btn[i]) continue;
        lv_obj_set_pos(a->btn[i], b->x, b->y);
        lv_obj_set_size(a->btn[i], b->w, b->h);
        lv_obj_set_style_radius(a->btn[i], b->round ? LV_RADIUS_CIRCLE : 10, 0);
    }
    stick_show(a, false);
    a->redo = true;
    lv_obj_set_width(a->msg, BOX_W - 80);
    lv_obj_set_pos(a->msg, L.bx + 40, L.by + 60);
    lv_obj_set_size(a->touch, L.w, L.h);
}

static void pad_visible(app_t *a, bool on)
{
    lv_obj_t *objs[DK_BTN_N + 2];
    int n = 0;
    for (int i = 0; i < DK_BTN_N; i++) objs[n++] = a->btn[i];
    objs[n++] = a->base;
    objs[n++] = a->knob;
    for (int i = 0; i < n; i++) {
        if (!objs[i]) continue;
        if (on) lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    a->redo = true;
}

static void build_pad(app_t *a)
{
    lv_obj_t *r = a->root;
    a->base = lv_obj_create(r);
    lv_obj_remove_style_all(a->base);
    lv_obj_set_size(a->base, STICK_BASE * 2, STICK_BASE * 2);
    lv_obj_set_style_radius(a->base, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(a->base, lv_color_hex(0x9A7A18), 0);
    lv_obj_set_style_border_width(a->base, 3, 0);
    lv_obj_set_style_bg_color(a->base, lv_color_hex(0x181404), 0);
    lv_obj_set_style_bg_opa(a->base, LV_OPA_COVER, 0);
    lv_obj_remove_flag(a->base, LV_OBJ_FLAG_CLICKABLE);

    a->knob = lv_obj_create(r);
    lv_obj_remove_style_all(a->knob);
    lv_obj_set_size(a->knob, STICK_KNOB * 2, STICK_KNOB * 2);
    lv_obj_set_style_radius(a->knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(a->knob, lv_color_hex(0xC09020), 0);
    lv_obj_set_style_bg_opa(a->knob, LV_OPA_COVER, 0);
    lv_obj_remove_flag(a->knob, LV_OBJ_FLAG_CLICKABLE);

    a->btn[DK_BTN_FIRE]     = pad_button(r, _("FUEGO"));
    a->btn[DK_BTN_USE]      = pad_button(r, _("USAR"));
    a->btn[DK_BTN_JUMP]     = pad_button(r, _("SALTAR"));
    a->btn[DK_BTN_CROUCH]   = pad_button(r, _("AGACHAR"));
    a->btn[DK_BTN_MENU]     = pad_button(r, _("MENÚ"));
    a->btn[DK_BTN_MAP]      = pad_button(r, _("MAPA"));
    a->btn[DK_BTN_INV]      = pad_button(r, _("ÍTEM"));
    a->btn[DK_BTN_INV_NEXT] = pad_button(r, _("OTRO"));
    a->btn[DK_BTN_WEAPON]   = pad_button(r, _("ARMA"));
    a->btn[DK_BTN_STRAFE]   = pad_button(r, _("LATERAL"));
}

/* --------------------------------------------------------------------------
 * Two fingers
 * -------------------------------------------------------------------------- */

static int claim(app_t *a, int x, int y, int other_owner)
{
    if (x >= L.bx && x < L.bx + BOX_W) {
        return OWN_PICTURE;
    }
    if (x >= L.bx + BOX_W) {
        int b = right_row(y);
        if (b == DK_BTN_STRAFE) a->strafe_on = !a->strafe_on;
        return b;
    }
    if (y < STICK_TOP) {
        return left_small(x, y);
    }
    if (other_owner == OWN_STICK) {
        return OWN_NONE;
    }
    a->cx = x;
    a->cy = y;
    stick_show(a, true);
    return OWN_STICK;
}

static void pad_apply(app_t *a)
{
    bool want[DK_BTN_ALL] = { false };
    for (int b = 0; b < DK_BTN_ALL; b++) want[b] = (a->pad_want >> b) & 1u;
    for (int i = 0; i < 2; i++) {
        if (a->fown[i] == OWN_PICTURE) {
            want[DK_BTN_FIRE] = true;
        } else if (a->fown[i] >= 0 && a->fown[i] < DK_BTN_N) {
            want[a->fown[i]] = true;
        }
    }
    want[DK_BTN_STRAFE] = a->strafe_on;
    if (a->menu_tap) {
        a->menu_tap = false;
        want[DK_BTN_MENU] = true;
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int b = 0; b < DK_BTN_ALL; b++) {
        if (!want[b] && a->btn_on[b] && now - a->btn_ms[b] < MIN_PRESS_MS) {
            want[b] = true;
        }
        if (want[b] != a->btn_on[b]) {
            a->btn_on[b] = want[b];
            if (want[b]) a->btn_ms[b] = now;
            dk_button(b, want[b]);
            pad_lit(a, b, want[b]);
        }
    }
}

static void pad_release_all(app_t *a)
{
    for (int i = 0; i < 2; i++) {
        if (a->fown[i] == OWN_STICK) {
            dk_stick(0, 0);
            stick_show(a, false);
        }
        a->fown[i] = OWN_NONE;
        a->fid[i] = 0;
    }
    a->strafe_on = false;
    a->pad_want = 0;
    if (a->pad_stick) {
        dk_stick(0, 0);
        a->pad_stick = false;
    }
    pad_apply(a);
}

static void gamepad_poll(app_t *a, uint32_t now)
{
    aos_pad_update(&a->pad, lv_tick_get());
    const aos_pad_t *p = &a->pad;
    uint32_t w = 0;
    if (p->held & AOS_PAD_A) w |= 1u << DK_BTN_FIRE;
    if (p->held & AOS_PAD_B) w |= 1u << DK_BTN_USE;
    if (p->held & AOS_PAD_START) w |= 1u << DK_BTN_MENU;
    bool l = p->held & AOS_PAD_L, r = p->held & AOS_PAD_R;
    if (l && r) w |= 1u << DK_BTN_WEAPON;
    else if (l) w |= 1u << DK_BTN_JUMP;
    else if (r) w |= 1u << DK_BTN_CROUCH;
    a->pad_want = w;

    if (p->pressed & AOS_PAD_START) a->pad_start_ms = now;
    if ((p->held & AOS_PAD_START) && now - a->pad_start_ms >= MENU_HOLD_MS) a->want_exit = true;

    if (a->fown[0] == OWN_STICK || a->fown[1] == OWN_STICK) return;
    int x = p->x * 100 / 32767, y = p->y * 100 / 32767;
    if (x > -8 && x < 8) x = 0;
    if (y > -8 && y < 8) y = 0;
    if (p->held & AOS_PAD_LEFT) x = -100;
    if (p->held & AOS_PAD_RIGHT) x = 100;
    if (p->held & AOS_PAD_UP) y = -100;
    if (p->held & AOS_PAD_DOWN) y = 100;
    if (x || y || a->pad_stick) {
        dk_stick(x, y);
        a->pad_stick = x || y;
    }
}

static void pad_poll(app_t *a)
{
    aos_touch_point_t pts[2];
    aos_touch_points(pts);
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();

    for (int i = 0; i < 2; i++) {
        uint8_t id = pts[i].down ? pts[i].id : 0;
        int x = (int)pts[i].x - rc.x1, y = (int)pts[i].y - rc.y1;
        if (id != a->fid[i]) {
            if (a->fown[i] == OWN_STICK) {
                dk_stick(0, 0);
                stick_show(a, false);
            }
            a->fown[i] = id ? claim(a, x, y, a->fown[1 - i]) : OWN_NONE;
            a->fid[i] = id;
            a->fdown[i] = now;
        }
        if (id && a->fown[i] == OWN_STICK) {
            stick_update(a, x, y);
        }
        if (id && a->fown[i] == DK_BTN_MENU && now - a->fdown[i] >= MENU_HOLD_MS) {
            a->want_exit = true;
        }
    }
    gamepad_poll(a, now);
    pad_apply(a);
}

/* --------------------------------------------------------------------------
 * Messages over the picture
 * -------------------------------------------------------------------------- */

static void show_msg(app_t *a, const char *text, bool tap_to_exit)
{
    lv_label_set_text(a->msg, text);
    lv_obj_remove_flag(a->msg, LV_OBJ_FLAG_HIDDEN);
    a->exit_on_tap = tap_to_exit;
}

static void no_grp(app_t *a)
{
    char text[400];
    snprintf(text, sizeof text, "%s\n\n%s\n%s\n\n%s",
             _("No está DUKE3D.GRP"),
             _("Copiá DUKE3D.GRP (shareware o el juego completo) a la carpeta"),
             dk_data_dir(),
             _("Tocá para salir"));
    show_msg(a, text, true);
}

static void gestures_block(app_t *a, bool on)
{
    if (a->blocked != on) {
        a->blocked = on;
        aos_ui_block_gestures(on);
    }
}

static void stopped(app_t *a)
{
    a->running = false;
    pad_release_all(a);
    gestures_block(a, false);
    lv_obj_invalidate(a->root);
}

/* --------------------------------------------------------------------------
 * The frame loop
 * -------------------------------------------------------------------------- */

static rect_t to_panel(rect_t r, int rot)
{
    switch (rot) {
    case 90:  return (rect_t){ AOS_PANEL_W - (r.y + r.h), r.x, r.h, r.w };
    case 180: return (rect_t){ AOS_PANEL_W - (r.x + r.w), AOS_PANEL_H - (r.y + r.h), r.w, r.h };
    case 270: return (rect_t){ r.y, AOS_PANEL_H - (r.x + r.w), r.h, r.w };
    default:  return r;
    }
}

static uint32_t copy_box(uint16_t *dst, const uint16_t *src, rect_t s, int rot)
{
    rect_t p = to_panel(s, rot);
    if (p.w <= 0 || p.h <= 0) return 0;
    for (int y = 0; y < p.h; y++) {
        size_t o = (size_t)(p.y + y) * AOS_PANEL_W + p.x;
        memcpy(dst + o, src + o, (size_t)p.w * 2);
    }
    return (uint32_t)p.w * p.h * 2;
}

static void clear_box(uint16_t *dst, rect_t s, int rot)
{
    rect_t p = to_panel(s, rot);
    if (p.w <= 0 || p.h <= 0) return;
    for (int y = 0; y < p.h; y++) memset(dst + (size_t)(p.y + y) * AOS_PANEL_W + p.x, 0, (size_t)p.w * 2);
}

static fbrec_t *fb_rec(app_t *a, uint16_t *fb)
{
    for (int i = 0; i < FB_N; i++)
        if (a->fbr[i].fb == fb) return &a->fbr[i];
    for (int i = 0; i < FB_N; i++) {
        if (!a->fbr[i].fb) {
            a->fbr[i].fb = fb;
            a->fbr[i].whole = false;
            return &a->fbr[i];
        }
    }
    return NULL;
}

/* Everything between the side bands that the picture does not cover:
 * the strips above and below it, and beside it for a 4:3 mode. */
static void clear_around(uint16_t *fb, const rect_t *p, int rot)
{
    int x0 = L.bx, x1 = L.bx + BOX_W;
    clear_box(fb, (rect_t){ x0, 0, BOX_W, p->y }, rot);
    clear_box(fb, (rect_t){ x0, p->y + p->h, BOX_W, L.h - p->y - p->h }, rot);
    clear_box(fb, (rect_t){ x0, p->y, p->x - x0, p->h }, rot);
    clear_box(fb, (rect_t){ p->x + p->w, p->y, x1 - p->x - p->w, p->h }, rot);
}

static bool present_flip(app_t *a, const uint16_t *f, int fw, int fh)
{
    uint16_t *fb = aos_hal_display_back();
    fbrec_t *r = fb ? fb_rec(a, fb) : NULL;
    uint32_t age = 0;
    if (!r || !aos_hal_display_back_age(fb, &age, NULL)) return false;
    if (a->redo) {
        a->redo = false;
        for (int i = 0; i < FB_N; i++) a->fbr[i].whole = false;
    }
    if (age == UINT32_MAX) r->whole = false;
    if (a->pad_refr || !r->whole) {
        a->pad_refr = false;
        lv_refr_now(NULL);
    }
    int rot = aos_hal_display_get_rotation();
    const rect_t *p = &a->pic;
    if (!aos_hal_display_blit_into_fit(fb, p->x, p->y, p->w, p->h, f, fw, fh, fw, false)) return false;

    uint64_t t0 = aos_hal_uptime_us();
    const uint16_t *front = NULL;
    uint32_t bytes = 0;
    if (!r->whole || r->dirty[0] || r->dirty[1]) {
        int sw, sh;
        if (!aos_hal_display_fb(&front, &sw, &sh)) front = NULL;
    }
    if (!r->whole && front) {
        clear_around(fb, p, rot);
        bytes += copy_box(fb, front, (rect_t){ 0, 0, L.band, L.h }, rot);
        bytes += copy_box(fb, front, (rect_t){ L.w - L.band, 0, L.band, L.h }, rot);
        r->whole = true;
        r->dirty[0] = r->dirty[1] = false;
        a->wholes++;
    } else if (front) {
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        for (int b = 0; b < 2; b++) {
            if (!r->dirty[b]) continue;
            bytes += copy_box(fb, front, r->d[b], rot);
            if (now - a->pad_ms[b] >= PAD_SETTLE_MS) r->dirty[b] = false;
        }
    }
    a->pad_us += aos_hal_uptime_us() - t0;
    a->pad_kb += bytes / 1024;
    if (!aos_hal_display_flip(fb)) {
        r->whole = false;
        return false;
    }
    a->flips++;
    return true;
}

static void push_frame(app_t *a)
{
    int fw = 0, fh = 0;
    const uint16_t *f = dk_frame_take(&fw, &fh);
    if (!f) return;
    if (!a->shown_any) {
        a->shown_any = true;
        lv_obj_add_flag(a->msg, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        a->redo = true;
    }
    if (fw != a->fw || fh != a->fh) {
        /* a new video mode: a new place for the picture, and every buffer
         * gets its strips cleared again */
        a->fw = fw;
        a->fh = fh;
        a->pic = pic_rect(fw, fh);
        a->redo = true;
        aos_hal_log("duke3d", "picture %dx%d -> %dx%d at %d,%d", fw, fh, a->pic.w, a->pic.h,
                    a->pic.x, a->pic.y);
    }
    uint64_t t0 = aos_hal_uptime_us();
    bool flip = !(a->over & AOS_UI_OVER_TOAST) && present_flip(a, f, fw, fh);
    if (flip || aos_hal_display_blit_fit(a->pic.x, a->pic.y, a->pic.w, a->pic.h, f, fw, fh, fw, false)) {
        a->blit_us += aos_hal_uptime_us() - t0;
        a->blit_n++;
        if (!flip) {
            a->directs++;
            a->redo = true;
        }
    }
    dk_frame_blitted();
    a->blits++;
}

static void log_rates(app_t *a, uint32_t now)
{
    if (!a->fps_ms) {
        a->fps_ms = now;
        a->fps_frames = dk_frames();
        a->fps_blits = a->blits;
        return;
    }
    if (now - a->fps_ms < 5000) return;
    uint32_t dt = now - a->fps_ms;
    uint32_t fr = dk_frames() - a->fps_frames, bl = a->blits - a->fps_blits;
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    uint32_t cm, ct;
    dk_audio_cycles(&cm, &ct);
    unsigned pm = (unsigned)((uint64_t)(cm - a->cyc_music) * 1000 / (CPU_MHZ * 1000) / dt);
    unsigned pt = (unsigned)((uint64_t)(ct - a->cyc_total) * 1000 / (CPU_MHZ * 1000) / dt);
    unsigned bu = a->blit_n ? (unsigned)(a->blit_us / a->blit_n) : 0;
    unsigned pu = a->flips ? (unsigned)(a->pad_us / a->flips) : 0;
    unsigned pk = a->flips ? (unsigned)(a->pad_kb / a->flips) : 0;
    a->cyc_music = cm;
    a->cyc_total = ct;
    /* the game's own time per frame, the 30 fps wait left out: what the
     * frame would cost uncapped (audio included, it mixes in the same task) */
    uint64_t cus;
    uint32_t cn;
    dk_frame_cost(&cus, &cn);
    unsigned fc = cn - a->cost_n ? (unsigned)((cus - a->cost_us) / (cn - a->cost_n)) : 0;
    a->cost_us = cus;
    a->cost_n = cn;
    aos_hal_log("duke3d", "%s%s: %u.%u fps rendered, %u.%u shown, frame %u.%u ms of game, blit %u us (%u flips, %u direct), "
                "pad %u us %u KB, whole %u | audio %u.%u%% of core 0 (music %u.%u%%) | internal %u B, psram %u B",
                dk_where(), a->held ? " PAUSED" : "",
                (unsigned)(fr * 10000 / dt / 10), (unsigned)(fr * 10000 / dt % 10),
                (unsigned)(bl * 10000 / dt / 10), (unsigned)(bl * 10000 / dt % 10),
                fc / 1000, fc % 1000 / 100, bu, (unsigned)a->flips, (unsigned)a->directs, pu, pk, (unsigned)a->wholes,
                pt / 10, pt % 10, pm / 10, pm % 10,
                (unsigned)fi, (unsigned)fp);
    a->flips = a->directs = a->wholes = 0;
    a->pad_us = 0;
    a->pad_kb = 0;
    a->fps_ms = now;
    a->fps_frames = dk_frames();
    a->fps_blits = a->blits;
    a->blit_us = 0;
    a->blit_n = 0;
}

static void frame(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->want_exit) {
        a->want_exit = false;
        aos_ui_home();
        return;
    }
    if (!a->running) {
        aos_pad_update(&a->pad, lv_tick_get());
        if (a->exit_on_tap && aos_pad_pressed(&a->pad, AOS_PAD_A | AOS_PAD_START)) a->want_exit = true;
        return;
    }

    dk_state_t st = dk_state();
    if (st == DK_QUIT) {
        stopped(a);
        aos_ui_home();
        return;
    }
    if (st == DK_ERROR || st == DK_STOPPED) {
        stopped(a);
        char text[360];
        snprintf(text, sizeof text, "%s\n\n%s\n\n%s", _("Duke Nukem 3D se detuvo"), dk_error(),
                 _("Tocá para salir"));
        show_msg(a, text, true);
        return;
    }
    if (a->paused_msg) {
        return;
    }
    a->over = aos_ui_overlay();
    const char *cur = aos_ui_current_app();
    if (!cur || strcmp(cur, a->self->desc.id) != 0) a->over |= AOS_UI_OVER_NOT_FRONT;
    bool hold = (a->over & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    if (hold != a->held) {
        a->held = hold;
        dk_pause(hold);
        if (hold) pad_release_all(a);
        a->redo = true;
        aos_hal_log("duke3d", hold ? "paused: 0x%x over the game" : "playing again (0x%x)",
                    (unsigned)a->over);
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!hold) {
        pad_poll(a);
        push_frame(a);
    }
    log_rates(a, now);
}

static void touch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->exit_on_tap) a->want_exit = true;
}

static void orientation_check(app_t *a)
{
    bool pause = !L.ok;
    if (pause == a->paused_msg) return;
    a->paused_msg = pause;
    pad_visible(a, !pause);
    if (pause) {
        if (a->running) pad_release_all(a);
        gestures_block(a, false);
        char text[200];
        snprintf(text, sizeof text, "%s\n\n%s", _("Duke Nukem 3D se juega con la pantalla horizontal"),
                 _("Girá la pantalla para seguir"));
        show_msg(a, text, !a->running);
    } else {
        if (a->running) gestures_block(a, true);
        if (a->shown_any || !a->running) lv_obj_add_flag(a->msg, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(a->root);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static bool duke_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    /* a back from elsewhere (the portal) is Duke's MENU while playing */
    if (!a || !a->running) return false;
    a->menu_tap = true;
    return true;
}

static bool duke_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)root;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    layout_compute();
    if (a->fw) a->pic = pic_rect(a->fw, a->fh);
    pad_place(a);
    orientation_check(a);
    return true;
}

static void *duke_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->fown[0] = a->fown[1] = OWN_NONE;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("duke3d", "opening | internal %u B, psram %u B", (unsigned)hi, (unsigned)hp);

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    layout_compute();
    a->pic = pic_rect(320, 200);
    build_pad(a);

    a->msg = lv_label_create(root);
    lv_label_set_long_mode(a->msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(a->msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(a->msg, lv_color_hex(0xF8E8B0), 0);
    lv_obj_set_style_text_font(a->msg, aos_font_body, 0);
    lv_obj_remove_flag(a->msg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->msg, LV_OBJ_FLAG_HIDDEN);

    a->touch = lv_obj_create(root);
    lv_obj_remove_style_all(a->touch);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSED, a);

    pad_place(a);
    orientation_check(a);

    a->timer = lv_timer_create(frame, TICK_MS, a);

#ifdef AOS_SIM
    /* The game keeps pointers in 32-bit integers (the CON scripts'
     * bytecode, the actors' state): it runs on the board's 32-bit RISC-V,
     * not in the 64-bit simulator. */
    show_msg(a, _("Duke Nukem 3D corre en la placa, no en el simulador"), true);
    return a;
#endif

    char grp[128];
    if (!dk_find_grp(grp, sizeof grp)) {
        aos_hal_log("duke3d", "no GRP in %s", dk_data_dir());
        no_grp(a);
        return a;
    }
    aos_hal_log("duke3d", "GRP %s", grp);

    a->sound = aos_hal_spk_open(DK_RATE);
    dk_sound_enable(a->sound);
    if (!a->sound) aos_hal_log("duke3d", "no speaker: playing silent");

    if (!a->paused_msg) show_msg(a, _("Cargando..."), false);
    if (!dk_start(grp)) {
        char text[300];
        snprintf(text, sizeof text, "%s\n\n%s\n\n%s", _("Duke Nukem 3D no pudo arrancar"),
                 dk_error()[0] ? dk_error() : _("Falta memoria"), _("Tocá para salir"));
        show_msg(a, text, true);
        return a;
    }
    a->running = true;
    if (!a->paused_msg) gestures_block(a, true);

    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("duke3d", "started | internal %u B, psram %u B", (unsigned)hi, (unsigned)hp);
    return a;
}

static void duke_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->timer = NULL;
    gestures_block(a, false);
    dk_stop();
    if (a->sound) aos_hal_spk_close();
    dk_sound_enable(false);
    if (a->root) lv_obj_clean(a->root);
    lv_free(a);
}

/* The launcher icon: the radiation trefoil, black on yellow. */
static const uint8_t DUKE_ICON[] = {
    AIC_HEADER,
    AIC_ARC(AIC_CENTER, 0, 0, 64, 0, 22, 0, 360, 240, 300, 0, AIC_C_BG, 0, AIC_C_LIT(0x141008), 255),
    AIC_ARC(AIC_CENTER, 0, 0, 64, 0, 22, 0, 360,   0,  60, 0, AIC_C_BG, 0, AIC_C_LIT(0x141008), 255),
    AIC_ARC(AIC_CENTER, 0, 0, 64, 0, 22, 0, 360, 120, 180, 0, AIC_C_BG, 0, AIC_C_LIT(0x141008), 255),
    AIC_RECT(AIC_CENTER, 0, 0, 14, 14, AIC_CIRCLE, AIC_C_LIT(0x141008), 255),
    AIC_END
};

static bool duke_init(aos_app_t *app)
{
    app->desc.id       = "demo.duke3d";
    app->desc.name     = "Duke Nukem 3D";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, DUKE_ICON, sizeof DUKE_ICON);
    app->desc.color_a  = 0xF5C800;
    app->desc.color_b  = 0xB06A00;
    app->desc.order    = 163;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG |
                         AOS_APP_FLAG_LANDSCAPE;

    app->create  = duke_create;
    app->destroy = duke_destroy;
    app->back    = duke_back;
    app->resize  = duke_resize;
    return true;
}

AOS_APP_ENTRY(duke_init);
