/*
 * P4OS - Cartero: a small JavaScript, for the tests and pre-request scripts
 * of a collection (README, "Scripts").
 *
 * What Postman's scripts are written in, and not more: declarations,
 * functions and arrows with closures, the usual statements, try/catch,
 * objects, arrays, template strings, optional chaining, destructuring in a
 * declaration; strings, arrays and numbers with their common methods, JSON,
 * Math, Date. No regular expressions, classes, generators or promises: the
 * board says so in a SyntaxError and the portal, which runs the browser's
 * JavaScript, runs those.
 *
 * Parsed into a tree (ct_js_int.h) in the job's arena, then walked. A throw
 * is a longjmp to the nearest try. A runaway script is stopped by a count of
 * steps and a clock, and a deep recursion by a count of calls: the job's
 * thread has a stack of its own and nothing here may outgrow it.
 */
#include "ct_js_int.h"

#include "aos_hal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_STEPS     20000000L
#define MAX_MS        6000
#define MAX_DEPTH     48
#define MAX_PDEPTH    120

/* ---- small numeric helpers (the board's libm has float only) ----------------------------- */

static double d_trunc(double x)
{
    if (x != x || x > 4.5e15 || x < -4.5e15) return x;
    return (double)(int64_t)x;
}

static double d_floor(double x)
{
    double t = d_trunc(x);
    return (t > x) ? t - 1 : t;
}

static double d_ceil(double x)
{
    double t = d_trunc(x);
    return (t < x) ? t + 1 : t;
}

static double d_fmod(double a, double b)
{
    if (b == 0 || a != a || b != b) return 0.0 / 0.0;
    if (a > 1.7e308 || a < -1.7e308) return 0.0 / 0.0;
    if (b > 1.7e308 || b < -1.7e308) return a;
    double q = d_trunc(a / b);
    double r = a - q * b;
    /* the division can be off by one at the edges */
    if ((a >= 0 && r < 0) || (a < 0 && r > 0)) r += (r < 0) == (b < 0) ? -b : b;
    if ((b > 0 && (r >= b || r <= -b)) || (b < 0 && (r <= b || r >= -b))) r -= q;
    return r;
}

static double d_pow(double a, double b)
{
    if (b == 0) return 1;
    if (b == d_trunc(b) && b > -1024 && b < 1024) {
        int64_t e = (int64_t)(b < 0 ? -b : b);
        double r = 1, x = a;
        while (e) {
            if (e & 1) r *= x;
            x *= x;
            e >>= 1;
        }
        return b < 0 ? 1 / r : r;
    }
    return (double)powf((float)a, (float)b);
}

static double d_sqrt(double x)
{
    if (x < 0 || x != x) return 0.0 / 0.0;
    if (x == 0 || x > 1.7e308) return x;
    double r = (double)sqrtf((float)x);
    if (r == 0) r = x;
    for (int i = 0; i < 3; i++) r = 0.5 * (r + x / r);
    return r;
}

static int32_t to_i32(double d)
{
    if (d != d || d > 1.7e308 || d < -1.7e308) return 0;
    d = d_trunc(d);
    if (d > 9.2e18 || d < -9.2e18) return 0;
    return (int32_t)(uint32_t)(int64_t)d;
}

double ct_now_ms(void)
{
    if (aos_hal_time_is_valid()) {
        return (double)time(NULL) * 1000.0 + (double)(aos_hal_uptime_ms() % 1000);
    }
    return (double)aos_hal_uptime_ms();
}

uint32_t ct_rand(ct_vm_t *vm)
{
    uint32_t x = vm->rnd ? vm->rnd : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->rnd = x;
    return x;
}

/* ---- allocation and throwing ------------------------------------------------------------- */

void ct_throw(ct_vm_t *vm, ctv_t v)
{
    vm->thrown = v;
    longjmp(*vm->jb, 1);
}

ctv_t ct_make_error(ct_vm_t *vm, const char *name, const char *msg)
{
    ctv_t e = ct_obj_new(vm->ar, 3);
    if (e.t != CT_OBJ) {
        vm->thrown = ct_null();
        longjmp(*vm->jb, 1);
    }
    e.o->kind = CT_K_ERROR;
    ct_obj_set(vm->ar, e.o, "name", 4, ct_cstr(vm->ar, name));
    ct_obj_set(vm->ar, e.o, "message", 7, ct_cstr(vm->ar, msg));
    return e;
}

void ct_throw_err(ct_vm_t *vm, const char *name, const char *fmt, ...)
{
    char msg[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ct_throw(vm, ct_make_error(vm, name, msg));
}

static void oom(ct_vm_t *vm) __attribute__((noreturn));
static void oom(ct_vm_t *vm)
{
    /* the arena is full: the error itself may not fit, so it is a null
     * the top level knows to report as memory */
    vm->thrown = ct_undef();
    vm->thrown.t = CT_NULL;
    vm->ar->full = true;
    longjmp(*vm->jb, 1);
}

static void *vm_alloc(ct_vm_t *vm, size_t n)
{
    void *p = ct_alloc(vm->ar, n);
    if (!p) oom(vm);
    return p;
}

ctv_t vm_str(ct_vm_t *vm, const char *p, size_t n)
{
    ctv_t v = ct_strv(vm->ar, p, n);
    if (v.t != CT_STR) oom(vm);
    return v;
}

ctv_t vm_cstr(ct_vm_t *vm, const char *p)
{
    return vm_str(vm, p ? p : "", p ? strlen(p) : 0);
}

ctv_t vm_arr(ct_vm_t *vm)
{
    ctv_t v = ct_arr_new(vm->ar, 0);
    if (v.t != CT_ARR) oom(vm);
    return v;
}

ctv_t vm_obj(ct_vm_t *vm)
{
    ctv_t v = ct_obj_new(vm->ar, 0);
    if (v.t != CT_OBJ) oom(vm);
    return v;
}

ctv_t vm_fn(ct_vm_t *vm, ct_native_t f, void *ud, const char *name)
{
    ctv_t v = ct_fn_native(vm->ar, f, ud, name);
    if (v.t != CT_FN) oom(vm);
    return v;
}

void vm_push(ct_vm_t *vm, ctv_t arr, ctv_t v)
{
    if (arr.t != CT_ARR || !ct_arr_push(vm->ar, arr.a, v)) oom(vm);
}

void vm_set(ct_vm_t *vm, ctv_t obj, const char *k, ctv_t v)
{
    if (obj.t != CT_OBJ || !ct_obj_set(vm->ar, obj.o, k, strlen(k), v)) oom(vm);
}

void vm_method(ct_vm_t *vm, ctv_t obj, const char *k, ct_native_t f, void *ud)
{
    vm_set(vm, obj, k, vm_fn(vm, f, ud, k));
}

ct_str_t *vm_to_str(ct_vm_t *vm, ctv_t v)
{
    ct_str_t *s = ct_to_str(vm->ar, v);
    if (!s) oom(vm);
    return s;
}

/* Every name the program uses goes through here, so a variable is found by
 * comparing pointers. */
ct_str_t *ct_intern(ct_vm_t *vm, const char *p, size_t n)
{
    intern_t *t = &vm->names;
    if (t->n * 2 >= t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 256;
        ct_str_t **slot = vm_alloc(vm, cap * sizeof *slot);
        for (uint32_t i = 0; i < t->cap; i++) {
            ct_str_t *s = t->slot[i];
            if (!s) continue;
            uint32_t h = 2166136261u;
            for (uint32_t k = 0; k < s->len; k++) h = (h ^ (uint8_t)s->p[k]) * 16777619u;
            uint32_t j = h & (cap - 1);
            while (slot[j]) j = (j + 1) & (cap - 1);
            slot[j] = s;
        }
        t->slot = slot;
        t->cap = cap;
    }
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < n; k++) h = (h ^ (uint8_t)p[k]) * 16777619u;
    uint32_t j = h & (t->cap - 1);
    while (t->slot[j]) {
        ct_str_t *s = t->slot[j];
        if (s->len == n && !memcmp(s->p, p, n)) return s;
        j = (j + 1) & (t->cap - 1);
    }
    ct_str_t *s = ct_str_new(vm->ar, p, n);
    if (!s) oom(vm);
    t->slot[j] = s;
    t->n++;
    return s;
}

/* ---- the lexer ------------------------------------------------------------------------------ */

enum { TK_EOF, TK_NUM, TK_STR, TK_TPL, TK_ID, TK_P };

typedef struct {
    int         t;
    const char *s;
    int         n;
    double      num;
    ct_str_t   *str;
    int         line;
    bool        nl;
} tok_t;

typedef struct {
    ct_vm_t    *vm;
    const char *p, *end;
    int         line;
    tok_t       tk;
    bool        operand;    /* the last token ends an operand: a '/' is a division */
    bool        no_in;      /* inside "for (x in": the 'in' is not an operator */
} ps_t;

static void syntax(ps_t *ps, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void syntax(ps_t *ps, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ps->vm->line = ps->tk.line ? ps->tk.line : ps->line;
    ct_throw_err(ps->vm, "SyntaxError", "%s", msg);
}

static bool id_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}

static bool id_char(unsigned char c)
{
    return id_start(c) || (c >= '0' && c <= '9');
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* the text between p and end with its escapes undone (strings and the
 * cooked parts of a template) */
static ct_str_t *unescape(ps_t *ps, const char *p, const char *end)
{
    ct_str_t *s = vm_alloc(ps->vm, sizeof(ct_str_t) + (size_t)(end - p) + 1);
    char *o = s->p;
    while (p < end) {
        if (*p != '\\') {
            if (*p == '\n') ps->line++;
            *o++ = *p++;
            continue;
        }
        p++;
        if (p >= end) break;
        char c = *p++;
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'v': *o++ = '\v'; break;
        case '0': *o++ = '\0'; break;
        case '\r': if (p < end && *p == '\n') p++; break;
        case '\n': ps->line++; break;
        case 'x': {
            int a = p + 1 < end ? hexv(p[0]) : -1, b = p + 1 < end ? hexv(p[1]) : -1;
            if (a < 0 || b < 0) syntax(ps, "Invalid hexadecimal escape sequence");
            o += put_utf8(o, (uint32_t)(a * 16 + b));
            p += 2;
            break;
        }
        case 'u': {
            uint32_t cp = 0;
            if (p < end && *p == '{') {
                p++;
                while (p < end && *p != '}') {
                    int h = hexv(*p++);
                    if (h < 0) syntax(ps, "Invalid Unicode escape sequence");
                    cp = cp * 16 + (uint32_t)h;
                }
                p++;
            } else {
                for (int i = 0; i < 4; i++) {
                    int h = p < end ? hexv(*p++) : -1;
                    if (h < 0) syntax(ps, "Invalid Unicode escape sequence");
                    cp = cp * 16 + (uint32_t)h;
                }
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int i = 0; i < 4; i++) {
                        int h = hexv(p[2 + i]);
                        if (h < 0) ok = false;
                        lo = lo * 16 + (uint32_t)(h < 0 ? 0 : h);
                    }
                    if (ok && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
            }
            o += put_utf8(o, cp);
            break;
        }
        default: *o++ = c;
        }
    }
    *o = '\0';
    s->len = (uint32_t)(o - s->p);
    return s;
}

static const char *const PUNCT[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
    "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
    "^=", "**", "<<", ">>",
};

static void next(ps_t *ps)
{
    tok_t *t = &ps->tk;
    bool nl = false;
    for (;;) {
        while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r' || *ps->p == '\n' ||
                                   (unsigned char)*ps->p == 0xC2 || (unsigned char)*ps->p == 0xA0)) {
            if (*ps->p == '\n') {
                ps->line++;
                nl = true;
            }
            ps->p++;
        }
        if (ps->p + 1 < ps->end && ps->p[0] == '/' && ps->p[1] == '/') {
            while (ps->p < ps->end && *ps->p != '\n') ps->p++;
            continue;
        }
        if (ps->p + 1 < ps->end && ps->p[0] == '/' && ps->p[1] == '*') {
            ps->p += 2;
            while (ps->p + 1 < ps->end && !(ps->p[0] == '*' && ps->p[1] == '/')) {
                if (*ps->p == '\n') {
                    ps->line++;
                    nl = true;
                }
                ps->p++;
            }
            ps->p += 2;
            continue;
        }
        break;
    }
    memset(t, 0, sizeof *t);
    t->nl = nl;
    t->line = ps->line;
    t->s = ps->p;
    if (ps->p >= ps->end) {
        t->t = TK_EOF;
        return;
    }
    unsigned char c = (unsigned char)*ps->p;
    if (id_start(c)) {
        const char *q = ps->p;
        while (q < ps->end && id_char((unsigned char)*q)) q++;
        t->t = TK_ID;
        t->n = (int)(q - ps->p);
        t->str = ct_intern(ps->vm, ps->p, (size_t)t->n);
        ps->p = q;
        static const char *const NOT_OPERAND[] = { "return", "typeof", "case", "do", "else", "in", "of", "new",
                                                   "delete", "void", "throw", "instanceof", "yield", "await" };
        ps->operand = true;
        for (size_t i = 0; i < sizeof NOT_OPERAND / sizeof NOT_OPERAND[0]; i++) {
            if (strlen(NOT_OPERAND[i]) == (size_t)t->n && !memcmp(NOT_OPERAND[i], t->s, (size_t)t->n)) {
                ps->operand = false;
            }
        }
        return;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && ps->p + 1 < ps->end && ps->p[1] >= '0' && ps->p[1] <= '9')) {
        const char *q = ps->p;
        double v = 0;
        if (c == '0' && q + 1 < ps->end && (q[1] == 'x' || q[1] == 'X' || q[1] == 'b' || q[1] == 'B' || q[1] == 'o' ||
                                            q[1] == 'O')) {
            int base = (q[1] == 'x' || q[1] == 'X') ? 16 : (q[1] == 'b' || q[1] == 'B') ? 2 : 8;
            q += 2;
            while (q < ps->end) {
                int h = hexv(*q);
                if (*q == '_') { q++; continue; }
                if (h < 0 || h >= base) break;
                v = v * base + h;
                q++;
            }
        } else {
            char tmp[64];
            int k = 0;
            while (q < ps->end && k < 62 && ((*q >= '0' && *q <= '9') || *q == '.' || *q == '_' ||
                                             ((*q == 'e' || *q == 'E')) ||
                                             ((*q == '+' || *q == '-') && (q[-1] == 'e' || q[-1] == 'E')))) {
                if (*q != '_') tmp[k++] = *q;
                q++;
            }
            tmp[k] = '\0';
            v = strtod(tmp, NULL);
        }
        if (q < ps->end && *q == 'n') syntax(ps, "BigInt is not supported on the board");
        if (q < ps->end && id_start((unsigned char)*q)) syntax(ps, "Invalid or unexpected token");
        t->t = TK_NUM;
        t->num = v;
        t->n = (int)(q - ps->p);
        ps->p = q;
        ps->operand = true;
        return;
    }
    if (c == '"' || c == '\'') {
        const char *q = ps->p + 1;
        while (q < ps->end && *q != (char)c) {
            if (*q == '\\') q++;
            else if (*q == '\n') syntax(ps, "Invalid or unexpected token (an unterminated string)");
            q++;
        }
        if (q >= ps->end) syntax(ps, "Invalid or unexpected token (an unterminated string)");
        t->t = TK_STR;
        t->str = unescape(ps, ps->p + 1, q);
        t->n = (int)(q + 1 - ps->p);
        ps->p = q + 1;
        ps->operand = true;
        return;
    }
    if (c == '`') {
        const char *q = ps->p + 1;
        int startline = ps->line;
        while (q < ps->end && *q != '`') {
            if (*q == '\\') {
                q += 2;
                continue;
            }
            if (*q == '$' && q + 1 < ps->end && q[1] == '{') {
                /* skip the expression, its strings and its braces */
                int depth = 0;
                q += 2;
                while (q < ps->end) {
                    if (*q == '{') depth++;
                    else if (*q == '}') {
                        if (!depth) break;
                        depth--;
                    } else if (*q == '"' || *q == '\'' || *q == '`') {
                        char qc = *q++;
                        while (q < ps->end && *q != qc) {
                            if (*q == '\\') q++;
                            q++;
                        }
                    } else if (*q == '\n') {
                        ps->line++;
                    }
                    q++;
                }
                if (q < ps->end) q++;
                continue;
            }
            if (*q == '\n') ps->line++;
            q++;
        }
        if (q >= ps->end) syntax(ps, "Unterminated template literal");
        t->t = TK_TPL;
        t->line = startline;
        t->n = (int)(q + 1 - ps->p);
        ps->p = q + 1;
        ps->operand = true;
        return;
    }
    for (size_t i = 0; i < sizeof PUNCT / sizeof PUNCT[0]; i++) {
        size_t k = strlen(PUNCT[i]);
        if ((size_t)(ps->end - ps->p) >= k && !memcmp(ps->p, PUNCT[i], k)) {
            if (!strcmp(PUNCT[i], "?.") && ps->p + 2 < ps->end && ps->p[2] >= '0' && ps->p[2] <= '9') continue;
            t->t = TK_P;
            t->n = (int)k;
            ps->p += k;
            ps->operand = !strcmp(PUNCT[i], "++") || !strcmp(PUNCT[i], "--");
            return;
        }
    }
    if (c == '/' && !ps->operand) syntax(ps, "regular expressions are not supported on the board");
    if (strchr("{}()[];,<>+-*/%&|^!~?:=.@#", (char)c)) {
        t->t = TK_P;
        t->n = 1;
        ps->p++;
        ps->operand = c == ')' || c == ']' || c == '}';
        return;
    }
    syntax(ps, "Invalid or unexpected token");
}

static bool is(ps_t *ps, const char *p)
{
    size_t k = strlen(p);
    return ps->tk.t == TK_P && (size_t)ps->tk.n == k && !memcmp(ps->tk.s, p, k);
}

static bool kw(ps_t *ps, const char *w)
{
    size_t k = strlen(w);
    return ps->tk.t == TK_ID && (size_t)ps->tk.n == k && !memcmp(ps->tk.s, w, k);
}

static bool eat(ps_t *ps, const char *p)
{
    if (!is(ps, p)) return false;
    next(ps);
    return true;
}

