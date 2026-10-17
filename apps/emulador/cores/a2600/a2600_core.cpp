/*
 * P4OS - Emulador, Atari 2600 core: Stella 3.9.3 (as RetroESP32-P4 carries
 * it) behind the emu_core_t table.
 *
 * Stella is C++ and keeps one console in a handful of objects (OSystem,
 * Settings, Console and the devices the Console owns). This file makes
 * them at load() and deletes them at unload(), turns the TIA's frame of
 * palette indices into RGB565 for the host, and hands on the sound Stella
 * mixes into a buffer of ours.
 *
 * What Stella does not do and this file does: the save-state file (a header
 * with the cartridge's MD5 and a CRC over Stella's own serialised state, so
 * a foreign or damaged file is refused before anything is touched), the
 * console switches as options, and the phosphor blend.
 *
 * The 2600 has no battery RAM, so there is no .sav (sram_flush() has
 * nothing to write).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"

#include "bspf.hxx"
#include "Cart.hxx"
#include "Console.hxx"
#include "Control.hxx"
#include "Event.hxx"
#include "EventHandler.hxx"
#include "MD5.hxx"
#include "OSystem.hxx"
#include "Props.hxx"
#include "PropsSet.hxx"
#include "Serializer.hxx"
#include "Settings.hxx"
#include "SoundSDL.hxx"
#include "Switches.hxx"
#include "System.hxx"
#include "TIA.hxx"
#include "TIATables.hxx"

#ifndef N_
#define N_(s) s
#endif

/* The TIA's sound as SoundSDL mixes it (TIASnd.cpp has it fixed). */
#define A26_RATE        31400
/* The biggest cartridge the schemes built here take (3E: 512 KB; the
 * Supercharger's tapes are 8448 bytes a load). */
#define A26_ROM_MAX     (512 * 1024)
#define A26_W           160
#define A26_H_MAX       256

/* Read by Stella's TIA.cpp: drawing is skipped while it is false (the
 * frontend's frame skip). RetroESP32-P4 defined it in its own runner. */
bool RenderFlag = true;

static const emu_host_t *s_host;
static OSystem *s_os;
static Settings *s_settings;
static Console *s_console;
static bool s_loaded;

static uint16_t *s_frame;               /* the RGB565 picture, 160 x 256 */
static int16_t *s_audio;                /* one frame of sound, stereo    */
static int s_spf;                       /* sound frames per frame        */
static uint16_t s_pal[256];
static string *s_md5;
static bool s_pal_tv;                   /* a 50 Hz game                  */

/* What the game's database entry says, for "Según el juego". */
static bool s_prop_left_a, s_prop_right_a, s_prop_color, s_prop_phosphor;
static int s_prop_blend;                /* 0..100, Stella's Display.PPBlend */

/* Options, as indices of their values below. */
static int s_opt_left;                  /* 0 game, 1 A, 2 B         */
static int s_opt_right;
static int s_opt_tv;                    /* 0 game, 1 colour, 2 B/W  */
static int s_opt_phosphor;              /* 0 game, 1 yes, 2 no      */
static int s_opt_palette;               /* 0 Stella, 1 z26          */

static char s_stats[96];

/* ---- options ----------------------------------------------------------- */

static const char *const s_diff_values[] = {
    N_("Según el juego"), N_("A (difícil)"), N_("B (fácil)"), NULL,
};
static const char *const s_tv_values[] = {
    N_("Según el juego"), N_("Color"), N_("Blanco y negro"), NULL,
};
static const char *const s_phos_values[] = {
    N_("Según el juego"), N_("Sí"), N_("No"), NULL,
};
static const char *const s_palette_values[] = {
    N_("Stella"), N_("z26"), NULL,
};

static const emu_option_t s_options[] = {
    { "diff_left",  N_("Dificultad izquierda"), s_diff_values,    0 },
    { "diff_right", N_("Dificultad derecha"),   s_diff_values,    0 },
    { "tv_type",    N_("Televisor"),            s_tv_values,      0 },
    { "phosphor",   N_("Suavizar parpadeo"), s_phos_values, 0 },
    { "palette",    N_("Paleta"),               s_palette_values, 0 },
};
#define A26_N_OPTIONS ((int)(sizeof(s_options) / sizeof(s_options[0])))

