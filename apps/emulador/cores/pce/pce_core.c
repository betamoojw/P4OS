/*
 * P4OS - Emulador, PC Engine core: pce-go (Retro-Go's PC Engine emulator,
 * from HuExpress and Hu-Go!) behind the emu_core_t table emu_core_pce.
 * HuCards only: no CD-ROM, no Arcade Card.
 *
 * pce-go holds one console in the PCE global. It draws each frame as
 * 8-bit indices into a buffer XBUF_WIDTH (368) bytes a line, as wide as
 * the game's mode (256, 320, 336 or 352 dots, which a game may change
 * between screens), and its sound generator is asked for samples a
 * scanline at a time (pce.c). This file turns the picture into RGB565 for
 * the host (compressing the wide modes to the TV's proportions, by
 * default) and hands the sound on.
 *
 * What pce-go does not do and this file does: reading the HuCard file,
 * padding the image so no bank maps past its end, the backup RAM as a
 * .sav, writing a state through a temporary file.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"
#include "pce-go.h"
#include "pce.h"
#include "gfx.h"
#include "psg.h"

#ifndef N_
#define N_(s) s
#endif

/* 44.1 kHz and 737 samples a frame: the frame rate the app is told is
 * 44100 / 737 = 59.84 (the console's 7159090 / (455 x 263) = 59.83), so
 * picture and sound never drift. */
#define PCE_SAMPLE_RATE 44100
#define PCE_SAMPLES_PER_FRAME 737

/* Street Fighter II's 2.5 MB is the biggest HuCard. */
#define PCE_ROM_MAX (4 * 1024 * 1024)

/* The backup RAM that goes to the .sav: 2 KB, as Mednafen keeps it. */
#define PCE_BRAM_SAVE 0x800

/* The picture's largest size: XBUF_WIDTH less its 16 bytes of margin. */
#define PCE_PIC_W (XBUF_WIDTH - 16)
#define PCE_PIC_H XBUF_HEIGHT

static const emu_host_t *s_host;
static bool s_loaded;
static bool s_draw;
static int s_w, s_h;                /* the frame's size, as gfx.c asked     */

static uint8_t *s_xbuf_mem;         /* pce-go's 8-bit frame, with margins   */
static uint8_t *s_xbuf;             /* its first pixel                      */
static uint16_t *s_frame;           /* the RGB565 picture for video()       */
static uint16_t s_pal[256];
static int16_t s_audio[PCE_SAMPLES_PER_FRAME * 2];

static char *s_save_path;
static uint8_t s_bram_copy[PCE_BRAM_SAVE];

static bool s_opt_compress = true;  /* wide modes to the TV's width        */

static char s_stats[96];

/* ---- the log, CRC, what pce-go calls ------------------------------------ */

#define PCE_LOG_BUDGET 40
static int s_log_count;

void pce_log(int level, const char *fmt, ...)
{
    if (!s_host || level > 2 || s_log_count > PCE_LOG_BUDGET) return;
    if (++s_log_count > PCE_LOG_BUDGET) {
        s_host->log("pce: too many messages, the rest are dropped");
        return;
    }
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    s_host->log("pce: %s", line);
}

/* zlib's CRC-32 (pce-go's flags table is made with it), half a byte at a
 * time: a 64-byte table. */
uint32_t pce_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    static const uint32_t tab[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        crc = (crc >> 4) ^ tab[crc & 15];
        crc = (crc >> 4) ^ tab[crc & 15];
    }
    return ~crc;
}

/* gfx.c asks for the frame's buffer once per batch of lines; NULL when
 * this frame is not drawn. */
uint8_t *osd_gfx_framebuffer(int width, int height)
{
    if (width > 0 && height > 0) {
        s_w = width;
        s_h = height;
    }
    return s_draw ? s_xbuf : NULL;
}

static char *pc_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- options ----------------------------------------------------------- */

static const char *const s_wide_values[] = {
    N_("Proporción de TV"), N_("Píxeles enteros"), NULL,
};

static const emu_option_t s_options[] = {
    { "wide", N_("Modos anchos"), s_wide_values, 0 },
};

