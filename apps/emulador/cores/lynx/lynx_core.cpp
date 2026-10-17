/*
 * P4OS - Emulador, Atari Lynx core: Handy (as Retro-Go carries it, with its
 * high-level BIOS) behind the emu_core_t table.
 *
 * Handy is one CSystem object and a few globals. Mikie draws each line
 * straight into an RGB565 buffer of ours (rotated when asked), and leaves
 * its sound, 32 kHz stereo, in another.
 *
 * Time. A Lynx game sets its own frame rate with Mikie's timers (75 Hz is
 * common, 60 and 50 exist), so one run() is a fixed slice of time, not one
 * of the game's frames: 266,500 cycles of the 16 MHz clock, which is
 * exactly 533 sound frames at 32 kHz, so fps() reports 32000 / 533 = 60.04
 * and sound and picture never drift. run() shows the last frame the game
 * finished; a 75 Hz game shows four of its five frames.
 *
 * The BIOS. Retro-Go's Handy replaces the Lynx's boot ROM with a high-level
 * version of its three routines (decrypting the cartridge's loader): no
 * lynxboot.img is needed, and none is read.
 *
 * Also here: the battery save (the cartridge's EEPROM, raw, as other
 * emulators keep it), checks on the file before Handy trusts its header,
 * the rotation of sideways games, and complete save states (Handy's own
 * plus the screen's position and buffers, which it leaves out).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_core.h"
#include "handy.h"
#include "rg_system.h"

#ifndef N_
#define N_(s) s
#endif

#define LYNX_RATE       32000
#define LYNX_SPF        533                             /* sound frames a run   */
#define LYNX_SLICE      (LYNX_SPF * HANDY_AUDIO_SAMPLE_PERIOD)  /* 266,500 cycles */
#define LYNX_ROM_MAX    (1024 * 1024)
#define LYNX_PITCH      HANDY_SCREEN_WIDTH              /* pixels a buffer row  */
#define LYNX_BUF_PX     (HANDY_SCREEN_WIDTH * HANDY_SCREEN_WIDTH)

static const emu_host_t *s_host;
static CSystem *s_sys;
static uint8_t *s_rom;          /* the file, kept for reset() */
static uint32_t s_rom_size;
static uint16_t *s_buf[2];      /* Mikie draws into s_buf[s_cur]           */
static int s_cur;
static int s_done = -1;         /* the last finished frame's buffer, or -1 */
static uint32_t s_frames;       /* frames finished, for the stats          */
static uint32_t s_frames_seen, s_stats_runs;
static ULONG s_debt;            /* cycles the last slice ran over          */
static SWORD *s_audio;
static int s_rot;               /* MIKIE_NO_ROTATE / _L / _R in use        */
static uint32_t s_crc;

static char *s_save_path;
static uint8_t *s_eeprom_copy;
static int s_eeprom_size;

static int s_opt_rot;           /* 0 auto, 1 none, 2 left, 3 right */
static char s_stats[96];

/* ---- options ----------------------------------------------------------- */

static const char *const s_rot_values[] = {
    N_("Automática"), N_("No"), N_("Izquierda"), N_("Derecha"), NULL,
};
static const emu_option_t s_options[] = {
    { "rotation", N_("Girar la pantalla"), s_rot_values, 0 },
};
#define LYNX_N_OPTIONS ((int)(sizeof(s_options) / sizeof(s_options[0])))

/* The Lynx's own names: OPTION 1 and 2 on the shoulders, PAUSE on START. */
static const char *const s_labels[EMU_BTN_COUNT] = {
    /* UP DOWN LEFT RIGHT A B X Y */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* L */ "OPTION 1", /* R */ "OPTION 2", /* SELECT */ NULL, /* START */ "PAUSE",
};

/* ---- the file ---------------------------------------------------------- */

static bool lynx_bank_ok(UWORD pages, bool zero_ok)
{
    return (zero_ok && pages == 0) || pages == 0x100 || pages == 0x200 || pages == 0x400 || pages == 0x800;
}

