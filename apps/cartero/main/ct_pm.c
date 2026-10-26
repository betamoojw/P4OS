/*
 * P4OS - Cartero: what a script finds around it. Postman's pm (tests,
 * chai's expect, the response, the variables, the request), console, the
 * old globals collections from before pm still carry (tests["..."],
 * responseBody, postman.setEnvironmentVariable), and the {{variables}} of a
 * request.
 *
 * The messages of a failed assertion are chai's, in English, as Postman
 * shows them: "expected 404 to equal 200". They are the developer's own
 * language and the same text the portal's page (the browser's chai-like
 * expect) gives.
 */
#include "ct_js_int.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { SC_LOCAL, SC_ENV, SC_COLL, SC_GLOB };

static const char *const SCOPE_NAME[] = { "local", "environment", "collectionVariables", "globals" };

static ctv_t *scope_obj(ct_script_t *ctx, int sc)
{
    switch (sc) {
    case SC_ENV: return &ctx->env;
    case SC_COLL: return &ctx->coll;
    case SC_GLOB: return &ctx->glob;
    default: return &ctx->local;
    }
}

static ctv_t strv(ct_str_t *s)
{
    ctv_t v = ct_undef();
    v.t = CT_STR;
    v.s = s;
    return v;
}

static bool ieq(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b);
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return true;
}

/* ---- variables ---------------------------------------------------------------------------------- */

ctv_t ct_var_get(const ct_script_t *ctx, const char *k)
{
    const ctv_t *order[] = { &ctx->local, &ctx->env, &ctx->coll, &ctx->glob };
    for (int i = 0; i < 4; i++) {
        if (order[i]->t != CT_OBJ) continue;
        ctv_t *v = ct_obj_find(order[i]->o, k, strlen(k));
        if (v) return *v;
    }
    return ct_undef();
}

static uint32_t g_seed;

static uint32_t rnd(void)
{
    if (!g_seed) g_seed = (uint32_t)aos_hal_uptime_us() | 1;
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static bool dynamic(const char *name, size_t n, char *out, size_t on)
{
    static const char HEX[] = "0123456789abcdef";
    if ((n == 5 && !memcmp(name, "$guid", 5)) || (n == 11 && !memcmp(name, "$randomUUID", 11))) {
        char u[37];
        for (int i = 0; i < 36; i++) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                u[i] = '-';
                continue;
            }
            uint32_t r = rnd() & 15;
            if (i == 14) r = 4;
            if (i == 19) r = (r & 3) | 8;
            u[i] = HEX[r];
        }
        u[36] = '\0';
        ct_copy(out, on, u);
        return true;
    }
    if (n == 10 && !memcmp(name, "$timestamp", 10)) {
        snprintf(out, on, "%lld", (long long)(ct_now_ms() / 1000));
        return true;
    }
    if (n == 13 && !memcmp(name, "$isoTimestamp", 13)) {
        double ms = ct_now_ms();
        time_t t = (time_t)(int64_t)(ms / 1000);
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(out, on, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, (int)((int64_t)ms % 1000));
        return true;
    }
    if (n == 10 && !memcmp(name, "$randomInt", 10)) {
        snprintf(out, on, "%u", (unsigned)(rnd() % 1001));
        return true;
    }
    if (n == 14 && !memcmp(name, "$randomBoolean", 14)) {
        ct_copy(out, on, (rnd() & 1) ? "true" : "false");
        return true;
    }
    if (n == 19 && !memcmp(name, "$randomAlphaNumeric", 19)) {
        static const char AN[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        if (on >= 2) {
            out[0] = AN[rnd() % 36];
            out[1] = '\0';
        }
        return true;
    }
    return false;
}

char *ct_resolve(ct_arena_t *a, const ct_script_t *ctx, const char *in)
{
    const char *src = in ? in : "";
    char *cur = NULL;           /* the text of the last round, malloc'd */
    /* a value may itself hold {{names}}: three rounds, as Postman does */
    for (int round = 0; round < 3 && strstr(src, "{{"); round++) {
        ct_sb_t b = { 0 };
        bool again = false;
        const char *p = src;
        while (*p) {
            const char *o = strstr(p, "{{");
            const char *c = o ? strstr(o + 2, "}}") : NULL;
            if (!o || !c) {
                ct_sb_adds(&b, p);
                break;
            }
            ct_sb_add(&b, p, (size_t)(o - p));
            char name[128];
            size_t nl = (size_t)(c - o - 2);
            if (nl >= sizeof name || memchr(o + 2, '{', nl)) {
                ct_sb_add(&b, o, 2);
                p = o + 2;
                continue;
            }
            memcpy(name, o + 2, nl);
            name[nl] = '\0';
            ctv_t v = ct_var_get(ctx, name);
            char dyn[64];
            if (v.t != CT_UNDEF) {
                ct_str_t *s = ct_to_str(a, v);
                if (s) {
                    ct_sb_add(&b, s->p, s->len);
                    if (strstr(s->p, "{{")) again = true;
                }
            } else if (name[0] == '$' && dynamic(name, nl, dyn, sizeof dyn)) {
                ct_sb_adds(&b, dyn);
            } else {
                ct_sb_add(&b, o, (size_t)(c + 2 - o));
            }
            p = c + 2;
        }
        free(cur);
        cur = b.p;
        src = cur ? cur : "";
        if (!again) break;
    }
    size_t n = strlen(src);
    char *out = ct_alloc(a, n + 1);
    if (out) memcpy(out, src, n + 1);
    free(cur);
    return out;
}

static void scope_set(ct_vm_t *vm, int sc, ct_str_t *k, ctv_t v)
{
    ct_script_t *ctx = vm->ctx;
    ctv_t *o = scope_obj(ctx, sc);
    if (o->t != CT_OBJ) *o = vm_obj(vm);
    if (v.t == CT_UNDEF) ct_obj_del(o->o, k->p, k->len);
    else if (!ct_obj_set(vm->ar, o->o, k->p, k->len, v)) ct_throw_err(vm, "RangeError", "out of memory");
    if (sc == SC_LOCAL) return;
    if (ctx->changes.t != CT_ARR) ctx->changes = vm_arr(vm);
    ctv_t e = vm_arr(vm);
    vm_push(vm, e, vm_cstr(vm, SCOPE_NAME[sc]));
    vm_push(vm, e, strv(k));
    /* what is kept is data: an object goes in as a copy */
    vm_push(vm, e, v.t == CT_UNDEF ? ct_undef() : ct_deep_copy(vm->ar, v));
    vm_push(vm, ctx->changes, e);
}

enum { V_GET, V_SET, V_UNSET, V_HAS, V_TOOBJECT, V_REPLACEIN, V_CLEAR };

