/*
 * P4OS - Emulador: the consoles there are, and loading the one that plays.
 *
 * On the board every core is a module of its own, the card's
 * apps/emulador/<dir>.so (the app's CMakeLists.txt builds them), and a
 * console costs memory only while it plays. The library still has to know
 * every console, though: its name, its folder, its extensions, the shape of
 * its picture. Opening every module to read that would cost seconds at each
 * start, so what each module's tables say is kept in a cache on the card
 * (emulador/.modulos.txt), keyed by the module's name, size and date; a
 * module the cache does not know is opened once, read and closed.
 *
 * What the library gets is a copy of each table with its metadata only
 * (its functions NULL): a "shadow". emu_core_open() turns a shadow into the
 * module's real table for a game, and emu_core_close() lets the module go
 * when the game ends; nothing may keep a pointer into the module after
 * that (labels, options, the functions).
 *
 * In the simulator the cores are linked into the app (sim.cmake, by
 * EMU_HAVE_<ID>) and their tables are their own shadows.
 */
#include "emu.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONSOLES_MAX 32

const emu_core_t *const *emu_cores;
static const emu_core_t *s_list[CONSOLES_MAX + 1];

/* A number for a console that does not move when consoles are added or
 * left out (for settings that name a console): its place in this list,
 * which only ever grows at the end. */
static const char *const STABLE[] = {
    "nes", "gba", "snes", "gb", "gbc", "md", "sms", "gg", "col", "neogeo", "pce",
    "a2600", "a7800", "lynx",
};

int emu_core_code(const emu_core_t *core)
{
    for (size_t i = 0; i < sizeof STABLE / sizeof STABLE[0]; i++) {
        if (!strcmp(STABLE[i], core->id)) return (int)i + 1;
    }
    return 0;
}

const emu_core_t *emu_core_by_id(const char *id)
{
    for (int i = 0; emu_cores && emu_cores[i]; i++) {
        if (strcmp(emu_cores[i]->id, id) == 0) return emu_cores[i];
    }
    return NULL;
}

/* The library's order: the stable list's, then any newer console by id. */
static int order_cmp(const void *pa, const void *pb)
{
    const emu_core_t *a = *(const emu_core_t *const *)pa, *b = *(const emu_core_t *const *)pb;
    int ca = emu_core_code(a), cb = emu_core_code(b);
    if (!ca) ca = 1000;
    if (!cb) cb = 1000;
    if (ca != cb) return ca - cb;
    return strcmp(a->id, b->id);
}

#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)

/* ---------------------------------------------------------------------------
 * The simulator: the cores are linked in
 * ------------------------------------------------------------------------ */

#define CORE(id) extern const emu_core_t emu_core_##id;
CORE(nes) CORE(snes) CORE(gb) CORE(gbc) CORE(gba) CORE(sms) CORE(gg) CORE(col)
CORE(md) CORE(pce) CORE(neogeo) CORE(a2600) CORE(a7800) CORE(lynx)
#undef CORE

void emu_cores_scan(void)
{
    int n = 0;
#define ADD(id) s_list[n++] = &emu_core_##id;
#ifdef EMU_HAVE_NES
    ADD(nes)
#endif
#ifdef EMU_HAVE_SNES
    ADD(snes)
#endif
#ifdef EMU_HAVE_GB
    ADD(gb)
#endif
#ifdef EMU_HAVE_GBC
    ADD(gbc)
#endif
#ifdef EMU_HAVE_GBA
    ADD(gba)
#endif
#ifdef EMU_HAVE_MD
    ADD(md)
#endif
#ifdef EMU_HAVE_SMS
    ADD(sms)
#endif
#ifdef EMU_HAVE_GG
    ADD(gg)
#endif
#ifdef EMU_HAVE_COL
    ADD(col)
#endif
#ifdef EMU_HAVE_NEOGEO
    ADD(neogeo)
#endif
#ifdef EMU_HAVE_PCE
    ADD(pce)
#endif
#ifdef EMU_HAVE_A2600
    ADD(a2600)
#endif
#ifdef EMU_HAVE_A7800
    ADD(a7800)
#endif
#ifdef EMU_HAVE_LYNX
    ADD(lynx)
#endif
#undef ADD
    qsort(s_list, (size_t)n, sizeof s_list[0], order_cmp);
    s_list[n] = NULL;
    emu_cores = s_list;
}

const emu_core_t *emu_core_open(const emu_core_t *shadow, char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    return shadow;
}

void emu_core_close(void) {}

void emu_cores_free(void) {}

#else

/* ---------------------------------------------------------------------------
 * The board: modules on the card
 * ------------------------------------------------------------------------ */

typedef struct {
    emu_core_t  shadow;
    char        id[16], name[48], short_name[12], exts[64];
    char        file[32];               /* "gb": the module, without ".so" */
    int         index;                  /* its table in the module         */
} console_t;

