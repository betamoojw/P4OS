/*
 * P4OS - Emulador, Atari 7800 core: ProSystem (as RetroESP32-P4 carries it)
 * behind the emu_core_t table.
 *
 * ProSystem is one console held in globals. Each frame MARIA draws its
 * lines as palette indices into a 320-wide surface (the 160-pixel modes
 * write every pixel twice), and the TIA and POKEY leave two 8-bit samples
 * a line in buffers of their own; this file turns the first into RGB565
 * for the host and mixes the second into 16-bit stereo, at the console's
 * own rate (two samples a line: 524 a frame NTSC, 624 PAL), so nothing is
 * resampled and sound and picture never drift.
 *
 * What this file adds: complete save states (P4State.h: ProSystem kept only
 * the CPU and the RAM), checks on the cartridge before ProSystem maps it,
 * the console switches, and the optional BIOS.
 *
 * The 7800 had no battery RAM (the High Score Cartridge is not emulated),
 * so there is no .sav.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"

#include "Bios.h"
#include "Cartridge.h"
#include "Database.h"
#include "Maria.h"
#include "Memory.h"
#include "P4State.h"
#include "Palette.h"
#include "Pokey.h"
#include "ProSystem.h"
#include "Region.h"
#include "Tia.h"

#ifndef N_
#define N_(s) s
#endif

#define A78_W           320
#define A78_H_NTSC      223     /* MARIA's visible lines, as ProSystem's   */
#define A78_H_PAL       272     /* regions define them (Region.c)          */
#define A78_ROM_MAX     (1024 * 1024 + 128)
#define A78_RATE_NTSC   31400   /* 524 samples a frame: 59.92 fps          */
#define A78_RATE_PAL    31112   /* 624 samples a frame: 49.86 fps          */

/* ProSystem's RAM (Memory.c), checked after the power-on. */
extern uint8_t *memory_ram;

/* Read by MARIA: lines are only drawn while it is true (frame skip). */
bool RenderFlag = true;

static const emu_host_t *s_host;
static bool s_loaded;
static char *s_bios_dir;
static uint16_t *s_frame;
static int16_t *s_audio;
static uint16_t s_pal[256];
static int s_spf;               /* samples a frame: 524 or 624 */
static int s_rate;
static bool s_pal_tv;

/* The DC filter on the mixed sound (the chips' output is all positive). */
static int32_t s_dc_x, s_dc_y;

/* Options. */
static int s_opt_left = 1;      /* 0 A, 1 B */
static int s_opt_right = 0;
static int s_opt_bios = 0;      /* 0 no, 1 if present */
static bool s_bios_on;          /* the BIOS is loaded and in use */

static char s_stats[96];

/* ---- options ----------------------------------------------------------- */

static const char *const s_diff_values[] = { N_("A (difícil)"), N_("B (fácil)"), NULL };
static const char *const s_bios_values[] = { N_("No usar"), N_("Usar si está"), NULL };

static const emu_option_t s_options[] = {
    { "diff_left",  N_("Dificultad izquierda"), s_diff_values, 1 },
    { "diff_right", N_("Dificultad derecha"),   s_diff_values, 0 },
    { "bios",       N_("BIOS de la 7800"),      s_bios_values, 0 },
};
#define A78_N_OPTIONS ((int)(sizeof(s_options) / sizeof(s_options[0])))

/* The ProLine joystick's two buttons are 1 (left) and 2 (right); the
 * console's PAUSE is START and its RESET the right shoulder. */
static const char *const s_labels[EMU_BTN_COUNT] = {
    /* UP DOWN LEFT RIGHT */ NULL, NULL, NULL, NULL,
    /* A */ "2", /* B */ "1", /* X Y L */ NULL, NULL, NULL,
    /* R */ "RESET", /* SELECT */ "SELECT", /* START */ "PAUSE",
};

static void a78_build_palette(void)
{
    if (!palette_data) return;
    for (int i = 0; i < 256; i++) {
        uint16_t r = palette_data[i * 3], g = palette_data[i * 3 + 1], b = palette_data[i * 3 + 2];
        s_pal[i] = (uint16_t)(((r << 8) & 0xF800) | ((g << 3) & 0x07E0) | (b >> 3));
    }
}

