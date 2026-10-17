/*
 * P4OS - Emulador, Game Boy and Game Boy Color core: gnuboy (as Retro-Go
 * carries it) behind two emu_core_t tables, emu_core_gb and emu_core_gbc.
 *
 * gnuboy is one console held in globals (GB, cart and the statics of its
 * files): one game at a time, which is all the app asks for. Both tables
 * drive the same emulator; the console a game runs as comes from its
 * cartridge header, as on a Game Boy Color: a colour game (header byte
 * 0x143 bit 7) runs as a GBC on either table, a Game Boy game as a Game
 * Boy with the palette the option picks. The gb table starts on the green
 * DMG screen, the gbc table on the colours a GBC gives a Game Boy game.
 *
 * gnuboy draws each line straight into a 160x144 RGB565 buffer of ours and
 * fills a buffer of stereo samples that it hands back through a callback.
 *
 * What gnuboy does not do and this file does: the battery save in the
 * common .sav format (the RAM as it is, then the MBC3 clock as VBA-M and
 * mGBA write it, with the real time it was written so the clock catches up
 * on the time the game was not running), checking a state file before
 * trusting it, keeping the battery RAM and the clock across a reset and
 * the clock across a state load.
 *
 * Everything in the library but the two tables is made local when it is
 * built (board.cmake, sim.cmake), so gnuboy's names (GB, cart, cpu...)
 * never meet another core's.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu_core.h"
#include "gnuboy.h"
#include "hw.h"

/* cpu.h (through hw.h) defines the CPU's registers as one-letter macros,
 * and hw.h 'host' as GB: none of them is used below. */
#undef A
#undef B
#undef C
#undef D
#undef E
#undef F
#undef H
#undef L
#undef host

#define N_(s) s

/* gnuboy makes a sample every snd.rate double-speed cycles, a whole
 * number worked out from the rate it is given: 2^21 / 32768 = 64 exactly,
 * so the rate it really plays at is the one reported. A frame is 35112 of
 * those cycles: 548.625 samples a frame at 59.7275 frames a second, both
 * exact, so sound and picture never drift. */
#define GB_SAMPLE_RATE   32768
#define GB_DSC_PER_SEC   2097152
#define GB_FRAME_DSC     35112

#define GB_W 160
#define GB_H 144

/* The biggest Game Boy Color games are 8 MB; the header ends at 0x150. */
#define GB_ROM_MAX   (8u * 1024 * 1024)
#define GB_ROM_MIN   0x150u
#define GB_BANK      0x4000u

/* gnuboy's sound buffer, in int16 values (two a stereo frame). A frame
 * makes 549 frames at most; the rest is room for a frame with the screen
 * off or a game that reads the sound registers late. */
#define GB_AUDIO_LEN 4096

/* The MBC3 clock after the RAM in a .sav: ten 32-bit words (seconds,
 * minutes, hours, days, control; the same latched) and the time it was
 * written, 64 bits (48 bytes, VBA-M and mGBA) or 32 (44 bytes, older). */
#define GB_RTC_LEN     48
#define GB_RTC_LEN_OLD 44

/* gnuboy's state: a 4 KB header of tagged values, then RAM, VRAM and
 * the cartridge's RAM in 4 KB blocks (gnuboy.c, do_save_load). */
#define GB_STATE_BLOCK   4096
#define GB_STATE_VERSION 0x107

/* A broken game can log an invalid opcode on every instruction. */
#define GB_LOG_BUDGET 40

static const emu_host_t *s_host;
static bool s_inited;               /* gnuboy_init() done                 */
static bool s_loaded;

static uint8_t *s_rom;              /* the whole game, padded to its banks */
static size_t s_rom_size;
static uint16_t *s_frame;           /* gnuboy's RGB565 picture             */
static int16_t *s_audio;

static char *s_save_path;
static int s_ram_code;              /* header byte 0x149                   */
static bool s_rtc_saved;            /* the clock went to the .sav once     */

static int s_cart_type;
static int s_log_count;

