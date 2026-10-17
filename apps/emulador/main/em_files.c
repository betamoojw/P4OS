/*
 * P4OS - Emulador: the card.
 *
 *   /sdcard/emulador/
 *       ajustes.txt            the app's settings, "key=value" a line
 *       recientes.txt          when each game was last played, and for how long
 *       nes/                   a folder per console: the games (subfolders too)
 *           bios/              files a console may need (the GBA's BIOS...)
 *           saves/<game>.sav   battery saves, the format other emulators use
 *           states/<game>.st0  save states: 0 the automatic one, 1..4 by hand,
 *           states/<game>.st0.thm   each with its thumbnail
 *       gba/ ...
 *
 * The settings live in a file and not in the board's preferences: those are
 * NVS, whose keys stop at 15 characters ("emu.nes.spritelimit" is 19), and a
 * file can be read and written by the app's page in the portal too.
 */
#include "emu.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

void *emu_psram(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

/* ---------------------------------------------------------------------------
 * Folders
 * ------------------------------------------------------------------------ */

static char s_root[96];

const char *emu_root(void)
{
    if (!s_root[0]) {
        const char *sd = aos_hal_path_sd_root();
        snprintf(s_root, sizeof s_root, "%s/emulador", sd ? sd : ".");
    }
    return s_root;
}

void emu_console_dir(const emu_core_t *core, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", emu_root(), core->id);
}

static void mkdir_p(const char *path)
{
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        aos_hal_log(EMU_TAG, "cannot make %s (%d)", path, errno);
    }
}

void emu_folders_make(void)
{
    mkdir_p(emu_root());
    /* the consoles of this build, for the portal's page: "id<TAB>name<TAB>
     * short name<TAB>extensions" a line */
    char list[EMU_PATH_MAX];
    snprintf(list, sizeof list, "%s/consolas.txt", emu_root());
    FILE *lf = fopen(list, "w");
    if (lf) {
        for (int i = 0; emu_cores[i]; i++) {
            const emu_core_t *c = emu_cores[i];
            fprintf(lf, "%s\t%s\t%s\t%s\n", c->id, c->name, c->short_name ? c->short_name : c->name, c->exts);
        }
        fclose(lf);
    }
    for (int i = 0; emu_cores[i]; i++) {
        const emu_core_t *c = emu_cores[i];
        char dir[EMU_PATH_MAX], sub[EMU_PATH_MAX + 16];
        emu_console_dir(c, dir, sizeof dir);
        mkdir_p(dir);
        snprintf(sub, sizeof sub, "%s/saves", dir);
        mkdir_p(sub);
        snprintf(sub, sizeof sub, "%s/states", dir);
        mkdir_p(sub);
        snprintf(sub, sizeof sub, "%s/bios", dir);
        mkdir_p(sub);
        snprintf(sub, sizeof sub, "%s/LEEME.txt", dir);
        if (!emu_file_exists(sub, NULL)) {
            FILE *f = fopen(sub, "w");
            if (f) {
                fprintf(f, "%s\n\n%s: %s\n%s\n", c->name, _("Juegos"), c->exts,
                        _("Copiá acá los juegos de esta consola (también en subcarpetas).\n"
                          "bios/ guarda los archivos de sistema que la consola pueda usar;\n"
                          "saves/ y states/ los escribe el emulador."));
                fclose(f);
            }
        }
    }
}

bool emu_file_exists(const char *path, uint32_t *mtime)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (mtime) *mtime = (uint32_t)st.st_mtime;
    return true;
}

/* ---------------------------------------------------------------------------
 * Games
 * ------------------------------------------------------------------------ */

static bool ext_match(const emu_core_t *core, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    char ext[12];
    size_t n = strlen(dot);
    if (n >= sizeof ext) return false;
    for (size_t i = 0; i <= n; i++) {
        char c = dot[i];
        ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    const char *p = core->exts;
    while (*p) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && strncmp(p, ext, n) == 0) return true;
        p = e;
    }
    return false;
}

/* "Legend of Zelda, The (USA) (Rev 1).nes" -> "The Legend of Zelda" and
 * "(USA) (Rev 1)": the No-Intro and GoodTools names most collections use. */
