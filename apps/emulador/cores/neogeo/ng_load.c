/*
 * P4OS - Emulador, Neo Geo core: from the zip to a console ready to start.
 *
 * Takes the place of the loader in gngeo's roms.c (dr_load_roms,
 * dr_load_bios, dr_load_game) as RetroESP32-P4 rewrote it, keeping its
 * design: the small ROMs (program, sound program, text layer) are read
 * into memory, and the big ones are read into memory when they fit or
 * streamed from flat files on the card through caches in PSRAM when they
 * do not:
 *
 *   <folder of the zip>/<game>/<game>.ctile   the sprite tiles, converted
 *                                             to gngeo's 4bpp layout
 *                              <game>.cusage  "CU09" + 2 bits a tile
 *                              <game>.vroma   ADPCM-A samples, flat
 *                              <game>.vromb   ADPCM-B samples, if separate
 *                              <game>.sfix    text layer (CMC games only)
 *
 * The same files and layout as RetroESP32-P4's gen_cache.py writes, so a
 * cache made there on a computer works here. Unlike RetroESP32 (where a
 * missing cache is made on the device with a read-modify-write per 4 KB),
 * the device makes it in one pass: the two halves of each sprite ROM pair
 * are inflated side by side, interleaved, converted and written in order.
 * Games whose sprites are encrypted (CMC42/CMC50, from King of Fighters
 * '99 on) need the whole sprite ROM in memory to be decrypted, which no
 * such game's fits in: they need a cache made on a computer.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "gngeo/emu.h"     /* gngeo's (main/ has an emu.h too) */
#include "memory.h"
#include "roms.h"
#include "video.h"
#include "transpack.h"
#include "adpcm_cache.h"
#include "gnutil.h"
#include "ym2610/ym2610.h"

#include "ng_core.h"

/* the firmware's (aos_hal.h), for the log when the PSRAM runs out */
void aos_hal_heap_info(uint32_t *free_internal, uint32_t *free_psram);
#include "ng_drivers.h"
#include "ng_zip.h"

/* gngeo, kept in roms.c / video.c */
int init_roms(GAME_ROMS *r);
int game_needs_gfx_decrypt(const char *name);
int convert_roms_tile(Uint8 *g, int tileno);
void convert_all_char(Uint8 *Ptr, int Taille, Uint8 *usage_ptr);
extern int neogeo_fix_bank_type;
extern int neogeo_pvc_protection;

extern jmp_buf *ng_fatal_jump;

#define SPR_BANK        4096            /* the sprite cache's unit: 32 tiles */
#define CUSAGE_MAGIC    0x43553039u     /* "CU09", as RetroESP32 */
#define PATHN           320
#define MB              (1024u * 1024u)
/* What the budget keeps free after the ROMs and caches. */
#define LOAD_SLACK      (256u * 1024)
#define CHUNK           (16u * 1024)    /* bytes per ROM half per step when converting */

/* ---- the state of a load -------------------------------------------------- */

typedef struct {
    const ng_driver_t    *drv;
    const ng_load_opts_t *opts;
    ng_load_info_t       *info;
    char                  game[24];
    char                  rom_dir[PATHN];
    char                  cache_dir[PATHN];
    char                  parent_cache_dir[PATHN];
    ng_zip_t             *zgame, *zparent;
    ng_zip_t             *zbios[2];     /* neogeo.zip, aes.zip */
    const char           *bios_dir;
    char                 *err;
    size_t                errlen;
} ld_t;

/* What stays open while the game runs. */
static FILE  *s_spr_file;
static FILE  *s_adpcm_file[2];
static bool   s_adpcmb_alias;
static Uint32 *s_spr_scratch;           /* prefetch: missing banks + seen bitmap */
static Uint32 s_spr_scratch_seen;       /* index of the bitmap in it */

static ng_zip_t *open_zip_in(const char *dir, const char *name);
static void close_zips(ld_t *L);

static void set_err(ld_t *L, const char *msg)
{
    if (!L->err[0]) snprintf(L->err, L->errlen, "%s", msg);
}

/* ---- regions -------------------------------------------------------------- */

static bool region_alloc(ld_t *L, ROM_REGION *r, Uint32 size)
{
    r->p = NULL;
    r->size = 0;
    if (!size) return true;
    r->p = ng_big_alloc(size);
    if (!r->p) {
        set_err(L, "No hay memoria");
        return false;
    }
    r->size = size;
    return true;
}

static void region_free(ROM_REGION *r)
{
    ng_big_free(r->p);
    r->p = NULL;
    r->size = 0;
}

/* ---- files ---------------------------------------------------------------- */

static void path_join(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%.200s/%.100s", dir, name);
}

static void cache_path(char *out, size_t n, const char *dir, const char *game, const char *ext)
{
    snprintf(out, n, "%.240s/%.23s.%.8s", dir, game, ext);
}

