/*
 * CAÑOS - drawing the board, pixel by pixel.
 *
 * The board is one RGB565 picture in PSRAM that an lv_canvas shows; this
 * module writes into it and the app tells LVGL which cells changed. It
 * works in screen cells: the app turns the upright grid when the screen
 * lies down, and hands over each cell already turned (cn_tile_t).
 *
 * Three layers, all the board's size:
 *   floor   the world's ground, drawn once per level
 *   base    the floor with the pipes on it (and their shadows), redrawn per
 *           cell when a pipe turns or is put down
 *   fb      what is shown: base plus the liquid, the valve, the drain, the
 *           spill, redrawn per cell every frame while something moves
 *
 * Every pipe shape and turn has a geometry table, made once per level at
 * the cell's size: for each pixel, where it is across the channel (u, -127
 * at one wall to 127 at the other), how far along it from the channel's
 * entry (s), and how the tube there is lit. The liquid is shaded from that
 * alone, so the per-pixel work is a few table reads and adds:
 *
 *   - the front is a parabola (the middle of a pipe runs ahead of its
 *     walls) that wobbles, measured along the whole run, so it crosses
 *     from one cell into the next without a seam;
 *   - streaks of noise are carried along the flow at its speed, two layers
 *     at different speeds whose crossings make the caustics of a clear
 *     liquid; bubbles ride along at their own pace and the acid fizzes;
 *   - foam piles at the front of water, a fuel carries a rainbow film,
 *     lava floats a dark crust with glowing seams.
 *
 * A leak throws drops (particles with drag) that land into a height field;
 * the field spreads by diffusion, at the liquid's own pace, and is drawn as
 * a puddle lit by its own slope. Fumes, sparks or mist go with it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cn_world.h"

/* shapes on screen */
enum {
    CN_SH_NONE = 0,
    CN_SH_STRAIGHT,
    CN_SH_BEND,
    CN_SH_CROSS,
    CN_SH_SOURCE,
    CN_SH_DRAIN,
    CN_SH_BLOCK,
    CN_SHAPES
};

/* One pixel inside a channel. A shape's table lists only those: what the
 * liquid loop reads is a few KB a cell, not the whole cell. */
typedef struct {
    uint8_t  x, y;
    int8_t   u;             /* across the channel, -127..127                */
    uint8_t  lit;           /* the tube's diffuse light there               */
    uint8_t  spec;          /* and its highlight                            */
    uint8_t  ch;            /* which channel (a cross has two)              */
    uint16_t s;             /* along it from its entry side, 1/16 px        */
} cn_gpx_t;

typedef struct {
    cn_gpx_t *px;
    int       n;
} cn_geo_t;

/* A bubble's slot along the run (see cn_draw_begin). */
typedef struct {
    int32_t  cs;            /* its centre along the run, 1/16 px */
    int16_t  cu;            /* and across, u units               */
    uint8_t  on;
    uint32_t r2o, r2i;      /* its radius and its rim's inside, squared, (1/16 px)^2 */
} cn_bub_t;

#define CN_BUBS 2048

typedef struct {
    uint16_t *rgb;
    uint8_t  *a;
} cn_spr_t;

typedef struct {
    float   x, y, vx, vy, r, life, life0;
    uint8_t kind;           /* 0 drop, 1 fume/mist/smoke, 2 spark */
} cn_part_t;

#define CN_PARTS 360

/* One cell as the app wants it drawn, in screen terms. */
typedef struct {
    uint8_t shape, srot;
    uint8_t fixed;          /* on a bolted plate */
    float   turn;           /* degrees it still has to turn (animation), 0 = still */
    float   scale;          /* 1 = still; > 1 while a piece drops in            */
    struct {
        uint8_t on, rev;    /* liquid in it; it runs against the channel's s    */
        float   start_px;   /* where this channel starts along the whole run    */
    } ch[2];
    float   valve;          /* source: the wheel's turns                        */
    uint8_t flowing;        /* source: the tank is draining (it swirls)         */
    float   drain;          /* drain: 0..1 filled                               */
} cn_tile_t;