/* The console's two switches on the pad: START is the 2600's RESET. The
 * joystick's one button (A) keeps no name. */
static const char *const s_labels[EMU_BTN_COUNT] = {
    /* UP DOWN LEFT RIGHT A B X Y L R */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* SELECT */ "SELECT", /* START */ "RESET",
};

static void a26_build_palette(void)
{
    if (!s_console) return;
    s_console->setPalette(s_opt_palette == 1 ? "z26" : "standard");
    const uInt32 *p = s_console->getPalette(0);
    if (!p) return;
    for (int i = 0; i < 256; i++) {
        uInt32 c = p[i];
        uint16_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        s_pal[i] = (uint16_t)(((r << 8) & 0xF800) | ((g << 3) & 0x07E0) | (b >> 3));
    }
}

/* ---- load and unload --------------------------------------------------- */

static void a26_free_all(void)
{
    /* The Console deletes the System, and the System every device attached
     * to it, the cartridge included. */
    delete s_console;
    s_console = NULL;
    if (s_os) s_os->myConsole = NULL;
    /* the TIA's 200 KB table, made by the first TIA */
    TIATables::releaseTables();
    delete s_settings;
    s_settings = NULL;
    delete s_os;
    s_os = NULL;
    delete s_md5;
    s_md5 = NULL;
    free(s_frame);
    free(s_audio);
    s_frame = NULL;
    s_audio = NULL;
    s_spf = 0;
    s_loaded = false;
    RenderFlag = true;
}

static void a26_unload(void)
{
    a26_free_all();
    s_host = NULL;
}

static uint8_t *a26_read_file(const char *path, uint32_t *size, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return NULL;
    }
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, errlen, "No es un juego de Atari 2600");
        return NULL;
    }
    if (n > A26_ROM_MAX) {
        fclose(f);
        snprintf(err, errlen, "El archivo es demasiado grande");
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (!data) {
        fclose(f);
        snprintf(err, errlen, "No hay memoria");
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(data);
        snprintf(err, errlen, "No se pudo leer el archivo");
        return NULL;
    }
    *size = (uint32_t)n;
    return data;
}

