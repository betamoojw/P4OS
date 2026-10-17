/*
 * P4OS - Emulador, Mega Drive core: Gwenesis behind the emu_core_t table.
 *
 * Gwenesis (gwenesis/, from RetroESP32-P4, which took it from bzhxx's
 * Gwenesis by way of Retro-Go) is a whole console held in globals: a
 * Musashi 68000, Marat Fayzullin's Z80, the VDP drawn a line at a time,
 * the YM2612 and the SN76489. It brings no frame loop of its own; this file
 * is that loop (the 262 or 313 lines of a frame, the line and frame
 * interrupts), the memory every part of it works in, and the rest of what
 * the app asks of a console: pads, battery RAM, states, sound at a rate
 * the speaker takes.
 *
 * Memory. Gwenesis keeps its RAMs and tables behind pointers (RetroESP32-P4
 * did that to put the hot ones in internal RAM). Here they are carved out
 * of two blocks: one of what the 68000, the VDP and the sound chips touch
 * on every line (s_hot_pieces, 178 KB on the board: the 68000's and the
 * Z80's RAM, the VRAM, the palettes, the chips' state and tables) and one
 * of the rest (the YM2612's LFO table, the sound buffers, 132 KB). Both
 * are in PSRAM for now; the front of s_hot_pieces is what is worth
 * internal RAM, if the app ever offers it (README.md, "Internal RAM").
 *
 * Sound. The YM2612 makes a sample every 144 of its clocks (53267 Hz on an
 * NTSC console, 52781 on a PAL one) and the PSG is sampled at the same
 * instants. Both are brought up to the CPU's time on every write to them
 * (GWENESIS_AUDIO_ACCURATE), so the DAC's sampled drums and voices come out
 * at the moment they were written. The mix is turned into 48000 Hz, the
 * speaker's own rate, by linear interpolation; fps() is worked out from the
 * same step, so picture and sound never drift.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"

#include "m68k.h"
#include "z80inst.h"
#include "ym2612.h"
#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "gwenesis_savestate.h"
#include "gwenesis_sn76489.h"

#ifndef N_
#define N_(s) s
#endif

#define MD_OUT_RATE     48000
#define MD_ROM_MAX      0x800000    /* the cartridge's whole 8 MB window */
#define MD_SRAM_SIZE    0x10000
#define MD_FRAME_W      320
#define MD_FRAME_H      240
#define MD_AUDIO_MAX    1100        /* chip samples a frame: 1062 at most (PAL) */
#define MD_OUT_MAX      1000        /* 48 kHz samples a frame: 966 at most (PAL) */
#define MD_STATE_MAX    (1024 * 1024)
#define MD_STATE_VERSION 1

/* ---- what Gwenesis expects the host to define ----------------------------- */

unsigned char *M68K_RAM;
unsigned char *ZRAM;
int16 *gwenesis_sn76489_buffer;
int sn76489_index;
int sn76489_clock;
int16_t *gwenesis_ym2612_buffer;
int ym2612_index;
int ym2612_clock;
int32_t *lfo_pm_table;
signed int *tl_tab;
int scan_line;
int system_clock;
unsigned char gw_region_reg = 0xA0;
int gw_psg_clock = 3579545;
int gw_psg_rate = 53267;

/* Gwenesis's own, set up from here */
extern m68ki_cpu_core *m68k;
extern unsigned char *VRAM;
extern unsigned short *CRAM, *CRAM565, *VSRAM, *fifo;
extern unsigned char *SAT_CACHE, *gwenesis_vdp_regs;
extern unsigned short gwenesis_vdp_status;
extern int screen_width, screen_height, hint_pending, mode_pal;
extern unsigned char *ROM_DATA;
extern unsigned int gwenesis_rom_size;
extern unsigned int gw_rom_fast;
extern int zclk;

void gwenesis_io_get_buttons(void) { }

/* ---- this core's state --------------------------------------------------- */

static const emu_host_t *s_host;
static bool s_loaded;

static uint8_t *s_rom;
static uint8_t *s_hot;              /* the two blocks (see the top)        */
static uint8_t *s_cold;
static uint16_t *s_frame;           /* 320 x 240 RGB565                    */
static int16_t *s_out;              /* 48 kHz stereo, a frame of it        */

static bool s_pal;                  /* the console: 313 lines, 50 Hz       */
static int s_region_detected;       /* the header's, Gens's bits           */
static char s_title[49];

/* the battery RAM */
static char *s_save_path;
static uint8_t *s_sram_copy;        /* as last read or written             */
static unsigned s_sram_bytes;       /* how much of it goes to the .sav     */

/* sound */
static int s_quality;               /* 0 full, 1 fast, 2 no FM             */
static uint32_t s_rs_step;          /* chip samples per output sample, 16.16 */
static uint32_t s_rs_pos;
static int s_rs_last;

/* options, as the indices of their values */
static int s_opt_pad6;
static int s_opt_region;            /* 0 auto, 1 Japan, 2 USA, 3 Europe    */
static int s_opt_sound;
static uint32_t s_frames;           /* run() calls since load()            */