static ctv_t var_api(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    int sc = (int)((intptr_t)ud >> 8), op = (int)((intptr_t)ud & 0xFF);
    ct_script_t *ctx = vm->ctx;
    ctv_t *o = scope_obj(ctx, sc);
    switch (op) {
    case V_GET: {
        ct_str_t *k = vm_to_str(vm, arg(argc, argv, 0));
        if (sc == SC_LOCAL) return ct_var_get(ctx, k->p);   /* pm.variables: every scope */
        ctv_t *v = o->t == CT_OBJ ? ct_obj_find(o->o, k->p, k->len) : NULL;
        return v ? *v : ct_undef();
    }
    case V_HAS: {
        ct_str_t *k = vm_to_str(vm, arg(argc, argv, 0));
        if (sc == SC_LOCAL) return ct_bool(ct_var_get(ctx, k->p).t != CT_UNDEF);
        return ct_bool(o->t == CT_OBJ && ct_obj_find(o->o, k->p, k->len));
    }
    case V_SET: {
        ct_str_t *k = vm_to_str(vm, arg(argc, argv, 0));
        ctv_t v = arg(argc, argv, 1);
        if (v.t == CT_UNDEF) v = vm_cstr(vm, "");
        scope_set(vm, sc, k, v);
        return ct_undef();
    }
    case V_UNSET:
        scope_set(vm, sc, vm_to_str(vm, arg(argc, argv, 0)), ct_undef());
        return ct_undef();
    case V_CLEAR:
        if (o->t == CT_OBJ) {
            while (o->o->n) scope_set(vm, sc, o->o->p[0].k, ct_undef());
        }
        return ct_undef();
    case V_TOOBJECT: {
        if (sc != SC_LOCAL) return o->t == CT_OBJ ? *o : vm_obj(vm);
        ctv_t r = vm_obj(vm);
        const ctv_t *order[] = { &ctx->glob, &ctx->coll, &ctx->env, &ctx->local };
        for (int i = 0; i < 4; i++) {
            if (order[i]->t != CT_OBJ) continue;
            for (uint32_t k = 0; k < order[i]->o->n; k++) vm_set(vm, r, order[i]->o->p[k].k->p, order[i]->o->p[k].v);
        }
        return r;
    }
    case V_REPLACEIN: {
        ct_str_t *s = vm_to_str(vm, arg(argc, argv, 0));
        char *r = ct_resolve(vm->ar, ctx, s->p);
        if (!r) ct_throw_err(vm, "RangeError", "out of memory");
        return vm_cstr(vm, r);
    }
    }
    return ct_undef();
}

static ctv_t var_obj(ct_vm_t *vm, int sc)
{
    ctv_t o = vm_obj(vm);
    o.o->kind = CT_K_SYS;
    static const struct { const char *n; int op; } M[] = {
        { "get", V_GET }, { "set", V_SET }, { "unset", V_UNSET }, { "has", V_HAS }, { "toObject", V_TOOBJECT },
        { "replaceIn", V_REPLACEIN }, { "clear", V_CLEAR },
    };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
        vm_method(vm, o, M[i].n, var_api, (void *)(intptr_t)((sc << 8) | M[i].op));
    }
    vm_set(vm, o, "name", vm_cstr(vm, SCOPE_NAME[sc]));
    return o;
}

/* ---- console ------------------------------------------------------------------------------------ */

static ctv_t console_api(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    ct_sb_t *c = vm->ctx->console;
    if (!c) return ct_undef();
    static const char LEVEL[] = "liwe";
    ct_sb_addc(c, LEVEL[(intptr_t)ud & 3]);
    ct_sb_addc(c, ' ');
    for (int i = 0; i < argc; i++) {
        if (i) ct_sb_addc(c, ' ');
        if (argv[i].t == CT_STR) ct_sb_add(c, argv[i].s->p, argv[i].s->len > 2000 ? 2000 : argv[i].s->len);
        else ct_inspect(c, argv[i], 0);
    }
    ct_sb_addc(c, '\n');
    if (c->n > 64 * 1024) {
        /* a script that logs in a loop: keep the last part */
        size_t cut = c->n - 48 * 1024;
        while (cut < c->n && c->p[cut] != '\n') cut++;
        memmove(c->p, c->p + cut + 1, c->n - cut - 1);
        c->n -= cut + 1;
        c->p[c->n] = '\0';
    }
    return ct_undef();
}

/* ---- the response ------------------------------------------------------------------------------- */

static ctv_t resp_json(ct_vm_t *vm)
{
    ct_script_t *ctx = vm->ctx;
    if (!ctx->resp) ct_throw_err(vm, "Error", "there is no response yet (a pre-request script)");
    if (ctx->resp_json_done) return ctx->resp_json;
    ctv_t v;
    char err[160];
    if (!ct_json_parse(vm->ar, (const char *)(ctx->resp->body ? ctx->resp->body : (const uint8_t *)""),
                       (size_t)ctx->resp->len, &v, err, sizeof err)) {
        ct_throw_err(vm, "JSONError", "%s", err);
    }
    ctx->resp_json = v;
    ctx->resp_json_done = true;
    return v;
}

static ctv_t hdr_value(ct_vm_t *vm, const char *name)
{
    const char *v = ct_resp_header(vm->ctx->resp, name);
    return v ? vm_cstr(vm, v) : ct_undef();
}

enum { R_JSON, R_TEXT, R_HGET, R_HHAS, R_HOBJ, R_HALL, R_HEACH, R_CGET, R_CHAS, R_COBJ, R_HCOUNT, R_REASON };

/* "name=value; Path=/" of each Set-Cookie */
static bool cookie(ct_vm_t *vm, const char *name, char *out, size_t on, int idx, char *nm, size_t nn)
{
    const ct_resp_t *r = vm->ctx->resp;
    int k = 0;
    for (int i = 0; r && i < r->nhdr; i++) {
        if (!ieq(r->hdr[i].k, strlen(r->hdr[i].k), "set-cookie")) continue;
        const char *v = r->hdr[i].v;
        const char *eq = strchr(v, '=');
        if (!eq) continue;
        size_t kl = (size_t)(eq - v);
        const char *end = strchr(eq, ';');
        size_t vl = end ? (size_t)(end - eq - 1) : strlen(eq + 1);
        bool match = name ? (strlen(name) == kl && !memcmp(name, v, kl)) : (k == idx);
        k++;
        if (!match) continue;
        if (nm) {
            if (kl >= nn) kl = nn - 1;
            memcpy(nm, v, kl);
            nm[kl] = '\0';
        }
        if (vl >= on) vl = on - 1;
        memcpy(out, eq + 1, vl);
        out[vl] = '\0';
        return true;
    }
    return false;
}

static ctv_t resp_api(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    const ct_resp_t *r = vm->ctx->resp;
    int op = (int)(intptr_t)ud;
    if (!r) ct_throw_err(vm, "Error", "there is no response yet (a pre-request script)");
    switch (op) {
    case R_JSON: return resp_json(vm);
    case R_TEXT: return vm_str(vm, r->body ? (const char *)r->body : "", (size_t)r->len);
    case R_REASON: return vm_cstr(vm, r->reason);
    case R_HGET: return hdr_value(vm, vm_to_str(vm, arg(argc, argv, 0))->p);
    case R_HHAS: {
        ct_str_t *k = vm_to_str(vm, arg(argc, argv, 0));
        const char *v = ct_resp_header(r, k->p);
        if (!v) return ct_bool(false);
        if (argc > 1) return ct_bool(!strcmp(v, vm_to_str(vm, argv[1])->p));
        return ct_bool(true);
    }
    case R_HCOUNT: return ct_num(r->nhdr);
    case R_HOBJ: {
        ctv_t o = vm_obj(vm);
        for (int i = 0; i < r->nhdr; i++) vm_set(vm, o, r->hdr[i].k, vm_cstr(vm, r->hdr[i].v));
        return o;
    }
    case R_HALL: case R_HEACH: {
        ctv_t a = vm_arr(vm);
        for (int i = 0; i < r->nhdr; i++) {
            ctv_t h = vm_obj(vm);
            vm_set(vm, h, "key", vm_cstr(vm, r->hdr[i].k));
            vm_set(vm, h, "value", vm_cstr(vm, r->hdr[i].v));
            if (op == R_HEACH) {
                ctv_t fn = arg(argc, argv, 0);
                ct_call(vm, fn, ct_undef(), 1, &h);
            } else {
                vm_push(vm, a, h);
            }
        }
        return op == R_HALL ? a : ct_undef();
    }
    case R_CGET: case R_CHAS: {
        char v[512];
        ct_str_t *k = vm_to_str(vm, arg(argc, argv, 0));
        bool f = cookie(vm, k->p, v, sizeof v, 0, NULL, 0);
        if (op == R_CHAS) return ct_bool(f);
        return f ? vm_cstr(vm, v) : ct_undef();
    }
    case R_COBJ: {
        ctv_t o = vm_obj(vm);
        char v[512], nm[128];
        for (int i = 0; cookie(vm, NULL, v, sizeof v, i, nm, sizeof nm); i++) vm_set(vm, o, nm, vm_cstr(vm, v));
        return o;
    }
    }
    return ct_undef();
}