/* The palette option, as an index into s_palette_map. */
static int s_opt_palette;

static char s_stats[96];

/* ---- log ----------------------------------------------------------------- */

/* gnuboy's levels: 1 error, 2 warning, 3 info. */
void gnuboy_p4os_log(int level, const char *fmt, ...)
{
    if (!s_host || level > 2 || s_log_count > GB_LOG_BUDGET) return;
    if (++s_log_count > GB_LOG_BUDGET) {
        s_host->log("gb: too many messages, the rest are dropped");
        return;
    }
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    s_host->log("gb: %s", line);
}

/* ---- options ------------------------------------------------------------- */

static const char *const s_palette_values[] = {
    N_("Verde (DMG)"), N_("Gris"), N_("Pocket"), N_("Color automático"), NULL,
};
static const gb_palette_t s_palette_map[] = {
    GB_PALETTE_DMG, GB_PALETTE_MGB0, GB_PALETTE_POCKET, GB_PALETTE_CGB,
};
#define GB_N_PALETTES ((int)(sizeof(s_palette_map) / sizeof(s_palette_map[0])))
#define GB_PAL_DMG  0
#define GB_PAL_AUTO 3

static const emu_option_t s_gb_options[] = {
    { "palette", N_("Paleta"), s_palette_values, GB_PAL_DMG },
};
/* A colour game brings its own colours: on the gbc table the option is
 * only for the Game Boy games played there. */
static const emu_option_t s_gbc_options[] = {
    { "palette", N_("Paleta (juegos de Game Boy)"), s_palette_values, GB_PAL_AUTO },
};

static void gbc_set_option(const char *key, int value)
{
    if (strcmp(key, "palette") == 0) {
        if (value < 0 || value >= GB_N_PALETTES) value = GB_PAL_DMG;
        s_opt_palette = value;
        if (s_loaded) gnuboy_set_palette(s_palette_map[value]);
    }
}

/* ---- little helpers ------------------------------------------------------ */

static uint32_t gbc_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void gbc_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static char *gbc_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* The board has no clock with a battery: until SNTP answers time() is
 * somewhere in 1970. Anything before 2020 is "not known". */
static int64_t gbc_now(void)
{
    int64_t t = (int64_t)time(NULL);
    return t >= 1577836800 ? t : 0;
}

/* ---- the cartridge's clock ----------------------------------------------- */

/* Moves the MBC3 clock on by 'secs' seconds, as the cartridge's own
 * crystal would have while the game was not running. */
static void gbc_rtc_advance(uint32_t secs)
{
    gb_rtc_t *r = &cart.rtc;
    if (r->flags & 0x40) return;            /* the game stopped the clock */

    uint32_t days = secs / 86400;
    uint32_t rest = secs % 86400 + (uint32_t)r->s + (uint32_t)r->m * 60 + (uint32_t)r->h * 3600;
    days += rest / 86400;
    rest %= 86400;
    r->h = (int)(rest / 3600);
    r->m = (int)(rest / 60 % 60);
    r->s = (int)(rest % 60);

    days += (uint32_t)r->d;
    if (days >= 512) {
        r->flags |= 0x80;                   /* the day counter's carry */
        days %= 512;
    }
    r->d = (int)days;
}

/* The bytes of the .sav before the clock: the RAM the header declares, as
 * other emulators write it. An MBC2 has 512 half-bytes inside the chip
 * (written as 512 bytes); a battery cartridge whose header says no RAM
 * gets gnuboy's 8 KB, so nothing the game writes is lost. */
static size_t gbc_sram_bytes(void)
{
    if (!cart.has_battery || !cart.rambanks) return 0;
    if (cart.mbc == MBC_MBC2) return 512;
    size_t n;
    switch (s_ram_code) {
    case 1: n = 2048; break;
    case 2: n = 8192; break;
    case 3: n = 32768; break;
    case 4: n = 131072; break;
    case 5: n = 65536; break;
    default: n = cart.has_rtc ? 0 : 8192; break;
    }
    size_t have = (size_t)cart.ramsize * 8192;
    return n < have ? n : have;
}