static void clean_title(const char *file, char *title, size_t tn, char *tags, size_t gn)
{
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    char tmp[EMU_NAME_MAX];
    snprintf(tmp, sizeof tmp, "%s", base);
    char *dot = strrchr(tmp, '.');
    if (dot) *dot = 0;

    tags[0] = 0;
    char *cut = NULL;
    for (char *p = tmp; *p; p++) {
        if ((*p == '(' || *p == '[') && p > tmp) {
            cut = p;
            break;
        }
    }
    if (cut) {
        snprintf(tags, gn, "%s", cut);
        while (cut > tmp && cut[-1] == ' ') cut--;
        *cut = 0;
    }
    /* "Name, The" -> "The Name" */
    size_t n = strlen(tmp);
    static const char *const ARTS[] = { ", The", ", A", ", El", ", La", ", Los", ", Las", ", Die", ", Der", ", Das" };
    for (size_t i = 0; i < sizeof ARTS / sizeof ARTS[0]; i++) {
        size_t an = strlen(ARTS[i]);
        if (n > an && strcmp(tmp + n - an, ARTS[i]) == 0) {
            char moved[EMU_NAME_MAX];
            tmp[n - an] = 0;
            snprintf(moved, sizeof moved, "%.4s %.110s", ARTS[i] + 2, tmp);
            snprintf(tmp, sizeof tmp, "%s", moved);
            break;
        }
    }
    for (char *p = tmp; *p; p++) if (*p == '_') *p = ' ';
    snprintf(title, tn, "%s", tmp[0] ? tmp : base);
}

static bool reserved_dir(const char *name)
{
    return !strcmp(name, "bios") || !strcmp(name, "saves") || !strcmp(name, "states") ||
           !strcmp(name, "covers");
}

typedef struct {
    const emu_core_t *core;
    emu_games_t      *out;
    char              sub[EMU_PATH_MAX];    /* the folder being walked, under the console's */
    char              dirs[8][EMU_NAME_MAX];
    int               ndirs;
} scan_ctx_t;

static bool push_game(emu_games_t *g, const emu_core_t *core, const char *rel, uint32_t size)
{
    if (g->n == g->cap) {
        int cap = g->cap ? g->cap * 2 : 64;
        emu_game_t *v = realloc(g->v, (size_t)cap * sizeof *v);
        if (!v) return false;
        g->v = v;
        g->cap = cap;
    }
    emu_game_t *e = &g->v[g->n++];
    memset(e, 0, sizeof *e);
    e->core = core;
    snprintf(e->rel, sizeof e->rel, "%s", rel);
    e->size = size;
    clean_title(rel, e->title, sizeof e->title, e->tags, sizeof e->tags);
    return true;
}

static bool scan_cb(const aos_dir_entry_t *e, void *vctx)
{
    scan_ctx_t *c = vctx;
    if (e->name[0] == '.') return true;
    if (e->dir) {
        if (!c->sub[0] && reserved_dir(e->name)) return true;
        if (c->ndirs < 8) snprintf(c->dirs[c->ndirs++], EMU_NAME_MAX, "%s", e->name);
        return true;
    }
    if (!ext_match(c->core, e->name)) return true;
    /* system files that share the games' extension: the Neo Geo's BIOS sets */
    if (!strcasecmp(e->name, "neogeo.zip") || !strcasecmp(e->name, "aes.zip")) return true;
    char rel[EMU_PATH_MAX];
    if (c->sub[0]) {
        size_t a = strlen(c->sub), b = strlen(e->name);
        if (a + 1 + b >= sizeof rel) return true;
        memcpy(rel, c->sub, a);
        rel[a] = '/';
        memcpy(rel + a + 1, e->name, b + 1);
    } else {
        snprintf(rel, sizeof rel, "%s", e->name);
    }
    return push_game(c->out, c->core, rel, e->size) && c->out->n < 4000;
}

static void scan_dir(const emu_core_t *core, emu_games_t *out, const char *sub, int depth)
{
    scan_ctx_t *c = calloc(1, sizeof *c);
    if (!c) return;
    c->core = core;
    c->out = out;
    snprintf(c->sub, sizeof c->sub, "%s", sub);
    char dir[EMU_PATH_MAX * 2];
    emu_console_dir(core, dir, sizeof dir);
    if (sub[0]) {
        size_t n = strlen(dir);
        snprintf(dir + n, sizeof dir - n, "/%s", sub);
    }
    aos_hal_dir_scan(dir, scan_cb, c);
    if (depth < 2) {
        for (int i = 0; i < c->ndirs; i++) {
            char next[EMU_PATH_MAX];
            if (sub[0]) {
                size_t a = strlen(sub), b = strlen(c->dirs[i]);
                if (a + 1 + b >= sizeof next) continue;
                memcpy(next, sub, a);
                next[a] = '/';
                memcpy(next + a + 1, c->dirs[i], b + 1);
            } else {
                snprintf(next, sizeof next, "%s", c->dirs[i]);
            }
            scan_dir(core, out, next, depth + 1);
        }
    }
    free(c);
}