/* ---- chai's expect -------------------------------------------------------------------------------- */

typedef struct {
    ctv_t actual;
    ctv_t msg;
    bool  neg, deep, resp;
} expect_t;

static ctv_t expect_get(ct_vm_t *vm, ct_obj_t *o, const char *k, size_t kl, bool *found);

static ctv_t make_expect(ct_vm_t *vm, ctv_t actual, ctv_t msg, bool resp)
{
    ctv_t o = vm_obj(vm);
    o.o->kind = CT_K_EXPECT;
    expect_t *e = ct_alloc(vm->ar, sizeof *e);
    if (!e) ct_throw_err(vm, "RangeError", "out of memory");
    e->actual = actual;
    e->msg = msg;
    e->resp = resp;
    o.o->ud = e;
    o.o->get = expect_get;
    return o;
}

static void show(ct_sb_t *b, ctv_t v)
{
    ct_inspect(b, v, 0);
}

/* the assertion's outcome: passes when ok, or when !ok under .not */
static void verdict(ct_vm_t *vm, expect_t *e, bool ok, const char *what, ctv_t *expected, const char *tail)
{
    if (ok != e->neg) return;
    ct_sb_t b = { 0 };
    if (e->msg.t == CT_STR && e->msg.s->len) {
        ct_sb_add(&b, e->msg.s->p, e->msg.s->len);
        ct_sb_adds(&b, ": ");
    }
    ct_sb_adds(&b, "expected ");
    if (e->resp) ct_sb_adds(&b, "response");
    else show(&b, e->actual);
    ct_sb_adds(&b, e->neg ? " to not " : " to ");
    ct_sb_adds(&b, what);
    if (expected) {
        ct_sb_addc(&b, ' ');
        show(&b, *expected);
    }
    if (tail) ct_sb_adds(&b, tail);
    ctv_t err = ct_make_error(vm, "AssertionError", b.p ? b.p : "assertion failed");
    ct_sb_free(&b);
    ct_throw(vm, err);
}

