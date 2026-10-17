/*
 * P4OS - Emulador, Neo Geo core: gngeo (as RetroESP32-P4 carries it) behind
 * the emu_core_t table.
 *
 * gngeo keeps one console in globals, as the app wants: one game at a time.
 * This file is what gngeo's emu.c and RetroESP32's esp32_platform.c were:
 * the frame (the 68000 and the Z80 in slices, the YM2610's timers between
 * them, the picture, the sound), the pads, the options, the battery RAM and
 * the save states. ng_load.c loads the game and sets up the caches on the
 * card.
 *
 * Timing is the MVS's: 59.1856 frames a second (24 MHz / 4 / 384 / 264),
 * 202752 cycles of the 68000 (12 MHz) and 67584 of the Z80 (4 MHz) a frame,
 * the frame cut in 16 slices so the sound CPU follows the main one. gngeo
 * ran the Z80's whole frame before the 68000's and RetroESP32 runs it on
 * the other core; here both run on the app's worker, one after the other.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"
#include "gngeo/emu.h"     /* gngeo's (main/ has an emu.h too) */
#include "memory.h"
#include "video.h"
#include "timer.h"
#include "pd4990a.h"
#include "ym2610/2610intf.h"
#include "ym2610/ym2610.h"
#include "generator68k/generator.h"
#include "generator68k/cpu68k.h"

#include "ng_core.h"

#ifndef N_
#define N_(s) s
#endif

#define NG_FPS          59.185606f
#define NG_FRAME_S      (1.0 / 59.185606)
#define CYC68K_FRAME    202752
#define CYCZ80_FRAME    67584
#define SLICES          16
#define LINES           264
#define CYC68K_LINE     (CYC68K_FRAME / LINES)  /* 768 */

/* The picture gngeo draws: 352 x 256, the 320 x 224 the console shows at
 * (16, 16). */
#define BUF_W           352
#define BUF_H           256

/* ---- what gngeo's platform layer defines ---------------------------------- */

struct gngeo_conf conf;
SDL_Surface *buffer;
SDL_Rect visible_area;
Uint16 *play_buffer;
int current_line;

static SDL_Surface s_buffer_surface;
static SDL_PixelFormat s_buffer_fmt = { 16, 2 };

/* memory.c / generator68k, P4OS additions */
void ng_mem_reset_statics(void);
void FMTimerInit(void);
void install_pvc_protection(void);
extern int neogeo_pvc_protection;
extern int neogeo_fix_bank_type;
extern uint32 cpu68k_flushes;

/* ---- the core's state -------------------------------------------------------- */

static const emu_host_t *s_host;
static bool s_loaded;
static ng_load_info_t s_info;
static char s_bios_dir[300];

static uint16_t *s_pixels;              /* the buffer's pixels */
static int16_t  *s_audio;               /* one frame of stereo samples */
static int s_rate;                      /* the YM2610's, fixed once it starts */
static int s_spf;                       /* samples a frame */
static bool s_sound_on;

static int s_fc;                        /* gngeo's auto-animation divider */
static int s_last_line;                 /* raster: the last line drawn */

static char *s_save_path;
static uint8_t *s_sram_copy;            /* what was last read or written */

/* Options, as the indices of their values. */
static int s_opt_system = NG_SYS_MVS;
static int s_opt_region = 0;
static bool s_opt_crop = true;
static int s_opt_rate = 1;
static int s_opt_cache = 0;
static ng_system_t s_loaded_system;
static int s_loaded_region;

static char s_stats[128];

/* ---- options --------------------------------------------------------------- */

static const char *const s_system_values[] = {
    N_("MVS (arcade)"), N_("AES (consola)"), N_("Universe BIOS"), NULL,
};
static const char *const s_region_values[] = {
    N_("Europa"), N_("EE. UU."), N_("Japón"), N_("Asia"), NULL,
};
static const char *const s_yes_no[] = { N_("Sí"), N_("No"), NULL };
static const char *const s_rate_values[] = {
    N_("Alta (44 kHz)"), N_("Media (22 kHz)"), N_("Baja (11 kHz)"), NULL,
};
static const int s_rates[] = { 44100, 22050, 11025 };
static const char *const s_cache_values[] = {
    N_("Automática"), N_("4 MB"), N_("8 MB"), N_("12 MB"), NULL,
};
static const int s_cache_mb[] = { 0, 4, 8, 12 };