static bool gbc_has_save(void)
{
    return cart.has_battery && (gbc_sram_bytes() > 0 || cart.has_rtc);
}

static void gbc_rtc_from_sav(const uint8_t *t, size_t len)
{
    uint32_t v[10];
    for (int i = 0; i < 10; i++) v[i] = gbc_le32(t + 4 * i);
    uint32_t days = v[3], ctrl = v[4];

    gb_rtc_t *r = &cart.rtc;
    r->s = (int)(v[0] % 60);
    r->m = (int)(v[1] % 60);
    r->h = (int)(v[2] % 24);
    /* VBA-M and mGBA keep the low 8 bits of the day here and the ninth in
     * the control word; gnuboy kept the whole count. */
    r->d = (int)(days > 255 ? days % 512 : ((days & 0xFF) | ((ctrl & 1) << 8)));
    r->flags = (int)(ctrl & 0xC0);
    for (int i = 0; i < 5; i++) r->regs[i] = (int)(v[5 + i] & 0xFF);
    r->ticks = 0;

    int64_t stamp = (int64_t)gbc_le32(t + 40);
    if (len >= GB_RTC_LEN) stamp |= (int64_t)((uint64_t)gbc_le32(t + 44) << 32);
    int64_t now = gbc_now();
    /* Retro-Go wrote a made-up stamp in 2030: it is in the future and
     * skipped. More than ten years is a bad clock somewhere. */
    if (stamp >= 946684800 && now > stamp && now - stamp < 10LL * 366 * 86400) {
        uint32_t gone = (uint32_t)(now - stamp);
        gbc_rtc_advance(gone);
        s_host->log("gb: clock moved on %u s since the save", (unsigned)gone);
    }
}

static void gbc_rtc_to_sav(uint8_t *t)
{
    const gb_rtc_t *r = &cart.rtc;
    uint32_t v[10] = {
        (uint32_t)r->s, (uint32_t)r->m, (uint32_t)r->h, (uint32_t)r->d & 0xFF,
        ((uint32_t)r->flags & 0xC0) | (((uint32_t)r->d >> 8) & 1),
        (uint32_t)r->regs[0] & 0xFF, (uint32_t)r->regs[1] & 0xFF, (uint32_t)r->regs[2] & 0xFF,
        (uint32_t)r->regs[3] & 0xFF, (uint32_t)r->regs[4] & 0xFF,
    };
    for (int i = 0; i < 10; i++) gbc_put_le32(t + 4 * i, v[i]);
    uint64_t now = (uint64_t)gbc_now();     /* 0: unknown, no catching up */
    gbc_put_le32(t + 40, (uint32_t)now);
    gbc_put_le32(t + 44, (uint32_t)(now >> 32));
}

/* ---- the battery save ---------------------------------------------------- */

static void gbc_sram_open(const char *path)
{
    if (!path || !gbc_has_save()) return;
    s_save_path = gbc_strdup(path);
    if (!s_save_path) {
        s_host->log("gb: no memory for the battery save");
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return;                         /* a first game: nothing yet */

    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) size = 0;

    size_t ram = gbc_sram_bytes();
    size_t want = (size_t)size < ram ? (size_t)size : ram;
    size_t got = want ? fread(cart.rambanks, 1, want, f) : 0;

    bool rtc = false;
    if (cart.has_rtc && (size_t)size >= ram + GB_RTC_LEN_OLD) {
        uint8_t t[GB_RTC_LEN] = {0};
        size_t tl = (size_t)size - ram >= GB_RTC_LEN ? GB_RTC_LEN : GB_RTC_LEN_OLD;
        if (fseek(f, (long)ram, SEEK_SET) == 0 && fread(t, 1, tl, f) == tl) {
            gbc_rtc_from_sav(t, tl);
            rtc = true;
        }
    }
    fclose(f);
    cart.sram_dirty = 0;
    cart.rtc.dirty = 0;
    s_host->log("gb: battery save read (%u bytes%s)", (unsigned)got, rtc ? ", clock" : "");
}

