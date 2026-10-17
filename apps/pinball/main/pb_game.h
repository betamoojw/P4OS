/*
 * PINBALL - the rules: what every hit is worth, the lamps, the missions,
 * multiball, the DMD's words and the sounds to play
 *
 * One ruleset for every table, the way real machines share their bones:
 *   - the top lanes light up (the flippers move the lit ones); all three
 *     raise the bonus multiplier (up to x5); a launch that rolls through
 *     the blinking one is a skill shot;
 *   - the inlanes, both lit, light the kickback in the left outlane;
 *   - the pop bumpers grow in value every 25 hits;
 *   - the drop targets, all down, light a mission at the saucer;
 *   - the saucer starts the lit mission: the table's own, each one asking
 *     for N hits of one kind (bumpers, ramps, orbits...) in so many seconds;
 *   - the standup targets, all hit, light the lock at the ramp; two locks
 *     start multiball (three balls), with the jackpot at the ramp and the
 *     saucer while it lasts;
 *   - the orbits double the spinner (up to x4) and are combos after a ramp;
 *   - five ramps light the extra ball at the saucer;
 *   - every mission played lights the next; all of them start the final
 *     mode (a multiball where everything scores five times);
 *   - a ball saver for the first seconds of each ball, the tilt for a
 *     table shaken too much, the bonus at the end of each ball.
 * The table names its objects by role (pb_table.h) and its lamps by name
 * (README, "Lamps"); a lamp the table lacks is simply not shown.
 */
#pragma once

#include "pb_dmd.h"
#include "pb_video.h"
#include "pb_phys.h"
#include "pb_table.h"

#include <stdbool.h>
#include <stdint.h>

/* sounds: the pack's "s_<name>" (pb_audio.c), in this order */
enum {
    SND_FLIPPER, SND_BUMPER, SND_SLING, SND_DROP, SND_BANK, SND_TARGET, SND_LANE, SND_LANES, SND_SPINNER,
    SND_SAUCER, SND_EJECT, SND_RAMP, SND_RAMP_FAIL, SND_ORBIT, SND_LAUNCH, SND_DRAIN, SND_SAVE, SND_KICKBACK,
    SND_MISSION, SND_MISSION_DONE, SND_JACKPOT, SND_MULTIBALL, SND_LOCK, SND_EXTRA, SND_TILT_WARN, SND_TILT,
    SND_SKILL, SND_BONUS, SND_GAMEOVER, SND_START, SND_WALL, SND_POST,
    SND_TOY_HIT, SND_TOY_DOWN, SND_VIDEO, SND_CANNON, SND_MAGNET, SND_DISC, SND_LETTER, SND_VIDEO_HIT, SND_N
};
extern const char *const PB_SND_NAMES[SND_N];

enum { GS_ATTRACT, GS_PLAY, GS_BONUS, GS_OVER };

enum { LM_OFF, LM_ON, LM_SLOW, LM_FAST, LM_FLASH };

#define PB_BALLS        3
#define PB_MSG_MAX      6
#define PB_SND_Q        16

typedef struct {
    char  a[PB_TXT * 2], b[PB_TXT * 2];
    float secs;
    uint8_t big;            /* 1: line a at twice the size                 */
    uint8_t blink;
} pb_msg_t;

/* the table's own attractions (its "feat" lines), as they stand in a game */
enum { FK_BASH, FK_LOCKPEN, FK_LETTERS, FK_VIDEO, FK_ROULETTE, FK_CANNON, FK_MAGNET, FK_DISC, FK_N };

typedef struct {
    uint8_t  kind;
    int16_t  toy, obj;          /* the toy, the saucer it uses                */
    int      need, need0, count, round;
    uint32_t points;
    float    t;                 /* a timer of its own                         */
    float    every, on_s;
    uint32_t mask;              /* letters lit                                */
    int      nobjs;
    int16_t  objs[8];
    char     word[12];
    char     text[12];          /* the table's word for it (its "text" lines) */
    char     text_hit[12], text_done[12], text_back[12];
    uint8_t  lights;            /* what it lights: 0 none, 1 video, 2 lock, 3 multiball */
    uint8_t  need_lit;          /* lockpen: only while the lock is lit        */
} pb_fstate_t;