static char s_stats[96];

/* ---- options -------------------------------------------------------------- */

static const char *const s_pad_values[] = { N_("3 botones"), N_("6 botones"), NULL };
static const char *const s_region_values[] = {
    N_("Automática"), N_("Japón"), N_("América"), N_("Europa"), NULL,
};
static const char *const s_sound_values[] = {
    N_("Completo"), N_("Rápido"), N_("Sin FM"), NULL,
};

static const emu_option_t s_options[] = {
    { "pad",    N_("Mando"),  s_pad_values,    0 },
    { "region", N_("Región"), s_region_values, 0 },
    { "sound",  N_("Sonido"), s_sound_values,  0 },
};
#define MD_N_OPTIONS ((int)(sizeof(s_options) / sizeof(s_options[0])))

/* What the console calls its buttons: SELECT is the six-button pad's MODE. */
static const char *const s_labels[EMU_BTN_COUNT] = {
    [10] = "MODE",
};

/* ---- the log ------------------------------------------------------------- */

void md_core_printf(const char *fmt, ...)
{
    if (!s_host || !s_host->log) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) s_host->log("md: %s", line);
}

/* ---- memory -------------------------------------------------------------- */

/* One piece of a block: where its pointer is and how big it is. */
typedef struct {
    void **ptr;
    size_t size;
} md_piece_t;

#define PIECE(p, n) { (void **)&(p), (n) }

/* What the 68000, the Z80, the VDP and the sound chips touch all the time. */
static md_piece_t s_hot_pieces[] = {
    PIECE(m68k, sizeof(m68ki_cpu_core)),
    PIECE(gwenesis_vdp_regs, REG_SIZE),
    PIECE(CRAM565, CRAM_MAX_SIZE * 4 * sizeof(uint16_t)),
    PIECE(CRAM, CRAM_MAX_SIZE * sizeof(uint16_t)),
    PIECE(VSRAM, VSRAM_MAX_SIZE * sizeof(uint16_t)),
    PIECE(fifo, FIFO_SIZE * sizeof(uint16_t)),
    PIECE(SAT_CACHE, SAT_CACHE_MAX_SIZE),
    PIECE(render_buffer, SCREEN_WIDTH + PIX_OVERFLOW * 2),
    PIECE(sprite_buffer, SCREEN_WIDTH + PIX_OVERFLOW * 2),
    PIECE(ym2612, sizeof(YM2612)),
    PIECE(OPNREGS, 512),
    PIECE(sin_tab, SIN_LEN * sizeof(unsigned int)),
    PIECE(ZRAM, MAX_Z80_RAM_SIZE),
    PIECE(tl_tab, 13 * 2 * 256 * sizeof(signed int)),
    PIECE(M68K_RAM, MAX_RAM_SIZE),
    PIECE(VRAM, VRAM_MAX_SIZE),
};

static md_piece_t s_cold_pieces[] = {
    PIECE(lfo_pm_table, 128 * 8 * 32 * sizeof(int32_t)),
    PIECE(gwenesis_ym2612_buffer, MD_AUDIO_MAX * sizeof(int16_t)),
    PIECE(gwenesis_sn76489_buffer, MD_AUDIO_MAX * sizeof(int16_t)),
};

static size_t md_block_size(const md_piece_t *p, int n)
{
    size_t total = 0;
    for (int i = 0; i < n; i++) total += (p[i].size + 15) & ~(size_t)15;
    return total;
}

static uint8_t *md_block_alloc(md_piece_t *p, int n)
{
    uint8_t *b = calloc(1, md_block_size(p, n));
    if (!b) return NULL;
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        *p[i].ptr = b + off;
        off += (p[i].size + 15) & ~(size_t)15;
    }
    return b;
}

static void md_block_forget(md_piece_t *p, int n)
{
    for (int i = 0; i < n; i++) *p[i].ptr = NULL;
}

#define N_HOT  ((int)(sizeof(s_hot_pieces) / sizeof(s_hot_pieces[0])))
#define N_COLD ((int)(sizeof(s_cold_pieces) / sizeof(s_cold_pieces[0])))

/* ---- the battery RAM ----------------------------------------------------- */

static uint32_t md_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* From the header (the "RA" block at $1B0, raw big-endian bytes), as
 * Genesis Plus GX reads it; a cartridge without one but of 2 MB or less
 * gets 64 KB at $200000 all the same, which is where games put it. */
static void md_sram_detect(const uint8_t *rom, size_t size, bool *has, uint32_t *start, uint32_t *end)
{
    *has = false;
    if (size >= 0x1BC && rom[0x1B0] == 'R' && rom[0x1B1] == 'A') {
        uint32_t s = md_be32(rom + 0x1B4), e = md_be32(rom + 0x1B8);
        if (s > e || e - s >= 0x10000 || s < 0x200000 || e >= 0x800000) {
            s = 0x200000;
            e = 0x20FFFF;
        }
        *start = s & 0xFFFFFE;
        *end = e | 1;
        *has = true;
    } else if (size <= 0x200000) {
        *start = 0x200000;
        *end = 0x20FFFF;
        *has = true;
    }
}