static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* A ROM of the game: in its zip, else in the parent's, else in the BIOS
 * zips (some drivers name the BIOS's own sfix). */
static const ng_zent_t *find_rom(ld_t *L, const ng_drvrom_t *f, ng_zip_t **zip)
{
    const ng_zent_t *e = ng_zip_find(L->zgame, f->file, f->crc);
    if (e) { *zip = L->zgame; return e; }
    if (L->zparent && (e = ng_zip_find(L->zparent, f->file, f->crc))) { *zip = L->zparent; return e; }
    for (int i = 0; i < 2; i++)
        if (L->zbios[i] && (e = ng_zip_find(L->zbios[i], f->file, f->crc))) { *zip = L->zbios[i]; return e; }
    return NULL;
}

/* gngeo: the files of these regions may be missing (BIOS parts, sound
 * samples); the others may not. */
static bool region_optional(int region)
{
    return region == REGION_AUDIO_CPU_BIOS || region == REGION_FIXED_LAYER_BIOS ||
           region == REGION_MAIN_CPU_BIOS || region == REGION_AUDIO_DATA_1 ||
           region == REGION_AUDIO_DATA_2;
}

/* Every file the driver lists, before anything big is allocated. */
static bool check_files(ld_t *L)
{
    for (int i = 0; i < L->drv->nrom; i++) {
        const ng_drvrom_t *f = &L->drv->rom[i];
        ng_zip_t *z;
        if (find_rom(L, f, &z) || region_optional(f->region)) continue;
        ng_log("%s missing", f->file);
        if (strcmp(L->drv->parent, "neogeo") != 0 && !L->zparent)
            set_err(L, "Falta el zip del juego original (este es una variante)");
        else
            set_err(L, "Falta un archivo del juego en el zip");
        return false;
    }
    return true;
}

/* Opens a ROM file at its offset 'src'. NULL when it is missing (allowed
 * for the optional regions) or the zip cannot be read (err set). */
static ng_zstream_t *open_rom(ld_t *L, const ng_drvrom_t *f, uint32_t skip)
{
    ng_zip_t *z;
    const ng_zent_t *e = find_rom(L, f, &z);
    if (!e) return NULL;
    ng_zstream_t *s = ng_zs_open(z, e);
    if (!s) {
        set_err(L, "No se pudo leer el zip del juego");
        return NULL;
    }
    if (skip && !ng_zs_skip(s, skip)) {
        ng_zs_close(s);
        set_err(L, "El zip del juego está dañado");
        return NULL;
    }
    return s;
}

/* Reads 'size' bytes of a ROM file (from its 'src') into dst. true also
 * when an optional file is missing. */
static bool read_rom(ld_t *L, const ng_drvrom_t *f, uint8_t *dst, uint32_t size)
{
    ng_zstream_t *s = open_rom(L, f, f->src);
    if (!s) return L->err[0] == 0 && region_optional(f->region);
    int got = ng_zs_read(s, dst, size);
    ng_zs_close(s);
    if (got < 0) {
        set_err(L, "El zip del juego está dañado");
        return false;
    }
    /* A shorter file leaves the rest blank, as gngeo. */
    return true;
}

/* ---- the BIOS ------------------------------------------------------------- */

/* The MVS BIOS by region (MAME's names), each list ending with the others
 * as a fall-back: the region picks the language and the censorship of
 * many games. */
static const char *const s_bios_mvs[4][17] = {
    { "sp-s2.sp1", "sp-s.sp1", "sp-e.sp1", "sp-u2.sp1", "sp1-u2", "sp1-u4.bin", "neo-geo.rom",
      "vs-bios.rom", "sp-j2.sp1", "sp1.jipan.1024", "sp-j3.sp1", "japan-j3.bin", "sp1-j3.bin",
      "sp-45.sp1", "asia-s3.rom", NULL },
    { "sp-u2.sp1", "sp1-u2", "sp-e.sp1", "sp1-u4.bin", "sp-s2.sp1", "sp-s.sp1", "neo-geo.rom",
      "vs-bios.rom", "sp-j2.sp1", "sp1.jipan.1024", "sp-j3.sp1", "japan-j3.bin", "sp1-j3.bin",
      "sp-45.sp1", "asia-s3.rom", NULL },
    { "vs-bios.rom", "sp-j2.sp1", "sp1.jipan.1024", "sp-j3.sp1", "japan-j3.bin", "sp1-j3.bin",
      "sp-s2.sp1", "sp-s.sp1", "sp-e.sp1", "sp-u2.sp1", "sp1-u2", "sp1-u4.bin", "neo-geo.rom",
      "sp-45.sp1", "asia-s3.rom", NULL },
    { "sp-45.sp1", "asia-s3.rom", "sp-s2.sp1", "sp-s.sp1", "sp-e.sp1", "sp-u2.sp1", "sp1-u2",
      "sp1-u4.bin", "neo-geo.rom", "vs-bios.rom", "sp-j2.sp1", "sp1.jipan.1024", "sp-j3.sp1",
      "japan-j3.bin", "sp1-j3.bin", NULL },
};
/* The AES BIOS: Japan's for Japan, the export one otherwise. */
static const char *const s_bios_aes[2][4] = {
    { "neo-epo.bin", "aes-bios.bin", "neo-po.bin", NULL },
    { "neo-po.bin", "neo-epo.bin", "aes-bios.bin", NULL },
};
static const char *const s_bios_uni[] = { "uni-bios", NULL };
static const char *const s_bios_sfix[] = { "sfix.sfix", "sfix.sfx", "ng-sfix.rom", NULL };
static const char *const s_bios_lo[] = { "000-lo.lo", "ng-lo.rom", NULL };

static bool has_prefix(const char *s, const char *p)
{
    return strncasecmp(s, p, strlen(p)) == 0;
}

/* One BIOS file: from neogeo.zip or aes.zip in the BIOS folder, or loose
 * in it. With 'prefix', any file whose name starts with the name (the
 * Universe BIOS comes as uni-bios_4_0.rom, uni-bios_3_3.rom...). Up to
 * 'size' bytes into dst; the file's name to 'found'. */
static bool bios_read(ld_t *L, const char *const *names, bool prefix, uint8_t *dst, uint32_t size,
                      char *found, size_t found_n)
{
    for (int i = 0; names[i]; i++) {
        for (int k = 0; k < 2; k++) {
            if (!L->zbios[k]) continue;
            const ng_zent_t *e = prefix ? ng_zip_find_prefix(L->zbios[k], names[i])
                                        : ng_zip_find(L->zbios[k], names[i], 0);
            if (!e || e->usize == 0) continue;
            uint32_t n = e->usize < size ? e->usize : size;
            if (ng_zip_read(L->zbios[k], e, dst, n) != (int)n) continue;
            if (found) snprintf(found, found_n, "%s", e->name);
            return true;
        }
        char path[PATHN];
        if (prefix) {
            /* the first matching file in the folder */
            DIR *d = opendir(L->bios_dir);
            if (!d) continue;
            struct dirent *de;
            path[0] = 0;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] != '.' && has_prefix(de->d_name, names[i])) {
                    path_join(path, sizeof(path), L->bios_dir, de->d_name);
                    break;
                }
            }
            closedir(d);
            if (!path[0]) continue;
        } else {
            path_join(path, sizeof(path), L->bios_dir, names[i]);
        }
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t got = fread(dst, 1, size, f);
        fclose(f);
        if (got > 0) {
            if (found) snprintf(found, found_n, "%s", strrchr(path, '/') + 1);
            return true;
        }
    }
    return false;
}