static const emu_option_t s_options[] = {
    { "system",    N_("Sistema"),           s_system_values, 0 },
    { "region",    N_("Región"),            s_region_values, 0 },
    { "crop",      N_("Recortar costados"), s_yes_no,        0 },
    { "sound",     N_("Calidad de sonido"), s_rate_values,   1 },
    { "spr_cache", N_("Caché de sprites"),  s_cache_values,  0 },
};
#define NG_N_OPTIONS ((int)(sizeof(s_options) / sizeof(s_options[0])))


/* ---- gngeo's emu.c, what the core keeps of it ------------------------------ */

/* Protection checks some sets need patched out (gngeo's setup_misc_patch). */
static void setup_misc_patch(const char *name)
{
    if (!strcmp(name, "ssideki")) {
        WRITE_WORD_ROM(&memory.rom.cpu_m68k.p[0x2240], 0x4e71);
    }
    if (!strcmp(name, "mslugx")) {
        Uint8 *RAM = memory.rom.cpu_m68k.p;
        for (Uint32 i = 0; i + 8 <= memory.rom.cpu_m68k.size; i += 2) {
            if ((READ_WORD_ROM(&RAM[i + 0]) == 0x0243) &&
                (READ_WORD_ROM(&RAM[i + 2]) == 0x0001) && /* andi.w  #$1, D3 */
                (READ_WORD_ROM(&RAM[i + 4]) == 0x6600)) { /* bne xxxx */
                WRITE_WORD_ROM(&RAM[i + 4], 0x4e71);
                WRITE_WORD_ROM(&RAM[i + 6], 0x4e71);
            }
        }
        WRITE_WORD_ROM(&RAM[0x3bdc], 0x4e71);
        WRITE_WORD_ROM(&RAM[0x3bde], 0x4e71);
        WRITE_WORD_ROM(&RAM[0x3be0], 0x4e71);
        WRITE_WORD_ROM(&RAM[0x3c0c], 0x4e71);
        WRITE_WORD_ROM(&RAM[0x3c0e], 0x4e71);
        WRITE_WORD_ROM(&RAM[0x3c10], 0x4e71);
        WRITE_WORD_ROM(&RAM[0x3c36], 0x4e71);
        WRITE_WORD_ROM(&RAM[0x3c38], 0x4e71);
    }
}

/* gngeo's neogeo_reset(): the 68000 from the BIOS, the pads let go. */
void neogeo_reset(void)
{
    sram_lock = 0;
    sound_code = 0;
    pending_command = 0;
    result_code = 0;
    memory.intern_p1 = 0xFF;
    memory.intern_p2 = 0xFF;
    memory.intern_start = 0x8F;
    memory.intern_coin = 0x3F;
    if (memory.rom.cpu_m68k.size > 0x100000)
        cpu_68k_bankswitch(0x100000);
    else
        cpu_68k_bankswitch(0);
    cpu_68k_reset();
}

/* Power on (load and reset): the work and video RAM blank, the BIOS's
 * vectors and text layer, the sound CPU and chip from the start. The
 * battery RAM and the memory card keep what they have. */