static void expect(ps_t *ps, const char *p)
{
    if (!eat(ps, p)) {
        if (ps->tk.t == TK_EOF) syntax(ps, "Unexpected end of input (expected '%s')", p);
        syntax(ps, "Unexpected token '%.*s' (expected '%s')", ps->tk.n > 20 ? 20 : ps->tk.n, ps->tk.s, p);
    }
}

static void unexpected(ps_t *ps) __attribute__((noreturn));
static void unexpected(ps_t *ps)
{
    if (ps->tk.t == TK_EOF) syntax(ps, "Unexpected end of input");
    syntax(ps, "Unexpected token '%.*s'", ps->tk.n > 20 ? 20 : ps->tk.n, ps->tk.s);
}

static const char *const RESERVED[] = {
    "var", "let", "const", "function", "return", "if", "else", "for", "while", "do", "break", "continue", "new",
    "delete", "typeof", "instanceof", "in", "try", "catch", "finally", "throw", "switch", "case", "default", "void",
    "class", "this", "true", "false", "null", "import", "export", "extends", "super", "yield", "async", "await",
    "with", "debugger", "enum",
};

static bool reserved(const tok_t *t)
{
    for (size_t i = 0; i < sizeof RESERVED / sizeof RESERVED[0]; i++) {
        if (strlen(RESERVED[i]) == (size_t)t->n && !memcmp(RESERVED[i], t->s, (size_t)t->n)) return true;
    }
    return false;
}

/* ---- the parser ----------------------------------------------------------------------------- */

static node_t *mk(ps_t *ps, int k)
{
    node_t *n = vm_alloc(ps->vm, sizeof *n);
    n->k = (uint8_t)k;
    n->line = ps->tk.line;
    return n;
}

typedef struct {
    node_t **v;
    int      n, cap;
} nlist_t;

static void nl_add(ps_t *ps, nlist_t *l, node_t *x)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 4;
        node_t **v = vm_alloc(ps->vm, (size_t)cap * sizeof *v);
        if (l->n) memcpy(v, l->v, (size_t)l->n * sizeof *v);
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = x;
}

static node_t *expr(ps_t *ps);
static node_t *assign(ps_t *ps);
static node_t *statement(ps_t *ps);
static node_t *block(ps_t *ps);
static node_t *func_rest(ps_t *ps, node_t *fn);

static void enter(ps_t *ps)
{
    if (++ps->vm->pdepth > MAX_PDEPTH) syntax(ps, "the script is nested too deeply");
}

static void leave(ps_t *ps)
{
    ps->vm->pdepth--;
}

static ct_str_t *ident(ps_t *ps)
{
    if (ps->tk.t != TK_ID || reserved(&ps->tk)) unexpected(ps);
    ct_str_t *s = ps->tk.str;
    next(ps);
    return s;
}

/* a property name after '.', or a key in a literal: keywords are fine there */
static ct_str_t *prop_name(ps_t *ps)
{
    ct_str_t *s;
    if (ps->tk.t == TK_ID || ps->tk.t == TK_STR) {
        s = ps->tk.str;
    } else if (ps->tk.t == TK_NUM) {
        char b[40];
        ct_num_fmt(ps->tk.num, b);
        s = ct_intern(ps->vm, b, strlen(b));
    } else {
        unexpected(ps);
    }
    next(ps);
    return s;
}

static node_t *template_lit(ps_t *ps)
{
    node_t *n = mk(ps, N_TEMPLATE);
    nlist_t parts = { 0 };
    const char *p = ps->tk.s + 1, *end = ps->tk.s + ps->tk.n - 1;
    int line = ps->tk.line;
    const char *run = p;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '$' && p + 1 < end && p[1] == '{') {
            node_t *s = mk(ps, N_STR);
            ps_t sub = { .vm = ps->vm, .line = line };
            s->s = unescape(&sub, run, p);
            nl_add(ps, &parts, s);
            const char *q = p + 2;
            int depth = 0;
            while (q < end) {
                if (*q == '{') depth++;
                else if (*q == '}') {
                    if (!depth) break;
                    depth--;
                } else if (*q == '"' || *q == '\'' || *q == '`') {
                    char qc = *q++;
                    while (q < end && *q != qc) {
                        if (*q == '\\') q++;
                        q++;
                    }
                }
                q++;
            }
            ps_t in = { .vm = ps->vm, .p = p + 2, .end = q, .line = line };
            next(&in);
            nl_add(ps, &parts, expr(&in));
            if (in.tk.t != TK_EOF) unexpected(&in);
            for (const char *c = run; c < q; c++) if (*c == '\n') line++;
            p = q + 1;
            run = p;
            continue;
        }
        p++;
    }
    node_t *s = mk(ps, N_STR);
    ps_t sub = { .vm = ps->vm, .line = line };
    s->s = unescape(&sub, run, end);
    nl_add(ps, &parts, s);
    n->list = parts.v;
    n->n = parts.n;
    next(ps);
    return n;
}

/* a binding: a name, or { a, b: c, d = 1 } / [x, y] in a declaration or a
 * parameter list */
static node_t *pattern(ps_t *ps)
{
    if (is(ps, "{") || is(ps, "[")) {
        bool obj = is(ps, "{");
        node_t *p = mk(ps, obj ? N_OBJPAT : N_ARRPAT);
        nlist_t l = { 0 };
        next(ps);
        while (!is(ps, obj ? "}" : "]")) {
            node_t *pp = mk(ps, N_PATPROP);
            if (!obj && is(ps, ",")) {
                next(ps);
                nl_add(ps, &l, NULL);
                continue;
            }
            if (eat(ps, "...")) {
                pp->flag |= NF_REST;
                pp->a = pattern(ps);
            } else if (obj) {
                pp->s = prop_name(ps);
                if (eat(ps, ":")) {
                    pp->a = pattern(ps);
                } else {
                    node_t *id = mk(ps, N_IDENT);
                    id->s = pp->s;
                    pp->a = id;
                }
            } else {
                pp->a = pattern(ps);
            }
            if (eat(ps, "=")) pp->b = assign(ps);
            nl_add(ps, &l, pp);
            if (!eat(ps, ",")) break;
        }
        expect(ps, obj ? "}" : "]");
        p->list = l.v;
        p->n = l.n;
        return p;
    }
    node_t *id = mk(ps, N_IDENT);
    id->s = ident(ps);
    return id;
}

static node_t *params_and_body(ps_t *ps, node_t *fn)
{
    expect(ps, "(");
    nlist_t l = { 0 };
    while (!is(ps, ")")) {
        node_t *pp = mk(ps, N_PATPROP);
        if (eat(ps, "...")) pp->flag |= NF_REST;
        pp->a = pattern(ps);
        if (eat(ps, "=")) pp->b = assign(ps);
        nl_add(ps, &l, pp);
        if (!eat(ps, ",")) break;
    }
    expect(ps, ")");
    fn->list = l.v;
    fn->n = l.n;
    return func_rest(ps, fn);
}

static node_t *func_rest(ps_t *ps, node_t *fn)
{
    ps->vm->nfuncs++;
    if (!is(ps, "{")) unexpected(ps);
    fn->a = block(ps);
    return fn;
}

/* (a, b) => ...: looking past the parenthesis without consuming it */
static bool arrow_ahead(ps_t *ps)
{
    ps_t save = *ps;
    int depth = 0;
    for (;;) {
        if (ps->tk.t == TK_EOF) break;
        if (is(ps, "(") || is(ps, "[") || is(ps, "{")) depth++;
        else if (is(ps, ")") || is(ps, "]") || is(ps, "}")) {
            depth--;
            if (depth == 0) {
                next(ps);
                bool r = is(ps, "=>") && !ps->tk.nl;
                *ps = save;
                return r;
            }
        }
        next(ps);
    }
    *ps = save;
    return false;
}

static node_t *arrow(ps_t *ps, bool single)
{
    node_t *fn = mk(ps, N_FUNC);
    fn->flag |= NF_ARROW;
    ps->vm->nfuncs++;
    if (single) {
        node_t *pp = mk(ps, N_PATPROP);
        node_t *id = mk(ps, N_IDENT);
        id->s = ident(ps);
        pp->a = id;
        fn->list = vm_alloc(ps->vm, sizeof(node_t *));
        fn->list[0] = pp;
        fn->n = 1;
    } else {
        expect(ps, "(");
        nlist_t l = { 0 };
        while (!is(ps, ")")) {
            node_t *pp = mk(ps, N_PATPROP);
            if (eat(ps, "...")) pp->flag |= NF_REST;
            pp->a = pattern(ps);
            if (eat(ps, "=")) pp->b = assign(ps);
            nl_add(ps, &l, pp);
            if (!eat(ps, ",")) break;
        }
        expect(ps, ")");
        fn->list = l.v;
        fn->n = l.n;
    }
    expect(ps, "=>");
    if (is(ps, "{")) {
        fn->a = block(ps);
    } else {
        fn->flag |= NF_EXPRBODY;
        fn->a = assign(ps);
    }
    return fn;
}

static node_t *args(ps_t *ps, node_t *call)
{
    expect(ps, "(");
    nlist_t l = { 0 };
    while (!is(ps, ")")) {
        if (eat(ps, "...")) {
            node_t *sp = mk(ps, N_SPREAD);
            sp->a = assign(ps);
            nl_add(ps, &l, sp);
        } else {
            nl_add(ps, &l, assign(ps));
        }
        if (!eat(ps, ",")) break;
    }
    expect(ps, ")");
    call->list = l.v;
    call->n = l.n;
    return call;
}

static node_t *primary(ps_t *ps)
{
    tok_t *t = &ps->tk;
    node_t *n;
    switch (t->t) {
    case TK_NUM:
        n = mk(ps, N_NUM);
        n->num = t->num;
        next(ps);
        return n;
    case TK_STR:
        n = mk(ps, N_STR);
        n->s = t->str;
        next(ps);
        return n;
    case TK_TPL:
        return template_lit(ps);
    case TK_ID:
        if (kw(ps, "function")) {
            next(ps);
            n = mk(ps, N_FUNC);
            if (ps->tk.t == TK_ID && !is(ps, "(")) n->s = ident(ps);
            return params_and_body(ps, n);
        }
        if (kw(ps, "this")) {
            next(ps);
            return mk(ps, N_THIS);
        }
        if (kw(ps, "true") || kw(ps, "false")) {
            n = mk(ps, N_NUM);
            n->op = 1;          /* a boolean literal */
            n->num = kw(ps, "true");
            next(ps);
            return n;
        }
        if (kw(ps, "null")) {
            n = mk(ps, N_NUM);
            n->op = 2;          /* null */
            next(ps);
            return n;
        }
        if (kw(ps, "new")) {
            next(ps);
            n = mk(ps, N_NEW);
            /* new X.Y(...) : the member expression, then the arguments */
            node_t *callee = primary(ps);
            while (is(ps, ".")) {
                next(ps);
                node_t *m = mk(ps, N_MEMBER);
                m->a = callee;
                m->s = prop_name(ps);
                callee = m;
            }
            n->a = callee;
            if (is(ps, "(")) args(ps, n);
            return n;
        }
        if (kw(ps, "class")) syntax(ps, "classes are not supported on the board");
        if (kw(ps, "async") || kw(ps, "await")) syntax(ps, "async and await are not supported on the board");
        /* require('lodash') throws when it runs; import cannot even parse */
        if (kw(ps, "import")) syntax(ps, "modules are not supported on the board");
        if (reserved(t)) unexpected(ps);
        {
            ps_t look = *ps;
            next(&look);
            if (look.tk.t == TK_P && look.tk.n == 2 && !memcmp(look.tk.s, "=>", 2) && !look.tk.nl) {
                return arrow(ps, true);
            }
        }
        n = mk(ps, N_IDENT);
        n->s = t->str;
        next(ps);
        return n;
    case TK_P:
        if (is(ps, "(")) {
            if (arrow_ahead(ps)) return arrow(ps, false);
            next(ps);
            n = expr(ps);
            expect(ps, ")");
            return n;
        }
        if (is(ps, "[")) {
            n = mk(ps, N_ARRAY);
            next(ps);
            nlist_t l = { 0 };
            while (!is(ps, "]")) {
                if (is(ps, ",")) {
                    next(ps);
                    nl_add(ps, &l, NULL);
                    continue;
                }
                if (eat(ps, "...")) {
                    node_t *sp = mk(ps, N_SPREAD);
                    sp->a = assign(ps);
                    nl_add(ps, &l, sp);
                } else {
                    nl_add(ps, &l, assign(ps));
                }
                if (!eat(ps, ",")) break;
            }
            expect(ps, "]");
            n->list = l.v;
            n->n = l.n;
            return n;
        }
        if (is(ps, "{")) {
            n = mk(ps, N_OBJECT);
            next(ps);
            nlist_t l = { 0 };
            while (!is(ps, "}")) {
                node_t *p = mk(ps, N_PROP);
                if (eat(ps, "...")) {
                    p->k = N_SPREAD;
                    p->a = assign(ps);
                } else if (eat(ps, "[")) {
                    p->flag |= NF_COMPUTED;
                    p->b = assign(ps);
                    expect(ps, "]");
                    expect(ps, ":");
                    p->a = assign(ps);
                } else {
                    bool was_id = ps->tk.t == TK_ID;
                    if ((kw(ps, "get") || kw(ps, "set"))) {
                        ps_t look = *ps;
                        next(&look);
                        if (look.tk.t == TK_ID || look.tk.t == TK_STR) {
                            syntax(ps, "getters and setters are not supported on the board");
                        }
                    }
                    p->s = prop_name(ps);
                    if (eat(ps, ":")) {
                        p->a = assign(ps);
                    } else if (is(ps, "(")) {
                        node_t *fn = mk(ps, N_FUNC);
                        fn->s = p->s;
                        p->a = params_and_body(ps, fn);
                    } else if (was_id) {
                        node_t *id = mk(ps, N_IDENT);
                        id->s = p->s;
                        p->a = id;
                    } else {
                        unexpected(ps);
                    }
                }
                nl_add(ps, &l, p);
                if (!eat(ps, ",")) break;
            }
            expect(ps, "}");
            n->list = l.v;
            n->n = l.n;
            return n;
        }
        unexpected(ps);
    default:
        unexpected(ps);
    }
}

static node_t *call_member(ps_t *ps)
{
    node_t *e = primary(ps);
    bool opt = false;
    for (;;) {
        if (is(ps, ".")) {
            next(ps);
            node_t *m = mk(ps, N_MEMBER);
            m->a = e;
            m->s = prop_name(ps);
            e = m;
        } else if (is(ps, "?.")) {
            next(ps);
            opt = true;
            node_t *m;
            if (is(ps, "(")) {
                m = mk(ps, N_CALL);
                m->a = e;
                m->flag |= NF_OPTIONAL;
                args(ps, m);
            } else if (eat(ps, "[")) {
                m = mk(ps, N_INDEX);
                m->a = e;
                m->b = expr(ps);
                m->flag |= NF_OPTIONAL;
                expect(ps, "]");
            } else {
                m = mk(ps, N_MEMBER);
                m->a = e;
                m->s = prop_name(ps);
                m->flag |= NF_OPTIONAL;
            }
            e = m;
        } else if (is(ps, "[")) {
            next(ps);
            node_t *m = mk(ps, N_INDEX);
            m->a = e;
            m->b = expr(ps);
            expect(ps, "]");
            e = m;
        } else if (is(ps, "(")) {
            node_t *c = mk(ps, N_CALL);
            c->a = e;
            args(ps, c);
            e = c;
        } else if (ps->tk.t == TK_TPL && !ps->tk.nl) {
            syntax(ps, "tagged templates are not supported on the board");
        } else {
            break;
        }
    }
    if (opt) e->flag |= NF_SHORT;
    return e;
}

static node_t *unary(ps_t *ps)
{
    static const struct { const char *t; int op; bool word; } U[] = {
        { "!", OP_NOT, false }, { "-", OP_NEG, false }, { "+", OP_POS, false }, { "~", OP_BNOT, false },
        { "typeof", OP_TYPEOF, true }, { "void", OP_VOID, true }, { "delete", OP_DELETE, true },
    };
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++) {
        if (U[i].word ? kw(ps, U[i].t) : is(ps, U[i].t)) {
            node_t *n = mk(ps, N_UNARY);
            n->op = (uint8_t)U[i].op;
            next(ps);
            enter(ps);
            n->a = unary(ps);
            leave(ps);
            return n;
        }
    }
    if (is(ps, "++") || is(ps, "--")) {
        node_t *n = mk(ps, N_UPDATE);
        n->op = is(ps, "++") ? OP_INC : OP_DEC;
        n->flag |= NF_PREFIX;
        next(ps);
        n->a = unary(ps);
        if (n->a->k != N_IDENT && n->a->k != N_MEMBER && n->a->k != N_INDEX) {
            syntax(ps, "Invalid left-hand side expression in prefix operation");
        }
        return n;
    }
    node_t *e = call_member(ps);
    if ((is(ps, "++") || is(ps, "--")) && !ps->tk.nl) {
        node_t *n = mk(ps, N_UPDATE);
        n->op = is(ps, "++") ? OP_INC : OP_DEC;
        n->a = e;
        if (e->k != N_IDENT && e->k != N_MEMBER && e->k != N_INDEX) {
            syntax(ps, "Invalid left-hand side expression in postfix operation");
        }
        next(ps);
        return n;
    }
    return e;
}

typedef struct {
    const char *t;
    int         op, prec;
    bool        word;
} binop_t;

static const binop_t BIN[] = {
    { "??", OP_NULLISH, 1, false }, { "||", OP_OR, 2, false }, { "&&", OP_AND, 3, false },
    { "|", OP_BOR, 4, false }, { "^", OP_BXOR, 5, false }, { "&", OP_BAND, 6, false },
    { "===", OP_SEQ, 7, false }, { "!==", OP_SNE, 7, false }, { "==", OP_EQ, 7, false }, { "!=", OP_NE, 7, false },
    { "<=", OP_LE, 8, false }, { ">=", OP_GE, 8, false }, { "<", OP_LT, 8, false }, { ">", OP_GT, 8, false },
    { "instanceof", OP_INSTANCEOF, 8, true }, { "in", OP_IN, 8, true },
    { ">>>", OP_USHR, 9, false }, { "<<", OP_SHL, 9, false }, { ">>", OP_SHR, 9, false },
    { "+", OP_ADD, 10, false }, { "-", OP_SUB, 10, false },
    { "*", OP_MUL, 11, false }, { "/", OP_DIV, 11, false }, { "%", OP_MOD, 11, false },
    { "**", OP_EXP, 12, false },
};

