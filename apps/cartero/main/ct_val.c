/*
 * P4OS - Cartero: arenas, growing texts and the values (ct.h).
 */
#include "ct.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(CT_HOST_TEST)
#include "esp_heap_caps.h"
#define ON_BOARD 1
#endif

void *ct_psram(size_t n)
{
#ifdef ON_BOARD
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
#else
    return malloc(n ? n : 1);
#endif
}

void *ct_psram_realloc(void *p, size_t n)
{
#ifdef ON_BOARD
    void *q = heap_caps_realloc(p, n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n ? n : 1);
#else
    return realloc(p, n ? n : 1);
#endif
}

void ct_copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t k = src ? strlen(src) : 0;
    if (k >= n) k = n - 1;
    if (k) memcpy(dst, src, k);
    dst[k] = '\0';
}

/* ---- arenas ------------------------------------------------------------------------------------ */

struct ct_block {
    ct_block_t *next;
    size_t      size, used;
    /* the memory follows, 8-aligned */
};

#define BLOCK_FIRST  (8 * 1024)
#define BLOCK_TOP    (256 * 1024)
#define HDR          ((sizeof(ct_block_t) + 7) & ~(size_t)7)

void ct_arena_init(ct_arena_t *a, size_t cap)
{
    memset(a, 0, sizeof *a);
    a->cap = cap;
    a->next = BLOCK_FIRST;
}

void *ct_alloc(ct_arena_t *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (!n) n = 8;
    if (a->cap && a->used + n > a->cap) {
        a->full = true;
        return NULL;
    }
    ct_block_t *b = a->head;
    if (!b || b->used + n > b->size) {
        size_t sz = a->next ? a->next : BLOCK_FIRST;
        if (sz < n) sz = n;
        b = ct_psram(HDR + sz);
        if (!b) {
            a->full = true;
            return NULL;
        }
        b->size = sz;
        b->used = 0;
        /* a big one-off allocation goes behind the current block, so the
         * room left in that block is not lost */
        if (a->head && sz == n && a->head->used + 64 < a->head->size) {
            b->next = a->head->next;
            a->head->next = b;
        } else {
            b->next = a->head;
            a->head = b;
            if (a->next < BLOCK_TOP) a->next *= 2;
        }
    }
    void *p = (char *)b + HDR + b->used;
    b->used += n;
    a->used += n;
    memset(p, 0, n);
    return p;
}

void ct_arena_free(ct_arena_t *a)
{
    for (ct_block_t *b = a->head; b;) {
        ct_block_t *n = b->next;
        free(b);
        b = n;
    }
    size_t cap = a->cap;
    ct_arena_init(a, cap);
}

/* ---- growing texts ------------------------------------------------------------------------------ */

static bool sb_room(ct_sb_t *b, size_t more)
{
    if (b->oom) return false;
    if (b->n + more + 1 <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + more + 1) cap *= 2;
    char *p = ct_psram_realloc(b->p, cap);
    if (!p) {
        b->oom = true;
        return false;
    }
    b->p = p;
    b->cap = cap;
    return true;
}