static void power_on(void)
{
    memset(memory.ram, 0, sizeof(memory.ram));
    memset(memory.vid.ram, 0, sizeof(memory.vid.ram));
    memset(memory.vid.pal_neo, 0, sizeof(memory.vid.pal_neo));
    memset(memory.vid.pal_host, 0, sizeof(memory.vid.pal_host));
    memory.vid.currentpal = 0;
    memory.vid.currentfix = 0;
    memory.vid.rbuf = 0;
    memory.vid.vptr = 0;
    memory.vid.modulo = 1;
    memory.vid.irq2control = 0;
    memory.vid.irq2taken = 0;
    memory.vid.irq2start = 1000;
    memory.vid.irq2pos = 0;
    memory.current_vector = 0;
    memory.watchdog = 0;
    memory.kof98_prot_state = 0;
    neogeo_frame_counter = 0;
    neogeo_frame_counter_speed = 8;
    s_fc = 0;
    s_last_line = 0;
    current_line = 0;
    memcpy(memory.rom.cpu_m68k.p, memory.rom.bios_m68k.p, 0x80);
    current_pal = memory.vid.pal_neo[0];
    current_pc_pal = (Uint32 *)memory.vid.pal_host[0];
    current_fix = memory.rom.bios_sfix.p;
    fix_usage = memory.fix_board_usage;
    cpu68k_clearcache();
    cpu_z80_init();
    if (s_sound_on) YM2610Reset();
    ng_timer_init(NG_FRAME_S, SLICES);
    if (s_sound_on) FMTimerInit();
    neogeo_reset();
}

/* The YM2610 at the rate the options asked for, the first time sound or a
 * state needs it (the app gives the options after load()). */
static void sound_start(void)
{
    if (s_sound_on || !s_loaded) return;
    s_rate = s_rates[s_opt_rate];
    s_spf = (int)((float)s_rate / NG_FPS + 0.5f);
    conf.sample_rate = (Uint16)s_rate;
    YM2610_sh_start();
    s_sound_on = true;
}

/* ---- the frame ----------------------------------------------------------- */

static void frame_counter_tick(void)
{
    pd4990a_addretrace();
    if (!(memory.vid.irq2control & 0x8)) {
        if (s_fc >= (int)neogeo_frame_counter_speed) {
            s_fc = 0;
            neogeo_frame_counter++;
        }
        s_fc++;
    }
}

/* gngeo's update_scanline() (raster mode): the timer interrupt's line. */
static int update_scanline(bool draw)
{
    memory.vid.irq2taken = 0;
    if (memory.vid.irq2control & 0x10) {
        if (current_line == (int)memory.vid.irq2start) {
            if (memory.vid.irq2control & 0x80)
                memory.vid.irq2start += (memory.vid.irq2pos + 3) / 0x180;
            memory.vid.irq2taken = 1;
        }
    }
    if (memory.vid.irq2taken) {
        if (draw) {
            if (s_last_line < 21) s_last_line = 21;
            if (current_line < 20) current_line = 20;
            draw_screen_scanline(s_last_line - 21, current_line - 20, 0);
        }
        s_last_line = current_line;
    }
    current_line++;
    return memory.vid.irq2taken;
}

/* gngeo's update_screen() (raster mode): the rest of the picture. */
static void update_screen(bool draw)
{
    if (memory.vid.irq2control & 0x40)
        memory.vid.irq2start = (memory.vid.irq2pos + 3) / 0x180;
    else
        memory.vid.irq2start = 1000;
    if (draw) {
        if (s_last_line < 21)
            draw_screen();
        else
            draw_screen_scanline(s_last_line - 21, 262, 1);
    }
    s_last_line = 0;
    pd4990a_addretrace();
    if (s_fc >= (int)neogeo_frame_counter_speed) {
        s_fc = 0;
        neogeo_frame_counter++;
    }
    s_fc++;
}

static void z80_slice(int i)
{
    int from = CYCZ80_FRAME * i / SLICES, to = CYCZ80_FRAME * (i + 1) / SLICES;
    cpu_z80_run(to - from);
    my_timer();
}

static void run_frame(bool draw)
{
    cpu_68k_frame_start();
    if (!s_info.raster) {
        int done = 0;
        for (int i = 0; i < SLICES; i++) {
            int target = CYC68K_FRAME * (i + 1) / SLICES;
            done = target + cpu_68k_run((Uint32)(target - done));
            z80_slice(i);
        }
        frame_counter_tick();
        if (draw) draw_screen();
    } else {
        int over = 0, zs = 0;
        current_line = 0;
        for (int line = 0; line < LINES; line++) {
            over = cpu_68k_run((Uint32)(CYC68K_LINE - over));
            if (update_scanline(draw)) cpu_68k_interrupt(2);
            int next = (line + 1) * SLICES / LINES;
            while (zs < next) z80_slice(zs++);
        }
        while (zs < SLICES) z80_slice(zs++);
        update_screen(draw);
    }
    memory.watchdog++;
    if (memory.watchdog > 7) {
        /* The BIOS and the games write to the watchdog every frame; one
         * that stops for 8 frames resets the 68000, as the hardware. */
        ng_log("watchdog reset");
        cpu_68k_reset();
        memory.watchdog = 0;
    }
    cpu_68k_interrupt(1);
}