static const binop_t *binop(ps_t *ps, bool no_in)
{
    for (size_t i = 0; i < sizeof BIN / sizeof BIN[0]; i++) {
        if (BIN[i].word ? kw(ps, BIN[i].t) : is(ps, BIN[i].t)) {
            if (no_in && BIN[i].op == OP_IN) return NULL;
            return &BIN[i];
        }
    }
    return NULL;
}

static node_t *binary(ps_t *ps, int minprec)
{
    enter(ps);
    node_t *l = unary(ps);
    /* a left-leaning chain is walked recursively: keep it within the
     * thread's stack */
    for (int terms = 0;; terms++) {
        const binop_t *b = binop(ps, ps->no_in);
        if (!b || b->prec < minprec) break;
        if (terms > 200) syntax(ps, "the expression is too long for the board (split it)");
        next(ps);
        /* ** is right-associative */
        node_t *r = binary(ps, b->op == OP_EXP ? b->prec : b->prec + 1);
        node_t *n = mk(ps, (b->op == OP_AND || b->op == OP_OR || b->op == OP_NULLISH) ? N_LOGICAL : N_BINARY);
        n->op = (uint8_t)b->op;
        n->a = l;
        n->b = r;
        l = n;
    }
    leave(ps);
    return l;
}

static node_t *cond(ps_t *ps)
{
    node_t *c = binary(ps, 1);
    if (!is(ps, "?")) return c;
    next(ps);
    node_t *n = mk(ps, N_COND);
    n->a = c;
    bool save = ps->no_in;
    ps->no_in = false;
    n->b = assign(ps);
    ps->no_in = save;
    expect(ps, ":");
    n->c = assign(ps);
    return n;
}

static node_t *assign(ps_t *ps)
{
    enter(ps);
    node_t *l = cond(ps);
    static const struct { const char *t; int op; } A[] = {
        { "=", OP_SET }, { "+=", OP_ADD }, { "-=", OP_SUB }, { "*=", OP_MUL }, { "/=", OP_DIV }, { "%=", OP_MOD },
        { "**=", OP_EXP }, { "<<=", OP_SHL }, { ">>=", OP_SHR }, { ">>>=", OP_USHR }, { "&=", OP_BAND },
        { "|=", OP_BOR }, { "^=", OP_BXOR }, { "&&=", OP_AND }, { "||=", OP_OR }, { "?\?=", OP_NULLISH },
    };
    for (size_t i = 0; i < sizeof A / sizeof A[0]; i++) {
        if (!is(ps, A[i].t)) continue;
        if (l->k != N_IDENT && l->k != N_MEMBER && l->k != N_INDEX) {
            if (A[i].op == OP_SET && (l->k == N_OBJECT || l->k == N_ARRAY)) {
                syntax(ps, "destructuring assignment is not supported on the board (only in declarations)");
            }
            syntax(ps, "Invalid left-hand side in assignment");
        }
        next(ps);
        node_t *n = mk(ps, N_ASSIGN);
        n->op = (uint8_t)A[i].op;
        n->a = l;
        n->b = assign(ps);
        leave(ps);
        return n;
    }
    leave(ps);
    return l;
}

static node_t *expr(ps_t *ps)
{
    node_t *e = assign(ps);
    if (!is(ps, ",")) return e;
    node_t *s = mk(ps, N_SEQ);
    nlist_t l = { 0 };
    nl_add(ps, &l, e);
    while (eat(ps, ",")) nl_add(ps, &l, assign(ps));
    s->list = l.v;
    s->n = l.n;
    return s;
}

/* the end of a statement: a semicolon, or one the rules insert (a line
 * break, a closing brace, the end) */
static void semi(ps_t *ps)
{
    if (eat(ps, ";")) return;
    if (is(ps, "}") || ps->tk.t == TK_EOF || ps->tk.nl) return;
    unexpected(ps);
}

static node_t *var_decl(ps_t *ps, bool need_semi)
{
    node_t *n = mk(ps, N_VAR);
    if (kw(ps, "const")) n->flag |= NF_CONST;
    next(ps);
    nlist_t l = { 0 };
    do {
        node_t *d = mk(ps, N_DECL);
        d->a = pattern(ps);
        if (eat(ps, "=")) d->b = assign(ps);
        nl_add(ps, &l, d);
    } while (eat(ps, ","));
    n->list = l.v;
    n->n = l.n;
    if (need_semi) semi(ps);
    return n;
}

static node_t *block(ps_t *ps)
{
    node_t *b = mk(ps, N_BLOCK);
    expect(ps, "{");
    nlist_t l = { 0 };
    while (!is(ps, "}")) {
        if (ps->tk.t == TK_EOF) unexpected(ps);
        node_t *st = statement(ps);
        if (st->k == N_VAR || st->k == N_FUNCDECL) b->flag |= NF_DECLS;
        nl_add(ps, &l, st);
    }
    next(ps);
    b->list = l.v;
    b->n = l.n;
    return b;
}

static node_t *statement(ps_t *ps)
{
    enter(ps);
    node_t *n;
    if (is(ps, "{")) {
        n = block(ps);
    } else if (is(ps, ";")) {
        n = mk(ps, N_EMPTY);
        next(ps);
    } else if (kw(ps, "var") || kw(ps, "let") || kw(ps, "const")) {
        n = var_decl(ps, true);
    } else if (kw(ps, "function")) {
        next(ps);
        n = mk(ps, N_FUNCDECL);
        node_t *fn = mk(ps, N_FUNC);
        fn->s = ident(ps);
        n->s = fn->s;
        n->a = params_and_body(ps, fn);
    } else if (kw(ps, "if")) {
        n = mk(ps, N_IF);
        next(ps);
        expect(ps, "(");
        n->a = expr(ps);
        expect(ps, ")");
        n->b = statement(ps);
        if (kw(ps, "else")) {
            next(ps);
            n->c = statement(ps);
        }
    } else if (kw(ps, "for")) {
        int funcs = ps->vm->nfuncs;
        next(ps);
        expect(ps, "(");
        node_t *init = NULL;
        if (!is(ps, ";")) {
            ps->no_in = true;
            if (kw(ps, "var") || kw(ps, "let") || kw(ps, "const")) init = var_decl(ps, false);
            else init = expr(ps);
            ps->no_in = false;
        }
        if (kw(ps, "of") || kw(ps, "in")) {
            bool of = kw(ps, "of");
            n = mk(ps, of ? N_FOROF : N_FORIN);
            next(ps);
            n->a = init;
            n->b = of ? assign(ps) : expr(ps);
            expect(ps, ")");
            n->c = statement(ps);
        } else {
            n = mk(ps, N_FOR);
            expect(ps, ";");
            n->a = init;
            if (!is(ps, ";")) n->b = expr(ps);
            expect(ps, ";");
            if (!is(ps, ")")) n->c = expr(ps);
            expect(ps, ")");
            n->d = statement(ps);
        }
        if (ps->vm->nfuncs != funcs) n->flag |= NF_HASFN;
    } else if (kw(ps, "while")) {
        n = mk(ps, N_WHILE);
        next(ps);
        expect(ps, "(");
        n->a = expr(ps);
        expect(ps, ")");
        n->b = statement(ps);
    } else if (kw(ps, "do")) {
        n = mk(ps, N_DO);
        next(ps);
        n->b = statement(ps);
        if (!kw(ps, "while")) unexpected(ps);
        next(ps);
        expect(ps, "(");
        n->a = expr(ps);
        expect(ps, ")");
        eat(ps, ";");
    } else if (kw(ps, "return")) {
        n = mk(ps, N_RETURN);
        next(ps);
        if (!is(ps, ";") && !is(ps, "}") && ps->tk.t != TK_EOF && !ps->tk.nl) n->a = expr(ps);
        semi(ps);
    } else if (kw(ps, "break") || kw(ps, "continue")) {
        n = mk(ps, kw(ps, "break") ? N_BREAK : N_CONTINUE);
        next(ps);
        if (ps->tk.t == TK_ID && !ps->tk.nl && !reserved(&ps->tk)) syntax(ps, "labels are not supported on the board");
        semi(ps);
    } else if (kw(ps, "throw")) {
        n = mk(ps, N_THROW);
        next(ps);
        n->a = expr(ps);
        semi(ps);
    } else if (kw(ps, "try")) {
        n = mk(ps, N_TRY);
        next(ps);
        n->a = block(ps);
        if (kw(ps, "catch")) {
            next(ps);
            if (eat(ps, "(")) {
                n->d = pattern(ps);
                expect(ps, ")");
            }
            n->b = block(ps);
        }
        if (kw(ps, "finally")) {
            next(ps);
            n->c = block(ps);
        }
        if (!n->b && !n->c) syntax(ps, "Missing catch or finally after try");
    } else if (kw(ps, "switch")) {
        n = mk(ps, N_SWITCH);
        next(ps);
        expect(ps, "(");
        n->a = expr(ps);
        expect(ps, ")");
        expect(ps, "{");
        nlist_t cases = { 0 };
        while (!is(ps, "}")) {
            node_t *c = mk(ps, N_CASE);
            if (kw(ps, "case")) {
                next(ps);
                c->a = expr(ps);
            } else if (kw(ps, "default")) {
                next(ps);
            } else {
                unexpected(ps);
            }
            expect(ps, ":");
            nlist_t body = { 0 };
            while (!kw(ps, "case") && !kw(ps, "default") && !is(ps, "}")) {
                if (ps->tk.t == TK_EOF) unexpected(ps);
                nl_add(ps, &body, statement(ps));
            }
            c->list = body.v;
            c->n = body.n;
            nl_add(ps, &cases, c);
        }
        next(ps);
        n->list = cases.v;
        n->n = cases.n;
    } else {
        n = mk(ps, N_EXPR);
        n->a = expr(ps);
        semi(ps);
    }
    leave(ps);
    return n;
}

/* ---- scopes ---------------------------------------------------------------------------------- */

static scope_t *scope_new(ct_vm_t *vm, scope_t *up)
{
    scope_t *s = vm_alloc(vm, sizeof *s);
    s->up = up;
    return s;
}

void ct_declare(ct_vm_t *vm, scope_t *s, ct_str_t *name, ctv_t v, bool konst)
{
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->v[i].name == name) {
            s->v[i].v = v;
            s->v[i].konst = konst;
            return;
        }
    }
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 4;
        var_t *nv = vm_alloc(vm, cap * sizeof *nv);
        if (s->n) memcpy(nv, s->v, s->n * sizeof *nv);
        s->v = nv;
        s->cap = cap;
    }
    s->v[s->n].name = name;
    s->v[s->n].v = v;
    s->v[s->n].konst = konst;
    s->n++;
}

static var_t *lookup(scope_t *s, ct_str_t *name)
{
    for (; s; s = s->up) {
        for (uint32_t i = 0; i < s->n; i++) if (s->v[i].name == name) return &s->v[i];
    }
    return NULL;
}

/* ---- values: equality, conversion, showing -------------------------------------------------- */

static bool nullish(ctv_t v)
{
    return v.t == CT_UNDEF || v.t == CT_NULL;
}

bool ct_strict_eq(ctv_t a, ctv_t b)
{
    if (a.t != b.t) return false;
    switch (a.t) {
    case CT_UNDEF: case CT_NULL: return true;
    case CT_BOOL: return a.b == b.b;
    case CT_NUM: return a.n == b.n;
    case CT_STR: return a.s == b.s || (a.s->len == b.s->len && !memcmp(a.s->p, b.s->p, a.s->len));
    case CT_ARR: return a.a == b.a;
    case CT_OBJ: return a.o == b.o;
    case CT_FN: return a.f == b.f;
    }
    return false;
}

bool ct_loose_eq(ct_vm_t *vm, ctv_t a, ctv_t b)
{
    if (a.t == b.t) return ct_strict_eq(a, b);
    if (nullish(a) && nullish(b)) return true;
    if (nullish(a) || nullish(b)) return false;
    if (a.t == CT_BOOL) return ct_loose_eq(vm, ct_num(a.b ? 1 : 0), b);
    if (b.t == CT_BOOL) return ct_loose_eq(vm, a, ct_num(b.b ? 1 : 0));
    if ((a.t == CT_NUM && b.t == CT_STR) || (a.t == CT_STR && b.t == CT_NUM)) return ct_to_num(a) == ct_to_num(b);
    if ((a.t == CT_ARR || a.t == CT_OBJ) && (b.t == CT_STR || b.t == CT_NUM)) {
        ctv_t s = ct_undef();
        s.t = CT_STR;
        s.s = vm_to_str(vm, a);
        return ct_loose_eq(vm, s, b);
    }
    if ((b.t == CT_ARR || b.t == CT_OBJ) && (a.t == CT_STR || a.t == CT_NUM)) return ct_loose_eq(vm, b, a);
    return false;
}

static void inspect_str(ct_sb_t *b, const ct_str_t *s, char q)
{
    ct_sb_addc(b, q);
    for (uint32_t i = 0; i < s->len && i < 400; i++) {
        char c = s->p[i];
        if (c == q || c == '\\') ct_sb_addc(b, '\\');
        if (c == '\n') ct_sb_adds(b, "\\n");
        else ct_sb_addc(b, c);
    }
    if (s->len > 400) ct_sb_adds(b, "...");
    ct_sb_addc(b, q);
}

/* chai shows values this way in its messages: 'text', [ 1, 2 ], { a: 1 } */
void ct_inspect(ct_sb_t *b, ctv_t v, int depth)
{
    char n[40];
    switch (v.t) {
    case CT_STR: inspect_str(b, v.s, '\''); break;
    case CT_NUM: ct_num_fmt(v.n, n); ct_sb_adds(b, n); break;
    case CT_ARR:
        if (depth > 2) {
            ct_sb_adds(b, "[Array]");
            break;
        }
        if (!v.a->n) {
            ct_sb_adds(b, "[]");
            break;
        }
        ct_sb_adds(b, "[ ");
        for (uint32_t i = 0; i < v.a->n; i++) {
            if (i >= 10) {
                ct_sb_addf(b, ", ... %u more", (unsigned)(v.a->n - 10));
                break;
            }
            if (i) ct_sb_adds(b, ", ");
            ct_inspect(b, v.a->v[i], depth + 1);
        }
        ct_sb_adds(b, " ]");
        break;
    case CT_OBJ:
        if (v.o->kind == CT_K_ERROR) {
            ctv_t *nm = ct_obj_find(v.o, "name", 4), *msg = ct_obj_find(v.o, "message", 7);
            ct_sb_adds(b, nm && nm->t == CT_STR ? nm->s->p : "Error");
            if (msg && msg->t == CT_STR && msg->s->len) {
                ct_sb_adds(b, ": ");
                ct_sb_add(b, msg->s->p, msg->s->len);
            }
            break;
        }
        if (v.o->kind == CT_K_DATE) {
            ctv_t *iso = ct_obj_find(v.o, "__iso", 5);
            ct_sb_adds(b, iso && iso->t == CT_STR ? iso->s->p : "Invalid Date");
            break;
        }
        if (depth > 2) {
            ct_sb_adds(b, "[Object]");
            break;
        }
        {
            int shown = 0;
            ct_sb_adds(b, "{");
            for (uint32_t i = 0; i < v.o->n; i++) {
                ct_str_t *k = v.o->p[i].k;
                if (v.o->kind != CT_K_PLAIN && k->len >= 2 && k->p[0] == '_' && k->p[1] == '_') continue;
                if (shown >= 10) {
                    ct_sb_adds(b, ", ...");
                    break;
                }
                ct_sb_adds(b, shown ? ", " : " ");
                bool plain = k->len > 0 && id_start((unsigned char)k->p[0]);
                for (uint32_t c = 0; c < k->len && plain; c++) if (!id_char((unsigned char)k->p[c])) plain = false;
                if (plain) ct_sb_add(b, k->p, k->len);
                else inspect_str(b, k, '\'');
                ct_sb_adds(b, ": ");
                ct_inspect(b, v.o->p[i].v, depth + 1);
                shown++;
            }
            ct_sb_adds(b, shown ? " }" : "}");
        }
        break;
    case CT_FN: ct_sb_adds(b, "[Function]"); break;
    case CT_UNDEF: ct_sb_adds(b, "undefined"); break;
    case CT_NULL: ct_sb_adds(b, "null"); break;
    case CT_BOOL: ct_sb_adds(b, v.b ? "true" : "false"); break;
    }
}

static ctv_t strv_of(ct_str_t *s)
{
    ctv_t v = ct_undef();
    v.t = CT_STR;
    v.s = s;
    return v;
}

/* ---- the built-in methods of strings, arrays and numbers ------------------------------------ */

static ct_str_t *self_str(ct_vm_t *vm, ctv_t self)
{
    return self.t == CT_STR ? self.s : vm_to_str(vm, self);
}

static double arg_num(int argc, ctv_t *argv, int i, double dflt)
{
    if (i >= argc || argv[i].t == CT_UNDEF) return dflt;
    return ct_to_num(argv[i]);
}

/* a relative index (negative from the end) clamped into 0..len */
static int32_t rel_index(double d, int32_t len)
{
    if (d != d) return 0;
    d = d_trunc(d);
    if (d < 0) {
        d += len;
        if (d < 0) d = 0;
    }
    if (d > len) d = len;
    return (int32_t)d;
}

static const char *find_sub(const char *h, size_t hn, const char *n, size_t nn, size_t from)
{
    if (nn == 0) return from <= hn ? h + from : NULL;
    for (size_t i = from; i + nn <= hn; i++) if (h[i] == n[0] && !memcmp(h + i, n, nn)) return h + i;
    return NULL;
}

enum {
    S_INCLUDES = 1, S_STARTS, S_ENDS, S_INDEXOF, S_LASTINDEXOF, S_SLICE, S_SUBSTRING, S_SUBSTR, S_SPLIT, S_REPLACE,
    S_REPLACEALL, S_TRIM, S_TRIMSTART, S_TRIMEND, S_UPPER, S_LOWER, S_PADSTART, S_PADEND, S_CHARAT, S_CHARCODEAT,
    S_AT, S_REPEAT, S_CONCAT, S_TOSTRING, S_LOCALECOMPARE, S_MATCH, S_NORMALIZE,
};