typedef struct {
    const cn_world_t *w;
    int       world;
    int       C;            /* cell side, px */
    int       cols, rows;   /* on screen */
    int       W, H;
    uint16_t *fb, *floor, *base;
    uint8_t  *field;        /* the puddle's height, W x H */
    cn_spr_t  spr[CN_SHAPES][4];
    cn_spr_t  plate;
    cn_geo_t  geo[CN_SHAPES][4];
    uint8_t  *pol_a, *pol_r;    /* a cell's polar coordinates: angle (1/256 turn), radius (1/510 cell) */
    uint16_t  ramp[256];
    uint8_t   noise[128 * 128];
    float     ri_px;        /* the channel's inner radius, px */

    /* this frame */
    float     t, phase_px, front_px, speed_px;
    int32_t   F16;                  /* the front, 1/16 px */
    int32_t   curv16[128], wob16[64], edge16[128];
    int32_t   Pm1, Pm2, kq1, kq2;   /* the streaks' offsets and scales */
    int16_t   litg[256], texk[256], caus[256], specg[256], logt[256];
    cn_bub_t *bub, *fiz;            /* bubbles and fizz, by slot */
    int       bub_k0, bub_n, fiz_k0, fiz_n;
    float     bub_inv, fiz_inv;
    int32_t   Pb16, Pf16, bub_rmax16, fiz_rmax16;

    /* the leak */
    bool      spilling;
    float     sx, sy;       /* where it comes out */
    float     sdx, sdy;     /* which way */
    bool      s_edge;       /* out of the board's edge */
    float     s_t, s_emit, s_fume, s_diff;
    cn_part_t part[CN_PARTS];
    int       npart;
    int       fx0, fy0, fx1, fy1;   /* what the field covers, px (x1 < x0: nothing) */
    int       px0, py0, px1, py1;   /* what the particles cover this frame */
    int       rx0, ry0, rx1, ry1;   /* to draw again: the field, the particles now and before */
    uint8_t  *row_tmp;
} cn_draw_t;

/* PSRAM first (the board and its tables are megabytes); free() releases it. */
void *cn_draw_alloc(size_t n);

bool cn_draw_init(cn_draw_t *d, int world, int cols, int rows, int cell);
void cn_draw_free(cn_draw_t *d);

/* base <- floor + the cell's pipe (a still one) */
void cn_draw_base(cn_draw_t *d, int sc, int sr, const cn_tile_t *t);

/* Per frame, before the cells: time, how far the flow has carried its
 * streaks, where the front is (px along the run) and how fast it goes. */
void cn_draw_begin(cn_draw_t *d, float t, float phase_px, float front_px, float speed_px);

/* fb <- the cell, with everything on it */
void cn_draw_tile(cn_draw_t *d, int sc, int sr, const cn_tile_t *t);

/* A leak at (x, y) px, going 'dir' (0 N, 1 E, 2 S, 3 W on screen); 'edge'
 * when it pours over the board's edge. */
void cn_draw_spill_start(cn_draw_t *d, float x, float y, int dir, bool edge);
/* 'flow' 0..1: how much is still coming out. */
void cn_draw_spill_step(cn_draw_t *d, float dt, float flow);
/* What has to be drawn again because of it, px; false: nothing. */
bool cn_draw_spill_rect(const cn_draw_t *d, int *x0, int *y0, int *x1, int *y1);
void cn_draw_spill_clear(cn_draw_t *d);

/* Pictures for the UI, without a board. 'out' is RGB565A8 (the colour
 * plane, then the alpha plane) of size x size. */
bool cn_draw_icon(int world, int shape, int srot, int size, uint8_t *out);
/* A pipe across a strip of floor with the liquid running in it, w x h
 * RGB565 ('t' seconds into the flow). */
bool cn_draw_preview(int world, int w, int h, float t, uint16_t *out);
