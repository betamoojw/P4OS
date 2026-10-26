/*
 * P4OS - Cartero: the files on the card (README, "The files").
 *
 * Collections and environments are Postman's own JSON, read whole into a
 * document (an arena and the value tree). Nothing of a file is thrown away:
 * a request is changed inside the tree and the tree written back, so the
 * fields Cartero does not know (descriptions, saved examples, Postman's
 * ids) survive the round trip.
 *
 * The portal's page writes the same files. Before changing one, the file is
 * read again if its date or size moved, and only the item or the variable
 * being changed is touched: the board and an open page do not undo each
 * other. After a write the document is read back from the card, which also
 * gives its arena back the garbage of the edit.
 */
#include "ct_app.h"

#include "aos_hal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define FILE_MAX   (8u * 1024 * 1024)
#define DOC_ARENA  (40u * 1024 * 1024)

static const char *const SCHEMA = "https://schema.getpostman.com/json/collection/v2.1.0/collection.json";

/* ---- paths and files --------------------------------------------------------------------- */

const char *ct_dir(void)
{
    static char d[160];
    if (!d[0]) snprintf(d, sizeof d, "%.140s/cartero", aos_hal_path_data());
    return d;
}

void ct_path(char *out, size_t n, const char *rel)
{
    snprintf(out, n, "%s/%s", ct_dir(), rel);
}

bool ct_write_file(const char *path, const void *data, size_t len)
{
    char tmp[340];
    snprintf(tmp, sizeof tmp, "%.320s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = !len || fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

char *ct_read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (size_t)n > max) {
        fclose(f);
        return NULL;
    }
    char *p = ct_psram((size_t)n + 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    size_t k = n ? fread(p, 1, (size_t)n, f) : 0;
    fclose(f);
    p[k] = '\0';
    if (len) *len = k;
    return p;
}

static bool file_stat(const char *path, uint32_t *mtime, uint32_t *size)
{
    struct stat sb;
    if (stat(path, &sb) != 0) return false;
    if (mtime) *mtime = (uint32_t)sb.st_mtime;
    if (size) *size = (uint32_t)sb.st_size;
    return true;
}

/* a name the card takes: lower case, a-z 0-9 and '-', no accents (as the
 * portal's fsSlug) */
void ct_slug(char *out, size_t n, const char *name)
{
    static const struct { const char *u; char a; } ACC[] = {
        { "á", 'a' }, { "é", 'e' }, { "í", 'i' }, { "ó", 'o' }, { "ú", 'u' }, { "ü", 'u' }, { "ñ", 'n' },
        { "Á", 'a' }, { "É", 'e' }, { "Í", 'i' }, { "Ó", 'o' }, { "Ú", 'u' }, { "Ü", 'u' }, { "Ñ", 'n' },
        { "ç", 'c' }, { "à", 'a' }, { "è", 'e' }, { "ò", 'o' }, { "ö", 'o' }, { "ä", 'a' },
    };
    size_t k = 0;
    bool dash = false;
    for (const char *p = name ? name : ""; *p && k + 1 < n;) {
        char c = *p;
        if ((unsigned char)c >= 0x80) {
            bool hit = false;
            for (size_t i = 0; i < sizeof ACC / sizeof ACC[0]; i++) {
                size_t l = strlen(ACC[i].u);
                if (!strncmp(p, ACC[i].u, l)) {
                    out[k++] = ACC[i].a;
                    p += l;
                    hit = true;
                    dash = false;
                    break;
                }
            }
            if (hit) continue;
            p++;
            continue;
        }
        p++;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[k++] = c;
            dash = false;
        } else if (k && !dash) {
            out[k++] = '-';
            dash = true;
        }
        if (k >= 48) break;
    }
    while (k && out[k - 1] == '-') k--;
    out[k] = '\0';
    if (!k) ct_copy(out, n, "sin-nombre");
}

/* ---- documents --------------------------------------------------------------------------------- */

static void doc_free(ct_doc_t *d)
{
    ct_arena_free(&d->ar);
    memset(d, 0, sizeof *d);
}

static void doc_name(ct_doc_t *d)
{
    const char *n = ct_s(ct_get(ct_get(d->root, "info"), "name"));
    if (!n[0]) n = ct_s(ct_get(d->root, "name"));
    ct_copy(d->name, sizeof d->name, n[0] ? n : d->slug);
}

static bool doc_load(ct_doc_t *d, const char *dir, const char *slug)
{
    memset(d, 0, sizeof *d);
    ct_arena_init(&d->ar, DOC_ARENA);
    ct_copy(d->slug, sizeof d->slug, slug);
    char path[320];
    if (dir) snprintf(path, sizeof path, "%s/%s/%s.json", ct_dir(), dir, slug);
    else snprintf(path, sizeof path, "%s/%s.json", ct_dir(), slug);
    file_stat(path, &d->mtime, &d->size);
    size_t len = 0;
    char *text = ct_read_file(path, FILE_MAX, &len);
    if (!text) {
        d->root = ct_undef();
        d->bad = d->size > 0;
        if (d->bad) ct_copy(d->err, sizeof d->err, "too big or unreadable");
        doc_name(d);
        return false;
    }
    char err[160];
    if (!ct_json_parse(&d->ar, text, len, &d->root, err, sizeof err)) {
        d->bad = true;
        ct_copy(d->err, sizeof d->err, err);
        d->root = ct_undef();
        aos_hal_log(CT_TAG, "%s: %s", path, err);
    }
    free(text);
    doc_name(d);
    return !d->bad;
}