static bool md_sram_open(const char *path)
{
    s_sram_bytes = (gw_sram_end & 0xFFFF) + 1;
    s_sram_copy = malloc(MD_SRAM_SIZE);
    s_save_path = path ? malloc(strlen(path) + 1) : NULL;
    if (!s_sram_copy || (path && !s_save_path)) return false;
    if (path) strcpy(s_save_path, path);

    memset(gw_sram, 0xFF, MD_SRAM_SIZE);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) {
        size_t got = fread(gw_sram, 1, MD_SRAM_SIZE, f);
        fclose(f);
        s_host->log("md: battery save read (%u bytes)", (unsigned)got);
    }
    memcpy(s_sram_copy, gw_sram, MD_SRAM_SIZE);
    gw_sram_dirty = 0;
    return true;
}

static bool md_sram_flush(void)
{
    if (!s_loaded || !gw_sram || !s_save_path || !gw_sram_dirty) return true;
    gw_sram_dirty = 0;
    if (memcmp(gw_sram, s_sram_copy, MD_SRAM_SIZE) == 0) return true;

    size_t plen = strlen(s_save_path) + 5;
    char *tmp = malloc(plen);
    if (!tmp) return false;
    snprintf(tmp, plen, "%s.tmp", s_save_path);
    bool ok = false;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        ok = fwrite(gw_sram, 1, s_sram_bytes, f) == s_sram_bytes;
        if (fclose(f) != 0) ok = false;
    }
    if (ok) {
        remove(s_save_path);                 /* FAT does not rename over a file */
        ok = rename(tmp, s_save_path) == 0;
    }
    if (!ok) {
        remove(tmp);
        gw_sram_dirty = 1;                   /* try again next time */
        s_host->log("md: writing %s failed", s_save_path);
    } else {
        memcpy(s_sram_copy, gw_sram, MD_SRAM_SIZE);
    }
    free(tmp);
    return ok;
}

/* ---- region and sound timing --------------------------------------------- */

static bool md_region_pal(void)
{
    switch (s_opt_region) {
    case 1: return false;
    case 2: return false;
    case 3: return true;
    default:
        /* USA first, then Europe, then Japan, as Gwenesis chose */
        if (s_region_detected & 4) return false;
        if (s_region_detected & 8) return true;
        return false;
    }
}

static unsigned char md_region_reg(void)
{
    int r = s_opt_region;
    if (r == 0) {
        if (s_region_detected & 4) r = 2;
        else if (s_region_detected & 8) r = 3;
        else if (s_region_detected & 1) r = 1;
        else r = 2;
    }
    /* bit 7 overseas, bit 6 PAL, bit 5 no expansion unit (no Mega-CD) */
    switch (r) {
    case 1:  return 0x20;
    case 3:  return 0xE0;
    default: return 0xA0;
    }
}

static int md_lines(void) { return s_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC; }
static double md_master(void) { return s_pal ? MCLOCK_PAL : MCLOCK_NTSC; }

/* The resampler's step: chip samples (at master / 1008, or / 2016 in the
 * fast mode) per 48 kHz sample, in 16.16. */
static void md_audio_timing(void)
{
    double in_rate = md_master() / (s_quality == 1 ? 2.0 * AUDIO_FREQ_DIVISOR : AUDIO_FREQ_DIVISOR);
    s_rs_step = (uint32_t)(in_rate * 65536.0 / MD_OUT_RATE + 0.5);
}

/* The YM2612 and the PSG at the chip's rate, or both at half of it. */
static void md_apply_quality(void)
{
    int half = s_quality == 1;
    ym2612_set_half_rate(half);
    gwenesis_SN76489_SetRate(gw_psg_clock, half ? gw_psg_rate / 2 : gw_psg_rate,
                             AUDIO_FREQ_DIVISOR * (half ? 2 : 1));
    md_audio_timing();
}

static void md_apply_region(void)
{
    s_pal = md_region_pal();
    gw_region_reg = md_region_reg();
    gw_psg_clock = s_pal ? 3546895 : 3579545;
    gw_psg_rate = (int)(md_master() / AUDIO_FREQ_DIVISOR + 0.5);
    md_audio_timing();
}

/* ---- the console's globals ----------------------------------------------- */

/* Everything a frame carries over to the next, back to a power-on. */
static void md_clear_frame_state(void)
{
    system_clock = 0;
    scan_line = 0;
    zclk = 0;
    ym2612_clock = 0;
    ym2612_index = 0;
    sn76489_clock = 0;
    sn76489_index = 0;
    s_rs_pos = 0;
    s_rs_last = 0;
    gwenesis_io_end_frame();
}

/* The console's globals that power_on() and reset_emulation() leave as
 * the last game had them, set as a fresh start of the program has them. */