static ctv_t str_method(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    ct_str_t *s = self_str(vm, self);
    int m = (int)(intptr_t)ud;
    int32_t len = (int32_t)s->len;
    switch (m) {
    case S_INCLUDES: case S_STARTS: case S_ENDS: case S_INDEXOF: case S_LASTINDEXOF: {
        ct_str_t *n = vm_to_str(vm, arg(argc, argv, 0));
        if (m == S_STARTS) {
            int32_t at = rel_index(arg_num(argc, argv, 1, 0), len);
            return ct_bool((int32_t)n->len <= len - at && !memcmp(s->p + at, n->p, n->len));
        }
        if (m == S_ENDS) {
            int32_t end = rel_index(arg_num(argc, argv, 1, len), len);
            return ct_bool((int32_t)n->len <= end && !memcmp(s->p + end - n->len, n->p, n->len));
        }
        if (m == S_LASTINDEXOF) {
            for (int32_t i = len - (int32_t)n->len; i >= 0; i--) {
                if (!memcmp(s->p + i, n->p, n->len)) return ct_num(i);
            }
            return ct_num(-1);
        }
        int32_t from = rel_index(arg_num(argc, argv, 1, 0), len);
        const char *f = find_sub(s->p, s->len, n->p, n->len, (size_t)from);
        if (m == S_INCLUDES) return ct_bool(f != NULL);
        return ct_num(f ? (double)(f - s->p) : -1);
    }
    case S_SLICE: case S_SUBSTRING: case S_SUBSTR: {
        double a = arg_num(argc, argv, 0, 0), b = arg_num(argc, argv, 1, len);
        int32_t from, to;
        if (m == S_SLICE) {
            from = rel_index(a, len);
            to = rel_index(b, len);
        } else if (m == S_SUBSTRING) {
            from = a != a || a < 0 ? 0 : a > len ? len : (int32_t)a;
            to = b != b || b < 0 ? 0 : b > len ? len : (int32_t)b;
            if (from > to) {
                int32_t t = from;
                from = to;
                to = t;
            }
        } else {
            from = rel_index(a, len);
            double cnt = arg_num(argc, argv, 1, len - from);
            to = from + (cnt < 0 ? 0 : cnt > len - from ? len - from : (int32_t)cnt);
        }
        if (to < from) to = from;
        return vm_str(vm, s->p + from, (size_t)(to - from));
    }
    case S_SPLIT: {
        ctv_t out = vm_arr(vm);
        ctv_t sepv = arg(argc, argv, 0);
        double lim = arg_num(argc, argv, 1, 4294967295.0);
        if (sepv.t == CT_UNDEF) {
            vm_push(vm, out, strv_of(s));
            return out;
        }
        ct_str_t *sep = vm_to_str(vm, sepv);
        if (sep->len == 0) {
            for (int32_t i = 0; i < len && out.a->n < lim;) {
                /* one character: a whole UTF-8 sequence */
                int k = 1;
                unsigned char c = (unsigned char)s->p[i];
                if (c >= 0xF0) k = 4;
                else if (c >= 0xE0) k = 3;
                else if (c >= 0xC0) k = 2;
                if (i + k > len) k = len - i;
                vm_push(vm, out, vm_str(vm, s->p + i, (size_t)k));
                i += k;
            }
            return out;
        }
        size_t at = 0;
        for (;;) {
            if (out.a->n >= lim) break;
            const char *f = find_sub(s->p, s->len, sep->p, sep->len, at);
            if (!f) {
                vm_push(vm, out, vm_str(vm, s->p + at, s->len - at));
                break;
            }
            vm_push(vm, out, vm_str(vm, s->p + at, (size_t)(f - s->p) - at));
            at = (size_t)(f - s->p) + sep->len;
        }
        return out;
    }
    case S_REPLACE: case S_REPLACEALL: {
        ctv_t pat = arg(argc, argv, 0), rep = arg(argc, argv, 1);
        if (pat.t == CT_OBJ) ct_throw_err(vm, "TypeError", "regular expressions are not supported on the board");
        ct_str_t *p = vm_to_str(vm, pat);
        ct_sb_t b = { 0 };
        size_t at = 0;
        bool any = false;
        for (;;) {
            const char *f = find_sub(s->p, s->len, p->p, p->len, at);
            if (!f || (any && m == S_REPLACE)) break;
            any = true;
            ct_sb_add(&b, s->p + at, (size_t)(f - s->p) - at);
            if (rep.t == CT_FN) {
                ctv_t a[2] = { strv_of(p), ct_num((double)(f - s->p)) };
                ct_str_t *r = vm_to_str(vm, ct_call(vm, rep, ct_undef(), 2, a));
                ct_sb_add(&b, r->p, r->len);
            } else {
                ct_str_t *r = vm_to_str(vm, rep);
                ct_sb_add(&b, r->p, r->len);
            }
            at = (size_t)(f - s->p) + p->len;
            if (p->len == 0) {
                if (at < s->len) ct_sb_addc(&b, s->p[at]);
                at++;
                if (at > s->len) break;
            }
        }
        if (at < s->len) ct_sb_add(&b, s->p + at, s->len - at);
        ctv_t r = b.oom ? ct_undef() : vm_str(vm, b.p ? b.p : "", b.n);
        ct_sb_free(&b);
        if (r.t == CT_UNDEF) oom(vm);
        return r;
    }
    case S_TRIM: case S_TRIMSTART: case S_TRIMEND: {
        int32_t a = 0, z = len;
        if (m != S_TRIMEND) while (a < z && (s->p[a] == ' ' || s->p[a] == '\t' || s->p[a] == '\n' || s->p[a] == '\r')) a++;
        if (m != S_TRIMSTART) while (z > a && (s->p[z - 1] == ' ' || s->p[z - 1] == '\t' || s->p[z - 1] == '\n' || s->p[z - 1] == '\r')) z--;
        return vm_str(vm, s->p + a, (size_t)(z - a));
    }
    case S_UPPER: case S_LOWER: {
        ctv_t r = vm_str(vm, s->p, s->len);
        for (uint32_t i = 0; i < r.s->len; i++) {
            char c = r.s->p[i];
            if (m == S_UPPER && c >= 'a' && c <= 'z') r.s->p[i] = (char)(c - 32);
            if (m == S_LOWER && c >= 'A' && c <= 'Z') r.s->p[i] = (char)(c + 32);
        }
        return r;
    }
    case S_PADSTART: case S_PADEND: {
        int32_t want = (int32_t)arg_num(argc, argv, 0, 0);
        ct_str_t *pad = argc > 1 && argv[1].t != CT_UNDEF ? vm_to_str(vm, argv[1]) : vm_to_str(vm, vm_cstr(vm, " "));
        if (want <= len || !pad->len || want > 1 << 20) return strv_of(s);
        ct_sb_t b = { 0 };
        if (m == S_PADEND) ct_sb_add(&b, s->p, s->len);
        for (int32_t k = 0; k < want - len; k++) ct_sb_addc(&b, pad->p[k % pad->len]);
        if (m == S_PADSTART) ct_sb_add(&b, s->p, s->len);
        ctv_t r = b.oom ? ct_undef() : vm_str(vm, b.p, b.n);
        ct_sb_free(&b);
        if (r.t == CT_UNDEF) oom(vm);
        return r;
    }
    case S_CHARAT: case S_AT: {
        double i = arg_num(argc, argv, 0, 0);
        if (m == S_AT && i < 0) i += len;
        if (i < 0 || i >= len || i != i) return m == S_AT ? ct_undef() : vm_cstr(vm, "");
        return vm_str(vm, s->p + (int32_t)i, 1);
    }
    case S_CHARCODEAT: {
        double i = arg_num(argc, argv, 0, 0);
        if (i < 0 || i >= len || i != i) return ct_num(0.0 / 0.0);
        return ct_num((unsigned char)s->p[(int32_t)i]);
    }
    case S_REPEAT: {
        double n = arg_num(argc, argv, 0, 0);
        if (n < 0 || n != n || (double)len * n > (1 << 22)) ct_throw_err(vm, "RangeError", "Invalid count value");
        ct_sb_t b = { 0 };
        for (int32_t k = 0; k < (int32_t)n; k++) ct_sb_add(&b, s->p, s->len);
        ctv_t r = b.oom ? ct_undef() : vm_str(vm, b.p ? b.p : "", b.n);
        ct_sb_free(&b);
        if (r.t == CT_UNDEF) oom(vm);
        return r;
    }
    case S_CONCAT: {
        ct_sb_t b = { 0 };
        ct_sb_add(&b, s->p, s->len);
        for (int i = 0; i < argc; i++) {
            ct_str_t *a = vm_to_str(vm, argv[i]);
            ct_sb_add(&b, a->p, a->len);
        }
        ctv_t r = b.oom ? ct_undef() : vm_str(vm, b.p ? b.p : "", b.n);
        ct_sb_free(&b);
        if (r.t == CT_UNDEF) oom(vm);
        return r;
    }
    case S_TOSTRING: case S_NORMALIZE: return strv_of(s);
    case S_LOCALECOMPARE: {
        ct_str_t *o = vm_to_str(vm, arg(argc, argv, 0));
        int c = strcmp(s->p, o->p);
        return ct_num(c < 0 ? -1 : c > 0 ? 1 : 0);
    }
    case S_MATCH: ct_throw_err(vm, "TypeError", "regular expressions are not supported on the board");
    }
    return ct_undef();
}

static const struct { const char *name; int m; } STR_METHODS[] = {
    { "includes", S_INCLUDES }, { "startsWith", S_STARTS }, { "endsWith", S_ENDS }, { "indexOf", S_INDEXOF },
    { "lastIndexOf", S_LASTINDEXOF }, { "slice", S_SLICE }, { "substring", S_SUBSTRING }, { "substr", S_SUBSTR },
    { "split", S_SPLIT }, { "replace", S_REPLACE }, { "replaceAll", S_REPLACEALL }, { "trim", S_TRIM },
    { "trimStart", S_TRIMSTART }, { "trimEnd", S_TRIMEND }, { "toUpperCase", S_UPPER }, { "toLowerCase", S_LOWER },
    { "toLocaleUpperCase", S_UPPER }, { "toLocaleLowerCase", S_LOWER }, { "padStart", S_PADSTART },
    { "padEnd", S_PADEND }, { "charAt", S_CHARAT }, { "charCodeAt", S_CHARCODEAT }, { "at", S_AT },
    { "repeat", S_REPEAT }, { "concat", S_CONCAT }, { "toString", S_TOSTRING }, { "valueOf", S_TOSTRING },
    { "localeCompare", S_LOCALECOMPARE }, { "match", S_MATCH }, { "matchAll", S_MATCH }, { "search", S_MATCH },
    { "normalize", S_NORMALIZE },
};

/* merge sort: stable, as JavaScript's is, and the comparator may throw */
static int sort_cmp(ct_vm_t *vm, ctv_t fn, ctv_t a, ctv_t b)
{
    if (a.t == CT_UNDEF) return b.t == CT_UNDEF ? 0 : 1;
    if (b.t == CT_UNDEF) return -1;
    if (fn.t == CT_FN) {
        ctv_t av[2] = { a, b };
        double r = ct_to_num(ct_call(vm, fn, ct_undef(), 2, av));
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    ct_str_t *x = vm_to_str(vm, a), *y = vm_to_str(vm, b);
    int c = strcmp(x->p, y->p);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

static void msort(ct_vm_t *vm, ctv_t fn, ctv_t *v, ctv_t *tmp, uint32_t n)
{
    if (n < 2) return;
    uint32_t h = n / 2;
    msort(vm, fn, v, tmp, h);
    msort(vm, fn, v + h, tmp, n - h);
    uint32_t i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = sort_cmp(vm, fn, v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < h) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof *v);
}

enum {
    A_PUSH = 1, A_POP, A_SHIFT, A_UNSHIFT, A_INCLUDES, A_INDEXOF, A_LASTINDEXOF, A_JOIN, A_SLICE, A_SPLICE,
    A_CONCAT, A_REVERSE, A_MAP, A_FILTER, A_FOREACH, A_FIND, A_FINDINDEX, A_FINDLAST, A_SOME, A_EVERY, A_REDUCE,
    A_SORT, A_FLAT, A_FILL, A_AT, A_TOSTRING, A_KEYS, A_FLATMAP,
};

static ctv_t arr_method(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    if (self.t != CT_ARR) ct_throw_err(vm, "TypeError", "not an array");
    ct_arr_t *a = self.a;
    int m = (int)(intptr_t)ud;
    switch (m) {
    case A_PUSH:
        for (int i = 0; i < argc; i++) vm_push(vm, self, argv[i]);
        return ct_num(a->n);
    case A_POP:
        if (!a->n) return ct_undef();
        return a->v[--a->n];
    case A_SHIFT: {
        if (!a->n) return ct_undef();
        ctv_t v = a->v[0];
        ct_arr_remove(a, 0);
        return v;
    }
    case A_UNSHIFT:
        for (int i = argc - 1; i >= 0; i--) if (!ct_arr_insert(vm->ar, a, 0, argv[i])) oom(vm);
        return ct_num(a->n);
    case A_INCLUDES: case A_INDEXOF: case A_LASTINDEXOF: {
        ctv_t x = arg(argc, argv, 0);
        if (m == A_LASTINDEXOF) {
            for (int32_t i = (int32_t)a->n - 1; i >= 0; i--) if (ct_strict_eq(a->v[i], x)) return ct_num(i);
            return ct_num(-1);
        }
        int32_t from = rel_index(arg_num(argc, argv, 1, 0), (int32_t)a->n);
        for (uint32_t i = (uint32_t)from; i < a->n; i++) {
            bool eq = ct_strict_eq(a->v[i], x) ||
                      (m == A_INCLUDES && x.t == CT_NUM && x.n != x.n && a->v[i].t == CT_NUM && a->v[i].n != a->v[i].n);
            if (eq) return m == A_INCLUDES ? ct_bool(true) : ct_num(i);
        }
        return m == A_INCLUDES ? ct_bool(false) : ct_num(-1);
    }
    case A_JOIN: case A_TOSTRING: {
        ct_str_t *sep = (m == A_JOIN && argc && argv[0].t != CT_UNDEF) ? vm_to_str(vm, argv[0]) : NULL;
        ct_sb_t b = { 0 };
        for (uint32_t i = 0; i < a->n; i++) {
            if (i) {
                if (sep) ct_sb_add(&b, sep->p, sep->len);
                else ct_sb_addc(&b, ',');
            }
            if (!nullish(a->v[i])) {
                ct_str_t *s = vm_to_str(vm, a->v[i]);
                ct_sb_add(&b, s->p, s->len);
            }
        }
        ctv_t r = b.oom ? ct_undef() : vm_str(vm, b.p ? b.p : "", b.n);
        ct_sb_free(&b);
        if (r.t == CT_UNDEF) oom(vm);
        return r;
    }
    case A_SLICE: {
        int32_t from = rel_index(arg_num(argc, argv, 0, 0), (int32_t)a->n);
        int32_t to = rel_index(arg_num(argc, argv, 1, a->n), (int32_t)a->n);
        ctv_t r = vm_arr(vm);
        for (int32_t i = from; i < to; i++) vm_push(vm, r, a->v[i]);
        return r;
    }
    case A_SPLICE: {
        int32_t from = rel_index(arg_num(argc, argv, 0, 0), (int32_t)a->n);
        double dc = arg_num(argc, argv, 1, a->n - from);
        int32_t cnt = dc < 0 ? 0 : dc > (int32_t)a->n - from ? (int32_t)a->n - from : (int32_t)dc;
        if (argc < 2) cnt = (int32_t)a->n - from;
        ctv_t removed = vm_arr(vm);
        for (int32_t i = 0; i < cnt; i++) vm_push(vm, removed, a->v[from + i]);
        for (int32_t i = 0; i < cnt; i++) ct_arr_remove(a, (uint32_t)from);
        for (int i = 2; i < argc; i++) if (!ct_arr_insert(vm->ar, a, (uint32_t)(from + i - 2), argv[i])) oom(vm);
        return removed;
    }
    case A_CONCAT: {
        ctv_t r = vm_arr(vm);
        for (uint32_t i = 0; i < a->n; i++) vm_push(vm, r, a->v[i]);
        for (int k = 0; k < argc; k++) {
            if (argv[k].t == CT_ARR) {
                for (uint32_t i = 0; i < argv[k].a->n; i++) vm_push(vm, r, argv[k].a->v[i]);
            } else {
                vm_push(vm, r, argv[k]);
            }
        }
        return r;
    }
    case A_REVERSE:
        for (uint32_t i = 0; i < a->n / 2; i++) {
            ctv_t t = a->v[i];
            a->v[i] = a->v[a->n - 1 - i];
            a->v[a->n - 1 - i] = t;
        }
        return self;
    case A_MAP: case A_FILTER: case A_FOREACH: case A_FIND: case A_FINDINDEX: case A_FINDLAST: case A_SOME:
    case A_EVERY: case A_FLATMAP: {
        ctv_t fn = arg(argc, argv, 0);
        if (fn.t != CT_FN) ct_throw_err(vm, "TypeError", "%s is not a function", ct_typeof(fn));
        ctv_t r = (m == A_MAP || m == A_FILTER || m == A_FLATMAP) ? vm_arr(vm) : ct_undef();
        uint32_t n = a->n;
        if (m == A_FINDLAST) {
            for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
                ctv_t av[3] = { a->v[i], ct_num(i), self };
                if (ct_truthy(ct_call(vm, fn, arg(argc, argv, 1), 3, av))) return a->v[i];
            }
            return ct_undef();
        }
        for (uint32_t i = 0; i < n && i < a->n; i++) {
            ctv_t av[3] = { a->v[i], ct_num(i), self };
            ctv_t x = ct_call(vm, fn, arg(argc, argv, 1), 3, av);
            switch (m) {
            case A_MAP: vm_push(vm, r, x); break;
            case A_FLATMAP:
                if (x.t == CT_ARR) for (uint32_t k = 0; k < x.a->n; k++) vm_push(vm, r, x.a->v[k]);
                else vm_push(vm, r, x);
                break;
            case A_FILTER: if (ct_truthy(x)) vm_push(vm, r, a->v[i]); break;
            case A_FIND: if (ct_truthy(x)) return a->v[i]; break;
            case A_FINDINDEX: if (ct_truthy(x)) return ct_num(i); break;
            case A_SOME: if (ct_truthy(x)) return ct_bool(true); break;
            case A_EVERY: if (!ct_truthy(x)) return ct_bool(false); break;
            default: break;
            }
        }
        if (m == A_FINDINDEX) return ct_num(-1);
        if (m == A_SOME) return ct_bool(false);
        if (m == A_EVERY) return ct_bool(true);
        return r;
    }
    case A_REDUCE: {
        ctv_t fn = arg(argc, argv, 0);
        if (fn.t != CT_FN) ct_throw_err(vm, "TypeError", "%s is not a function", ct_typeof(fn));
        uint32_t i = 0;
        ctv_t acc;
        if (argc > 1) {
            acc = argv[1];
        } else {
            if (!a->n) ct_throw_err(vm, "TypeError", "Reduce of empty array with no initial value");
            acc = a->v[0];
            i = 1;
        }
        for (; i < a->n; i++) {
            ctv_t av[4] = { acc, a->v[i], ct_num(i), self };
            acc = ct_call(vm, fn, ct_undef(), 4, av);
        }
        return acc;
    }
    case A_SORT: {
        if (a->n > 1) {
            ctv_t *tmp = vm_alloc(vm, a->n * sizeof(ctv_t));
            msort(vm, arg(argc, argv, 0), a->v, tmp, a->n);
        }
        return self;
    }
    case A_FLAT: {
        ctv_t r = vm_arr(vm);
        for (uint32_t i = 0; i < a->n; i++) {
            if (a->v[i].t == CT_ARR) for (uint32_t k = 0; k < a->v[i].a->n; k++) vm_push(vm, r, a->v[i].a->v[k]);
            else vm_push(vm, r, a->v[i]);
        }
        return r;
    }
    case A_FILL: {
        int32_t from = rel_index(arg_num(argc, argv, 1, 0), (int32_t)a->n);
        int32_t to = rel_index(arg_num(argc, argv, 2, a->n), (int32_t)a->n);
        for (int32_t i = from; i < to; i++) a->v[i] = arg(argc, argv, 0);
        return self;
    }
    case A_AT: {
        double i = arg_num(argc, argv, 0, 0);
        if (i < 0) i += a->n;
        if (i < 0 || i >= a->n || i != i) return ct_undef();
        return a->v[(uint32_t)i];
    }
    case A_KEYS: {
        ctv_t r = vm_arr(vm);
        for (uint32_t i = 0; i < a->n; i++) vm_push(vm, r, ct_num(i));
        return r;
    }
    }
    return ct_undef();
}