static console_t     *s_cons;
static int            s_n;
static aos_module_t  *s_open;           /* the module of the game playing */

typedef int (*abi_fn_t)(void);
typedef const emu_core_t *(*core_fn_t)(int);

static void mod_dir(char *out, size_t n)
{
    snprintf(out, n, "%s/emulador", aos_hal_path_apps());
}

static void cache_path(char *out, size_t n)
{
    snprintf(out, n, "%s/.modulos.txt", emu_root());
}

static console_t *add(void)
{
    if (s_n >= CONSOLES_MAX) return NULL;
    console_t *c = &s_cons[s_n++];
    memset(c, 0, sizeof *c);
    return c;
}

static void shadow_fix(console_t *c)
{
    c->shadow.id = c->id;
    c->shadow.name = c->name;
    c->shadow.short_name = c->short_name[0] ? c->short_name : NULL;
    c->shadow.exts = c->exts;
}

static void copy_meta(console_t *c, const emu_core_t *t, const char *file, int index)
{
    snprintf(c->id, sizeof c->id, "%s", t->id);
    snprintf(c->name, sizeof c->name, "%s", t->name);
    snprintf(c->short_name, sizeof c->short_name, "%s", t->short_name ? t->short_name : "");
    snprintf(c->exts, sizeof c->exts, "%s", t->exts);
    snprintf(c->file, sizeof c->file, "%s", file);
    c->index = index;
    c->shadow.max_w = t->max_w;
    c->shadow.max_h = t->max_h;
    c->shadow.pixel_aspect = t->pixel_aspect;
    c->shadow.players = t->players;
    c->shadow.buttons = t->buttons;
    c->shadow.face = t->face;
    shadow_fix(c);
}

/* The cache: one line per table,
 *   file size mtime index id name short exts max_w max_h players buttons face aspect*1000
 * separated by tabs. */
typedef struct {
    char     file[32];
    uint32_t size, mtime;
} seen_t;