/* ---- the BIOS -----------------------------------------------------------
 * Optional: a game runs without it (ProSystem starts the cartridge
 * directly). With it the console shows its logo and checks the cartridge's
 * signature, as the real one does: an unsigned homebrew then does not
 * start, which is why it is off unless asked for. "7800 BIOS (U).rom" is
 * the NTSC one, "7800 BIOS (E).rom" the PAL one. */

static bool a78_bios_try(bool pal)
{
    bios_Release();
    bios_enabled = false;
    if (!s_bios_dir) return false;
    static const char *const ntsc[] = { "7800 BIOS (U).rom", "7800bios.bin", "7800.rom" };
    static const char *const palv[] = { "7800 BIOS (E).rom", "7800pal.bin", "7800 BIOS (U).rom" };
    const char *const *names = pal ? palv : ntsc;
    size_t len = strlen(s_bios_dir) + 32;
    char *path = malloc(len);
    if (!path) return false;
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        snprintf(path, len, "%s/%s", s_bios_dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        long n = -1;
        if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
        fclose(f);
        /* whole 4 KB pages, as ProSystem maps them */
        if ((n != 4096 && n != 16384) || !bios_Load(path)) continue;
        if (!bios_IsLoaded()) continue;
        ok = true;
        s_host->log("a7800: BIOS %s", names[i]);
    }
    free(path);
    bios_enabled = ok;
    return ok;
}

/* ---- load and unload --------------------------------------------------- */

static void a78_free_all(void)
{
    if (s_loaded) prosystem_Close();
    cartridge_Release();
    bios_Release();
    bios_enabled = false;
    memory_Release();
    palette_Release();
    pokey_Release();
    free(maria_surface);
    maria_surface = NULL;
    free(s_frame);
    free(s_audio);
    free(s_bios_dir);
    s_frame = NULL;
    s_audio = NULL;
    s_bios_dir = NULL;
    s_loaded = false;
    s_bios_on = false;
    s_dc_x = s_dc_y = 0;
    RenderFlag = true;
    region_type = REGION_AUTO;
    palette_default = true;
}

static void a78_unload(void)
{
    a78_free_all();
    s_host = NULL;
}

/* Whether ProSystem can map this cartridge without reading past it: its
 * own code trusts the size. */
static bool a78_cart_fits(uint32_t size)
{
    if (size == 0 || size % 4096) return false;
    switch (cartridge_type) {
    case CARTRIDGE_TYPE_NORMAL:
        return size <= 49152;
    case CARTRIDGE_TYPE_SUPERCART:
    case CARTRIDGE_TYPE_SUPERCART_RAM:
    case CARTRIDGE_TYPE_SUPERCART_ROM:
    case CARTRIDGE_TYPE_SUPERCART_LARGE:
        return size % 16384 == 0 && size >= 32768;
    case CARTRIDGE_TYPE_ABSOLUTE:
        return size >= 65536 && size % 16384 == 0;
    case CARTRIDGE_TYPE_ACTIVISION:
        return size >= 131072;
    default:
        return false;
    }
}

/* ---- power-on ------------------------------------------------------------
 *
 * ProSystem's resets leave much as it was (MARIA's line buffer and display
 * list pointers, the RIOT's timer, the sound chips' counters): a game
 * loaded after another, or reset, did not start from the same machine and
 * its first frames differed. The modules' variables as the program starts
 * (their C initialisers) are kept here the first time, before anything has
 * run, and put back at every power-on before ProSystem's own reset. A
 * static block: it is a copy of statics and must outlive unload(). */

static uint8_t s_pristine[4096];
static uint32_t s_pristine_n;   /* 0: not taken yet */

typedef struct { uint8_t *buf; uint32_t pos, size; bool in; } a78_pwalk_t;