/* ---- the backup RAM ---------------------------------------------------- */

static void pc_bram_open(const char *path)
{
    if (!path) return;
    s_save_path = pc_strdup(path);
    if (!s_save_path) {
        s_host->log("pce: no memory for the backup RAM's file");
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t tmp[PCE_BRAM_SAVE];
        size_t got = fread(tmp, 1, sizeof(tmp), f);
        fclose(f);
        if (got == sizeof(tmp)) {
            memcpy(PCE.BRAM, tmp, sizeof(tmp));
            s_host->log("pce: backup RAM read");
        } else {
            s_host->log("pce: %s is not a 2 KB backup RAM, left alone", path);
            free(s_save_path);
            s_save_path = NULL;
        }
    }
    memcpy(s_bram_copy, PCE.BRAM, PCE_BRAM_SAVE);
}

static bool pc_sram_flush(void)
{
    if (!s_loaded || !s_save_path || !PCE.BRAM) return true;
    if (memcmp(PCE.BRAM, s_bram_copy, PCE_BRAM_SAVE) == 0) return true;
    FILE *f = fopen(s_save_path, "wb");
    if (!f) {
        s_host->log("pce: cannot write %s", s_save_path);
        return false;
    }
    bool ok = fwrite(PCE.BRAM, 1, PCE_BRAM_SAVE, f) == PCE_BRAM_SAVE;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        memcpy(s_bram_copy, PCE.BRAM, PCE_BRAM_SAVE);
    } else {
        s_host->log("pce: writing %s failed", s_save_path);
    }
    return ok;
}

/* ---- load and unload --------------------------------------------------- */

static void pc_free_all(void)
{
    /* pce_term() frees the ROM too, which LoadCard() took over */
    ShutdownPCE();
    s_loaded = false;
    free(s_xbuf_mem);
    free(s_frame);
    free(s_save_path);
    s_xbuf_mem = NULL;
    s_xbuf = NULL;
    s_frame = NULL;
    s_save_path = NULL;
    s_w = s_h = 0;
    memset(&PCE, 0, sizeof(PCE));
    memset(PageR, 0, sizeof(PageR));
    memset(PageW, 0, sizeof(PageW));
}

static void pc_unload(void)
{
    pc_free_all();
    s_host = NULL;
}

/* The file, into a buffer padded to a power of two of 8 KB banks with the
 * image repeated: pce-go maps banks by a mask of that size, and a 384 KB or
 * 2.5 MB card would otherwise map banks past the end of the image. */
static uint8_t *pc_read_rom(const char *path, size_t *size, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return NULL;
    }
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n < 0x2000) {
        fclose(f);
        snprintf(err, errlen, "No es un juego de PC Engine");
        return NULL;
    }
    if (n > PCE_ROM_MAX) {
        fclose(f);
        snprintf(err, errlen, "El archivo es demasiado grande");
        return NULL;
    }
    size_t header = (size_t)n & 0x1FFF;     /* a copier's header, 512 bytes */
    size_t rom = (size_t)n - header;
    size_t banks = 1;
    while (banks * 0x2000 < rom) banks <<= 1;
    size_t cap = header + banks * 0x2000;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        snprintf(err, errlen, "No hay memoria");
        return NULL;
    }
    bool ok = fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(buf);
        snprintf(err, errlen, "No se pudo leer el archivo");
        return NULL;
    }
    for (size_t i = rom; i < banks * 0x2000; i++) buf[header + i] = buf[header + i % rom];
    *size = (size_t)n;
    return buf;
}