static void md_reset_io(void)
{
    gwenesis_io_reset();
    extern int tmss_state, tmss_count, Z80_BANK;
    tmss_state = 0;
    tmss_count = 0;
    Z80_BANK = 0;
}

static void md_power_cycle(void)
{
    extern int sprite_overflow, dma_fill_pending;
    extern bool sprite_collision;
    sprite_overflow = 0;
    sprite_collision = false;
    dma_fill_pending = 0;
    md_reset_io();
    power_on();
    reset_emulation();
    set_region();
    gwenesis_vdp_status = (gwenesis_vdp_status & ~STATUS_PAL) | (s_pal ? STATUS_PAL : 0);
    gw_sram_ctrl = 0;
    gwenesis_bus_sram_update();
    md_clear_frame_state();
    md_apply_quality();
}

/* ---- load and unload ----------------------------------------------------- */

static void md_free_all(void)
{
    s_loaded = false;
    free(s_rom);
    free(s_hot);
    free(s_cold);
    free(s_frame);
    free(s_out);
    free(gw_sram);
    free(s_sram_copy);
    free(s_save_path);
    s_rom = s_hot = s_cold = NULL;
    s_frame = NULL;
    s_out = NULL;
    gw_sram = NULL;
    s_sram_copy = NULL;
    s_save_path = NULL;
    md_block_forget(s_hot_pieces, N_HOT);
    md_block_forget(s_cold_pieces, N_COLD);
    ROM_DATA = NULL;
    gwenesis_rom_size = 0;
    gw_rom_fast = 0;
    gw_sram_start = gw_sram_end = 0;
    gw_sram_ctrl = 0;
    gw_sram_dirty = 0;
    s_frames = 0;
}

static void md_unload(void)
{
    md_free_all();
    s_host = NULL;
}

static bool md_read_rom(const char *path, char *err, size_t errlen, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return false;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0x200 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, errlen, "No es un juego de Mega Drive");
        return false;
    }
    if (size > MD_ROM_MAX + 512) {
        fclose(f);
        snprintf(err, errlen, "El archivo es demasiado grande");
        return false;
    }
    s_rom = malloc((size_t)size);
    if (!s_rom) {
        fclose(f);
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    size_t got = fread(s_rom, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        snprintf(err, errlen, "No se pudo leer el juego");
        return false;
    }
    *size_out = got;
    return true;
}

/* An SMD dump (a 512-byte header, then the ROM in 16 KB blocks of odd
 * bytes and even bytes), as load_cartridge() recognises it. */
static bool md_is_smd(const uint8_t *r, size_t size)
{
    return size > 512 && (size & 0x3FFF) == 512 && r[1] == 0x03 && r[8] == 0xAA && r[9] == 0xBB;
}

static void md_smd_to_bin(uint8_t *r, size_t *size)
{
    size_t n = *size - 512;
    memmove(r, r + 512, n);
    uint8_t *tmp = malloc(0x4000);
    if (!tmp) return;
    for (size_t i = 0; i + 0x4000 <= n; i += 0x4000) {
        memcpy(tmp, r + i, 0x4000);
        for (size_t j = 0; j < 0x2000; ++j) {
            r[i + j * 2 + 0] = tmp[0x2000 + j];
            r[i + j * 2 + 1] = tmp[j];
        }
    }
    free(tmp);
    *size = n;
}

static bool md_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    md_free_all();
    s_host = host;

    size_t size = 0;
    if (!md_read_rom(req->rom, err, errlen, &size)) {
        md_free_all();
        return false;
    }
    /* SMD dumps are put in order here: load_cartridge() would do it too,
     * but after the header had been looked at. */
    if (md_is_smd(s_rom, size)) md_smd_to_bin(s_rom, &size);
    if (size > MD_ROM_MAX) {
        md_free_all();
        snprintf(err, errlen, "El archivo es demasiado grande");
        return false;
    }
    /* 32X cartridges say so in their header; they need the 32X's two
     * SH-2s, which this core does not have. */
    if (memcmp(s_rom + 0x100, "SEGA 32X", 8) == 0) {
        md_free_all();
        snprintf(err, errlen, "Los juegos de 32X no se pueden jugar");
        return false;
    }
    if (memcmp(s_rom + 0x100, "SEGA", 4) != 0 && memcmp(s_rom + 0x101, "SEGA", 4) != 0)
        host->log("md: no SEGA in the header, trying anyway");

    s_hot = md_block_alloc(s_hot_pieces, N_HOT);
    s_cold = md_block_alloc(s_cold_pieces, N_COLD);
    s_frame = calloc(MD_FRAME_W * MD_FRAME_H, sizeof(uint16_t));
    s_out = malloc(MD_OUT_MAX * 2 * sizeof(int16_t));
    bool sram;
    uint32_t sram_start = 0, sram_end = 0;
    md_sram_detect(s_rom, size, &sram, &sram_start, &sram_end);
    if (sram) gw_sram = malloc(MD_SRAM_SIZE);
    if (!s_hot || !s_cold || !s_frame || !s_out || (sram && !gw_sram)) {
        md_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    if (sram) {
        gw_sram_start = sram_start;
        gw_sram_end = sram_end;
        if (!md_sram_open(req->save)) {
            md_free_all();
            snprintf(err, errlen, "No hay memoria");
            return false;
        }
    }

    for (int i = 0; i < 48; i++) {
        char c = (char)s_rom[0x150 + i];
        s_title[i] = (c >= 32 && c < 127) ? c : ' ';
    }
    s_title[48] = 0;
    for (int i = 47; i >= 0 && s_title[i] == ' '; i--) s_title[i] = 0;

    /* load_cartridge() byte-swaps the ROM into the CPU's order, then reads
     * the region from it (gw_region_detect). */
    load_cartridge(s_rom, size);
    s_region_detected = gw_region_detect();
    s_quality = s_opt_sound;
    md_apply_region();
    md_power_cycle();
    s_loaded = true;

    host->log("md: \"%s\" %uK, %s%s, sram %s", s_title, (unsigned)(size / 1024),
              s_pal ? "PAL" : "NTSC",
              (s_region_detected & 1) && !(s_region_detected & 12) ? " (Japan)" : "",
              gw_sram ? "yes" : "no");
    return true;
}

