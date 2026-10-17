/*
 * P4OS - Emulador, NES core: nofrendo (as Retro-Go carries it) behind the
 * emu_core_t table.
 *
 * nofrendo is a single console held in globals: one game at a time, which
 * is all the app asks for. It draws each frame as 8-bit palette indices
 * into a 272x240 buffer (8 pixels of margin on each side of the 256 the
 * console shows) and fills one frame of sound into a buffer of its own;
 * this file turns the first into RGB565 for the host and hands the second
 * on as it is.
 *
 * What nofrendo does not do and this file does: the battery save (Retro-Go
 * keeps it only inside its save states), checking a state file before
 * trusting it, and keeping the battery RAM across a reset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"
#include "nes_rg_shim.h"
#include "nofrendo.h"

#ifndef N_
#define N_(s) s
#endif

/* The APU's rate. nofrendo makes a whole number of samples per frame
 * (rate / 60, or / 50 for PAL), so the frame rate the app is told is
 * worked out from it (see nc_fps) and sound and picture never drift. */
#define NES_SAMPLE_RATE 32000

/* nofrendo refuses ROMs above this size (rom_loadfile): the biggest NES
 * games are 1 MB. */
#define NES_ROM_MAX 0x200000

#define NES_SRAM_MAX (ROM_PRG_BANK_SIZE * 4)

static const emu_host_t *s_host;
static nes_t *s_nes;
static bool s_loaded;

static uint8_t *s_vidbuf;           /* nofrendo's 8-bit frame, PITCH x 240  */
static uint16_t *s_frame;           /* the RGB565 picture handed to video() */

/* The palette, 256 entries: nofrendo's pixels carry two priority bits
 * above the 64 colours, and the GUI's 8 colours sit at 192. Half a
 * kilobyte that the conversion loop keeps in the data cache by itself, so
 * it is left wherever the .so's data goes. */
static uint16_t s_pal[256];

/* The battery save: where it goes, and the RAM as it was last written (or
 * read), to tell whether the game changed it. NULL when the game has no
 * battery. */
static char *s_save_path;
static uint8_t *s_sram_copy;
static size_t s_sram_size;

/* Options, as the indices of their values below. */
static int s_opt_palette = NES_PALETTE_PVM;
static bool s_opt_overscan = true;
static bool s_opt_crop_sides = false;
static bool s_opt_sprite_limit = true;

static char s_stats[96];

/* ---- options ----------------------------------------------------------- */

static const char *const s_palette_values[] = {
    /* In nofrendo's order (nespal_t). */
    N_("Nofrendo"), N_("Compuesta"), N_("NES Classic"), N_("NTSC"),
    N_("PVM"), N_("Suave"), NULL,
};
static const char *const s_yes_no[] = { N_("Sí"), N_("No"), NULL };

static const emu_option_t s_options[] = {
    { "palette",      N_("Paleta"),            s_palette_values, NES_PALETTE_PVM },
    { "overscan",     N_("Recortar bordes"),   s_yes_no,         0 },
    { "crop_sides",   N_("Recortar costados"), s_yes_no,         1 },
    { "sprite_limit", N_("Límite de sprites"), s_yes_no,         0 },
};
#define NES_N_OPTIONS ((int)(sizeof(s_options) / sizeof(s_options[0])))

static void nc_build_palette(int n)
{
    /* nofrendo_buildpalette(n, 16) gives plain RGB565 values (red in the
     * top bits) in the CPU's byte order, which is what LVGL takes on both
     * the board and the Mac. Retro-Go swaps them for its SPI panels; this
     * does not. */
    uint16_t *pal = nofrendo_buildpalette((nespal_t)n, 16);
    if (!pal) return;
    memcpy(s_pal, pal, sizeof(s_pal));
    free(pal);
}

/* ---- the battery save -------------------------------------------------- */

static bool nc_has_battery(void)
{
    return s_nes && s_nes->cart && s_nes->cart->type == ROM_TYPE_INES
        && s_nes->cart->battery && s_nes->cart->prg_ram && s_nes->cart->prg_ram_banks > 0;
}

/* Reads the save into PRG-RAM (if there is one) and keeps a copy to
 * compare against. */
static void nc_sram_open(const char *path)
{
    if (!path || !nc_has_battery()) return;

    size_t size = (size_t)s_nes->cart->prg_ram_banks * ROM_PRG_BANK_SIZE;
    if (size > NES_SRAM_MAX) size = NES_SRAM_MAX;

    s_save_path = nes_rg_strdup(path);
    s_sram_copy = malloc(size);
    if (!s_save_path || !s_sram_copy) {
        free(s_save_path);
        free(s_sram_copy);
        s_save_path = NULL;
        s_sram_copy = NULL;
        s_host->log("nes: no memory for the battery save");
        return;
    }
    s_sram_size = size;

    FILE *f = fopen(path, "rb");
    if (f) {
        size_t got = fread(s_nes->cart->prg_ram, 1, size, f);
        fclose(f);
        s_host->log("nes: battery save read (%u bytes)", (unsigned)got);
    }
    memcpy(s_sram_copy, s_nes->cart->prg_ram, size);
}

