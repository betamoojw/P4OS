/*
 * P4OS - Emulador: the Game Boy Advance.
 *
 * gpSP (midworld/gpsp-esp32, vendored in gpsp/) behind emu_core.h. The core
 * is driven through its own libretro entry points, as any libretro frontend
 * would, and this file is that frontend: the environment callback answers
 * the handful of questions gpSP asks (pixel format, BIOS folder, its
 * options), the video and audio callbacks forward to the app's host table,
 * and the buttons come back through the input callback as one bit mask.
 *
 * The board build carries gpSP's RV32 recompiler, the simulator's does not
 * (sim.cmake builds the portable interpreter). On the board the recompiler
 * needs executable memory, which the app may not have yet: with the host's
 * exec_* functions missing, gpSP is told there is no platform, its block
 * cache fails to allocate on load and its run loop falls back to the
 * interpreter on its own (gpsp_rv32_block_init() returns false and
 * execute_arm_translate() runs execute_arm()). Slow, but the same game.
 *
 * Everything in this library but emu_core_gba is made local when it is
 * built (board.cmake and sim.cmake), so gpSP's generic names (reg, dma,
 * retro_run...) never meet the other cores' in the same .so.
 */
#include "emu_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "common.h"
#include "gpsp_backup.h"
#include "gpsp_frame_stats.h"
#include "gpsp_memory.h"
#if defined(GPSP_RV32_JIT)
#include "gpsp_rv32_platform.h"
#include "riscv/rv32_code_arena.h"
#endif

#define N_(s) s

/* How much of a game stays in memory, in MiB. A game up to this size is
 * read whole when it loads; a bigger one (GBA games reach 32 MiB) keeps
 * this many MiB of 32 KiB pages and reads the others from the card as the
 * game asks for them. The app shares 32 MB of PSRAM with the system and
 * ~17 MB are free with it open: 8 MiB of game plus up to 4 MiB of
 * recompiled code leaves room for the rest. */
#ifndef GBA_ROM_BUFFER_MIB
#define GBA_ROM_BUFFER_MIB 8
#endif

/* GBA cartridges are at most 32 MiB; the header ends at 0xC0. */
#define GBA_ROM_MAX   (32u * 1024 * 1024)
#define GBA_ROM_MIN   0xC0u

/* A .sav may carry a trailer after the save itself (VBA's RTC block, for
 * one), as gpsp-esp32's own reader allows. */
#define GBA_SAV_TRAILER 64

/* Options, in the order of gba_options[]. */
enum { GBA_OPT_BIOS, GBA_OPT_COLOR, GBA_OPT_MIX, GBA_OPT_COUNT };
enum { GBA_BIOS_AUTO, GBA_BIOS_BUILTIN };

static const char *const gba_vals_bios[] = { N_("Automática"), N_("Integrada"), NULL };
static const char *const gba_vals_onoff[] = { N_("No"), N_("Sí"), NULL };

static const emu_option_t gba_options[GBA_OPT_COUNT] = {
    [GBA_OPT_BIOS]  = { "bios",   N_("BIOS"),                gba_vals_bios,  GBA_BIOS_AUTO },
    [GBA_OPT_COLOR] = { "color",  N_("Corrección de color"), gba_vals_onoff, 0 },
    [GBA_OPT_MIX]   = { "mezcla", N_("Mezcla de cuadros"),   gba_vals_onoff, 0 },
};

/* gpSP's own symbols this file reaches past the libretro API. */
extern bool require_m1_hle_bios;
extern u32 gpsp_frontend_skip;          /* libretro.c, a P4OS addition */
#if defined(GPSP_RV32_JIT)
bool gpsp_rv32_block_is_ready(void);    /* rv32_block_runtime.cc, a P4OS addition */
#endif