static bool a26_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    a26_free_all();
    s_host = host;

    uint32_t size = 0;
    uint8_t *data = a26_read_file(req->rom, &size, err, errlen);
    if (!data) return false;
    /* No 2600 cartridge is smaller than 2 KB (Stella pads a smaller one,
     * but a file that small is something else). */
    if (size < 2048 && size != 1024 && size != 512) {
        free(data);
        snprintf(err, errlen, "No es un juego de Atari 2600");
        return false;
    }

    s_frame = (uint16_t *)malloc(A26_W * A26_H_MAX * sizeof(uint16_t));
    s_md5 = new string(MD5(data, size));
    s_os = new OSystem();
    s_settings = new Settings(s_os);
    if (!s_frame) {
        free(data);
        a26_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    /* Stella's settings start empty: everything false or zero. The ones
     * it reads that must not be. */
    s_settings->setValue("sound", true);
    s_settings->setValue("volume", "100");
    s_settings->setValue("fastscbios", true);   /* Supercharger: no tape wait */
    s_settings->setValue("romloadcount", "0");

    /* The TIA's tables (one 200 KB block, built once): made here, where a
     * failure can still be told, rather than inside the Console. */
    TIATables::computeAllTables();
    if (!TIATables::ready()) {
        free(data);
        a26_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    Properties props;
    bool known = s_os->propSet().getMD5(*s_md5, props);
    string type = props.get(Cartridge_Type);
    string id;
    Cartridge *cart = Cartridge::create(data, size, *s_md5, type, id, *s_os, *s_settings);
    free(data);
    if (!cart) {
        a26_free_all();
        snprintf(err, errlen, "Tipo de cartucho no soportado");
        return false;
    }

    s_console = new Console(s_os, cart, props);
    s_os->myConsole = s_console;
    s_console->initializeVideo();
    s_console->initializeAudio();

    const Properties &cp = s_console->properties();
    s_prop_left_a = cp.get(Console_LeftDifficulty) == "A";
    s_prop_right_a = cp.get(Console_RightDifficulty) == "A";
    s_prop_color = cp.get(Console_TelevisionType) != "BW";
    s_prop_phosphor = cp.get(Display_Phosphor) == "YES";
    s_prop_blend = atoi(cp.get(Display_PPBlend).c_str());
    if (s_prop_blend <= 0 || s_prop_blend > 100) s_prop_blend = 77;

    /* A whole number of sound frames a frame, and fps() worked out from
     * it, so picture and sound never drift: 524 for NTSC (59.92 fps, the
     * console's), 629 for PAL (49.92). */
    float fr = s_console->getFramerate();
    if (fr < 40.0f || fr > 70.0f) fr = 59.92f;
    s_pal_tv = fr < 55.0f;
    s_spf = (int)(A26_RATE / fr + 0.5f);
    s_audio = (int16_t *)malloc((size_t)s_spf * 2 * sizeof(int16_t));
    if (!s_audio) {
        a26_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    a26_build_palette();
    s_loaded = true;
    host->log("a2600: %s, %s%s, %s, %d samples a frame, md5 %s",
              cart->about().c_str(), s_console->about().DisplayFormat.c_str(),
              known ? "" : " (not in the database)",
              cp.get(Controller_Left).c_str(), s_spf, s_md5->c_str());
    return true;
}

/* ---- running ----------------------------------------------------------- */

/* Stella's phosphor: of two colours, the brighter channel by channel pulled
 * back toward the darker by the blend (TIA's flickering objects stay on). */
static inline uint16_t a26_phos(uint16_t a, uint16_t b, int w)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int hr = ar > br ? ar : br, lr = ar + br - hr;
    int hg = ag > bg ? ag : bg, lg = ag + bg - hg;
    int hb = ab > bb ? ab : bb, lb = ab + bb - hb;
    int r = lr + (((hr - lr) * w) >> 7);
    int g = lg + (((hg - lg) * w) >> 7);
    int bl = lb + (((hb - lb) * w) >> 7);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void a26_blit(void)
{
    TIA &tia = s_console->tia();
    int h = (int)tia.height();
    if (h > A26_H_MAX) h = A26_H_MAX;
    const uint8_t *cur = tia.currentFrameBuffer();
    const uint16_t *pal = s_pal;
    uint16_t *dst = s_frame;

    bool phos = s_opt_phosphor == 0 ? s_prop_phosphor : s_opt_phosphor == 1;
    if (phos) {
        const uint8_t *prev = tia.previousFrameBuffer();
        int w = s_prop_blend * 128 / 100;
        for (int i = 0; i < A26_W * h; i++)
            dst[i] = a26_phos(pal[cur[i]], pal[prev[i]], w);
    } else {
        for (int i = 0; i < A26_W * h; i += 4) {
            dst[i + 0] = pal[cur[i + 0]];
            dst[i + 1] = pal[cur[i + 1]];
            dst[i + 2] = pal[cur[i + 2]];
            dst[i + 3] = pal[cur[i + 3]];
        }
    }
    s_host->video(s_frame, A26_W, h, A26_W * (int)sizeof(uint16_t));
}

static void a26_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;
    uint32_t p0 = pads ? pads[0] : 0, p1 = pads ? pads[1] : 0;
    Event &ev = s_os->eventHandler().event();

    /* The joysticks. Paddles and driving controllers read the same events
     * (left and right turn them, fire is fire): Stella maps them so. */
    ev.set(Event::JoystickZeroUp,    (p0 & EMU_BTN_UP) != 0);
    ev.set(Event::JoystickZeroDown,  (p0 & EMU_BTN_DOWN) != 0);
    ev.set(Event::JoystickZeroLeft,  (p0 & EMU_BTN_LEFT) != 0);
    ev.set(Event::JoystickZeroRight, (p0 & EMU_BTN_RIGHT) != 0);
    ev.set(Event::JoystickZeroFire,  (p0 & EMU_BTN_A) != 0);
    ev.set(Event::JoystickOneUp,     (p1 & EMU_BTN_UP) != 0);
    ev.set(Event::JoystickOneDown,   (p1 & EMU_BTN_DOWN) != 0);
    ev.set(Event::JoystickOneLeft,   (p1 & EMU_BTN_LEFT) != 0);
    ev.set(Event::JoystickOneRight,  (p1 & EMU_BTN_RIGHT) != 0);
    ev.set(Event::JoystickOneFire,   (p1 & EMU_BTN_A) != 0);

    /* The console's switches: SELECT and RESET from either pad, the
     * difficulty and TV switches from the options. */
    uint32_t both = p0 | p1;
    ev.set(Event::ConsoleSelect, (both & EMU_BTN_SELECT) != 0);
    ev.set(Event::ConsoleReset,  (both & EMU_BTN_START) != 0);
    bool la = s_opt_left == 0 ? s_prop_left_a : s_opt_left == 1;
    bool ra = s_opt_right == 0 ? s_prop_right_a : s_opt_right == 1;
    bool col = s_opt_tv == 0 ? s_prop_color : s_opt_tv == 1;
    ev.set(Event::ConsoleLeftDiffA, la);
    ev.set(Event::ConsoleLeftDiffB, !la);
    ev.set(Event::ConsoleRightDiffA, ra);
    ev.set(Event::ConsoleRightDiffB, !ra);
    ev.set(Event::ConsoleColor, col);
    ev.set(Event::ConsoleBlackWhite, !col);

    s_console->controller(Controller::Left).update();
    s_console->controller(Controller::Right).update();
    s_console->switches().update();

    RenderFlag = draw;
    s_console->tia().update();
    RenderFlag = true;
    if (draw) a26_blit();

    SoundSDL *snd = (SoundSDL *)&s_os->sound();
    snd->processFragment(s_audio, (uInt32)s_spf);
    s_host->audio(s_audio, s_spf);
}

/* The console's power switch: everything starts over (Stella's own reset of
 * the System), as Stella's "reload" does without reading the file again. */
static void a26_reset(void)
{
    if (!s_loaded) return;
    s_console->system().reset();
}

static float a26_fps(void)
{
    if (!s_loaded || s_spf <= 0) return 59.92f;
    return (float)A26_RATE / (float)s_spf;
}

static int a26_sample_rate(void)
{
    return A26_RATE;
}

/* ---- save states --------------------------------------------------------
 *
 * The file: "P4A26ST\0", a version, the payload's length and CRC-32, the
 * cartridge's MD5 (32 characters), then Stella's own state as
 * Console::save() writes it into an in-memory Serializer. */

#define A26_ST_MAGIC   "P4A26ST"
#define A26_ST_VERSION 1u
#define A26_ST_HEAD    (8 + 4 + 4 + 4 + 32)
#define A26_ST_MAX     (1024 * 1024)

static uint32_t a26_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void a26_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t a26_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool a26_state_save(const char *path)
{
    if (!s_loaded) return false;
    Serializer out;
    if (!s_console->save(out)) return false;
    string data = out.get();

    uint8_t head[A26_ST_HEAD];
    memset(head, 0, sizeof(head));
    memcpy(head, A26_ST_MAGIC, 8);
    a26_put32(head + 8, A26_ST_VERSION);
    a26_put32(head + 12, (uint32_t)data.size());
    a26_put32(head + 16, a26_crc32((const uint8_t *)data.data(), data.size()));
    memcpy(head + 20, s_md5->c_str(), 32);

    size_t plen = strlen(path);
    char *tmp = (char *)malloc(plen + 5);
    if (!tmp) return false;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL;
    if (ok) {
        ok = fwrite(head, 1, sizeof(head), f) == sizeof(head)
          && fwrite(data.data(), 1, data.size(), f) == data.size();
        if (fclose(f) != 0) ok = false;
    }
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        s_host->log("a2600: writing the state %s failed", path);
    }
    free(tmp);
    return ok;
}