void ct_sb_add(ct_sb_t *b, const char *s, size_t n)
{
    if (!sb_room(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

void ct_sb_adds(ct_sb_t *b, const char *s)
{
    ct_sb_add(b, s, strlen(s));
}

void ct_sb_addc(ct_sb_t *b, char c)
{
    ct_sb_add(b, &c, 1);
}

void ct_sb_addf(ct_sb_t *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if ((size_t)k < sizeof tmp) {
        ct_sb_add(b, tmp, (size_t)k);
        return;
    }
    if (!sb_room(b, (size_t)k)) return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)k;
}

void ct_sb_free(ct_sb_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* ---- values ----------------------------------------------------------------------------------- */

ct_str_t *ct_str_new(ct_arena_t *a, const char *p, size_t n)
{
    ct_str_t *s = ct_alloc(a, sizeof(ct_str_t) + n + 1);
    if (!s) return NULL;
    s->len = (uint32_t)n;
    if (n) memcpy(s->p, p, n);
    s->p[n] = '\0';
    return s;
}

ctv_t ct_strv(ct_arena_t *a, const char *p, size_t n)
{
    ctv_t v = ct_undef();
    ct_str_t *s = ct_str_new(a, p, n);
    if (!s) return v;
    v.t = CT_STR;
    v.s = s;
    return v;
}

ctv_t ct_cstr(ct_arena_t *a, const char *p)
{
    return ct_strv(a, p ? p : "", p ? strlen(p) : 0);
}

ctv_t ct_arr_new(ct_arena_t *a, uint32_t cap)
{
    ctv_t v = ct_undef();
    ct_arr_t *arr = ct_alloc(a, sizeof *arr);
    if (!arr) return v;
    if (cap) {
        arr->v = ct_alloc(a, cap * sizeof(ctv_t));
        arr->cap = arr->v ? cap : 0;
    }
    v.t = CT_ARR;
    v.a = arr;
    return v;
}

ctv_t ct_obj_new(ct_arena_t *a, uint32_t cap)
{
    ctv_t v = ct_undef();
    ct_obj_t *o = ct_alloc(a, sizeof *o);
    if (!o) return v;
    if (cap) {
        o->p = ct_alloc(a, cap * sizeof(ct_prop_t));
        o->cap = o->p ? cap : 0;
    }
    v.t = CT_OBJ;
    v.o = o;
    return v;
}

ctv_t ct_fn_native(ct_arena_t *a, ct_native_t f, void *ud, const char *name)
{
    ctv_t v = ct_undef();
    ct_fn_t *fn = ct_alloc(a, sizeof *fn);
    if (!fn) return v;
    fn->native = f;
    fn->ud = ud;
    fn->name = name;
    v.t = CT_FN;
    v.f = fn;
    return v;
}

static bool arr_grow(ct_arena_t *a, ct_arr_t *arr, uint32_t need)
{
    if (need <= arr->cap) return true;
    uint32_t cap = arr->cap ? arr->cap * 2 : 4;
    while (cap < need) cap *= 2;
    ctv_t *v = ct_alloc(a, cap * sizeof(ctv_t));
    if (!v) return false;
    if (arr->n) memcpy(v, arr->v, arr->n * sizeof(ctv_t));
    arr->v = v;
    arr->cap = cap;
    return true;
}

bool ct_arr_push(ct_arena_t *a, ct_arr_t *arr, ctv_t v)
{
    if (!arr || !arr_grow(a, arr, arr->n + 1)) return false;
    arr->v[arr->n++] = v;
    return true;
}

bool ct_arr_insert(ct_arena_t *a, ct_arr_t *arr, uint32_t at, ctv_t v)
{
    if (!arr || !arr_grow(a, arr, arr->n + 1)) return false;
    if (at > arr->n) at = arr->n;
    memmove(arr->v + at + 1, arr->v + at, (arr->n - at) * sizeof(ctv_t));
    arr->v[at] = v;
    arr->n++;
    return true;
}

void ct_arr_remove(ct_arr_t *arr, uint32_t at)
{
    if (!arr || at >= arr->n) return;
    memmove(arr->v + at, arr->v + at + 1, (arr->n - at - 1) * sizeof(ctv_t));
    arr->n--;
}

ctv_t *ct_obj_find(ct_obj_t *o, const char *k, size_t kl)
{
    if (!o) return NULL;
    for (uint32_t i = 0; i < o->n; i++) {
        ct_str_t *s = o->p[i].k;
        if (s->len == kl && !memcmp(s->p, k, kl)) return &o->p[i].v;
    }
    return NULL;
}

bool ct_obj_set(ct_arena_t *a, ct_obj_t *o, const char *k, size_t kl, ctv_t v)
{
    if (!o) return false;
    ctv_t *cur = ct_obj_find(o, k, kl);
    if (cur) {
        *cur = v;
        return true;
    }
    if (o->n == o->cap) {
        uint32_t cap = o->cap ? o->cap * 2 : 4;
        ct_prop_t *p = ct_alloc(a, cap * sizeof(ct_prop_t));
        if (!p) return false;
        if (o->n) memcpy(p, o->p, o->n * sizeof(ct_prop_t));
        o->p = p;
        o->cap = cap;
    }
    ct_str_t *ks = ct_str_new(a, k, kl);
    if (!ks) return false;
    o->p[o->n].k = ks;
    o->p[o->n].v = v;
    o->n++;
    return true;
}

bool ct_obj_del(ct_obj_t *o, const char *k, size_t kl)
{
    if (!o) return false;
    for (uint32_t i = 0; i < o->n; i++) {
        ct_str_t *s = o->p[i].k;
        if (s->len == kl && !memcmp(s->p, k, kl)) {
            memmove(o->p + i, o->p + i + 1, (o->n - i - 1) * sizeof(ct_prop_t));
            o->n--;
            return true;
        }
    }
    return false;
}

ctv_t ct_get(ctv_t v, const char *k)
{
    if (v.t != CT_OBJ) return ct_undef();
    ctv_t *p = ct_obj_find(v.o, k, strlen(k));
    return p ? *p : ct_undef();
}

ctv_t ct_at(ctv_t v, uint32_t i)
{
    if (v.t != CT_ARR || i >= v.a->n) return ct_undef();
    return v.a->v[i];
}

uint32_t ct_len(ctv_t v)
{
    return v.t == CT_ARR ? v.a->n : 0;
}

const char *ct_s(ctv_t v)
{
    return v.t == CT_STR ? v.s->p : "";
}

bool ct_is(ctv_t v, ct_type_t t)
{
    return v.t == t;
}

void ct_sets(ct_arena_t *a, ctv_t obj, const char *k, const char *s)
{
    if (obj.t == CT_OBJ) ct_obj_set(a, obj.o, k, strlen(k), ct_cstr(a, s));
}

void ct_setv(ct_arena_t *a, ctv_t obj, const char *k, ctv_t v)
{
    if (obj.t == CT_OBJ) ct_obj_set(a, obj.o, k, strlen(k), v);
}

bool ct_truthy(ctv_t v)
{
    switch (v.t) {
    case CT_UNDEF: case CT_NULL: return false;
    case CT_BOOL: return v.b;
    case CT_NUM: return v.n != 0 && v.n == v.n;
    case CT_STR: return v.s->len > 0;
    default: return true;
    }
}

static ctv_t copy_depth(ct_arena_t *a, ctv_t v, int depth)
{
    if (depth > 200) return ct_null();
    switch (v.t) {
    case CT_STR: return ct_strv(a, v.s->p, v.s->len);
    case CT_ARR: {
        ctv_t r = ct_arr_new(a, v.a->n);
        if (r.t != CT_ARR) return ct_undef();
        for (uint32_t i = 0; i < v.a->n; i++) ct_arr_push(a, r.a, copy_depth(a, v.a->v[i], depth + 1));
        return r;
    }
    case CT_OBJ: {
        ctv_t r = ct_obj_new(a, v.o->n);
        if (r.t != CT_OBJ) return ct_undef();
        for (uint32_t i = 0; i < v.o->n; i++) {
            ctv_t c = copy_depth(a, v.o->p[i].v, depth + 1);
            if (c.t == CT_UNDEF) continue;
            ct_obj_set(a, r.o, v.o->p[i].k->p, v.o->p[i].k->len, c);
        }
        return r;
    }
    case CT_FN: return ct_undef();
    default: return v;
    }
}

ctv_t ct_deep_copy(ct_arena_t *a, ctv_t v)
{
    return copy_depth(a, v, 0);
}

bool ct_deep_equal(ctv_t a, ctv_t b)
{
    if (a.t != b.t) return false;
    switch (a.t) {
    case CT_UNDEF: case CT_NULL: return true;
    case CT_BOOL: return a.b == b.b;
    case CT_NUM: return a.n == b.n || (a.n != a.n && b.n != b.n);
    case CT_STR: return a.s->len == b.s->len && !memcmp(a.s->p, b.s->p, a.s->len);
    case CT_ARR:
        if (a.a->n != b.a->n) return false;
        for (uint32_t i = 0; i < a.a->n; i++) if (!ct_deep_equal(a.a->v[i], b.a->v[i])) return false;
        return true;
    case CT_OBJ:
        if (a.o->n != b.o->n) return false;
        for (uint32_t i = 0; i < a.o->n; i++) {
            ctv_t *o = ct_obj_find(b.o, a.o->p[i].k->p, a.o->p[i].k->len);
            if (!o || !ct_deep_equal(a.o->p[i].v, *o)) return false;
        }
        return true;
    case CT_FN: return a.f == b.f;
    }
    return false;
}

/* ---- conversions ------------------------------------------------------------------------------- */

/* JavaScript prints the shortest text that reads back as the same number,
 * with no exponent from 1e-7 to 1e21: try more digits until it does. */
void ct_num_fmt(double d, char out[40])
{
    if (d != d) {
        strcpy(out, "NaN");
        return;
    }
    if (d > 1.7976931348623157e308) {
        strcpy(out, "Infinity");
        return;
    }
    if (d < -1.7976931348623157e308) {
        strcpy(out, "-Infinity");
        return;
    }
    if (d == 0) {
        strcpy(out, "0");
        return;
    }
    double ad = d < 0 ? -d : d;
    if (ad < 1e15 && d == (double)(int64_t)d) {
        snprintf(out, 40, "%lld", (long long)(int64_t)d);
        return;
    }
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(out, 40, "%.*g", prec, d);
        if (strtod(out, NULL) == d) break;
    }
    /* "1e-07" -> "1e-7", "1e+21" stays */
    char *e = strchr(out, 'e');
    if (e) {
        char *p = e + 1;
        if (*p == '+' || *p == '-') p++;
        char *q = p;
        while (*q == '0' && q[1]) q++;
        if (q != p) memmove(p, q, strlen(q) + 1);
        if (ad >= 1e-7 && ad < 1e21) {
            /* %g chose an exponent JavaScript would not: print it plainly,
             * with as few decimals as read back the same */
            if (ad >= 1) {
                snprintf(out, 40, "%.0f", d);
                if (strtod(out, NULL) == d) return;
            }
            for (int prec = 1; prec <= 20; prec++) {
                char t[40];
                snprintf(t, sizeof t, "%.*f", prec, d);
                if (strtod(t, NULL) == d) {
                    strcpy(out, t);
                    break;
                }
            }
        }
    }
}

static void to_str_sb(ct_sb_t *b, ctv_t v, int depth)
{
    char n[40];
    switch (v.t) {
    case CT_UNDEF: ct_sb_adds(b, "undefined"); break;
    case CT_NULL: ct_sb_adds(b, "null"); break;
    case CT_BOOL: ct_sb_adds(b, v.b ? "true" : "false"); break;
    case CT_NUM: ct_num_fmt(v.n, n); ct_sb_adds(b, n); break;
    case CT_STR: ct_sb_add(b, v.s->p, v.s->len); break;
    case CT_ARR:
        if (depth > 20) break;
        for (uint32_t i = 0; i < v.a->n; i++) {
            if (i) ct_sb_addc(b, ',');
            if (v.a->v[i].t != CT_UNDEF && v.a->v[i].t != CT_NULL) to_str_sb(b, v.a->v[i], depth + 1);
        }
        break;
    case CT_OBJ:
        if (v.o->kind == CT_K_ERROR) {
            ctv_t *nm = ct_obj_find(v.o, "name", 4), *msg = ct_obj_find(v.o, "message", 7);
            ct_sb_adds(b, nm && nm->t == CT_STR ? nm->s->p : "Error");
            if (msg && msg->t == CT_STR && msg->s->len) {
                ct_sb_adds(b, ": ");
                ct_sb_add(b, msg->s->p, msg->s->len);
            }
        } else if (v.o->kind == CT_K_DATE) {
            ctv_t *iso = ct_obj_find(v.o, "__iso", 5);
            ct_sb_adds(b, iso && iso->t == CT_STR ? iso->s->p : "Invalid Date");
        } else {
            /* a system object that stands for a text (pm.request.url) */
            ctv_t *s = v.o->kind == CT_K_SYS ? ct_obj_find(v.o, "__str", 5) : NULL;
            if (s && s->t == CT_STR) ct_sb_add(b, s->s->p, s->s->len);
            else ct_sb_adds(b, "[object Object]");
        }
        break;
    case CT_FN: ct_sb_adds(b, "function () { [code] }"); break;
    }
}

ct_str_t *ct_to_str(ct_arena_t *a, ctv_t v)
{
    if (v.t == CT_STR) return v.s;
    ct_sb_t b = { 0 };
    to_str_sb(&b, v, 0);
    ct_str_t *s = ct_str_new(a, b.p ? b.p : "", b.n);
    ct_sb_free(&b);
    return s;
}

double ct_to_num(ctv_t v)
{
    switch (v.t) {
    case CT_UNDEF: return 0.0 / 0.0;
    case CT_NULL: return 0;
    case CT_BOOL: return v.b ? 1 : 0;
    case CT_NUM: return v.n;
    case CT_STR: {
        const char *p = v.s->p;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) return 0;
        char *end;
        double d;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) d = (double)strtoul(p + 2, &end, 16);
        else if (!strncmp(p, "Infinity", 8)) { d = 1.0 / 0.0; end = (char *)p + 8; }
        else if (!strncmp(p, "-Infinity", 9)) { d = -1.0 / 0.0; end = (char *)p + 9; }
        else d = strtod(p, &end);
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
        return *end ? 0.0 / 0.0 : d;
    }
    case CT_ARR:
        if (v.a->n == 0) return 0;
        if (v.a->n == 1) return ct_to_num(v.a->v[0]);
        return 0.0 / 0.0;
    case CT_OBJ:
        if (v.o->kind == CT_K_DATE) {
            ctv_t *t = ct_obj_find(v.o, "__t", 3);
            return t ? t->n : 0.0 / 0.0;
        }
        return 0.0 / 0.0;
    default: return 0.0 / 0.0;
    }
}

const char *ct_typeof(ctv_t v)
{
    switch (v.t) {
    case CT_UNDEF: return "undefined";
    case CT_BOOL: return "boolean";
    case CT_NUM: return "number";
    case CT_STR: return "string";
    case CT_FN: return "function";
    default: return "object";
    }
}