static const emu_host_t *gba_host;
static bool     gba_loaded;
static uint32_t gba_pad;                /* player 1's EMU_BTN_* this frame   */
static bool     gba_draw;               /* run() asked for a picture          */
static uint32_t gba_frames;             /* run() calls since load or reset    */
static bool     gba_vars_changed;       /* answered by GET_VARIABLE_UPDATE    */
static int      gba_opt[GBA_OPT_COUNT] = { GBA_BIOS_AUTO, 0, 0 };
static bool     gba_bios_official;      /* what bios_rom holds now            */
static char     gba_bios_dir[320];
static char     gba_save_path[320];
static uint32_t gba_save_hash;          /* of the last save read or written   */
static uint32_t gba_save_size;
static char     gba_stats_line[160];

#if !defined(AOS_SIM)
/* libretro-common's file code duplicates paths with strdup, which the
 * firmware does not lend to apps. This one stays local to the core like
 * everything else here (board.cmake). */
char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
#endif

/* ---------------------------------------------------------------------------
 * The host's log, for gpSP's messages
 * ------------------------------------------------------------------------ */

static void gba_vlog(const char *fmt, va_list ap)
{
    char line[200];
    vsnprintf(line, sizeof(line), fmt, ap);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n && gba_host && gba_host->log) gba_host->log("gba: %s", line);
}