/* The L0 ROM's zoom table (000-lo.lo), when the BIOS set has none. The
 * console shrinks a sprite of 256 lines to n+1 of them by keeping lines in
 * a fixed order that spreads them evenly (the halves, then the quarters,
 * and so on, as a bit-reversed count); level n of the table lists the
 * lines kept, in order, and 0xFF after them. The same table as nullbios's
 * zoom-rom.py (ngdevkit) makes; built here from that description. */
static void make_zoom_table(uint8_t *lo)
{
    static const int8_t I[8] = { 8, 12, 10, 14, 9, 13, 11, 15 };
    static const int8_t J[4] = { 0, 16, -8, 8 };
    static const int16_t K[8] = { 0, -128, 64, -64, 32, -96, 96, -32 };
    uint8_t kept[256];
    for (int pass = 0; pass < 2; pass++) {
        memset(kept, 0, sizeof(kept));
        int level = 0;
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++)
                for (int k = 0; k < 8; k++) {
                    kept[128 + I[i] + J[j] + K[k]] = 1;
                    uint8_t *out = lo + (pass * 256 + level) * 256;
                    int n = 0;
                    for (int line = 0; line < 256; line++)
                        if (kept[line]) out[n++] = (uint8_t)line;
                    memset(out + n, 0xFF, 256 - n);
                    level++;
                }
    }
}

/* The 68000's BIOS of a system; false when none of its files is there. */
static bool read_system_bios(ld_t *L, ng_system_t sys, int region, uint8_t *dst, char *found, size_t fn)
{
    if (region < 0 || region > 3) region = 0;
    switch (sys) {
    case NG_SYS_AES: return bios_read(L, s_bios_aes[region == 2], false, dst, 0x20000, found, fn);
    case NG_SYS_UNI: return bios_read(L, s_bios_uni, true, dst, 0x20000, found, fn);
    default:         return bios_read(L, s_bios_mvs[region], false, dst, 0x20000, found, fn);
    }
}

/* The system asked for, else whichever there is (the option can be set
 * after the game is open). */
static bool read_any_bios(ld_t *L, ng_system_t want, int region, uint8_t *dst, ng_system_t *got,
                          char *found, size_t fn)
{
    static const ng_system_t order[3] = { NG_SYS_MVS, NG_SYS_AES, NG_SYS_UNI };
    if (read_system_bios(L, want, region, dst, found, fn)) {
        *got = want;
        return true;
    }
    for (int i = 0; i < 3; i++) {
        if (order[i] != want && read_system_bios(L, order[i], region, dst, found, fn)) {
            *got = order[i];
            return true;
        }
    }
    return false;
}

static bool load_bios(ld_t *L)
{
    GAME_ROMS *r = &memory.rom;
    char found[48];

    /* The 68000's BIOS, unless the game brings its own (some bootlegs). */
    L->info->system = L->opts->system;
    if (!(r->info.flags & HAS_CUSTOM_CPU_BIOS)) {
        if (!region_alloc(L, &r->bios_m68k, 0x20000)) return false;
        if (!read_any_bios(L, L->opts->system, L->opts->region, r->bios_m68k.p, &L->info->system,
                           found, sizeof found)) {
            set_err(L, "Falta la BIOS de Neo Geo: neogeo.zip de MAME (sp-s2.sp1, sfix.sfix) en la carpeta bios");
            return false;
        }
        ng_log("BIOS %s", found);
    }

    /* The BIOS's text layer. */
    if (!(r->info.flags & HAS_CUSTOM_SFIX_BIOS)) {
        if (!region_alloc(L, &r->bios_sfix, 0x20000)) return false;
        if (!bios_read(L, s_bios_sfix, false, r->bios_sfix.p, 0x20000, NULL, 0)) {
            set_err(L, "Falta sfix.sfix (la BIOS de Neo Geo está incompleta)");
            return false;
        }
    }
    convert_all_char(r->bios_sfix.p, 0x20000, memory.fix_board_usage);

    /* The zoom table: the set's, else made here. */
    memory.ng_lo = ng_big_alloc(0x20000);
    if (!memory.ng_lo) {
        set_err(L, "No hay memoria");
        return false;
    }
    if (!bios_read(L, s_bios_lo, false, memory.ng_lo, 0x20000, NULL, 0))
        make_zoom_table(memory.ng_lo);
    return true;
}

bool ng_bios_switch(const char *bios_dir, ng_system_t sys, int region, ng_system_t *got)
{
    GAME_ROMS *r = &memory.rom;
    if (!r->bios_m68k.p || (r->info.flags & HAS_CUSTOM_CPU_BIOS)) return false;
    ld_t L;
    char err[8] = "";
    memset(&L, 0, sizeof(L));
    L.err = err;
    L.errlen = sizeof(err);
    L.bios_dir = bios_dir;
    L.zbios[0] = open_zip_in(bios_dir, "neogeo");
    L.zbios[1] = open_zip_in(bios_dir, "aes");
    uint8_t *tmp = calloc(1, 0x20000);
    char found[48];
    bool ok = tmp && read_system_bios(&L, sys, region, tmp, found, sizeof found);
    if (ok) {
        memcpy(r->bios_m68k.p, tmp, 0x20000);
        *got = sys;
        ng_log("BIOS %s", found);
    } else {
        ng_log("no BIOS for that system: keeping the one loaded");
    }
    free(tmp);
    close_zips(&L);
    return ok;
}

/* ---- sprites -------------------------------------------------------------- */

/* The sprite ROMs come in pairs: one holds the even bytes of a range of
 * the sprite region, the other the odd ones. A pair is the two entries
 * whose dest differ only in bit 0. */
typedef struct {
    const ng_drvrom_t *half[2];     /* even, odd (either may be NULL) */
    uint32_t base, size;            /* bytes of the region the pair fills */
} spr_pair_t;