/* What Handy would make of the file, checked before it does: its banks
 * must be sizes it knows (otherwise it allocates by an uninitialised
 * mask), and a homebrew's load must fit the 64 KB of RAM. */
static bool lynx_file_ok(const uint8_t *d, uint32_t n)
{
    if (n < 64) return false;
    if (memcmp(d + 6, "BS93", 4) == 0) {
        uint32_t addr = (uint32_t)(d[2] << 8 | d[3]);  /* big-endian, as Handy reads it */
        uint32_t size = (uint32_t)(d[4] << 8 | d[5]);
        if (size > n) size = n;
        return addr >= 10 && addr - 10 + size <= RAM_SIZE;
    }
    if (memcmp(d, "LYNX", 4) == 0) {
        UWORD b0 = (UWORD)(d[4] | d[5] << 8), b1 = (UWORD)(d[6] | d[7] << 8);
        return lynx_bank_ok(b0, false) && lynx_bank_ok(b1, true);
    }
    /* a raw image: Handy takes bank 0's page size from its length */
    return lynx_bank_ok((UWORD)(n >> 8), false) && (n & 0xff) == 0;
}

/* ---- rotation ---------------------------------------------------------- */

/* Retro-Go's list: games whose header does not say they are sideways. */
static int lynx_auto_rotation(void)
{
    switch (s_sys->mCart->CRC32()) {
    case 0x97501709: /* Centipede   */
    case 0x0271B6E9: /* Lexis       */
    case 0x006FD398: /* NFL Football */
    case 0xBCD10C3A: /* Raiden      */
        return MIKIE_ROTATE_L;
    case 0x7F0EC7AD: /* Gauntlet    */
    case 0xAC564BAA: /* Gauntlet - The Third Encounter */
    case 0xA53649F1: /* Klax        */
        return MIKIE_ROTATE_R;
    }
    ULONG r = s_sys->mCart->CartGetRotate();
    if (r == CART_ROTATE_LEFT) return MIKIE_ROTATE_L;
    if (r == CART_ROTATE_RIGHT) return MIKIE_ROTATE_R;
    return MIKIE_NO_ROTATE;
}

static void lynx_apply_rotation(void)
{
    if (!s_sys) return;
    int rot = s_opt_rot == 1 ? MIKIE_NO_ROTATE
            : s_opt_rot == 2 ? MIKIE_ROTATE_L
            : s_opt_rot == 3 ? MIKIE_ROTATE_R
            : lynx_auto_rotation();
    if (rot != s_rot) {
        /* the other shape leaves the old picture's edges behind */
        memset(s_buf[0], 0, LYNX_BUF_PX * sizeof(uint16_t));
        memset(s_buf[1], 0, LYNX_BUF_PX * sizeof(uint16_t));
    }
    s_rot = rot;
    s_sys->mMikie->SetRotation((UBYTE)rot);
}

/* ---- the battery save -------------------------------------------------- */

static void lynx_sram_open(const char *path)
{
    if (!path || !s_sys->mEEPROM->Available()) return;
    int size = s_sys->mEEPROM->Size();
    size_t plen = strlen(path) + 1;
    s_save_path = (char *)malloc(plen);
    s_eeprom_copy = (uint8_t *)malloc((size_t)size);
    if (!s_save_path || !s_eeprom_copy) {
        free(s_save_path);
        free(s_eeprom_copy);
        s_save_path = NULL;
        s_eeprom_copy = NULL;
        return;
    }
    memcpy(s_save_path, path, plen);
    s_eeprom_size = size;
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t got = fread(s_sys->mEEPROM->Data(), 1, (size_t)size, f);
        fclose(f);
        s_host->log("lynx: EEPROM save read (%u bytes)", (unsigned)got);
    }
    memcpy(s_eeprom_copy, s_sys->mEEPROM->Data(), (size_t)size);
}

