/*
 * P4OS - Emulador: what the app's own files share.
 *
 *   emulador.c   the app: life cycle, icon, which screen is up
 *   em_lib.c     the library: consoles, games, the one to continue
 *   em_game.c    the game screen: the picture, the pad on screen, the
 *                gamepads, presenting frames (LVGL's task)
 *   em_menu.c    the pause menu: states, options, reset, quit
 *   em_run.c     the console running in the worker (core 0): pacing,
 *                sound, frame skip, commands; never touches LVGL
 *   em_files.c   the card: folders, games, saves, states, thumbnails,
 *                recents, settings
 *   emu_cores.c  the consoles in this build
 */
#pragma once

#include "emu_core.h"

#include "aos_app.h"
#include "lvgl.h"
#include "aos_sys_glyphs.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EMU_TAG "emulador"

/* The consoles there are, ending with NULL (emu_cores.c): on the board one
 * "shadow" per table of every module on the card (metadata only, no
 * functions), in the simulator the linked-in tables. emu_cores_scan()
 * fills it when the app opens. */
extern const emu_core_t *const *emu_cores;
void emu_cores_scan(void);
void emu_cores_free(void);
const emu_core_t *emu_core_by_id(const char *id);
/* The real table for a game: loads the console's module (the board), NULL
 * with a reason in err. emu_core_close() lets it go: no pointer into it may
 * outlive the game. */
const emu_core_t *emu_core_open(const emu_core_t *shadow, char *err, size_t errlen);
void emu_core_close(void);
int emu_core_code(const emu_core_t *core);      /* stable: for settings, never 0 for a known one */

/* Memory in PSRAM (malloc in the simulator); free() frees it. */
void *emu_psram(size_t n);

/* ---------------------------------------------------------------------------
 * The card (em_files.c)
 * ------------------------------------------------------------------------ */

#define EMU_PATH_MAX    320
#define EMU_NAME_MAX    128
#define EMU_SLOTS       4               /* manual save-state slots, 1..4; 0 is the automatic one */

/* /sdcard/emulador, and a console's folder in it (/sdcard/emulador/nes). */
const char *emu_root(void);
void emu_console_dir(const emu_core_t *core, char *out, size_t n);
/* Makes the root and every console's folder, with a LEEME.txt in each. */
void emu_folders_make(void);

typedef struct {
    const emu_core_t *core;
    char  rel[EMU_PATH_MAX];            /* the path under the console's folder  */
    char  title[EMU_NAME_MAX];          /* the name without extension and tags  */
    char  tags[48];                     /* "(USA)", "[!]"... as one line        */
    uint32_t size;
    uint32_t last;                      /* recents' order, 0 = never played     */
    uint32_t played_s;                  /* seconds played                       */
    bool  has_auto;                     /* an automatic state to continue from  */
} emu_game_t;

/* Every game on the card for 'core' (NULL: all consoles). The list is the
 * caller's to free with emu_games_free(). */
typedef struct {
    emu_game_t *v;
    int         n, cap;
} emu_games_t;
int  emu_games_scan(const emu_core_t *core, emu_games_t *out);
void emu_games_sort(emu_games_t *g, bool by_recent);
void emu_games_free(emu_games_t *g);

/* Whether a file name has one of the console's extensions. */
bool emu_ext_match(const emu_core_t *core, const char *name);

/* Zipped games (em_zip.c): the console's file inside is inflated once into
 * <console>/.cache/ and 'out' gets its path. */
bool emu_is_zip(const char *path);
bool emu_unzip_rom(const emu_game_t *g, const char *zip, char *out, size_t n, char *err, size_t errlen);

/* A game's files. 'slot' 0 is the automatic state. */
void emu_game_rom(const emu_game_t *g, char *out, size_t n);
void emu_game_save(const emu_game_t *g, char *out, size_t n);
void emu_game_state(const emu_game_t *g, int slot, char *out, size_t n);
void emu_game_thumb(const emu_game_t *g, int slot, char *out, size_t n);
void emu_game_key(const emu_game_t *g, char *out, size_t n);   /* "nes/Some Game.nes" */
bool emu_file_exists(const char *path, uint32_t *mtime);

/* Recents: when each game was last played and for how long, in
 * /sdcard/emulador/recientes.txt. */
void emu_recent_touch(const emu_game_t *g, uint32_t add_seconds);
void emu_recent_apply(emu_games_t *g);
bool emu_recent_last(emu_game_t *out);                  /* the last game played */

/* Thumbnails: a picture written next to each state (RGB565, half size). */
bool emu_thumb_write(const char *path, const uint16_t *px, int w, int h, int stride);
/* Read into an LVGL image descriptor whose data the caller frees with
 * emu_thumb_free(). */
bool emu_thumb_read(const char *path, lv_image_dsc_t *dsc);
void emu_thumb_free(lv_image_dsc_t *dsc);

/* Settings, kept in the board's preferences ("emu.<key>" or
 * "emu.<console>.<key>"). */
int  emu_pref(const char *key, int def);
void emu_pref_set(const char *key, int value);
int  emu_core_pref(const emu_core_t *core, const char *key, int def);
void emu_core_pref_set(const emu_core_t *core, const char *key, int value);

/* The app's settings, in one place so the menu and the game agree. */
enum { EMU_SCALE_FIT = 0, EMU_SCALE_INT, EMU_SCALE_FILL };
enum { EMU_SKIP_AUTO = 0, EMU_SKIP_0, EMU_SKIP_1, EMU_SKIP_2 };