/* A reset is the console's power switch; the battery RAM survives it. */
static void md_reset(void)
{
    if (!s_loaded) return;
    md_power_cycle();
}

/* ---- running ------------------------------------------------------------- */

static void md_set_pads(const uint32_t *pads)
{
    for (int i = 0; i < 2; i++) {
        uint32_t b = pads ? pads[i] : 0;
        unsigned char s = 0xFF;
        if (b & EMU_BTN_UP)    s &= ~(1 << PAD_UP);
        if (b & EMU_BTN_DOWN)  s &= ~(1 << PAD_DOWN);
        if (b & EMU_BTN_LEFT)  s &= ~(1 << PAD_LEFT);
        if (b & EMU_BTN_RIGHT) s &= ~(1 << PAD_RIGHT);
        if (b & EMU_BTN_A)     s &= ~(1 << PAD_A);
        if (b & EMU_BTN_B)     s &= ~(1 << PAD_B);
        if (b & EMU_BTN_C)     s &= ~(1 << PAD_C);
        if (b & EMU_BTN_START) s &= ~(1 << PAD_S);
        button_state[i] = s;

        unsigned char s6 = 0xFF;
        if (s_opt_pad6) {
            if (b & EMU_BTN_Z)      s6 &= ~1;
            if (b & EMU_BTN_Y)      s6 &= ~2;
            if (b & EMU_BTN_X)      s6 &= ~4;
            if (b & EMU_BTN_SELECT) s6 &= ~8;
        }
        button_state6[i] = s6;
        gwenesis_io_pad6[i] = (unsigned char)s_opt_pad6;
    }
}

/* The frame's chip samples (YM2612 + PSG, mono) to 48 kHz stereo, by
 * linear interpolation, carrying the position and the last sample over to
 * the next frame. */
static int md_resample(int n)
{
    const int16_t *ym = gwenesis_ym2612_buffer;
    const int16 *sn = gwenesis_sn76489_buffer;
    bool fm = s_quality != 2;
    int out = 0;
    uint32_t pos = s_rs_pos;
    const uint32_t step = s_rs_step;
    const uint32_t end = (uint32_t)n << 16;
    int prev = s_rs_last;

    /* pos's integer part k: between chip sample k - 1 (prev for k = 0)
     * and chip sample k */
    while (pos < end && out < MD_OUT_MAX) {
        int k = (int)(pos >> 16);
        int a = k ? (fm ? ym[k - 1] : 0) + sn[k - 1] : prev;
        int b = (fm ? ym[k] : 0) + sn[k];
        int v = a + (((b - a) * (int)(pos & 0xFFFF)) >> 16);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        s_out[2 * out] = (int16_t)v;
        s_out[2 * out + 1] = (int16_t)v;
        out++;
        pos += step;
    }
    if (n > 0) {
        s_rs_last = (fm ? ym[n - 1] : 0) + sn[n - 1];
        pos -= end;
    }
    s_rs_pos = pos;
    return out;
}