static int spr_pairs(const ng_driver_t *drv, spr_pair_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < drv->nrom; i++) {
        const ng_drvrom_t *f = &drv->rom[i];
        if (f->region != REGION_SPRITES) continue;
        uint32_t base = f->dest & ~1u;
        int k;
        for (k = 0; k < n; k++)
            if (out[k].base == base && !out[k].half[f->dest & 1]) break;
        if (k == n) {
            if (n == max) break;
            memset(&out[n], 0, sizeof(out[n]));
            out[n].base = base;
            n++;
        }
        out[k].half[f->dest & 1] = f;
        if (2 * f->size > out[k].size) out[k].size = 2 * f->size;
    }
    /* in the order they sit in the region */
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && out[b - 1].base > out[b].base; b--) {
            spr_pair_t t = out[b];
            out[b] = out[b - 1];
            out[b - 1] = t;
        }
    return n;
}

/* Calls 'sink' with the interleaved bytes of the sprite region, in order,
 * CHUNK*2 at a time (blank where no ROM covers it). */
typedef bool (*spr_sink_t)(ld_t *L, uint32_t ofs, uint8_t *data, uint32_t len, void *ctx);

static bool spr_stream(ld_t *L, uint32_t total, spr_sink_t sink, void *ctx)
{
    spr_pair_t pairs[32];
    int np = spr_pairs(L->drv, pairs, 32);
    uint8_t *half[2] = { malloc(CHUNK), malloc(CHUNK) };
    uint8_t *out = malloc(2 * CHUNK);
    bool ok = half[0] && half[1] && out;
    if (!ok) set_err(L, "No hay memoria");
    uint32_t pos = 0, next_log = 8 * MB;

    for (int p = 0; ok && p <= np; p++) {
        uint32_t base = p < np ? pairs[p].base : total;
        if (base > total) base = total;
        /* blank up to this pair */
        memset(out, 0, 2 * CHUNK);
        while (ok && pos < base) {
            uint32_t n = base - pos < 2 * CHUNK ? base - pos : 2 * CHUNK;
            ok = sink(L, pos, out, n, ctx);
            pos += n;
        }
        if (!ok || p == np) break;
        if (pos > base) continue; /* overlaps what was written: skip */

        ng_zstream_t *s[2] = { NULL, NULL };
        for (int h = 0; h < 2; h++)
            if (pairs[p].half[h]) {
                s[h] = open_rom(L, pairs[p].half[h], pairs[p].half[h]->src / 2);
                if (!s[h] && L->err[0]) ok = false;
            }
        uint32_t end = base + pairs[p].size;
        if (end > total) end = total;
        while (ok && pos < end) {
            uint32_t n = (end - pos) / 2 < CHUNK ? (end - pos) / 2 : CHUNK;
            for (int h = 0; h < 2; h++) {
                if (!s[h]) {
                    memset(half[h], 0, n);
                    continue;
                }
                int got = ng_zs_read(s[h], half[h], n);
                if (got < 0) {
                    set_err(L, "El zip del juego está dañado");
                    ok = false;
                    break;
                }
                if ((uint32_t)got < n) memset(half[h] + got, 0, n - got);
            }
            if (!ok) break;
            for (uint32_t i = 0; i < n; i++) {
                out[2 * i] = half[0][i];
                out[2 * i + 1] = half[1][i];
            }
            ok = sink(L, pos, out, 2 * n, ctx);
            pos += 2 * n;
            if (pos >= next_log) {
                ng_log("sprites: %u of %u MB", (unsigned)(pos / MB), (unsigned)(total / MB));
                next_log += 8 * MB;
            }
        }
        for (int h = 0; h < 2; h++) ng_zs_close(s[h]);
    }
    free(half[0]);
    free(half[1]);
    free(out);
    return ok;
}

static bool sink_ram(ld_t *L, uint32_t ofs, uint8_t *data, uint32_t len, void *ctx)
{
    (void)L;
    memcpy((uint8_t *)ctx + ofs, data, len);
    return true;
}

typedef struct {
    FILE   *f;
    Uint32 *usage;
} file_sink_t;

static bool sink_file(ld_t *L, uint32_t ofs, uint8_t *data, uint32_t len, void *ctx)
{
    file_sink_t *fs = ctx;
    for (uint32_t t = 0; t < len / 128; t++) {
        uint32_t tile = ofs / 128 + t;
        fs->usage[tile >> 4] |= (Uint32)convert_roms_tile(data, (int)t);
    }
    if (fwrite(data, 1, len, fs->f) != len) {
        set_err(L, "No hay lugar en la tarjeta para la caché del juego");
        return false;
    }
    return true;
}

/* gngeo's tile conversion and usage, over the region in memory. */
static void convert_tiles_ram(GAME_ROMS *r)
{
    Uint32 n = r->tiles.size >> 7;
    Uint32 *usage = (Uint32 *)r->spr_usage.p;
    for (Uint32 i = 0; i < n; i++) usage[i >> 4] |= (Uint32)convert_roms_tile(r->tiles.p, (int)i);
}

static bool cusage_ok(const char *path, uint32_t tiles_size)
{
    return file_size(path) == (long)(4 + (tiles_size >> 11) * 4);
}

/* The sprite cache files: the game's, or the parent's (clones often share
 * the sprites), checked by size. */
static bool spr_cache_find(ld_t *L, uint32_t tiles_size, char *ctile, char *cusage)
{
    const char *dirs[2] = { L->cache_dir, L->parent_cache_dir };
    const char *names[2] = { L->game, L->drv->parent };
    for (int i = 0; i < 2; i++) {
        if (!dirs[i][0]) continue;
        cache_path(ctile, PATHN, dirs[i], names[i], "ctile");
        cache_path(cusage, PATHN, dirs[i], names[i], "cusage");
        if (file_size(ctile) == (long)tiles_size && cusage_ok(cusage, tiles_size)) return true;
    }
    cache_path(ctile, PATHN, L->cache_dir, L->game, "ctile");
    cache_path(cusage, PATHN, L->cache_dir, L->game, "cusage");
    return false;
}

