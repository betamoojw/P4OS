/*
 * P4OS - Emulador, Super Nintendo core: Snes9x (the snes9x2005 / CATSFC
 * line, as Retro-Go carries it) behind the emu_core_t table.
 *
 * Snes9x is one console held in globals (CPU, PPU, APU, Memory, GFX...):
 * one game at a time, which is all the app asks for. Each run() is one
 * S9xMainLoop(), which returns at the top of the frame with the picture
 * of the frame just drawn in GFX.Screen (RGB565, R in the top bits, the
 * CPU's byte order: what LVGL takes). The sound is mixed afterwards, a
 * frame's worth at a time, by Snes9x's own DSP (soundux.c).
 *
 * What Snes9x does not do and this file does: the battery save as a raw
 * .srm/.sav, save states that are checked whole before anything is
 * touched, the 512-pixel modes brought down to 256 for the app, and the
 * refusal of games with chips the core does not have.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"

#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "gfx.h"
#include "apu.h"
#include "soundux.h"
#include "cpuexec.h"
#include "display.h"
#include "dma.h"
#include "dsp.h"
#include "obc1.h"

#ifndef N_
#define N_(s) s
#endif

/* Snes9x's HDMA pointers (globals.c, used by dma.c without a header). */
extern uint8_t *HDMAMemPointers[8];
extern uint8_t *HDMABasePointers[8];

/* The DSP's own rate. Snes9x mixes whatever number of samples it is asked
 * for, so a frame gets a whole number of them (533 NTSC, 640 PAL) and the
 * frame rate the app is told is worked out from that (snes_fps): sound
 * and picture never drift. */
#define SNES_RATE         32000
#define SNES_SPF_NTSC     533       /* 32000 / 533 = 60.04 (the console: 60.10) */
#define SNES_SPF_PAL      640       /* 32000 / 640 = 50.00 (the console: 50.01) */

/* The largest cartridges are 6 MB (Tales of Phantasia, Star Ocean) and
 * the odd 8 MB hack. With the copier header. */
#define SNES_ROM_MAX      (0x800000 + 0x200)

/* Snes9x draws the 512-pixel modes (5, 6, interlace) 512 wide and an
 * interlaced picture 478 lines high: the buffers are that big, the pitch
 * is always 1024 bytes. A normal picture uses the left half of the top
 * 224 or 239 lines. */
#define SNES_PITCH_PX     512
#define SNES_BUF_LINES    (SNES_HEIGHT_EXTENDED * 2)

static const emu_host_t *s_host;
static bool s_loaded;

static uint8_t *s_screen;           /* GFX.Screen / SubScreen, ZBuffers     */
static uint8_t *s_subscreen;
static uint8_t *s_zbuf;
static uint8_t *s_subzbuf;
static uint16_t *s_frame;           /* a 512-wide picture brought to 256     */
static int16_t *s_audio;            /* a frame of sound, stereo              */
static int s_spf;                   /* stereo samples a frame                */

static uint32_t s_pads[2];          /* SNES_*_MASK, read by S9xReadJoypad    */

/* The battery save: where it goes, its size, and the RAM as it was last
 * written (or read), to tell whether the game changed it. */
static char *s_save_path;
static uint8_t *s_sram_copy;
static size_t s_sram_size;

/* Options, as the indices of their values below. */
static int s_opt_sound;             /* 0 high, 1 normal, 2 fast, 3 off       */
static bool s_opt_crop = true;

/* The second core (see snes_split() and README.md). The host's
 * parallel(fn, arg) runs fn(arg, 1) on a helper on the other core and
 * fn(arg, 0) on the caller, and returns when both are done. It is not in
 * emu_core.h yet (proposed as emu_host_t.parallel): until it is, the stub
 * below runs the two halves one after the other on the worker, which
 * gives the same results, only without the time saved. */
typedef void (*snes_half_fn)(void *arg, int half);
typedef void (*snes_parallel_fn)(snes_half_fn fn, void *arg);

#if !defined(SNES_TEST_PARALLEL) && !defined(SNES_NO_SPLIT)
static void snes_parallel_stub(snes_half_fn fn, void *arg)
{
    fn(arg, 1);
    fn(arg, 0);
}
#endif

#ifdef SNES_TEST_PARALLEL
/* The Mac test harness hands in a real one (two threads). */
snes_parallel_fn snes_test_parallel(void);
#endif

static snes_parallel_fn s_parallel;
static bool s_mix_pending;          /* a frame emulated whose sound is not mixed yet */
static bool s_draw;                 /* this frame is drawn                   */

/* What the frames were like, for stats(). */
static uint32_t s_frames, s_frames_hires;
static char s_stats[112];

/* ---- options ----------------------------------------------------------- */