bool ct_doc_save(ct_doc_t *d, const char *dir)
{
    if (d->bad || d->root.t == CT_UNDEF) return false;
    ct_sb_t b = { 0 };
    ct_json_write(&b, d->root, -1);
    ct_sb_addc(&b, '\n');
    if (b.oom) {
        ct_sb_free(&b);
        return false;
    }
    char path[320];
    if (dir) snprintf(path, sizeof path, "%s/%s/%s.json", ct_dir(), dir, d->slug);
    else snprintf(path, sizeof path, "%s/%s.json", ct_dir(), d->slug);
    bool ok = ct_write_file(path, b.p, b.n);
    ct_sb_free(&b);
    if (!ok) {
        aos_hal_log(CT_TAG, "could not write %s", path);
        return false;
    }
    /* read back: the arena loses the edit's garbage, the date is the file's */
    char slug[64];
    ct_copy(slug, sizeof slug, d->slug);
    doc_free(d);
    doc_load(d, dir, slug);
    return true;
}

bool ct_doc_refresh(ct_doc_t *d, const char *dir)
{
    char path[320];
    if (dir) snprintf(path, sizeof path, "%s/%s/%s.json", ct_dir(), dir, d->slug);
    else snprintf(path, sizeof path, "%s/%s.json", ct_dir(), d->slug);
    uint32_t mt = 0, sz = 0;
    bool there = file_stat(path, &mt, &sz);
    if (there && mt == d->mtime && sz == d->size && !d->bad) return false;
    if (!there && d->root.t == CT_UNDEF) return false;
    char slug[64];
    ct_copy(slug, sizeof slug, d->slug);
    doc_free(d);
    doc_load(d, dir, slug);
    return true;
}

/* ---- the folders ------------------------------------------------------------------------------- */

typedef struct {
    char     slug[64];
    uint32_t mtime, size;
} entry_t;

static int list_dir(const char *dir, entry_t *out, int max)
{
    char path[300];
    snprintf(path, sizeof path, "%s/%s", ct_dir(), dir);
    DIR *d = opendir(path);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (nm[0] == '.' || l < 6 || l - 5 >= sizeof out[n].slug || strcmp(nm + l - 5, ".json")) continue;
        memcpy(out[n].slug, nm, l - 5);
        out[n].slug[l - 5] = '\0';
        char fp[400];
        snprintf(fp, sizeof fp, "%.140s/%.250s", path, nm);
        file_stat(fp, &out[n].mtime, &out[n].size);
        n++;
    }
    closedir(d);
    return n;
}

static int by_slug(const void *a, const void *b)
{
    return strcmp(((const entry_t *)a)->slug, ((const entry_t *)b)->slug);
}

static bool same_listing(const ct_doc_t *docs, int n, const entry_t *e, int ne)
{
    if (n != ne) return false;
    for (int i = 0; i < n; i++) {
        if (strcmp(docs[i].slug, e[i].slug) || docs[i].mtime != e[i].mtime || docs[i].size != e[i].size) return false;
    }
    return true;
}

static int by_name(const void *a, const void *b)
{
    const ct_doc_t *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->slug, y->slug);
}

/* the folder read again, if anything in it changed; the documents sorted by
 * their names */
static bool load_set(ct_doc_t *docs, int *n, int max, const char *dir, bool force)
{
    entry_t *e = ct_psram(sizeof(entry_t) * (size_t)max);
    if (!e) return false;
    int ne = list_dir(dir, e, max);
    qsort(e, (size_t)ne, sizeof *e, by_slug);
    /* compare against the documents in slug order */
    ct_doc_t *sorted = ct_psram(sizeof(ct_doc_t) * (size_t)(*n ? *n : 1));
    bool same = false;
    if (sorted && !force) {
        memcpy(sorted, docs, sizeof(ct_doc_t) * (size_t)*n);
        for (int i = 1; i < *n; i++) {
            for (int j = i; j > 0 && strcmp(sorted[j - 1].slug, sorted[j].slug) > 0; j--) {
                ct_doc_t t = sorted[j];
                sorted[j] = sorted[j - 1];
                sorted[j - 1] = t;
            }
        }
        same = same_listing(sorted, *n, e, ne);
    }
    free(sorted);
    if (same) {
        free(e);
        return false;
    }
    /* keep what did not change, read what did */
    ct_doc_t *old = ct_psram(sizeof(ct_doc_t) * (size_t)(*n ? *n : 1));
    int nold = *n;
    if (old) memcpy(old, docs, sizeof(ct_doc_t) * (size_t)nold);
    int k = 0;
    for (int i = 0; i < ne && k < max; i++) {
        int reuse = -1;
        for (int j = 0; old && j < nold; j++) {
            if (old[j].ar.head && !strcmp(old[j].slug, e[i].slug) && old[j].mtime == e[i].mtime &&
                old[j].size == e[i].size) {
                reuse = j;
                break;
            }
        }
        if (reuse >= 0) {
            docs[k] = old[reuse];
            old[reuse].ar.head = NULL;      /* handed over */
        } else {
            memset(&docs[k], 0, sizeof docs[k]);
            doc_load(&docs[k], dir, e[i].slug);
        }
        k++;
    }
    for (int j = 0; old && j < nold; j++) if (old[j].ar.head) ct_arena_free(&old[j].ar);
    free(old);
    *n = k;
    qsort(docs, (size_t)k, sizeof *docs, by_name);
    free(e);
    return true;
}