static bool spr_cache_make(ld_t *L, uint32_t tiles_size, const char *ctile, const char *cusage)
{
    char tmp[PATHN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", ctile);
    mkdir(L->cache_dir, 0777);
    uint32_t usage_n = (tiles_size >> 11) * 4;
    file_sink_t fs = { fopen(tmp, "wb"), malloc(usage_n) };
    if (!fs.f || !fs.usage) {
        if (fs.f) fclose(fs.f);
        free(fs.usage);
        set_err(L, fs.usage ? "No se pudo escribir la caché del juego en la tarjeta" : "No hay memoria");
        return false;
    }
    memset(fs.usage, 0, usage_n);
    ng_log("making the sprite cache (%u MB), only this time", (unsigned)(tiles_size / MB));
    bool ok = spr_stream(L, tiles_size, sink_file, &fs);
    if (fclose(fs.f) != 0 && ok) {
        set_err(L, "No hay lugar en la tarjeta para la caché del juego");
        ok = false;
    }
    if (ok) {
        FILE *u = fopen(cusage, "wb");
        uint32_t magic = CUSAGE_MAGIC;
        ok = u && fwrite(&magic, 4, 1, u) == 1 && fwrite(fs.usage, 1, usage_n, u) == usage_n;
        if (u && fclose(u) != 0) ok = false;
        if (!ok) set_err(L, "No hay lugar en la tarjeta para la caché del juego");
    }
    free(fs.usage);
    if (ok) {
        remove(ctile);
        ok = rename(tmp, ctile) == 0;
        if (!ok) set_err(L, "No se pudo escribir la caché del juego en la tarjeta");
    }
    if (!ok) {
        remove(tmp);
        remove(cusage);
    }
    return ok;
}

/* ---- ADPCM ---------------------------------------------------------------- */

static bool vrom_cache_make(ld_t *L, int region, uint32_t size, const char *path)
{
    char tmp[PATHN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    mkdir(L->cache_dir, 0777);
    FILE *f = fopen(tmp, "wb");
    uint8_t *buf = malloc(64 * 1024);
    bool ok = f && buf;
    if (!ok) set_err(L, buf ? "No se pudo escribir la caché del juego en la tarjeta" : "No hay memoria");
    ng_log("making the sound cache (%u MB), only this time", (unsigned)(size / MB));
    uint32_t pos = 0;
    /* the files in the order they sit in the region */
    while (ok) {
        const ng_drvrom_t *next = NULL;
        for (int i = 0; i < L->drv->nrom; i++) {
            const ng_drvrom_t *r = &L->drv->rom[i];
            if (r->region == region && r->dest >= pos && (!next || r->dest < next->dest)) next = r;
        }
        uint32_t upto = next ? (next->dest < size ? next->dest : size) : size;
        memset(buf, 0, 64 * 1024);
        while (ok && pos < upto) {
            uint32_t n = upto - pos < 64 * 1024 ? upto - pos : 64 * 1024;
            ok = fwrite(buf, 1, n, f) == n;
            pos += n;
        }
        if (!ok || !next) break;
        ng_zstream_t *s = open_rom(L, next, next->src);
        if (!s && L->err[0]) { ok = false; break; }
        uint32_t end = next->dest + next->size < size ? next->dest + next->size : size;
        while (ok && pos < end) {
            uint32_t n = end - pos < 64 * 1024 ? end - pos : 64 * 1024;
            int got = s ? ng_zs_read(s, buf, n) : 0;
            if (got < 0) {
                set_err(L, "El zip del juego está dañado");
                ok = false;
                break;
            }
            if ((uint32_t)got < n) memset(buf + got, 0, n - got);
            ok = fwrite(buf, 1, n, f) == n;
            pos += n;
        }
        ng_zs_close(s);
    }
    if (f && fclose(f) != 0) ok = false;
    free(buf);
    if (!ok) {
        set_err(L, "No hay lugar en la tarjeta para la caché del juego");
        remove(tmp);
        return false;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        set_err(L, "No se pudo escribir la caché del juego en la tarjeta");
        remove(tmp);
        return false;
    }
    return true;
}

static bool vrom_cache_find(ld_t *L, const char *ext, uint32_t size, char *path)
{
    const char *dirs[2] = { L->cache_dir, L->parent_cache_dir };
    const char *names[2] = { L->game, L->drv->parent };
    for (int i = 0; i < 2; i++) {
        if (!dirs[i][0]) continue;
        cache_path(path, PATHN, dirs[i], names[i], ext);
        if (file_size(path) == (long)size) return true;
    }
    cache_path(path, PATHN, L->cache_dir, L->game, ext);
    return false;
}

static bool adpcm_stream(ld_t *L, int which, const char *path, uint32_t size, uint32_t cache)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(L, "No se pudo leer la caché del juego");
        return false;
    }
    adpcm_cache_t *c = which ? &adpcm_cacheB : &adpcm_cacheA;
    if (adpcm_cache_init(c, f, size, cache) != 0) {
        fclose(f);
        set_err(L, "No hay memoria");
        return false;
    }
    s_adpcm_file[which] = f;
    return true;
}

/* ---- the sprite cache's reads (video.c) ------------------------------------ */

int ng_spr_read_banks(Uint32 bank, Uint32 count, Uint8 *dest)
{
    if (!s_spr_file) return -1;
    if (fseek(s_spr_file, (long)bank * SPR_BANK, SEEK_SET) != 0) return -1;
    size_t want = (size_t)count * SPR_BANK;
    return fread(dest, 1, want, s_spr_file) == want ? 0 : -1;
}

Uint32 *ng_spr_scratch(Uint32 **seen_words)
{
    *seen_words = s_spr_scratch ? s_spr_scratch + s_spr_scratch_seen : NULL;
    return s_spr_scratch;
}

/* ---- the game ---------------------------------------------------------------- */

/* Games gngeo draws line by line (they change the picture in the middle of
 * the screen with the timer interrupt), as RetroESP32's list. */
static const char *const s_raster_games[] = {
    "ridhero", "ridheroh", "lbowling", "ssideki", "ssideki2", "ssideki3", "ssideki4",
    "turfmast", "ironclad", NULL,
};

static void dir_of(char *out, size_t n, const char *path)
{
    snprintf(out, n, "%.300s", path);
    char *s = strrchr(out, '/');
    if (s) *s = 0;
    else snprintf(out, n, ".");
}