static bool a26_state_load(const char *path)
{
    if (!s_loaded) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    uint8_t *buf = NULL;
    bool ok = n > A26_ST_HEAD && n <= A26_ST_MAX && fseek(f, 0, SEEK_SET) == 0
           && (buf = (uint8_t *)malloc((size_t)n)) != NULL
           && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (ok) {
        uint32_t len = a26_get32(buf + 12);
        ok = memcmp(buf, A26_ST_MAGIC, 8) == 0
          && a26_get32(buf + 8) == A26_ST_VERSION
          && len == (uint32_t)(n - A26_ST_HEAD)
          && a26_crc32(buf + A26_ST_HEAD, len) == a26_get32(buf + 16)
          && memcmp(buf + 20, s_md5->c_str(), 32) == 0;
    }
    if (!ok) {
        free(buf);
        s_host->log("a2600: %s is not a state for this game", path);
        return false;
    }

    /* What the console is now, to go back to if Stella refuses the state
     * half way (its devices load one after another). */
    Serializer backup;
    bool have_backup = s_console->save(backup);

    Serializer in;
    in.set(string((const char *)buf + A26_ST_HEAD, (size_t)(n - A26_ST_HEAD)));
    free(buf);
    if (s_console->load(in)) return true;

    s_host->log("a2600: Stella refused the state");
    if (have_backup) {
        backup.reset();
        if (s_console->load(backup)) return false;
    }
    s_console->system().reset();
    return false;
}