static const char *const s_sound_values[] = {
    N_("Alta"), N_("Normal"), N_("Rápida"), N_("Sin sonido"), NULL,
};
static const char *const s_yes_no[] = { N_("Sí"), N_("No"), NULL };

static const emu_option_t s_options[] = {
    /* Alta: interpolated, with echo. Normal: no interpolation. Rápida: no
     * echo either. Sin sonido: the DSP is not mixed at all (the sound CPU
     * still runs: the games wait on it). */
    { "sound_quality", N_("Calidad del sonido"), s_sound_values, 0 },
    { "overscan",      N_("Recortar bordes"),    s_yes_no,       0 },
};
#define SNES_N_OPTIONS ((int)(sizeof(s_options) / sizeof(s_options[0])))

/* The names on a Super Nintendo pad are the letters, which is the default;
 * only SELECT and START are spelled as the pad has them. */
static const char *const s_labels[EMU_BTN_COUNT] = {
    [10] = "SELECT",
    [11] = "START",
};

static void snes_apply_sound_options(void)
{
    Settings.InterpolatedSound = (s_opt_sound == 0);
    bool no_echo = (s_opt_sound >= 2);
    if (Settings.DisableSoundEcho != no_echo) {
        Settings.DisableSoundEcho = no_echo;
        /* S9xSetEchoEnable() reads the setting; with echo coming back the
         * history starts clean, as Snes9x does when a game turns it on. */
        if (s_loaded) S9xSetEchoEnable(APU.DSP[APU_EON]);
    }
}

/* ---- what Snes9x asks of the port (display.h, ppu.h) -------------------- */

uint32_t S9xReadJoypad(int32_t port)
{
    if (port < 0 || port > 1) return 0;
    return s_pads[port];
}

bool S9xReadMousePosition(int32_t which, int32_t *x, int32_t *y, uint32_t *buttons)
{
    (void)which; (void)x; (void)y; (void)buttons;
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    (void)x; (void)y; (void)buttons;
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

/* The buffers are set up by snes_load() itself; Snes9x never calls these. */
bool S9xInitDisplay(void)
{
    return true;
}

void S9xDeinitDisplay(void)
{
}

void S9xToggleSoundChannel(int32_t channel)
{
    (void)channel;
}

static uint32_t snes_pad_bits(uint32_t b)
{
    uint32_t p = 0;
    if (b & EMU_BTN_A)      p |= SNES_A_MASK;
    if (b & EMU_BTN_B)      p |= SNES_B_MASK;
    if (b & EMU_BTN_X)      p |= SNES_X_MASK;
    if (b & EMU_BTN_Y)      p |= SNES_Y_MASK;
    if (b & EMU_BTN_L)      p |= SNES_TL_MASK;
    if (b & EMU_BTN_R)      p |= SNES_TR_MASK;
    if (b & EMU_BTN_SELECT) p |= SNES_SELECT_MASK;
    if (b & EMU_BTN_START)  p |= SNES_START_MASK;
    if (b & EMU_BTN_UP)     p |= SNES_UP_MASK;
    if (b & EMU_BTN_DOWN)   p |= SNES_DOWN_MASK;
    if (b & EMU_BTN_LEFT)   p |= SNES_LEFT_MASK;
    if (b & EMU_BTN_RIGHT)  p |= SNES_RIGHT_MASK;
    /* Up and down (left and right) at once lock some games up, and no pad
     * can press both: the second one wins nothing. */
    if ((p & SNES_UP_MASK) && (p & SNES_DOWN_MASK))    p &= ~SNES_DOWN_MASK;
    if ((p & SNES_LEFT_MASK) && (p & SNES_RIGHT_MASK)) p &= ~SNES_RIGHT_MASK;
    /* Bit 31: a pad is plugged in (Snes9x's convention). */
    return p | 0x80000000u;
}

/* ---- the battery save --------------------------------------------------- */

static size_t snes_sram_bytes(void)
{
    if (!Memory.SRAMSize || !Memory.SRAM) return 0;
    size_t n = (size_t)Memory.SRAMMask + 1;
    return n > SRAM_SIZE ? SRAM_SIZE : n;
}

static char *snes_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* Reads the save into the cartridge's RAM (if it has any) and keeps a copy
 * to compare against. The file is the RAM as it is, the size the cartridge
 * says (.srm in Snes9x, .sav for most others); a shorter or longer file is
 * read as far as it goes. */
static void snes_sram_open(const char *path)
{
    size_t size = snes_sram_bytes();
    if (!path || !size) return;

    s_save_path = snes_strdup(path);
    s_sram_copy = malloc(size);
    if (!s_save_path || !s_sram_copy) {
        free(s_save_path);
        free(s_sram_copy);
        s_save_path = NULL;
        s_sram_copy = NULL;
        s_host->log("no memory for the battery save");
        return;
    }
    s_sram_size = size;

    FILE *f = fopen(path, "rb");
    if (f) {
        size_t got = fread(Memory.SRAM, 1, size, f);
        fclose(f);
        s_host->log("battery save read (%u of %u bytes)", (unsigned)got, (unsigned)size);
    }
    memcpy(s_sram_copy, Memory.SRAM, size);
}

static bool snes_sram_flush(void)
{
    if (!s_loaded || !s_sram_copy) return true;
    if (memcmp(Memory.SRAM, s_sram_copy, s_sram_size) == 0) return true;

    FILE *f = fopen(s_save_path, "wb");
    if (!f) {
        s_host->log("cannot write %s", s_save_path);
        return false;
    }
    bool ok = fwrite(Memory.SRAM, 1, s_sram_size, f) == s_sram_size;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        memcpy(s_sram_copy, Memory.SRAM, s_sram_size);
    } else {
        s_host->log("writing %s failed", s_save_path);
    }
    return ok;
}