/* ---- pads ------------------------------------------------------------------ */

static uint8_t pad_bits(uint32_t b)
{
    uint8_t v = 0xFF;
    if (b & EMU_BTN_UP)    v &= ~0x01;
    if (b & EMU_BTN_DOWN)  v &= ~0x02;
    if (b & EMU_BTN_LEFT)  v &= ~0x04;
    if (b & EMU_BTN_RIGHT) v &= ~0x08;
    if (b & EMU_BTN_A)     v &= ~0x10;
    if (b & EMU_BTN_B)     v &= ~0x20;
    if (b & EMU_BTN_C)     v &= ~0x40;
    if (b & EMU_BTN_D)     v &= ~0x80;
    return v;
}

static void set_pads(const uint32_t *pads)
{
    uint32_t p1 = pads ? pads[0] : 0, p2 = pads ? pads[1] : 0;
    memory.intern_p1 = pad_bits(p1);
    memory.intern_p2 = pad_bits(p2);
    /* REG_STATUS_B: start and select of each player, the memory card in
     * (bits 4-6 low), bit 7 the system (1: MVS). */
    uint8_t st = s_loaded_system == NG_SYS_AES ? 0x0F : 0x8F;
    if (p1 & EMU_BTN_START) st &= ~0x01;
    if (p2 & EMU_BTN_START) st &= ~0x04;
    /* REG_STATUS_A: the coins. On an arcade board SELECT is the coin; on
     * the home console it is SELECT. */
    uint8_t coin = 0x3F;
    if (s_loaded_system == NG_SYS_AES) {
        if (p1 & EMU_BTN_SELECT) st &= ~0x02;
        if (p2 & EMU_BTN_SELECT) st &= ~0x08;
    } else {
        if (p1 & EMU_BTN_SELECT) coin &= ~0x01;
        if (p2 & EMU_BTN_SELECT) coin &= ~0x02;
    }
    memory.intern_start = st;
    memory.intern_coin = coin;
}

/* ---- battery RAM ------------------------------------------------------------- */

/* The .sav is the MVS's 64 KB backup RAM followed by the 2 KB memory card
 * (where the home console keeps its saves), as the 68000 sees them. A
 * file of 64 KB alone (the backup RAM) is read too. */
#define SAV_SIZE (0x10000 + 0x800)

static void sram_open(const char *path)
{
    s_save_path = NULL;
    s_sram_copy = NULL;
    if (!path) return;
    size_t n = strlen(path) + 1;
    s_save_path = malloc(n);
    s_sram_copy = malloc(SAV_SIZE);
    if (!s_save_path || !s_sram_copy) {
        free(s_save_path);
        free(s_sram_copy);
        s_save_path = NULL;
        s_sram_copy = NULL;
        return;
    }
    memcpy(s_save_path, path, n);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t got = fread(memory.sram, 1, 0x10000, f);
        if (got == 0x10000) {
            size_t c = fread(memory.memcard, 1, 0x800, f);
            (void)c;
        }
        fclose(f);
        ng_log("battery RAM read (%u bytes)", (unsigned)got);
    }
    memcpy(s_sram_copy, memory.sram, 0x10000);
    memcpy(s_sram_copy + 0x10000, memory.memcard, 0x800);
}

