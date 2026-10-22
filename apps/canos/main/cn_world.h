/*
 * CAÑOS - the worlds: what runs through the pipes, and what the pipes and
 * the floor are made of.
 *
 * Each world is one liquid with its own way of moving (how fast it runs,
 * how round its front is, how much it wobbles) and its own look (the colour
 * ramp, caustics, bubbles, foam, a fuel's rainbow sheen, lava's crust), and
 * one plant to hold it: the pipes' material and the floor under them. The
 * numbers here are the whole difference between the worlds; the drawing
 * code reads them and has no world of its own.
 */
#pragma once

#include <stdint.h>

#define CN_WORLDS 6

/* what the pipes are made of (cn_draw.c draws each) */
enum {
    CN_PIPE_STEEL = 0,      /* painted steel, plain couplings               */
    CN_PIPE_BANDS,          /* steel with hazard bands at the couplings      */
    CN_PIPE_GLASS,          /* lab glass: the liquid shows through the wall  */
    CN_PIPE_RUST,           /* old iron, pitted                              */
    CN_PIPE_CERAMIC,        /* refractory channel                           */
    CN_PIPE_FROST,          /* chrome under a crust of frost                 */
};

/* the floor under the pipes */
enum {
    CN_GROUND_TILES = 0,    /* wet concrete tiles  */
    CN_GROUND_TREAD,        /* diamond tread plate */
    CN_GROUND_LAB,          /* lab bench tiles     */
    CN_GROUND_SAND,         /* packed earth        */
    CN_GROUND_BASALT,       /* basalt with glowing cracks */
    CN_GROUND_ICE,          /* icy steel           */
};

/* what a leak throws up besides the drops */
enum {
    CN_SPILL_SPLASH = 0,
    CN_SPILL_FUMES,         /* acid: green fumes          */
    CN_SPILL_SPARKS,        /* lava: sparks and smoke     */
    CN_SPILL_MIST,          /* nitrogen: a cold mist      */
};

typedef struct {
    const char *fluid;          /* N_(): "Agua"                  */
    const char *place;          /* N_(): "Planta de agua"        */
    const char *leak;           /* N_(): what a leak looks like  */

    /* the liquid's colour ramp, dark to light, and the highlight */
    uint32_t c_deep, c_mid, c_light, c_spec;

    float speed;                /* run speed against water's (viscosity)       */
    float alpha;                /* opacity over the empty pipe, 0..1           */
    float curv;                 /* how far the front's middle leads its sides,
                                 * in channel radii (Poiseuille's parabola)    */
    float wobble;               /* the front's wobble, in px at a 100 px cell  */
    float spread;               /* how fast a puddle spreads (diffusion)       */
    float drop_speed;           /* how far the drops fly, cells/s              */

    uint8_t tex;                /* streaks along the flow, 0..255              */
    uint8_t caustics;           /* bright moving lines (clear liquids)         */
    uint8_t bubbles;            /* how many bubbles ride along                 */
    uint8_t bubble_r;           /* their radius, px at a 100 px cell           */
    uint8_t fizz;               /* tiny fast bubbles (acid, boiling nitrogen)  */
    uint8_t foam;               /* white at the front                          */
    uint8_t sheen;              /* a fuel's rainbow film                       */
    uint8_t crust;              /* dark floating crust (lava)                  */
    uint8_t spec;               /* the tube's highlight on the liquid          */
    uint8_t glow;               /* emissive: lights its own puddle             */

    uint8_t pipe, ground, spill;
    uint32_t p_hi, p_mid, p_lo; /* pipe metal, lit to shadowed                 */
    uint32_t p_inner;           /* inside an empty pipe                        */
    uint32_t p_accent;          /* bands, bolts, the valve's wheel             */
    uint32_t g_a, g_b, g_line;  /* floor: two tones and its seams             */

    uint32_t ui_a, ui_b;        /* the world's card and HUD gradient           */
} cn_world_t;

const cn_world_t *cn_world(int i);
