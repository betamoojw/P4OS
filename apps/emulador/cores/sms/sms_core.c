/*
 * P4OS - Emulador, Sega 8-bit core: SMS Plus (as Retro-Go carries it, with
 * a Z80 and an SN76489 of our own) behind three emu_core_t tables: the
 * Master System (emu_core_sms, which also takes SG-1000 cartridges), the
 * Game Gear (emu_core_gg) and the ColecoVision (emu_core_col).
 *
 * SMS Plus holds one console in globals, which is all the app asks for.
 * It draws each line as 8-bit palette indices into a 256-wide bitmap
 * (the Game Gear's 160 columns sit 48 pixels in) and makes a frame of
 * sound per frame; this file turns the first into RGB565 for the host and
 * hands the second on.
 *
 * What SMS Plus does not do and this file does: reading the ROM file
 * (copier headers, small images mirrored to the 32 KB the mapper
 * expects), the ColecoVision BIOS from the card, the battery save as a raw
 * .sav, and state files that are checked before anything is touched.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"
#include "shared.h"

#ifndef N_
#define N_(s) s
#endif

/* 44.1 kHz; a frame makes a whole number of samples (sound.c), and the
 * frame rate the app is told is worked out from it, so picture and sound
 * never drift: 44100 / 736 = 59.92 for NTSC (the console's 59.92) and
 * 44100 / 887 = 49.72 for PAL (49.70). */
#define SMS_SAMPLE_RATE 44100

/* The biggest Master System and Game Gear cartridges are 1 MB; Korean
 * multi-carts go to 4 MB. */
#define SMS_ROM_MAX (4 * 1024 * 1024)
/* ColecoVision cartridges map at $8000-$FFFF; mega-carts are not
 * supported. */
#define COL_ROM_MAX 0x8000
#define COL_BIOS_SIZE 0x2000

#define SMS_PIC_W 256
#define SMS_PIC_H 240

enum { K_SMS, K_GG, K_COL };

static const emu_host_t *s_host;
static bool s_loaded;
static int s_kind;              /* which table loaded the game              */
static bool s_sg1000;           /* an SG-1000 cartridge on the SMS table    */
static uint32_t s_frames;       /* frames run since load                     */

static uint8_t *s_rom;          /* the cartridge image SMS Plus maps        */
static uint8_t *s_bios;         /* the ColecoVision BIOS                    */
static uint8_t *s_pix;          /* SMS Plus's 8-bit bitmap, 256 x 240       */
static uint16_t *s_frame;       /* the RGB565 picture handed to video()     */
static uint16_t s_pal[256];

/* The battery save: where it goes, and the cartridge RAM as last written
 * or read, to tell whether the game changed it. */
static char *s_save_path;
static uint8_t *s_sram_copy;
static bool s_sram_live;        /* the file existed or the game mapped RAM  */

/* Options, as the indices of their values. */
static int s_opt_region;        /* 0 auto, 1 USA, 2 Europe, 3 Japan          */
static int s_opt_region_used;   /* what the loaded game was set up with      */
static bool s_opt_sprite_limit = true;
static int s_opt_tms_pal = 2;   /* ColecoVision's palette                    */

static char s_stats[96];

/* ---- the log, CRC, and what SMS Plus calls --------------------------- */

#define SMS_LOG_BUDGET 40
static int s_log_count;

void sms_log(int level, const char *fmt, ...)
{
    if (!s_host || level > 2 || s_log_count > SMS_LOG_BUDGET) return;
    if (++s_log_count > SMS_LOG_BUDGET) {
        s_host->log("sms: too many messages, the rest are dropped");
        return;
    }
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    s_host->log("sms: %s", line);
}

/* zlib's CRC-32 (the database's entries are made with it), half a byte at
 * a time: a 64-byte table. */
uint32 sms_crc32(uint32 crc, const uint8 *buf, size_t len)
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

static char *sc_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- options ----------------------------------------------------------- */