static void gba_core_log(enum retro_log_level level, const char *fmt, ...)
{
    if (level == RETRO_LOG_DEBUG) return;
    va_list ap;
    va_start(ap, fmt);
    gba_vlog(fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------------------
 * Executable memory for the recompiler (board only)
 * ------------------------------------------------------------------------ */

#if defined(GPSP_RV32_JIT)
static void *gba_exec_alloc(size_t align, size_t size)
{
    return gba_host->exec_alloc(align, size);
}

static void gba_exec_free(void *code)
{
    gba_host->exec_free(code);
}

static bool gba_exec_publish(void *code, size_t size)
{
    return gba_host->exec_publish(code, size);
}

static void gba_jit_vlog(gpsp_rv32_log_level level, const char *tag,
                         const char *fmt, va_list ap)
{
    (void)level;
    (void)tag;
    gba_vlog(fmt, ap);
}

static const struct gpsp_rv32_platform_ops gba_jit_ops = {
    .exec_alloc   = gba_exec_alloc,
    .exec_free    = gba_exec_free,
    .exec_publish = gba_exec_publish,
    .vlog         = gba_jit_vlog,
};
#endif

/* ---------------------------------------------------------------------------
 * The libretro side
 * ------------------------------------------------------------------------ */

static const char *gba_variable(const char *key)
{
    if (!strcmp(key, "gpsp_bios"))
        return gba_opt[GBA_OPT_BIOS] == GBA_BIOS_BUILTIN ? "builtin" : "auto";
    if (!strcmp(key, "gpsp_color_correction"))
        return gba_opt[GBA_OPT_COLOR] ? "enabled" : "disabled";
    if (!strcmp(key, "gpsp_frame_mixing"))
        return gba_opt[GBA_OPT_MIX] ? "enabled" : "disabled";
    /* gpSP's lower rate: under the speaker's 48 kHz, so nothing is thrown
     * away, and half the mixing work of its default 65536 Hz. */
    if (!strcmp(key, "gpsp_sound_rate")) return "32768";
    /* No link cable, no Game Boy Player, no rumble. */
    if (!strcmp(key, "gpsp_serial")) return "disabled";
    if (!strcmp(key, "gpsp_rumble")) return "disabled";
    /* The cartridge clock follows the frames from the moment the game
     * loads, so a state brings its time with it. */
    if (!strcmp(key, "gpsp_rtc_time_source")) return "deterministic";
    if (!strcmp(key, "gpsp_rtc")) return "auto";
    if (!strcmp(key, "gpsp_boot_mode")) return "game";
    /* The app skips frames itself (run() with draw false). */
    if (!strcmp(key, "gpsp_frameskip")) return "disabled";
    if (!strcmp(key, "gpsp_sprlim")) return "disabled";
    return NULL;
}

static bool gba_environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = gba_core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = gba_bios_dir[0] ? gba_bios_dir : NULL;
        return gba_bios_dir[0] != 0;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = v->key ? gba_variable(v->key) : NULL;
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = gba_vars_changed;
        gba_vars_changed = false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const struct retro_message *m = data;
        if (m && m->msg && gba_host) gba_host->log("gba: %s", m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
        const struct retro_message_ext *m = data;
        if (m && m->msg && gba_host) gba_host->log("gba: %s", m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        return true;
    default:
        return false;
    }
}

static void gba_video_cb(const void *px, unsigned w, unsigned h, size_t pitch)
{
    /* NULL is a skipped frame (gpsp_frontend_skip). gpSP draws RGB565 with
     * red in the top five bits, LVGL's order, in native (little-endian)
     * 16-bit words. */
    if (!px || !gba_draw || !gba_host->video) return;
    gba_host->video((const uint16_t *)px, (int)w, (int)h, (int)pitch);
}

static size_t gba_audio_cb(const int16_t *lr, size_t frames)
{
    if (gba_host->audio && frames) gba_host->audio(lr, (int)frames);
    return frames;
}

static void gba_audio_sample_cb(int16_t l, int16_t r)
{
    int16_t lr[2] = { l, r };
    gba_audio_cb(lr, 1);
}

static void gba_input_poll_cb(void) {}

static int16_t gba_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    static const struct { uint32_t emu; uint8_t retro; } map[] = {
        { EMU_BTN_B,      RETRO_DEVICE_ID_JOYPAD_B      },
        { EMU_BTN_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT },
        { EMU_BTN_START,  RETRO_DEVICE_ID_JOYPAD_START  },
        { EMU_BTN_UP,     RETRO_DEVICE_ID_JOYPAD_UP     },
        { EMU_BTN_DOWN,   RETRO_DEVICE_ID_JOYPAD_DOWN   },
        { EMU_BTN_LEFT,   RETRO_DEVICE_ID_JOYPAD_LEFT   },
        { EMU_BTN_RIGHT,  RETRO_DEVICE_ID_JOYPAD_RIGHT  },
        { EMU_BTN_A,      RETRO_DEVICE_ID_JOYPAD_A      },
        { EMU_BTN_L,      RETRO_DEVICE_ID_JOYPAD_L      },
        { EMU_BTN_R,      RETRO_DEVICE_ID_JOYPAD_R      },
    };
    int16_t mask = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (gba_pad & map[i].emu) mask |= (int16_t)(1 << map[i].retro);
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return mask;
    return id < 16 ? (mask >> id) & 1 : 0;
}

/* ---------------------------------------------------------------------------
 * BIOS
 * ------------------------------------------------------------------------ */

/* The BIOS in bios_rom as the option asks: the official one from
 * <bios_dir>/gba_bios.bin when "Automática" finds it (better
 * compatibility), gpSP's built-in open replacement otherwise. gpSP itself
 * makes the same choice at load, and forces the built-in one for ROMs
 * with a non-standard header and for M1 hacks; this repeats it for a
 * change of option after load. */
static void gba_bios_apply(void)
{
    gba_bios_official = false;
    if (gba_opt[GBA_OPT_BIOS] == GBA_BIOS_AUTO && gba_bios_dir[0] &&
        !gamepak_header_nonstandard && !require_m1_hle_bios) {
        char path[sizeof(gba_bios_dir) + 16];
        snprintf(path, sizeof(path), "%s/gba_bios.bin", gba_bios_dir);
        FILE *f = fopen(path, "rb");
        if (f) {
            gba_bios_official = fread(bios_rom, 1, 0x4000, f) == 0x4000 && bios_rom[0] == 0x18;
            fclose(f);
        }
    }
    if (!gba_bios_official) memcpy(bios_rom, open_gba_bios_rom, 0x4000);
}

/* ---------------------------------------------------------------------------
 * Battery saves: the raw .sav other emulators use
 * ------------------------------------------------------------------------ */

static uint32_t gba_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* The size of the save a file of 'len' bytes holds, for a game whose save
 * is 'expected' bytes (0: not known yet). EEPROM's size is only known once
 * the game uses it, so 512 B and 8 KiB stand for each other. */
static uint32_t gba_sav_size(long len, uint32_t expected)
{
    static const uint32_t sizes[] = { 128 * 1024, 64 * 1024, 32 * 1024, 8 * 1024, 512 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint32_t s = sizes[i];
        bool fits = !expected || s == expected ||
                    ((expected == 512 || expected == 8 * 1024) && (s == 512 || s == 8 * 1024));
        if (fits && len >= (long)s && len - (long)s <= GBA_SAV_TRAILER) return s;
    }
    return 0;
}

static void gba_save_read(void)
{
    gba_save_size = (uint32_t)gpsp_backup_size();
    FILE *f = gba_save_path[0] ? fopen(gba_save_path, "rb") : NULL;
    if (f) {
        long len = -1;
        if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
        uint32_t size = len > 0 ? gba_sav_size(len, gba_save_size) : 0;
        if (size && fseek(f, 0, SEEK_SET) == 0 &&
            fread(gamepak_backup, 1, size, f) == size) {
            gba_host->log("gba: save read, %u bytes", (unsigned)size);
        } else if (len > 0) {
            /* Not a size gpSP can use: left alone, and not overwritten
             * until the game saves. */
            gba_host->log("gba: save not used (%ld bytes)", len);
            memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
        }
        fclose(f);
    }
    if (gba_save_size) gba_save_hash = gba_hash(gamepak_backup, gba_save_size);
}

static bool gba_sram_flush(void)
{
    if (!gba_loaded || !gba_save_path[0]) return true;
    uint32_t size = (uint32_t)gpsp_backup_size();
    if (!size) return true;                         /* the game has no save */
    uint32_t hash = gba_hash(gamepak_backup, size);
    if (size == gba_save_size && hash == gba_save_hash) return true;

    char tmp[sizeof(gba_save_path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", gba_save_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(gamepak_backup, 1, size, f) == size;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        remove(gba_save_path);                       /* FAT does not rename over a file */
        ok = rename(tmp, gba_save_path) == 0;
    }
    if (!ok) {
        remove(tmp);
        return false;
    }
    gba_save_size = size;
    gba_save_hash = hash;
    return true;
}

/* ---------------------------------------------------------------------------
 * The core
 * ------------------------------------------------------------------------ */

static void gba_copy(char *dst, size_t n, const char *src)
{
    size_t len = src ? strlen(src) : 0;
    if (len >= n) len = n - 1;
    if (len) memcpy(dst, src, len);
    dst[len] = 0;
}

static void gba_unload(void);

static bool gba_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
#define GBA_FAIL(msg) do { gba_copy(err, errlen, msg); return false; } while (0)
    if (gba_loaded) gba_unload();
    if (!host || !req || !req->rom) GBA_FAIL(N_("No se pudo abrir el juego"));
    gba_host = host;

    /* A file that cannot be a GBA game is refused before gpSP sees it. */
    FILE *f = fopen(req->rom, "rb");
    if (!f) GBA_FAIL(N_("No se pudo abrir el juego"));
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    fclose(f);
    if (len < (long)GBA_ROM_MIN) GBA_FAIL(N_("El archivo no es un juego de Game Boy Advance"));
    if (len > (long)GBA_ROM_MAX) GBA_FAIL(N_("El juego es más grande que un cartucho de Game Boy Advance"));

    gba_copy(gba_bios_dir, sizeof(gba_bios_dir), req->bios_dir);
    gba_copy(gba_save_path, sizeof(gba_save_path), req->save);
    gba_pad = 0;
    gba_draw = true;
    gba_frames = 0;
    gba_vars_changed = false;
    gpsp_frontend_skip = 0;

#if defined(GPSP_RV32_JIT)
    /* The recompiler only with somewhere to put its code; without it the
     * block cache fails to allocate and gpSP interprets. */
    if (host->exec_alloc && host->exec_free && host->exec_publish)
        gpsp_rv32_set_platform(&gba_jit_ops);
    else
        gpsp_rv32_set_platform(NULL);
#if defined(GPSP_AFFINE_PIE)
    /* The affine renderer uses the PIE vector unit: this first PIE
     * instruction makes FreeRTOS give the worker its PIE save area now,
     * as gpsp-esp32's frontend does before the first frame. */
    __asm__ volatile(".option push\n.option arch, +xespv\nesp.zero.q q0\n.option pop\n" ::: "memory");
#endif
#endif

    /* gpSP takes the limit only before retro_init() allocates. */
    gpsp_set_rom_buffer_limit(GBA_ROM_BUFFER_MIB);

    retro_set_environment(gba_environment);
    retro_set_video_refresh(gba_video_cb);
    retro_set_audio_sample(gba_audio_sample_cb);
    retro_set_audio_sample_batch(gba_audio_cb);
    retro_set_input_poll(gba_input_poll_cb);
    retro_set_input_state(gba_input_state_cb);
    host->log("gba: init (JIT %s)", gba_host && gba_host->exec_alloc ? "on" : "off");
    retro_init();

    if (!gamepak_buffer_count || !gba_screen_pixels) {
        retro_deinit();
#if defined(GPSP_RV32_JIT)
        gpsp_rv32_set_platform(NULL);
#endif
        GBA_FAIL(N_("No hay memoria suficiente para el juego"));
    }

    struct retro_game_info game = { .path = req->rom, .data = NULL, .size = 0, .meta = NULL };
    if (!retro_load_game(&game)) {
        retro_deinit();
#if defined(GPSP_RV32_JIT)
        gpsp_rv32_set_platform(NULL);
#endif
        GBA_FAIL(N_("No se pudo cargar el juego"));
    }

    gba_bios_official = memcmp(bios_rom, open_gba_bios_rom, 0x4000) != 0;
    gba_save_read();
    gba_loaded = true;

    host->log("gba: %s, %u KiB of ROM, %u MiB in memory, BIOS %s, sound %u Hz",
#if defined(GPSP_RV32_JIT)
              gpsp_rv32_block_is_ready() ? "recompiler" : "interpreter (no executable memory)",
#else
              "interpreter",
#endif
              (unsigned)(gamepak_size / 1024), (unsigned)gamepak_buffer_count,
              gba_bios_official ? "official" : "built-in", (unsigned)sound_frequency);
    return true;
#undef GBA_FAIL
}

static void gba_unload(void)
{
    if (!gba_loaded) return;
    /* retro_deinit() frees what retro_init() and the game took: the ROM
     * buffer and its file, the picture buffers, the recompiler's caches
     * and tables (gpsp_rv32_block_deinit). */
    retro_unload_game();
    retro_deinit();
#if defined(GPSP_RV32_JIT)
    gpsp_rv32_set_platform(NULL);
#endif
    gba_loaded = false;
    gba_save_path[0] = 0;
    gba_save_size = 0;
}

static void gba_reset(void)
{
    if (!gba_loaded) return;
    gba_bios_apply();
    retro_reset();
    gba_frames = 0;
}

static void gba_run(const uint32_t *pads, bool draw)
{
    if (!gba_loaded) return;
    gba_pad = pads ? pads[0] : 0;
    gba_draw = draw;
    /* Without a picture gpSP skips the scanline renderer altogether. */
    gpsp_frontend_skip = draw ? 0 : 1;
    retro_run();
    gba_frames++;
}

static float gba_fps(void)
{
    return (float)GBC_BASE_RATE / (308.0f * 228.0f * 4.0f);   /* 59.73 */
}

static int gba_sample_rate(void)
{
    return (int)sound_frequency;
}

static bool gba_state_save(const char *path)
{
    if (!gba_loaded || !path) return false;
    size_t size = retro_serialize_size();
    uint8_t *buf = malloc(size);
    if (!buf) return false;
    bool ok = retro_serialize(buf, size);

    char tmp[340];
    snprintf(tmp, sizeof(tmp), "%.320s.tmp", path);
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    if (!f) ok = false;
    if (f) {
        ok = fwrite(buf, 1, size, f) == size;
        if (fclose(f) != 0) ok = false;
    }
    free(buf);
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

static bool gba_state_load(const char *path)
{
    if (!gba_loaded || !path) return false;
    size_t size = retro_serialize_size();
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    uint8_t *buf = len == (long)size && fseek(f, 0, SEEK_SET) == 0 ? malloc(size) : NULL;
    bool ok = buf && fread(buf, 1, size, f) == size;
    fclose(f);
    /* gba_load_state() checks the whole state before it touches the
     * console: a bad one leaves the game as it was. */
    ok = ok && retro_unserialize(buf, size);
    free(buf);
    return ok;
}

static void gba_set_option(const char *key, int value)
{
    for (int i = 0; i < GBA_OPT_COUNT; i++) {
        if (strcmp(key, gba_options[i].key) != 0) continue;
        int n = 0;
        while (gba_options[i].values[n]) n++;
        if (value < 0 || value >= n) value = gba_options[i].def;
        bool changed = gba_opt[i] != value;
        gba_opt[i] = value;
        if (!changed || !gba_loaded) return;
        if (i == GBA_OPT_BIOS) {
            /* A BIOS change resets the console, so it waits for the next
             * reset or game, unless the game has not run yet (the app
             * hands over the kept options right after load). */
            if (gba_frames == 0) {
                gba_bios_apply();
                retro_reset();
                gba_host->log("gba: BIOS %s", gba_bios_official ? "official" : "built-in");
            }
        } else {
            gba_vars_changed = true;   /* gpSP reads them after the next frame */
        }
        return;
    }
}

static const char *gba_stats(void)
{
    if (!gba_loaded) return NULL;
    uint32_t loads = 0, reloads = 0, remaps = 0, cycles = 0;
    gpsp_rom_page_stats(&loads, &reloads, &remaps, &cycles);
    char pages[48] = "";
    if (gamepak_must_swap())
        snprintf(pages, sizeof(pages), ", %u ROM pages read", (unsigned)loads);
#if defined(GPSP_RV32_JIT)
    if (gpsp_rv32_block_is_ready()) {
        uint32_t rom = rom_translation_ptr ? gpsp_rv32_arena_offset(&gpsp_rv32_rom_arena, rom_translation_ptr) : 0;
        uint32_t ram = ram_translation_ptr ? gpsp_rv32_arena_offset(&gpsp_rv32_ram_arena, ram_translation_ptr) : 0;
        if (rom == UINT32_MAX) rom = 0;
        if (ram == UINT32_MAX) ram = 0;
        snprintf(gba_stats_line, sizeof(gba_stats_line),
                 "gba: recompiler, ROM code %u/%u KiB, RAM code %u/%u KiB%s",
                 (unsigned)(rom / 1024), (unsigned)(gpsp_rv32_arena_capacity(&gpsp_rv32_rom_arena) / 1024),
                 (unsigned)(ram / 1024), (unsigned)(gpsp_rv32_arena_capacity(&gpsp_rv32_ram_arena) / 1024),
                 pages);
        return gba_stats_line;
    }
#endif
    snprintf(gba_stats_line, sizeof(gba_stats_line), "gba: interpreter%s", pages);
    return gba_stats_line;
}

const emu_core_t emu_core_gba = {
    .id          = "gba",
    .name        = "Game Boy Advance",
    .short_name  = "GBA",
    .exts        = ".gba .agb .bin",
    .max_w       = 240,
    .max_h       = 160,
    .players     = 1,
    .buttons     = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_L | EMU_BTN_R |
                   EMU_BTN_SELECT | EMU_BTN_START,
    .face        = EMU_FACE_DIAG2,
    .options     = gba_options,
    .n_options   = GBA_OPT_COUNT,
    .load        = gba_load,
    .unload      = gba_unload,
    .reset       = gba_reset,
    .run         = gba_run,
    .fps         = gba_fps,
    .sample_rate = gba_sample_rate,
    .state_save  = gba_state_save,
    .state_load  = gba_state_load,
    .sram_flush  = gba_sram_flush,
    .set_option  = gba_set_option,
    .stats       = gba_stats,
};