static bool pc_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    pc_free_all();
    s_host = host;
    s_log_count = 0;

    size_t size = 0;
    uint8_t *rom = pc_read_rom(req->rom, &size, err, errlen);
    if (!rom) return false;

    /* the frame: a line of margin above and below, and the 16 bytes gfx.c
     * may write left of a line's first pixel */
    s_xbuf_mem = calloc((size_t)XBUF_WIDTH * (XBUF_HEIGHT + 2) + 64, 1);
    s_frame = malloc((size_t)PCE_PIC_W * PCE_PIC_H * sizeof(uint16_t));
    if (!s_xbuf_mem || !s_frame) {
        free(rom);
        pc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    s_xbuf = s_xbuf_mem + XBUF_WIDTH + 16;

    if (InitPCE(PCE_SAMPLE_RATE, true) != 0) {
        free(rom);
        pc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    if (LoadCard(rom, size) != 0) {
        /* LoadCard() owns the image once it has looked at it */
        if (PCE.ROM != rom) free(rom);
        pc_free_all();
        snprintf(err, errlen, "No se pudo cargar el juego");
        return false;
    }
    /* LoadCard()'s reset is a soft one, which leaves RAM and VRAM as
     * malloc() gave them: power on instead, so every load starts alike */
    ResetPCE(true);

    uint16_t *pal = PalettePCE(16);
    if (!pal) {
        pc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    /* RGB565 in the CPU's order, as LVGL takes it (Retro-Go swapped the
     * bytes for its SPI panels) */
    memcpy(s_pal, pal, sizeof(s_pal));
    free(pal);

    s_loaded = true;
    pc_bram_open(req->save);

    host->log("pce: %u KB, crc %08X%s", (unsigned)(PCE.ROM_SIZE * 8), (unsigned)PCE.ROM_CRC,
              PCE.ROM_SIZE >= 0xC0 ? ", SF2 mapper" : "");
    return true;
}

/* ---- running ----------------------------------------------------------- */

static uint8_t pc_pad(uint32_t b)
{
    uint8_t p = 0;
    if (b & EMU_BTN_A)      p |= JOY_A;        /* I  */
    if (b & EMU_BTN_B)      p |= JOY_B;        /* II */
    if (b & EMU_BTN_SELECT) p |= JOY_SELECT;
    if (b & EMU_BTN_START)  p |= JOY_RUN;
    if (b & EMU_BTN_UP)     p |= JOY_UP;
    if (b & EMU_BTN_DOWN)   p |= JOY_DOWN;
    if (b & EMU_BTN_LEFT)   p |= JOY_LEFT;
    if (b & EMU_BTN_RIGHT)  p |= JOY_RIGHT;
    return p;
}

static inline uint16_t pc_mix(uint16_t a, uint16_t b, int wa)
{
    /* wa/4 of a, the rest of b, per channel */
    int wb = 4 - wa;
    int r = (((a >> 11) & 31) * wa + ((b >> 11) & 31) * wb + 2) >> 2;
    int g = (((a >> 5) & 63) * wa + ((b >> 5) & 63) * wb + 2) >> 2;
    int bl = ((a & 31) * wa + (b & 31) * wb + 2) >> 2;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void pc_blit(void)
{
    int w = s_w, h = s_h;
    /* a frame before the VDC drew a line (display off): the size it has */
    if (w <= 0 || h <= 0) {
        w = PCE.VDC.screen_width ? (int)PCE.VDC.screen_width : 256;
        h = PCE.VDC.screen_height ? (int)PCE.VDC.screen_height : 240;
    }
    if (w > PCE_PIC_W) w = PCE_PIC_W;
    if (h > PCE_PIC_H) h = PCE_PIC_H;
    const uint16_t *pal = s_pal;

    /* The 7.16 MHz modes (320 to 352 dots) put a third more dots in the
     * same width of the TV than the 5.37 MHz one the table's pixel aspect
     * is for: four of them become three. */
    if (s_opt_compress && w > 256 && (w & 3) == 0) {
        int ow = w * 3 / 4;
        uint16_t *dst = s_frame;
        for (int y = 0; y < h; y++) {
            const uint8_t *src = s_xbuf + y * XBUF_WIDTH;
            for (int x = 0, o = 0; x < w; x += 4, o += 3) {
                uint16_t p0 = pal[src[x]], p1 = pal[src[x + 1]];
                uint16_t p2 = pal[src[x + 2]], p3 = pal[src[x + 3]];
                dst[o] = pc_mix(p0, p1, 3);
                dst[o + 1] = pc_mix(p1, p2, 2);
                dst[o + 2] = pc_mix(p3, p2, 3);
            }
            dst += ow;
        }
        s_host->video(s_frame, ow, h, ow * (int)sizeof(uint16_t));
        return;
    }

    uint16_t *dst = s_frame;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = s_xbuf + y * XBUF_WIDTH;
        for (int x = 0; x < w; x += 4) {
            dst[x + 0] = pal[src[x + 0]];
            dst[x + 1] = pal[src[x + 1]];
            dst[x + 2] = pal[src[x + 2]];
            dst[x + 3] = pal[src[x + 3]];
        }
        dst += w;
    }
    s_host->video(s_frame, w, h, w * (int)sizeof(uint16_t));
}

static void pc_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;
    PCE.Joypad.regs[0] = pads ? pc_pad(pads[0]) : 0;
    PCE.Joypad.regs[1] = pads ? pc_pad(pads[1]) : 0;
    s_draw = draw;
    RunFramePCE(s_audio, PCE_SAMPLES_PER_FRAME);
    if (draw) pc_blit();
    /* pce-go's mix peaks at 3600 a channel, 21600 for all six: twice
     * that, held to 16 bits, sits with the other consoles' level */
    for (int i = 0; i < PCE_SAMPLES_PER_FRAME * 2; i++) {
        int v = s_audio[i] * 2;
        s_audio[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    s_host->audio(s_audio, PCE_SAMPLES_PER_FRAME);
}

/* A reset is the power switch: the backup RAM keeps what it has. */
static void pc_reset(void)
{
    if (!s_loaded) return;
    ResetPCE(true);
}

static float pc_fps(void)
{
    return (float)PCE_SAMPLE_RATE / (float)PCE_SAMPLES_PER_FRAME;
}

static int pc_sample_rate(void)
{
    return PCE_SAMPLE_RATE;
}

/* ---- save states ------------------------------------------------------- */

static bool pc_state_save(const char *path)
{
    if (!s_loaded) return false;
    size_t n = strlen(path) + 5;
    char *tmp = malloc(n);
    if (!tmp) return false;
    snprintf(tmp, n, "%s.tmp", path);
    bool ok = SaveState(tmp) == 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        s_host->log("pce: cannot write the state %s", path);
    }
    free(tmp);
    return ok;
}

static bool pc_state_load(const char *path)
{
    if (!s_loaded) return false;
    /* LoadState() checks the whole file before it applies any of it */
    if (LoadState(path) != 0) {
        s_host->log("pce: %s is not a state of this build", path);
        return false;
    }
    return true;
}

/* ---- options and stats ------------------------------------------------- */

static void pc_set_option(const char *key, int value)
{
    if (strcmp(key, "wide") == 0) s_opt_compress = (value == 0);
}

static const char *pc_stats(void)
{
    if (!s_loaded) return NULL;
    snprintf(s_stats, sizeof(s_stats), "pce: %dx%d, %s speed, %u KB",
             s_w, s_h, PCE.Timer.cycles_per_line > 200 ? "high" : "low",
             (unsigned)(PCE.ROM_SIZE * 8));
    return s_stats;
}

/* ---- the table --------------------------------------------------------- */

/* The pad's II (left) and I (right), and RUN. */
static const char *const s_labels[EMU_BTN_COUNT] = {
    [4] = "I", [5] = "II", [11] = "RUN",
};

const emu_core_t emu_core_pce = {
    .id = "pce",
    .name = "PC Engine",
    .short_name = "PCE",
    .exts = ".pce",
    .max_w = PCE_PIC_W,
    .max_h = PCE_PIC_H,
    .pixel_aspect = 8.0f / 7.0f,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_SELECT | EMU_BTN_START,
    .face = EMU_FACE_ROW2,
    .labels = s_labels,
    .options = s_options,
    .n_options = 1,
    .load = pc_load,
    .unload = pc_unload,
    .reset = pc_reset,
    .run = pc_run,
    .fps = pc_fps,
    .sample_rate = pc_sample_rate,
    .state_save = pc_state_save,
    .state_load = pc_state_load,
    .sram_flush = pc_sram_flush,
    .set_option = pc_set_option,
    .stats = pc_stats,
};