/* ---- load and unload ---------------------------------------------------- */

/* The screens' margins (see snes_init_display()). */
#define SNES_GUARD 64

static uint8_t *snes_guarded(size_t bytes)
{
    uint8_t *p = calloc(1, bytes + 2 * SNES_GUARD);
    return p ? p + SNES_GUARD : NULL;
}

static void snes_unguard(uint8_t *p)
{
    if (p) free(p - SNES_GUARD);
}



/* Every global Snes9x keeps, back to zero, so a second game starts from
 * what the first one did not leave. The allocations are gone by now. */
static void snes_clear_globals(void)
{
    memset(&CPU, 0, sizeof(CPU));
    memset(&ICPU, 0, sizeof(ICPU));
    memset(&APU, 0, sizeof(APU));
    memset(&IAPU, 0, sizeof(IAPU));
    memset(&SoundData, 0, sizeof(SoundData));
    memset(&so, 0, sizeof(so));
    memset(&Settings, 0, sizeof(Settings));
    memset(&Memory, 0, sizeof(Memory));
    memset(&PPU, 0, sizeof(PPU));
    memset(&IPPU, 0, sizeof(IPPU));
    memset(DMA, 0, sizeof(DMA));
    memset(&BG, 0, sizeof(BG));
    memset(&GFX, 0, sizeof(GFX));
    memset(HDMAMemPointers, 0, sizeof(HDMAMemPointers));
    memset(HDMABasePointers, 0, sizeof(HDMABasePointers));
    OpenBus = 0;
}

static void snes_free_all(void)
{
    /* Each of these frees only what was taken and clears its pointers;
     * after a failed load some were never called. */
    if (GFX.Screen) S9xDeinitGFX();
    S9xMixRAMSplit(false);
    S9xDSPDeinitDeferred();
    S9xDeinitSound();
    S9xDeinitAPU();
    S9xDeinitDSP();
    S9xDeinitMemory();

    snes_unguard(s_screen);
    snes_unguard(s_subscreen);
    snes_unguard(s_zbuf);
    snes_unguard(s_subzbuf);
    free(s_frame);
    free(s_audio);
    free(s_save_path);
    free(s_sram_copy);
    s_screen = s_subscreen = s_zbuf = s_subzbuf = NULL;
    s_frame = NULL;
    s_audio = NULL;
    s_save_path = NULL;
    s_sram_copy = NULL;
    s_sram_size = 0;
    s_loaded = false;
    s_mix_pending = false;
    snes_clear_globals();
}

static void snes_unload(void)
{
    snes_free_all();
    s_host = NULL;
}

/* What Retro-Go's driver sets before anything else (main_snes.c). */
static void snes_settings(void)
{
    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = SNES_RATE;
    Settings.SoundInputRate = SNES_RATE;
    Settings.DisableSoundEcho = (s_opt_sound >= 2);
    Settings.InterpolatedSound = (s_opt_sound == 0);
}

/* The screens Snes9x draws into (what S9xInitDisplay() does in other
 * ports), in PSRAM. 2.4 MB in all: the 512 x 478 the interlaced
 * high-resolution modes need. Each has a margin of SNES_GUARD bytes on
 * both sides: a tile clipped at the left edge reads the depth buffer one
 * byte before its start (seen with AddressSanitizer in mode 5), as it does
 * in every Snes9x of this line. */
static bool snes_init_display(void)
{
    size_t px = (size_t)SNES_PITCH_PX * SNES_BUF_LINES;
    s_screen = snes_guarded(px * 2);
    s_subscreen = snes_guarded(px * 2);
    s_zbuf = snes_guarded(px);
    s_subzbuf = snes_guarded(px);
    s_frame = malloc((size_t)SNES_WIDTH * SNES_HEIGHT_EXTENDED * 2);
    if (!s_screen || !s_subscreen || !s_zbuf || !s_subzbuf || !s_frame) return false;
    GFX.Pitch = SNES_PITCH_PX * 2;
    GFX.Screen = s_screen;
    GFX.SubScreen = s_subscreen;
    GFX.ZBuffer = s_zbuf;
    GFX.SubZBuffer = s_subzbuf;
    return true;
}