static bool includes(ctv_t hay, ctv_t needle, bool deep)
{
    if (hay.t == CT_STR) {
        if (needle.t != CT_STR) return false;
        return needle.s->len == 0 || (needle.s->len <= hay.s->len && strstr(hay.s->p, needle.s->p));
    }
    if (hay.t == CT_ARR) {
        for (uint32_t i = 0; i < hay.a->n; i++) {
            if (deep ? ct_deep_equal(hay.a->v[i], needle) : ct_strict_eq(hay.a->v[i], needle)) return true;
            /* chai's include of an object in an array is by identity, but
             * Postman users mean the value: try deep as well */
            if (!deep && (needle.t == CT_OBJ || needle.t == CT_ARR) && ct_deep_equal(hay.a->v[i], needle)) return true;
        }
        return false;
    }
    if (hay.t == CT_OBJ && needle.t == CT_OBJ) {
        /* a subset of the properties */
        for (uint32_t i = 0; i < needle.o->n; i++) {
            ctv_t *v = ct_obj_find(hay.o, needle.o->p[i].k->p, needle.o->p[i].k->len);
            if (!v) return false;
            if (!(deep ? ct_deep_equal(*v, needle.o->p[i].v) : (ct_strict_eq(*v, needle.o->p[i].v) ||
                                                                ct_deep_equal(*v, needle.o->p[i].v)))) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static const char *type_name(ctv_t v)
{
    switch (v.t) {
    case CT_ARR: return "array";
    case CT_NULL: return "null";
    case CT_OBJ:
        if (v.o->kind == CT_K_ERROR) return "error";
        if (v.o->kind == CT_K_DATE) return "date";
        return "object";
    default: return ct_typeof(v);
    }
}

static int size_of(ct_vm_t *vm, ctv_t v)
{
    if (v.t == CT_STR) return (int)v.s->len;
    if (v.t == CT_ARR) return (int)v.a->n;
    if (v.t == CT_OBJ) {
        ctv_t *l = ct_obj_find(v.o, "length", 6);
        if (l) return (int)ct_to_num(*l);
    }
    ct_throw_err(vm, "AssertionError", "expected a string or an array to have a length");
}

/* a JS-like path into a value: data[0].id, a.b */
static bool path_get(ct_vm_t *vm, ctv_t v, const char *path, ctv_t *out)
{
    const char *p = path;
    while (*p) {
        char key[128];
        size_t k = 0;
        if (*p == '.') p++;
        if (*p == '[') {
            p++;
            while (*p && *p != ']' && k < sizeof key - 1) {
                if (*p != '"' && *p != '\'') key[k++] = *p;
                p++;
            }
            if (*p == ']') p++;
        } else {
            while (*p && *p != '.' && *p != '[' && k < sizeof key - 1) key[k++] = *p++;
        }
        key[k] = '\0';
        if (v.t == CT_ARR) {
            char *e;
            long i = strtol(key, &e, 10);
            if (*e || i < 0 || (uint32_t)i >= v.a->n) return false;
            v = v.a->v[i];
        } else if (v.t == CT_OBJ) {
            ctv_t *x = ct_obj_find(v.o, key, k);
            if (!x) return false;
            v = *x;
        } else {
            return false;
        }
    }
    (void)vm;
    *out = v;
    return true;
}

enum {
    E_EQUAL = 1, E_EQL, E_ABOVE, E_BELOW, E_LEAST, E_MOST, E_WITHIN, E_CLOSETO, E_INCLUDE, E_PROPERTY, E_LENGTHOF,
    E_A, E_ONEOF, E_MATCH, E_STRING, E_KEYS, E_MEMBERS, E_SATISFY, E_INSTANCEOF, E_STATUS, E_HEADER, E_JSONBODY,
    E_BODY, E_JSONSCHEMA, E_RESPTIME, E_EXIST_FN,
};

static ctv_t expect_call(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    if (self.t != CT_OBJ || self.o->kind != CT_K_EXPECT) ct_throw_err(vm, "TypeError", "not an assertion");
    expect_t *e = self.o->ud;
    ctv_t a = e->actual, x = arg(argc, argv, 0);
    int m = (int)(intptr_t)ud;
    const ct_resp_t *r = vm->ctx->resp;
    switch (m) {
    case E_EQUAL:
        verdict(vm, e, e->deep ? ct_deep_equal(a, x) : ct_strict_eq(a, x), e->deep ? "deeply equal" : "equal", &x, NULL);
        break;
    case E_EQL:
        verdict(vm, e, ct_deep_equal(a, x), "deeply equal", &x, NULL);
        break;
    case E_ABOVE: case E_BELOW: case E_LEAST: case E_MOST: {
        if (a.t != CT_NUM && !(a.t == CT_OBJ && a.o->kind == CT_K_DATE)) {
            ct_sb_t b = { 0 };
            show(&b, a);
            ctv_t err = ct_make_error(vm, "AssertionError", "");
            ct_sb_adds(&b, " is not a number or a date");
            vm_set(vm, err, "message", vm_cstr(vm, b.p ? b.p : ""));
            ct_sb_free(&b);
            ct_throw(vm, err);
        }
        double av = ct_to_num(a), xv = ct_to_num(x);
        static const char *const W[] = { "be above", "be below", "be at least", "be at most" };
        bool ok = m == E_ABOVE ? av > xv : m == E_BELOW ? av < xv : m == E_LEAST ? av >= xv : av <= xv;
        verdict(vm, e, ok, W[m - E_ABOVE], &x, NULL);
        break;
    }
    case E_WITHIN: {
        double av = ct_to_num(a), lo = ct_to_num(x), hi = ct_to_num(arg(argc, argv, 1));
        char t[96], l1[40], l2[40];
        ct_num_fmt(lo, l1);
        ct_num_fmt(hi, l2);
        snprintf(t, sizeof t, "be within %s..%s", l1, l2);
        verdict(vm, e, av >= lo && av <= hi, t, NULL, NULL);
        break;
    }
    case E_CLOSETO: {
        double av = ct_to_num(a), xv = ct_to_num(x), d = ct_to_num(arg(argc, argv, 1));
        char t[96], d1[40];
        ct_num_fmt(d, d1);
        snprintf(t, sizeof t, "be close to");
        char tail[64];
        snprintf(tail, sizeof tail, " +/- %s", d1);
        double diff = av - xv;
        verdict(vm, e, (diff < 0 ? -diff : diff) <= d, t, &x, tail);
        break;
    }
    case E_INCLUDE:
        verdict(vm, e, includes(a, x, e->deep), e->deep ? "deep include" : "include", &x, NULL);
        break;
    case E_PROPERTY: {
        ct_str_t *k = vm_to_str(vm, x);
        ctv_t *v = NULL;
        ctv_t got;
        if (a.t == CT_OBJ) v = ct_obj_find(a.o, k->p, k->len);
        if (!v && (a.t == CT_ARR || a.t == CT_STR) && !strcmp(k->p, "length")) {
            got = ct_num(a.t == CT_ARR ? a.a->n : a.s->len);
            v = &got;
        }
        if (!v && a.t == CT_ARR) {
            char *end;
            long i = strtol(k->p, &end, 10);
            if (!*end && i >= 0 && (uint32_t)i < a.a->n) {
                got = a.a->v[i];
                v = &got;
            }
        }
        if (!v && strchr(k->p, '.') && path_get(vm, a, k->p, &got)) v = &got;
        if (argc < 2) {
            char t[200];
            snprintf(t, sizeof t, "have property '%.150s'", k->p);
            verdict(vm, e, v != NULL, t, NULL, NULL);
        } else {
            char t[200];
            snprintf(t, sizeof t, "have property '%.150s' of", k->p);
            ctv_t want = argv[1];
            bool ok = v && (e->deep ? ct_deep_equal(*v, want) : ct_strict_eq(*v, want));
            if (!ok && v && !e->neg) {
                ct_sb_t b = { 0 };
                ct_sb_adds(&b, ", but got ");
                show(&b, *v);
                verdict(vm, e, ok, t, &want, b.p);
                ct_sb_free(&b);
            }
            verdict(vm, e, ok, t, &want, NULL);
        }
        /* chai moves the subject to the property */
        if (v && !e->neg) e->actual = *v;
        break;
    }
    case E_LENGTHOF: {
        int n = size_of(vm, a);
        char tail[64], w[40];
        ct_num_fmt(n, w);
        snprintf(tail, sizeof tail, " but got %s", w);
        verdict(vm, e, n == (int)ct_to_num(x), "have a length of", &x, n == (int)ct_to_num(x) ? NULL : tail);
        break;
    }
    case E_A: {
        ct_str_t *t = vm_to_str(vm, x);
        char want[40];
        ct_copy(want, sizeof want, t->p);
        for (char *p = want; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        const char *got = type_name(a);
        char w[80];
        bool vowel = strchr("aeiou", want[0]) != NULL;
        snprintf(w, sizeof w, "be %s %s", vowel ? "an" : "a", want);
        verdict(vm, e, !strcmp(got, want), w, NULL, NULL);
        break;
    }
    case E_ONEOF: {
        bool ok = false;
        for (uint32_t i = 0; i < ct_len(x); i++) {
            if (e->deep ? ct_deep_equal(a, x.a->v[i]) : ct_strict_eq(a, x.a->v[i])) ok = true;
        }
        verdict(vm, e, ok, "be one of", &x, NULL);
        break;
    }
    case E_MATCH: case E_STRING: {
        if (x.t == CT_OBJ) ct_throw_err(vm, "TypeError", "regular expressions are not supported on the board");
        ct_str_t *s = vm_to_str(vm, a), *n = vm_to_str(vm, x);
        verdict(vm, e, strstr(s->p, n->p) != NULL, m == E_MATCH ? "match" : "contain", &x, NULL);
        break;
    }
    case E_KEYS: {
        if (a.t != CT_OBJ) verdict(vm, e, false, "have keys", &x, NULL);
        ctv_t want = x.t == CT_ARR ? x : vm_arr(vm);
        if (x.t != CT_ARR) for (int i = 0; i < argc; i++) vm_push(vm, want, argv[i]);
        bool ok = true;
        for (uint32_t i = 0; i < ct_len(want); i++) {
            ct_str_t *k = vm_to_str(vm, want.a->v[i]);
            if (!ct_obj_find(a.o, k->p, k->len)) ok = false;
        }
        verdict(vm, e, ok, "have keys", &want, NULL);
        break;
    }
    case E_MEMBERS: {
        bool ok = a.t == CT_ARR && x.t == CT_ARR && a.a->n == x.a->n;
        for (uint32_t i = 0; ok && i < x.a->n; i++) ok = includes(a, x.a->v[i], true);
        verdict(vm, e, ok, "have the same members as", &x, NULL);
        break;
    }
    case E_SATISFY: {
        ctv_t ok = ct_call(vm, x, ct_undef(), 1, &a);
        verdict(vm, e, ct_truthy(ok), "satisfy the function", NULL, NULL);
        break;
    }
    case E_INSTANCEOF: {
        const char *tag = ct_s(ct_get(x, "__tag"));
        bool ok = (!strcmp(tag, "Array") && a.t == CT_ARR) || (!strcmp(tag, "Object") && a.t == CT_OBJ) ||
                  (!strcmp(tag, "Error") && a.t == CT_OBJ && a.o->kind == CT_K_ERROR) ||
                  (!strcmp(tag, "Date") && a.t == CT_OBJ && a.o->kind == CT_K_DATE);
        char w[64];
        snprintf(w, sizeof w, "be an instance of %s", tag[0] ? tag : "it");
        verdict(vm, e, ok, w, NULL, NULL);
        break;
    }
    case E_STATUS: {
        if (!r) ct_throw_err(vm, "Error", "there is no response");
        char w[160];
        if (x.t == CT_STR) {
            snprintf(w, sizeof w, "have status reason '%.60s' but got '%.60s'", x.s->p, r->reason);
            verdict(vm, e, ieq(r->reason, strlen(r->reason), x.s->p), w, NULL, NULL);
        } else {
            char a1[40];
            ct_num_fmt(ct_to_num(x), a1);
            snprintf(w, sizeof w, "have status code %s but got %d", a1, r->status);
            verdict(vm, e, r->status == (int)ct_to_num(x), w, NULL, NULL);
        }
        break;
    }
    case E_HEADER: {
        if (!r) ct_throw_err(vm, "Error", "there is no response");
        ct_str_t *k = vm_to_str(vm, x);
        const char *v = ct_resp_header(r, k->p);
        char w[300];
        if (argc < 2) {
            snprintf(w, sizeof w, "have header '%.100s'", k->p);
            verdict(vm, e, v != NULL, w, NULL, NULL);
        } else {
            ct_str_t *want = vm_to_str(vm, argv[1]);
            snprintf(w, sizeof w, "have header '%.100s' with value '%.100s'", k->p, want->p);
            verdict(vm, e, v && !strcmp(v, want->p), w, NULL, NULL);
        }
        break;
    }
    case E_JSONBODY: {
        ctv_t j = resp_json(vm);
        if (argc == 0) {
            verdict(vm, e, true, "have a JSON body", NULL, NULL);
            break;
        }
        ct_str_t *path = vm_to_str(vm, x);
        ctv_t got;
        bool has = path_get(vm, j, path->p, &got);
        char w[200];
        if (argc < 2) {
            snprintf(w, sizeof w, "have a JSON body with '%.150s'", path->p);
            verdict(vm, e, has, w, NULL, NULL);
        } else {
            snprintf(w, sizeof w, "have a JSON body with '%.150s' equal to", path->p);
            verdict(vm, e, has && ct_deep_equal(got, argv[1]), w, &argv[1], NULL);
        }
        break;
    }
    case E_BODY: {
        if (!r) ct_throw_err(vm, "Error", "there is no response");
        if (argc == 0) {
            verdict(vm, e, r->len > 0, "have a body", NULL, NULL);
        } else {
            ct_str_t *want = vm_to_str(vm, x);
            bool ok = (size_t)r->len == want->len && r->body && !memcmp(r->body, want->p, want->len);
            verdict(vm, e, ok, "have body", &x, NULL);
        }
        break;
    }
    case E_JSONSCHEMA:
        ct_throw_err(vm, "TypeError", "jsonSchema is not supported on the board (the portal runs it)");
    case E_EXIST_FN:
        verdict(vm, e, a.t != CT_UNDEF && a.t != CT_NULL, "exist", NULL, NULL);
        break;
    }
    return self;
}

static const struct { const char *n; int m; } EXPECT_FNS[] = {
    { "equal", E_EQUAL }, { "equals", E_EQUAL }, { "eq", E_EQUAL }, { "eql", E_EQL }, { "eqls", E_EQL },
    { "above", E_ABOVE }, { "gt", E_ABOVE }, { "greaterThan", E_ABOVE }, { "below", E_BELOW }, { "lt", E_BELOW },
    { "lessThan", E_BELOW }, { "least", E_LEAST }, { "gte", E_LEAST }, { "greaterThanOrEqual", E_LEAST },
    { "most", E_MOST }, { "lte", E_MOST }, { "lessThanOrEqual", E_MOST }, { "within", E_WITHIN },
    { "closeTo", E_CLOSETO }, { "approximately", E_CLOSETO }, { "include", E_INCLUDE }, { "includes", E_INCLUDE },
    { "contain", E_INCLUDE }, { "contains", E_INCLUDE }, { "property", E_PROPERTY }, { "lengthOf", E_LENGTHOF },
    { "length", E_LENGTHOF }, { "a", E_A }, { "an", E_A }, { "oneOf", E_ONEOF }, { "match", E_MATCH },
    { "string", E_STRING }, { "keys", E_KEYS }, { "key", E_KEYS }, { "members", E_MEMBERS },
    { "satisfy", E_SATISFY }, { "satisfies", E_SATISFY }, { "instanceOf", E_INSTANCEOF },
    { "instanceof", E_INSTANCEOF }, { "status", E_STATUS }, { "header", E_HEADER }, { "jsonBody", E_JSONBODY },
    { "body", E_BODY }, { "jsonSchema", E_JSONSCHEMA },
};

static const char *const CHAINS[] = {
    "to", "be", "been", "is", "that", "which", "and", "has", "have", "with", "at", "of", "same", "but", "does",
    "still", "also", "own", "nested", "any", "all", "ordered",
};

static bool word(const char *k, size_t kl, const char *w)
{
    return strlen(w) == kl && !memcmp(k, w, kl);
}

static ctv_t expect_get(ct_vm_t *vm, ct_obj_t *o, const char *k, size_t kl, bool *found)
{
    expect_t *e = o->ud;
    ctv_t self = ct_undef();
    self.t = CT_OBJ;
    self.o = o;
    *found = true;
    for (size_t i = 0; i < sizeof CHAINS / sizeof CHAINS[0]; i++) if (word(k, kl, CHAINS[i])) return self;
    if (word(k, kl, "not")) {
        e->neg = !e->neg;
        return self;
    }
    if (word(k, kl, "deep")) {
        e->deep = true;
        return self;
    }
    ctv_t a = e->actual;
    const ct_resp_t *r = vm->ctx->resp;
    if (e->resp) {
        static const struct { const char *w; int lo, hi; const char *say; } RS[] = {
            { "ok", 200, 200, "be ok" }, { "success", 200, 299, "be a success (2xx)" },
            { "accepted", 202, 202, "be accepted (202)" }, { "badRequest", 400, 400, "be a bad request (400)" },
            { "unauthorized", 401, 401, "be unauthorized (401)" }, { "forbidden", 403, 403, "be forbidden (403)" },
            { "notFound", 404, 404, "be not found (404)" }, { "rateLimited", 429, 429, "be rate limited (429)" },
            { "clientError", 400, 499, "be a client error (4xx)" }, { "serverError", 500, 599, "be a server error (5xx)" },
            { "error", 400, 599, "be an error (4xx or 5xx)" }, { "info", 100, 199, "be informational (1xx)" },
            { "redirection", 300, 399, "be a redirection (3xx)" }, { "redirect", 300, 399, "be a redirection (3xx)" },
        };
        for (size_t i = 0; i < sizeof RS / sizeof RS[0]; i++) {
            if (!word(k, kl, RS[i].w)) continue;
            if (!r) ct_throw_err(vm, "Error", "there is no response");
            char w[96];
            snprintf(w, sizeof w, "%s but got %d", RS[i].say, r->status);
            verdict(vm, e, r->status >= RS[i].lo && r->status <= RS[i].hi, w, NULL, NULL);
            return self;
        }
        if (word(k, kl, "json")) {
            const char *ct = r ? ct_resp_header(r, "Content-Type") : NULL;
            bool ok = ct && strstr(ct, "json");
            verdict(vm, e, ok, "be json", NULL, NULL);
            if (ok && !e->neg) resp_json(vm);
            return self;
        }
        if (word(k, kl, "withBody")) {
            verdict(vm, e, r && r->len > 0, "have a body", NULL, NULL);
            return self;
        }
        if (word(k, kl, "html") || word(k, kl, "xml")) {
            const char *ct = r ? ct_resp_header(r, "Content-Type") : NULL;
            bool ok = ct && strstr(ct, k[0] == 'h' ? "html" : "xml");
            verdict(vm, e, ok, k[0] == 'h' ? "be html" : "be xml", NULL, NULL);
            return self;
        }
    }
    if (word(k, kl, "true")) { verdict(vm, e, a.t == CT_BOOL && a.b, "be true", NULL, NULL); return self; }
    if (word(k, kl, "false")) { verdict(vm, e, a.t == CT_BOOL && !a.b, "be false", NULL, NULL); return self; }
    if (word(k, kl, "null")) { verdict(vm, e, a.t == CT_NULL, "be null", NULL, NULL); return self; }
    if (word(k, kl, "undefined")) { verdict(vm, e, a.t == CT_UNDEF, "be undefined", NULL, NULL); return self; }
    if (word(k, kl, "NaN")) { verdict(vm, e, a.t == CT_NUM && a.n != a.n, "be NaN", NULL, NULL); return self; }
    if (word(k, kl, "ok")) { verdict(vm, e, ct_truthy(a), "be truthy", NULL, NULL); return self; }
    if (word(k, kl, "exist") || word(k, kl, "exists")) {
        verdict(vm, e, a.t != CT_UNDEF && a.t != CT_NULL, "exist", NULL, NULL);
        return self;
    }
    if (word(k, kl, "finite")) {
        verdict(vm, e, a.t == CT_NUM && a.n == a.n && a.n < 1.7e308 && a.n > -1.7e308, "be finite", NULL, NULL);
        return self;
    }
    if (word(k, kl, "empty")) {
        bool ok = (a.t == CT_STR && !a.s->len) || (a.t == CT_ARR && !a.a->n) || (a.t == CT_OBJ && !a.o->n);
        verdict(vm, e, ok, "be empty", NULL, NULL);
        return self;
    }
    for (size_t i = 0; i < sizeof EXPECT_FNS / sizeof EXPECT_FNS[0]; i++) {
        if (!word(k, kl, EXPECT_FNS[i].n)) continue;
        ctv_t f = vm_fn(vm, expect_call, (void *)(intptr_t)EXPECT_FNS[i].m, EXPECT_FNS[i].n);
        f.f->self = self;
        f.f->bound = true;
        return f;
    }
    *found = false;
    return ct_undef();
}

static ctv_t resp_get(ct_vm_t *vm, ct_obj_t *o, const char *k, size_t kl, bool *found)
{
    *found = false;
    if (!word(k, kl, "to")) return ct_undef();
    *found = true;
    ctv_t self = ct_undef();
    self.t = CT_OBJ;
    self.o = o;
    return make_expect(vm, self, ct_undef(), true);
}

static ctv_t pm_expect(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    (void)ud;
    return make_expect(vm, arg(argc, argv, 0), arg(argc, argv, 1), false);
}

/* ---- pm.test ------------------------------------------------------------------------------------ */

static void add_test(ct_vm_t *vm, const char *name, bool ok, const char *err, bool skipped)
{
    ct_script_t *ctx = vm->ctx;
    if (ctx->tests.t != CT_ARR) ctx->tests = vm_arr(vm);
    ctv_t t = vm_obj(vm);
    vm_set(vm, t, "name", vm_cstr(vm, name));
    vm_set(vm, t, "ok", ct_bool(ok));
    vm_set(vm, t, "error", vm_cstr(vm, err ? err : ""));
    if (skipped) vm_set(vm, t, "skipped", ct_bool(true));
    vm_push(vm, ctx->tests, t);
}

static ctv_t pm_test(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    ct_str_t *name = vm_to_str(vm, arg(argc, argv, 0));
    ctv_t fn = arg(argc, argv, 1);
    if (ud || fn.t != CT_FN) {
        add_test(vm, name->p, true, NULL, true);
        return ct_undef();
    }
    jmp_buf jb;
    jmp_buf *prev = vm->jb;
    int depth = vm->depth;
    ctv_t this_v = vm->this_v;
    vm->jb = &jb;
    if (!setjmp(jb)) {
        ct_call(vm, fn, ct_undef(), 0, NULL);
        vm->jb = prev;
        add_test(vm, name->p, true, NULL, false);
        return ct_undef();
    }
    vm->jb = prev;
    vm->depth = depth;
    vm->this_v = this_v;
    vm->shorted = false;
    /* out of memory or stopped: that ends the script, not only the test */
    if (vm->ar->full || (vm->ctx->stop && *vm->ctx->stop)) longjmp(*vm->jb, 1);
    ctv_t err = vm->thrown;
    char msg[300];
    if (err.t == CT_OBJ && err.o->kind == CT_K_ERROR) {
        snprintf(msg, sizeof msg, "%s: %.250s (line %d)", ct_s(ct_get(err, "name")), ct_s(ct_get(err, "message")),
                 vm->line);
    } else {
        ct_sb_t b = { 0 };
        ct_inspect(&b, err, 0);
        snprintf(msg, sizeof msg, "%.280s", b.p ? b.p : "?");
        ct_sb_free(&b);
    }
    add_test(vm, name->p, false, msg, false);
    return ct_undef();
}

/* ---- the request ---------------------------------------------------------------------------------- */

static ctv_t *req_headers(ct_vm_t *vm)
{
    ctv_t *req = &vm->ctx->req;
    if (req->t != CT_OBJ) *req = vm_obj(vm);
    ctv_t *h = ct_obj_find(req->o, "header", 6);
    if (!h || h->t != CT_ARR) {
        vm_set(vm, *req, "header", vm_arr(vm));
        h = ct_obj_find(req->o, "header", 6);
    }
    return h;
}

enum { H_ADD, H_UPSERT, H_REMOVE, H_GET, H_HAS, H_TOOBJECT, H_ALL };

static ctv_t req_hdr_api(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    int op = (int)(intptr_t)ud;
    ctv_t *h = req_headers(vm);
    ctv_t a0 = arg(argc, argv, 0);
    ct_str_t *key = NULL;
    ctv_t val = ct_undef();
    if (a0.t == CT_OBJ) {
        key = vm_to_str(vm, ct_get(a0, "key"));
        val = ct_get(a0, "value");
    } else if (a0.t != CT_UNDEF) {
        key = vm_to_str(vm, a0);
        val = arg(argc, argv, 1);
    }
    switch (op) {
    case H_ADD: case H_UPSERT: {
        if (!key) return ct_undef();
        if (op == H_UPSERT) {
            for (uint32_t i = 0; i < h->a->n; i++) {
                ctv_t e = h->a->v[i];
                if (ieq(ct_s(ct_get(e, "key")), strlen(ct_s(ct_get(e, "key"))), key->p) && !ct_truthy(ct_get(e, "disabled"))) {
                    vm_set(vm, e, "value", strv(vm_to_str(vm, val)));
                    return ct_undef();
                }
            }
        }
        ctv_t e = vm_obj(vm);
        vm_set(vm, e, "key", strv(key));
        vm_set(vm, e, "value", strv(vm_to_str(vm, val.t == CT_UNDEF ? vm_cstr(vm, "") : val)));
        vm_push(vm, *h, e);
        return ct_undef();
    }
    case H_REMOVE:
        if (!key) return ct_undef();
        for (uint32_t i = h->a->n; i-- > 0;) {
            const char *k = ct_s(ct_get(h->a->v[i], "key"));
            if (ieq(k, strlen(k), key->p)) ct_arr_remove(h->a, i);
        }
        return ct_undef();
    case H_GET: case H_HAS:
        for (uint32_t i = 0; key && i < h->a->n; i++) {
            ctv_t e = h->a->v[i];
            const char *k = ct_s(ct_get(e, "key"));
            if (ieq(k, strlen(k), key->p) && !ct_truthy(ct_get(e, "disabled"))) {
                return op == H_HAS ? ct_bool(true) : ct_get(e, "value");
            }
        }
        return op == H_HAS ? ct_bool(false) : ct_undef();
    case H_TOOBJECT: {
        ctv_t o = vm_obj(vm);
        for (uint32_t i = 0; i < h->a->n; i++) {
            ctv_t e = h->a->v[i];
            if (ct_truthy(ct_get(e, "disabled"))) continue;
            vm_set(vm, o, ct_s(ct_get(e, "key")), ct_get(e, "value"));
        }
        return o;
    }
    case H_ALL:
        return *h;
    }
    return ct_undef();
}

static ctv_t url_api(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    int op = (int)(intptr_t)ud;
    ctv_t *req = &vm->ctx->req;
    ct_str_t *url = vm_to_str(vm, ct_get(*req, "url"));
    char host[160], path[1024];
    int port = 0;
    bool tls = false;
    if (!ct_url_split(url->p, host, sizeof host, &port, path, sizeof path, &tls)) {
        host[0] = '\0';
        ct_copy(path, sizeof path, url->p);
    }
    switch (op) {
    case 0: return strv(url);                                   /* toString */
    case 1: return vm_cstr(vm, host);                           /* getHost */
    case 2: {                                                   /* getPath */
        char *q = strchr(path, '?');
        if (q) *q = '\0';
        return vm_cstr(vm, path);
    }
    case 3: {                                                   /* getQueryString */
        char *q = strchr(path, '?');
        return vm_cstr(vm, q ? q + 1 : "");
    }
    case 4: {                                                   /* query.add({key, value}) */
        ctv_t a0 = arg(argc, argv, 0);
        ct_str_t *k = vm_to_str(vm, a0.t == CT_OBJ ? ct_get(a0, "key") : a0);
        ct_str_t *v = vm_to_str(vm, a0.t == CT_OBJ ? ct_get(a0, "value") : arg(argc, argv, 1));
        ct_sb_t b = { 0 };
        ct_sb_add(&b, url->p, url->len);
        ct_sb_addc(&b, strchr(url->p, '?') ? '&' : '?');
        ct_sb_add(&b, k->p, k->len);
        ct_sb_addc(&b, '=');
        ct_sb_add(&b, v->p, v->len);
        vm_set(vm, *req, "url", vm_str(vm, b.p ? b.p : "", b.n));
        ct_sb_free(&b);
        return ct_undef();
    }
    case 5: {                                                   /* query.get(key) */
        ct_str_t *k = vm_to_str(vm, arg(argc, argv, 0));
        const char *q = strchr(url->p, '?');
        while (q) {
            q++;
            if (!strncmp(q, k->p, k->len) && (q[k->len] == '=' || q[k->len] == '&' || !q[k->len])) {
                const char *v = q[k->len] == '=' ? q + k->len + 1 : q + k->len;
                size_t n = strcspn(v, "&#");
                return vm_str(vm, v, n);
            }
            q = strchr(q, '&');
        }
        return ct_undef();
    }
    }
    return ct_undef();
}

static ctv_t req_get(ct_vm_t *vm, ct_obj_t *o, const char *k, size_t kl, bool *found)
{
    (void)o;
    *found = false;
    if (word(k, kl, "headers")) {
        ctv_t h = vm_obj(vm);
        h.o->kind = CT_K_SYS;
        static const struct { const char *n; int op; } M[] = {
            { "add", H_ADD }, { "upsert", H_UPSERT }, { "remove", H_REMOVE }, { "get", H_GET }, { "has", H_HAS },
            { "toObject", H_TOOBJECT }, { "all", H_ALL },
        };
        for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) vm_method(vm, h, M[i].n, req_hdr_api, (void *)(intptr_t)M[i].op);
        *found = true;
        return h;
    }
    if (word(k, kl, "url")) {
        ctv_t *req = &vm->ctx->req;
        ctv_t *raw = ct_obj_find(req->o, "url", 3);
        ctv_t u = vm_obj(vm);
        u.o->kind = CT_K_SYS;
        vm_set(vm, u, "__str", raw && raw->t == CT_STR ? *raw : vm_cstr(vm, ""));
        static const char *const M[] = { "toString", "getHost", "getPath", "getQueryString" };
        for (int i = 0; i < 4; i++) vm_method(vm, u, M[i], url_api, (void *)(intptr_t)i);
        ctv_t q = vm_obj(vm);
        q.o->kind = CT_K_SYS;
        vm_method(vm, q, "add", url_api, (void *)4);
        vm_method(vm, q, "get", url_api, (void *)5);
        vm_set(vm, u, "query", q);
        *found = true;
        return u;
    }
    return ct_undef();
}

/* ---- the runner and the old postman object --------------------------------------------------------- */

enum { X_NEXT, X_SKIP, P_SETENV, P_GETENV, P_CLEARENV, P_SETGLOB, P_GETGLOB, P_CLEARGLOB, P_RESPHDR };

static ctv_t exec_api(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    ct_script_t *ctx = vm->ctx;
    ctv_t a = arg(argc, argv, 0);
    switch ((int)(intptr_t)ud) {
    case X_NEXT:
        ctx->next_set = true;
        if (a.t == CT_NULL || a.t == CT_UNDEF || (a.t == CT_STR && !a.s->len)) {
            ctx->next_stop = true;
            ctx->next[0] = '\0';
        } else {
            ctx->next_stop = false;
            ct_copy(ctx->next, sizeof ctx->next, vm_to_str(vm, a)->p);
        }
        return ct_undef();
    case X_SKIP:
        ctx->skip = true;
        return ct_undef();
    case P_SETENV: case P_SETGLOB: {
        ctv_t v = arg(argc, argv, 1);
        scope_set(vm, (intptr_t)ud == P_SETENV ? SC_ENV : SC_GLOB, vm_to_str(vm, a), v.t == CT_UNDEF ? vm_cstr(vm, "") : v);
        return ct_undef();
    }
    case P_CLEARENV: case P_CLEARGLOB:
        scope_set(vm, (intptr_t)ud == P_CLEARENV ? SC_ENV : SC_GLOB, vm_to_str(vm, a), ct_undef());
        return ct_undef();
    case P_GETENV: case P_GETGLOB: {
        ctv_t *o = scope_obj(ctx, (intptr_t)ud == P_GETENV ? SC_ENV : SC_GLOB);
        ct_str_t *k = vm_to_str(vm, a);
        ctv_t *v = o->t == CT_OBJ ? ct_obj_find(o->o, k->p, k->len) : NULL;
        return v ? *v : ct_undef();
    }
    case P_RESPHDR:
        if (!ctx->resp) return ct_undef();
        return hdr_value(vm, vm_to_str(vm, a)->p);
    }
    return ct_undef();
}

static void global(ct_vm_t *vm, const char *name, ctv_t v)
{
    ct_declare(vm, vm->global, ct_intern(vm, name, strlen(name)), v, false);
}

void ct_pm_install(ct_vm_t *vm)
{
    ct_script_t *ctx = vm->ctx;
    ctv_t pm = vm_obj(vm);
    pm.o->kind = CT_K_SYS;

    vm_method(vm, pm, "test", pm_test, NULL);
    vm_method(vm, pm, "expect", pm_expect, NULL);
    vm_set(vm, pm, "environment", var_obj(vm, SC_ENV));
    vm_set(vm, pm, "globals", var_obj(vm, SC_GLOB));
    vm_set(vm, pm, "collectionVariables", var_obj(vm, SC_COLL));
    vm_set(vm, pm, "variables", var_obj(vm, SC_LOCAL));
    vm_set(vm, pm, "iterationData", var_obj(vm, SC_LOCAL));

    ctv_t info = vm_obj(vm);
    vm_set(vm, info, "requestName", vm_cstr(vm, ctx->request_name ? ctx->request_name : ""));
    vm_set(vm, info, "eventName", vm_cstr(vm, ctx->event ? ctx->event : ""));
    vm_set(vm, info, "iteration", ct_num(ctx->iteration));
    vm_set(vm, info, "iterationCount", ct_num(ctx->iteration + 1));
    vm_set(vm, pm, "info", info);

    ctv_t ex = vm_obj(vm);
    ex.o->kind = CT_K_SYS;
    vm_method(vm, ex, "setNextRequest", exec_api, (void *)X_NEXT);
    vm_method(vm, ex, "skipRequest", exec_api, (void *)X_SKIP);
    vm_set(vm, pm, "execution", ex);

    if (ctx->req.t != CT_OBJ) ctx->req = vm_obj(vm);
    ctx->req.o->get = req_get;
    vm_set(vm, pm, "request", ctx->req);

    if (ctx->resp) {
        const ct_resp_t *r = ctx->resp;
        ctv_t res = vm_obj(vm);
        res.o->kind = CT_K_SYS;
        vm_set(vm, res, "code", ct_num(r->status));
        vm_set(vm, res, "status", vm_cstr(vm, r->reason));
        vm_set(vm, res, "responseTime", ct_num(r->t_total));
        vm_set(vm, res, "responseSize", ct_num(r->len));
        vm_method(vm, res, "json", resp_api, (void *)R_JSON);
        vm_method(vm, res, "text", resp_api, (void *)R_TEXT);
        vm_method(vm, res, "reason", resp_api, (void *)R_REASON);
        ctv_t h = vm_obj(vm);
        h.o->kind = CT_K_SYS;
        vm_method(vm, h, "get", resp_api, (void *)R_HGET);
        vm_method(vm, h, "has", resp_api, (void *)R_HHAS);
        vm_method(vm, h, "toObject", resp_api, (void *)R_HOBJ);
        vm_method(vm, h, "all", resp_api, (void *)R_HALL);
        vm_method(vm, h, "each", resp_api, (void *)R_HEACH);
        vm_method(vm, h, "count", resp_api, (void *)R_HCOUNT);
        vm_set(vm, res, "headers", h);
        ctv_t c = vm_obj(vm);
        c.o->kind = CT_K_SYS;
        vm_method(vm, c, "get", resp_api, (void *)R_CGET);
        vm_method(vm, c, "has", resp_api, (void *)R_CHAS);
        vm_method(vm, c, "toObject", resp_api, (void *)R_COBJ);
        vm_set(vm, res, "cookies", c);
        res.o->get = resp_get;     /* .to: a fresh assertion each time */
        vm_set(vm, pm, "response", res);

        /* the old globals */
        global(vm, "responseBody", vm_str(vm, r->body ? (const char *)r->body : "", (size_t)r->len));
        ctv_t code = vm_obj(vm);
        vm_set(vm, code, "code", ct_num(r->status));
        vm_set(vm, code, "name", vm_cstr(vm, r->reason));
        vm_set(vm, code, "detail", vm_cstr(vm, r->reason));
        global(vm, "responseCode", code);
        global(vm, "responseTime", ct_num(r->t_total));
        ctv_t rh = vm_obj(vm);
        for (int i = 0; i < r->nhdr; i++) vm_set(vm, rh, r->hdr[i].k, vm_cstr(vm, r->hdr[i].v));
        global(vm, "responseHeaders", rh);
    }

    global(vm, "pm", pm);

    ctv_t con = vm_obj(vm);
    con.o->kind = CT_K_SYS;
    vm_method(vm, con, "log", console_api, (void *)0);
    vm_method(vm, con, "debug", console_api, (void *)0);
    vm_method(vm, con, "info", console_api, (void *)1);
    vm_method(vm, con, "warn", console_api, (void *)2);
    vm_method(vm, con, "error", console_api, (void *)3);
    global(vm, "console", con);

    ctv_t postman = vm_obj(vm);
    postman.o->kind = CT_K_SYS;
    vm_method(vm, postman, "setNextRequest", exec_api, (void *)X_NEXT);
    vm_method(vm, postman, "setEnvironmentVariable", exec_api, (void *)P_SETENV);
    vm_method(vm, postman, "getEnvironmentVariable", exec_api, (void *)P_GETENV);
    vm_method(vm, postman, "clearEnvironmentVariable", exec_api, (void *)P_CLEARENV);
    vm_method(vm, postman, "setGlobalVariable", exec_api, (void *)P_SETGLOB);
    vm_method(vm, postman, "getGlobalVariable", exec_api, (void *)P_GETGLOB);
    vm_method(vm, postman, "clearGlobalVariable", exec_api, (void *)P_CLEARGLOB);
    vm_method(vm, postman, "getResponseHeader", exec_api, (void *)P_RESPHDR);
    global(vm, "postman", postman);

    vm->legacy_tests = vm_obj(vm);
    global(vm, "tests", vm->legacy_tests);
    global(vm, "environment", ctx->env.t == CT_OBJ ? ctx->env : vm_obj(vm));
    global(vm, "globals", ctx->glob.t == CT_OBJ ? ctx->glob : vm_obj(vm));
    global(vm, "iteration", ct_num(ctx->iteration));
    global(vm, "data", vm_obj(vm));
    ctv_t lreq = vm_obj(vm);
    vm_set(vm, lreq, "url", ct_get(ctx->req, "url"));
    vm_set(vm, lreq, "method", ct_get(ctx->req, "method"));
    vm_set(vm, lreq, "name", vm_cstr(vm, ctx->request_name ? ctx->request_name : ""));
    ctv_t lh = vm_obj(vm);
    ctv_t hs = ct_get(ctx->req, "header");
    for (uint32_t i = 0; i < ct_len(hs); i++) {
        ctv_t e = hs.a->v[i];
        if (!ct_truthy(ct_get(e, "disabled"))) vm_set(vm, lh, ct_s(ct_get(e, "key")), ct_get(e, "value"));
    }
    vm_set(vm, lreq, "headers", lh);
    vm_set(vm, lreq, "data", ct_get(ct_get(ctx->req, "body"), "raw"));
    global(vm, "request", lreq);
}

void ct_pm_finish(ct_vm_t *vm)
{
    ctv_t t = vm->legacy_tests;
    if (t.t != CT_OBJ) return;
    for (uint32_t i = 0; i < t.o->n; i++) {
        bool ok = ct_truthy(t.o->p[i].v);
        add_test(vm, t.o->p[i].k->p, ok, ok ? NULL : "AssertionError: the test was false", false);
    }
}
