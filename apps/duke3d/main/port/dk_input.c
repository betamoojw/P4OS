#include "dk_compat.h" /* P4OS: the C library as the board has it */
/*
 * P4OS - Duke Nukem 3D: the pad, as the game wants it.
 *
 * The app (LVGL's task) only writes what the fingers and the gamepad hold:
 * a mask of DK_BTN_* and the stick. The game's task reads them two ways:
 *
 *   - Playing: as the game's functions. control.c's ACTION(i), which the
 *     game asks for every function it handles, asks dk_action(i) first, so
 *     FIRE is gamefunc_Fire whatever keys the player's config binds. A
 *     function the game "clears" after acting on it (the map, the next
 *     weapon, the inventory: CONTROL_ClearAction) stays off until its
 *     button is let go and pressed again, as a key would.
 *     The stick is analogue, added to what CONTROL_GetInput() reads from
 *     the mouse: sideways turns (or sidesteps with STRAFE latched), up and
 *     down walk. Duke always runs here, as Doom does.
 *   - In a menu, a y/n question, the screens between levels and the demos
 *     on the title: as the keys those read. FIRE is Enter (also "yes"),
 *     USE is Escape (back, "no"), MENU is Escape, and the stick is the
 *     arrow keys with a keyboard's auto-repeat.
 *
 * MENU is always Escape, in the game too: that is how Duke opens its menu.
 *
 * The keys go through the game's own keyhandler() (dk_feed_scancode in
 * dk_display.c), as the SDL driver sent SDL's key events. Every key this
 * file pressed is remembered, so that a mode change (FIRE held as a menu
 * closes) releases it instead of leaving it stuck.
 */
#include "../duke_port.h"
#include "dk_port.h"
#include "function.h"
#include "keyboard.h"
#include "aos_hal.h"

void dk_feed_scancode(unsigned code);
bool dk_autowalk(void);

/* ---- what the app holds (LVGL's task writes, the game's reads) ---- */

static volatile uint32_t s_btn;         /* 1 << DK_BTN_* */
static volatile int      s_sx, s_sy;    /* -100..100 */
static volatile int      s_look;        /* -100..100 */

void dk_button(int btn, bool down)
{
    if (btn < 0 || btn >= DK_BTN_ALL) return;
    if (down) __atomic_or_fetch(&s_btn, 1u << btn, __ATOMIC_RELEASE);
    else      __atomic_and_fetch(&s_btn, ~(1u << btn), __ATOMIC_RELEASE);
}

void dk_stick(int x, int y)
{
    s_sx = x < -100 ? -100 : x > 100 ? 100 : x;
    s_sy = y < -100 ? -100 : y > 100 ? 100 : y;
}

void dk_look(int y)
{
    s_look = y < -100 ? -100 : y > 100 ? 100 : y;
}

/* ---- the game's side ---- */

static uint32_t s_cleared;              /* functions cleared until released */
static uint32_t s_cleared_hi;

/* Which pad button holds each game function, playing. */
static int func_button(int f)
{
    switch (f) {
    case gamefunc_Fire:            return DK_BTN_FIRE;
    case gamefunc_Open:            return DK_BTN_USE;
    case gamefunc_Jump:            return DK_BTN_JUMP;
    case gamefunc_Crouch:          return DK_BTN_CROUCH;
    case gamefunc_Map:             return DK_BTN_MAP;
    case gamefunc_Next_Weapon:     return DK_BTN_WEAPON;
    case gamefunc_Inventory:       return DK_BTN_INV;
    case gamefunc_Inventory_Right: return DK_BTN_INV_NEXT;
    case gamefunc_Strafe:          return DK_BTN_STRAFE;
    case gamefunc_Strafe_Left:     return DK_BTN_STRAFE_L;
    case gamefunc_Strafe_Right:    return DK_BTN_STRAFE_R;
    case gamefunc_Look_Up:         return DK_BTN_LOOK_UP;
    case gamefunc_Look_Down:       return DK_BTN_LOOK_DOWN;
    default:                       return -1;
    }
}