int emu_games_scan(const emu_core_t *core, emu_games_t *out)
{
    memset(out, 0, sizeof *out);
    for (int i = 0; emu_cores[i]; i++) {
        if (core && emu_cores[i] != core) continue;
        scan_dir(emu_cores[i], out, "", 0);
    }
    emu_recent_apply(out);
    for (int i = 0; i < out->n; i++) {
        char st[EMU_PATH_MAX];
        emu_game_state(&out->v[i], 0, st, sizeof st);
        out->v[i].has_auto = out->v[i].last && emu_file_exists(st, NULL);
    }
    return out->n;
}

static int cmp_title(const void *pa, const void *pb)
{
    const emu_game_t *a = pa, *b = pb;
    return strcasecmp(a->title, b->title);
}

static int cmp_recent(const void *pa, const void *pb)
{
    const emu_game_t *a = pa, *b = pb;
    if (a->last != b->last) return a->last > b->last ? -1 : 1;
    return strcasecmp(a->title, b->title);
}

void emu_games_sort(emu_games_t *g, bool by_recent)
{
    if (g->n > 1) qsort(g->v, (size_t)g->n, sizeof g->v[0], by_recent ? cmp_recent : cmp_title);
}

void emu_games_free(emu_games_t *g)
{
    free(g->v);
    memset(g, 0, sizeof *g);
}

/* "sub/Some Game.nes" -> "sub_Some Game": a game's name for its saves */
static void flat_base(const emu_game_t *g, char *out, size_t n)
{
    snprintf(out, n, "%s", g->rel);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    for (char *p = out; *p; p++) if (*p == '/') *p = '_';
}

void emu_game_rom(const emu_game_t *g, char *out, size_t n)
{
    char dir[EMU_PATH_MAX];
    emu_console_dir(g->core, dir, sizeof dir);
    snprintf(out, n, "%.120s/%.190s", dir, g->rel);
}

void emu_game_save(const emu_game_t *g, char *out, size_t n)
{
    char dir[EMU_PATH_MAX], base[EMU_PATH_MAX];
    emu_console_dir(g->core, dir, sizeof dir);
    flat_base(g, base, sizeof base);
    snprintf(out, n, "%.120s/saves/%.180s.sav", dir, base);
}

void emu_game_state(const emu_game_t *g, int slot, char *out, size_t n)
{
    char dir[EMU_PATH_MAX], base[EMU_PATH_MAX];
    emu_console_dir(g->core, dir, sizeof dir);
    flat_base(g, base, sizeof base);
    snprintf(out, n, "%.120s/states/%.180s.st%d", dir, base, slot);
}

void emu_game_thumb(const emu_game_t *g, int slot, char *out, size_t n)
{
    char st[EMU_PATH_MAX];
    emu_game_state(g, slot, st, sizeof st);
    snprintf(out, n, "%.300s.thm", st);
}

void emu_game_key(const emu_game_t *g, char *out, size_t n)
{
    snprintf(out, n, "%.8s/%.300s", g->core->id, g->rel);
}

/* ---------------------------------------------------------------------------
 * Recents: "<order>\t<seconds played>\t<console>/<path>" a line, newest
 * first. The order is a counter and not a date: the board has no clock
 * until the network sets it.
 * ------------------------------------------------------------------------ */

#define RECENT_MAX 200

typedef struct {
    uint32_t order, played;
    char     key[EMU_PATH_MAX];
} recent_t;