/* ---------------------------------------------------------------------------
 * The console in the worker (em_run.c)
 * ------------------------------------------------------------------------ */

typedef enum {
    EMU_RUN_IDLE = 0,
    EMU_RUN_LOADING,
    EMU_RUN_RUNNING,
    EMU_RUN_ERROR,
} emu_run_state_t;

typedef enum {
    EMU_CMD_NONE = 0,
    EMU_CMD_SAVE,           /* arg: slot  */
    EMU_CMD_LOAD,           /* arg: slot  */
    EMU_CMD_RESET,
    EMU_CMD_OPTION,         /* key + arg  */
    EMU_CMD_FLUSH,          /* the battery save */
} emu_cmd_t;

/* Starts the worker and loads the game in it. Returns at once; the state
 * goes LOADING -> RUNNING (or ERROR, with emu_run_error()). */
bool emu_run_start(const emu_core_t *core, const emu_game_t *game);
/* Saves the automatic state (when 'save_auto'), writes the battery save,
 * unloads and stops the worker. Blocking; from LVGL's task. */
void emu_run_stop(bool save_auto);
emu_run_state_t emu_run_state(void);
float emu_run_fps(void);        /* the game's frames a second, once RUNNING */
const char *emu_run_error(void);

void emu_run_pause(bool paused);
bool emu_run_paused(void);
void emu_run_pads(const uint32_t pads[EMU_MAX_PLAYERS]);
void emu_run_turbo(bool on);
void emu_run_sound(bool on);
void emu_run_skip(int mode);    /* EMU_SKIP_* */

/* A command for the worker, done between two frames. Returns a ticket;
 * emu_run_done() says whether it finished and how. */
uint32_t emu_run_command(emu_cmd_t cmd, int arg, const char *key);
bool     emu_run_done(uint32_t ticket, bool *ok);

/* The newest frame the console drew, for the screen: NULL when there is
 * none since the last take. Release it once blitted. */
const uint16_t *emu_frame_take(int *w, int *h, int *stride);
void emu_frame_release(void);

typedef struct {
    float    fps_emu;           /* console frames a second             */
    float    fps_drawn;         /* of them, drawn                      */
    float    run_ms;            /* the core's time per frame, average  */
    float    run_ms_max;
    uint32_t skipped;           /* frames not drawn in the last period */
    uint32_t underruns;         /* the speaker ran dry                 */
} emu_run_stats_t;
void emu_run_stats(emu_run_stats_t *out);   /* the last ~2 s */

/* ---------------------------------------------------------------------------
 * The screens
 * ------------------------------------------------------------------------ */

typedef struct emu_app emu_app_t;

/* The library (em_lib.c). */
void emu_lib_show(emu_app_t *a);
void emu_lib_hide(emu_app_t *a);
void emu_lib_resize(emu_app_t *a);
void emu_lib_tick(emu_app_t *a);
void emu_lib_destroy(emu_app_t *a);

/* The game (em_game.c). */
bool emu_game_open(emu_app_t *a, const emu_game_t *g);
void emu_game_close(emu_app_t *a, bool save_auto);     /* back to the library */
bool emu_game_is_open(emu_app_t *a);
void emu_game_resize(emu_app_t *a);
bool emu_game_back(emu_app_t *a);
void emu_game_destroy(emu_app_t *a);
void emu_game_tick(emu_app_t *a);                  /* the app's timer */
const emu_game_t *emu_game_current(emu_app_t *a);
void emu_game_resume(emu_app_t *a);                /* the menu closed */
void emu_game_relayout(emu_app_t *a);              /* a setting moved the picture */

/* The pause menu (em_menu.c), over the game. */
void emu_menu_open(emu_app_t *a);
void emu_menu_close(emu_app_t *a);
bool emu_menu_is_open(emu_app_t *a);
void emu_menu_step(emu_app_t *a);           /* every frame: the gamepad */
void emu_menu_resize(emu_app_t *a);

/* What the screens share. One instance at a time (the app is one). */
struct emu_app {
    aos_app_t  *self;
    lv_obj_t   *root;
    int         w, h;                       /* root's size              */

    lv_obj_t   *lib;                        /* the library's screen     */
    struct emu_lib   *L;
    struct emu_game  *G;
    struct emu_menu  *M;

    lv_timer_t *timer;
};

/* Colours of the app's own screens (the game's pad takes the console's). */
#define EMU_C_BG        0x0B0B12
#define EMU_C_CARD      0x171724
#define EMU_C_CARD2     0x222236
#define EMU_C_LINE      0x2E2E48
#define EMU_C_TEXT      0xF2F2F7
#define EMU_C_DIM       0x9090A8
#define EMU_C_ACCENT    0xFF2D95
#define EMU_C_ACCENT2   0x7B5CFF

/* A console's colours: the library's badge and the pad's body. */
typedef struct {
    uint32_t body, body2;       /* pad background; body2 the badge's lower half */
    uint32_t key, key_lit;      /* d-pad and small keys                         */
    uint32_t face[7];           /* the face buttons: A B X Y C Z D              */
    uint32_t badge;             /* the library's chip                           */
    uint32_t label;
} emu_skin_t;
const emu_skin_t *emu_skin(const emu_core_t *core);