static void game_name(char *out, size_t n, const char *path)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t i = 0;
    for (; b[i] && b[i] != '.' && i + 1 < n; i++) out[i] = (char)(b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i]);
    out[i] = 0;
}

static ng_zip_t *open_zip_in(const char *dir, const char *name)
{
    char path[PATHN];
    snprintf(path, sizeof(path), "%.250s/%.40s.zip", dir, name);
    return ng_zip_open(path);
}

static void close_zips(ld_t *L)
{
    ng_zip_close(L->zgame);
    ng_zip_close(L->zparent);
    ng_zip_close(L->zbios[0]);
    ng_zip_close(L->zbios[1]);
    L->zgame = L->zparent = L->zbios[0] = L->zbios[1] = NULL;
}

/* Everything that can fail without leaving a half-built console. */
static bool load_inner(ld_t *L, const emu_load_t *req)
{
    const ng_driver_t *drv = L->drv;
    GAME_ROMS *r = &memory.rom;
    const ng_load_opts_t *o = L->opts;

    r->info.name = (char *)drv->name;
    r->info.longname = (char *)drv->longname;
    r->info.year = drv->year;
    r->info.flags = 0;

    if (!check_files(L)) return false;

    /* The small regions, in memory. */
    if (!region_alloc(L, &r->cpu_m68k, drv->romsize[REGION_MAIN_CPU_CARTRIDGE])) return false;
    if (drv->romsize[REGION_AUDIO_CPU_CARTRIDGE] == 0 && drv->romsize[REGION_AUDIO_CPU_ENCRYPTED] != 0) {
        if (!region_alloc(L, &r->cpu_z80c, 0x80000) || !region_alloc(L, &r->cpu_z80, 0x90000)) return false;
    } else if (!region_alloc(L, &r->cpu_z80, drv->romsize[REGION_AUDIO_CPU_CARTRIDGE])) {
        return false;
    }
    if (!region_alloc(L, &r->game_sfix, drv->romsize[REGION_FIXED_LAYER_CARTRIDGE])) return false;
    if (!region_alloc(L, &r->gfix_usage, r->game_sfix.size >> 5)) return false;
    if (drv->romsize[REGION_MAIN_CPU_BIOS]) {
        r->info.flags |= HAS_CUSTOM_CPU_BIOS;
        if (!region_alloc(L, &r->bios_m68k, drv->romsize[REGION_MAIN_CPU_BIOS])) return false;
    }
    if (drv->romsize[REGION_AUDIO_CPU_BIOS]) {
        r->info.flags |= HAS_CUSTOM_AUDIO_BIOS;
        if (!region_alloc(L, &r->bios_audio, drv->romsize[REGION_AUDIO_CPU_BIOS])) return false;
    }
    if (drv->romsize[REGION_FIXED_LAYER_BIOS]) {
        r->info.flags |= HAS_CUSTOM_SFIX_BIOS;
        if (!region_alloc(L, &r->bios_sfix, drv->romsize[REGION_FIXED_LAYER_BIOS])) return false;
    }

    /* The BIOS before the big regions: a missing BIOS is the commonest
     * error and should not wait for a cache to be made. */
    if (!load_bios(L)) return false;

    Uint32 cpu_next = 0;
    for (int i = 0; i < drv->nrom; i++) {
        const ng_drvrom_t *f = &drv->rom[i];
        ROM_REGION *dst = NULL;
        Uint32 dest = f->dest;
        switch (f->region) {
        case REGION_MAIN_CPU_CARTRIDGE:
            /* RetroESP32: dest 2 marks a word-interleaved half the game's
             * init function puts together (mslug5, kof2003): sequential
             * here. */
            if (dest == 2) dest = cpu_next;
            cpu_next = dest + f->size;
            dst = &r->cpu_m68k;
            break;
        case REGION_AUDIO_CPU_CARTRIDGE: dst = &r->cpu_z80; break;
        case REGION_AUDIO_CPU_ENCRYPTED: dst = &r->cpu_z80c; break;
        case REGION_FIXED_LAYER_CARTRIDGE: dst = &r->game_sfix; break;
        case REGION_MAIN_CPU_BIOS: dst = (r->info.flags & HAS_CUSTOM_CPU_BIOS) ? &r->bios_m68k : NULL; break;
        case REGION_AUDIO_CPU_BIOS: dst = (r->info.flags & HAS_CUSTOM_AUDIO_BIOS) ? &r->bios_audio : NULL; break;
        case REGION_FIXED_LAYER_BIOS: dst = (r->info.flags & HAS_CUSTOM_SFIX_BIOS) ? &r->bios_sfix : NULL; break;
        default: break; /* sprites and samples below */
        }
        if (!dst || !dst->p) continue;
        if (dest >= dst->size) continue;
        uint32_t n = f->size;
        if (dest + n > dst->size) n = dst->size - dest;
        if (!read_rom(L, f, dst->p + dest, n)) return false;
    }

    /* The big ones: in memory if they fit, else through the card. */
    Uint32 tiles = drv->romsize[REGION_SPRITES];
    Uint32 va = drv->romsize[REGION_AUDIO_DATA_1];
    Uint32 vb = drv->romsize[REGION_AUDIO_DATA_2];
    bool encrypted = game_needs_gfx_decrypt(drv->name) && strcmp(drv->name, "kof98") != 0;
    size_t left = ng_big_left();
    size_t slack = LOAD_SLACK + (tiles >> 11) * 4;
    /* decrypting the sprites in memory takes a second copy */
    size_t tiles_need = encrypted ? 2u * tiles : tiles;
    bool tiles_ram, va_ram, vb_ram;
    if (tiles_need + va + vb + slack <= left) {
        tiles_ram = va_ram = vb_ram = true;
    } else if (tiles_need + 2 * MB + slack <= left) {
        tiles_ram = true;
        va_ram = vb_ram = false;
    } else {
        tiles_ram = false;
        va_ram = va + vb + 4 * MB + slack <= left;
        vb_ram = va_ram;
    }

    char ctile[PATHN], cusage[PATHN];
    if (!tiles_ram) {
        if (!spr_cache_find(L, tiles, ctile, cusage)) {
            if (encrypted) {
                set_err(L, "Este juego tiene los gráficos cifrados: hace falta su caché hecha en una computadora");
                return false;
            }
            if (!spr_cache_make(L, tiles, ctile, cusage)) return false;
        }
    }
    char vpath[2][PATHN];
    if (!va_ram && va && !vrom_cache_find(L, "vroma", va, vpath[0])) {
        if (encrypted) {
            set_err(L, "Este juego tiene los gráficos cifrados: hace falta su caché hecha en una computadora");
            return false;
        }
        if (!vrom_cache_make(L, REGION_AUDIO_DATA_1, va, vpath[0])) return false;
    }
    if (!vb_ram && vb && !vrom_cache_find(L, "vromb", vb, vpath[1])) {
        if (!vrom_cache_make(L, REGION_AUDIO_DATA_2, vb, vpath[1])) return false;
    }

    /* Samples in memory. */
    if (va_ram && va) {
        if (!region_alloc(L, &r->adpcma, va)) return false;
        for (int i = 0; i < drv->nrom; i++) {
            const ng_drvrom_t *f = &drv->rom[i];
            if (f->region != REGION_AUDIO_DATA_1 || f->dest >= va) continue;
            if (!read_rom(L, f, r->adpcma.p + f->dest, f->dest + f->size <= va ? f->size : va - f->dest))
                return false;
        }
    }
    if (vb_ram && vb) {
        if (!region_alloc(L, &r->adpcmb, vb)) return false;
        for (int i = 0; i < drv->nrom; i++) {
            const ng_drvrom_t *f = &drv->rom[i];
            if (f->region != REGION_AUDIO_DATA_2 || f->dest >= vb) continue;
            if (!read_rom(L, f, r->adpcmb.p + f->dest, f->dest + f->size <= vb ? f->size : vb - f->dest))
                return false;
        }
    }

    /* Sprites in memory: interleaved as they come (converted after the
     * decryption). */
    if (tiles_ram && tiles) {
        if (!region_alloc(L, &r->tiles, tiles)) return false;
        if (!spr_stream(L, tiles, sink_ram, r->tiles.p)) return false;
    }

    /* The game's own decryption and protection (gngeo's init functions);
     * a failed allocation in them jumps back here. */
    jmp_buf jb;
    if (setjmp(jb)) {
        ng_fatal_jump = NULL;
        set_err(L, "No hay memoria");
        return false;
    }
    ng_fatal_jump = &jb;
    init_roms(r);
    ng_fatal_jump = NULL;

    /* The sprites' usage: 2 bits a tile (invisible ones are skipped). */
    if (!region_alloc(L, &r->spr_usage, tiles >= 2048 ? (tiles >> 11) * 4 : 4)) return false;
    memory.nb_of_tiles = tiles >> 7;
    if (tiles_ram) {
        convert_tiles_ram(r);
    } else {
        FILE *u = fopen(cusage, "rb");
        uint32_t magic = 0;
        bool ok = u && fread(&magic, 4, 1, u) == 1 && magic == CUSAGE_MAGIC &&
                  fread(r->spr_usage.p, 1, r->spr_usage.size, u) == r->spr_usage.size;
        if (u) fclose(u);
        if (!ok) {
            set_err(L, "No se pudo leer la caché del juego");
            return false;
        }
        s_spr_file = fopen(ctile, "rb");
        if (!s_spr_file) {
            set_err(L, "No se pudo leer la caché del juego");
            return false;
        }
        r->tiles.p = NULL;
        r->tiles.size = tiles;
    }

    /* The sample caches, then the sprite cache with what is left. */
    left = ng_big_left();
    uint32_t adpcm_cache = 0;
    if (!va_ram && va) {
        /* P4OS: 1 MB of the samples; every other MB goes to the sprites,
         * which the screen needs every frame */
        adpcm_cache = left > 24 * MB ? 4 * MB : 1 * MB;
        if (!adpcm_stream(L, 0, vpath[0], va, adpcm_cache)) return false;
    }
    if (!vb_ram && vb) {
        if (!adpcm_stream(L, 1, vpath[1], vb, 1 * MB)) return false;
    }
    if (!vb) {
        /* gngeo: ADPCM-B reads the ADPCM-A ROM when there is none of its
         * own. */
        r->adpcmb.p = r->adpcma.p;
        r->adpcmb.size = r->adpcma.size;
        if (adpcm_cacheA.active) {
            adpcm_cacheB = adpcm_cacheA;
            s_adpcmb_alias = true;
        }
    }
    if (!va_ram && va) r->adpcma.size = va;
    if (!vb_ram && vb) r->adpcmb.size = vb;

    if (!tiles_ram) {
        left = ng_big_left();
        uint32_t total_banks = tiles / SPR_BANK;
        /* The cache's own tables: a pointer per bank, the prefetch's list
         * and bitmap. */
        size_t tables = (size_t)total_banks * sizeof(void *) + 512 * 4 + ((total_banks + 31) / 32) * 4 + 64 * 1024;
        size_t avail = left > tables + LOAD_SLACK ? left - tables - LOAD_SLACK : 0;
        /* P4OS: "Automática" takes all there is (the frame-aware eviction
         * in video.c makes every MB count) */
        size_t want = o->spr_cache_mb ? (size_t)o->spr_cache_mb * MB : avail;
        if (want > avail) want = avail;
        if (want > tiles) want = tiles;
        want &= ~(size_t)(SPR_BANK - 1);
        if (want < 1 * MB) {
            set_err(L, "No hay memoria");
            return false;
        }
        s_spr_scratch_seen = 512;
        s_spr_scratch = ng_big_alloc((512 + (total_banks + 31) / 32) * sizeof(Uint32));
        if (!s_spr_scratch) {
            set_err(L, "No hay memoria");
            return false;
        }
        /* P4OS: the budget is not the heap. What is left of the board's
         * PSRAM may be in pieces smaller than the cache wanted (seen with
         * Metal Slug: the 12 MB block refused right after the conversion),
         * so a smaller cache is tried, 2 MB at a time, down to 1 MB. */
        size_t first = want;
        while (init_sprite_cache((Uint32)want, SPR_BANK) != GN_TRUE) {
            if (want <= 1 * MB) {
                uint32_t fi = 0, fp = 0;
                aos_hal_heap_info(&fi, &fp);
                ng_log("no sprite cache even at 1 MB: %u KB of PSRAM free", (unsigned)(fp / 1024));
                set_err(L, "No hay memoria");
                return false;
            }
            want = want > 3 * MB ? want - 2 * MB : 1 * MB;
            want &= ~(size_t)(SPR_BANK - 1);
        }
        if (want != first) ng_log("sprite cache %u MB (%u MB did not fit)", (unsigned)(want / MB), (unsigned)(first / MB));
        L->info->spr_cache_kb = (uint32_t)(want / 1024);
    }

    /* For CMC games read from a cache: the text layer comes decrypted in
     * .sfix (gngeo takes it from the end of the sprite ROM). */
    if (encrypted && !tiles_ram) {
        char sfix[PATHN];
        cache_path(sfix, sizeof(sfix), L->cache_dir, L->game, "sfix");
        long sz = file_size(sfix);
        if (sz <= 0 && L->parent_cache_dir[0]) {
            cache_path(sfix, sizeof(sfix), L->parent_cache_dir, drv->parent, "sfix");
            sz = file_size(sfix);
        }
        if (sz > 0 && (Uint32)sz <= r->game_sfix.size) {
            FILE *f = fopen(sfix, "rb");
            if (f) {
                if (fread(r->game_sfix.p, 1, sz, f) != (size_t)sz) ng_log("short .sfix");
                fclose(f);
            }
        } else {
            ng_log("no .sfix: the text layer may be wrong");
        }
    }

    memory.fix_game_usage = r->gfix_usage.p;
    /* dr_load_game(): the game's vectors aside, the BIOS's in place. */
    memcpy(memory.game_vector, r->cpu_m68k.p, 0x80);
    memcpy(r->cpu_m68k.p, r->bios_m68k.p, 0x80);
    convert_all_char(r->game_sfix.p, r->game_sfix.size, memory.fix_game_usage);
    init_video();

    L->info->tiles_streamed = !tiles_ram;
    L->info->adpcma_streamed = !va_ram && va;
    L->info->adpcmb_streamed = !vb_ram && vb;
    for (int i = 0; s_raster_games[i]; i++)
        if (strcmp(drv->name, s_raster_games[i]) == 0) L->info->raster = true;
    (void)req;
    return true;
}