static int recent_read(recent_t **out)
{
    char path[EMU_PATH_MAX];
    snprintf(path, sizeof path, "%s/recientes.txt", emu_root());
    *out = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    recent_t *v = calloc(RECENT_MAX, sizeof *v);
    if (!v) {
        fclose(f);
        return 0;
    }
    int n = 0;
    char line[EMU_PATH_MAX + 40];
    while (n < RECENT_MAX && fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t1 = *t2 = 0;
        char *key = t2 + 1;
        key[strcspn(key, "\r\n")] = 0;
        v[n].order = (uint32_t)strtoul(line, NULL, 10);
        v[n].played = (uint32_t)strtoul(t1 + 1, NULL, 10);
        snprintf(v[n].key, sizeof v[n].key, "%.300s", key);
        n++;
    }
    fclose(f);
    *out = v;
    return n;
}

void emu_recent_touch(const emu_game_t *g, uint32_t add_seconds)
{
    recent_t *v = NULL;
    int n = recent_read(&v);
    char key[EMU_PATH_MAX];
    emu_game_key(g, key, sizeof key);
    uint32_t top = 0, played = 0;
    for (int i = 0; i < n; i++) {
        if (v[i].order > top) top = v[i].order;
        if (!strcmp(v[i].key, key)) played = v[i].played;
    }
    char path[EMU_PATH_MAX], tmp[EMU_PATH_MAX + 8];
    snprintf(path, sizeof path, "%s/recientes.txt", emu_root());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "%u\t%u\t%s\n", (unsigned)(top + 1), (unsigned)(played + add_seconds), key);
        int kept = 1;
        for (int i = 0; i < n && kept < RECENT_MAX; i++) {
            if (!strcmp(v[i].key, key)) continue;
            fprintf(f, "%u\t%u\t%s\n", (unsigned)v[i].order, (unsigned)v[i].played, v[i].key);
            kept++;
        }
        fclose(f);
        remove(path);
        rename(tmp, path);
    }
    free(v);
}

void emu_recent_apply(emu_games_t *g)
{
    recent_t *v = NULL;
    int n = recent_read(&v);
    for (int i = 0; i < g->n; i++) {
        char key[EMU_PATH_MAX];
        emu_game_key(&g->v[i], key, sizeof key);
        for (int j = 0; j < n; j++) {
            if (!strcmp(v[j].key, key)) {
                g->v[i].last = v[j].order;
                g->v[i].played_s = v[j].played;
                break;
            }
        }
    }
    free(v);
}

bool emu_recent_last(emu_game_t *out)
{
    recent_t *v = NULL;
    int n = recent_read(&v);
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        char *slash = strchr(v[i].key, '/');
        if (!slash) continue;
        *slash = 0;
        const emu_core_t *core = emu_core_by_id(v[i].key);
        if (!core) continue;
        memset(out, 0, sizeof *out);
        out->core = core;
        snprintf(out->rel, sizeof out->rel, "%.300s", slash + 1);
        char rom[EMU_PATH_MAX];
        emu_game_rom(out, rom, sizeof rom);
        if (!emu_file_exists(rom, NULL)) continue;
        clean_title(out->rel, out->title, sizeof out->title, out->tags, sizeof out->tags);
        out->last = v[i].order;
        out->played_s = v[i].played;
        char st[EMU_PATH_MAX];
        emu_game_state(out, 0, st, sizeof st);
        out->has_auto = emu_file_exists(st, NULL);
        found = true;
    }
    free(v);
    return found;
}

/* ---------------------------------------------------------------------------
 * Thumbnails: "ETH1", width and height (16 bits each, little-endian), then
 * the pixels, RGB565 as LVGL takes them. Half the picture's size.
 * ------------------------------------------------------------------------ */