static const struct { const char *name; int m; } ARR_METHODS[] = {
    { "push", A_PUSH }, { "pop", A_POP }, { "shift", A_SHIFT }, { "unshift", A_UNSHIFT }, { "includes", A_INCLUDES },
    { "indexOf", A_INDEXOF }, { "lastIndexOf", A_LASTINDEXOF }, { "join", A_JOIN }, { "slice", A_SLICE },
    { "splice", A_SPLICE }, { "concat", A_CONCAT }, { "reverse", A_REVERSE }, { "map", A_MAP },
    { "filter", A_FILTER }, { "forEach", A_FOREACH }, { "find", A_FIND }, { "findIndex", A_FINDINDEX },
    { "findLast", A_FINDLAST }, { "some", A_SOME }, { "every", A_EVERY }, { "reduce", A_REDUCE }, { "sort", A_SORT },
    { "flat", A_FLAT }, { "flatMap", A_FLATMAP }, { "fill", A_FILL }, { "at", A_AT }, { "toString", A_TOSTRING },
    { "keys", A_KEYS },
};

static ctv_t num_method(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    double d = self.t == CT_NUM ? self.n : ct_to_num(self);
    int m = (int)(intptr_t)ud;
    char b[400];
    if (m == 1) {   /* toFixed */
        int k = (int)arg_num(argc, argv, 0, 0);
        if (k < 0 || k > 100) ct_throw_err(vm, "RangeError", "toFixed() digits argument must be between 0 and 100");
        if (d != d) return vm_cstr(vm, "NaN");
        snprintf(b, sizeof b, "%.*f", k, d);
        return vm_cstr(vm, b);
    }
    if (m == 2) {   /* toString(radix) */
        int radix = (int)arg_num(argc, argv, 0, 10);
        if (radix == 10 || d != d || d != d_trunc(d)) {
            ct_num_fmt(d, b);
            return vm_cstr(vm, b);
        }
        if (radix < 2 || radix > 36) ct_throw_err(vm, "RangeError", "toString() radix must be between 2 and 36");
        bool neg = d < 0;
        uint64_t u = (uint64_t)(neg ? -d : d);
        char t[80];
        int k = 0;
        do {
            t[k++] = "0123456789abcdefghijklmnopqrstuvwxyz"[u % (uint64_t)radix];
            u /= (uint64_t)radix;
        } while (u && k < 78);
        int o = 0;
        if (neg) b[o++] = '-';
        while (k) b[o++] = t[--k];
        b[o] = '\0';
        return vm_cstr(vm, b);
    }
    if (m == 3) {   /* toPrecision */
        int k = (int)arg_num(argc, argv, 0, 0);
        if (argc == 0 || argv[0].t == CT_UNDEF) {
            ct_num_fmt(d, b);
            return vm_cstr(vm, b);
        }
        if (k < 1 || k > 100) ct_throw_err(vm, "RangeError", "toPrecision() argument must be between 1 and 100");
        snprintf(b, sizeof b, "%.*g", k, d);
        return vm_cstr(vm, b);
    }
    return self;    /* valueOf */
}

static ctv_t obj_has_own(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)ud;
    ct_str_t *k = vm_to_str(vm, arg(argc, argv, 0));
    if (self.t == CT_OBJ) return ct_bool(ct_obj_find(self.o, k->p, k->len) != NULL);
    if (self.t == CT_ARR) {
        double i = ct_to_num(strv_of(k));
        return ct_bool(i >= 0 && i < self.a->n);
    }
    return ct_bool(false);
}

static ctv_t obj_to_string(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)argc;
    (void)argv;
    (void)ud;
    return strv_of(vm_to_str(vm, self));
}

static ctv_t fn_call(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    int m = (int)(intptr_t)ud;
    if (self.t != CT_FN) ct_throw_err(vm, "TypeError", "not a function");
    if (m == 1) return ct_call(vm, self, arg(argc, argv, 0), argc > 0 ? argc - 1 : 0, argv + 1);
    if (m == 2) {
        ctv_t list = arg(argc, argv, 1);
        if (list.t == CT_ARR) return ct_call(vm, self, arg(argc, argv, 0), (int)list.a->n, list.a->v);
        return ct_call(vm, self, arg(argc, argv, 0), 0, NULL);
    }
    /* bind: a copy with its 'this' fixed (arguments are not bound) */
    ct_fn_t *f = vm_alloc(vm, sizeof *f);
    *f = *self.f;
    f->self = arg(argc, argv, 0);
    f->bound = true;
    ctv_t r = ct_undef();
    r.t = CT_FN;
    r.f = f;
    return r;
}

static ctv_t bound_method(ct_vm_t *vm, ctv_t self, ct_native_t f, int m, const char *name)
{
    ctv_t v = vm_fn(vm, f, (void *)(intptr_t)m, name);
    v.f->self = self;
    v.f->bound = true;
    return v;
}

static bool index_of(const char *k, size_t kl, uint32_t *out)
{
    if (!kl || kl > 9) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < kl; i++) {
        if (k[i] < '0' || k[i] > '9') return false;
        v = v * 10 + (uint32_t)(k[i] - '0');
    }
    if (kl > 1 && k[0] == '0') return false;
    *out = v;
    return true;
}

ctv_t ct_get_prop(ct_vm_t *vm, ctv_t o, const char *k, size_t kl)
{
    uint32_t idx;
    switch (o.t) {
    case CT_UNDEF: case CT_NULL:
        ct_throw_err(vm, "TypeError", "Cannot read properties of %s (reading '%.*s')", o.t == CT_NULL ? "null" : "undefined",
                     (int)(kl > 60 ? 60 : kl), k);
    case CT_STR:
        if (kl == 6 && !memcmp(k, "length", 6)) return ct_num(o.s->len);
        if (index_of(k, kl, &idx)) return idx < o.s->len ? vm_str(vm, o.s->p + idx, 1) : ct_undef();
        for (size_t i = 0; i < sizeof STR_METHODS / sizeof STR_METHODS[0]; i++) {
            if (strlen(STR_METHODS[i].name) == kl && !memcmp(STR_METHODS[i].name, k, kl)) {
                return bound_method(vm, o, str_method, STR_METHODS[i].m, STR_METHODS[i].name);
            }
        }
        return ct_undef();
    case CT_NUM: case CT_BOOL:
        if (kl == 7 && !memcmp(k, "toFixed", 7)) return bound_method(vm, o, num_method, 1, "toFixed");
        if (kl == 8 && !memcmp(k, "toString", 8)) return bound_method(vm, o, num_method, o.t == CT_NUM ? 2 : 0, "toString");
        if (kl == 11 && !memcmp(k, "toPrecision", 11)) return bound_method(vm, o, num_method, 3, "toPrecision");
        if (kl == 7 && !memcmp(k, "valueOf", 7)) return bound_method(vm, o, num_method, 0, "valueOf");
        if (o.t == CT_BOOL && kl == 8 && !memcmp(k, "toString", 8)) return bound_method(vm, o, obj_to_string, 0, "toString");
        return ct_undef();
    case CT_ARR:
        if (kl == 6 && !memcmp(k, "length", 6)) return ct_num(o.a->n);
        if (index_of(k, kl, &idx)) return idx < o.a->n ? o.a->v[idx] : ct_undef();
        for (size_t i = 0; i < sizeof ARR_METHODS / sizeof ARR_METHODS[0]; i++) {
            if (strlen(ARR_METHODS[i].name) == kl && !memcmp(ARR_METHODS[i].name, k, kl)) {
                return bound_method(vm, o, arr_method, ARR_METHODS[i].m, ARR_METHODS[i].name);
            }
        }
        if (kl == 14 && !memcmp(k, "hasOwnProperty", 14)) return bound_method(vm, o, obj_has_own, 0, "hasOwnProperty");
        return ct_undef();
    case CT_OBJ: {
        if (o.o->get) {
            bool found = false;
            ctv_t v = o.o->get(vm, o.o, k, kl, &found);
            if (found) return v;
        }
        ctv_t *p = ct_obj_find(o.o, k, kl);
        if (p) return *p;
        if (kl == 14 && !memcmp(k, "hasOwnProperty", 14)) return bound_method(vm, o, obj_has_own, 0, "hasOwnProperty");
        if (kl == 8 && !memcmp(k, "toString", 8)) return bound_method(vm, o, obj_to_string, 0, "toString");
        return ct_undef();
    }
    case CT_FN:
        if (kl == 4 && !memcmp(k, "call", 4)) return bound_method(vm, o, fn_call, 1, "call");
        if (kl == 5 && !memcmp(k, "apply", 5)) return bound_method(vm, o, fn_call, 2, "apply");
        if (kl == 4 && !memcmp(k, "bind", 4)) return bound_method(vm, o, fn_call, 3, "bind");
        if (kl == 4 && !memcmp(k, "name", 4)) return vm_cstr(vm, o.f->name ? o.f->name : "");
        return ct_undef();
    }
    return ct_undef();
}

static void set_prop(ct_vm_t *vm, ctv_t o, const char *k, size_t kl, ctv_t v)
{
    uint32_t idx;
    switch (o.t) {
    case CT_UNDEF: case CT_NULL:
        ct_throw_err(vm, "TypeError", "Cannot set properties of %s (setting '%.*s')", o.t == CT_NULL ? "null" : "undefined",
                     (int)(kl > 60 ? 60 : kl), k);
    case CT_ARR:
        if (kl == 6 && !memcmp(k, "length", 6)) {
            double n = ct_to_num(v);
            if (n < 0 || n != d_trunc(n) || n > (1 << 24)) ct_throw_err(vm, "RangeError", "Invalid array length");
            while (o.a->n < (uint32_t)n) vm_push(vm, o, ct_undef());
            o.a->n = (uint32_t)n;
            return;
        }
        if (index_of(k, kl, &idx)) {
            if (idx > (1u << 24)) ct_throw_err(vm, "RangeError", "Invalid array index");
            while (o.a->n <= idx) vm_push(vm, o, ct_undef());
            o.a->v[idx] = v;
            return;
        }
        return;     /* other properties of an array are not kept */
    case CT_OBJ:
        if (!ct_obj_set(vm->ar, o.o, k, kl, v)) oom(vm);
        return;
    default:
        return;     /* a primitive's properties are dropped, as in sloppy mode */
    }
}

/* a property key out of a value: o[k] */
static ct_str_t *key_of(ct_vm_t *vm, ctv_t k)
{
    return vm_to_str(vm, k);
}

/* ---- calling -------------------------------------------------------------------------------- */

static ctv_t eval(ct_vm_t *vm, scope_t *sc, node_t *n);
static int exec(ct_vm_t *vm, scope_t *sc, node_t *n);
static void bind_pattern(ct_vm_t *vm, scope_t *sc, node_t *pat, ctv_t v, bool konst);

static void step(ct_vm_t *vm)
{
    if ((++vm->steps & 1023) == 0) {
        if (vm->steps > MAX_STEPS || aos_hal_uptime_ms() - vm->t0 > MAX_MS) {
            ct_throw_err(vm, "RangeError", "the script took too long (an endless loop?)");
        }
        if (vm->ctx->stop && *vm->ctx->stop) ct_throw_err(vm, "Error", "stopped");
    }
}

static void hoist(ct_vm_t *vm, scope_t *sc, node_t **list, int n)
{
    for (int i = 0; i < n; i++) {
        node_t *s = list[i];
        if (!s || s->k != N_FUNCDECL) continue;
        ct_fn_t *f = vm_alloc(vm, sizeof *f);
        f->node = s->a;
        f->scope = sc;
        f->name = s->s->p;
        ctv_t v = ct_undef();
        v.t = CT_FN;
        v.f = f;
        ct_declare(vm, sc, s->s, v, false);
    }
}

ctv_t ct_call(ct_vm_t *vm, ctv_t fn, ctv_t self, int argc, ctv_t *argv)
{
    step(vm);
    if (fn.t == CT_OBJ) {
        /* the globals that are both a function and an object (Number,
         * String, Date, Error, Array, Object) answer calls with __call */
        ctv_t *c = ct_obj_find(fn.o, "__call", 6);
        if (c && c->t == CT_FN) return ct_call(vm, *c, self, argc, argv);
    }
    if (fn.t != CT_FN) ct_throw_err(vm, "TypeError", "%s is not a function", ct_typeof(fn));
    ct_fn_t *f = fn.f;
    if (f->bound) self = f->self;
    if (f->native) return f->native(vm, self, argc, argv, f->ud);
    if (++vm->depth > MAX_DEPTH) {
        vm->depth = 0;
        ct_throw_err(vm, "RangeError", "Maximum call stack size exceeded");
    }
    node_t *fnode = f->node;
    scope_t *sc = scope_new(vm, f->scope);
    for (int i = 0; i < fnode->n; i++) {
        node_t *pp = fnode->list[i];
        ctv_t v;
        if (pp->flag & NF_REST) {
            v = vm_arr(vm);
            for (int k = i; k < argc; k++) vm_push(vm, v, argv[k]);
        } else {
            v = i < argc ? argv[i] : ct_undef();
            if (v.t == CT_UNDEF && pp->b) v = eval(vm, sc, pp->b);
        }
        bind_pattern(vm, sc, pp->a, v, false);
    }
    ctv_t saved_this = vm->this_v;
    if (!(fnode->flag & NF_ARROW)) vm->this_v = self;
    ctv_t r = ct_undef();
    if (fnode->flag & NF_EXPRBODY) {
        r = eval(vm, sc, fnode->a);
    } else {
        node_t *body = fnode->a;
        hoist(vm, sc, body->list, body->n);
        for (int i = 0; i < body->n; i++) {
            int c = exec(vm, sc, body->list[i]);
            if (c == C_RETURN) {
                r = vm->ret;
                break;
            }
            if (c != C_NORMAL) break;
        }
    }
    vm->this_v = saved_this;
    vm->depth--;
    return r;
}

static ctv_t make_closure(ct_vm_t *vm, scope_t *sc, node_t *n)
{
    ct_fn_t *f = vm_alloc(vm, sizeof *f);
    f->node = n;
    f->scope = sc;
    f->name = n->s ? n->s->p : "";
    if (n->flag & NF_ARROW) {
        f->self = vm->this_v;
        f->bound = true;
    }
    ctv_t v = ct_undef();
    v.t = CT_FN;
    v.f = f;
    return v;
}

/* ---- evaluation ----------------------------------------------------------------------------- */

static double num_op(int op, double a, double b)
{
    switch (op) {
    case OP_SUB: return a - b;
    case OP_MUL: return a * b;
    case OP_DIV: return a / b;
    case OP_MOD: return d_fmod(a, b);
    case OP_EXP: return d_pow(a, b);
    case OP_BAND: return to_i32(a) & to_i32(b);
    case OP_BOR: return to_i32(a) | to_i32(b);
    case OP_BXOR: return to_i32(a) ^ to_i32(b);
    case OP_SHL: return (int32_t)((uint32_t)to_i32(a) << (to_i32(b) & 31));
    case OP_SHR: return to_i32(a) >> (to_i32(b) & 31);
    case OP_USHR: return (uint32_t)to_i32(a) >> (to_i32(b) & 31);
    }
    return 0.0 / 0.0;
}

