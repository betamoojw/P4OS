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
 * The cores live in cores/<dir>/, each built as a library of its own
 * (cores/<dir>/board.cmake and sim.cmake), so a core's own globals never
 * meet another's: the only names a core exports are its tables,
 * emu_core_<id>. One library may bring several consoles (the Sega 8-bit
 * core: sms, gg, col), each a table and a folder on the card of its own;
 * its cmake files then set EMU_CORE_IDS to them.
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
    EMU_BTN_D      = 1u << 16,  /* Neo Geo */
};
#define EMU_BTN_COUNT 17

/* How the face buttons sit on the screen's pad (emu_core_t.face). */
typedef enum {
    EMU_FACE_AUTO = 0,  /* from the buttons the console has                  */
    EMU_FACE_ONE,       /* a single fire button (Atari 2600)                  */
    EMU_FACE_ROW2,      /* B A side by side (NES, Master System, PC Engine)   */
    EMU_FACE_DIAG2,     /* B low, A high (Game Boy, GBA, Lynx)                */
    EMU_FACE_DIAMOND,   /* Y X B A as a diamond (SNES)                        */
    EMU_FACE_ROW3,      /* A B C in a slanted row (Mega Drive, 3 buttons)     */
    EMU_FACE_GRID6,     /* X Y Z over A B C (Mega Drive, 6 buttons)           */
    EMU_FACE_ARC4,      /* A B C D on a rising arc (Neo Geo)                  */
} emu_face_t;
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

    /* The other CPU core: runs fn(arg, 1) on a helper pinned to the core
     * the console does not run on and fn(arg, 0) on the caller, and
     * returns when both are done (aos_hal_worker_split). The halves share
     * nothing while they run; half 1 never calls the host back. In the
     * simulator, or when the helper cannot start, the two run one after
     * the other. Never NULL. */
    void  (*parallel)(void (*fn)(void *arg, int half), void *arg);
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
    const char *short_name;     /* "NES", "GBA", "MD": the library's chips and
                                   badges, at most 6 characters (NULL: name)    */
    const char *exts;           /* ROM extensions, lower case, with the dot,
                                   separated by spaces: ".nes .fds"            */
    int         max_w, max_h;   /* the largest picture it draws                 */
    float       pixel_aspect;   /* a pixel's width over its height on the TV the
                                   console was made for (0: square). The
                                   picture is stretched by it: the Atari 2600's
                                   160 pixels a line are wide ones.             */
    int         players;        /* how many pads it reads                       */
    uint32_t    buttons;        /* EMU_BTN_* the console has                    */
    emu_face_t  face;           /* how the face buttons sit on the screen       */
    /* What the console calls each button, indexed by its bit (EMU_BTN_A is
     * bit 4): "1" and "2" on a Master System, "I" and "II" on a PC Engine,
     * "RESET" for the 2600's START. NULL, or a NULL entry: the letter. */
    const char *const *labels;
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

    /* Optional (NULL: the table's 'buttons'). The buttons the console has
     * now, when an option changes them (the Mega Drive's 3- or 6-button
     * pad): read after load() and after every set_option(); the face
     * layout follows them when 'face' is EMU_FACE_AUTO. */
    uint32_t (*buttons_now)(void);

    /* Optional (NULL: w * pixel_aspect / h). The shape a frame of w x h is
     * shown at, width over height, for consoles whose pixels change with
     * the video mode (the Mega Drive's 256 and 320 modes both fill a 4:3
     * TV; the PC Engine's 256..352). */
    float (*frame_aspect)(int w, int h);
} emu_core_t;

/* The version of this contract. A core built against another one is
 * refused by the app (main/emu_cores.c): bump it whenever emu_core_t,
 * emu_host_t or emu_load_t change. */
#define EMU_CORE_ABI 3

/* On the board each cores/<dir> is a module of its own, the card's
 * apps/emulador/<dir>.so, loaded only while one of its consoles plays
 * (aos_hal_module_open). The firmware's loader finds functions only, so a
 * module exports these two, which the build generates
 * (cores/emu_module.c.in): */
int               emu_module_abi(void);
const emu_core_t *emu_module_core(int index);     /* NULL past the last */

/* The tables are named emu_core_<id> (nes, snes, gb, gbc, gba, md, sms, gg,
 * col, neogeo, pce, a2600, a7800, lynx...): main/emu_cores.c lists them. */

#ifdef __cplusplus
}
#endif