static bool cleared(int f)
{
    return f < 32 ? (s_cleared >> f) & 1u : (s_cleared_hi >> (f - 32)) & 1u;
}

static void set_cleared(int f, bool on)
{
    uint32_t *w = f < 32 ? &s_cleared : &s_cleared_hi;
    uint32_t bit = 1u << (f < 32 ? f : f - 32);
    if (on) *w |= bit;
    else    *w &= ~bit;
}

int dk_action(int f)
{
    if (dk_game_mode() != DK_MODE_GAME) return 0;
    if (f == gamefunc_Run) return 1;            /* always run */
    int look = s_look;
    if (f == gamefunc_Aim_Up && look < -50) return 1;
    if (f == gamefunc_Aim_Down && look > 50) return 1;
    int b = func_button(f);
    if (b < 0) return 0;
    bool held = (s_btn >> b) & 1u;
    if (!held) {
        set_cleared(f, false);
        return 0;
    }
    return !cleared(f);
}

/* control.c's CONTROL_ClearAction: the game acted on f, a press of it is
 * used up until its button is let go. */
void dk_clear_action(int f)
{
    if (f >= 0 && f < 64 && func_button(f) >= 0 && ((s_btn >> func_button(f)) & 1u)) {
        set_cleared(f, true);
    }
}

static int32_t s_last_dz, s_last_dyaw;     /* for the log */

/* Walking, apart from the rest: getinput() in player.c sets the forward
 * motion it read from CONTROL_GetInput() back to 0 (xDuke's FIX_00021, so
 * that moving a mouse up and down does not walk), and calls this after. */
void dk_analog_walk(int32_t *dz)
{
    if (dk_game_mode() != DK_MODE_GAME) return;
    int y = dk_autowalk() ? -100 : s_sy;
    *dz += (int32_t)y * (90 * 64) / 100;
    s_last_dz = *dz;
}

void dk_input_last(int *x, int *y, int32_t *dz, int32_t *dyaw)
{
    *x = s_sx;
    *y = s_sy;
    *dz = s_last_dz;
    *dyaw = s_last_dyaw;
}

void dk_analog(int32_t *dyaw, int32_t *dx, int32_t *dz, int32_t *dpitch)
{
    (void)dpitch;
    if (dk_game_mode() != DK_MODE_GAME) return;
    int x = s_sx, y = s_sy;
    /* player.c: vel = -dz >> 6 and svel = -dx >> 6, clamped to 90 (running
     * on the keyboard is 80); angvel = dyaw / 64, the keyboard's fast turn
     * is 30. Walking is linear, turning is squared, so a small push aims
     * finely and the edge still turns fast. */
    int32_t walk = 90 * 64;                     /* full push */
    int32_t turn = (int32_t)x * (x < 0 ? -x : x) * 40 * 64 / 10000;
    if ((s_btn >> DK_BTN_STRAFE) & 1u) {
        *dx += (int32_t)x * walk / 100;
    } else {
        *dyaw += turn;
    }
    s_last_dyaw = *dyaw;
}

/* ---- the game waiting for a key ----
 *
 * Duke waits for "any key" in many places that are not menus: the
 * shareware's ordering screens on the way out, the end of an episode, the
 * score after a level. They are loops of while (!KB_KeyWaiting()), with the
 * game still in MODE_GAME, so FIRE would go out as gamefunc_Fire and never
 * as a key. What they have in common: KB_KeyWaiting() called over and over
 * with no frame drawn. Playing, the game asks it a few times a frame at
 * most. So past a count without a frame the pad sends keys, as in a menu,
 * and each call sleeps a millisecond and keeps the sound fed: on core 0 a
 * loop that never blocks starves the idle task (and the task watchdog). */
#define WAIT_CALLS  64

static uint32_t s_key_calls;            /* KB_KeyWaiting() since the last frame */

