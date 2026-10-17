/*
 * PINBALL - the ball and what it meets
 *
 * Two dimensions on the playfield (the table's millimetres, y down the
 * slope) stepped in substeps of a millisecond, so a ball at 5 m/s moves
 * 5 mm a step, under half its radius: nothing is crossed without being met.
 * Ramps are the third dimension: on one, the ball follows the ramp's path
 * in (x, y, z) at a speed gravity and friction change, and comes off it at
 * the end or back at the mouth.
 *
 * The physics knows shapes and what kind of object each belongs to; what a
 * hit is worth is the rules' (pb_game.c), told through events.
 */
#pragma once

#include "pb_table.h"

#include <stdbool.h>
#include <stdint.h>

#define PB_MAX_BALLS 4
#define PB_MAX_EV    48

enum { BS_OFF, BS_PLUNGER, BS_FREE, BS_RAMP, BS_SAUCER, BS_HELD };

enum {
    EV_HIT,         /* a bumper, sling, target, drop, post with an object   */
    EV_ROLL,        /* a rollover (lane, inlane, outlane, orbit)            */
    EV_SPIN,        /* the spinner turned half a turn                       */
    EV_SAUCER,      /* a ball sits in the saucer                            */
    EV_EJECT,       /* ... and left it                                      */
    EV_RAMP_IN,
    EV_RAMP_DONE,
    EV_RAMP_FAIL,
    EV_KICKBACK,
    EV_DRAIN,
    EV_FLIP,        /* a flipper went up (the sound)                        */
    EV_WALL,        /* a hard knock on a wall (a sound), obj = -1           */
    EV_LAUNCH,
    EV_TOY_HIT,     /* a bash toy or a mover was hit; obj is the toy's object */
    EV_DISC_IN,     /* the ball got onto a disc                             */
    EV_DISC_OUT,    /* ... and left it; speed holds the angle it left at, on
                     * the disc (radians, 0..2pi), for a roulette's number  */
    EV_CANNON_IN,   /* the cannon has the ball                              */
    EV_CANNON_FIRE,
    EV_MAGNET_GRAB,
    EV_MAGNET_THROW,
};

typedef struct {
    uint8_t kind;
    int8_t  ball;
    int16_t obj;
    float   speed;
} pb_event_t;

typedef struct {
    uint8_t  state;
    float    x, y, z;
    float    vx, vy;
    float    px, py;            /* where it was a substep ago (triggers)      */
    int8_t   ramp;
    float    rs, rv;            /* on a ramp: how far along, how fast         */
    int16_t  saucer;            /* the saucer it sits in                      */
    float    hold;              /* seconds left in it                         */
    int16_t  saucer_skip;       /* the saucer it just left (no recapture)     */
    uint32_t inside[2];         /* the rollovers it is over (two words: a 64-bit shift is a libgcc call the board lacks) */
    float    still;             /* seconds it has barely moved                */
    float    roll;              /* how far it rolled: the sprite turns        */
    int8_t   holder;            /* BS_HELD: the toy (cannon, magnet) holding it */
    int8_t   on_disc;           /* the disc it is riding, -1 none              */
    bool     vanish;            /* when its saucer is done it leaves the table (a toy swallowed it) */
} pb_ball_t;

typedef struct {
    float ang, omega;
    bool  held;
} pb_flip_t;

typedef struct {
    float   level;              /* bash, mover: 1 up, 0 sunk; it moves to 'up'  */
    bool    up;
    float   anim;               /* seconds since the last hit (the wobble)      */
    float   phase;              /* mover: 0..1 along its path; disc, cannon: angle (rad) */
    float   omega;              /* disc: rad/s now                              */
    float   target;             /* disc: rad/s wanted                           */
    bool    on;                 /* magnet: pulling                              */
    float   x, y;               /* where it is (a mover moves)                  */
    float   cool;
} pb_toy_t;

typedef struct {
    const pb_table_t *t;
    pb_ball_t ball[PB_MAX_BALLS];
    pb_flip_t flip[PB_MAX_FLIP];
    pb_toy_t  toy[PB_MAX_TOY];
    uint8_t   keep[PB_MAX_OBJ];     /* saucers that keep their ball (a lock)  */
    uint8_t   down[PB_MAX_OBJ];     /* drop targets down                     */
    float     cool[PB_MAX_OBJ];     /* a bumper or sling just fired          */
    float     spin_ang[PB_MAX_OBJ]; /* spinners                              */
    float     spin_vel[PB_MAX_OBJ];
    bool      kickback_lit;
    bool      tilted;
    float     saucer_hold;          /* seconds a ball stays in the saucer    */
    pb_event_t ev[PB_MAX_EV];
    int       nev;
    float     acc;                  /* time not yet stepped                  */
    uint32_t  kicks;                /* stuck balls shaken loose (the log)    */
    float     kick_x, kick_y;       /* where the last one was                */
    uint32_t  rng;
    /* the shapes near each cell of a grid over the table */
    int       gw, gh;
    uint16_t *cell_at;              /* gw*gh+1 offsets into cell_ref          */
    uint16_t *cell_ref;             /* bit 15: a circle; else a segment      */
} pb_phys_t;

bool pb_phys_init(pb_phys_t *p, const pb_table_t *t);
void pb_phys_free(pb_phys_t *p);
/* everything back as a new game finds it (drops up, no balls) */
void pb_phys_reset(pb_phys_t *p);
/* steps 'dt' seconds; events pile up in p->ev until the caller clears nev */
void pb_phys_step(pb_phys_t *p, float dt);

/* a new ball on the plunger; -1 when there is no room */
int  pb_phys_serve(pb_phys_t *p);
/* the ball on the plunger goes, power 0..1 */
bool pb_phys_launch(pb_phys_t *p, float power);
/* a ball waits on the plunger */
int  pb_phys_on_plunger(const pb_phys_t *p);
int  pb_phys_balls(const pb_phys_t *p);         /* on the table, any state */
void pb_phys_nudge(pb_phys_t *p, float dx, float dy);
void pb_phys_flipper(pb_phys_t *p, int side, bool held);
void pb_phys_drops_up(pb_phys_t *p, const char *bank_prefix);
/* a ball out of the saucer now, wherever its hold is */
void pb_phys_saucer_release(pb_phys_t *p);
/* where flipper i is between rest (0) and up (1) */
float pb_phys_flip_t(const pb_phys_t *p, int i);

/* the toys: raise or sink a bash toy or a mover, a magnet on or off, a
 * disc's speed, the cannon's shot */
void pb_phys_toy_up(pb_phys_t *p, int toy, bool up);
void pb_phys_magnet(pb_phys_t *p, int toy, bool on);
void pb_phys_disc_speed(pb_phys_t *p, int toy, float rad_s);
/* the cannon holding a ball, or -1 */
int  pb_phys_cannon_loaded(const pb_phys_t *p);
void pb_phys_cannon_fire(pb_phys_t *p);
/* a saucer that keeps the ball that falls in (a lock you can see) */
void pb_phys_saucer_keep(pb_phys_t *p, int obj, bool keep);
/* balls sitting in saucers that keep them */
int  pb_phys_kept(const pb_phys_t *p);