/* Reads the whole file into a buffer of its own size. The buffer is the
 * cartridge from then on (Memory.ROM, freed by S9xDeinitMemory()). Some
 * maps and one patch (WWF Super WrestleMania) look up to 2 MB in and a
 * bank past the end, so it is never smaller than 2 MB + 64 KB. */
static bool snes_read_rom(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return false;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return false;
    }
    if (size < 0x8000) {
        fclose(f);
        snprintf(err, errlen, "No es un juego de Super Nintendo");
        return false;
    }
    if (size > SNES_ROM_MAX) {
        fclose(f);
        snprintf(err, errlen, "El archivo es demasiado grande");
        return false;
    }
    size_t alloc = (size_t)size < 0x200000 ? 0x200000 : (size_t)size;
    alloc += 0x10000 + 0x200;
    uint8_t *rom = calloc(1, alloc);
    if (!rom) {
        fclose(f);
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    size_t got = fread(rom, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(rom);
        snprintf(err, errlen, "No se pudo leer el archivo");
        return false;
    }
    Memory.ROM = rom;
    Memory.ROM_Offset = 0;
    Memory.ROM_AllocSize = (size_t)size;    /* what LoadROM(NULL) takes as the file */
    return true;
}

/* The chips this Snes9x does not have: the game would hang or show
 * nothing, so it is refused with a message the player can act on. */
static const char *snes_missing_chip(void)
{
    if (Settings.SuperFX)
        return "Este juego usa el chip Super FX, que el emulador no tiene";
    if ((Memory.ROMSpeed & ~0x10) == 0x23 && (Memory.ROMType & 0xF0) == 0x30)
        return "Este juego usa el chip SA-1, que el emulador no tiene";
    if (Settings.SDD1)
        return "Este juego usa el chip S-DD1, que el emulador no tiene";
    if (Settings.SPC7110)
        return "Este juego usa el chip SPC7110, que el emulador no tiene";
    return NULL;
}

static const char *snes_chip_name(void)
{
    if (Settings.C4) return "C4";
    if (Settings.OBC1) return "OBC1";
    if (Settings.SRTC) return "S-RTC";
    switch (Settings.DSP) {
    case 1: return "DSP-1";
    case 2: return "DSP-2";
    case 3: return "DSP-3";
    case 4: return "DSP-4";
    default: return NULL;
    }
}