static bool nc_sram_flush(void)
{
    if (!s_loaded || !s_sram_copy) return true;
    if (memcmp(s_sram_copy, memory.sram, 0x10000) == 0 &&
        memcmp(s_sram_copy + 0x10000, memory.memcard, 0x800) == 0)
        return true;
    FILE *f = fopen(s_save_path, "wb");
    if (!f) {
        ng_log("cannot write %s", s_save_path);
        return false;
    }
    bool ok = fwrite(memory.sram, 1, 0x10000, f) == 0x10000 &&
              fwrite(memory.memcard, 1, 0x800, f) == 0x800;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        memcpy(s_sram_copy, memory.sram, 0x10000);
        memcpy(s_sram_copy + 0x10000, memory.memcard, 0x800);
    } else {
        ng_log("writing %s failed", s_save_path);
    }
    return ok;
}

/* ---- load and unload ------------------------------------------------------- */

static void free_all(void)
{
    if (ng_mem) ng_unload_game();
    cpu_68k_free();
    ng_ym_free();
    ng_z80_free();
    ng_big_free(s_pixels);
    ng_big_free(s_audio);
    ng_big_free(ng_mem);
    s_pixels = NULL;
    s_audio = NULL;
    play_buffer = NULL;
    ng_mem = NULL;
    buffer = NULL;
    free(s_save_path);
    free(s_sram_copy);
    s_save_path = NULL;
    s_sram_copy = NULL;
    s_loaded = false;
    s_sound_on = false;
}

static void nc_unload(void)
{
    free_all();
    if (ng_big_used()) ng_log("%u bytes still allocated after unload", (unsigned)ng_big_used());
    s_host = NULL;
}

static bool nc_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    free_all();
    s_host = host;
    ng_platform_init(host);
    ng_printf_reset();
    memset(&conf, 0, sizeof(conf));
    conf.sound = 1;
    conf.sample_rate = 22050;
    memset(&s_info, 0, sizeof(s_info));
    snprintf(s_bios_dir, sizeof(s_bios_dir), "%s", req->bios_dir ? req->bios_dir : ".");

    ng_mem = ng_big_alloc(sizeof(neo_mem));
    s_pixels = ng_big_alloc(BUF_W * BUF_H * sizeof(uint16_t));
    s_audio = ng_big_alloc(2 * 1024 * sizeof(int16_t));
    if (!ng_mem || !s_pixels || !s_audio || !cpu_68k_alloc() || !ng_ym_alloc() || !ng_z80_alloc()) {
        free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    play_buffer = (Uint16 *)s_audio;
    s_buffer_surface.format = &s_buffer_fmt;
    s_buffer_surface.w = BUF_W;
    s_buffer_surface.h = BUF_H;
    s_buffer_surface.pitch = BUF_W * 2;
    s_buffer_surface.pixels = s_pixels;
    buffer = &s_buffer_surface;
    visible_area.x = 16;
    visible_area.y = 16;
    visible_area.w = 320;
    visible_area.h = 224;

    ng_mem_reset_statics();
    neogeo_fix_bank_type = 0;
    neogeo_pvc_protection = 0;

    ng_load_opts_t lo = {
        .system = (ng_system_t)s_opt_system,
        .region = s_opt_region,
        .spr_cache_mb = s_cache_mb[s_opt_cache],
        .sample_rate = s_rates[s_opt_rate],
    };
    if (!ng_load_game(host, req, &lo, &s_info, err, errlen)) {
        free_all();
        return false;
    }
    s_loaded_system = s_info.system;   /* the loader may fall back to another */
    s_loaded_region = lo.region;

    /* gngeo's init_neo() */
    cpu_68k_init();
    if (neogeo_pvc_protection) install_pvc_protection();
    pd4990a_init();
    setup_misc_patch(s_info.game);
    s_loaded = true;
    power_on();
    sram_open(req->save);

    host->log("neogeo: %s (%s), %s%s, sprites %s, samples %s, %u KB used",
              s_info.game, s_info.longname,
              s_loaded_system == NG_SYS_AES ? "AES" : s_loaded_system == NG_SYS_UNI ? "Universe BIOS" : "MVS",
              s_info.raster ? ", drawn line by line" : "",
              s_info.tiles_streamed ? "from the card" : "in memory",
              s_info.adpcma_streamed ? "from the card" : "in memory",
              (unsigned)(ng_big_used() / 1024));
    return true;
}