static bool a26_sram_flush(void)
{
    return true;    /* no battery RAM on the 2600 */
}

/* ---- options and stats ------------------------------------------------- */

static void a26_set_option(const char *key, int value)
{
    if (strcmp(key, "diff_left") == 0) {
        s_opt_left = value >= 0 && value <= 2 ? value : 0;
    } else if (strcmp(key, "diff_right") == 0) {
        s_opt_right = value >= 0 && value <= 2 ? value : 0;
    } else if (strcmp(key, "tv_type") == 0) {
        s_opt_tv = value >= 0 && value <= 2 ? value : 0;
    } else if (strcmp(key, "phosphor") == 0) {
        s_opt_phosphor = value >= 0 && value <= 2 ? value : 0;
    } else if (strcmp(key, "palette") == 0) {
        s_opt_palette = value == 1 ? 1 : 0;
        a26_build_palette();
    }
}

static const char *a26_stats(void)
{
    if (!s_loaded) return NULL;
    int fps100 = s_spf > 0 ? (A26_RATE * 100) / s_spf : 0;
    snprintf(s_stats, sizeof(s_stats), "a2600: %.24s, %s, %d lines, %d.%02d fps",
             s_console->cartridge().about().c_str(), s_pal_tv ? "PAL" : "NTSC",
             (int)s_console->tia().scanlines(), fps100 / 100, fps100 % 100);
    return s_stats;
}

extern "C" const emu_core_t emu_core_a2600 = {
    .id = "a2600",
    .name = "Atari 2600",
    .short_name = "2600",
    .exts = ".a26 .bin",
    .max_w = A26_W,
    .max_h = A26_H_MAX,
    /* 160 pixels a line shown 4:3 with the NTSC's usual 210 lines:
     * 210 * 4 / 3 / 160. */
    .pixel_aspect = 1.75f,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_SELECT | EMU_BTN_START,
    .face = EMU_FACE_ONE,
    .labels = s_labels,
    .options = s_options,
    .n_options = A26_N_OPTIONS,
    .load = a26_load,
    .unload = a26_unload,
    .reset = a26_reset,
    .run = a26_run,
    .fps = a26_fps,
    .sample_rate = a26_sample_rate,
    .state_save = a26_state_save,
    .state_load = a26_state_load,
    .sram_flush = a26_sram_flush,
    .set_option = a26_set_option,
    .stats = a26_stats,
};
