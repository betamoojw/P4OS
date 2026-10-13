/*
 * P4OS - Python: what a Python app's view (py_script.c, in the LVGL task)
 * and the aos module (mod_aos.c, on the interpreter's thread) share.
 *
 * One word hands everything over, as in Lua's app: while 'phase' says
 * PYC_SCRIPT the canvas, its lists and the snapshot belong to the script's
 * thread; while it says PYC_SHOW they are the LVGL task's. The script draws
 * and calls aos.frame(), which sets PYC_SHOW and waits; the view's frame
 * timer copies what changed onto the OS's canvas, writes the finger and the
 * pad into the snapshot, queues the events and sets PYC_SCRIPT again. Nobody
 * else waits for anybody.
 *
 * Only the standard headers and px_pixel.h here: tools/gen_upy.sh scans
 * mod_aos.c with the Mac's compiler and none of the firmware's headers.
 */
#pragma once

#include "px_pixel.h"

#include <stdbool.h>
#include <stdint.h>

enum { PYC_SCRIPT = 0, PYC_SHOW };

/* A finger event or a turn of the screen, on its way to the script. */
enum { PYC_EV_DOWN = 0, PYC_EV_MOVE, PYC_EV_UP, PYC_EV_GESTURE, PYC_EV_RESIZE };

typedef struct {
    uint8_t     kind;           /* PYC_EV_*                                  */
    uint8_t     nargs;          /* gesture: how many of a, b, c go           */
    const char *name;           /* gesture: "tap", "pinch"... (static)       */
    int16_t     x, y;           /* resize: the new w, h                      */
    float       a, b, c;
} pyc_ev_t;

#define PYC_MAX_EVENTS  32
#define PYC_MAX_BEEPS   4

/* the gamepad's roles, as aos_pad.h numbers them (py_script.c checks) */
#define PYC_PAD_UP      (1u << 0)
#define PYC_PAD_DOWN    (1u << 1)
#define PYC_PAD_LEFT    (1u << 2)
#define PYC_PAD_RIGHT   (1u << 3)
#define PYC_PAD_A       (1u << 4)
#define PYC_PAD_B       (1u << 5)
#define PYC_PAD_L       (1u << 6)
#define PYC_PAD_R       (1u << 7)
#define PYC_PAD_START   (1u << 8)

typedef struct {
    /* --- the canvas: allocated once for the largest it may be, so a turn
     *     never moves it under a framebuf the script holds --- */
    uint16_t  *work;            /* w x h, where the script draws             */
    uint16_t  *back;            /* aos.background()'s copy                   */
    bool       back_on;         /* there is one: frames start from it        */
    size_t     cap_px;          /* what work and back hold                   */
    int16_t    w, h;
    px_buf_t   buf;
    /* 'drawn' is what the script touched since the last frame (the
     * primitives mark it), 'dirty' what goes to the screen (that and what
     * was undone from the background), 'prev' what to undo next. */
    px_dirty_t drawn, dirty, prev;
    bool       fb_used;         /* aos.fb was taken: its drawing is not
                                 * marked, so every frame goes out whole     */

    int        phase;           /* PYC_*, atomically                         */

    /* --- written by the view before it hands a frame back --- */
    int16_t    touch_x, touch_y;
    bool       touch_down;
    uint8_t    fingers;
    uint8_t    finger_id[2];
    int16_t    finger_x[2], finger_y[2];
    uint16_t   pad_held, pad_pressed;
    int16_t    pad_x, pad_y;
    bool       pad_on;
    pyc_ev_t   ev[PYC_MAX_EVENTS];
    int        n_ev;
    uint32_t   t0;              /* ms when the script started                */
    uint16_t   ms_screen, ms_frame;
    uint16_t   rows;
    uint32_t   px;

    /* --- written by the script, read by the view --- */
    struct { uint16_t hz, ms; } beeps[PYC_MAX_BEEPS];
    uint8_t    n_beeps;
    bool       has_resize;      /* __main__ defines resize(w, h)             */
    bool       framed;          /* aos.frame() was called at least once      */
    uint16_t   ms_script;
    uint32_t   t_script;
} pyc_t;

/* The canvas of the app this copy of the module is: set by py_script.c while
 * a Python app is open, NULL in the console (the aos module then says it
 * only works in an app). Each app loaded from python.so is its own copy of
 * the module, so this is never two things at once. */
extern pyc_t *pyc_current;

/* Forgets what the aos module kept of the last script (its events, its
 * framebuf): before a script is handed to a fresh interpreter. */
void p4py_aos_reset(void);