static ctv_t binop_eval(ct_vm_t *vm, int op, ctv_t a, ctv_t b)
{
    switch (op) {
    case OP_ADD:
        if (a.t == CT_NUM && b.t == CT_NUM) return ct_num(a.n + b.n);
        if (a.t == CT_STR || b.t == CT_STR || a.t == CT_ARR || b.t == CT_ARR || a.t == CT_OBJ || b.t == CT_OBJ ||
            a.t == CT_FN || b.t == CT_FN) {
            if (a.t == CT_OBJ && a.o->kind == CT_K_DATE && b.t == CT_NUM) return ct_num(ct_to_num(a) + b.n);
            ct_str_t *x = vm_to_str(vm, a), *y = vm_to_str(vm, b);
            ct_str_t *s = vm_alloc(vm, sizeof(ct_str_t) + x->len + y->len + 1);
            memcpy(s->p, x->p, x->len);
            memcpy(s->p + x->len, y->p, y->len);
            s->len = x->len + y->len;
            s->p[s->len] = '\0';
            return strv_of(s);
        }
        return ct_num(ct_to_num(a) + ct_to_num(b));
    case OP_EQ: return ct_bool(ct_loose_eq(vm, a, b));
    case OP_NE: return ct_bool(!ct_loose_eq(vm, a, b));
    case OP_SEQ: return ct_bool(ct_strict_eq(a, b));
    case OP_SNE: return ct_bool(!ct_strict_eq(a, b));
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
        if (a.t == CT_STR && b.t == CT_STR) {
            int c = strcmp(a.s->p, b.s->p);
            switch (op) {
            case OP_LT: return ct_bool(c < 0);
            case OP_GT: return ct_bool(c > 0);
            case OP_LE: return ct_bool(c <= 0);
            default: return ct_bool(c >= 0);
            }
        }
        double x = ct_to_num(a), y = ct_to_num(b);
        switch (op) {
        case OP_LT: return ct_bool(x < y);
        case OP_GT: return ct_bool(x > y);
        case OP_LE: return ct_bool(x <= y);
        default: return ct_bool(x >= y);
        }
    }
    case OP_IN: {
        ct_str_t *k = key_of(vm, a);
        if (b.t == CT_OBJ) return ct_bool(ct_obj_find(b.o, k->p, k->len) != NULL);
        if (b.t == CT_ARR) {
            uint32_t i;
            return ct_bool((k->len == 6 && !memcmp(k->p, "length", 6)) || (index_of(k->p, k->len, &i) && i < b.a->n));
        }
        ct_throw_err(vm, "TypeError", "Cannot use 'in' operator to search for '%s' in %s", k->p, ct_typeof(b));
    }
    case OP_INSTANCEOF: {
        ctv_t tag = b.t == CT_OBJ ? ct_get(b, "__tag") : ct_undef();
        const char *t = ct_s(tag);
        if (!strcmp(t, "Array")) return ct_bool(a.t == CT_ARR);
        if (!strcmp(t, "Object")) return ct_bool(a.t == CT_OBJ || a.t == CT_ARR || a.t == CT_FN);
        if (!strcmp(t, "Error")) return ct_bool(a.t == CT_OBJ && a.o->kind == CT_K_ERROR);
        if (!strcmp(t, "Date")) return ct_bool(a.t == CT_OBJ && a.o->kind == CT_K_DATE);
        if (!strcmp(t, "Function")) return ct_bool(a.t == CT_FN);
        if (b.t == CT_FN || tag.t == CT_STR) return ct_bool(false);
        ct_throw_err(vm, "TypeError", "Right-hand side of 'instanceof' is not callable");
    }
    default:
        return ct_num(num_op(op, ct_to_num(a), ct_to_num(b)));
    }
}

typedef struct {
    int       kind;     /* N_IDENT, N_MEMBER, N_INDEX */
    var_t    *var;
    ct_str_t *name;
    ctv_t     obj;
    ct_str_t *key;
} ref_t;

static ref_t reference(ct_vm_t *vm, scope_t *sc, node_t *n)
{
    ref_t r = { .kind = n->k };
    if (n->k == N_IDENT) {
        r.var = lookup(sc, n->s);
        r.name = n->s;
    } else if (n->k == N_MEMBER) {
        r.obj = eval(vm, sc, n->a);
        r.key = n->s;
    } else {
        r.obj = eval(vm, sc, n->a);
        r.key = key_of(vm, eval(vm, sc, n->b));
    }
    return r;
}

static ctv_t ref_get(ct_vm_t *vm, ref_t *r)
{
    if (r->kind == N_IDENT) {
        if (!r->var) ct_throw_err(vm, "ReferenceError", "%s is not defined", r->name->p);
        return r->var->v;
    }
    return ct_get_prop(vm, r->obj, r->key->p, r->key->len);
}

static void ref_set(ct_vm_t *vm, ref_t *r, ctv_t v)
{
    if (r->kind == N_IDENT) {
        if (!r->var) {
            /* an assignment to an undeclared name makes a global, as sloppy
             * scripts expect */
            ct_declare(vm, vm->global, r->name, v, false);
            r->var = lookup(vm->global, r->name);
            return;
        }
        if (r->var->konst) ct_throw_err(vm, "TypeError", "Assignment to constant variable '%s'.", r->name->p);
        r->var->v = v;
        return;
    }
    set_prop(vm, r->obj, r->key->p, r->key->len, v);
}

static void bind_pattern(ct_vm_t *vm, scope_t *sc, node_t *pat, ctv_t v, bool konst)
{
    if (pat->k == N_IDENT) {
        ct_declare(vm, sc, pat->s, v, konst);
        return;
    }
    if (nullish(v)) ct_throw_err(vm, "TypeError", "Cannot destructure '%s' as it is %s.", ct_typeof(v),
                                 v.t == CT_NULL ? "null" : "undefined");
    if (pat->k == N_OBJPAT) {
        for (int i = 0; i < pat->n; i++) {
            node_t *pp = pat->list[i];
            if (!pp) continue;
            ctv_t x;
            if (pp->flag & NF_REST) {
                x = vm_obj(vm);
                if (v.t == CT_OBJ) {
                    for (uint32_t k = 0; k < v.o->n; k++) {
                        bool taken = false;
                        for (int j = 0; j < pat->n; j++) {
                            node_t *q = pat->list[j];
                            if (q && !(q->flag & NF_REST) && q->s->len == v.o->p[k].k->len &&
                                !memcmp(q->s->p, v.o->p[k].k->p, q->s->len)) {
                                taken = true;
                            }
                        }
                        if (!taken) vm_set(vm, x, v.o->p[k].k->p, v.o->p[k].v);
                    }
                }
            } else {
                x = ct_get_prop(vm, v, pp->s->p, pp->s->len);
                if (x.t == CT_UNDEF && pp->b) x = eval(vm, sc, pp->b);
            }
            bind_pattern(vm, sc, pp->a, x, konst);
        }
        return;
    }
    /* [a, , b, ...rest] */
    for (int i = 0; i < pat->n; i++) {
        node_t *pp = pat->list[i];
        if (!pp) continue;
        ctv_t x;
        if (pp->flag & NF_REST) {
            x = vm_arr(vm);
            if (v.t == CT_ARR) for (uint32_t k = (uint32_t)i; k < v.a->n; k++) vm_push(vm, x, v.a->v[k]);
        } else {
            x = v.t == CT_ARR ? ct_at(v, (uint32_t)i)
                : v.t == CT_STR ? ((uint32_t)i < v.s->len ? vm_str(vm, v.s->p + i, 1) : ct_undef()) : ct_undef();
            if (x.t == CT_UNDEF && pp->b) x = eval(vm, sc, pp->b);
        }
        bind_pattern(vm, sc, pp->a, x, konst);
    }
}

static int eval_args(ct_vm_t *vm, scope_t *sc, node_t *n, ctv_t **out)
{
    int cnt = 0;
    for (int i = 0; i < n->n; i++) {
        if (n->list[i]->k == N_SPREAD) {
            ctv_t a = eval(vm, sc, n->list[i]->a);
            if (a.t != CT_ARR) ct_throw_err(vm, "TypeError", "spread of a value that is not an array");
            cnt += (int)a.a->n;
        } else {
            cnt++;
        }
    }
    ctv_t *v = cnt ? vm_alloc(vm, (size_t)cnt * sizeof *v) : NULL;
    int k = 0;
    for (int i = 0; i < n->n; i++) {
        if (n->list[i]->k == N_SPREAD) {
            ctv_t a = eval(vm, sc, n->list[i]->a);
            for (uint32_t j = 0; j < a.a->n && k < cnt; j++) v[k++] = a.a->v[j];
        } else if (k < cnt) {
            v[k++] = eval(vm, sc, n->list[i]);
        }
    }
    *out = v;
    return k;
}

static ctv_t eval(ct_vm_t *vm, scope_t *sc, node_t *n)
{
    step(vm);
    vm->line = n->line;
    switch (n->k) {
    case N_NUM:
        if (n->op == 1) return ct_bool(n->num != 0);
        if (n->op == 2) return ct_null();
        return ct_num(n->num);
    case N_STR:
        return strv_of(n->s);
    case N_TEMPLATE: {
        ct_sb_t b = { 0 };
        for (int i = 0; i < n->n; i++) {
            ctv_t v = eval(vm, sc, n->list[i]);
            ct_str_t *s = vm_to_str(vm, v);
            ct_sb_add(&b, s->p, s->len);
        }
        ctv_t r = b.oom ? ct_undef() : vm_str(vm, b.p ? b.p : "", b.n);
        ct_sb_free(&b);
        if (r.t == CT_UNDEF) oom(vm);
        return r;
    }
    case N_IDENT: {
        var_t *v = lookup(sc, n->s);
        if (!v) {
            if (n->s->len == 9 && !memcmp(n->s->p, "undefined", 9)) return ct_undef();
            if (n->s->len == 3 && !memcmp(n->s->p, "NaN", 3)) return ct_num(0.0 / 0.0);
            if (n->s->len == 8 && !memcmp(n->s->p, "Infinity", 8)) return ct_num(1.0 / 0.0);
            ct_throw_err(vm, "ReferenceError", "%s is not defined", n->s->p);
        }
        return v->v;
    }
    case N_THIS:
        return vm->this_v;
    case N_ARRAY: {
        ctv_t a = vm_arr(vm);
        for (int i = 0; i < n->n; i++) {
            node_t *e = n->list[i];
            if (!e) {
                vm_push(vm, a, ct_undef());
            } else if (e->k == N_SPREAD) {
                ctv_t s = eval(vm, sc, e->a);
                if (s.t == CT_ARR) for (uint32_t k = 0; k < s.a->n; k++) vm_push(vm, a, s.a->v[k]);
                else if (s.t == CT_STR) for (uint32_t k = 0; k < s.s->len; k++) vm_push(vm, a, vm_str(vm, s.s->p + k, 1));
                else ct_throw_err(vm, "TypeError", "%s is not iterable", ct_typeof(s));
            } else {
                vm_push(vm, a, eval(vm, sc, e));
            }
        }
        return a;
    }
    case N_OBJECT: {
        ctv_t o = vm_obj(vm);
        for (int i = 0; i < n->n; i++) {
            node_t *p = n->list[i];
            if (p->k == N_SPREAD) {
                ctv_t s = eval(vm, sc, p->a);
                if (s.t == CT_OBJ) for (uint32_t k = 0; k < s.o->n; k++) vm_set(vm, o, s.o->p[k].k->p, s.o->p[k].v);
                else if (s.t == CT_ARR) for (uint32_t k = 0; k < s.a->n; k++) {
                    char b[16];
                    snprintf(b, sizeof b, "%u", (unsigned)k);
                    vm_set(vm, o, b, s.a->v[k]);
                }
                continue;
            }
            ct_str_t *k = (p->flag & NF_COMPUTED) ? key_of(vm, eval(vm, sc, p->b)) : p->s;
            ctv_t v = eval(vm, sc, p->a);
            if (!ct_obj_set(vm->ar, o.o, k->p, k->len, v)) oom(vm);
        }
        return o;
    }
    case N_FUNC:
        return make_closure(vm, sc, n);
    case N_MEMBER: case N_INDEX: {
        ctv_t o = eval(vm, sc, n->a);
        ctv_t r = ct_undef();
        if (vm->shorted) goto chain_end;
        if ((n->flag & NF_OPTIONAL) && nullish(o)) {
            vm->shorted = true;
            goto chain_end;
        }
        if (n->k == N_MEMBER) {
            r = ct_get_prop(vm, o, n->s->p, n->s->len);
        } else {
            ctv_t k = eval(vm, sc, n->b);
            if (k.t == CT_NUM && k.n >= 0 && k.n == d_trunc(k.n) && k.n < 4294967295.0) {
                if (o.t == CT_ARR) {
                    r = (uint32_t)k.n < o.a->n ? o.a->v[(uint32_t)k.n] : ct_undef();
                    goto chain_end;
                }
            }
            ct_str_t *ks = key_of(vm, k);
            r = ct_get_prop(vm, o, ks->p, ks->len);
        }
chain_end:
        if (n->flag & NF_SHORT) vm->shorted = false;
        return r;
    }
    case N_CALL: {
        ctv_t self = ct_undef(), fn = ct_undef();
        ctv_t r = ct_undef();
        node_t *c = n->a;
        if (c->k == N_MEMBER || c->k == N_INDEX) {
            self = eval(vm, sc, c->a);
            if (vm->shorted) goto call_end;
            if ((c->flag & NF_OPTIONAL) && nullish(self)) {
                vm->shorted = true;
                goto call_end;
            }
            if (c->k == N_MEMBER) {
                fn = ct_get_prop(vm, self, c->s->p, c->s->len);
            } else {
                ct_str_t *ks = key_of(vm, eval(vm, sc, c->b));
                fn = ct_get_prop(vm, self, ks->p, ks->len);
            }
            if (c->flag & NF_SHORT) vm->shorted = false;
        } else {
            fn = eval(vm, sc, c);
            if (vm->shorted) goto call_end;
        }
        if ((n->flag & NF_OPTIONAL) && nullish(fn)) {
            vm->shorted = true;
            goto call_end;
        }
        if (fn.t != CT_FN && !(fn.t == CT_OBJ && ct_obj_find(fn.o, "__call", 6))) {
            const char *what = c->k == N_IDENT ? c->s->p : c->k == N_MEMBER ? c->s->p : "expression";
            ct_throw_err(vm, "TypeError", "%s is not a function", what);
        }
        {
            ctv_t *av;
            int argc = eval_args(vm, sc, n, &av);
            int line = n->line;
            r = ct_call(vm, fn, self, argc, av);
            vm->line = line;
        }
call_end:
        if (n->flag & NF_SHORT) vm->shorted = false;
        return r;
    }
    case N_NEW: {
        ctv_t fn = eval(vm, sc, n->a);
        ctv_t *av;
        int argc = eval_args(vm, sc, n, &av);
        if (fn.t == CT_OBJ) {
            ctv_t *c = ct_obj_find(fn.o, "__new", 5);
            if (!c) c = ct_obj_find(fn.o, "__call", 6);
            if (c && c->t == CT_FN) return ct_call(vm, *c, ct_undef(), argc, av);
        }
        if (fn.t == CT_FN && !fn.f->native) {
            /* a constructor function: a new object as its 'this' */
            ctv_t o = vm_obj(vm);
            ctv_t r = ct_call(vm, fn, o, argc, av);
            return (r.t == CT_OBJ || r.t == CT_ARR) ? r : o;
        }
        ct_throw_err(vm, "TypeError", "%s is not a constructor", n->a->k == N_IDENT ? n->a->s->p : "expression");
    }
    case N_UNARY: {
        if (n->op == OP_TYPEOF && n->a->k == N_IDENT && !lookup(sc, n->a->s)) return vm_cstr(vm, "undefined");
        if (n->op == OP_DELETE) {
            if (n->a->k == N_MEMBER || n->a->k == N_INDEX) {
                ref_t r = reference(vm, sc, n->a);
                if (r.obj.t == CT_OBJ) ct_obj_del(r.obj.o, r.key->p, r.key->len);
                else if (r.obj.t == CT_ARR) {
                    uint32_t i;
                    if (index_of(r.key->p, r.key->len, &i) && i < r.obj.a->n) r.obj.a->v[i] = ct_undef();
                }
            }
            return ct_bool(true);
        }
        ctv_t v = eval(vm, sc, n->a);
        switch (n->op) {
        case OP_NOT: return ct_bool(!ct_truthy(v));
        case OP_NEG: return ct_num(-ct_to_num(v));
        case OP_POS: return ct_num(ct_to_num(v));
        case OP_BNOT: return ct_num(~to_i32(ct_to_num(v)));
        case OP_TYPEOF: return vm_cstr(vm, ct_typeof(v));
        case OP_VOID: return ct_undef();
        }
        return ct_undef();
    }
    case N_UPDATE: {
        ref_t r = reference(vm, sc, n->a);
        double old = ct_to_num(ref_get(vm, &r));
        double nv = n->op == OP_INC ? old + 1 : old - 1;
        ref_set(vm, &r, ct_num(nv));
        return ct_num((n->flag & NF_PREFIX) ? nv : old);
    }
    case N_BINARY:
        {
            ctv_t a = eval(vm, sc, n->a);
            ctv_t b = eval(vm, sc, n->b);
            return binop_eval(vm, n->op, a, b);
        }
    case N_LOGICAL: {
        ctv_t a = eval(vm, sc, n->a);
        if (n->op == OP_AND) return ct_truthy(a) ? eval(vm, sc, n->b) : a;
        if (n->op == OP_OR) return ct_truthy(a) ? a : eval(vm, sc, n->b);
        return nullish(a) ? eval(vm, sc, n->b) : a;
    }
    case N_COND:
        return ct_truthy(eval(vm, sc, n->a)) ? eval(vm, sc, n->b) : eval(vm, sc, n->c);
    case N_ASSIGN: {
        ref_t r = reference(vm, sc, n->a);
        ctv_t v;
        if (n->op == OP_SET) {
            v = eval(vm, sc, n->b);
            /* a function assigned to a name takes that name */
            if (v.t == CT_FN && !v.f->native && (!v.f->name || !v.f->name[0]) && n->a->k == N_IDENT) {
                v.f->name = n->a->s->p;
            }
        } else if (n->op == OP_AND || n->op == OP_OR || n->op == OP_NULLISH) {
            ctv_t cur = ref_get(vm, &r);
            bool keep = n->op == OP_AND ? !ct_truthy(cur) : n->op == OP_OR ? ct_truthy(cur) : !nullish(cur);
            if (keep) return cur;
            v = eval(vm, sc, n->b);
        } else {
            ctv_t cur = ref_get(vm, &r);
            v = binop_eval(vm, n->op, cur, eval(vm, sc, n->b));
        }
        ref_set(vm, &r, v);
        return v;
    }
    case N_SEQ: {
        ctv_t v = ct_undef();
        for (int i = 0; i < n->n; i++) v = eval(vm, sc, n->list[i]);
        return v;
    }
    }
    ct_throw_err(vm, "SyntaxError", "unexpected construct");
}

