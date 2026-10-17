/*
 * PINBALL - a table: what the "table" entry of its pack says
 *
 * The definition is text, one record per line (tools/pbtable.py writes
 * it, Table.def_text()). The game knows object TYPES (how the ball meets
 * them) and ROLES (what a hit means to the rules); a new table is a new
 * pack, never new code, as long as it uses these.
 *
 * Table coordinates: millimetres, x to the right, y down the slope (0 is
 * the far end), z up from the playfield.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PB_MAX_OBJ      128
#define PB_MAX_SEG      640
#define PB_MAX_CIRC     96
#define PB_MAX_TRIG     24
#define PB_MAX_ROLL     48
#define PB_MAX_LAMP     72
#define PB_MAX_RAMP     4
#define PB_MAX_RAMP_PTS 40
#define PB_MAX_MISSION  8
#define PB_MAX_TEXT     16
#define PB_TXT          48          /* one line of a string, UTF-8 */
#define PB_LANGS        3           /* es, en, de */

enum {
    OT_WALL, OT_POST, OT_BUMPER, OT_SLING, OT_DROP, OT_TARGET, OT_ROLLOVER, OT_SPINNER, OT_SAUCER,
    OT_RAMP, OT_GATE, OT_KICKBACK, OT_FLIPPER, OT_N
};

enum {
    RL_NONE, RL_LANE, RL_INLANE, RL_OUTLANE, RL_BUMPER, RL_SLING, RL_DROP, RL_TARGET, RL_SPINNER,
    RL_SAUCER, RL_RAMP, RL_ORBIT, RL_KICKBACK, RL_POST, RL_N
};

/* what a mission asks for */
enum { GL_BUMPER, GL_SLING, GL_DROP, GL_TARGET, GL_SPINNER, GL_RAMP, GL_ORBIT, GL_LANE, GL_SAUCER, GL_ANY, GL_N };

/* physical materials (pb_phys.c has their restitution and friction) */
enum { MT_WALL, MT_RUBBER, MT_POST, MT_PLASTIC, MT_METAL, MT_FLIPPER, MT_N };

enum { LK_INSERT, LK_BUMPER, LK_SLING, LK_FLASHER };

typedef struct {
    uint8_t type, role;
    int8_t  index;          /* among the objects of its role, in order      */
    int8_t  bank;           /* drop targets: which bank                    */
    int16_t lamp;           /* a lamp of the same name, or -1             */
    float   a, b;           /* kick; eject angle and speed; ramp's min speed; kickback's vx, vy */
    char    name[16];
} pb_obj_t;

typedef struct {
    float   x0, y0, x1, y1;
    float   dx, dy;         /* unit direction a -> b                        */
    float   len;
    uint8_t mat;
    int16_t obj;            /* -1: a plain wall                             */
} pb_seg_t;

typedef struct {
    float   x, y, r;
    uint8_t mat;
    int16_t obj;
} pb_circ_t;

typedef struct {
    float   x0, y0, x1, y1, dx, dy, len;
    int16_t obj;
} pb_trig_t;

typedef struct {
    float   x, y, r;
    int16_t obj;
} pb_roll_t;

typedef struct {
    uint8_t side;           /* 0 left, 1 right                              */
    float   x, y, len, r0, r1;
    float   rest, up;       /* direction pivot -> tip, radians, y down       */
} pb_flipdef_t;

typedef struct {
    int16_t obj;
    float   exit_speed;
    int     n;
    float   x[PB_MAX_RAMP_PTS], y[PB_MAX_RAMP_PTS], z[PB_MAX_RAMP_PTS], s[PB_MAX_RAMP_PTS];
    float   total;
} pb_ramp_t;

typedef struct {
    char     name[16];
    uint8_t  kind;
    int16_t  sx, sy;        /* where it is on the screen (glow, flash)       */
    uint32_t color;
} pb_lampdef_t;

typedef struct {
    uint8_t  goal;
    int16_t  count, secs;
    uint32_t points;
    char     name[PB_LANGS][PB_TXT];
} pb_mission_t;

typedef struct {
    char key[12];
    char s[PB_LANGS][PB_TXT];
} pb_text_t;

typedef struct {
    float f, cx, cy;        /* screen pixels                                 */
    float px, py, pz;       /* the camera, table coordinates                 */
    float pitch, sp, cp;
} pb_cam_t;

typedef struct {
    char     id[16];
    int      order;
    char     name[PB_LANGS][PB_TXT];
    char     desc[PB_LANGS][128];
    float    w, h, grav, ball_r;
    pb_cam_t cam;
    float    plunger_x, plunger_y;
    uint32_t c_dmd, c_bg, c_accent, c_panel, c_text;

    int          nobj, nseg, ncirc, ntrig, nroll, nflip, nramp, nlamp, nmission, ntext;
    pb_obj_t     obj[PB_MAX_OBJ];
    pb_seg_t     seg[PB_MAX_SEG];
    pb_circ_t    circ[PB_MAX_CIRC];
    pb_trig_t    trig[PB_MAX_TRIG];
    pb_roll_t    roll[PB_MAX_ROLL];
    pb_flipdef_t flip[2];
    pb_ramp_t    ramp[PB_MAX_RAMP];
    pb_lampdef_t lamp[PB_MAX_LAMP];
    pb_mission_t mission[PB_MAX_MISSION];
    pb_text_t    text[PB_MAX_TEXT];
    int          role_n[RL_N];  /* how many objects of each role            */
} pb_table_t;

/* parses the definition; false (and a log line) on what it cannot read.
 * 'only_head': the name, colours and order only (the carousel) */
bool pb_table_parse(pb_table_t *t, const char *text, bool only_head);

int  pb_table_find_obj(const pb_table_t *t, const char *name);
int  pb_table_find_lamp(const pb_table_t *t, const char *name);
const char *pb_table_text(const pb_table_t *t, const char *key, int lang);

/* the current language: 0 es, 1 en, 2 de (others read English) */
int  pb_lang(void);

/* the camera */
void  pb_project(const pb_cam_t *c, float x, float y, float z, float *sx, float *sy, float *depth);
/* depth (mm) of the plane z = h seen through screen row 'sy' */
float pb_plane_depth(const pb_cam_t *c, float sy, float h);