static bool lynx_sram_flush(void)
{
    if (!s_sys || !s_eeprom_copy) return true;
    const uint8_t *now = s_sys->mEEPROM->Data();
    if (memcmp(now, s_eeprom_copy, (size_t)s_eeprom_size) == 0) return true;
    FILE *f = fopen(s_save_path, "wb");
    if (!f) {
        s_host->log("lynx: cannot write %s", s_save_path);
        return false;
    }
    bool ok = fwrite(now, 1, (size_t)s_eeprom_size, f) == (size_t)s_eeprom_size;
    if (fclose(f) != 0) ok = false;
    if (ok) memcpy(s_eeprom_copy, now, (size_t)s_eeprom_size);
    else s_host->log("lynx: writing %s failed", s_save_path);
    return ok;
}

/* ---- load and unload --------------------------------------------------- */

static void lynx_free_all(void)
{
    delete s_sys;
    s_sys = NULL;
    gPrimaryFrameBuffer = NULL;
    gAudioBuffer = NULL;
    gAudioBufferPointer = 0;
    free(s_rom);
    free(s_buf[0]);
    free(s_buf[1]);
    free(s_audio);
    free(s_save_path);
    free(s_eeprom_copy);
    s_rom = NULL;
    s_rom_size = 0;
    s_buf[0] = s_buf[1] = NULL;
    s_audio = NULL;
    s_save_path = NULL;
    s_eeprom_copy = NULL;
    s_eeprom_size = 0;
    s_cur = 0;
    s_done = -1;
    s_frames = s_frames_seen = s_stats_runs = 0;
    s_debt = 0;
    s_rot = MIKIE_NO_ROTATE;
}

static void lynx_unload(void)
{
    lynx_free_all();
    lynx_shim_set_log(NULL);
    s_host = NULL;
}

/* Handy's console, made from the kept file, on our buffers. */
static bool lynx_make_system(void)
{
    s_sys = new CSystem(s_rom, s_rom_size, MIKIE_PIXEL_FORMAT_16BPP_565, LYNX_RATE);
    if (s_sys->mFileType == HANDY_FILETYPE_ILLEGAL) return false;
    s_cur = 0;
    s_done = -1;
    s_debt = 0;
    gPrimaryFrameBuffer = (UBYTE *)s_buf[0];
    gAudioBuffer = s_audio;
    gAudioBufferPointer = 0;
    gAudioEnabled = TRUE;
    s_rot = -1;
    lynx_apply_rotation();
    return true;
}

static bool lynx_load(const emu_host_t *host, const emu_load_t *req, char *err, size_t errlen)
{
    lynx_free_all();
    s_host = host;
    lynx_shim_set_log(host->log);

    FILE *f = fopen(req->rom, "rb");
    if (!f) {
        snprintf(err, errlen, "No se pudo abrir el archivo");
        return false;
    }
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n < 64 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, errlen, "No es un juego de Atari Lynx");
        return false;
    }
    if (n > LYNX_ROM_MAX) {
        fclose(f);
        snprintf(err, errlen, "El archivo es demasiado grande");
        return false;
    }
    s_rom = (uint8_t *)malloc((size_t)n);
    s_buf[0] = (uint16_t *)calloc(LYNX_BUF_PX, sizeof(uint16_t));
    s_buf[1] = (uint16_t *)calloc(LYNX_BUF_PX, sizeof(uint16_t));
    s_audio = (SWORD *)calloc(HANDY_AUDIO_BUFFER_LENGTH, sizeof(SWORD));
    if (!s_rom || !s_buf[0] || !s_buf[1] || !s_audio) {
        fclose(f);
        lynx_free_all();
        snprintf(err, errlen, "No hay memoria");
        return false;
    }
    bool got = fread(s_rom, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    s_rom_size = (uint32_t)n;
    if (!got) {
        lynx_free_all();
        snprintf(err, errlen, "No se pudo leer el archivo");
        return false;
    }
    if (!lynx_file_ok(s_rom, s_rom_size) || !lynx_make_system()) {
        lynx_free_all();
        snprintf(err, errlen, "No es un juego de Atari Lynx");
        return false;
    }
    s_crc = s_sys->mCart->CRC32();
    lynx_sram_open(req->save);

    const char *kind = s_sys->mFileType == HANDY_FILETYPE_HOMEBREW ? "homebrew (BS93)"
                     : s_sys->mFileType == HANDY_FILETYPE_LNX ? ".lnx" : "raw image";
    host->log("lynx: %s, %u KB, crc %08X, EEPROM %s, %s", kind,
              (unsigned)(s_rom_size / 1024), (unsigned)s_crc,
              s_sys->mEEPROM->Available() ? "yes" : "no",
              s_rot == MIKIE_ROTATE_L ? "turned left" : s_rot == MIKIE_ROTATE_R ? "turned right" : "upright");
    return true;
}