/* ---- statements ----------------------------------------------------------------------------- */

static int exec_list(ct_vm_t *vm, scope_t *sc, node_t **list, int n)
{
    hoist(vm, sc, list, n);
    for (int i = 0; i < n; i++) {
        int c = exec(vm, sc, list[i]);
        if (c != C_NORMAL) return c;
    }
    return C_NORMAL;
}

static void declare_var(ct_vm_t *vm, scope_t *sc, node_t *decl)
{
    bool konst = (decl->flag & NF_CONST) != 0;
    for (int i = 0; i < decl->n; i++) {
        node_t *d = decl->list[i];
        ctv_t v = d->b ? eval(vm, sc, d->b) : ct_undef();
        if (v.t == CT_FN && !v.f->native && (!v.f->name || !v.f->name[0]) && d->a->k == N_IDENT) {
            v.f->name = d->a->s->p;
        }
        bind_pattern(vm, sc, d->a, v, konst);
    }
}

/* one loop body: break and continue are the loop's, the rest goes up */
static int loop_body(ct_vm_t *vm, scope_t *sc, node_t *body, bool *stop)
{
    int c = exec(vm, sc, body);
    if (c == C_BREAK) {
        *stop = true;
        return C_NORMAL;
    }
    if (c == C_CONTINUE) return C_NORMAL;
    if (c == C_RETURN) *stop = true;
    return c;
}

static int exec(ct_vm_t *vm, scope_t *sc, node_t *n)
{
    step(vm);
    vm->line = n->line;
    switch (n->k) {
    case N_EMPTY: case N_FUNCDECL:
        return C_NORMAL;
    case N_EXPR:
        eval(vm, sc, n->a);
        return C_NORMAL;
    case N_VAR:
        declare_var(vm, sc, n);
        return C_NORMAL;
    case N_BLOCK:
        return exec_list(vm, (n->flag & NF_DECLS) ? scope_new(vm, sc) : sc, n->list, n->n);
    case N_IF:
        if (ct_truthy(eval(vm, sc, n->a))) return exec(vm, sc, n->b);
        return n->c ? exec(vm, sc, n->c) : C_NORMAL;
    case N_WHILE: case N_DO: {
        bool stop = false;
        if (n->k == N_DO) {
            int c = loop_body(vm, sc, n->b, &stop);
            if (stop) return c;
        }
        while (ct_truthy(eval(vm, sc, n->a))) {
            int c = loop_body(vm, sc, n->b, &stop);
            if (stop) return c;
        }
        return C_NORMAL;
    }
    case N_FOR: {
        scope_t *iter = scope_new(vm, sc);
        if (n->a) {
            if (n->a->k == N_VAR) declare_var(vm, iter, n->a);
            else eval(vm, iter, n->a);
        }
        bool per_turn = (n->flag & NF_HASFN) && n->a && n->a->k == N_VAR && iter->n;
        bool stop = false;
        for (;;) {
            if (n->b && !ct_truthy(eval(vm, iter, n->b))) break;
            int c = loop_body(vm, iter, n->d, &stop);
            if (stop) return c;
            if (per_turn) {
                /* the next turn gets its own copy of the loop's variables,
                 * so a closure made in this one keeps this turn's values */
                scope_t *nx = scope_new(vm, sc);
                for (uint32_t i = 0; i < iter->n; i++) ct_declare(vm, nx, iter->v[i].name, iter->v[i].v, iter->v[i].konst);
                iter = nx;
            }
            if (n->c) eval(vm, iter, n->c);
        }
        return C_NORMAL;
    }
    case N_FOROF: case N_FORIN: {
        ctv_t it = eval(vm, sc, n->b);
        node_t *target = n->a;
        node_t *pat = NULL;
        bool konst = false;
        if (target->k == N_VAR) {
            pat = target->list[0]->a;
            konst = (target->flag & NF_CONST) != 0;
        }
        uint32_t count;
        if (n->k == N_FOROF) {
            if (it.t == CT_ARR) count = it.a->n;
            else if (it.t == CT_STR) count = it.s->len;
            else if (it.t == CT_OBJ && ct_obj_find(it.o, "__entries", 9)) {
                it = *ct_obj_find(it.o, "__entries", 9);
                count = ct_len(it);
            } else ct_throw_err(vm, "TypeError", "%s is not iterable", ct_typeof(it));
        } else {
            if (it.t == CT_OBJ) count = it.o->n;
            else if (it.t == CT_ARR) count = it.a->n;
            else if (it.t == CT_STR) count = it.s->len;
            else count = 0;
        }
        bool stop = false;
        scope_t *turn = scope_new(vm, sc);
        for (uint32_t i = 0; i < count; i++) {
            ctv_t v;
            if (n->k == N_FOROF) {
                if (it.t == CT_ARR) {
                    if (i >= it.a->n) break;
                    v = it.a->v[i];
                } else {
                    v = vm_str(vm, it.s->p + i, 1);
                }
            } else if (it.t == CT_OBJ) {
                if (i >= it.o->n) break;
                if (it.o->kind != CT_K_PLAIN && it.o->p[i].k->len >= 2 && it.o->p[i].k->p[0] == '_' &&
                    it.o->p[i].k->p[1] == '_') {
                    continue;
                }
                v = strv_of(it.o->p[i].k);
            } else {
                char b[16];
                snprintf(b, sizeof b, "%u", (unsigned)i);
                v = vm_cstr(vm, b);
            }
            /* a fresh binding per turn only when a closure may keep it */
            if ((n->flag & NF_HASFN) && i) turn = scope_new(vm, sc);
            if (pat) {
                bind_pattern(vm, turn, pat, v, konst);
            } else {
                ref_t r = reference(vm, turn, target);
                ref_set(vm, &r, v);
            }
            int c = loop_body(vm, turn, n->c, &stop);
            if (stop) return c;
        }
        return C_NORMAL;
    }
    case N_RETURN:
        vm->ret = n->a ? eval(vm, sc, n->a) : ct_undef();
        return C_RETURN;
    case N_BREAK:
        return C_BREAK;
    case N_CONTINUE:
        return C_CONTINUE;
    case N_THROW:
        ct_throw(vm, eval(vm, sc, n->a));
    case N_TRY: {
        jmp_buf jb;
        jmp_buf *prev = vm->jb;
        int depth = vm->depth;
        ctv_t this_v = vm->this_v;
        volatile int c = C_NORMAL;
        volatile bool threw = false;
        ctv_t err = ct_undef();
        vm->jb = &jb;
        if (!setjmp(jb)) {
            c = exec(vm, sc, n->a);
            vm->jb = prev;
        } else {
            vm->jb = prev;
            vm->depth = depth;
            vm->this_v = this_v;
            vm->shorted = false;
            err = vm->thrown;
            threw = true;
            if (vm->ar->full && err.t == CT_NULL) {
                /* out of memory: not something a catch can do anything about */
                longjmp(*vm->jb, 1);
            }
            if (vm->ctx->stop && *vm->ctx->stop) longjmp(*vm->jb, 1);
        }
        if (threw && n->b) {
            threw = false;
            scope_t *cs = scope_new(vm, sc);
            if (n->d) bind_pattern(vm, cs, n->d, err, false);
            if (n->c) {
                /* the finally has to run even if the catch throws */
                jmp_buf jb2;
                vm->jb = &jb2;
                if (!setjmp(jb2)) {
                    c = exec(vm, cs, n->b);
                    vm->jb = prev;
                } else {
                    vm->jb = prev;
                    vm->depth = depth;
                    vm->this_v = this_v;
                    err = vm->thrown;
                    threw = true;
                }
            } else {
                c = exec(vm, cs, n->b);
            }
        }
        if (n->c) {
            int f = exec(vm, sc, n->c);
            if (f != C_NORMAL) return f;
        }
        if (threw) ct_throw(vm, err);
        return c;
    }
    case N_SWITCH: {
        ctv_t d = eval(vm, sc, n->a);
        scope_t *s = scope_new(vm, sc);
        int start = -1;
        for (int i = 0; i < n->n && start < 0; i++) {
            if (n->list[i]->a && ct_strict_eq(d, eval(vm, s, n->list[i]->a))) start = i;
        }
        if (start < 0) {
            for (int i = 0; i < n->n; i++) if (!n->list[i]->a) start = i;
        }
        if (start < 0) return C_NORMAL;
        for (int i = start; i < n->n; i++) {
            int c = exec_list(vm, s, n->list[i]->list, n->list[i]->n);
            if (c == C_BREAK) return C_NORMAL;
            if (c != C_NORMAL) return c;
        }
        return C_NORMAL;
    }
    default:
        eval(vm, sc, n);
        return C_NORMAL;
    }
}

/* ---- the globals: JSON, Math, Number, String, Object, Array, Date, Error... ----------------- */

static ctv_t g_json_parse(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    (void)ud;
    ct_str_t *s = vm_to_str(vm, arg(argc, argv, 0));
    ctv_t v;
    char err[160];
    if (!ct_json_parse(vm->ar, s->p, s->len, &v, err, sizeof err)) {
        if (vm->ar->full) oom(vm);
        ct_throw_err(vm, "SyntaxError", "%s", err);
    }
    return v;
}

static ctv_t g_json_stringify(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    (void)ud;
    ctv_t v = arg(argc, argv, 0);
    if (v.t == CT_UNDEF || v.t == CT_FN) return ct_undef();
    int indent = 0;
    ctv_t sp = arg(argc, argv, 2);
    if (sp.t == CT_NUM) indent = sp.n < 0 ? 0 : sp.n > 10 ? 10 : (int)sp.n;
    else if (sp.t == CT_STR && sp.s->len) indent = sp.s->p[0] == '\t' ? -1 : (int)(sp.s->len > 10 ? 10 : sp.s->len);
    ct_sb_t b = { 0 };
    ct_json_write(&b, v, indent);
    ctv_t r = b.oom ? ct_undef() : vm_str(vm, b.p ? b.p : "", b.n);
    ct_sb_free(&b);
    if (r.t == CT_UNDEF) oom(vm);
    return r;
}

enum { M_FLOOR = 1, M_CEIL, M_ROUND, M_TRUNC, M_ABS, M_MIN, M_MAX, M_POW, M_SQRT, M_RANDOM, M_SIGN, M_LOG, M_EXP,
       M_SIN, M_COS, M_LOG10, M_CBRT, M_HYPOT };

static ctv_t g_math(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    int m = (int)(intptr_t)ud;
    double x = arg_num(argc, argv, 0, 0.0 / 0.0);
    switch (m) {
    case M_FLOOR: return ct_num(d_floor(x));
    case M_CEIL: return ct_num(d_ceil(x));
    case M_ROUND: return ct_num(d_floor(x + 0.5));
    case M_TRUNC: return ct_num(d_trunc(x));
    case M_ABS: return ct_num(x < 0 ? -x : x);
    case M_SIGN: return ct_num(x > 0 ? 1 : x < 0 ? -1 : x);
    case M_MIN: case M_MAX: {
        double r = m == M_MIN ? 1.0 / 0.0 : -1.0 / 0.0;
        for (int i = 0; i < argc; i++) {
            double v = ct_to_num(argv[i]);
            if (v != v) return ct_num(v);
            if (m == M_MIN ? v < r : v > r) r = v;
        }
        return ct_num(r);
    }
    case M_POW: return ct_num(d_pow(x, arg_num(argc, argv, 1, 0.0 / 0.0)));
    case M_SQRT: return ct_num(d_sqrt(x));
    case M_RANDOM: return ct_num((double)(ct_rand(vm) >> 8) / 16777216.0);
    case M_LOG: return ct_num((double)logf((float)x));
    case M_LOG10: return ct_num((double)log10f((float)x));
    case M_EXP: return ct_num((double)expf((float)x));
    case M_SIN: return ct_num((double)sinf((float)x));
    case M_COS: return ct_num((double)cosf((float)x));
    case M_CBRT: return ct_num(x < 0 ? -(double)powf((float)-x, 1.0f / 3) : (double)powf((float)x, 1.0f / 3));
    case M_HYPOT: {
        double s = 0;
        for (int i = 0; i < argc; i++) {
            double v = ct_to_num(argv[i]);
            s += v * v;
        }
        return ct_num(d_sqrt(s));
    }
    }
    return ct_undef();
}

static ctv_t g_parse_int(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    (void)ud;
    ct_str_t *s = vm_to_str(vm, arg(argc, argv, 0));
    int radix = (int)arg_num(argc, argv, 1, 0);
    const char *p = s->p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    bool neg = false;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((radix == 0 || radix == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        radix = 16;
    }
    if (radix == 0) radix = 10;
    if (radix < 2 || radix > 36) return ct_num(0.0 / 0.0);
    double v = 0;
    bool any = false;
    for (; *p; p++) {
        int d;
        char c = *p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= radix) break;
        v = v * radix + d;
        any = true;
    }
    if (!any) return ct_num(0.0 / 0.0);
    return ct_num(neg ? -v : v);
}

static ctv_t g_parse_float(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    (void)ud;
    ct_str_t *s = vm_to_str(vm, arg(argc, argv, 0));
    const char *p = s->p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!strncmp(p, "Infinity", 8) || !strncmp(p, "+Infinity", 9)) return ct_num(1.0 / 0.0);
    if (!strncmp(p, "-Infinity", 9)) return ct_num(-1.0 / 0.0);
    char *end;
    double d = strtod(p, &end);
    if (end == p) return ct_num(0.0 / 0.0);
    return ct_num(d);
}

enum { G_NUMBER = 1, G_STRING, G_BOOLEAN, G_ISNAN, G_ISFINITE, G_ISINTEGER, G_ISARRAY, G_KEYS, G_VALUES, G_ENTRIES,
       G_ASSIGN, G_FROMENTRIES, G_ERROR, G_TYPEERROR, G_RANGEERROR, G_SYNTAXERROR, G_REFERENCEERROR, G_ARRAY,
       G_ARRAY_FROM, G_ARRAY_OF, G_OBJECT, G_SETTIMEOUT, G_REQUIRE, G_FREEZE, G_ISSAFE };

static ctv_t g_misc(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    int m = (int)(intptr_t)ud;
    ctv_t a = arg(argc, argv, 0);
    switch (m) {
    case G_NUMBER: return ct_num(argc ? ct_to_num(a) : 0);
    case G_STRING: return argc ? strv_of(vm_to_str(vm, a)) : vm_cstr(vm, "");
    case G_BOOLEAN: return ct_bool(ct_truthy(a));
    case G_ISNAN: { double d = ct_to_num(a); return ct_bool(d != d); }
    case G_ISFINITE: { double d = ct_to_num(a); return ct_bool(d == d && d < 1.7e308 && d > -1.7e308); }
    case G_ISINTEGER: return ct_bool(a.t == CT_NUM && a.n == d_trunc(a.n) && a.n < 1.7e308 && a.n > -1.7e308);
    case G_ISSAFE: return ct_bool(a.t == CT_NUM && a.n == d_trunc(a.n) && a.n <= 9007199254740991.0 && a.n >= -9007199254740991.0);
    case G_ISARRAY: return ct_bool(a.t == CT_ARR);
    case G_KEYS: case G_VALUES: case G_ENTRIES: {
        ctv_t r = vm_arr(vm);
        if (a.t == CT_OBJ) {
            for (uint32_t i = 0; i < a.o->n; i++) {
                ct_str_t *k = a.o->p[i].k;
                if (a.o->kind != CT_K_PLAIN && k->len >= 2 && k->p[0] == '_' && k->p[1] == '_') continue;
                if (m == G_KEYS) vm_push(vm, r, strv_of(k));
                else if (m == G_VALUES) vm_push(vm, r, a.o->p[i].v);
                else {
                    ctv_t e = vm_arr(vm);
                    vm_push(vm, e, strv_of(k));
                    vm_push(vm, e, a.o->p[i].v);
                    vm_push(vm, r, e);
                }
            }
        } else if (a.t == CT_ARR || a.t == CT_STR) {
            uint32_t n = a.t == CT_ARR ? a.a->n : a.s->len;
            for (uint32_t i = 0; i < n; i++) {
                char b[16];
                snprintf(b, sizeof b, "%u", (unsigned)i);
                ctv_t val = a.t == CT_ARR ? a.a->v[i] : vm_str(vm, a.s->p + i, 1);
                if (m == G_KEYS) vm_push(vm, r, vm_cstr(vm, b));
                else if (m == G_VALUES) vm_push(vm, r, val);
                else {
                    ctv_t e = vm_arr(vm);
                    vm_push(vm, e, vm_cstr(vm, b));
                    vm_push(vm, e, val);
                    vm_push(vm, r, e);
                }
            }
        }
        return r;
    }
    case G_ASSIGN: {
        if (a.t != CT_OBJ) return a;
        for (int i = 1; i < argc; i++) {
            if (argv[i].t != CT_OBJ) continue;
            for (uint32_t k = 0; k < argv[i].o->n; k++) vm_set(vm, a, argv[i].o->p[k].k->p, argv[i].o->p[k].v);
        }
        return a;
    }
    case G_FROMENTRIES: {
        ctv_t r = vm_obj(vm);
        for (uint32_t i = 0; i < ct_len(a); i++) {
            ctv_t e = a.a->v[i];
            ct_str_t *k = vm_to_str(vm, ct_at(e, 0));
            if (!ct_obj_set(vm->ar, r.o, k->p, k->len, ct_at(e, 1))) oom(vm);
        }
        return r;
    }
    case G_FREEZE: return a;
    case G_ERROR: case G_TYPEERROR: case G_RANGEERROR: case G_SYNTAXERROR: case G_REFERENCEERROR: {
        static const char *const NAMES[] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError" };
        const char *msg = a.t == CT_UNDEF ? "" : vm_to_str(vm, a)->p;
        return ct_make_error(vm, NAMES[m - G_ERROR], msg);
    }
    case G_ARRAY: {
        ctv_t r = vm_arr(vm);
        if (argc == 1 && a.t == CT_NUM) {
            if (a.n < 0 || a.n > (1 << 20) || a.n != d_trunc(a.n)) ct_throw_err(vm, "RangeError", "Invalid array length");
            for (uint32_t i = 0; i < (uint32_t)a.n; i++) vm_push(vm, r, ct_undef());
        } else {
            for (int i = 0; i < argc; i++) vm_push(vm, r, argv[i]);
        }
        return r;
    }
    case G_ARRAY_FROM: {
        ctv_t r = vm_arr(vm);
        ctv_t fn = arg(argc, argv, 1);
        uint32_t n = 0;
        if (a.t == CT_ARR) n = a.a->n;
        else if (a.t == CT_STR) n = a.s->len;
        else if (a.t == CT_OBJ) n = (uint32_t)ct_to_num(ct_get(a, "length"));
        for (uint32_t i = 0; i < n && i < (1 << 20); i++) {
            ctv_t v = a.t == CT_ARR ? a.a->v[i] : a.t == CT_STR ? vm_str(vm, a.s->p + i, 1) : ct_undef();
            if (fn.t == CT_FN) {
                ctv_t av[2] = { v, ct_num(i) };
                v = ct_call(vm, fn, ct_undef(), 2, av);
            }
            vm_push(vm, r, v);
        }
        return r;
    }
    case G_ARRAY_OF: {
        ctv_t r = vm_arr(vm);
        for (int i = 0; i < argc; i++) vm_push(vm, r, argv[i]);
        return r;
    }
    case G_OBJECT:
        return (a.t == CT_OBJ || a.t == CT_ARR) ? a : vm_obj(vm);
    case G_SETTIMEOUT: {
        if (a.t == CT_FN) vm_push(vm, vm->timeouts, a);
        return ct_num(ct_len(vm->timeouts));
    }
    case G_REQUIRE:
        ct_throw_err(vm, "Error", "require('%s'): there are no modules on the board", a.t == CT_STR ? a.s->p : "");
    }
    return ct_undef();
}