bool emu_thumb_write(const char *path, const uint16_t *px, int w, int h, int stride)
{
    int tw = w / 2, th = h / 2;
    if (tw <= 0 || th <= 0) return false;
    uint16_t *t = emu_psram((size_t)tw * th * 2);
    if (!t) return false;
    for (int y = 0; y < th; y++) {
        const uint16_t *s = (const uint16_t *)((const uint8_t *)px + (size_t)(2 * y) * stride);
        for (int x = 0; x < tw; x++) t[y * tw + x] = s[2 * x];
    }
    char tmp[EMU_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = false;
    if (f) {
        uint8_t hdr[8] = { 'E', 'T', 'H', '1', (uint8_t)tw, (uint8_t)(tw >> 8), (uint8_t)th, (uint8_t)(th >> 8) };
        ok = fwrite(hdr, 1, 8, f) == 8 && fwrite(t, 2, (size_t)tw * th, f) == (size_t)tw * th;
        fclose(f);
        if (ok) {
            remove(path);
            ok = rename(tmp, path) == 0;
        } else {
            remove(tmp);
        }
    }
    free(t);
    return ok;
}

bool emu_thumb_read(const char *path, lv_image_dsc_t *dsc)
{
    memset(dsc, 0, sizeof *dsc);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[8];
    bool ok = false;
    if (fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "ETH1", 4)) {
        int w = hdr[4] | hdr[5] << 8, h = hdr[6] | hdr[7] << 8;
        if (w > 0 && h > 0 && w <= 512 && h <= 512) {
            uint16_t *px = emu_psram((size_t)w * h * 2);
            if (px && fread(px, 2, (size_t)w * h, f) == (size_t)w * h) {
                dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
                dsc->header.cf = LV_COLOR_FORMAT_RGB565;
                dsc->header.w = (uint32_t)w;
                dsc->header.h = (uint32_t)h;
                dsc->header.stride = (uint32_t)w * 2;
                dsc->data = (const uint8_t *)px;
                dsc->data_size = (uint32_t)w * h * 2;
                ok = true;
            } else {
                free(px);
            }
        }
    }
    fclose(f);
    return ok;
}

/* exported by the firmware, not declared by lvgl.h */
extern void lv_image_cache_drop(const void *src);

void emu_thumb_free(lv_image_dsc_t *dsc)
{
    if (dsc->data) {
        lv_image_cache_drop(dsc);
        free((void *)dsc->data);
    }
    memset(dsc, 0, sizeof *dsc);
}

/* ---------------------------------------------------------------------------
 * Settings: ajustes.txt, read once and written on every change (they
 * change by hand, a few times a session).
 * ------------------------------------------------------------------------ */

#define PREF_MAX 64

static struct {
    bool loaded;
    int  n;
    char key[PREF_MAX][32];
    int  val[PREF_MAX];
} P;

static void prefs_path(char *out, size_t n)
{
    snprintf(out, n, "%s/ajustes.txt", emu_root());
}

static void prefs_load(void)
{
    if (P.loaded) return;
    P.loaded = true;
    P.n = 0;
    char path[EMU_PATH_MAX];
    prefs_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[96];
    while (P.n < PREF_MAX && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        snprintf(P.key[P.n], sizeof P.key[0], "%.31s", line);
        P.val[P.n] = (int)strtol(eq + 1, NULL, 10);
        P.n++;
    }
    fclose(f);
}