/* Writes the .sav when the game changed its RAM or set its clock since the
 * last time, and once per session for a game with a clock, so the file
 * carries the time it was written. To a .tmp first: a card pulled half way
 * through leaves the old save. */
static bool gbc_sram_flush(void)
{
    if (!s_loaded || !s_save_path) return true;
    bool need = cart.sram_dirty != 0 || cart.rtc.dirty || (cart.has_rtc && !s_rtc_saved);
    if (!need) return true;

    size_t ram = gbc_sram_bytes();
    uint8_t t[GB_RTC_LEN];
    if (cart.has_rtc) gbc_rtc_to_sav(t);

    char tmp[340];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", s_save_path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        s_host->log("gb: cannot write %s", tmp);
        return false;
    }
    bool ok = !ram || fwrite(cart.rambanks, 1, ram, f) == ram;
    if (ok && cart.has_rtc) ok = fwrite(t, 1, GB_RTC_LEN, f) == GB_RTC_LEN;
    if (fclose(f) != 0) ok = false;
    if (ok) {
        remove(s_save_path);                /* FAT does not rename over a file */
        ok = rename(tmp, s_save_path) == 0;
    }
    if (!ok) {
        remove(tmp);
        s_host->log("gb: writing %s failed", s_save_path);
        return false;
    }
    cart.sram_dirty = 0;
    cart.rtc.dirty = 0;
    s_rtc_saved = true;
    return true;
}

/* ---- load and unload ----------------------------------------------------- */

static void gbc_audio_cb(void *buffer, size_t length)
{
    /* 'length' counts int16 values, two a stereo frame. */
    if (s_host && length >= 2) s_host->audio((const int16_t *)buffer, (int)(length / 2));
}

static void gbc_free_all(void)
{
    if (s_inited) {
        gnuboy_free_rom();                  /* the bank table and the cartridge's RAM */
        gnuboy_free_bios();
        free(GB.rambanks);
        free(GB.vbanks);
        memset(&GB, 0, sizeof(GB));
        memset(&cart, 0, sizeof(cart));
    }
    s_inited = false;
    s_loaded = false;
    free(s_rom);
    free(s_frame);
    free(s_audio);
    free(s_save_path);
    s_rom = NULL;
    s_rom_size = 0;
    s_frame = NULL;
    s_audio = NULL;
    s_save_path = NULL;
    s_ram_code = 0;
    s_cart_type = 0;
    s_rtc_saved = false;
}

static void gbc_unload(void)
{
    gbc_free_all();
    s_host = NULL;
}

/* Cartridges gnuboy has no banking for: they would start and then run
 * into the wrong code. */
static bool gbc_type_unsupported(int type)
{
    return (type >= 0x0B && type <= 0x0D)   /* MMM01            */
        || type == 0x20                     /* MBC6             */
        || type == 0x22                     /* MBC7 (tilt)      */
        || type == 0xFC                     /* Pocket Camera    */
        || type == 0xFD;                    /* Bandai TAMA5     */
}

/* Reads the whole game into memory, padded with 0xFF to the size its
 * header declares (what gnuboy maps its banks from). */