static void md_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;
    md_set_pads(pads);

    const int lines = md_lines();
    int hint_counter = gwenesis_vdp_regs[10];

    screen_width = REG12_MODE_H40 ? 320 : 256;
    screen_height = REG1_PAL ? 240 : 224;   /* the V30 bit: 240 lines */
    gwenesis_vdp_set_buffer(s_frame, MD_FRAME_W);
    gwenesis_vdp_render_config();

    /* "Sin FM" keeps the YM2612's registers but never makes its samples */
    if (s_quality == 2) ym2612_clock = 0x1000000;
    else if (ym2612_clock >= 0x800000) ym2612_clock = 0;

    system_clock = 0;
    scan_line = 0;
    while (scan_line < lines) {
        m68k_run(system_clock + VDP_CYCLES_PER_LINE);
        z80_run(system_clock + VDP_CYCLES_PER_LINE);

        if (draw && scan_line < screen_height)
            gwenesis_vdp_render_line(scan_line);

        /* the line counter is reloaded on these lines */
        if (scan_line == 0 || scan_line > screen_height)
            hint_counter = REG10_LINE_COUNTER;

        if (--hint_counter < 0) {
            if (REG0_LINE_INTERRUPT != 0 && scan_line <= screen_height) {
                hint_pending = 1;
                if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                    m68k_update_irq(4);
            }
            hint_counter = REG10_LINE_COUNTER;
        }

        scan_line++;

        /* vertical blank from the end of the last line shown */
        if (scan_line == screen_height) {
            if (REG1_VBLANK_INTERRUPT != 0) {
                gwenesis_vdp_status |= STATUS_VIRQPENDING;
                m68k_set_irq(6);
            }
            z80_irq_line(1);
        }
        if (scan_line == screen_height + 1)
            z80_irq_line(0);

        system_clock += VDP_CYCLES_PER_LINE;
    }

    /* the sound chips up to the end of the frame */
    gwenesis_SN76489_run(system_clock);
    if (s_quality != 2) ym2612_run(system_clock);
    int n = sn76489_index;
    if (s_quality != 2 && ym2612_index < n) n = ym2612_index;

    /* every clock counts from the start of the next frame */
    m68k->cycles -= system_clock;
    zclk -= system_clock;
    sn76489_clock -= system_clock;
    if (s_quality != 2) ym2612_clock -= system_clock;
    sn76489_index = 0;
    ym2612_index = 0;
    gwenesis_io_end_frame();
    s_frames++;

    if (draw) s_host->video(s_frame, screen_width, screen_height, MD_FRAME_W * (int)sizeof(uint16_t));

    int out = md_resample(n);
    if (out > 0) s_host->audio(s_out, out);
}

static float md_fps(void)
{
    /* What the resampler makes a frame on average is 48000 / fps: the
     * chip samples of a frame (lines x 3420 master clocks / 1008) over the
     * step. 59.92 NTSC, 49.70 PAL. */
    double chip_per_frame = (double)md_lines() * VDP_CYCLES_PER_LINE / AUDIO_FREQ_DIVISOR;
    if (s_quality == 1) chip_per_frame /= 2.0;
    double out_per_frame = chip_per_frame * 65536.0 / (double)s_rs_step;
    return (float)(MD_OUT_RATE / out_per_frame);
}

static int md_sample_rate(void)
{
    return MD_OUT_RATE;
}

/* ---- save states ----------------------------------------------------------
 *
 * Gwenesis saves itself as a list of named records (gwenesis_save_state()
 * calls saveGwenesisStateSet...() for each of its variables). The records
 * go to memory and then to the file, after a first record of our own (the
 * format's version) and before a last one (a CRC-32 of everything before
 * it). To load, the file is read whole and checked against what a save
 * from this build would hold, record by record (names and sizes), before
 * one byte of it reaches the console: a short, damaged or foreign file is
 * refused and the game goes on as it was.
 */

typedef struct {
    char key[28];
    uint32_t length;
} md_svar_t;

enum { ST_OFF, ST_WRITE, ST_LAYOUT, ST_READ };
static int s_st_mode;
static uint8_t *s_st_buf;
static size_t s_st_len, s_st_cap, s_st_pos;
static bool s_st_err;

static bool md_st_reserve(size_t more)
{
    if (s_st_len + more <= s_st_cap) return true;
    size_t cap = s_st_cap ? s_st_cap : 64 * 1024;
    while (cap < s_st_len + more) cap *= 2;
    if (cap > MD_STATE_MAX) return false;
    uint8_t *b = realloc(s_st_buf, cap);
    if (!b) return false;
    s_st_buf = b;
    s_st_cap = cap;
    return true;
}

SaveState *saveGwenesisStateOpenForRead(const char *fileName)
{
    (void)fileName;
    return (SaveState *)1;
}

SaveState *saveGwenesisStateOpenForWrite(const char *fileName)
{
    (void)fileName;
    return (SaveState *)1;
}

void saveGwenesisStateSetBuffer(SaveState *state, const char *tagName, const void *buffer, int length)
{
    (void)state;
    if (s_st_err || length < 0) {
        s_st_err = true;
        return;
    }
    md_svar_t v;
    memset(&v, 0, sizeof(v));
    strncpy(v.key, tagName, sizeof(v.key) - 1);
    v.length = (uint32_t)length;
    size_t need = sizeof(v) + (s_st_mode == ST_WRITE ? (size_t)length : 0);
    if (!md_st_reserve(need)) {
        s_st_err = true;
        return;
    }
    memcpy(s_st_buf + s_st_len, &v, sizeof(v));
    s_st_len += sizeof(v);
    if (s_st_mode == ST_WRITE) {
        memcpy(s_st_buf + s_st_len, buffer, (size_t)length);
        s_st_len += (size_t)length;
    }
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value)
{
    saveGwenesisStateSetBuffer(state, tagName, &value, sizeof(int));
}

