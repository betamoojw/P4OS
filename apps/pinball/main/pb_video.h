/*
 * PINBALL - the video modes: little games on the DMD, played with the
 * flipper buttons while the ball waits in a saucer, the way the machines of
 * the nineties did (Attack from Mars' saucer, Indiana Jones' Path of
 * Adventure, Twilight Zone's slot machine). A table names the one it has
 * (its "video" feature); pb_game.c starts it and pays what it scores.
 *
 *   shoot     zombies rise from three graves: the left flipper shoots the
 *             left one, the right the right one, both the middle
 *   invaders  a ship at the bottom moved by the flippers, firing by itself
 *   runner    three paths and falling rocks: the flippers change path,
 *             collect the idols
 *   darts     balloons rise; the flippers move the thrower, both throw
 *   slots     three reels; each flipper press stops the next one
 *   duel      the enemy shows where he will strike; parry with the flipper
 *             on that side (left high, right low) before he does
 */
#pragma once

#include "pb_dmd.h"

#include <stdbool.h>
#include <stdint.h>

enum { VM_SHOOT, VM_INVADERS, VM_RUNNER, VM_DARTS, VM_SLOTS, VM_DUEL, VM_N };

#define VM_ITEMS 12

typedef struct {
    int16_t x, y;       /* dots, x16 for the sub-dot moves where needed   */
    int8_t  kind, life;
    float   t;
} pb_vitem_t;

typedef struct {
    int      kind;
    bool     on;
    float    t, dur;
    uint32_t score;
    int      hits, lives, misses;
    int      px;            /* the player's x (dots)                      */
    int      lane;
    float    next, fire;
    int      reel[3];
    int      stopped;
    float    spin[3];
    int      enemy_hp, threat;  /* duel: 0 none, 1 high, 2 low            */
    float    threat_t;
    pb_vitem_t it[VM_ITEMS];
    uint32_t rng;
    float    end_t;          /* the result shown, then over               */
    int      result;         /* slots: what came up                        */
} pb_video_t;

void pb_video_start(pb_video_t *v, int kind, uint32_t seed);
/* one step; lp, rp: a flipper button went down; lh, rh: held.
 * false once the game is over (v->score is what it made) */
bool pb_video_step(pb_video_t *v, float dt, bool lp, bool rp, bool lh, bool rh);
void pb_video_draw(const pb_video_t *v, pb_dmd_t *d, int lang);
int  pb_video_kind(const char *name);