static bool nc_sram_flush(void)
{
    if (!s_loaded || !s_sram_copy) return true;
    const uint8_t *ram = s_nes->cart->prg_ram;
    if (memcmp(ram, s_sram_copy, s_sram_size) == 0) return true;

    FILE *f = fopen(s_save_path, "wb");
    if (!f) {
        s_host->log("nes: cannot write %s", s_save_path);
        return false;
    }
    bool ok = fwrite(ram, 1, s_sram_size, f) == s_sram_size;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        memcpy(s_sram_copy, ram, s_sram_size);
    } else {
        s_host->log("nes: writing %s failed", s_save_path);
    }
    return ok;
}

/* ---- load and unload --------------------------------------------------- */

static void nc_free_all(void)
{
    if (s_nes) nes_shutdown();
    s_nes = NULL;
    s_loaded = false;
    free(s_vidbuf);
    free(s_frame);
    free(s_save_path);
    free(s_sram_copy);
    s_vidbuf = NULL;
    s_frame = NULL;
    s_save_path = NULL;
    s_sram_copy = NULL;
    s_sram_size = 0;
}

static void nc_unload(void)
{
    nc_free_all();
    nes_rg_shim_init(NULL, 0);
    s_host = NULL;
}

/* A look at the file before nofrendo gets it, for an error the player can
 * act on: nofrendo only says -1 for all of these. The mapper is read here
 * too because nofrendo frees the cart before saying it does not know it. */
static bool nc_precheck(const char *path, int *mapper, char *err, size_t errlen)
{
    *mapper = -1;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return false;
    }
    uint8_t h[16];
    size_t got = fread(h, 1, sizeof(h), f);
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    fclose(f);

    if (got < sizeof(h) || size < 16) {
        snprintf(err, errlen, "No es un juego de NES");
        return false;
    }
    if (size > NES_ROM_MAX) {
        snprintf(err, errlen, "El archivo es demasiado grande");
        return false;
    }
    if (memcmp(h, ROM_NES_MAGIC, 4) == 0) {
        /* The same rule as rom_loadmem(): junk in the last header bytes
         * of an old iNES dump means the high nibble is junk too. */
        *mapper = (h[7] & 0xF0) | (h[6] >> 4);
        if (h[12] | h[13] | h[14] | h[15]) *mapper &= 0x0F;
        return true;
    }
    if (memcmp(h, ROM_FDS_MAGIC, 4) == 0 || memcmp(h, ROM_FDS_RAW_MAGIC, 15) == 0
        || memcmp(h, ROM_NSF_MAGIC, 5) == 0) {
        return true;
    }
    snprintf(err, errlen, "No es un juego de NES");
    return false;
}

/* The Famicom Disk System's BIOS: disksys.rom is the name most emulators
 * use, fds_bios.bin the one Retro-Go uses. NULL when there is no folder. */
static char *nc_fds_bios(const char *dir)
{
    if (!dir) return NULL;
    static const char *const names[] = { "disksys.rom", "fds_bios.bin" };
    size_t len = strlen(dir) + 16;
    char *path = malloc(len);
    if (!path) return NULL;
    for (int i = 0; i < 2; i++) {
        snprintf(path, len, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            return path;
        }
    }
    snprintf(path, len, "%s/%s", dir, names[0]);
    return path;
}