/* ---- running ----------------------------------------------------------------- */

static void nc_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;
    sound_start();
    set_pads(pads);
    run_frame(draw);
    if (draw) {
        int crop = s_opt_crop ? 8 : 0;
        const uint16_t *px = s_pixels + 16 * BUF_W + 16 + crop;
        s_host->video(px, 320 - 2 * crop, 224, BUF_W * 2);
    }
    YM2610Update_stream(s_spf);
    s_host->audio(s_audio, s_spf);
}

static void nc_reset(void)
{
    if (!s_loaded) return;
    power_on();
}

static float nc_fps(void)
{
    if (!s_spf) {
        int rate = s_rates[s_opt_rate];
        int spf = (int)((float)rate / NG_FPS + 0.5f);
        return (float)rate / (float)spf;
    }
    return (float)s_rate / (float)s_spf;
}

static int nc_sample_rate(void)
{
    return s_sound_on ? s_rate : s_rates[s_opt_rate];
}

/* ---- save states ---------------------------------------------------------- */

/* A state: "NGST", a version, the game's name, the length of what follows
 * and its CRC-32, then each part in turn. Read whole and checked before
 * anything is touched. */
#define ST_MAGIC   0x5453474Eu  /* "NGST" */
#define ST_VERSION 1
#define ST_HDR     (4 + 4 + 24 + 4 + 4)

uint32_t ng_crc32(uint32_t crc, const uint8_t *p, uint32_t n);

static void state_parts(ng_st_t *st)
{
    ng_68k_state(st);
    ng_z80_state(st);
    ng_mem_state(st);
    ng_timer_state(st);
    ng_ym_state(st);
    ng_rtc_state(st);
    int32_t mine[3] = { s_fc, s_last_line, current_line };
    ng_st_data(st, mine, sizeof(mine));
    if (!st->writing && !st->failed) {
        s_fc = mine[0];
        s_last_line = mine[1];
        current_line = mine[2];
    }
}

static bool nc_state_save(const char *path)
{
    if (!s_loaded) return false;
    sound_start();
    ng_st_t st = { .writing = true };
    uint8_t hdr[ST_HDR];
    memset(hdr, 0, sizeof(hdr));
    ng_st_data(&st, hdr, sizeof(hdr));
    state_parts(&st);
    bool ok = !st.failed;
    if (ok) {
        uint32_t len = st.len - ST_HDR;
        uint32_t crc = ng_crc32(0, st.buf + ST_HDR, len);
        uint32_t v[2] = { ST_MAGIC, ST_VERSION };
        memcpy(st.buf, v, 8);
        memcpy(st.buf + 8, s_info.game, strlen(s_info.game));
        memcpy(st.buf + 32, &len, 4);
        memcpy(st.buf + 36, &crc, 4);
        size_t pl = strlen(path);
        char *tmp = malloc(pl + 5);
        FILE *f = tmp ? (snprintf(tmp, pl + 5, "%s.tmp", path), fopen(tmp, "wb")) : NULL;
        ok = f && fwrite(st.buf, 1, st.len, f) == st.len;
        if (f && fclose(f) != 0) ok = false;
        if (ok) {
            remove(path);
            ok = rename(tmp, path) == 0;
        }
        if (!ok && tmp) remove(tmp);
        free(tmp);
    }
    free(st.buf);
    if (!ok) ng_log("state not written: %s", path);
    return ok;
}

