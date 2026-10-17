/*
 * PINBALL - the picture
 *
 * The table is rendered once in Blender (tools/blender/build_table.py)
 * and comes in the pack as a picture of the table with every lamp off and
 * every target up, and the depth of each of its pixels. On top of that:
 *
 *   - lamps: light added to their own pixels (PK_ADD), one entry each;
 *   - targets down: their pixels and depth replaced (PK_PATCH);
 *   - flippers and the spinner: frames rendered at each angle (PK_SPRITE),
 *     which write the depth of their plane;
 *   - the ball: a chrome ball rendered once and drawn at the size its
 *     distance gives, only where it is nearer than what is already there
 *     (so walls, ramps and posts in front of it hide it), with its shadow
 *     on the playfield under it;
 *   - the DMD above the table, and the launch and pause buttons.
 *
 * Only what changed is drawn again: each frame lists its dirty rectangles,
 * and the app pushes those rows to the panel.
 */
#pragma once

#include "pb_dmd.h"
#include "pb_game.h"
#include "pb_pak.h"
#include "pb_phys.h"
#include "pb_table.h"

#include <stdbool.h>
#include <stdint.h>

#define PB_SW       720
#define PB_SH       1280
#define PB_TOP      160         /* the table's picture starts at this row    */
#define PB_TH       (PB_SH - PB_TOP)
#define PB_MAX_DIRTY 48
#define PB_FLIP_FRAMES 24
#define PB_TOY_FRAMES 24
#define PB_SPIN_FRAMES 16
#define PB_BALL_MIN 14
#define PB_BALL_MAX 52

typedef struct {
    int16_t x0, y0, x1, y1;     /* inclusive x0,y0, exclusive x1,y1          */
} pb_rect_t;

typedef struct {
    int16_t   x, y, w, h;
    uint16_t *px;
    uint8_t  *a;
    uint8_t  *bump;             /* the ball: how far its surface stands out  */
    uint16_t *z;                /* a toy's frame: the depth of its pixels    */
} pb_spr_t;

typedef struct {
    int16_t   x, y, w, h;       /* table picture coordinates (screen - PB_TOP) */
    uint8_t   kind;             /* PK_ADD or PK_PATCH                         */
    uint16_t *px;
    uint16_t *z;
    uint8_t  *m;
} pb_patch_t;

/* what the app's own parts look like this frame */
typedef struct {
    bool  launch_show, launch_down;
    float charge;               /* 0..1                                       */
    bool  pause_down;
    bool  debug;                /* the physics' shapes over the picture       */
} pb_ui_t;

typedef struct {
    const pb_table_t *t;
    uint16_t  *fb;              /* PB_SW x PB_SH, the frame                   */
    uint16_t  *zfb;             /* PB_SW x PB_TH, depth of the frame's pixels */
    uint16_t  *base;            /* PB_SW x PB_TH, every lamp off              */
    uint16_t  *zbase;
    uint16_t  *dmd_bg;          /* PB_SW x PB_TOP, or NULL                    */
    pb_spr_t   flip[PB_MAX_FLIP][PB_FLIP_FRAMES];
    int        nflip[PB_MAX_FLIP];
    pb_rect_t  flip_area[PB_MAX_FLIP];
    pb_spr_t   toy[PB_MAX_TOY][PB_TOY_FRAMES];
    int        ntoy[PB_MAX_TOY];
    pb_rect_t  toy_area[PB_MAX_TOY];
    int        toy_drawn[PB_MAX_TOY];     /* frame * 256 + opacity drawn     */
    pb_spr_t   spin[PB_SPIN_FRAMES];
    int        nspin;
    int        spin_obj;
    pb_rect_t  spin_area;
    pb_spr_t   ball[PB_BALL_MAX + 1];  /* by diameter in pixels              */
    pb_spr_t   launch[2], pause;
    pb_patch_t lamp[PB_MAX_LAMP];
    pb_patch_t drop[PB_MAX_OBJ];
    bool       lamp_vis[PB_MAX_LAMP];
    uint8_t    drop_vis[PB_MAX_OBJ];
    uint16_t   zrow0[PB_SH];    /* the playfield's depth by row (0.1 mm)      */
    uint16_t   zrow_flip[PB_SH];
    uint16_t   dmd_lut[16][8];  /* a dot's colour by level and coverage       */
    int16_t    dmd_cx[PB_SW];   /* the dot column of a screen column, -1 gap  */
    uint8_t    dmd_wx[PB_SW];
    int16_t    dmd_cy[PB_TOP];
    uint8_t    dmd_wy[PB_TOP];
    pb_dmd_t   dmd, dmd_shown;
    bool       dmd_valid;
    pb_rect_t  dirty[PB_MAX_DIRTY];
    int        ndirty;
    pb_rect_t  prev[PB_MAX_BALLS * 2];
    int        nprev;
    int        flip_drawn[PB_MAX_FLIP], spin_drawn;
    int        launch_drawn;    /* -1 hidden, else pressed * 100 + charge %  */
    bool       pause_drawn;
    pb_rect_t  launch_rc, pause_rc;
    bool       debug;
    uint32_t   bytes;           /* PSRAM the pictures take                    */
} pb_render_t;

bool pb_render_init(pb_render_t *r, const pb_table_t *t, pb_pak_t *pak);
void pb_render_free(pb_render_t *r);
/* everything drawn again next frame */
void pb_render_full(pb_render_t *r);
/* draws what changed; r->dirty / r->ndirty say where */
void pb_render_frame(pb_render_t *r, const pb_game_t *g, const pb_phys_t *ph, const pb_ui_t *ui);
/* the table's picture as a game starts, for a thumbnail */
bool pb_render_inside(const pb_rect_t *rc, int x, int y);