static bool gbc_read_rom(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return false;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    uint8_t h[GB_ROM_MIN];
    if (size < (long)GB_ROM_MIN || fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, sizeof(h), f) != sizeof(h)) {
        fclose(f);
        snprintf(err, errlen, "No es un juego de Game Boy");
        return false;
    }
    if ((unsigned long)size > GB_ROM_MAX) {
        fclose(f);
        snprintf(err, errlen, "El archivo es demasiado grande");
        return false;
    }

    /* The banks the header declares, as gnuboy_load_rom() reads them. */
    int code = h[0x148];
    size_t banks;
    if (code < 9) banks = (size_t)2 << code;
    else if (code > 0x51 && code < 0x55) banks = 128;
    else {
        fclose(f);
        snprintf(err, errlen, "No es un juego de Game Boy");
        return false;
    }
    s_cart_type = h[0x147];
    if (gbc_type_unsupported(s_cart_type)) {
        fclose(f);
        s_host->log("gb: cartridge type 0x%02X has no support", s_cart_type);
        snprintf(err, errlen, "Este tipo de cartucho no está soportado");
        return false;
    }

    /* The boot ROM refuses a cartridge whose header checksum is wrong;
     * some homebrew never set it, so it is only told. */
    uint8_t sum = 0;
    for (int i = 0x134; i <= 0x14C; i++) sum = (uint8_t)(sum - h[i] - 1);
    if (sum != h[0x14D]) s_host->log("gb: header checksum is wrong (%02X, not %02X)", h[0x14D], sum);

    size_t alloc = banks * GB_BANK;
    if ((size_t)size > alloc)
        s_host->log("gb: file is %ld bytes, the header says %u: the rest is not used",
                    size, (unsigned)alloc);
    s_rom = malloc(alloc);
    if (!s_rom) {
        fclose(f);
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    size_t want = (size_t)size < alloc ? (size_t)size : alloc;
    bool ok = fseek(f, 0, SEEK_SET) == 0 && fread(s_rom, 1, want, f) == want;
    fclose(f);
    if (!ok) {
        snprintf(err, errlen, "No se pudo leer el juego");
        return false;
    }
    if (want < alloc) memset(s_rom + want, 0xFF, alloc - want);
    s_rom_size = alloc;
    s_ram_code = h[0x149];
    return true;
}