static const char *const s_region_values[] = {
    N_("Automática"), N_("Estados Unidos"), N_("Europa"), N_("Japón"), NULL,
};
static const char *const s_tms_pal_values[] = {
    /* SMS Plus's three TMS9918 palettes, in its order */
    N_("Sean Young"), N_("Omar Cornut"), N_("Richard Drushel"), NULL,
};
static const char *const s_yes_no[] = { N_("Sí"), N_("No"), NULL };

static const emu_option_t s_options_sms[] = {
    { "region",       N_("Región"),            s_region_values, 0 },
    { "sprite_limit", N_("Límite de sprites"), s_yes_no,        0 },
};
static const emu_option_t s_options_col[] = {
    { "palette",      N_("Paleta"),            s_tms_pal_values, 2 },
    { "sprite_limit", N_("Límite de sprites"), s_yes_no,         0 },
};

/* ---- the battery save -------------------------------------------------- */

static bool sc_has_battery(void)
{
    return s_kind != K_COL && !s_sg1000;
}

static void sc_sram_open(const char *path)
{
    if (!path || !sc_has_battery() || !cart.sram) return;
    s_save_path = sc_strdup(path);
    s_sram_copy = malloc(0x8000);
    if (!s_save_path || !s_sram_copy) {
        free(s_save_path);
        free(s_sram_copy);
        s_save_path = NULL;
        s_sram_copy = NULL;
        s_host->log("sms: no memory for the battery save");
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t got = fread(cart.sram, 1, 0x8000, f);
        fclose(f);
        s_sram_live = true;
        s_host->log("sms: battery save read (%u bytes)", (unsigned)got);
    }
    memcpy(s_sram_copy, cart.sram, 0x8000);
}

/* The whole 32 KB the mapper can put at $8000, raw: what SMS Plus GX and
 * Genesis Plus GX write; files of 8 KB from other emulators load too. */
static bool sc_sram_flush(void)
{
    if (!s_loaded || !s_sram_copy) return true;
    if (sms.save) s_sram_live = true;
    if (!s_sram_live || memcmp(cart.sram, s_sram_copy, 0x8000) == 0) return true;

    FILE *f = fopen(s_save_path, "wb");
    if (!f) {
        s_host->log("sms: cannot write %s", s_save_path);
        return false;
    }
    bool ok = fwrite(cart.sram, 1, 0x8000, f) == 0x8000;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        memcpy(s_sram_copy, cart.sram, 0x8000);
    } else {
        s_host->log("sms: writing %s failed", s_save_path);
    }
    return ok;
}

/* ---- load and unload --------------------------------------------------- */

static void sc_free_all(void)
{
    if (s_loaded || sms.wram || vdp.vram || snd.buffer) system_shutdown();
    s_loaded = false;
    free(s_rom);
    free(cart.sram);
    free(s_bios);
    free(s_pix);
    free(s_frame);
    free(s_save_path);
    free(s_sram_copy);
    s_rom = NULL;
    s_bios = NULL;
    s_pix = NULL;
    s_frame = NULL;
    s_save_path = NULL;
    s_sram_copy = NULL;
    s_sram_live = false;
    s_sg1000 = false;
    s_frames = 0;
    /* SMS Plus's globals, all of them, so the next game starts clean */
    memset(&smsplus, 0, sizeof(smsplus));
    memset(&sms, 0, sizeof(sms));
    memset(&bios, 0, sizeof(bios));
    memset(&slot, 0, sizeof(slot));
    memset(&coleco, 0, sizeof(coleco));
    memset(&vdp, 0, sizeof(vdp));
    memset(&snd, 0, sizeof(snd));
    memset(cpu_readmap, 0, sizeof(cpu_readmap));
    memset(cpu_writemap, 0, sizeof(cpu_writemap));
    z80_cycle_count = 0;
}

static void sc_unload(void)
{
    sc_free_all();
    s_host = NULL;
}

