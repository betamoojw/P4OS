/*
 * P4OS - Emulador: what a console's core is to the app.
 *
 * Every console the app plays (NES, Game Boy Advance, and whatever comes
 * next) is a core behind this table. The app knows nothing of a console
 * but what the table says: its name, the files it takes, the buttons it
 * has, the size of its picture. A core knows nothing of P4OS but the host
 * table the app hands it: it never touches LVGL, the screen, the speaker
 * or the pads.
 *
 * Threads. load(), run(), reset(), the states and the battery saves are
 * all called from the app's worker (core 0), one at a time; never two
 * calls at once and never from LVGL's task. The host's callbacks are
 * called back from inside them, on the same thread.
 *
 * The cores live in cores/<id>/, each built as a library of its own
 * (cores/<id>/board.cmake and sim.cmake), so a core's own globals never
 * meet another's except through the names below, which are the only ones
 * a core exports: emu_core_<id>.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The buttons, one bit each and the same bit on every console; a core
 * reads the ones its console has and ignores the rest. */
enum {
    EMU_BTN_UP     = 1u << 0,
    EMU_BTN_DOWN   = 1u << 1,
    EMU_BTN_LEFT   = 1u << 2,
    EMU_BTN_RIGHT  = 1u << 3,
    EMU_BTN_A      = 1u << 4,
    EMU_BTN_B      = 1u << 5,
    EMU_BTN_X      = 1u << 6,
    EMU_BTN_Y      = 1u << 7,
    EMU_BTN_L      = 1u << 8,
    EMU_BTN_R      = 1u << 9,
    EMU_BTN_SELECT = 1u << 10,
    EMU_BTN_START  = 1u << 11,
    EMU_BTN_C      = 1u << 12,  /* Mega Drive, later */
    EMU_BTN_Z      = 1u << 13,
    EMU_BTN_L2     = 1u << 14,
    EMU_BTN_R2     = 1u << 15,
};
#define EMU_BTN_DPAD (EMU_BTN_UP | EMU_BTN_DOWN | EMU_BTN_LEFT | EMU_BTN_RIGHT)

#define EMU_MAX_PLAYERS 4

/* What the app gives a core. */
typedef struct {
    /* One picture: RGB565 in LVGL's byte order (little-endian, as the
     * screen takes it), w x h, rows 'stride' BYTES apart. The pixels are
     * read before the call returns: the core may reuse its buffer. Called
     * at most once per run(), and only when run() was asked to draw. */
    void (*video)(const uint16_t *px, int w, int h, int stride);

    /* Sound: 'frames' stereo frames, L R L R..., signed 16 bit, at the
     * rate sample_rate() reports. Any number of calls per run(). */
    void (*audio)(const int16_t *lr, int frames);

    /* To the board's log (and the simulator's terminal). No newline. */
    void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    /* Executable memory for a recompiler: NULL in the simulator, where a
     * core runs its interpreter instead. exec_publish() makes what was
     * written visible to instruction fetch (writes the data cache back,
     * drops the instruction cache's lines) and must be called before the
     * new code runs. */
    void *(*exec_alloc)(size_t align, size_t size);
    void  (*exec_free)(void *code);
    bool  (*exec_publish)(void *code, size_t size);
} emu_host_t;

/* What the app asks a core to load. Every path is a full one on the
 * card. */
typedef struct {
    const char *rom;            /* the game                                     */
    const char *bios_dir;       /* <console>/bios: optional files (GBA's BIOS,
                                   the Famicom Disk System's...), may be absent */
    const char *save;           /* the battery save for this game (.sav); read
                                   by load() if it exists, written by
                                   sram_flush()                                 */
} emu_load_t;

/* A setting a core offers, shown in the app's options and kept per
 * console. Labels and values are Spanish keys for _() (N_() in the
 * table); 'values' ends with NULL. */
typedef struct {
    const char         *key;
    const char         *label;
    const char *const  *values;
    int                 def;
} emu_option_t;

typedef struct emu_core {
    const char *id;             /* "nes": the folder on the card, the prefs'
                                   prefix                                       */
    const char *name;           /* "NES", "Game Boy Advance": shown as is       */
    const char *exts;           /* ROM extensions, lower case, with the dot,
                                   separated by spaces: ".nes .fds"            */
    int         max_w, max_h;   /* the largest picture it draws                 */
    int         players;        /* how many pads it reads                       */
    uint32_t    buttons;        /* EMU_BTN_* the console has                    */
    const emu_option_t *options;
    int         n_options;

    /* Loads the game and leaves the console powered on, the battery save
     * read in. false with a message in 'err' (Spanish, for _()) when it
     * cannot: a missing BIOS, an unknown mapper, no memory. */
    bool  (*load)(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen);
    /* Frees everything load() took. The app calls sram_flush() first. */
    void  (*unload)(void);
    void  (*reset)(void);

    /* One frame of the console: pads[i] is player i's EMU_BTN_* mask
     * (players() of them). With draw false the core may skip drawing
     * (frame skip, fast forward); it never calls video() then. */
    void  (*run)(const uint32_t *pads, bool draw);

    float (*fps)(void);         /* frames a second of the loaded game (60.1,
                                   59.73, 50 for PAL)                           */
    int   (*sample_rate)(void); /* of what audio() gets                         */

    /* Save states, to a file. A state written by one build may not load in
     * another: false then, and the game goes on as it was. */
    bool  (*state_save)(const char *path);
    bool  (*state_load)(const char *path);

    /* Writes the battery save if the game changed it since the last time
     * (or always, if the core cannot tell). true when nothing failed. */
    bool  (*sram_flush)(void);

    /* A setting from 'options' changed (also once per option right after
     * load(), with the kept value). */
    void  (*set_option)(const char *key, int value);

    /* A line for the log about how the core runs (recompiler caches,
     * mapper...), or NULL. Called every few seconds while playing. */
    const char *(*stats)(void);
} emu_core_t;

extern const emu_core_t emu_core_nes;
extern const emu_core_t emu_core_gba;

#ifdef __cplusplus
}
#endif