static bool nc_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    nc_free_all();
    s_host = host;
    nes_rg_shim_init(host->log, 2);

    int mapper = -1;
    if (!nc_precheck(req->rom, &mapper, err, errlen)) return false;

    s_vidbuf = calloc(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT, 1);
    s_frame = malloc(NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT * sizeof(uint16_t));
    if (!s_vidbuf || !s_frame) {
        nc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    char *bios = nc_fds_bios(req->bios_dir);
    s_nes = nes_init(SYS_DETECT, NES_SAMPLE_RATE, true, bios);
    free(bios);
    if (!s_nes) {
        nc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    int ret = nes_loadfile(req->rom);
    if (ret < 0) {
        /* nofrendo has shut itself down already; shutting down twice is
         * harmless (it frees and clears). */
        nc_free_all();
        if (ret == -2)
            snprintf(err, errlen, "Mapper %d no soportado", mapper);
        else if (ret == -3)
            snprintf(err, errlen, "Falta la BIOS del Famicom Disk System (disksys.rom)");
        else
            snprintf(err, errlen, "No se pudo cargar el juego");
        return false;
    }

    /* The second pad: nofrendo connects only the first. */
    input_connect(1, NES_JOYPAD);
    s_nes->blit_func = NULL;
    nes_setvidbuf(s_vidbuf);

    nc_build_palette(s_opt_palette);
    ppu_setopt(PPU_LIMIT_SPRITES, s_opt_sprite_limit);

    s_loaded = true;
    nc_sram_open(req->save);

    /* Retro-Go runs two frames before anything else, "necessary for
     * successful state restoration". Their sound is dropped. */
    nes_emulate(false);
    nes_emulate(false);

    const rom_t *cart = s_nes->cart;
    host->log("nes: mapper %d (%s), %s, PRG %dK, CHR %dK%s, crc %08X",
              s_nes->mapper->number, s_nes->mapper->name ? s_nes->mapper->name : "?",
              s_nes->system == SYS_NES_PAL ? "PAL" : "NTSC",
              cart->prg_rom_banks * 8, cart->chr_rom_banks * 8,
              cart->battery ? ", battery" : "", (unsigned)cart->checksum);
    return true;
}

/* ---- running ----------------------------------------------------------- */

static uint8_t nc_pad_bits(uint32_t b)
{
    uint8_t p = 0;
    if (b & EMU_BTN_A)      p |= NES_PAD_A;
    if (b & EMU_BTN_B)      p |= NES_PAD_B;
    if (b & EMU_BTN_SELECT) p |= NES_PAD_SELECT;
    if (b & EMU_BTN_START)  p |= NES_PAD_START;
    if (b & EMU_BTN_UP)     p |= NES_PAD_UP;
    if (b & EMU_BTN_DOWN)   p |= NES_PAD_DOWN;
    if (b & EMU_BTN_LEFT)   p |= NES_PAD_LEFT;
    if (b & EMU_BTN_RIGHT)  p |= NES_PAD_RIGHT;
    return p;
}

static void nc_blit(void)
{
    /* nofrendo's overscan is 8 lines for NTSC (what a TV hides) and 0 for
     * PAL, as Retro-Go crops it. */
    int crop_v = s_opt_overscan ? s_nes->overscan : 0;
    int crop_h = s_opt_crop_sides ? 8 : 0;
    int w = NES_SCREEN_WIDTH - 2 * crop_h;
    int h = NES_SCREEN_HEIGHT - 2 * crop_v;

    /* An NSF is music only: the PPU is never turned on and would show its
     * backdrop grey. Black, so the app's own player screen can sit on it. */
    if (s_nes->cart->type == ROM_TYPE_NSF) {
        memset(s_frame, 0, (size_t)w * h * sizeof(uint16_t));
        s_host->video(s_frame, w, h, w * (int)sizeof(uint16_t));
        return;
    }

    const uint16_t *pal = s_pal;
    uint16_t *dst = s_frame;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = NES_SCREEN_GETPTR(s_vidbuf, crop_h, y + crop_v);
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

static void nc_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;

    input_update(0, pads ? nc_pad_bits(pads[0]) : 0);
    input_update(1, pads ? nc_pad_bits(pads[1]) : 0);

    /* nes_reset() forgets the buffer: give it every frame. */
    nes_setvidbuf(s_vidbuf);
    nes_emulate(draw);
    if (draw) nc_blit();

    s_host->audio(s_nes->apu->buffer, s_nes->apu->samples_per_frame);
}

/* A reset is the console's power switch, not its reset button: some games
 * do not start again cleanly from the button. The battery RAM survives it,
 * as it does on the cartridge. */
static void nc_do_reset(void)
{
    if (!s_loaded) return;
    uint8_t *keep = NULL;
    if (s_sram_copy) {
        keep = malloc(s_sram_size);
        if (keep) memcpy(keep, s_nes->cart->prg_ram, s_sram_size);
    }
    nes_reset(true);
    if (keep) {
        memcpy(s_nes->cart->prg_ram, keep, s_sram_size);
        free(keep);
    }
    nes_setvidbuf(s_vidbuf);
}

static float nc_fps(void)
{
    if (!s_loaded || s_nes->apu->samples_per_frame <= 0)
        return 60.0988f;
    /* 32000 / 533 = 60.04 for NTSC (the console's 60.10, 0.1 % slower)
     * and 32000 / 640 = 50.0 for PAL (50.007). */
    return (float)s_nes->apu->sample_rate / (float)s_nes->apu->samples_per_frame;
}

static int nc_sample_rate(void)
{
    return NES_SAMPLE_RATE;
}

/* ---- save states ------------------------------------------------------- */

static uint32_t nc_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* nofrendo's state_load() writes into the console block by block as it
 * reads, so a broken or foreign file would leave the game half restored.
 * This walks the file first: the SNSS header, every block inside the
 * file, a base block of the right size, and RAM blocks that fit this
 * cartridge. */
static bool nc_state_check(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    long size = -1;
    uint8_t h[12];

    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 8 || fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8
        || memcmp(h, "SNSS", 4) != 0)
        goto done;

    uint32_t blocks = nc_be32(h + 4);
    if (blocks == 0 || blocks > 16) goto done;

    const rom_t *cart = s_nes->cart;
    bool base = false;
    long pos = 8;
    for (uint32_t i = 0; i < blocks; i++) {
        if (fseek(f, pos, SEEK_SET) != 0 || fread(h, 1, 12, f) != 12) goto done;
        uint32_t len = nc_be32(h + 8);
        if (len > (uint32_t)(size - pos - 12)) goto done;
        if (memcmp(h, "BASR", 4) == 0) {
            if (len < 9 + 0x800 + 0x100 + 0x1000 + 0x20 + 8) goto done;
            base = true;
        } else if (memcmp(h, "VRAM", 4) == 0) {
            if (len > (uint32_t)cart->chr_ram_banks * ROM_CHR_BANK_SIZE) goto done;
        } else if (memcmp(h, "SRAM", 4) == 0) {
            if (len < 1 || len - 1 > (uint32_t)cart->prg_ram_banks * ROM_PRG_BANK_SIZE) goto done;
        }
        pos += 12 + (long)len;
    }
    ok = base;
done:
    fclose(f);
    return ok;
}