/* Records come back in the order they were written: each read takes the
 * next one. The file was checked before (md_state_check), so a name that
 * does not match is a bug, not a bad file. */
void saveGwenesisStateGetBuffer(SaveState *state, const char *tagName, void *buffer, int length)
{
    (void)state;
    if (s_st_err || s_st_pos + sizeof(md_svar_t) > s_st_len) {
        s_st_err = true;
        return;
    }
    md_svar_t v;
    memcpy(&v, s_st_buf + s_st_pos, sizeof(v));
    if (strncmp(v.key, tagName, sizeof(v.key)) != 0 || v.length != (uint32_t)length
        || s_st_pos + sizeof(v) + v.length > s_st_len) {
        s_st_err = true;
        return;
    }
    memcpy(buffer, s_st_buf + s_st_pos + sizeof(v), (size_t)length);
    s_st_pos += sizeof(v) + v.length;
}

int saveGwenesisStateGet(SaveState *state, const char *tagName)
{
    int value = 0;
    saveGwenesisStateGetBuffer(state, tagName, &value, sizeof(int));
    return value;
}

static uint32_t md_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* Everything a state holds, in its order: the same calls write it, lay it
 * out for the check, and read it back. */
static void md_state_all(bool load)
{
    if (load) {
        int version = saveGwenesisStateGet(NULL, "p4os_md");
        if (version != MD_STATE_VERSION) s_st_err = true;
        if (s_st_err) return;
        gwenesis_load_state();
        /* what this file carries from one frame to the next: the sound
         * chips' clocks past the frame's end and the resampler's place, so
         * that a loaded state plays on exactly as the game did */
        int32_t carry[4];
        saveGwenesisStateGetBuffer(NULL, "md_carry", carry, sizeof(carry));
        ym2612_clock = carry[0];
        sn76489_clock = carry[1];
        s_rs_pos = (uint32_t)carry[2];
        s_rs_last = carry[3];
    } else {
        saveGwenesisStateSet(NULL, "p4os_md", MD_STATE_VERSION);
        gwenesis_save_state();
        int32_t carry[4] = { ym2612_clock, sn76489_clock, (int32_t)s_rs_pos, s_rs_last };
        saveGwenesisStateSetBuffer(NULL, "md_carry", carry, sizeof(carry));
    }
}

static void md_st_reset(int mode)
{
    free(s_st_buf);
    s_st_buf = NULL;
    s_st_len = s_st_cap = s_st_pos = 0;
    s_st_err = false;
    s_st_mode = mode;
}