static void state_load(ct_store_t *st)
{
    char path[300];
    ct_path(path, sizeof path, "estado.json");
    file_stat(path, &st->state_mtime, NULL);
    size_t len;
    char *t = ct_read_file(path, 64 * 1024, &len);
    char active[64] = "";
    st->timeout_ms = 30000;
    st->follow = true;
    st->max_kb = 4096;
    if (t) {
        ct_arena_t a;
        ct_arena_init(&a, 1 << 20);
        ctv_t v;
        if (ct_json_parse(&a, t, len, &v, NULL, 0)) {
            ct_copy(active, sizeof active, ct_s(ct_get(v, "entorno")));
            ctv_t aj = ct_get(v, "ajustes");
            ctv_t x = ct_get(aj, "timeout");
            if (x.t == CT_NUM && x.n >= 1000 && x.n <= 300000) st->timeout_ms = (int)x.n;
            x = ct_get(aj, "seguir");
            if (x.t == CT_BOOL) st->follow = x.b;
            x = ct_get(aj, "max_kb");
            if (x.t == CT_NUM && x.n >= 16 && x.n <= 16384) st->max_kb = (int)x.n;
        }
        ct_arena_free(&a);
        free(t);
    }
    st->active = -1;
    for (int i = 0; i < st->nenv; i++) if (!strcmp(st->env[i].slug, active)) st->active = i;
}

void ct_state_save(ct_store_t *st)
{
    ct_sb_t b = { 0 };
    ct_sb_adds(&b, "{\n\t\"entorno\": ");
    const char *slug = st->active >= 0 && st->active < st->nenv ? st->env[st->active].slug : "";
    ct_json_str(&b, slug, strlen(slug));
    ct_sb_addf(&b, ",\n\t\"ajustes\": {\n\t\t\"timeout\": %d,\n\t\t\"seguir\": %s,\n\t\t\"max_kb\": %d\n\t}\n}\n",
               st->timeout_ms, st->follow ? "true" : "false", st->max_kb);
    char path[300];
    ct_path(path, sizeof path, "estado.json");
    if (!b.oom) ct_write_file(path, b.p, b.n);
    ct_sb_free(&b);
    file_stat(path, &st->state_mtime, NULL);
}

const char *ct_env_name(const ct_store_t *st)
{
    return st->active >= 0 && st->active < st->nenv ? st->env[st->active].name : "";
}

static void mkdirs(void)
{
    mkdir(aos_hal_path_data(), 0777);
    mkdir(ct_dir(), 0777);
    static const char *const SUB[] = { "colecciones", "entornos", "corridas", "respuestas", "web" };
    for (size_t i = 0; i < sizeof SUB / sizeof SUB[0]; i++) {
        char p[300];
        ct_path(p, sizeof p, SUB[i]);
        mkdir(p, 0777);
    }
}

void ct_store_load(ct_store_t *st)
{
    mkdirs();
    load_set(st->coll, &st->ncoll, CT_COLL_MAX, "colecciones", true);
    load_set(st->env, &st->nenv, CT_ENV_MAX, "entornos", true);
    doc_load(&st->glob, NULL, "globales");
    if (st->glob.root.t != CT_OBJ && !st->glob.bad) {
        st->glob.root = ct_obj_new(&st->glob.ar, 4);
        ct_sets(&st->glob.ar, st->glob.root, "name", "Globales");
        ct_setv(&st->glob.ar, st->glob.root, "values", ct_arr_new(&st->glob.ar, 0));
        ct_sets(&st->glob.ar, st->glob.root, "_postman_variable_scope", "globals");
    }
    doc_load(&st->hist, NULL, "historial");
    state_load(st);
}

void ct_store_free(ct_store_t *st)
{
    for (int i = 0; i < st->ncoll; i++) doc_free(&st->coll[i]);
    for (int i = 0; i < st->nenv; i++) doc_free(&st->env[i]);
    doc_free(&st->glob);
    doc_free(&st->hist);
    memset(st, 0, sizeof *st);
    st->active = -1;
}