/* ---- running ----------------------------------------------------------- */

static void lynx_frame_done(void *ctx)
{
    (void)ctx;
    /* The frame Mikie just finished is shown; the next is drawn into the
     * other buffer (Mikie takes gPrimaryFrameBuffer when its screen DMA
     * starts, a few lines before the first visible one). */
    s_done = s_cur;
    s_cur ^= 1;
    gPrimaryFrameBuffer = (UBYTE *)s_buf[s_cur];
    s_frames++;
}

static ULONG lynx_buttons(uint32_t b)
{
    ULONG o = 0;
    /* the d-pad turns with the picture */
    bool up = b & EMU_BTN_UP, down = b & EMU_BTN_DOWN, left = b & EMU_BTN_LEFT, right = b & EMU_BTN_RIGHT;
    if (s_rot == MIKIE_ROTATE_L) {
        if (up) o |= BUTTON_RIGHT;
        if (down) o |= BUTTON_LEFT;
        if (left) o |= BUTTON_UP;
        if (right) o |= BUTTON_DOWN;
    } else if (s_rot == MIKIE_ROTATE_R) {
        if (up) o |= BUTTON_LEFT;
        if (down) o |= BUTTON_RIGHT;
        if (left) o |= BUTTON_DOWN;
        if (right) o |= BUTTON_UP;
    } else {
        if (up) o |= BUTTON_UP;
        if (down) o |= BUTTON_DOWN;
        if (left) o |= BUTTON_LEFT;
        if (right) o |= BUTTON_RIGHT;
    }
    if (b & EMU_BTN_A) o |= BUTTON_A;
    if (b & EMU_BTN_B) o |= BUTTON_B;
    if (b & EMU_BTN_L) o |= BUTTON_OPT1;
    if (b & EMU_BTN_R) o |= BUTTON_OPT2;
    if (b & EMU_BTN_START) o |= BUTTON_PAUSE;
    return o;
}

static void lynx_run(const uint32_t *pads, bool draw)
{
    if (!s_sys) return;
    s_sys->SetButtonData(lynx_buttons(pads ? pads[0] : 0));

    /* Lines are always drawn (a line is 80 bytes through the palette, and a
     * frame half drawn while skipping would show later); draw only says
     * whether the host gets the picture. */
    ULONG want = s_debt < LYNX_SLICE ? LYNX_SLICE - s_debt : 1;
    ULONG ran = s_sys->RunCycles(want, true, lynx_frame_done, NULL);
    s_debt = ran > want ? ran - want : 0;

    s_sys->FlushSound();
    int frames = (int)(gAudioBufferPointer / 2);
    if (frames > 0) s_host->audio(s_audio, frames);
    gAudioBufferPointer = 0;

    s_stats_runs++;
    if (draw && s_done >= 0) {
        bool turned = s_rot != MIKIE_NO_ROTATE;
        int w = turned ? HANDY_SCREEN_HEIGHT : HANDY_SCREEN_WIDTH;
        int h = turned ? HANDY_SCREEN_WIDTH : HANDY_SCREEN_HEIGHT;
        s_host->video(s_buf[s_done], w, h, LYNX_PITCH * (int)sizeof(uint16_t));
    }
}

/* The power switch. Handy's own Reset() is not trusted to put everything
 * back (Retro-Go makes a new console for it too): the console is made
 * again from the kept file. The cartridge's EEPROM keeps its contents, as
 * the real chip does. */