static bool snes_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    snes_free_all();
    s_host = host;
    s_pads[0] = s_pads[1] = 0;
    s_frames = s_frames_hires = 0;

    snes_settings();

    if (!snes_init_display() || !S9xInitMemory() || !S9xInitAPU() || !S9xInitSound(0, 0)) {
        snes_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    /* S9xInitGFX() reads GFX.Pitch and the buffers set just above. */
    if (!S9xInitGFX()) {
        snes_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    if (!snes_read_rom(req->rom, err, errlen)) {
        snes_free_all();
        return false;
    }
    if (!LoadROM(NULL)) {
        snes_free_all();
        snprintf(err, errlen, "No es un juego de Super Nintendo");
        return false;
    }
    const char *chip = snes_missing_chip();
    if (chip) {
        host->log("%s needs a chip the core lacks (type %02X, map %02X)",
                  Memory.ROMName, Memory.ROMType, Memory.ROMSpeed);
        snes_free_all();
        snprintf(err, errlen, "%s", chip);
        return false;
    }
    size_t dsp_size;
    if (Settings.DSP && !S9xDSPState(&dsp_size)) {
        snes_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    S9xSetPlaybackRate(SNES_RATE);
    s_spf = Settings.PAL ? SNES_SPF_PAL : SNES_SPF_NTSC;
    s_audio = calloc((size_t)s_spf * 2, sizeof(int16_t));
    if (!s_audio) {
        snes_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

#if defined(SNES_TEST_PARALLEL)
    s_parallel = snes_test_parallel();
#elif defined(SNES_NO_SPLIT)
    s_parallel = NULL;              /* everything in series, the sound unsplit */
#else
    /* P4OS: the app's other core (emu_host_t.parallel, aos_hal_worker_split) */
    s_parallel = host->parallel ? host->parallel : snes_parallel_stub;
#endif
    if (s_parallel && (!S9xDSPInitDeferred() || !S9xMixRAMSplit(true))) {
        snes_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    s_mix_pending = false;

    s_loaded = true;
    snes_sram_open(req->save);

    const char *c = snes_chip_name();
    host->log("\"%s\" %s %s, %u KB, SRAM %u KB%s%s",
              Memory.ROMName, Memory.HiROM ? "HiROM" : "LoROM",
              Settings.PAL ? "PAL" : "NTSC",
              (unsigned)(Memory.CalculatedSize / 1024),
              (unsigned)(snes_sram_bytes() / 1024),
              c ? ", " : "", c ? c : "");
    return true;
}

/* ---- running ------------------------------------------------------------ */

/* Two pixels into one, rounding down: (a + b) / 2 per channel without
 * the carries running into the next channel. */
static inline uint16_t snes_mix565(uint16_t a, uint16_t b)
{
    return (uint16_t)((a & b) + (((a ^ b) & 0xF7DEu) >> 1));
}

static void snes_blit(void)
{
    int h = IPPU.RenderedScreenHeight;
    bool interlaced = IPPU.DoubleHeightPixels;
    if (interlaced) h >>= 1;                 /* one field: the even lines */
    int step = interlaced ? 2 : 1;           /* lines of the buffer a line */
    if (h <= 0) return;
    if (h > SNES_HEIGHT_EXTENDED) h = SNES_HEIGHT_EXTENDED;

    /* The overscan mode is 239 lines; the TV showed the middle 224. */
    int y0 = 0;
    if (s_opt_crop && h > SNES_HEIGHT) {
        y0 = (h - SNES_HEIGHT) / 2;
        h = SNES_HEIGHT;
    }

    const uint16_t *src = (const uint16_t *)s_screen + (size_t)y0 * step * SNES_PITCH_PX;
    s_frames++;

    if (IPPU.RenderedScreenWidth <= SNES_WIDTH && !interlaced) {
        /* The usual case: hand the buffer over as it is. */
        s_host->video(src, SNES_WIDTH, h, SNES_PITCH_PX * 2);
        return;
    }

    /* 512 wide (modes 5 and 6, pseudo high resolution, interlace): two
     * pixels into one, which is also what the transparencies made with it
     * look like on a TV. Lower-resolution lines in the same picture were
     * already doubled by Snes9x, so they come back as they were. */
    s_frames_hires++;
    uint16_t *dst = s_frame;
    for (int y = 0; y < h; y++) {
        const uint16_t *s = src + (size_t)y * step * SNES_PITCH_PX;
        if (IPPU.RenderedScreenWidth > SNES_WIDTH) {
            for (int x = 0; x < SNES_WIDTH; x++) dst[x] = snes_mix565(s[2 * x], s[2 * x + 1]);
        } else {
            memcpy(dst, s, SNES_WIDTH * 2);
        }
        dst += SNES_WIDTH;
    }
    s_host->video(s_frame, SNES_WIDTH, h, SNES_WIDTH * 2);
}

/* A frame's sound, into s_audio: silence with the sound off. */
static void snes_mix(void)
{
    if (s_opt_sound == 3) {
        memset(s_audio, 0, (size_t)s_spf * 2 * sizeof(int16_t));
    } else {
        S9xMixSamples(s_audio, s_spf * 2);
    }
}

/*
 * The two halves of a frame with the sound split: half 0 (the worker)
 * runs the console through this frame and draws it, half 1 (the other
 * core) mixes the sound of the frame before. They share nothing while
 * they run: the sound CPU's writes to the DSP wait in a queue (apu.c), its
 * reads of the DSP come from a picture taken before the halves started,
 * and the mixer reads its samples from its own copy of the sound RAM. So
 * the result does not depend on which half runs first or whether they
 * overlap, and the stub that runs them in series gives the same frames
 * and the same sound as the two cores.
 */
static void snes_split(void *arg, int half)
{
    (void)arg;
    if (half == 1) {
        if (s_mix_pending) snes_mix();
        else memset(s_audio, 0, (size_t)s_spf * 2 * sizeof(int16_t));
    } else {
        S9xMainLoop();
        if (s_draw) snes_blit();
    }
}

static void snes_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;

    s_pads[0] = snes_pad_bits(pads ? pads[0] : 0);
    s_pads[1] = snes_pad_bits(pads ? pads[1] : 0);

    s_draw = draw;
    IPPU.RenderThisFrame = draw;
    GFX.Screen = s_screen;

    if (s_parallel) {
        S9xDSPBeginDeferred();
        s_parallel(snes_split, NULL);
        /* Both done: the mixer's RAM catches up with the frame, then the
         * frame's DSP writes are made (key-ons read the sample directory
         * from that RAM), ready for the mix in the next frame. */
        S9xMixRAMSync(false);
        S9xDSPEndDeferred();
        s_mix_pending = true;
    } else {
        S9xMainLoop();
        if (draw) snes_blit();
        snes_mix();
    }
    /* With the sound split this is the frame before's: a frame later than
     * the picture, the same number of samples. */
    s_host->audio(s_audio, s_spf);
}

/* The console's power switch: the battery RAM survives it (S9xReset()
 * leaves Memory.SRAM alone), the rest starts again. */
static void snes_reset(void)
{
    if (!s_loaded) return;
    S9xReset();
    s_mix_pending = false;
}

static float snes_fps(void)
{
    if (!s_loaded) return (float)SNES_RATE / SNES_SPF_NTSC;
    return (float)SNES_RATE / (float)s_spf;
}

static int snes_sample_rate(void)
{
    return SNES_RATE;
}

/* ---- save states -------------------------------------------------------- */

/*
 * The file: "P4SNES" + version, a word that changes with the layout of
 * Snes9x's structs in this build (their sizes and the pointer size), the
 * game's header bytes, then the blocks, each a 4-letter tag and a length.
 * Snes9x's structs go in raw, as its own snapshot.c writes them: a state
 * is for this build and this game, and anything else is refused before
 * the game is touched. The whole file is read and checked first.
 *
 * Pointers inside the structs are stored as they were and put right on
 * the way in (as snapshot.c does), relative to what they point into.
 */
#define ST_MAGIC   "P4SNES01"

typedef struct {
    char     tag[4];
    uint32_t len;
} st_block_t;

typedef struct {
    uint32_t pc_at_opcode;          /* CPU.PCAtOpcodeStart - CPU.PCBase */
    uint32_t wait_address;          /* CPU.WaitAddress - CPU.PCBase     */
    uint8_t  has_pc_at_opcode, has_wait_address;
    uint8_t  ippu_hdma;
    uint8_t  ippu_first_vram_read;
    int32_t  ippu_controller;
    uint32_t ippu_frame_count;
    uint8_t  key_on[2];
    uint8_t  obc1;                  /* the OBC1 registers follow if set */
    uint8_t  mix_pending;           /* the frame's sound is not mixed yet */
    int32_t  obc1_regs[3];
} st_extra_t;

extern int32_t OBC1_Address, OBC1_BasePtr, OBC1_Shift;

static uint32_t st_layout(void)
{
    uint32_t h = 2166136261u;
    const uint32_t sizes[] = {
        sizeof(CPU), sizeof(ICPU), sizeof(PPU), sizeof(DMA), sizeof(APU),
        sizeof(IAPU), sizeof(SoundData), sizeof(void *), sizeof(st_extra_t),
        SNES_SPF_NTSC, SNES_RATE,
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        h = (h ^ sizes[i]) * 16777619u;
    }
    return h;
}

/* The game: its header as Snes9x found it (64 bytes at the header's place)
 * and its size. */
static void st_game_id(uint8_t id[68])
{
    uint32_t base = Memory.HiROM ? 0xFFC0 : 0x7FC0;
    if (Memory.ExtendedFormat == BIGFIRST) base += 0x400000;
    memcpy(id, Memory.ROM + base, 64);
    uint32_t n = Memory.CalculatedSize;
    id[64] = (uint8_t)n;
    id[65] = (uint8_t)(n >> 8);
    id[66] = (uint8_t)(n >> 16);
    id[67] = (uint8_t)(n >> 24);
}

typedef struct {
    const char *tag;
    void       *ptr;
    size_t      len;
} st_item_t;

/* Every block of a state for the game loaded now, in order. The extra
 * one is filled in by the caller. */
static int st_items(st_item_t *it, st_extra_t *extra)
{
    int n = 0;
    size_t mix_len, dsp_len;
    void *mix = S9xSoundMixerState(&mix_len);
    void *dsp = S9xDSPState(&dsp_len);
    it[n++] = (st_item_t){ "CPU ", &CPU, sizeof(CPU) };
    it[n++] = (st_item_t){ "ICPU", &ICPU, sizeof(ICPU) };
    it[n++] = (st_item_t){ "PPU ", &PPU, sizeof(PPU) };
    it[n++] = (st_item_t){ "DMA ", DMA, sizeof(DMA) };
    it[n++] = (st_item_t){ "VRAM", Memory.VRAM, VRAM_SIZE };
    it[n++] = (st_item_t){ "WRAM", Memory.RAM, RAM_SIZE };
    it[n++] = (st_item_t){ "SRAM", Memory.SRAM, SRAM_SIZE };
    it[n++] = (st_item_t){ "FILL", Memory.FillRAM, FILLRAM_SIZE };
    it[n++] = (st_item_t){ "APU ", &APU, sizeof(APU) };
    it[n++] = (st_item_t){ "IAPU", &IAPU, sizeof(IAPU) };
    it[n++] = (st_item_t){ "ARAM", IAPU.RAM, 0x10000 };
    it[n++] = (st_item_t){ "SND ", &SoundData, sizeof(SoundData) };
    it[n++] = (st_item_t){ "SO  ", &so, sizeof(so) };
    it[n++] = (st_item_t){ "MIX ", mix, mix_len };
    if (dsp) it[n++] = (st_item_t){ "DSPn", dsp, dsp_len };
    it[n++] = (st_item_t){ "XTRA", extra, sizeof(*extra) };
    return n;
}
#define ST_MAX_ITEMS 16

static bool st_write_all(FILE *f, const void *p, size_t n)
{
    return fwrite(p, 1, n, f) == n;
}

static bool snes_state_save(const char *path)
{
    if (!s_loaded || !path) return false;

    st_extra_t x;
    memset(&x, 0, sizeof(x));
    if (CPU.PCAtOpcodeStart) {
        x.has_pc_at_opcode = 1;
        x.pc_at_opcode = (uint32_t)(CPU.PCAtOpcodeStart - CPU.PCBase);
    }
    if (CPU.WaitAddress) {
        x.has_wait_address = 1;
        x.wait_address = (uint32_t)(CPU.WaitAddress - CPU.PCBase);
    }
    x.ippu_hdma = IPPU.HDMA;
    x.ippu_first_vram_read = IPPU.FirstVRAMRead;
    x.ippu_controller = IPPU.Controller;
    x.ippu_frame_count = IPPU.FrameCount;
    S9xAPUGetKeyOnState(x.key_on);
    x.mix_pending = s_parallel ? s_mix_pending : 0;
    if (Settings.OBC1) {
        x.obc1 = 1;
        x.obc1_regs[0] = OBC1_Address;
        x.obc1_regs[1] = OBC1_BasePtr;
        x.obc1_regs[2] = OBC1_Shift;
    }

    st_item_t it[ST_MAX_ITEMS];
    int n = st_items(it, &x);

    size_t plen = strlen(path);
    char *tmp = malloc(plen + 5);
    if (!tmp) return false;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        s_host->log("cannot write %s", tmp);
        free(tmp);
        return false;
    }
    uint8_t id[68];
    st_game_id(id);
    uint32_t layout = st_layout();
    uint32_t count = (uint32_t)n;
    bool ok = st_write_all(f, ST_MAGIC, 8)
           && st_write_all(f, &layout, 4)
           && st_write_all(f, id, sizeof(id))
           && st_write_all(f, &count, 4);
    for (int i = 0; ok && i < n; i++) {
        st_block_t b;
        memcpy(b.tag, it[i].tag, 4);
        b.len = (uint32_t)it[i].len;
        ok = st_write_all(f, &b, sizeof(b)) && st_write_all(f, it[i].ptr, it[i].len);
    }
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) {
        /* FAT will not rename over a file: take the old one out first. */
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        s_host->log("writing the state %s failed", path);
    }
    free(tmp);
    return ok;
}

static bool snes_state_load(const char *path)
{
    if (!s_loaded || !path) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 84 || size > 4 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        s_host->log("%s is not a state", path);
        return false;
    }
    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        s_host->log("no memory to read the state");
        return false;
    }
    bool got = fread(buf, 1, (size_t)size, f) == (size_t)size;
    fclose(f);

    /* The header: this format, this build, this game. */
    uint8_t id[68];
    st_game_id(id);
    uint32_t layout, count;
    const uint8_t *p = buf;
    const uint8_t *end = buf + size;
    if (!got || memcmp(p, ST_MAGIC, 8) != 0) goto refuse;
    memcpy(&layout, p + 8, 4);
    if (layout != st_layout()) goto refuse;
    if (memcmp(p + 12, id, sizeof(id)) != 0) goto refuse;
    memcpy(&count, p + 80, 4);
    p += 84;

    /* Every block where this game expects it, with its length. */
    st_extra_t x;
    st_item_t it[ST_MAX_ITEMS];
    int n = st_items(it, &x);
    if (count != (uint32_t)n) goto refuse;
    const uint8_t *data[ST_MAX_ITEMS];
    for (int i = 0; i < n; i++) {
        st_block_t b;
        if ((size_t)(end - p) < sizeof(b)) goto refuse;
        memcpy(&b, p, sizeof(b));
        p += sizeof(b);
        if (memcmp(b.tag, it[i].tag, 4) != 0 || b.len != it[i].len
            || (size_t)(end - p) < b.len)
            goto refuse;
        data[i] = p;
        p += b.len;
    }
    if (p != end) goto refuse;

    /* From here on it is all copying. Like snapshot.c: a reset first, so
     * whatever the structs do not hold (caches, tile buffers) starts
     * clean, then the blocks over it, then the pointers put right. */
    S9xReset();
    uint8_t *iapu_ram = IAPU.RAM;
    for (int i = 0; i < n; i++) memcpy(it[i].ptr, data[i], it[i].len);
    free(buf);

    IAPU.PC = IAPU.PC - IAPU.RAM + iapu_ram;
    IAPU.DirectPage = IAPU.DirectPage - IAPU.RAM + iapu_ram;
    if (IAPU.WaitAddress1) IAPU.WaitAddress1 = IAPU.WaitAddress1 - IAPU.RAM + iapu_ram;
    if (IAPU.WaitAddress2) IAPU.WaitAddress2 = IAPU.WaitAddress2 - IAPU.RAM + iapu_ram;
    IAPU.RAM = iapu_ram;

    FixROMSpeed();
    IPPU.ColorsChanged = true;
    IPPU.OBJChanged = true;
    CPU.InDMA = false;
    S9xFixColourBrightness();
    S9xAPUUnpackStatus();
    S9xSoundFixPointers();
    S9xAPUSetKeyOnState(x.key_on);
    ICPU.ShiftedPB = ICPU.Registers.PB << 16;
    ICPU.ShiftedDB = ICPU.Registers.DB << 16;
    S9xSetPCBase(ICPU.ShiftedPB + ICPU.Registers.PC);
    CPU.PCAtOpcodeStart = x.has_pc_at_opcode ? CPU.PCBase + x.pc_at_opcode : NULL;
    CPU.WaitAddress = x.has_wait_address ? CPU.PCBase + x.wait_address : NULL;
    S9xUnpackStatus();
    S9xFixCycles();
    /* Not S9xReschedule(), which snapshot.c calls: CPU.WhichEvent and
     * NextEvent are in the state already, and rescheduling again moves on
     * to the next event, skipping line 0's HDMA (the first frame after a
     * load came out with its top line and every HDMA effect one line off). */
    IPPU.HDMA = x.ippu_hdma;
    IPPU.FirstVRAMRead = x.ippu_first_vram_read;
    IPPU.Controller = x.ippu_controller;
    IPPU.FrameCount = x.ippu_frame_count;
    for (int i = 0; i < 8; i++) HDMAMemPointers[i] = HDMABasePointers[i] = NULL;
    if (x.obc1) {
        OBC1_Address = x.obc1_regs[0];
        OBC1_BasePtr = x.obc1_regs[1];
        OBC1_Shift = x.obc1_regs[2];
    }
    /* The echo setting is this session's, not the state's. */
    S9xSetEchoEnable(APU.DSP[APU_EON]);
    /* The mixer's RAM is the state's, and its sound picks up where the
     * state left it: a frame still to mix is mixed in the next run() with
     * the sound split, or now (and dropped) without. */
    S9xMixRAMSync(true);
    if (s_parallel) {
        s_mix_pending = x.mix_pending != 0;
    } else {
        if (x.mix_pending) snes_mix();
        s_mix_pending = false;
    }
    return true;