bool ct_store_poll(ct_store_t *st)
{
    char active[64] = "";
    if (st->active >= 0 && st->active < st->nenv) ct_copy(active, sizeof active, st->env[st->active].slug);
    bool ch = load_set(st->coll, &st->ncoll, CT_COLL_MAX, "colecciones", false);
    bool eh = load_set(st->env, &st->nenv, CT_ENV_MAX, "entornos", false);
    if (eh) {
        st->active = -1;
        for (int i = 0; i < st->nenv; i++) if (!strcmp(st->env[i].slug, active)) st->active = i;
    }
    bool gh = ct_doc_refresh(&st->glob, NULL);
    ct_doc_refresh(&st->hist, NULL);
    char path[300];
    ct_path(path, sizeof path, "estado.json");
    uint32_t mt = 0;
    bool sh = false;
    if (file_stat(path, &mt, NULL) && mt != st->state_mtime) {
        state_load(st);
        sh = true;
    }
    return ch || eh || gh || sh;
}

static int find_slug(const ct_doc_t *docs, int n, const char *slug)
{
    for (int i = 0; i < n; i++) if (!strcmp(docs[i].slug, slug)) return i;
    return -1;
}

static void unique_slug(const ct_doc_t *docs, int n, const char *name, char *out, size_t on)
{
    char base[56];
    ct_slug(base, sizeof base, name);
    ct_copy(out, on, base);
    for (int k = 2; find_slug(docs, n, out) >= 0 && k < 1000; k++) snprintf(out, on, "%s-%d", base, k);
}

static void make_uuid(char out[37])
{
    static const char HEX[] = "0123456789abcdef";
    uint32_t s = (uint32_t)aos_hal_uptime_us() | 1;
    for (int i = 0; i < 36; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        out[i] = (i == 8 || i == 13 || i == 18 || i == 23) ? '-' : i == 14 ? '4' : HEX[s & 15];
    }
    out[36] = '\0';
}

int ct_coll_new(ct_store_t *st, const char *name)
{
    if (st->ncoll >= CT_COLL_MAX) return -1;
    char slug[64];
    unique_slug(st->coll, st->ncoll, name, slug, sizeof slug);
    ct_arena_t a;
    ct_arena_init(&a, 1 << 20);
    ctv_t root = ct_obj_new(&a, 4), info = ct_obj_new(&a, 3);
    char id[37];
    make_uuid(id);
    ct_sets(&a, info, "_postman_id", id);
    ct_sets(&a, info, "name", name);
    ct_sets(&a, info, "schema", SCHEMA);
    ct_setv(&a, root, "info", info);
    ct_setv(&a, root, "item", ct_arr_new(&a, 0));
    ct_setv(&a, root, "variable", ct_arr_new(&a, 0));
    ct_sb_t b = { 0 };
    ct_json_write(&b, root, -1);
    ct_sb_addc(&b, '\n');
    char rel[120], path[300];
    snprintf(rel, sizeof rel, "colecciones/%s.json", slug);
    ct_path(path, sizeof path, rel);
    bool ok = !b.oom && ct_write_file(path, b.p, b.n);
    ct_sb_free(&b);
    ct_arena_free(&a);
    if (!ok) return -1;
    load_set(st->coll, &st->ncoll, CT_COLL_MAX, "colecciones", false);
    return find_slug(st->coll, st->ncoll, slug);
}

bool ct_coll_delete(ct_store_t *st, int c)
{
    if (c < 0 || c >= st->ncoll) return false;
    char rel[120], path[300];
    snprintf(rel, sizeof rel, "colecciones/%s.json", st->coll[c].slug);
    ct_path(path, sizeof path, rel);
    bool ok = remove(path) == 0;
    load_set(st->coll, &st->ncoll, CT_COLL_MAX, "colecciones", false);
    return ok;
}

int ct_env_new(ct_store_t *st, const char *name)
{
    if (st->nenv >= CT_ENV_MAX) return -1;
    char slug[64];
    unique_slug(st->env, st->nenv, name, slug, sizeof slug);
    char active[64] = "";
    if (st->active >= 0) ct_copy(active, sizeof active, st->env[st->active].slug);
    ct_sb_t b = { 0 };
    char id[37];
    make_uuid(id);
    ct_sb_addf(&b, "{\n\t\"id\": \"%s\",\n\t\"name\": ", id);
    ct_json_str(&b, name, strlen(name));
    ct_sb_adds(&b, ",\n\t\"values\": [],\n\t\"_postman_variable_scope\": \"environment\"\n}\n");
    char rel[120], path[300];
    snprintf(rel, sizeof rel, "entornos/%s.json", slug);
    ct_path(path, sizeof path, rel);
    bool ok = !b.oom && ct_write_file(path, b.p, b.n);
    ct_sb_free(&b);
    if (!ok) return -1;
    load_set(st->env, &st->nenv, CT_ENV_MAX, "entornos", false);
    st->active = find_slug(st->env, st->nenv, active);
    return find_slug(st->env, st->nenv, slug);
}

bool ct_env_delete(ct_store_t *st, int e)
{
    if (e < 0 || e >= st->nenv) return false;
    char active[64] = "";
    if (st->active >= 0 && st->active != e) ct_copy(active, sizeof active, st->env[st->active].slug);
    char rel[120], path[300];
    snprintf(rel, sizeof rel, "entornos/%s.json", st->env[e].slug);
    ct_path(path, sizeof path, rel);
    bool ok = remove(path) == 0;
    load_set(st->env, &st->nenv, CT_ENV_MAX, "entornos", false);
    st->active = find_slug(st->env, st->nenv, active);
    ct_state_save(st);
    return ok;
}