static void a78_pwalk_fn(void *ctx, void *ptr, uint32_t size)
{
    a78_pwalk_t *w = ctx;
    if (w->pos + size <= w->size) {
        if (w->in) memcpy(ptr, w->buf + w->pos, size);
        else memcpy(w->buf + w->pos, ptr, size);
    }
    w->pos += size;
}

static void a78_pristine(bool restore)
{
    a78_pwalk_t w = { s_pristine, 0, sizeof(s_pristine), restore };
    if (restore && !s_pristine_n) return;
    prosystem_State(a78_pwalk_fn, &w);
    sally_State(a78_pwalk_fn, &w);
    maria_State(a78_pwalk_fn, &w);
    riot_State(a78_pwalk_fn, &w);
    tia_State(a78_pwalk_fn, &w);
    pokey_State(a78_pwalk_fn, &w);
    if (!restore) s_pristine_n = w.pos <= sizeof(s_pristine) ? w.pos : 0;
}

static void a78_power_on(void)
{
    a78_pristine(true);
    region_Reset();             /* palette, scanlines, buffer sizes */
    prosystem_Reset();
    a78_build_palette();
    s_pal_tv = prosystem_scanlines > 262;
    s_spf = (int)tia_size;
    s_rate = s_pal_tv ? A78_RATE_PAL : A78_RATE_NTSC;
    s_dc_x = s_dc_y = 0;
}