void dk_key_waiting(void)
{
    if (++s_key_calls > WAIT_CALLS) {
        dk_sound_update();
        aos_hal_worker_sleep(1);
    }
}

void dk_frame_done(void)
{
    s_key_calls = 0;
}

bool dk_waiting_key(void)
{
    return s_key_calls > WAIT_CALLS;
}

/* ---- keys for the menus and the screens ---- */

/* raw scancodes: 0xE0 first for the extended ones */
#define K_ESC       0x01
#define K_ENTER     0x1C
#define K_SPACE     0x39
#define K_UP        0x148           /* 0x100: send 0xE0 first */
#define K_DOWN      0x150
#define K_LEFT      0x14B
#define K_RIGHT     0x14D

#define MAX_HELD    8
static unsigned s_held[MAX_HELD];
static int      s_nheld;
static unsigned s_rep_key;              /* the arrow repeating, 0 none */
static uint32_t s_rep_ms;
static uint32_t s_prev_btn;

#define REPEAT_FIRST_MS 350
#define REPEAT_MS       120

static void key_event(unsigned k, bool down)
{
    if (k & 0x100) dk_feed_scancode(0xE0);
    dk_feed_scancode((k & 0x7F) | (down ? 0 : 0x80));
}

static bool is_held(unsigned k)
{
    for (int i = 0; i < s_nheld; i++)
        if (s_held[i] == k) return true;
    return false;
}

static void press(unsigned k)
{
    if (is_held(k) || s_nheld >= MAX_HELD) return;
    s_held[s_nheld++] = k;
    key_event(k, true);
}

static void release(unsigned k)
{
    for (int i = 0; i < s_nheld; i++) {
        if (s_held[i] == k) {
            s_held[i] = s_held[--s_nheld];
            key_event(k, false);
            return;
        }
    }
}

void dk_input_reset(void)
{
    s_btn = 0;
    s_sx = s_sy = s_look = 0;
    s_cleared = s_cleared_hi = 0;
    s_nheld = 0;
    s_rep_key = 0;
    s_prev_btn = 0;
}

/* From _handle_events(), in the game's task: brings the keys the game
 * sees in line with what the pad holds now. */
void dk_keys_pump(void)
{
    int mode = dk_game_mode();
    uint32_t b = s_btn;
    unsigned want[MAX_HELD];
    int nw = 0;

    if (b & (1u << DK_BTN_MENU)) want[nw++] = K_ESC;
    unsigned arrow = 0;
    if (mode != DK_MODE_GAME) {
        if (b & (1u << DK_BTN_FIRE)) want[nw++] = K_ENTER;
        if (b & (1u << DK_BTN_USE)) want[nw++] = K_ESC;
        if (b & (1u << DK_BTN_JUMP)) want[nw++] = K_SPACE;
        int x = s_sx, y = s_sy;
        int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
        if (ax > 50 || ay > 50) {
            arrow = ay >= ax ? (y < 0 ? K_UP : K_DOWN) : (x < 0 ? K_LEFT : K_RIGHT);
            want[nw++] = arrow;
        }
    }

    /* release what is no longer wanted, then press what is new */
    for (int i = s_nheld - 1; i >= 0; i--) {
        bool keep = false;
        for (int j = 0; j < nw; j++)
            if (want[j] == s_held[i]) keep = true;
        if (!keep) release(s_held[i]);
    }
    for (int j = 0; j < nw; j++) press(want[j]);

    /* an arrow held repeats like a keyboard's: the menus act on a press */
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (arrow != s_rep_key) {
        s_rep_key = arrow;
        s_rep_ms = now + REPEAT_FIRST_MS;
    } else if (arrow && (int32_t)(now - s_rep_ms) >= 0) {
        s_rep_ms = now + REPEAT_MS;
        key_event(arrow, false);
        key_event(arrow, true);
    }
    s_prev_btn = b;
}