static void prefs_save(void)
{
    char path[EMU_PATH_MAX], tmp[EMU_PATH_MAX + 8];
    prefs_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs("# Emulador: ajustes (la app reescribe este archivo)\n", f);
    for (int i = 0; i < P.n; i++) fprintf(f, "%s=%d\n", P.key[i], P.val[i]);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

int emu_pref(const char *key, int def)
{
    prefs_load();
    for (int i = 0; i < P.n; i++) {
        if (!strcmp(P.key[i], key)) return P.val[i];
    }
    return def;
}

void emu_pref_set(const char *key, int value)
{
    prefs_load();
    int i = 0;
    while (i < P.n && strcmp(P.key[i], key)) i++;
    if (i == P.n) {
        if (P.n == PREF_MAX) return;
        snprintf(P.key[P.n++], sizeof P.key[0], "%.31s", key);
    } else if (P.val[i] == value) {
        return;
    }
    P.val[i] = value;
    prefs_save();
}

int emu_core_pref(const emu_core_t *core, const char *key, int def)
{
    char k[40];
    snprintf(k, sizeof k, "%s.%.24s", core->id, key);
    return emu_pref(k, def);
}

void emu_core_pref_set(const emu_core_t *core, const char *key, int value)
{
    char k[40];
    snprintf(k, sizeof k, "%s.%.24s", core->id, key);
    emu_pref_set(k, value);
}

/* ---------------------------------------------------------------------------
 * Skins: each console's colours, as the real thing.
 * ------------------------------------------------------------------------ */

/* each console's colours, after the real thing: body, body2, key, key lit,
 * the face buttons A B X Y C Z D, the library's badge, the labels */
#define FACE7(a, b, x, y, c, z, d) { a, b, x, y, c, z, d }
static const struct {
    const char *id;
    emu_skin_t  s;
} SKINS[] = {
    { "nes",    { 0x2A2A2E, 0x161618, 0x3C3C42, 0x6A6A74, FACE7(0xC8102E, 0xC8102E, 0, 0, 0, 0, 0), 0xD62828, 0xE8E2D6 } },
    { "snes",   { 0x2B2B33, 0x17171C, 0x45454F, 0x75757F, FACE7(0xD8352A, 0xF2B705, 0x2E5FD8, 0x2E9E48, 0, 0, 0), 0x6B5FB5, 0xE6E6EE } },
    { "gb",     { 0xC4C1BC, 0x8A8780, 0x2D2D30, 0x5A5A60, FACE7(0x9C2A5E, 0x9C2A5E, 0, 0, 0, 0, 0), 0x8BAC0F, 0xE8E8EC } },
    { "gbc",    { 0x5B3C88, 0x2E1E48, 0x3A3A40, 0x66666E, FACE7(0x3A3A40, 0x3A3A40, 0, 0, 0, 0, 0), 0x2FA8A0, 0xF0F0F0 } },
    { "gba",    { 0x3B2A8C, 0x1E154C, 0x2A2A3A, 0x585878, FACE7(0x6C6C80, 0x6C6C80, 0, 0, 0, 0, 0), 0x6A4CE0, 0xE6E2FF } },
    { "md",     { 0x1C1C1E, 0x0E0E10, 0x333336, 0x5A5A60, FACE7(0x2A2A2D, 0x2A2A2D, 0x3E3E42, 0x3E3E42, 0x2A2A2D, 0x3E3E42, 0), 0xD52B1E, 0xE6E6E6 } },
    { "sms",    { 0x161616, 0x0B0B0B, 0x2E2E2E, 0x56565C, FACE7(0x3A3A3A, 0x3A3A3A, 0, 0, 0, 0, 0), 0xE3262B, 0xF0F0F0 } },
    { "gg",     { 0x1C1C1E, 0x0E0E10, 0x2E2E30, 0x56565C, FACE7(0x3B3B3B, 0x3B3B3B, 0, 0, 0, 0, 0), 0x2F5DA8, 0xF0F0F0 } },
    { "col",    { 0x141414, 0x0A0A0A, 0x5A5A5E, 0x8A8A8A, FACE7(0xE8752A, 0xE8752A, 0, 0, 0, 0, 0), 0xE8752A, 0xF0F0F0 } },
    { "neogeo", { 0x1C1C1C, 0x0A0A0A, 0x34343A, 0x60606A, FACE7(0xD62828, 0xF2C200, 0, 0, 0x2EA84A, 0, 0x2E62D6), 0xC8A23C, 0xF0F0F0 } },
    { "pce",    { 0xE8E8E4, 0xB8B8B4, 0x2A2A2A, 0x55555A, FACE7(0x1C1C1C, 0x1C1C1C, 0, 0, 0, 0, 0), 0xE04A1F, 0xF0F0F0 } },
    { "a2600",  { 0x1C1A18, 0x6B4A2B, 0x2A2A2A, 0x55554F, FACE7(0xC8102E, 0, 0, 0, 0, 0, 0), 0xA8642C, 0xF0E6D8 } },
    { "a7800",  { 0x1A1A1A, 0x0D0D0D, 0x2E2E2E, 0x6E6E6E, FACE7(0xD0202E, 0xD0202E, 0, 0, 0, 0, 0), 0xA7A9AC, 0xF0F0F0 } },
    { "lynx",   { 0x2B2B2E, 0x151517, 0x3A3A3D, 0x5A5A60, FACE7(0x4A4A4F, 0x4A4A4F, 0, 0, 0, 0, 0), 0xE08A1E, 0xF0F0F0 } },
};
static const emu_skin_t SKIN_ANY = {
    0x24243A, 0x121220, 0x34344E, 0x5C5C80,
    FACE7(0xFF2D95, 0x7B5CFF, 0x30D158, 0xFFD60A, 0x40C8E0, 0xFF9F0A, 0xBF5AF2), 0x7B5CFF, 0xF2F2F7,
};

const emu_skin_t *emu_skin(const emu_core_t *core)
{
    for (size_t i = 0; core && i < sizeof SKINS / sizeof SKINS[0]; i++) {
        if (!strcmp(core->id, SKINS[i].id)) return &SKINS[i].s;
    }
    return &SKIN_ANY;
}