static bool gbc_load(const emu_host_t *hst, const emu_load_t *req, char *err, size_t errlen,
                     int default_palette)
{
    gbc_free_all();
    s_host = hst;
    s_log_count = 0;
    s_opt_palette = default_palette;        /* until the app's set_option() */

    if (!gbc_read_rom(req->rom, err, errlen)) {
        gbc_free_all();
        return false;
    }

    s_frame = calloc(GB_W * GB_H, sizeof(uint16_t));
    s_audio = calloc(GB_AUDIO_LEN, sizeof(int16_t));
    if (!s_frame || !s_audio) {
        gbc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }

    s_inited = true;
    if (gnuboy_init(GB_SAMPLE_RATE, GB_AUDIO_STEREO_S16, GB_PIXEL_565_LE, NULL, gbc_audio_cb) < 0) {
        gbc_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    gnuboy_set_framebuffer(s_frame);
    gnuboy_set_soundbuffer(s_audio, GB_AUDIO_LEN);

    int ret = gnuboy_load_rom(s_rom, s_rom_size);
    if (ret < 0) {
        gbc_free_all();
        if (ret == -3) snprintf(err, errlen, "No hay memoria");
        else snprintf(err, errlen, "No es un juego de Game Boy");
        return false;
    }

    gnuboy_set_palette(s_palette_map[s_opt_palette]);
    gnuboy_reset(true);
    s_loaded = true;
    gbc_sram_open(req->save);

    static const char *const mbc[] = {
        "none", "MBC1", "MBC2", "MBC3", "MBC5", "MBC6", "MBC7", "HuC1", "HuC3", "MMM01",
    };
    hst->log("gb: '%s', %s, type 0x%02X (%s%s%s), ROM %uK, RAM %uK",
             cart.name, gnuboy_get_hwtype() == GB_HW_CGB ? "GBC" : "GB", s_cart_type,
             cart.mbc >= 0 && cart.mbc < 10 ? mbc[cart.mbc] : "?",
             cart.has_battery ? ", battery" : "", cart.has_rtc ? ", clock" : "",
             (unsigned)(cart.romsize * 16), (unsigned)(gbc_sram_bytes() / 1024));
    return true;
}

static bool gbc_load_gb(const emu_host_t *hst, const emu_load_t *req, char *err, size_t errlen)
{
    return gbc_load(hst, req, err, errlen, GB_PAL_DMG);
}

static bool gbc_load_gbc(const emu_host_t *hst, const emu_load_t *req, char *err, size_t errlen)
{
    return gbc_load(hst, req, err, errlen, GB_PAL_AUTO);
}

/* ---- running ------------------------------------------------------------- */

static int gbc_pad_bits(uint32_t b)
{
    int p = 0;
    if (b & EMU_BTN_RIGHT)  p |= GB_PAD_RIGHT;
    if (b & EMU_BTN_LEFT)   p |= GB_PAD_LEFT;
    if (b & EMU_BTN_UP)     p |= GB_PAD_UP;
    if (b & EMU_BTN_DOWN)   p |= GB_PAD_DOWN;
    if (b & EMU_BTN_A)      p |= GB_PAD_A;
    if (b & EMU_BTN_B)      p |= GB_PAD_B;
    if (b & EMU_BTN_SELECT) p |= GB_PAD_SELECT;
    if (b & EMU_BTN_START)  p |= GB_PAD_START;
    /* The pad cannot press both ways at once; some games break if it does. */
    if ((p & (GB_PAD_LEFT | GB_PAD_RIGHT)) == (GB_PAD_LEFT | GB_PAD_RIGHT))
        p &= ~(GB_PAD_LEFT | GB_PAD_RIGHT);
    if ((p & (GB_PAD_UP | GB_PAD_DOWN)) == (GB_PAD_UP | GB_PAD_DOWN))
        p &= ~(GB_PAD_UP | GB_PAD_DOWN);
    return p;
}

static void gbc_run(const uint32_t *pads, bool draw)
{
    if (!s_loaded) return;
    gnuboy_set_pad(pads ? gbc_pad_bits(pads[0]) : 0);
    gnuboy_run(draw);
    /* With the screen off the picture is the last one drawn, as most
     * emulators leave it. */
    if (draw) s_host->video(s_frame, GB_W, GB_H, GB_W * (int)sizeof(uint16_t));
}

/* A reset is the power switch: the cartridge's RAM and clock live on
 * their own battery and survive it (gnuboy's hard reset clears both). */
static void gbc_reset(void)
{
    if (!s_loaded) return;
    size_t ram = (size_t)cart.ramsize * 8192;
    uint8_t *keep = ram ? malloc(ram) : NULL;
    if (keep) memcpy(keep, cart.rambanks, ram);
    gb_rtc_t rtc = cart.rtc;
    unsigned dirty = cart.sram_dirty;

    gnuboy_reset(true);

    if (keep) {
        memcpy(cart.rambanks, keep, ram);
        free(keep);
    }
    cart.rtc = rtc;
    cart.sram_dirty = dirty;
}

static float gbc_fps(void)
{
    return (float)GB_DSC_PER_SEC / (float)GB_FRAME_DSC;    /* 59.7275 */
}

static int gbc_sample_rate(void)
{
    return GB_SAMPLE_RATE;
}

/* ---- save states --------------------------------------------------------- */

static long gbc_state_size(void)
{
    bool cgb = gnuboy_get_hwtype() == GB_HW_CGB;
    return (long)GB_STATE_BLOCK * (1 + (cgb ? 8 : 2) + (cgb ? 4 : 2) + cart.ramsize * 2);
}

static bool gbc_state_save(const char *path)
{
    if (!s_loaded || !path) return false;
    char tmp[340];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;
    bool ok = gnuboy_save_state(tmp) == 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

/* gnuboy reads a state straight into the console block by block, so a
 * short or foreign file would leave the game half restored: the file's
 * size (it depends on GB or GBC and on the cartridge's RAM) and its first
 * tag, "GbSs" with the format's version, are checked first. */
static bool gbc_state_check(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[8];
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    bool ok = size == gbc_state_size() && fseek(f, 0, SEEK_SET) == 0
              && fread(h, 1, sizeof(h), f) == sizeof(h)
              && memcmp(h, "GbSs", 4) == 0 && gbc_le32(h + 4) == GB_STATE_VERSION;
    fclose(f);
    return ok;
}

static bool gbc_state_load(const char *path)
{
    if (!s_loaded || !path) return false;
    if (!gbc_state_check(path)) {
        s_host->log("gb: %s is not a state for this game", path);
        return false;
    }
    /* The cartridge's clock is real time: going back to a state does not
     * take it back with the rest (the latched registers do go back). */
    gb_rtc_t rtc = cart.rtc;
    if (gnuboy_load_state(path) != 0) {
        s_host->log("gb: state load failed half way, game reset");
        gbc_reset();
        return false;
    }
    if (cart.has_rtc) {
        cart.rtc.d = rtc.d;
        cart.rtc.h = rtc.h;
        cart.rtc.m = rtc.m;
        cart.rtc.s = rtc.s;
        cart.rtc.flags = rtc.flags;
        cart.rtc.ticks = rtc.ticks;
        cart.rtc.dirty = rtc.dirty;
    }
    /* The state brought its own battery RAM: it goes to the .sav. */
    if (cart.has_battery && cart.ramsize > 0) cart.sram_dirty = (1u << cart.ramsize) - 1;
    return true;
}

/* ---- stats --------------------------------------------------------------- */

static const char *gbc_stats(void)
{
    if (!s_loaded) return NULL;
    static const char *const mbc[] = {
        "no MBC", "MBC1", "MBC2", "MBC3", "MBC5", "MBC6", "MBC7", "HuC1", "HuC3", "MMM01",
    };
    snprintf(s_stats, sizeof(s_stats), "gb: %s, %s%s%s, 59.73 fps",
             gnuboy_get_hwtype() == GB_HW_CGB ? "GBC" : "GB",
             cart.mbc >= 0 && cart.mbc < 10 ? mbc[cart.mbc] : "?",
             cart.has_battery ? ", battery" : "",
             cart.has_rtc ? ", clock" : "");
    return s_stats;
}

/* ---- the tables ---------------------------------------------------------- */

#define GB_BUTTONS (EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_SELECT | EMU_BTN_START)

const emu_core_t emu_core_gb = {
    .id           = "gb",
    .name         = "Game Boy",
    .short_name   = "GB",
    .exts         = ".gb",
    .max_w        = GB_W,
    .max_h        = GB_H,
    .pixel_aspect = 0,
    .players      = 1,
    .buttons      = GB_BUTTONS,
    .face         = EMU_FACE_DIAG2,
    .labels       = NULL,
    .options      = s_gb_options,
    .n_options    = (int)(sizeof(s_gb_options) / sizeof(s_gb_options[0])),
    .load         = gbc_load_gb,
    .unload       = gbc_unload,
    .reset        = gbc_reset,
    .run          = gbc_run,
    .fps          = gbc_fps,
    .sample_rate  = gbc_sample_rate,
    .state_save   = gbc_state_save,
    .state_load   = gbc_state_load,
    .sram_flush   = gbc_sram_flush,
    .set_option   = gbc_set_option,
    .stats        = gbc_stats,
};

const emu_core_t emu_core_gbc = {
    .id           = "gbc",
    .name         = "Game Boy Color",
    .short_name   = "GBC",
    .exts         = ".gbc .cgb .gb",
    .max_w        = GB_W,
    .max_h        = GB_H,
    .pixel_aspect = 0,
    .players      = 1,
    .buttons      = GB_BUTTONS,
    .face         = EMU_FACE_DIAG2,
    .labels       = NULL,
    .options      = s_gbc_options,
    .n_options    = (int)(sizeof(s_gbc_options) / sizeof(s_gbc_options[0])),
    .load         = gbc_load_gbc,
    .unload       = gbc_unload,
    .reset        = gbc_reset,
    .run          = gbc_run,
    .fps          = gbc_fps,
    .sample_rate  = gbc_sample_rate,
    .state_save   = gbc_state_save,
    .state_load   = gbc_state_load,
    .sram_flush   = gbc_sram_flush,
    .set_option   = gbc_set_option,
    .stats        = gbc_stats,
};
