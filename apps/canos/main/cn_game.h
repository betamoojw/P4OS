/*
 * CAÑOS - the rules: the grid, the levels, and the liquid's way through it.
 *
 * Nothing here draws or knows about LVGL; the app feeds it time and taps and
 * reads back what changed.
 *
 * The grid is always "upright" (cols x rows as on a standing screen); the
 * app turns it on screen when the board lies down, so a level does not
 * change when the screen turns.
 *
 * The liquid runs along a list of segments, one per pipe channel it goes
 * through (a cross has two channels and can be crossed twice). The front is
 * one number, the distance from the source along that list, in cells: a
 * straight is 1, a bend a quarter circle of radius 1/2 (pi/4), the source
 * and the drain half a cell each. When the front reaches the end of the
 * last segment, the next cell decides: a pipe that opens towards it adds a
 * segment, the drain wins, and anything else (an empty cell, a wall, a pipe
 * turned the wrong way, the board's edge) is a leak.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CN_MAX_COLS     8
#define CN_MAX_ROWS     10
#define CN_LEVELS       10          /* per world */
#define CN_MAX_SEGS     (CN_MAX_COLS * CN_MAX_ROWS * 2 + 4)

enum { CN_N = 0, CN_E, CN_S, CN_W };
#define CN_OPP(d)   (((d) + 2) & 3)

enum { CN_MODE_ROTATE = 0, CN_MODE_FILL, CN_MODES };
enum { CN_EASY = 0, CN_NORMAL, CN_HARD, CN_DIFFS };

typedef enum {
    CN_EMPTY = 0,
    CN_STRAIGHT,
    CN_BEND,
    CN_CROSS,
    CN_SOURCE,
    CN_DRAIN,
    CN_BLOCK,
    CN_KINDS
} cn_kind_t;

/* The pieces the tray offers in the fill mode: every pipe in each of its
 * distinct turns. */
enum {
    CN_T_SV = 0,    /* straight, north-south  */
    CN_T_SH,        /* straight, east-west    */
    CN_T_NE,        /* bends: N-E, E-S, S-W, W-N */
    CN_T_ES,
    CN_T_SW,
    CN_T_WN,
    CN_T_X,         /* cross */
    CN_TYPES
};

#define CN_F_FIXED  0x01    /* bolted down: it cannot be turned, placed or taken */
#define CN_F_PATH   0x02    /* part of the generator's solution */
#define CN_F_PLACED 0x04    /* fill mode: put there by the player */

typedef struct {
    uint8_t kind, rot, flags;
    uint8_t sol_kind, sol_rot;      /* the solution's piece, on the path */
} cn_cell_t;

typedef struct {
    uint8_t   mode, diff, world, level;
    uint8_t   cols, rows;
    cn_cell_t cell[CN_MAX_ROWS][CN_MAX_COLS];
    int8_t    src_c, src_r, drn_c, drn_r;
    uint8_t   tray[CN_TYPES];       /* fill mode: pieces left of each type */
    int16_t   path_len;             /* cells on the solution, source and drain out */
    int16_t   gaps;                 /* fill mode: pieces the solution lacks */
    int16_t   par_moves;            /* rotate mode: turns the solution needs */
    float     par_time;             /* seconds, for the third star in easy */
    float     countdown;            /* normal/hard: seconds until the valve opens */
} cn_level_t;

typedef struct {
    int8_t  c, r;
    uint8_t in;         /* side the liquid came in by; 0xFF at the source */
    uint8_t out;        /* side it leaves by (the drain's: none, 0xFF)    */
    uint8_t ch;         /* channel: 0, or 1 for a cross's east-west one   */
    float   start, len; /* cells along the whole run                     */
} cn_seg_t;

typedef enum {
    CN_PH_EDIT = 0,     /* the valve is shut                   */
    CN_PH_OPENING,      /* the wheel is turning                */
    CN_PH_FLOW,         /* the front is running                */
    CN_PH_DRAIN,        /* it reached the drain: filling it    */
    CN_PH_SPILL,        /* it leaked: pouring out              */
    CN_PH_WON,
    CN_PH_LOST,
} cn_phase_t;

/* what a step reports */
#define CN_EV_OPEN      0x01    /* the wheel started turning (by itself or not) */
#define CN_EV_FLOW      0x02    /* the liquid came out of the source            */
#define CN_EV_PIPE      0x04    /* a pipe filled                                */
#define CN_EV_LEAK      0x08
#define CN_EV_DRAIN     0x10
#define CN_EV_WON       0x20
#define CN_EV_LOST      0x40
#define CN_EV_SECOND    0x80    /* the countdown went past a whole second       */
#define CN_EV_CROSS2    0x100   /* a cross was crossed the second time          */

typedef struct {
    cn_level_t lv;
    uint8_t    phase;
    bool       fast;            /* fast-forward */
    bool       auto_opened;     /* the countdown ran out */
    float      t_edit;          /* seconds played before opening */
    float      countdown;       /* left */
    float      valve;           /* the wheel, 0..1 while opening */
    float      front;           /* cells from the source */
    float      speed;           /* cells/s right now */
    float      flow_t;          /* time the liquid has been moving */
    float      end_t;           /* time in DRAIN or SPILL */
    cn_seg_t   seg[CN_MAX_SEGS];
    int        nseg;
    int16_t    segi[CN_MAX_ROWS][CN_MAX_COLS][2];   /* -1: dry */
    int        moves;           /* turns (rotate) or pieces put down (fill) */
    int        filled;          /* pipes filled */
    int        cross2;          /* crosses crossed twice */
    int        score, stars;
    int8_t     leak_c, leak_r;  /* the cell the liquid leaks out of */
    uint8_t    leak_dir;        /* and by which side */
} cn_game_t;

/* Openings of a piece as a mask of 1 << CN_N... */
uint8_t cn_open(uint8_t kind, uint8_t rot);
/* The tray type of a pipe, or -1. And back. */
int  cn_type_of(uint8_t kind, uint8_t rot);
void cn_type_piece(int type, uint8_t *kind, uint8_t *rot);

/* Builds level 'level' (0-based) of a world; the same arguments make the
 * same level. */
void cn_level_make(cn_level_t *lv, int mode, int diff, int world, int level);

void cn_game_start(cn_game_t *g, int mode, int diff, int world, int level);
float cn_game_base_speed(const cn_game_t *g);

/* Taps. Each returns true when it changed something. */
bool cn_game_editable(const cn_game_t *g, int c, int r);
bool cn_game_rotate(cn_game_t *g, int c, int r);
bool cn_game_place(cn_game_t *g, int c, int r, int type);
int  cn_game_take(cn_game_t *g, int c, int r);          /* the type taken, or -1 */
bool cn_game_open(cn_game_t *g);                         /* opens the valve */
void cn_game_set_fast(cn_game_t *g, bool on);

/* Puts the solution on the board (for trying the game out). */
void cn_game_solve(cn_game_t *g);

/* Advances 'dt' seconds; returns CN_EV_* bits. */
uint32_t cn_game_step(cn_game_t *g, float dt);

/* The cell has liquid in some channel. */
static inline bool cn_game_wet(const cn_game_t *g, int c, int r)
{
    return g->segi[r][c][0] >= 0 || g->segi[r][c][1] >= 0;
}