static void lynx_reset(void)
{
    if (!s_sys) return;
    uint8_t keep[2048];
    int size = s_sys->mEEPROM->Available() ? s_sys->mEEPROM->Size() : 0;
    if (size > (int)sizeof(keep)) size = sizeof(keep);
    if (size) memcpy(keep, s_sys->mEEPROM->Data(), (size_t)size);
    delete s_sys;
    s_sys = NULL;
    if (!lynx_make_system()) {
        s_host->log("lynx: the console could not be made again");
        delete s_sys;
        s_sys = NULL;
        return;
    }
    if (size) memcpy(s_sys->mEEPROM->Data(), keep, (size_t)size);
}

static float lynx_fps(void)
{
    return (float)LYNX_RATE / (float)LYNX_SPF;
}

static int lynx_sample_rate(void)
{
    return LYNX_RATE;
}

/* ---- save states --------------------------------------------------------
 *
 * "P4LNXST\0", a version, the payload's length and CRC-32, the cartridge's
 * CRC-32, then Handy's ContextSave() and a block of ours: the slice's
 * overrun, which buffer is being drawn and where, which one was finished
 * last, and both buffers. */

#define LYNX_ST_MAGIC   "P4LNXST"
#define LYNX_ST_VERSION 1u
#define LYNX_ST_HEAD    (8 + 4 + 4 + 4 + 4)

static bool lynx_context(LSS_FILE *fp, bool save)
{
    if (save) {
        if (!s_sys->ContextSave(fp)) return false;
        long off = s_sys->mMikie->GetDisplayOffset();
        int32_t v[4] = { (int32_t)s_debt, s_cur, s_done, (int32_t)off };
        return lss_write(v, sizeof(v), 1, fp)
            && lss_write(s_buf[0], LYNX_BUF_PX * sizeof(uint16_t), 1, fp)
            && lss_write(s_buf[1], LYNX_BUF_PX * sizeof(uint16_t), 1, fp);
    }
    if (!s_sys->ContextLoad(fp)) return false;
    int32_t v[4];
    if (!lss_read(v, sizeof(v), 1, fp)) return false;
    if (v[1] < 0 || v[1] > 1 || v[2] < -1 || v[2] > 1 || v[3] < -1
        || v[3] >= (int32_t)(LYNX_BUF_PX * sizeof(uint16_t)))
        return false;
    if (!lss_read(s_buf[0], LYNX_BUF_PX * sizeof(uint16_t), 1, fp)
        || !lss_read(s_buf[1], LYNX_BUF_PX * sizeof(uint16_t), 1, fp))
        return false;
    s_debt = (ULONG)v[0];
    s_cur = v[1];
    s_done = v[2];
    gPrimaryFrameBuffer = (UBYTE *)s_buf[s_cur];
    s_sys->mMikie->SetDisplayOffset(v[3]);
    gAudioBufferPointer = 0;
    return true;
}

static uint8_t *lynx_take(uint32_t *size)
{
    LSS_FILE count = { NULL, 0, 0xffffffff };
    lynx_context(&count, true);
    uint8_t *buf = (uint8_t *)malloc(count.index);
    if (!buf) return NULL;
    LSS_FILE out = { buf, 0, count.index };
    if (!lynx_context(&out, true) || out.index != count.index) {
        free(buf);
        return NULL;
    }
    *size = out.index;
    return buf;
}

static bool lynx_give(uint8_t *buf, uint32_t size)
{
    LSS_FILE in = { buf, 0, size };
    return lynx_context(&in, false) && in.index == size;
}

static uint32_t lynx_crc(const uint8_t *p, size_t n)
{
    return rg_crc32(0, p, n);
}