static bool nc_state_save(const char *path)
{
    if (!s_loaded) return false;
    return state_save(path) == 0;
}

static bool nc_state_load(const char *path)
{
    if (!s_loaded) return false;
    if (!nc_state_check(path)) {
        s_host->log("nes: %s is not a state for this game", path);
        return false;
    }
    if (state_load(path) != 0) {
        /* Only a read error after the check gets here, with the console
         * half restored: start the game again rather than run that. */
        s_host->log("nes: state load failed half way, game reset");
        nc_do_reset();
        return false;
    }
    nes_setvidbuf(s_vidbuf);
    return true;
}

/* ---- options and stats ------------------------------------------------- */

static void nc_set_option(const char *key, int value)
{
    if (strcmp(key, "palette") == 0) {
        if (value < 0 || value >= NES_PALETTE_COUNT) value = NES_PALETTE_PVM;
        s_opt_palette = value;
        nc_build_palette(value);
    } else if (strcmp(key, "overscan") == 0) {
        s_opt_overscan = (value == 0);
    } else if (strcmp(key, "crop_sides") == 0) {
        s_opt_crop_sides = (value == 0);
    } else if (strcmp(key, "sprite_limit") == 0) {
        s_opt_sprite_limit = (value == 0);
        if (s_loaded) ppu_setopt(PPU_LIMIT_SPRITES, s_opt_sprite_limit);
    }
}

static const char *nc_stats(void)
{
    if (!s_loaded) return NULL;
    /* In hundredths and integers: printf's floats are soft doubles. */
    int spf = s_nes->apu->samples_per_frame;
    int fps100 = spf > 0 ? (int)((NES_SAMPLE_RATE * 100L) / spf) : 0;
    snprintf(s_stats, sizeof(s_stats), "nes: mapper %d, %s, %d.%02d fps%s",
             s_nes->mapper->number,
             s_nes->system == SYS_NES_PAL ? "PAL" : "NTSC",
             fps100 / 100, fps100 % 100,
             s_sram_copy ? ", battery" : "");
    return s_stats;
}

const emu_core_t emu_core_nes = {
    .id = "nes",
    .name = "NES",
    .short_name = "NES",
    .exts = ".nes .fds .nsf",
    .max_w = NES_SCREEN_WIDTH,
    .max_h = NES_SCREEN_HEIGHT,
    .players = 2,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_SELECT | EMU_BTN_START,
    .face = EMU_FACE_ROW2,
    .options = s_options,
    .n_options = NES_N_OPTIONS,
    .load = nc_load,
    .unload = nc_unload,
    .reset = nc_do_reset,
    .run = nc_run,
    .fps = nc_fps,
    .sample_rate = nc_sample_rate,
    .state_save = nc_state_save,
    .state_load = nc_state_load,
    .sram_flush = nc_sram_flush,
    .set_option = nc_set_option,
    .stats = nc_stats,
};