static bool cache_read(const char *file, uint32_t size, uint32_t mtime)
{
    char path[EMU_PATH_MAX];
    cache_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool found = false;
    char line[320];
    while (fgets(line, sizeof line, f)) {
        char *field[14];
        int nf = 0;
        char *p = line;
        while (nf < 14) {
            field[nf++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        if (nf < 14) continue;
        field[13][strcspn(field[13], "\r\n")] = 0;
        if (strcmp(field[0], file) || (uint32_t)strtoul(field[1], NULL, 10) != size ||
            (uint32_t)strtoul(field[2], NULL, 10) != mtime) {
            continue;
        }
        console_t *c = add();
        if (!c) break;
        snprintf(c->file, sizeof c->file, "%s", file);
        c->index = atoi(field[3]);
        snprintf(c->id, sizeof c->id, "%s", field[4]);
        snprintf(c->name, sizeof c->name, "%s", field[5]);
        snprintf(c->short_name, sizeof c->short_name, "%s", field[6]);
        snprintf(c->exts, sizeof c->exts, "%s", field[7]);
        c->shadow.max_w = atoi(field[8]);
        c->shadow.max_h = atoi(field[9]);
        c->shadow.players = atoi(field[10]);
        c->shadow.buttons = (uint32_t)strtoul(field[11], NULL, 10);
        c->shadow.face = (emu_face_t)atoi(field[12]);
        c->shadow.pixel_aspect = atoi(field[13]) / 1000.0f;
        shadow_fix(c);
        found = true;
    }
    fclose(f);
    return found;
}

static void cache_write(const seen_t *seen, int nseen)
{
    char path[EMU_PATH_MAX], tmp[EMU_PATH_MAX + 8];
    cache_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < s_n; i++) {
        const console_t *c = &s_cons[i];
        const seen_t *s = NULL;
        for (int j = 0; j < nseen; j++) if (!strcmp(seen[j].file, c->file)) s = &seen[j];
        if (!s) continue;
        fprintf(f, "%s\t%u\t%u\t%d\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%u\t%d\t%d\n", c->file, (unsigned)s->size,
                (unsigned)s->mtime, c->index, c->id, c->name, c->short_name, c->exts, c->shadow.max_w,
                c->shadow.max_h, c->shadow.players, (unsigned)c->shadow.buttons, (int)c->shadow.face,
                (int)(c->shadow.pixel_aspect * 1000.0f + 0.5f));
    }
    fclose(f);
    remove(path);
    rename(tmp, path);
}

/* A module the cache does not know: open it, copy what its tables say. */
static bool probe(const char *file)
{
    char dir[EMU_PATH_MAX / 2], path[EMU_PATH_MAX], err[96] = "";
    mod_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s.so", dir, file);
    uint64_t t0 = aos_hal_uptime_us();
    aos_module_t *m = aos_hal_module_open(path, err, sizeof err);
    if (!m) {
        aos_hal_log(EMU_TAG, "module %s: %s", file, err);
        return false;
    }
    abi_fn_t abi = (abi_fn_t)aos_hal_module_sym(m, "emu_module_abi");
    core_fn_t core = (core_fn_t)aos_hal_module_sym(m, "emu_module_core");
    bool ok = abi && core && abi() == EMU_CORE_ABI;
    if (!ok) {
        aos_hal_log(EMU_TAG, "module %s: contract %d, the app's is %d: rebuild it", file, abi ? abi() : -1,
                    EMU_CORE_ABI);
    } else {
        for (int i = 0; ; i++) {
            const emu_core_t *t = core(i);
            if (!t) break;
            console_t *c = add();
            if (!c) break;
            copy_meta(c, t, file, i);
        }
    }
    aos_hal_module_close(m);
    aos_hal_log(EMU_TAG, "module %s read in %u ms", file, (unsigned)((aos_hal_uptime_us() - t0) / 1000));
    return ok;
}

typedef struct {
    seen_t v[CONSOLES_MAX];
    int    n;
} scan_t;

static bool mod_cb(const aos_dir_entry_t *e, void *ctx)
{
    scan_t *s = ctx;
    size_t n = strlen(e->name);
    if (e->dir || e->name[0] == '.' || n < 4 || n - 3 >= sizeof s->v[0].file || strcmp(e->name + n - 3, ".so")) {
        return true;
    }
    if (s->n >= CONSOLES_MAX) return false;
    seen_t *v = &s->v[s->n++];
    memcpy(v->file, e->name, n - 3);
    v->file[n - 3] = 0;
    v->size = e->size;
    v->mtime = e->mtime;
    return true;
}

void emu_cores_scan(void)
{
    if (!s_cons) s_cons = calloc(CONSOLES_MAX, sizeof *s_cons);
    s_n = 0;
    s_list[0] = NULL;
    emu_cores = s_list;
    if (!s_cons) return;

    scan_t *sc = calloc(1, sizeof *sc);
    if (!sc) return;
    char dir[EMU_PATH_MAX];
    mod_dir(dir, sizeof dir);
    uint64_t t0 = aos_hal_uptime_us();
    aos_hal_dir_scan(dir, mod_cb, sc);
    bool changed = false;
    for (int i = 0; i < sc->n; i++) {
        if (cache_read(sc->v[i].file, sc->v[i].size, sc->v[i].mtime)) continue;
        probe(sc->v[i].file);
        changed = true;
    }
    if (changed) cache_write(sc->v, sc->n);
    free(sc);

    for (int i = 0; i < s_n; i++) s_list[i] = &s_cons[i].shadow;
    qsort(s_list, (size_t)s_n, sizeof s_list[0], order_cmp);
    s_list[s_n] = NULL;
    aos_hal_log(EMU_TAG, "%d consoles in %s, %u ms", s_n, dir, (unsigned)((aos_hal_uptime_us() - t0) / 1000));
}

static console_t *of_shadow(const emu_core_t *shadow)
{
    for (int i = 0; i < s_n; i++) {
        if (&s_cons[i].shadow == shadow) return &s_cons[i];
    }
    return NULL;
}

const emu_core_t *emu_core_open(const emu_core_t *shadow, char *err, size_t errlen)
{
    console_t *c = of_shadow(shadow);
    if (!c) {
        snprintf(err, errlen, "%s", _("No está el módulo de esta consola"));
        return NULL;
    }
    emu_core_close();
    char dir[EMU_PATH_MAX / 2], path[EMU_PATH_MAX], why[96] = "";
    mod_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s.so", dir, c->file);
    uint64_t t0 = aos_hal_uptime_us();
    s_open = aos_hal_module_open(path, why, sizeof why);
    if (!s_open) {
        snprintf(err, errlen, "%s %s.so: %s", _("No se pudo cargar el módulo"), c->file, why);
        return NULL;
    }
    abi_fn_t abi = (abi_fn_t)aos_hal_module_sym(s_open, "emu_module_abi");
    core_fn_t core = (core_fn_t)aos_hal_module_sym(s_open, "emu_module_core");
    const emu_core_t *t = abi && core && abi() == EMU_CORE_ABI ? core(c->index) : NULL;
    if (!t || strcmp(t->id, c->id)) {
        snprintf(err, errlen, "%s (%s.so)", _("El módulo es de otra versión del emulador"), c->file);
        emu_core_close();
        return NULL;
    }
    aos_hal_log(EMU_TAG, "module %s.so loaded in %u ms", c->file, (unsigned)((aos_hal_uptime_us() - t0) / 1000));
    return t;
}

void emu_core_close(void)
{
    if (s_open) {
        aos_hal_module_close(s_open);
        s_open = NULL;
    }
}

void emu_cores_free(void)
{
    emu_core_close();
    free(s_cons);
    s_cons = NULL;
    s_n = 0;
    s_list[0] = NULL;
}

#endif