/* ---- the items of a collection ---------------------------------------------------------------- */

ctv_t ct_items(ctv_t node)
{
    ctv_t it = ct_get(node, "item");
    return it.t == CT_ARR ? it : ct_undef();
}

bool ct_is_folder(ctv_t item)
{
    return ct_get(item, "item").t == CT_ARR;
}

const char *ct_item_name(ctv_t item)
{
    const char *n = ct_s(ct_get(item, "name"));
    return n[0] ? n : ct_s(ct_get(ct_get(item, "info"), "name"));
}

static int coll_index(const ct_store_t *st, const ct_ref_t *r)
{
    if (r->coll >= 0 && r->coll < st->ncoll && !strcmp(st->coll[r->coll].slug, r->slug)) return r->coll;
    return find_slug(st->coll, st->ncoll, r->slug);
}

static ctv_t walk(ctv_t root, const uint8_t *path, int depth)
{
    ctv_t n = root;
    for (int i = 0; i < depth; i++) {
        n = ct_at(ct_items(n), path[i]);
        if (n.t != CT_OBJ) return ct_undef();
    }
    return n;
}

ctv_t ct_node(const ct_store_t *st, const ct_ref_t *r)
{
    int c = coll_index(st, r);
    if (c < 0) return ct_undef();
    return walk(st->coll[c].root, r->path, r->depth);
}

/* ---- a request, read ---------------------------------------------------------------------------- */

static ctv_t request(ctv_t item)
{
    return ct_get(item, "request");
}

const char *ct_req_method(ctv_t item)
{
    ctv_t r = request(item);
    if (r.t == CT_STR) return "GET";        /* a request that is only its URL */
    const char *m = ct_s(ct_get(r, "method"));
    return m[0] ? m : "GET";
}

const char *ct_req_url(ct_arena_t *a, ctv_t item)
{
    ctv_t r = request(item);
    if (r.t == CT_STR) return r.s->p;
    ctv_t u = ct_get(r, "url");
    if (u.t == CT_STR) return u.s->p;
    if (u.t != CT_OBJ) return "";
    ctv_t raw = ct_get(u, "raw");
    if (raw.t == CT_STR) return raw.s->p;
    /* rebuilt from the parts */
    ct_sb_t b = { 0 };
    const char *proto = ct_s(ct_get(u, "protocol"));
    if (proto[0]) {
        ct_sb_adds(&b, proto);
        ct_sb_adds(&b, "://");
    }
    ctv_t host = ct_get(u, "host");
    if (host.t == CT_STR) ct_sb_adds(&b, host.s->p);
    for (uint32_t i = 0; i < ct_len(host); i++) {
        if (i) ct_sb_addc(&b, '.');
        ct_sb_adds(&b, ct_s(ct_at(host, i)));
    }
    const char *port = ct_s(ct_get(u, "port"));
    if (port[0]) {
        ct_sb_addc(&b, ':');
        ct_sb_adds(&b, port);
    }
    ctv_t path = ct_get(u, "path");
    if (path.t == CT_STR) {
        if (path.s->p[0] != '/') ct_sb_addc(&b, '/');
        ct_sb_adds(&b, path.s->p);
    }
    for (uint32_t i = 0; i < ct_len(path); i++) {
        ct_sb_addc(&b, '/');
        ctv_t seg = ct_at(path, i);
        ct_sb_adds(&b, seg.t == CT_STR ? seg.s->p : ct_s(ct_get(seg, "value")));
    }
    ctv_t q = ct_get(u, "query");
    int k = 0;
    for (uint32_t i = 0; i < ct_len(q); i++) {
        ctv_t e = ct_at(q, i);
        if (ct_truthy(ct_get(e, "disabled"))) continue;
        ct_sb_addc(&b, k++ ? '&' : '?');
        ct_sb_adds(&b, ct_s(ct_get(e, "key")));
        ctv_t v = ct_get(e, "value");
        if (v.t != CT_NULL) {
            ct_sb_addc(&b, '=');
            ct_sb_adds(&b, ct_s(v));
        }
    }
    char *out = ct_alloc(a, b.n + 1);
    if (out && b.p) memcpy(out, b.p, b.n + 1);
    else if (out) out[0] = '\0';
    ct_sb_free(&b);
    return out ? out : "";
}

ctv_t ct_req_headers(ctv_t item)
{
    ctv_t h = ct_get(request(item), "header");
    return h.t == CT_ARR ? h : ct_undef();
}

ctv_t ct_req_body(ctv_t item)
{
    return ct_get(request(item), "body");
}

ctv_t ct_req_auth(ctv_t item)
{
    return ct_get(request(item), "auth");
}

