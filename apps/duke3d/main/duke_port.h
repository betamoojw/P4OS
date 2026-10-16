/*
 * P4OS - Duke Nukem 3D: the seam between the app (duke3d.c, LVGL's task)
 * and the game (engine/ + game/ + port/, the worker on core 0).
 *
 * As in Doom's port, neither side includes the other's headers: duke3d.c
 * knows nothing of the Build engine's globals, and the game knows nothing
 * of LVGL. What crosses is plain memory: three frame slots, the pad's
 * buttons and the stick, all written by one side and read by the other.
 */
#ifndef DUKE_PORT_H
#define DUKE_PORT_H

#include <stdbool.h>
#include <stdint.h>

/* The picture. The Build engine draws at any size; the game renders at
 * the size its config asks for (DUKE3D.CFG, ScreenWidth/ScreenHeight, or
 * Duke's own video menu), up to this. Each frame goes through the palette
 * into one of three RGB565 little-endian slots of that size, which the PPA
 * scales into the panel's free buffer. */
#define DK_MAX_W    640
#define DK_MAX_H    480
#define DK_SLOTS    3

/* The pad's buttons. Each is one of the game's functions while playing
 * (gamefunc_*); in the menus, the screens between levels and the y/n
 * questions the port turns them into the keys those want. */
enum {
    DK_BTN_FIRE = 0,
    DK_BTN_USE,         /* open, press a switch */
    DK_BTN_JUMP,
    DK_BTN_CROUCH,
    DK_BTN_MENU,        /* Escape */
    DK_BTN_MAP,
    DK_BTN_WEAPON,      /* the next weapon */
    DK_BTN_INV,         /* use the selected item */
    DK_BTN_INV_NEXT,    /* select the next item */
    DK_BTN_STRAFE,      /* held in the game while the app has it latched */
    DK_BTN_N,           /* the ones on the screen */
    /* a USB gamepad's own, with nothing on the screen */
    DK_BTN_STRAFE_L = DK_BTN_N,
    DK_BTN_STRAFE_R,
    DK_BTN_LOOK_UP,
    DK_BTN_LOOK_DOWN,
    DK_BTN_ALL
};

typedef enum {
    DK_IDLE = 0,
    DK_LOADING,     /* reading the GRP, the CON scripts, the art */
    DK_RUNNING,
    DK_QUIT,        /* the player quit from Duke's menu */
    DK_STOPPED,     /* stopped from outside (the app closed) */
    DK_ERROR,       /* Error(): dk_error() says what */
} dk_state_t;

/* Where the game's things live on the card. data_dir ends with '/'. */
bool        dk_find_grp(char *out, int len);
const char *dk_data_dir(void);

/* Starts the game in the app's worker. false: no memory or no worker. */
bool        dk_start(const char *grp_path);

/* Stops it (from destroy) and gives every byte back. */
void        dk_stop(void);

dk_state_t  dk_state(void);
const char *dk_error(void);

/* The newest finished frame and its size, or NULL. The slot stays
 * untouched by the engine until dk_frame_blitted(). */
const uint16_t *dk_frame_take(int *w, int *h);
void        dk_frame_blitted(void);
uint32_t    dk_frames(void);        /* frames finished since start */
const char *dk_where(void);         /* "E1L1 320x200", "menu"... for the log */
void        dk_audio_cycles(uint32_t *music, uint32_t *total);  /* running sums, CPU cycles */
void        dk_frame_cost(uint64_t *us, uint32_t *n);   /* running sums: the game's time per frame */

/* Paused: the game's clock stops and nothing runs, from the next frame on.
 * From LVGL's task, when something of the system's covers the picture. */
void        dk_pause(bool on);

/* Input, from LVGL's task. */
void        dk_button(int btn, bool down);
void        dk_stick(int x, int y);     /* -100..100 each, 0,0 = released */
void        dk_look(int y);             /* -100..100: the second stick's look */

/* Sound: the app opens the speaker on its side, the game only mixes into it. */
void        dk_sound_enable(bool on);
#define DK_RATE     22050               /* Duke's own mix rate (DUKE3D.CFG MixRate) */

#endif