static bool sc_ext_is(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static const char *sc_not_a_game(void)
{
    switch (s_kind) {
    case K_GG: return "No es un juego de Game Gear";
    case K_COL: return "No es un juego de ColecoVision";
    default: return "No es un juego de Master System";
    }
}

/* Reads the cartridge into a buffer of whole 16 KB pages and at least
 * 32 KB (the mapper's first two pages), the image repeated to fill it as
 * the address lines of a small ROM do. */
static bool sc_read_rom(const char *path, int *crc_size, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return false;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    long skip = 0;
    long max = s_kind == K_COL ? COL_ROM_MAX : SMS_ROM_MAX;
    /* a copier's 512-byte header: an odd number of 512-byte blocks */
    if (s_kind != K_COL && size > 0x4000 && ((size / 512) & 1)) skip = 512;
    long rom = size - skip;
    if (size < 0 || rom < 0x400) {
        fclose(f);
        snprintf(err, errlen, "%s", sc_not_a_game());
        return false;
    }
    if (rom > max) {
        fclose(f);
        snprintf(err, errlen, s_kind == K_COL ? "El juego es más grande que un cartucho de ColecoVision"
                                              : "El archivo es demasiado grande");
        return false;
    }
    long buf = (rom + 0x3FFF) & ~0x3FFFL;
    if (buf < 0x8000) buf = 0x8000;
    s_rom = malloc((size_t)buf);
    if (!s_rom) {
        fclose(f);
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    bool ok = fseek(f, skip, SEEK_SET) == 0 && fread(s_rom, 1, (size_t)rom, f) == (size_t)rom;
    fclose(f);
    if (!ok) {
        snprintf(err, errlen, "No se pudo leer el archivo");
        return false;
    }
    for (long i = rom; i < buf; i++) s_rom[i] = s_rom[i % rom];
    *crc_size = (int)rom;
    cart.size = (uint32)buf;
    return true;
}

/* The ColecoVision BIOS: coleco.rom (MAME's and most emulators' name) or
 * colecovision.rom, 8 KB, from the console's bios folder. */
static bool sc_read_bios(const char *dir)
{
    if (!dir) return false;
    static const char *const names[] = { "coleco.rom", "colecovision.rom" };
    char path[320];
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%.280s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        s_bios = malloc(COL_BIOS_SIZE);
        bool ok = s_bios && fread(s_bios, 1, COL_BIOS_SIZE, f) == COL_BIOS_SIZE;
        fclose(f);
        if (ok) return true;
        free(s_bios);
        s_bios = NULL;
        s_host->log("sms: %s is not an 8 KB BIOS", path);
    }
    return false;
}

/* SMS Plus's options, then its look at the cartridge (database, header),
 * for the kind of console the table stands for. */
static void sc_configure(void)
{
    system_reset_config();
    option.sndrate = SMS_SAMPLE_RATE;
    option.overscan = 0;
    option.extra_gg = 0;
    option.tms_pal = (uint8)s_opt_tms_pal;
    option.spritelimit = s_opt_sprite_limit;
    option.country = s_kind == K_COL ? 0 : s_opt_region;
    option.console = s_kind == K_COL ? 6 : s_sg1000 ? 5 : 0;
    set_rom_config();
    /* the Game Gear table plays Game Gear games: a cartridge without a
     * header (most Japanese ones) would be taken for a Master System one;
     * the database's GG-in-SMS-mode games stay as they are */
    if (s_kind == K_GG && !IS_GG) {
        option.console = 3;
        set_rom_config();
    }
    /* every Game Gear runs at 60 Hz: the region only says the language */
    if (s_kind == K_GG) sms.display = DISPLAY_NTSC;
    s_opt_region_used = s_opt_region;
}

static void sc_set_bitmap(void)
{
    bitmap.width = SMS_PIC_W;
    bitmap.height = SMS_PIC_H;
    bitmap.pitch = SMS_PIC_W;
    bitmap.data = s_pix;
}

static bool sc_load(int kind, const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    sc_free_all();
    s_host = host;
    s_log_count = 0;
    s_kind = kind;
    s_sg1000 = kind == K_SMS && sc_ext_is(req->rom, ".sg");

    if (kind == K_COL && !sc_read_bios(req->bios_dir)) {
        sc_free_all();
        snprintf(err, errlen, "Falta la BIOS de ColecoVision (coleco.rom)");
        return false;
    }

    int crc_size = 0;
    if (!sc_read_rom(req->rom, &crc_size, err, errlen)) {
        sc_free_all();
        return false;
    }

    s_pix = calloc(SMS_PIC_W * SMS_PIC_H, 1);
    s_frame = malloc(SMS_PIC_W * SMS_PIC_H * sizeof(uint16_t));
    if (!s_pix || !s_frame) {
        sc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    coleco.rom = s_bios;
    if (!load_rom(s_rom, (int)cart.size, crc_size)) {
        sc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    sc_configure();
    sc_set_bitmap();

    system_poweron();
    if (!sms.wram || !vdp.vram || !snd.buffer || !render_ok()) {
        sc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    /* The stack where the BIOS leaves it: a few games (Shadow Dancer, Ace
     * of Aces) start without setting it. */
    if (!IS_TMS) Z80.sp.w.l = 0xDFF0;

    s_loaded = true;
    sc_sram_open(req->save);

    static const char *const names[] = { "SMS", "GG", "Coleco" };
    host->log("sms: %s, console %02X, %s, %s, mapper %d, %u KB, crc %08X",
              names[kind], sms.console, sms.display == DISPLAY_PAL ? "PAL" : "NTSC",
              sms.territory == TERRITORY_DOMESTIC ? "Japan" : "export", cart.mapper,
              (unsigned)(crc_size / 1024), (unsigned)cart.crc);
    return true;
}

static bool sc_load_sms(const emu_host_t *h, const emu_load_t *r, char *e, size_t n) { return sc_load(K_SMS, h, r, e, n); }
static bool sc_load_gg(const emu_host_t *h, const emu_load_t *r, char *e, size_t n) { return sc_load(K_GG, h, r, e, n); }
static bool sc_load_col(const emu_host_t *h, const emu_load_t *r, char *e, size_t n) { return sc_load(K_COL, h, r, e, n); }

/* ---- running ----------------------------------------------------------- */

static uint8 sc_pad(uint32_t b)
{
    uint8 p = 0;
    if (b & EMU_BTN_UP)    p |= INPUT_UP;
    if (b & EMU_BTN_DOWN)  p |= INPUT_DOWN;
    if (b & EMU_BTN_LEFT)  p |= INPUT_LEFT;
    if (b & EMU_BTN_RIGHT) p |= INPUT_RIGHT;
    if (b & EMU_BTN_B)     p |= INPUT_BUTTON1;   /* the left button  */
    if (b & EMU_BTN_A)     p |= INPUT_BUTTON2;   /* the right button */
    return p;
}

/* The ColecoVision's keypad: four of its twelve keys are on the pad
 * (SELECT *, START #, L 1, R 2); 0xFF is no key. */
static uint8 sc_keypad(uint32_t b)
{
    if (b & EMU_BTN_L)      return 1;
    if (b & EMU_BTN_R)      return 2;
    if (b & EMU_BTN_SELECT) return 10;
    if (b & EMU_BTN_START)  return 11;
    return 0xFF;
}

static void sc_blit(void)
{
    render_copy_palette(s_pal);   /* only when it changed */
    int x0 = bitmap.viewport.x, w = bitmap.viewport.w, h = bitmap.viewport.h;
    if (w > SMS_PIC_W) w = SMS_PIC_W;
    if (h > SMS_PIC_H) h = SMS_PIC_H;
    const uint16_t *pal = s_pal;
    uint16_t *dst = s_frame;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = s_pix + y * SMS_PIC_W + x0;
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

static void sc_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;
    uint32_t p0 = pads ? pads[0] : 0, p1 = pads ? pads[1] : 0;

    input.pad[0] = sc_pad(p0);
    input.pad[1] = s_kind == K_GG ? 0 : sc_pad(p1);
    input.system = 0;
    if (s_kind == K_COL) {
        coleco.keypad[0] = sc_keypad(p0);
        coleco.keypad[1] = sc_keypad(p1);
    } else if (IS_GG) {
        /* in Master System mode START is the pause button */
        if (p0 & EMU_BTN_START)
            input.system |= sms.console == CONSOLE_GGMS ? (INPUT_START | INPUT_PAUSE) : INPUT_START;
    } else {
        /* the Master System's PAUSE button, on the console */
        if ((p0 | p1) & EMU_BTN_START) input.system |= INPUT_PAUSE;
    }

    sc_set_bitmap();
    system_frame(draw ? 0 : 1);
    if (draw) sc_blit();
    s_host->audio(snd.buffer, snd.sample_count);
    s_frames++;
}

/* A reset is the console's power switch: the cartridge's RAM survives it,
 * as it does on the cartridge. */
static void sc_reset(void)
{
    if (!s_loaded) return;
    system_reset();
    if (!IS_TMS) Z80.sp.w.l = 0xDFF0;
}

static float sc_fps(void)
{
    if (!s_loaded || snd.sample_count <= 0) return 59.92f;
    return (float)snd.sample_rate / (float)snd.sample_count;
}

static int sc_sample_rate(void)
{
    return SMS_SAMPLE_RATE;
}

/* ---- save states ------------------------------------------------------- */

/* A state is SMS Plus's structures as they are in memory, behind a header
 * that says which build and which game wrote it: the sizes of every
 * structure (a build whose structures differ refuses the file) and the
 * cartridge's CRC. Pointers inside the structures are put back from the
 * running game after loading. */
#define SC_MAGIC 0x4D533450u   /* "P4SM" */
#define SC_VERSION 1

typedef struct {
    uint32_t magic, version, crc, size;
    uint8_t kind, console, sg1000, _pad;
    uint16_t sz_sms, sz_vdp, sz_z80, sz_psg, sz_coleco, sz_render, sz_pio, _pad2;
} sc_state_hdr_t;

static size_t sc_state_body(void)
{
    return sizeof(sms_t) + 0x2000 + sizeof(vdp_t) + 0x4000 + 4 + 0x8000
         + sizeof(coleco_t) + sizeof(Z80_Regs) + sizeof(int) + sizeof(sms_psg_t)
         + render_state_size() + pio_state_size();
}

static void sc_state_hdr(sc_state_hdr_t *h)
{
    memset(h, 0, sizeof(*h));
    h->magic = SC_MAGIC;
    h->version = SC_VERSION;
    h->crc = cart.crc;
    h->size = (uint32_t)(sizeof(*h) + sc_state_body());
    h->kind = (uint8_t)s_kind;
    h->console = sms.console;
    h->sg1000 = s_sg1000;
    h->sz_sms = sizeof(sms_t);
    h->sz_vdp = sizeof(vdp_t);
    h->sz_z80 = sizeof(Z80_Regs);
    h->sz_psg = sizeof(sms_psg_t);
    h->sz_coleco = sizeof(coleco_t);
    h->sz_render = (uint16_t)render_state_size();
    h->sz_pio = (uint16_t)pio_state_size();
}

#define PUT(p, src, n) do { memcpy(p, src, n); p += (n); } while (0)
#define GET(dst, p, n) do { memcpy(dst, p, n); p += (n); } while (0)

static bool sc_state_save(const char *path)
{
    if (!s_loaded) return false;
    sc_state_hdr_t h;
    sc_state_hdr(&h);
    uint8_t *buf = malloc(h.size);
    if (!buf) return false;
    uint8_t *p = buf;
    PUT(p, &h, sizeof(h));
    PUT(p, &sms, sizeof(sms_t));
    PUT(p, sms.wram, 0x2000);
    PUT(p, &vdp, sizeof(vdp_t));
    PUT(p, vdp.vram, 0x4000);
    PUT(p, cart.fcr, 4);
    PUT(p, cart.sram, 0x8000);
    PUT(p, &coleco, sizeof(coleco_t));
    PUT(p, &Z80, sizeof(Z80_Regs));
    PUT(p, &z80_cycle_count, sizeof(int));
    PUT(p, &snd.psg, sizeof(sms_psg_t));
    render_state_save(p);
    p += render_state_size();
    pio_state_save(p);

    size_t n = strlen(path) + 5;
    char *tmp = malloc(n);
    bool ok = false;
    if (tmp) {
        snprintf(tmp, n, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            ok = fwrite(buf, 1, h.size, f) == h.size;
            if (fclose(f) != 0) ok = false;
            if (ok) {
                remove(path);
                ok = rename(tmp, path) == 0;
            }
            if (!ok) remove(tmp);
        }
        free(tmp);
    }
    free(buf);
    if (!ok) s_host->log("sms: cannot write the state %s", path);
    return ok;
}

static bool sc_state_load(const char *path)
{
    if (!s_loaded) return false;
    sc_state_hdr_t want, got;
    sc_state_hdr(&want);

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *buf = malloc(want.size);
    size_t n = buf ? fread(buf, 1, want.size + 1, f) : 0;
    fclose(f);
    if (!buf) return false;
    if (n != want.size) {
        free(buf);
        s_host->log("sms: %s is not a state of this build (%u bytes)", path, (unsigned)n);
        return false;
    }
    memcpy(&got, buf, sizeof(got));
    if (memcmp(&got, &want, sizeof(got)) != 0) {
        free(buf);
        s_host->log("sms: %s is not a state for this game and build", path);
        return false;
    }

    /* Everything checked: from here on the file is applied in full. */
    system_reset();

    const uint8_t *p = buf + sizeof(got);
    uint8 *wram = sms.wram, *vram = vdp.vram;
    uint8 *crom = coleco.rom;
    Z80_Regs keep = Z80;

    GET(&sms, p, sizeof(sms_t));
    sms.wram = wram;
    GET(sms.wram, p, 0x2000);
    GET(&vdp, p, sizeof(vdp_t));
    vdp.vram = vram;
    GET(vdp.vram, p, 0x4000);
    GET(cart.fcr, p, 4);
    GET(cart.sram, p, 0x8000);
    GET(&coleco, p, sizeof(coleco_t));
    coleco.rom = crom;
    GET(&Z80, p, sizeof(Z80_Regs));
    Z80.irq_callback = keep.irq_callback;
    Z80.mem_read = keep.mem_read;
    Z80.mem_write = keep.mem_write;
    Z80.port_read = keep.port_read;
    Z80.port_write = keep.port_write;
    int cycles;
    GET(&cycles, p, sizeof(int));
    sms_psg_t psg;
    GET(&psg, p, sizeof(sms_psg_t));
    const uint8_t *rs = p;
    p += render_state_size();
    const uint8_t *ps = p;

    /* The memory map, from the mapper's registers. */
    if (sms.console == CONSOLE_COLECO) {
        coleco_port_w(0x53, coleco.port53);
        coleco_port_w(0x7F, coleco.port7F);
    } else if (!IS_TMS) {
        slot.rom = cart.rom;
        slot.pages = cart.pages;
        slot.mapper = cart.mapper;
        slot.fcr = &cart.fcr[0];
        mapper_reset();
        cpu_readmap[0] = &slot.rom[0];
        uint8 fcr[4];
        memcpy(fcr, cart.fcr, 4);
        for (int i = 0; i < 4; i++) {
            if (slot.mapper != MAPPER_KOREA_MSX) mapper_16k_w(i, fcr[i]);
            else mapper_8k_w(i, fcr[i]);
        }
    }

    /* The VDP's derived state (lines, viewport, renderer, palette) and the
     * sound's timing, then what they keep from line to line. */
    vdp_init();
    for (int i = 0; i < PALETTE_SIZE; i++) palette_sync(i);
    sound_init();
    snd.psg = psg;
    snd.done_so_far = 0;
    render_state_load(rs);
    pio_state_load(ps);
    z80_cycle_count = cycles;
    free(buf);
    return true;
}

/* ---- options and stats ------------------------------------------------- */

static void sc_set_option(const char *key, int value)
{
    if (strcmp(key, "region") == 0) {
        if (value < 0 || value > 3) value = 0;
        s_opt_region = value;
        /* The region decides the console's timing: it is set up at load,
         * and changed here only before the first frame (the app's call
         * right after load); later, on the next load. */
        if (s_loaded && s_frames == 0 && s_opt_region != s_opt_region_used && s_kind != K_COL) {
            sc_configure();
            vdp_init();
            sound_init();
            system_reset();
            if (!IS_TMS) Z80.sp.w.l = 0xDFF0;
        }
    } else if (strcmp(key, "sprite_limit") == 0) {
        s_opt_sprite_limit = (value == 0);
        option.spritelimit = s_opt_sprite_limit;
    } else if (strcmp(key, "palette") == 0) {
        if (value < 0 || value > 2) value = 2;
        s_opt_tms_pal = value;
        option.tms_pal = (uint8)value;
        if (s_loaded) {
            for (int i = 0; i < PALETTE_SIZE; i++) palette_sync(i);
        }
    }
}

static const char *sc_stats(void)
{
    if (!s_loaded) return NULL;
    int fps100 = snd.sample_count > 0 ? snd.sample_rate * 100 / snd.sample_count : 0;
    snprintf(s_stats, sizeof(s_stats), "sms: console %02X, %s, mapper %d, %d.%02d fps%s",
             sms.console, sms.display == DISPLAY_PAL ? "PAL" : "NTSC", cart.mapper,
             fps100 / 100, fps100 % 100, s_sram_live ? ", battery" : "");
    return s_stats;
}

/* ---- the tables -------------------------------------------------------- */

/* What each console calls its buttons, by bit (emu_core.h): the Master
 * System's 1 and 2 and its PAUSE on the console, the Game Gear's 1, 2 and
 * START; the ColecoVision's side buttons and the four keypad keys on the
 * pad. */
static const char *const s_labels_sms[EMU_BTN_COUNT] = {
    [4] = "2", [5] = "1", [11] = "PAUSE",
};
static const char *const s_labels_gg[EMU_BTN_COUNT] = {
    [4] = "2", [5] = "1", [11] = "START",
};
static const char *const s_labels_col[EMU_BTN_COUNT] = {
    [4] = N_("Der"), [5] = N_("Izq"), [8] = "1", [9] = "2", [10] = "*", [11] = "#",
};

#define SC_COMMON                                   \
    .unload = sc_unload,                            \
    .reset = sc_reset,                              \
    .run = sc_run,                                  \
    .fps = sc_fps,                                  \
    .sample_rate = sc_sample_rate,                  \
    .state_save = sc_state_save,                    \
    .state_load = sc_state_load,                    \
    .sram_flush = sc_sram_flush,                    \
    .set_option = sc_set_option,                    \
    .stats = sc_stats

const emu_core_t emu_core_sms = {
    .id = "sms",
    .name = "Master System",
    .short_name = "SMS",
    .exts = ".sms .sg",
    .max_w = SMS_PIC_W,
    .max_h = SMS_PIC_H,
    .pixel_aspect = 8.0f / 7.0f,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_START,
    .face = EMU_FACE_ROW2,
    .labels = s_labels_sms,
    .options = s_options_sms,
    .n_options = 2,
    .load = sc_load_sms,
    SC_COMMON,
};

const emu_core_t emu_core_gg = {
    .id = "gg",
    .name = "Game Gear",
    .short_name = "GG",
    .exts = ".gg",
    .max_w = SMS_PIC_W,
    .max_h = 192,
    .pixel_aspect = 6.0f / 5.0f,
    .players = 1,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_START,
    .face = EMU_FACE_DIAG2,
    .labels = s_labels_gg,
    .options = s_options_sms,
    .n_options = 2,
    .load = sc_load_gg,
    SC_COMMON,
};

const emu_core_t emu_core_col = {
    .id = "col",
    .name = "ColecoVision",
    .short_name = "Coleco",
    .exts = ".col",
    .max_w = SMS_PIC_W,
    .max_h = 192,
    .pixel_aspect = 8.0f / 7.0f,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_L | EMU_BTN_R
             | EMU_BTN_SELECT | EMU_BTN_START,
    .face = EMU_FACE_ROW2,
    .labels = s_labels_col,
    .options = s_options_col,
    .n_options = 2,
    .load = sc_load_col,
    SC_COMMON,
};