char *ct_req_script(ct_arena_t *a, ctv_t item, const char *listen)
{
    ctv_t ev = ct_get(item, "event");
    ct_sb_t b = { 0 };
    for (uint32_t i = 0; i < ct_len(ev); i++) {
        ctv_t e = ct_at(ev, i);
        if (strcmp(ct_s(ct_get(e, "listen")), listen)) continue;
        if (ct_truthy(ct_get(e, "disabled"))) continue;
        ctv_t ex = ct_get(ct_get(e, "script"), "exec");
        if (ex.t == CT_STR) ct_sb_adds(&b, ex.s->p);
        for (uint32_t k = 0; k < ct_len(ex); k++) {
            if (k) ct_sb_addc(&b, '\n');
            ct_sb_adds(&b, ct_s(ct_at(ex, k)));
        }
        break;
    }
    char *out = ct_alloc(a, b.n + 1);
    if (out && b.p) memcpy(out, b.p, b.n + 1);
    else if (out) out[0] = '\0';
    ct_sb_free(&b);
    return out;
}

/* ---- a request, written --------------------------------------------------------------------------- */

ctv_t ct_req_ensure(ct_arena_t *a, ctv_t item)
{
    ctv_t r = ct_get(item, "request");
    if (r.t == CT_STR) {
        /* a request that was only its URL becomes a whole one */
        ctv_t o = ct_obj_new(a, 4);
        ct_sets(a, o, "method", "GET");
        ct_setv(a, o, "header", ct_arr_new(a, 0));
        ct_setv(a, o, "url", r);
        ct_setv(a, item, "request", o);
        return o;
    }
    if (r.t != CT_OBJ) {
        r = ct_obj_new(a, 4);
        ct_sets(a, r, "method", "GET");
        ct_setv(a, r, "header", ct_arr_new(a, 0));
        ct_setv(a, item, "request", r);
    }
    if (ct_get(r, "header").t != CT_ARR) ct_setv(a, r, "header", ct_arr_new(a, 0));
    return r;
}

void ct_req_set_url(ct_arena_t *a, ctv_t item, const char *raw)
{
    ctv_t r = ct_req_ensure(a, item);
    ctv_t old = ct_get(r, "url");
    ctv_t u = ct_obj_new(a, 6);
    ct_sets(a, u, "raw", raw);
    const char *p = raw;
    const char *sch = strstr(p, "://");
    const char *q0 = p + strcspn(p, "?#");
    if (sch && sch < q0) {
        ct_setv(a, u, "protocol", ct_strv(a, p, (size_t)(sch - p)));
        p = sch + 3;
    }
    /* host[:port] then the path, cut at '/' */
    const char *hend = p + strcspn(p, "/?#");
    const char *colon = NULL;
    for (const char *c = p; c < hend; c++) if (*c == ':') colon = c;
    /* a {{variable}} as the host may hold the scheme and port itself:
     * Postman keeps it as one host label */
    const char *hstop = colon ? colon : hend;
    ctv_t host = ct_arr_new(a, 4);
    if (hstop > p) {
        bool is_var = p[0] == '{' && p[1] == '{';
        const char *s = p;
        for (const char *c = p; c <= hstop; c++) {
            if (c == hstop || (*c == '.' && !is_var)) {
                ct_arr_push(a, host.a, ct_strv(a, s, (size_t)(c - s)));
                s = c + 1;
            }
        }
    }
    ct_setv(a, u, "host", host);
    if (colon) ct_setv(a, u, "port", ct_strv(a, colon + 1, (size_t)(hend - colon - 1)));
    ctv_t path = ct_arr_new(a, 4);
    const char *pp = hend;
    const char *pend = pp + strcspn(pp, "?#");
    if (*pp == '/') {
        pp++;
        const char *s = pp;
        for (const char *c = pp; c <= pend; c++) {
            if (c == pend || *c == '/') {
                ct_arr_push(a, path.a, ct_strv(a, s, (size_t)(c - s)));
                s = c + 1;
            }
        }
    }
    ct_setv(a, u, "path", path);
    ctv_t query = ct_arr_new(a, 4);
    if (*pend == '?') {
        const char *s = pend + 1;
        const char *qend = s + strcspn(s, "#");
        while (s < qend) {
            const char *amp = memchr(s, '&', (size_t)(qend - s));
            const char *e = amp ? amp : qend;
            const char *eq = memchr(s, '=', (size_t)(e - s));
            ctv_t qe = ct_obj_new(a, 2);
            ct_setv(a, qe, "key", ct_strv(a, s, (size_t)((eq ? eq : e) - s)));
            ct_setv(a, qe, "value", eq ? ct_strv(a, eq + 1, (size_t)(e - eq - 1)) : ct_null());
            ct_arr_push(a, query.a, qe);
            s = e + 1;
        }
    }
    /* the disabled ones are not in the raw text: they stay */
    ctv_t oq = ct_get(old, "query");
    for (uint32_t i = 0; i < ct_len(oq); i++) {
        if (ct_truthy(ct_get(ct_at(oq, i), "disabled"))) ct_arr_push(a, query.a, ct_at(oq, i));
    }
    if (ct_len(query)) ct_setv(a, u, "query", query);
    ct_setv(a, r, "url", u);
}