static bool md_state_save(const char *path)
{
    if (!s_loaded || !path) return false;
    md_st_reset(ST_WRITE);
    md_state_all(false);
    uint32_t crc = md_crc32(s_st_buf, s_st_len);
    saveGwenesisStateSetBuffer(NULL, "crc32", &crc, sizeof(crc));
    bool ok = !s_st_err;

    size_t plen = strlen(path) + 5;
    char *tmp = ok ? malloc(plen) : NULL;
    if (!tmp) ok = false;
    if (ok) {
        snprintf(tmp, plen, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        ok = f && fwrite(s_st_buf, 1, s_st_len, f) == s_st_len;
        if (f && fclose(f) != 0) ok = false;
        if (ok) {
            remove(path);                    /* FAT does not rename over a file */
            ok = rename(tmp, path) == 0;
        }
        if (!ok) remove(tmp);
    }
    free(tmp);
    md_st_reset(ST_OFF);
    if (!ok) s_host->log("md: state save to %s failed", path);
    return ok;
}

/* The file against the layout this build writes: the same records, names
 * and sizes, in the same order, then the CRC. */
static bool md_state_check(const uint8_t *file, size_t size)
{
    md_st_reset(ST_LAYOUT);
    md_state_all(false);
    uint32_t crc0 = 0;
    saveGwenesisStateSetBuffer(NULL, "crc32", &crc0, sizeof(crc0));
    bool ok = !s_st_err;
    size_t fp = 0;
    for (size_t lp = 0; ok && lp < s_st_len; lp += sizeof(md_svar_t)) {
        md_svar_t want, got;
        memcpy(&want, s_st_buf + lp, sizeof(want));
        if (fp + sizeof(got) > size) {
            ok = false;
            break;
        }
        memcpy(&got, file + fp, sizeof(got));
        if (memcmp(want.key, got.key, sizeof(want.key)) != 0 || want.length != got.length
            || fp + sizeof(got) + got.length > size) {
            ok = false;
            break;
        }
        if (strcmp(want.key, "crc32") == 0) {
            uint32_t crc;
            memcpy(&crc, file + fp + sizeof(got), sizeof(crc));
            if (crc != md_crc32(file, fp)) ok = false;
        }
        fp += sizeof(got) + got.length;
    }
    if (ok && fp != size) ok = false;
    md_st_reset(ST_OFF);
    return ok;
}

static bool md_state_load(const char *path)
{
    if (!s_loaded || !path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    uint8_t *file = NULL;
    bool ok = size > 0 && size <= MD_STATE_MAX && fseek(f, 0, SEEK_SET) == 0;
    if (ok) file = malloc((size_t)size);
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) ok = false;
    fclose(f);

    if (ok && !md_state_check(file, (size_t)size)) {
        s_host->log("md: %s is not a state for this build", path);
        ok = false;
    }
    if (!ok) {
        free(file);
        return false;
    }

    md_st_reset(ST_READ);
    s_st_buf = file;
    s_st_len = s_st_cap = (size_t)size;
    md_state_all(true);
    ok = !s_st_err;
    md_st_reset(ST_OFF);
    if (!ok) {
        /* the check passed, so this is a version mismatch read in the
         * first record, before anything changed: nothing to undo */
        s_host->log("md: %s is from another version", path);
        return false;
    }

    /* The console stays the one the app was told about (its fps): a state
     * saved under the other region keeps playing at this one's. */
    io_reg[0] = gw_region_reg;
    gwenesis_vdp_status = (gwenesis_vdp_status & ~STATUS_PAL) | (s_pal ? STATUS_PAL : 0);
    gwenesis_bus_sram_update();
    if (gw_sram) gw_sram_dirty = 1;          /* sram_flush compares */
    /* the frame starts clean: the clocks were saved between frames */
    sn76489_index = 0;
    ym2612_index = 0;
    if (s_quality == 2) ym2612_clock = 0x1000000;
    return true;
}

/* ---- options and stats --------------------------------------------------- */

static void md_set_option(const char *key, int value)
{
    if (strcmp(key, "pad") == 0) {
        s_opt_pad6 = value == 1;
    } else if (strcmp(key, "region") == 0) {
        if (value < 0 || value > 3) value = 0;
        if (value == s_opt_region) return;
        s_opt_region = value;
        /* Right after load() (the app sets the options then) the console
         * starts again in the new region. Later it waits for the next
         * load: the app has the frame rate already. */
        if (s_loaded && s_frames == 0) {
            md_apply_region();
            md_power_cycle();
        } else if (s_loaded) {
            s_host->log("md: region applies from the next load");
        }
    } else if (strcmp(key, "sound") == 0) {
        if (value < 0 || value > 2) value = 0;
        s_opt_sound = value;
        if (s_loaded && value != s_quality) {
            s_quality = value;
            md_apply_quality();
            /* between frames: both chips start the next one together */
            ym2612_clock = 0;
            sn76489_clock = 0;
        }
    }
}

static const char *md_stats(void)
{
    if (!s_loaded) return NULL;
    static const char *const q[] = { "full", "fast", "no FM" };
    snprintf(s_stats, sizeof(s_stats), "md: %s %dx%d, %s pad, sound %s%s",
             s_pal ? "PAL" : "NTSC", screen_width, screen_height,
             s_opt_pad6 ? "6-button" : "3-button", q[s_quality],
             gw_sram ? ", sram" : "");
    return s_stats;
}

/* The buttons the pad has now: the six-button pad adds X Y Z and MODE
 * (emu_core_t.buttons_now; the app lays its pad out by them). */
static uint32_t md_buttons_now(void)
{
    uint32_t b = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_C | EMU_BTN_START;
    if (s_opt_pad6) b |= EMU_BTN_X | EMU_BTN_Y | EMU_BTN_Z | EMU_BTN_SELECT;
    return b;
}

/* Both widths fill a 4:3 TV: the 256-dot mode's pixels are 8/7, the
 * 320-dot one's 32/35. */
static float md_frame_aspect(int w, int h)
{
    float px = w <= 256 ? 8.0f / 7.0f : 32.0f / 35.0f;
    return h > 0 ? w * px / h : 0;
}

const emu_core_t emu_core_md = {
    .id = "md",
    .name = "Mega Drive",
    .short_name = "MD",
    .exts = ".md .gen .bin .smd",
    .max_w = MD_FRAME_W,
    .max_h = MD_FRAME_H,
    .pixel_aspect = 32.0f / 35.0f,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_C | EMU_BTN_START
             | EMU_BTN_X | EMU_BTN_Y | EMU_BTN_Z | EMU_BTN_SELECT,
    .face = EMU_FACE_AUTO,          /* ROW3, or GRID6 with the 6-button pad */
    .labels = s_labels,
    .options = s_options,
    .n_options = MD_N_OPTIONS,
    .load = md_load,
    .unload = md_unload,
    .reset = md_reset,
    .run = md_run,
    .fps = md_fps,
    .sample_rate = md_sample_rate,
    .state_save = md_state_save,
    .state_load = md_state_load,
    .sram_flush = md_sram_flush,
    .set_option = md_set_option,
    .stats = md_stats,
    .buttons_now = md_buttons_now,
    .frame_aspect = md_frame_aspect,
};