bool ng_load_game(const emu_host_t *host, const emu_load_t *req, const ng_load_opts_t *opts,
                  ng_load_info_t *info, char *err, size_t errlen)
{
    (void)host;
    ld_t L;
    memset(&L, 0, sizeof(L));
    L.opts = opts;
    L.info = info;
    L.err = err;
    L.errlen = errlen;
    err[0] = 0;
    L.bios_dir = req->bios_dir ? req->bios_dir : ".";

    game_name(L.game, sizeof(L.game), req->rom);
    L.drv = ng_driver_find(L.game);
    if (!L.drv || strcmp(L.drv->name, "neogeo") == 0) {
        set_err(&L, "No es un juego de Neo Geo conocido (el zip tiene que llamarse como en MAME)");
        return false;
    }
    snprintf(info->game, sizeof(info->game), "%s", L.drv->name);
    snprintf(info->longname, sizeof(info->longname), "%.63s", L.drv->longname);

    dir_of(L.rom_dir, sizeof(L.rom_dir), req->rom);
    snprintf(L.cache_dir, sizeof(L.cache_dir), "%.250s/%.30s", L.rom_dir, L.game);
    if (strcmp(L.drv->parent, "neogeo") != 0)
        snprintf(L.parent_cache_dir, sizeof(L.parent_cache_dir), "%.250s/%.30s", L.rom_dir, L.drv->parent);

    L.zgame = ng_zip_open(req->rom);
    if (!L.zgame) {
        set_err(&L, "No se pudo abrir el zip del juego");
        return false;
    }
    if (L.parent_cache_dir[0]) L.zparent = open_zip_in(L.rom_dir, L.drv->parent);
    L.zbios[0] = open_zip_in(L.bios_dir, "neogeo");
    L.zbios[1] = open_zip_in(L.bios_dir, "aes");

    bool ok = load_inner(&L, req);
    close_zips(&L);
    if (!ok) {
        if (!err[0]) set_err(&L, "No se pudo cargar el juego");
        ng_unload_game();
    }
    return ok;
}