void ct_req_set_script(ct_arena_t *a, ctv_t item, const char *listen, const char *src)
{
    ctv_t ev = ct_get(item, "event");
    bool empty = true;
    for (const char *p = src ? src : ""; *p; p++) {
        if (*p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') empty = false;
    }
    if (ev.t != CT_ARR) {
        if (empty) return;
        ev = ct_arr_new(a, 2);
        ct_setv(a, item, "event", ev);
    }
    for (uint32_t i = 0; i < ev.a->n; i++) {
        if (strcmp(ct_s(ct_get(ev.a->v[i], "listen")), listen)) continue;
        if (empty) {
            ct_arr_remove(ev.a, i);
            return;
        }
        ct_arr_remove(ev.a, i);
        break;
    }
    if (empty) return;
    ctv_t e = ct_obj_new(a, 2), s = ct_obj_new(a, 2), lines = ct_arr_new(a, 8);
    for (const char *p = src;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        ct_arr_push(a, lines.a, ct_strv(a, p, n));
        if (!nl) break;
        p = nl + 1;
    }
    ct_sets(a, e, "listen", listen);
    ct_sets(a, s, "type", "text/javascript");
    ct_setv(a, s, "exec", lines);
    ct_setv(a, e, "script", s);
    ct_arr_push(a, ev.a, e);
}

ctv_t ct_new_request(ct_arena_t *a, const char *name)
{
    ctv_t it = ct_obj_new(a, 4);
    ct_sets(a, it, "name", name);
    ctv_t r = ct_req_ensure(a, it);
    (void)r;
    ct_req_set_url(a, it, "");
    ct_setv(a, it, "response", ct_arr_new(a, 0));
    return it;
}

/* ---- changing a collection on the card ------------------------------------------------------------ */

static ct_doc_t *coll_fresh(ct_store_t *st, const ct_ref_t *r)
{
    int c = coll_index(st, r);
    if (c < 0) return NULL;
    ct_doc_refresh(&st->coll[c], "colecciones");
    if (st->coll[c].bad || st->coll[c].root.t != CT_OBJ) return NULL;
    return &st->coll[c];
}

bool ct_coll_put(ct_store_t *st, const ct_ref_t *r, ctv_t item, const char *old_name)
{
    ct_doc_t *d = coll_fresh(st, r);
    if (!d || r->depth < 1) return false;
    ctv_t parent = walk(d->root, r->path, r->depth - 1);
    ctv_t items = ct_items(parent);
    if (items.t != CT_ARR) return false;
    uint32_t idx = r->path[r->depth - 1];
    /* the place it had, if the same request is still there; else found by
     * name (the page moved things); else it goes at the end */
    if (idx >= items.a->n || strcmp(ct_item_name(items.a->v[idx]), old_name)) {
        uint32_t k;
        for (k = 0; k < items.a->n; k++) if (!strcmp(ct_item_name(items.a->v[k]), old_name) && !ct_is_folder(items.a->v[k])) break;
        idx = k;
    }
    ctv_t copy = ct_deep_copy(&d->ar, item);
    if (copy.t != CT_OBJ) return false;
    if (idx < items.a->n) items.a->v[idx] = copy;
    else if (!ct_arr_push(&d->ar, items.a, copy)) return false;
    return ct_doc_save(d, "colecciones");
}

bool ct_coll_add(ct_store_t *st, const ct_ref_t *parent, ctv_t item, ct_ref_t *out)
{
    ct_doc_t *d = coll_fresh(st, parent);
    if (!d || parent->depth >= CT_DEPTH_MAX) return false;
    ctv_t p = walk(d->root, parent->path, parent->depth);
    ctv_t items = ct_items(p);
    if (items.t != CT_ARR) {
        items = ct_arr_new(&d->ar, 4);
        ct_setv(&d->ar, p, "item", items);
    }
    ctv_t copy = ct_deep_copy(&d->ar, item);
    if (copy.t != CT_OBJ || !ct_arr_push(&d->ar, items.a, copy)) return false;
    uint32_t at = items.a->n - 1;
    if (out) {
        *out = *parent;
        out->path[out->depth++] = (uint8_t)at;
    }
    bool ok = ct_doc_save(d, "colecciones");
    if (out) out->coll = coll_index(st, out);
    return ok;
}

bool ct_coll_add_folder(ct_store_t *st, const ct_ref_t *parent, const char *name)
{
    ct_arena_t a;
    ct_arena_init(&a, 64 * 1024);
    ctv_t f = ct_obj_new(&a, 2);
    ct_sets(&a, f, "name", name);
    ct_setv(&a, f, "item", ct_arr_new(&a, 0));
    bool ok = ct_coll_add(st, parent, f, NULL);
    ct_arena_free(&a);
    return ok;
}

bool ct_coll_remove(ct_store_t *st, const ct_ref_t *r)
{
    ct_doc_t *d = coll_fresh(st, r);
    if (!d || r->depth < 1) return false;
    ctv_t items = ct_items(walk(d->root, r->path, r->depth - 1));
    if (items.t != CT_ARR || r->path[r->depth - 1] >= items.a->n) return false;
    ct_arr_remove(items.a, r->path[r->depth - 1]);
    return ct_doc_save(d, "colecciones");
}

/* ---- variables ------------------------------------------------------------------------------------ */

static bool var_on(ctv_t e)
{
    ctv_t en = ct_get(e, "enabled");
    if (en.t == CT_BOOL && !en.b) return false;
    return !ct_truthy(ct_get(e, "disabled"));
}

ctv_t ct_vars_obj(ct_arena_t *a, ctv_t values)
{
    ctv_t o = ct_obj_new(a, ct_len(values));
    for (uint32_t i = 0; i < ct_len(values); i++) {
        ctv_t e = ct_at(values, i);
        const char *k = ct_s(ct_get(e, "key"));
        if (!k[0] || !var_on(e)) continue;
        ctv_t v = ct_get(e, "value");
        if (v.t == CT_UNDEF || v.t == CT_NULL) v = ct_cstr(a, "");
        ct_obj_set(a, o.o, k, strlen(k), ct_deep_copy(a, v));
    }
    return o;
}

void ct_vars_set(ct_arena_t *a, ctv_t values, const char *k, ctv_t v)
{
    if (values.t != CT_ARR) return;
    for (uint32_t i = 0; i < values.a->n; i++) {
        ctv_t e = values.a->v[i];
        if (strcmp(ct_s(ct_get(e, "key")), k)) continue;
        ct_setv(a, e, "value", ct_deep_copy(a, v));
        if (ct_get(e, "enabled").t == CT_BOOL) ct_setv(a, e, "enabled", ct_bool(true));
        if (ct_get(e, "disabled").t == CT_BOOL) ct_setv(a, e, "disabled", ct_bool(false));
        return;
    }
    ctv_t e = ct_obj_new(a, 4);
    ct_sets(a, e, "key", k);
    ct_setv(a, e, "value", ct_deep_copy(a, v));
    ct_sets(a, e, "type", "default");
    ct_setv(a, e, "enabled", ct_bool(true));
    ct_arr_push(a, values.a, e);
}

void ct_vars_unset(ctv_t values, const char *k)
{
    for (uint32_t i = ct_len(values); i-- > 0;) {
        if (!strcmp(ct_s(ct_get(values.a->v[i], "key")), k)) ct_arr_remove(values.a, i);
    }
}

static ctv_t values_of(ct_doc_t *d, const char *field)
{
    if (d->root.t != CT_OBJ) return ct_undef();
    ctv_t v = ct_get(d->root, field);
    if (v.t != CT_ARR) {
        v = ct_arr_new(&d->ar, 4);
        ct_setv(&d->ar, d->root, field, v);
    }
    return v;
}

int ct_store_apply(ct_store_t *st, int coll, ctv_t changes)
{
    if (!ct_len(changes)) return 0;
    ct_doc_t *env = st->active >= 0 && st->active < st->nenv ? &st->env[st->active] : NULL;
    ct_doc_t *col = coll >= 0 && coll < st->ncoll ? &st->coll[coll] : NULL;
    if (env) ct_doc_refresh(env, "entornos");
    if (col) ct_doc_refresh(col, "colecciones");
    ct_doc_refresh(&st->glob, NULL);
    bool te = false, tc = false, tg = false;
    int dropped = 0;
    for (uint32_t i = 0; i < ct_len(changes); i++) {
        ctv_t c = ct_at(changes, i);
        const char *scope = ct_s(ct_at(c, 0)), *k = ct_s(ct_at(c, 1));
        ctv_t v = ct_at(c, 2);
        ct_doc_t *d = NULL;
        const char *field = "values";
        if (!strcmp(scope, "environment")) {
            d = env;
            te = te || env;
        } else if (!strcmp(scope, "globals")) {
            d = &st->glob;
            tg = true;
        } else if (!strcmp(scope, "collectionVariables")) {
            d = col;
            field = "variable";
            tc = tc || col;
        }
        if (!d || d->bad) {
            dropped++;
            continue;
        }
        ctv_t vals = values_of(d, field);
        if (v.t == CT_UNDEF) ct_vars_unset(vals, k);
        else ct_vars_set(&d->ar, vals, k, v);
    }
    if (te) ct_doc_save(env, "entornos");
    if (tc) ct_doc_save(col, "colecciones");
    if (tg) ct_doc_save(&st->glob, NULL);
    return dropped;
}

/* ---- the history ------------------------------------------------------------------------------------ */

void ct_hist_add(ct_store_t *st, ctv_t entry)
{
    ct_doc_t *h = &st->hist;
    ct_doc_refresh(h, NULL);
    if (h->bad || h->root.t != CT_ARR) {
        ct_arena_free(&h->ar);
        memset(h, 0, sizeof *h);
        ct_arena_init(&h->ar, DOC_ARENA);
        ct_copy(h->slug, sizeof h->slug, "historial");
        h->root = ct_arr_new(&h->ar, 8);
    }
    ctv_t e = ct_deep_copy(&h->ar, entry);
    ct_arr_insert(&h->ar, h->root.a, 0, e);
    while (h->root.a->n > CT_HIST_MAX) h->root.a->n--;
    ct_doc_save(h, NULL);
}

bool ct_hist_clear(ct_store_t *st)
{
    char path[300];
    ct_path(path, sizeof path, "historial.json");
    bool ok = ct_write_file(path, "[]\n", 3);
    ct_doc_refresh(&st->hist, NULL);
    return ok;
}