/* ---- Date: only what scripts use (now, a timestamp, ISO text) ---------------------------------- */

static void iso_of(double ms, char out[48])
{
    if (ms != ms) {
        strcpy(out, "Invalid Date");
        return;
    }
    time_t t = (time_t)(int64_t)d_floor(ms / 1000);
    int frac = (int)(ms - (double)(int64_t)t * 1000);
    if (frac < 0) frac = 0;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, 48, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, frac);
}

/* days since 1970 of a civil date (Howard Hinnant's algorithm) */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static double parse_iso(const char *s)
{
    int y = 0, mo = 1, d = 1, h = 0, mi = 0, sec = 0, ms = 0;
    int n = sscanf(s, "%d-%d-%d", &y, &mo, &d);
    if (n < 1) return 0.0 / 0.0;
    const char *t = strchr(s, 'T');
    if (!t) t = strchr(s, ' ');
    if (t) {
        sscanf(t + 1, "%d:%d:%d", &h, &mi, &sec);
        const char *dot = strchr(t, '.');
        if (dot) {
            int k = 0;
            for (const char *p = dot + 1; *p >= '0' && *p <= '9'; p++, k++) if (k < 3) ms = ms * 10 + (*p - '0');
            while (k < 3) { ms *= 10; k++; }
        }
    }
    double v = (double)days_from_civil(y, mo, d) * 86400000.0 + h * 3600000.0 + mi * 60000.0 + sec * 1000.0 + ms;
    const char *z = t ? t + 1 : s;
    const char *off = strpbrk(z, "+-");
    if (t && off) {
        int oh = 0, om = 0;
        sscanf(off + 1, "%d:%d", &oh, &om);
        double o = (oh * 60 + om) * 60000.0;
        v += *off == '+' ? -o : o;
    }
    return v;
}

static ctv_t date_method(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud);

static ctv_t make_date(ct_vm_t *vm, double ms)
{
    ctv_t d = vm_obj(vm);
    d.o->kind = CT_K_DATE;
    char iso[48];
    iso_of(ms, iso);
    vm_set(vm, d, "__t", ct_num(ms));
    vm_set(vm, d, "__iso", vm_cstr(vm, iso));
    static const char *const M[] = { "getTime", "valueOf", "toISOString", "toJSON", "toString", "getFullYear",
                                     "getMonth", "getDate", "getDay", "getHours", "getMinutes", "getSeconds",
                                     "getMilliseconds", "toUTCString", "toLocaleString", "toLocaleDateString",
                                     "toLocaleTimeString", "getUTCFullYear", "getUTCMonth", "getUTCDate",
                                     "getUTCHours", "getUTCMinutes", "getUTCSeconds", "getTimezoneOffset" };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) vm_method(vm, d, M[i], date_method, (void *)(intptr_t)i);
    return d;
}

static ctv_t date_method(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)argc;
    (void)argv;
    double ms = ct_to_num(self);
    int m = (int)(intptr_t)ud;
    if (m <= 1) return ct_num(ms);
    char iso[48];
    iso_of(ms, iso);
    if (m <= 4 || m >= 13) {
        if (m == 23) return ct_num(0);
        if (m >= 17 && m <= 22) goto parts;
        return vm_cstr(vm, iso);
    }
parts:
    if (ms != ms) return ct_num(0.0 / 0.0);
    {
        time_t t = (time_t)(int64_t)d_floor(ms / 1000);
        struct tm tm;
        gmtime_r(&t, &tm);
        switch (m) {
        case 5: case 17: return ct_num(tm.tm_year + 1900);
        case 6: case 18: return ct_num(tm.tm_mon);
        case 7: case 19: return ct_num(tm.tm_mday);
        case 8: return ct_num(tm.tm_wday);
        case 9: case 20: return ct_num(tm.tm_hour);
        case 10: case 21: return ct_num(tm.tm_min);
        case 11: case 22: return ct_num(tm.tm_sec);
        case 12: return ct_num(ms - d_floor(ms / 1000) * 1000);
        }
    }
    return ct_undef();
}

static ctv_t g_date(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    int m = (int)(intptr_t)ud;
    if (m == 1) return ct_num(ct_now_ms());                       /* Date.now() */
    if (m == 2) return ct_num(parse_iso(vm_to_str(vm, arg(argc, argv, 0))->p));   /* Date.parse */
    double ms;
    if (argc == 0) ms = ct_now_ms();
    else if (argv[0].t == CT_NUM) ms = argv[0].n;
    else if (argv[0].t == CT_STR) ms = parse_iso(argv[0].s->p);
    else if (argv[0].t == CT_OBJ && argv[0].o->kind == CT_K_DATE) ms = ct_to_num(argv[0]);
    else ms = 0.0 / 0.0;
    if (argc >= 2 && argv[0].t == CT_NUM) {
        /* new Date(y, m, d, h, mi, s, ms), as UTC: the board keeps no zone */
        int y = (int)argv[0].n, mo = (int)arg_num(argc, argv, 1, 0), d = (int)arg_num(argc, argv, 2, 1);
        ms = (double)days_from_civil(y, mo + 1, d) * 86400000.0 + arg_num(argc, argv, 3, 0) * 3600000.0 +
             arg_num(argc, argv, 4, 0) * 60000.0 + arg_num(argc, argv, 5, 0) * 1000.0 + arg_num(argc, argv, 6, 0);
    }
    if (m == 3) {                                                  /* Date() called plainly: a text */
        char iso[48];
        iso_of(ct_now_ms(), iso);
        return vm_cstr(vm, iso);
    }
    return make_date(vm, ms);
}

/* ---- URIs and base64 ------------------------------------------------------------------------ */

static ctv_t g_uri(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    int m = (int)(intptr_t)ud;
    ct_str_t *s = vm_to_str(vm, arg(argc, argv, 0));
    ct_sb_t b = { 0 };
    if (m == 1 || m == 3) {         /* encodeURIComponent / encodeURI */
        static const char HEX[] = "0123456789ABCDEF";
        for (uint32_t i = 0; i < s->len; i++) {
            unsigned char c = (unsigned char)s->p[i];
            bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        strchr("-_.!~*'()", (char)c);
            if (m == 3 && c && strchr(";,/?:@&=+$#", (char)c)) keep = true;
            if (keep && c) {
                ct_sb_addc(&b, (char)c);
            } else {
                char e[3] = { '%', HEX[c >> 4], HEX[c & 15] };
                ct_sb_add(&b, e, 3);
            }
        }
    } else {                        /* decodeURIComponent / decodeURI */
        for (uint32_t i = 0; i < s->len; i++) {
            if (s->p[i] == '%' && i + 2 < s->len + 0 && hexv(s->p[i + 1]) >= 0 && hexv(s->p[i + 2]) >= 0) {
                ct_sb_addc(&b, (char)(hexv(s->p[i + 1]) * 16 + hexv(s->p[i + 2])));
                i += 2;
            } else if (s->p[i] == '%') {
                ct_sb_free(&b);
                ct_throw_err(vm, "URIError", "URI malformed");
            } else {
                ct_sb_addc(&b, s->p[i]);
            }
        }
    }
    ctv_t r = b.oom ? ct_undef() : vm_str(vm, b.p ? b.p : "", b.n);
    ct_sb_free(&b);
    if (r.t == CT_UNDEF) oom(vm);
    return r;
}

static ctv_t g_b64(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud)
{
    (void)self;
    ct_str_t *s = vm_to_str(vm, arg(argc, argv, 0));
    if (ud) {                       /* btoa */
        size_t n = (s->len + 2) / 3 * 4 + 8;
        char *o = vm_alloc(vm, n);
        size_t k = ct_b64(o, n, s->p, s->len);
        return vm_str(vm, o, k);
    }
    uint8_t *o = vm_alloc(vm, s->len + 1);
    int k = ct_b64_dec(o, s->len + 1, s->p);
    if (k < 0) ct_throw_err(vm, "InvalidCharacterError", "The string to be decoded is not correctly encoded.");
    return vm_str(vm, (const char *)o, (size_t)k);
}

/* a global that is a function with properties of its own (Number.isInteger,
 * Object.keys): an object that answers calls through __call */
static ctv_t callable(ct_vm_t *vm, const char *tag, ct_native_t f, void *ud)
{
    ctv_t o = vm_obj(vm);
    o.o->kind = CT_K_SYS;
    vm_set(vm, o, "__tag", vm_cstr(vm, tag));
    if (f) vm_set(vm, o, "__call", vm_fn(vm, f, ud, tag));
    return o;
}

static void global(ct_vm_t *vm, const char *name, ctv_t v)
{
    ct_declare(vm, vm->global, ct_intern(vm, name, strlen(name)), v, false);
}

static void install_globals(ct_vm_t *vm)
{
    ctv_t json = callable(vm, "JSON", NULL, NULL);
    vm_method(vm, json, "parse", g_json_parse, NULL);
    vm_method(vm, json, "stringify", g_json_stringify, NULL);
    global(vm, "JSON", json);

    ctv_t math = callable(vm, "Math", NULL, NULL);
    static const struct { const char *n; int m; } MF[] = {
        { "floor", M_FLOOR }, { "ceil", M_CEIL }, { "round", M_ROUND }, { "trunc", M_TRUNC }, { "abs", M_ABS },
        { "min", M_MIN }, { "max", M_MAX }, { "pow", M_POW }, { "sqrt", M_SQRT }, { "random", M_RANDOM },
        { "sign", M_SIGN }, { "log", M_LOG }, { "log10", M_LOG10 }, { "exp", M_EXP }, { "sin", M_SIN },
        { "cos", M_COS }, { "cbrt", M_CBRT }, { "hypot", M_HYPOT },
    };
    for (size_t i = 0; i < sizeof MF / sizeof MF[0]; i++) vm_method(vm, math, MF[i].n, g_math, (void *)(intptr_t)MF[i].m);
    vm_set(vm, math, "PI", ct_num(3.141592653589793));
    vm_set(vm, math, "E", ct_num(2.718281828459045));
    global(vm, "Math", math);

    ctv_t number = callable(vm, "Number", g_misc, (void *)G_NUMBER);
    vm_method(vm, number, "isInteger", g_misc, (void *)G_ISINTEGER);
    vm_method(vm, number, "isSafeInteger", g_misc, (void *)G_ISSAFE);
    vm_method(vm, number, "isNaN", g_misc, (void *)G_ISNAN);
    vm_method(vm, number, "isFinite", g_misc, (void *)G_ISFINITE);
    vm_method(vm, number, "parseFloat", g_parse_float, NULL);
    vm_method(vm, number, "parseInt", g_parse_int, NULL);
    vm_set(vm, number, "MAX_SAFE_INTEGER", ct_num(9007199254740991.0));
    vm_set(vm, number, "MIN_SAFE_INTEGER", ct_num(-9007199254740991.0));
    vm_set(vm, number, "EPSILON", ct_num(2.220446049250313e-16));
    global(vm, "Number", number);
    global(vm, "String", callable(vm, "String", g_misc, (void *)G_STRING));
    global(vm, "Boolean", callable(vm, "Boolean", g_misc, (void *)G_BOOLEAN));
    global(vm, "Function", callable(vm, "Function", NULL, NULL));

    ctv_t object = callable(vm, "Object", g_misc, (void *)G_OBJECT);
    vm_method(vm, object, "keys", g_misc, (void *)G_KEYS);
    vm_method(vm, object, "values", g_misc, (void *)G_VALUES);
    vm_method(vm, object, "entries", g_misc, (void *)G_ENTRIES);
    vm_method(vm, object, "assign", g_misc, (void *)G_ASSIGN);
    vm_method(vm, object, "fromEntries", g_misc, (void *)G_FROMENTRIES);
    vm_method(vm, object, "freeze", g_misc, (void *)G_FREEZE);
    global(vm, "Object", object);

    ctv_t array = callable(vm, "Array", g_misc, (void *)G_ARRAY);
    vm_method(vm, array, "isArray", g_misc, (void *)G_ISARRAY);
    vm_method(vm, array, "from", g_misc, (void *)G_ARRAY_FROM);
    vm_method(vm, array, "of", g_misc, (void *)G_ARRAY_OF);
    global(vm, "Array", array);

    static const char *const ERRS[] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError" };
    for (int i = 0; i < 5; i++) global(vm, ERRS[i], callable(vm, "Error", g_misc, (void *)(intptr_t)(G_ERROR + i)));

    ctv_t date = callable(vm, "Date", g_date, (void *)3);
    vm_set(vm, date, "__new", vm_fn(vm, g_date, (void *)0, "Date"));
    vm_method(vm, date, "now", g_date, (void *)1);
    vm_method(vm, date, "parse", g_date, (void *)2);
    global(vm, "Date", date);

    global(vm, "parseInt", vm_fn(vm, g_parse_int, NULL, "parseInt"));
    global(vm, "parseFloat", vm_fn(vm, g_parse_float, NULL, "parseFloat"));
    global(vm, "isNaN", vm_fn(vm, g_misc, (void *)G_ISNAN, "isNaN"));
    global(vm, "isFinite", vm_fn(vm, g_misc, (void *)G_ISFINITE, "isFinite"));
    global(vm, "encodeURIComponent", vm_fn(vm, g_uri, (void *)1, "encodeURIComponent"));
    global(vm, "decodeURIComponent", vm_fn(vm, g_uri, (void *)2, "decodeURIComponent"));
    global(vm, "encodeURI", vm_fn(vm, g_uri, (void *)3, "encodeURI"));
    global(vm, "decodeURI", vm_fn(vm, g_uri, (void *)4, "decodeURI"));
    global(vm, "btoa", vm_fn(vm, g_b64, (void *)1, "btoa"));
    global(vm, "atob", vm_fn(vm, g_b64, NULL, "atob"));
    global(vm, "setTimeout", vm_fn(vm, g_misc, (void *)G_SETTIMEOUT, "setTimeout"));
    global(vm, "require", vm_fn(vm, g_misc, (void *)G_REQUIRE, "require"));
    global(vm, "undefined", ct_undef());
    global(vm, "NaN", ct_num(0.0 / 0.0));
    global(vm, "Infinity", ct_num(1.0 / 0.0));
}

/* ---- running a script --------------------------------------------------------------------------- */

static void report(ct_vm_t *vm, ctv_t err)
{
    ct_script_t *ctx = vm->ctx;
    if (vm->ar->full) {
        snprintf(ctx->error, sizeof ctx->error, "RangeError: out of memory (line %d)", vm->line);
        return;
    }
    if (err.t == CT_OBJ && err.o->kind == CT_K_ERROR) {
        ctv_t nm = ct_get(err, "name"), msg = ct_get(err, "message");
        snprintf(ctx->error, sizeof ctx->error, "%s: %.200s (line %d)", nm.t == CT_STR ? nm.s->p : "Error",
                 ct_s(msg), vm->line);
        return;
    }
    ct_sb_t b = { 0 };
    ct_inspect(&b, err, 0);
    snprintf(ctx->error, sizeof ctx->error, "Uncaught %.200s (line %d)", b.p ? b.p : "?", vm->line);
    ct_sb_free(&b);
}

bool ct_js_run(ct_script_t *ctx, const char *src)
{
    ctx->error[0] = '\0';
    if (!src) return true;
    const char *p = src;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (!*p) return true;

    /* the machine lives in the arena: its jmp_buf and all, nothing on the
     * C stack survives a script */
    ct_vm_t *vm = ct_alloc(ctx->ar, sizeof *vm);
    if (!vm) {
        snprintf(ctx->error, sizeof ctx->error, "RangeError: out of memory");
        return false;
    }
    vm->ar = ctx->ar;
    vm->ctx = ctx;
    vm->t0 = aos_hal_uptime_ms();
    vm->rnd = (uint32_t)aos_hal_uptime_us() | 1;
    vm->shorted = false;

    jmp_buf top;
    vm->jb = &top;
    if (setjmp(top)) {
        report(vm, vm->thrown);
        return false;
    }
    vm->global = scope_new(vm, NULL);
    vm->timeouts = vm_arr(vm);
    install_globals(vm);
    ct_pm_install(vm);

    ps_t ps = { .vm = vm, .p = src, .end = src + strlen(src), .line = 1 };
    next(&ps);
    nlist_t prog = { 0 };
    while (ps.tk.t != TK_EOF) nl_add(&ps, &prog, statement(&ps));

    scope_t *sc = scope_new(vm, vm->global);
    vm->this_v = ct_undef();
    int c = exec_list(vm, sc, prog.v, prog.n);
    (void)c;
    for (uint32_t i = 0; i < ct_len(vm->timeouts); i++) ct_call(vm, vm->timeouts.a->v[i], ct_undef(), 0, NULL);
    ct_pm_finish(vm);
    return true;
}