static bool nc_state_load(const char *path)
{
    if (!s_loaded) return false;
    sound_start();
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    uint8_t *buf = NULL;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size >= ST_HDR && size < 4 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)size);
        if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) ok = true;
    }
    fclose(f);
    if (ok) {
        uint32_t v[2], len, crc;
        memcpy(v, buf, 8);
        memcpy(&len, buf + 32, 4);
        memcpy(&crc, buf + 36, 4);
        char game[25];
        memcpy(game, buf + 8, 24);
        game[24] = 0;
        ok = v[0] == ST_MAGIC && v[1] == ST_VERSION && strcmp(game, s_info.game) == 0 &&
             len == (uint32_t)size - ST_HDR && ng_crc32(0, buf + ST_HDR, len) == crc;
    }
    if (!ok) {
        free(buf);
        ng_log("%s is not a state of this game for this build", path);
        return false;
    }
    /* Checked whole: a dry run first would be needed only if a part could
     * refuse what the CRC accepts; the sizes are fixed for a build, so the
     * read below cannot run short. */
    ng_st_t st = { .writing = false, .buf = buf, .pos = ST_HDR, .len = (uint32_t)size };
    state_parts(&st);
    free(buf);
    if (st.failed) {
        ng_log("state load failed half way, game reset");
        power_on();
        return false;
    }
    cpu_68k_bankswitch(bankaddress);
    ng_ym_timers_relink();
    /* The sprite cache keeps its banks: they are the same ROM. */
    return true;
}

/* ---- options and stats ---------------------------------------------------- */

static void nc_set_option(const char *key, int value)
{
    if (strcmp(key, "system") == 0) {
        if (value < 0 || value > 2) value = 0;
        s_opt_system = value;
    } else if (strcmp(key, "region") == 0) {
        if (value < 0 || value > 3) value = 0;
        s_opt_region = value;
    } else if (strcmp(key, "crop") == 0) {
        s_opt_crop = value == 0;
        return;
    } else if (strcmp(key, "sound") == 0) {
        if (value < 0 || value > 2) value = 1;
        /* Taken when the sound starts (the first frame or state after
         * load); a change later waits for the next game: the app opened
         * the speaker at the rate it was told. */
        s_opt_rate = value;
        return;
    } else if (strcmp(key, "spr_cache") == 0) {
        if (value < 0 || value > 3) value = 0;
        s_opt_cache = value;  /* for the next game */
        return;
    } else {
        return;
    }
    /* Another BIOS: swapped in and the console started again (the app
     * gives the kept options right after load, before the first frame). */
    if (s_loaded && ((ng_system_t)s_opt_system != s_loaded_system || s_opt_region != s_loaded_region)) {
        ng_system_t got;
        if (ng_bios_switch(s_bios_dir, (ng_system_t)s_opt_system, s_opt_region, &got)) {
            s_loaded_system = got;
            s_loaded_region = s_opt_region;
            cpu_68k_init_bios();
            power_on();
        }
    }
}

static const char *nc_stats(void)
{
    if (!s_loaded) return NULL;
    const GFX_CACHE *c = &memory.vid.spr_cache;
    if (c->data) {
        uint32_t tot = c->hits + c->misses;
        snprintf(s_stats, sizeof(s_stats),
                 "neogeo: %s, sprites cache %u KB, %u%% hits, %u reads, 68k flushes %u",
                 s_info.game, (unsigned)(c->size / 1024),
                 tot ? (unsigned)(c->hits / (tot / 100 + 1)) : 100u, (unsigned)c->reads,
                 (unsigned)cpu68k_flushes);
        memory.vid.spr_cache.hits = memory.vid.spr_cache.misses = memory.vid.spr_cache.reads = 0;
    } else {
        snprintf(s_stats, sizeof(s_stats), "neogeo: %s, all in memory, 68k flushes %u",
                 s_info.game, (unsigned)cpu68k_flushes);
    }
    return s_stats;
}

const emu_core_t emu_core_neogeo = {
    .id = "neogeo",
    .name = "Neo Geo",
    .short_name = "NEO",
    .exts = ".zip",
    .max_w = 320,
    .max_h = 224,
    .pixel_aspect = 0,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_C | EMU_BTN_D |
               EMU_BTN_SELECT | EMU_BTN_START,
    .face = EMU_FACE_ARC4,
    .labels = NULL,
    .options = s_options,
    .n_options = NG_N_OPTIONS,
    .load = nc_load,
    .unload = nc_unload,
    .reset = nc_reset,
    .run = nc_run,
    .fps = nc_fps,
    .sample_rate = nc_sample_rate,
    .state_save = nc_state_save,
    .state_load = nc_state_load,
    .sram_flush = nc_sram_flush,
    .set_option = nc_set_option,
    .stats = nc_stats,
};