void ng_unload_game(void)
{
    GAME_ROMS *r = &memory.rom;
    /* While the sprites are streamed, tiles.p points into the cache. */
    if (memory.vid.spr_cache.data) r->tiles.p = NULL;
    free_sprite_cache();
    if (adpcm_cacheA.active) adpcm_cache_free(&adpcm_cacheA); /* closes its file */
    if (adpcm_cacheB.active && !s_adpcmb_alias) adpcm_cache_free(&adpcm_cacheB);
    memset(&adpcm_cacheA, 0, sizeof(adpcm_cacheA));
    memset(&adpcm_cacheB, 0, sizeof(adpcm_cacheB));
    s_adpcm_file[0] = s_adpcm_file[1] = NULL;
    s_adpcmb_alias = false;
    if (s_spr_file) fclose(s_spr_file);
    s_spr_file = NULL;
    ng_big_free(s_spr_scratch);
    s_spr_scratch = NULL;

    if (r->adpcmb.p == r->adpcma.p) r->adpcmb.p = NULL;
    region_free(&r->tiles);
    region_free(&r->cpu_m68k);
    region_free(&r->cpu_z80);
    region_free(&r->cpu_z80c);
    region_free(&r->game_sfix);
    region_free(&r->bios_sfix);
    region_free(&r->bios_audio);
    region_free(&r->bios_m68k);
    region_free(&r->adpcma);
    region_free(&r->adpcmb);
    region_free(&r->spr_usage);
    region_free(&r->gfix_usage);
    ng_big_free(memory.ng_lo);
    memory.ng_lo = NULL;
    memory.fix_game_usage = NULL;
}