refuse:
    free(buf);
    s_host->log("%s is not a state for this game and build", path);
    return false;
}

/* ---- options and stats -------------------------------------------------- */

static void snes_set_option(const char *key, int value)
{
    if (strcmp(key, "sound_quality") == 0) {
        if (value < 0 || value > 3) value = 0;
        s_opt_sound = value;
        snes_apply_sound_options();
    } else if (strcmp(key, "overscan") == 0) {
        s_opt_crop = (value == 0);
    }
}

static const char *snes_stats(void)
{
    if (!s_loaded) return NULL;
    const char *c = snes_chip_name();
    unsigned hires = s_frames ? (unsigned)((s_frames_hires * 100u) / s_frames) : 0;
    snprintf(s_stats, sizeof(s_stats), "%s %s%s%s, SRAM %uK, %u%% 512 px",
             Memory.HiROM ? "HiROM" : "LoROM", Settings.PAL ? "PAL" : "NTSC",
             c ? " " : "", c ? c : "",
             (unsigned)(s_sram_size / 1024), hires);
    s_frames = s_frames_hires = 0;
    return s_stats;
}

const emu_core_t emu_core_snes = {
    .id = "snes",
    .name = "Super Nintendo",
    .short_name = "SNES",
    .exts = ".sfc .smc .swc .fig",
    .max_w = SNES_WIDTH,
    .max_h = SNES_HEIGHT_EXTENDED,
    .pixel_aspect = 8.0f / 7.0f,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_X | EMU_BTN_Y
             | EMU_BTN_L | EMU_BTN_R | EMU_BTN_SELECT | EMU_BTN_START,
    .face = EMU_FACE_DIAMOND,
    .labels = s_labels,
    .options = s_options,
    .n_options = SNES_N_OPTIONS,
    .load = snes_load,
    .unload = snes_unload,
    .reset = snes_reset,
    .run = snes_run,
    .fps = snes_fps,
    .sample_rate = snes_sample_rate,
    .state_save = snes_state_save,
    .state_load = snes_state_load,
    .sram_flush = snes_sram_flush,
    .set_option = snes_set_option,
    .stats = snes_stats,
};