static bool a78_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    if (!s_pristine_n) a78_pristine(false);
    a78_free_all();
    s_host = host;
    if (req->bios_dir) {
        size_t n = strlen(req->bios_dir) + 1;
        s_bios_dir = malloc(n);
        if (s_bios_dir) memcpy(s_bios_dir, req->bios_dir, n);
    }

    FILE *f = fopen(req->rom, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return false;
    }
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n <= 128 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, errlen, "No es un juego de Atari 7800");
        return false;
    }
    if (n > A78_ROM_MAX) {
        fclose(f);
        snprintf(err, errlen, "El archivo es demasiado grande");
        return false;
    }
    uint8_t *data = malloc((size_t)n);
    if (!data) {
        fclose(f);
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    bool got = fread(data, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!got) {
        free(data);
        snprintf(err, errlen, "No se pudo leer el archivo");
        return false;
    }
    /* A .a78 says so in its header; a headerless image is taken as it
     * comes, the database or the size telling its type. A 2600 game is
     * not a 7800 one: ProSystem has no 2600 mode. */
    bool header = n > 128 && memcmp(data + 1, "ATARI7800", 9) == 0;
    if (!header && (n < 8192 || n % 4096)) {
        free(data);
        snprintf(err, errlen, "No es un juego de Atari 7800");
        return false;
    }
    bool ok = cartridge_Load(data, (uint32_t)n);
    free(data);
    if (!ok) {
        a78_free_all();
        snprintf(err, errlen, "No es un juego de Atari 7800");
        return false;
    }
    database_Load(cartridge_digest);
    uint32_t cart_size = 0;
    cartridge_Image(&cart_size);
    if (!a78_cart_fits(cart_size)) {
        host->log("a7800: type %d with %u bytes cannot be mapped", cartridge_type, (unsigned)cart_size);
        a78_free_all();
        snprintf(err, errlen, "Tipo de cartucho no soportado");
        return false;
    }

    maria_surface = calloc(1, MARIA_SURFACE_SIZE);
    s_frame = malloc(A78_W * A78_H_PAL * sizeof(uint16_t));
    s_audio = malloc(TIA_BUFFER_SIZE * 2 * sizeof(int16_t));
    if (!maria_surface || !s_frame || !s_audio) {
        a78_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    s_loaded = true;
    a78_power_on();
    if (!memory_ram || !palette_data || !pokey_Ready()) {
        a78_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    host->log("a7800: type %d, %u KB%s, %s, controllers %d/%d, md5 %s",
              cartridge_type, (unsigned)(cart_size / 1024), cartridge_pokey ? ", POKEY" : "",
              s_pal_tv ? "PAL" : "NTSC", cartridge_controller[0], cartridge_controller[1],
              cartridge_digest);
    return true;
}

/* ---- running ----------------------------------------------------------- */

static void a78_blit(void)
{
    int top = maria_visibleArea.top - maria_displayArea.top;
    int h = maria_visibleArea.bottom - maria_visibleArea.top + 1;
    if (h > A78_H_PAL) h = A78_H_PAL;
    const uint8_t *src = maria_surface + top * A78_W;
    const uint16_t *pal = s_pal;
    uint16_t *dst = s_frame;
    for (int i = 0; i < A78_W * h; i += 4) {
        dst[i + 0] = pal[src[i + 0]];
        dst[i + 1] = pal[src[i + 1]];
        dst[i + 2] = pal[src[i + 2]];
        dst[i + 3] = pal[src[i + 3]];
    }
    s_host->video(s_frame, A78_W, h, A78_W * (int)sizeof(uint16_t));
}

/* The two chips mixed, the DC taken out by a one-pole high-pass filter
 * (their output only ever rises from zero), as 16-bit stereo. */
static void a78_sound(void)
{
    int n = s_spf;
    bool pokey = cartridge_pokey;
    int32_t x1 = s_dc_x, y1 = s_dc_y;
    for (int i = 0; i < n; i++) {
        int32_t x = (int32_t)tia_buffer[i] * 96;        /* 0..120 -> 0..11520 */
        if (pokey) x += ((int32_t)pokey_buffer[i] - 8) * 64;   /* 0..240 -> 0..15360 */
        int32_t y = x - x1 + ((y1 * 255) >> 8);
        x1 = x;
        y1 = y;
        if (y > 32767) y = 32767;
        if (y < -32768) y = -32768;
        s_audio[i * 2] = s_audio[i * 2 + 1] = (int16_t)y;
    }
    s_dc_x = x1;
    s_dc_y = y1;
    s_host->audio(s_audio, n);
}

static void a78_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;
    uint32_t p0 = pads ? pads[0] : 0, p1 = pads ? pads[1] : 0;
    uint8_t in[17];
    /* ProSystem's input bytes: per player right, left, down, up, button 1,
     * button 2; then RESET, SELECT, PAUSE and the two difficulty switches
     * (1 = B). */
    in[0] = (p0 & EMU_BTN_RIGHT) != 0;
    in[1] = (p0 & EMU_BTN_LEFT) != 0;
    in[2] = (p0 & EMU_BTN_DOWN) != 0;
    in[3] = (p0 & EMU_BTN_UP) != 0;
    in[4] = (p0 & EMU_BTN_B) != 0;
    in[5] = (p0 & EMU_BTN_A) != 0;
    in[6] = (p1 & EMU_BTN_RIGHT) != 0;
    in[7] = (p1 & EMU_BTN_LEFT) != 0;
    in[8] = (p1 & EMU_BTN_DOWN) != 0;
    in[9] = (p1 & EMU_BTN_UP) != 0;
    in[10] = (p1 & EMU_BTN_B) != 0;
    in[11] = (p1 & EMU_BTN_A) != 0;
    uint32_t both = p0 | p1;
    in[12] = (both & EMU_BTN_R) != 0;
    in[13] = (both & EMU_BTN_SELECT) != 0;
    in[14] = (both & EMU_BTN_START) != 0;
    in[15] = s_opt_left == 1;
    in[16] = s_opt_right == 1;

    RenderFlag = draw;
    prosystem_ExecuteFrame(in);
    RenderFlag = true;
    if (draw) a78_blit();
    a78_sound();
}

/* The power switch: the game starts over, the BIOS (if in use) first. */
static void a78_reset(void)
{
    if (!s_loaded) return;
    a78_power_on();
}

static float a78_fps(void)
{
    if (!s_loaded || s_spf <= 0) return (float)A78_RATE_NTSC / 524.0f;
    return (float)s_rate / (float)s_spf;
}

static int a78_sample_rate(void)
{
    return s_loaded ? s_rate : A78_RATE_NTSC;
}

/* ---- save states --------------------------------------------------------
 *
 * "P4A78ST\0", a version, the payload's length and CRC-32, the cartridge's
 * MD5, then every module's variables in P4State.h's walk order, the memory
 * map as (region, offset) pairs, the BIOS flag and the sound filter. */

#define A78_ST_MAGIC   "P4A78ST"
#define A78_ST_VERSION 1u
#define A78_ST_HEAD    (8 + 4 + 4 + 4 + 32)

typedef struct {
    uint8_t *buf;       /* NULL: only count */
    uint32_t pos, size;
    bool in;            /* copying from buf into the variables */
} a78_walk_t;

static void a78_walk_fn(void *ctx, void *ptr, uint32_t size)
{
    a78_walk_t *w = ctx;
    if (w->buf && ptr && w->pos + size <= w->size) {
        if (w->in) memcpy(ptr, w->buf + w->pos, size);
        else memcpy(w->buf + w->pos, ptr, size);
    }
    w->pos += size;
}

/* The whole state; the map goes in or out separately, since installing it
 * can fail. */
static void a78_walk(a78_walk_t *w, uint32_t map[16])
{
    prosystem_State(a78_walk_fn, w);
    sally_State(a78_walk_fn, w);
    maria_State(a78_walk_fn, w);
    riot_State(a78_walk_fn, w);
    tia_State(a78_walk_fn, w);
    pokey_State(a78_walk_fn, w);
    cartridge_State(a78_walk_fn, w);
    memory_State(a78_walk_fn, w);
    a78_walk_fn(w, map, 16 * sizeof(uint32_t));
    a78_walk_fn(w, &bios_enabled, sizeof(bios_enabled));
    a78_walk_fn(w, &s_dc_x, sizeof(s_dc_x));
    a78_walk_fn(w, &s_dc_y, sizeof(s_dc_y));
}

static uint32_t a78_state_size(void)
{
    a78_walk_t w = { NULL, 0, 0, false };
    uint32_t map[16];
    a78_walk(&w, map);
    return w.pos;
}

static uint8_t *a78_state_take(uint32_t *size)
{
    uint32_t n = a78_state_size();
    uint8_t *buf = malloc(n);
    if (!buf) return NULL;
    uint32_t map[16];
    memory_MapOut(map);
    a78_walk_t w = { buf, 0, n, false };
    a78_walk(&w, map);
    *size = n;
    return buf;
}

static bool a78_state_give(const uint8_t *buf, uint32_t n)
{
    uint32_t map[16];
    a78_walk_t w = { (uint8_t *)buf, 0, n, true };
    a78_walk(&w, map);
    return w.pos == n && memory_MapIn(map);
}

static uint32_t a78_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void a78_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t a78_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool a78_state_save(const char *path)
{
    if (!s_loaded) return false;
    uint32_t n = 0;
    uint8_t *data = a78_state_take(&n);
    if (!data) return false;

    uint8_t head[A78_ST_HEAD];
    memcpy(head, A78_ST_MAGIC, 8);
    a78_put32(head + 8, A78_ST_VERSION);
    a78_put32(head + 12, n);
    a78_put32(head + 16, a78_crc32(data, n));
    memcpy(head + 20, cartridge_digest, 32);

    size_t plen = strlen(path);
    char *tmp = malloc(plen + 5);
    bool ok = tmp != NULL;
    if (ok) {
        memcpy(tmp, path, plen);
        memcpy(tmp + plen, ".tmp", 5);
        FILE *f = fopen(tmp, "wb");
        ok = f != NULL;
        if (ok) {
            ok = fwrite(head, 1, sizeof(head), f) == sizeof(head) && fwrite(data, 1, n, f) == n;
            if (fclose(f) != 0) ok = false;
        }
        if (ok) {
            remove(path);
            ok = rename(tmp, path) == 0;
        }
        if (!ok) remove(tmp);
    }
    if (!ok) s_host->log("a7800: writing the state %s failed", path);
    free(tmp);
    free(data);
    return ok;
}

static bool a78_state_load(const char *path)
{
    if (!s_loaded) return false;
    uint32_t want = a78_state_size();
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    uint8_t *buf = NULL;
    bool ok = n == (long)(A78_ST_HEAD + want) && fseek(f, 0, SEEK_SET) == 0
           && (buf = malloc((size_t)n)) != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (ok) {
        ok = memcmp(buf, A78_ST_MAGIC, 8) == 0
          && a78_get32(buf + 8) == A78_ST_VERSION
          && a78_get32(buf + 12) == want
          && a78_crc32(buf + A78_ST_HEAD, want) == a78_get32(buf + 16)
          && memcmp(buf + 20, cartridge_digest, 32) == 0;
    }
    if (!ok) {
        free(buf);
        s_host->log("a7800: %s is not a state for this game", path);
        return false;
    }
    /* What the console is now, to go back to if the state's memory map
     * does not fit this cartridge. */
    uint32_t bn = 0;
    uint8_t *backup = a78_state_take(&bn);
    if (!backup) {
        free(buf);
        return false;
    }
    ok = a78_state_give(buf + A78_ST_HEAD, want);
    free(buf);
    if (!ok) {
        s_host->log("a7800: the state does not fit this cartridge");
        if (!a78_state_give(backup, bn)) a78_power_on();
    }
    free(backup);
    return ok;
}

static bool a78_sram_flush(void)
{
    return true;    /* no battery RAM on the 7800 */
}

/* ---- options and stats ------------------------------------------------- */

static void a78_set_option(const char *key, int value)
{
    if (strcmp(key, "diff_left") == 0) {
        s_opt_left = value == 0 ? 0 : 1;
    } else if (strcmp(key, "diff_right") == 0) {
        s_opt_right = value == 1 ? 1 : 0;
    } else if (strcmp(key, "bios") == 0) {
        s_opt_bios = value == 1 ? 1 : 0;
        if (!s_loaded) return;
        /* The BIOS only runs from power-on: switching it starts the game
         * over (right after load() that is not seen). */
        bool want = s_opt_bios == 1;
        if (want == s_bios_on) return;
        if (want) {
            bool pal = cartridge_region == REGION_PAL;
            s_bios_on = a78_bios_try(pal);
            if (!s_bios_on) {
                s_host->log("a7800: no usable BIOS in %s", s_bios_dir ? s_bios_dir : "(none)");
                return;
            }
        } else {
            bios_Release();
            bios_enabled = false;
            s_bios_on = false;
        }
        a78_power_on();
    }
}

static const char *a78_stats(void)
{
    if (!s_loaded) return NULL;
    uint32_t size = 0;
    cartridge_Image(&size);
    int fps100 = s_spf > 0 ? (s_rate * 100) / s_spf : 0;
    snprintf(s_stats, sizeof(s_stats), "a7800: type %d, %u KB%s, %s, %d.%02d fps%s",
             cartridge_type, (unsigned)(size / 1024), cartridge_pokey ? ", POKEY" : "",
             s_pal_tv ? "PAL" : "NTSC", fps100 / 100, fps100 % 100, s_bios_on ? ", BIOS" : "");
    return s_stats;
}

const emu_core_t emu_core_a7800 = {
    .id = "a7800",
    .name = "Atari 7800",
    .short_name = "7800",
    .exts = ".a78 .bin",
    .max_w = A78_W,
    .max_h = A78_H_PAL,
    /* MARIA's 320 pixels a line are 7.16 MHz ones; a square pixel on a
     * 240-line NTSC picture is 6.14 MHz. */
    .pixel_aspect = 6.0f / 7.0f,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_R | EMU_BTN_SELECT | EMU_BTN_START,
    .face = EMU_FACE_ROW2,
    .labels = s_labels,
    .options = s_options,
    .n_options = A78_N_OPTIONS,
    .load = a78_load,
    .unload = a78_unload,
    .reset = a78_reset,
    .run = a78_run,
    .fps = a78_fps,
    .sample_rate = a78_sample_rate,
    .state_save = a78_state_save,
    .state_load = a78_state_load,
    .sram_flush = a78_sram_flush,
    .set_option = a78_set_option,
    .stats = a78_stats,
};