typedef struct {
    const pb_table_t *t;
    pb_phys_t *ph;
    pb_fstate_t feat[PB_MAX_FEAT];
    int      nfeat;
    pb_video_t video;
    bool     video_lit;
    int      video_feat;        /* the video feature playing, -1 none         */
    int      video_ball;
    int      video_hits;        /* the hits already heard                     */
    bool     in_lp, in_rp, in_lh, in_rh;   /* the flipper buttons, for the video modes */
    float    cannon_t;          /* since the cannon fired (its targets count)  */
    int      roulette_bet;      /* 0 red, 1 black                              */
    float    letters_show;      /* the word on the DMD for a moment            */
    int      letters_feat;
    int      lang;
    int      state;
    float    clock;             /* seconds since the game began            */
    uint32_t score, best;
    int      ball;              /* 1..PB_BALLS                              */
    int      extra_balls;
    bool     extra_lit, extra_given;
    uint32_t bonus;
    int      bonus_x;
    uint8_t  lanes;             /* lit top lanes                           */
    uint8_t  inlanes;
    int      skill_lane;        /* blinking at the launch, -1 none          */
    float    skill_t;
    int      bumper_hits, bumper_level;
    int      spin_x;
    int      ramps;
    uint32_t targets;           /* standups hit, a bit each                */
    bool     lock_lit;
    int      locks;
    bool     multiball, wizard;
    int      to_serve;          /* balls still to come out by themselves     */
    bool     auto_on_plunger;   /* the one on the plunger goes by itself      */
    float    mb_serve_t;
    float    bank_t;            /* a bank down: when it comes back up         */
    char     bank_name[16];
    uint32_t jackpot;
    int      mission_lit;       /* the mission the saucer starts, -1 none   */
    int      mission;           /* the one running, -1 none                 */
    int      mission_left;
    float    mission_t;
    uint32_t missions_done;
    int      mission_next;
    float    ball_save;         /* seconds left                            */
    bool     ball_saved_once;
    float    tilt;              /* the shaking, decays                     */
    int      tilt_warns;
    bool     tilted;
    float    orbit_t[2];        /* when each orbit sensor saw the ball     */
    float    combo_t;           /* a ramp was made: an orbit soon is a combo */
    float    end_t;             /* the bonus count / game over pause         */
    uint32_t bonus_shown;
    bool     need_serve;
    bool     launched;          /* the ball on the plunger has gone once     */
    float    attract_t;
    /* lamps */
    uint8_t  lamp_mode[PB_MAX_LAMP];
    float    lamp_flash[PB_MAX_LAMP];
    bool     lamp_on[PB_MAX_LAMP];      /* what the renderer shows          */
    /* the DMD */
    pb_msg_t msg[PB_MSG_MAX];
    int      nmsg;
    float    msg_t;
    int      scroll;
    /* sounds waiting for the mixer */
    uint8_t  snd[PB_SND_Q];
    int      nsnd;
    bool     new_best;
    bool     flash_req;         /* something big: the flashers fire         */
} pb_game_t;

void pb_game_init(pb_game_t *g, const pb_table_t *t, pb_phys_t *ph, uint32_t best);
void pb_game_start(pb_game_t *g);
/* after the physics' step: reads its events, runs the timers */
void pb_game_update(pb_game_t *g, float dt);
/* the player pressed a flipper (the lit lanes move) */
void pb_game_flipper(pb_game_t *g, int side);
/* the table was shaken: true if it counted */
bool pb_game_nudge(pb_game_t *g);
/* what the DMD shows now */
void pb_game_dmd(pb_game_t *g, pb_dmd_t *d);
/* the plunger is waiting for the player (the launch button shows), or the
 * cannon is loaded and waits to fire */
bool pb_game_wants_launch(const pb_game_t *g);
/* a video mode is on the DMD: the flipper buttons are its */
bool pb_game_video(const pb_game_t *g);
/* the flipper buttons this frame: went down, held */
void pb_game_buttons(pb_game_t *g, bool lp, bool rp, bool lh, bool rh);
void pb_game_sound(pb_game_t *g, int snd);
void pb_game_say(pb_game_t *g, const char *a, const char *b, float secs, bool big);