static void lynx_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t lynx_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool lynx_state_save(const char *path)
{
    if (!s_sys) return false;
    uint32_t n = 0;
    uint8_t *data = lynx_take(&n);
    if (!data) return false;
    uint8_t head[LYNX_ST_HEAD];
    memcpy(head, LYNX_ST_MAGIC, 8);
    lynx_put32(head + 8, LYNX_ST_VERSION);
    lynx_put32(head + 12, n);
    lynx_put32(head + 16, lynx_crc(data, n));
    lynx_put32(head + 20, s_crc);

    size_t plen = strlen(path);
    char *tmp = (char *)malloc(plen + 5);
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
    if (!ok) s_host->log("lynx: writing the state %s failed", path);
    free(tmp);
    free(data);
    return ok;
}

static bool lynx_state_load(const char *path)
{
    if (!s_sys) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    uint8_t *buf = NULL;
    bool ok = n > LYNX_ST_HEAD && n < 4 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0
           && (buf = (uint8_t *)malloc((size_t)n)) != NULL
           && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    uint32_t len = 0;
    if (ok) {
        len = lynx_get32(buf + 12);
        ok = memcmp(buf, LYNX_ST_MAGIC, 8) == 0
          && lynx_get32(buf + 8) == LYNX_ST_VERSION
          && len == (uint32_t)(n - LYNX_ST_HEAD)
          && lynx_crc(buf + LYNX_ST_HEAD, len) == lynx_get32(buf + 16)
          && lynx_get32(buf + 20) == s_crc;
    }
    if (!ok) {
        free(buf);
        s_host->log("lynx: %s is not a state for this game", path);
        return false;
    }
    /* Handy loads device by device into the running console: what it is
     * now goes back if the state stops half way. */
    uint32_t bn = 0;
    uint8_t *backup = lynx_take(&bn);
    if (!backup) {
        free(buf);
        return false;
    }
    ok = lynx_give(buf + LYNX_ST_HEAD, len);
    free(buf);
    if (!ok) {
        s_host->log("lynx: Handy refused the state");
        if (!lynx_give(backup, bn)) lynx_reset();
    }
    free(backup);
    return ok;
}

/* ---- options and stats ------------------------------------------------- */

static void lynx_set_option(const char *key, int value)
{
    if (strcmp(key, "rotation") == 0) {
        s_opt_rot = value >= 0 && value <= 3 ? value : 0;
        lynx_apply_rotation();
    }
}

static const char *lynx_stats(void)
{
    if (!s_sys) return NULL;
    /* the game's own frame rate, from the frames finished per run */
    uint32_t frames = s_frames - s_frames_seen;
    uint32_t runs = s_stats_runs;
    s_frames_seen = s_frames;
    s_stats_runs = 0;
    int hz100 = runs ? (int)((frames * 6004u) / runs) : 0;
    snprintf(s_stats, sizeof(s_stats), "lynx: crc %08X, game at %d.%02d Hz%s%s",
             (unsigned)s_crc, hz100 / 100, hz100 % 100,
             s_rot != MIKIE_NO_ROTATE ? ", rotated" : "", s_eeprom_copy ? ", EEPROM" : "");
    return s_stats;
}

extern "C" const emu_core_t emu_core_lynx = {
    .id = "lynx",
    .name = "Atari Lynx",
    .short_name = "Lynx",
    .exts = ".lnx .lyx .o",
    .max_w = HANDY_SCREEN_WIDTH,
    .max_h = HANDY_SCREEN_WIDTH,    /* rotated: 102 x 160 */
    .pixel_aspect = 0.0f,           /* the LCD's pixels are square */
    .players = 1,
    .buttons = EMU_BTN_DPAD | EMU_BTN_A | EMU_BTN_B | EMU_BTN_L | EMU_BTN_R | EMU_BTN_START,
    .face = EMU_FACE_DIAG2,
    .labels = s_labels,
    .options = s_options,
    .n_options = LYNX_N_OPTIONS,
    .load = lynx_load,
    .unload = lynx_unload,
    .reset = lynx_reset,
    .run = lynx_run,
    .fps = lynx_fps,
    .sample_rate = lynx_sample_rate,
    .state_save = lynx_state_save,
    .state_load = lynx_state_load,
    .sram_flush = lynx_sram_flush,
    .set_option = lynx_set_option,
    .stats = lynx_stats,
};
