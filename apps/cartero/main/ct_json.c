/*
 * P4OS - Cartero: JSON in and out of the values (ct.h).
 *
 * The parser keeps the containers it is inside on a stack of its own, not
 * on the C stack: the UI's thread reads collections with it, and a response
 * nested a few thousand levels deep must not take the thread down. The
 * messages are JavaScript's (SyntaxError's words), since a script's
 * JSON.parse gives them too.
 */
#include "ct.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    ctv_t     c;
    ct_str_t *key;
} frame_t;

typedef struct {
    const char *s, *p, *end;
    ct_arena_t *a;
    char       *err;
    size_t      errn;
} jp_t;

static void fail(jp_t *j, const char *why)
{
    if (!j->err || !j->errn) return;
    int line = 1, col = 1;
    for (const char *q = j->s; q < j->p && q < j->end; q++) {
        if (*q == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    if (j->p >= j->end) snprintf(j->err, j->errn, "Unexpected end of JSON input (%s)", why);
    else snprintf(j->err, j->errn, "%s at line %d, column %d", why, line, col);
}

static void ws(jp_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static int utf8_put(char *o, uint32_t cp)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* at the opening quote; the decoded text is never longer than the raw one */
static ct_str_t *str(jp_t *j)
{
    const char *q = j->p + 1;
    while (q < j->end && *q != '"') {
        if (*q == '\\') q++;
        q++;
    }
    if (q >= j->end) {
        j->p = j->end;
        fail(j, "Unterminated string");
        return NULL;
    }
    size_t raw = (size_t)(q - j->p - 1);
    ct_str_t *s = ct_alloc(j->a, sizeof(ct_str_t) + raw + 1);
    if (!s) {
        fail(j, "Out of memory");
        return NULL;
    }
    char *o = s->p;
    const char *p = j->p + 1;
    while (p < q) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) {
            j->p = p;
            fail(j, "Bad control character in string literal");
            return NULL;
        }
        if (c != '\\') {
            *o++ = (char)c;
            p++;
            continue;
        }
        p++;
        switch (*p) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            int cp = q - p > 4 ? hex4(p + 1) : -1;
            if (cp < 0) {
                j->p = p;
                fail(j, "Bad Unicode escape");
                return NULL;
            }
            p += 4;
            uint32_t u = (uint32_t)cp;
            if (u >= 0xD800 && u < 0xDC00 && q - p > 6 && p[1] == '\\' && p[2] == 'u') {
                int lo = hex4(p + 3);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                    p += 6;
                }
            }
            o += utf8_put(o, u);
            break;
        }
        default:
            j->p = p;
            fail(j, "Bad escaped character");
            return NULL;
        }
        p++;
    }
    *o = '\0';
    s->len = (uint32_t)(o - s->p);
    j->p = q + 1;
    return s;
}

static bool scalar(jp_t *j, ctv_t *out)
{
    char c = *j->p;
    if (c == '"') {
        ct_str_t *s = str(j);
        if (!s) return false;
        out->t = CT_STR;
        out->s = s;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *q = j->p;
        if (*q == '-') q++;
        if (q >= j->end || *q < '0' || *q > '9') {
            fail(j, "No number after minus sign");
            return false;
        }
        while (q < j->end && ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' ||
                              *q == '-')) {
            q++;
        }
        char tmp[64];
        size_t n = (size_t)(q - j->p);
        if (n >= sizeof tmp) n = sizeof tmp - 1;
        memcpy(tmp, j->p, n);
        tmp[n] = '\0';
        char *e;
        double d = strtod(tmp, &e);
        if (*e) {
            j->p += e - tmp;
            fail(j, "Unexpected token in number");
            return false;
        }
        *out = ct_num(d);
        j->p = q;
        return true;
    }
    static const struct { const char *w; int n; ct_type_t t; bool b; } K[] = {
        { "true", 4, CT_BOOL, true }, { "false", 5, CT_BOOL, false }, { "null", 4, CT_NULL, false },
    };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
        if (j->end - j->p >= K[i].n && !memcmp(j->p, K[i].w, (size_t)K[i].n)) {
            *out = K[i].t == CT_BOOL ? ct_bool(K[i].b) : ct_null();
            j->p += K[i].n;
            return true;
        }
    }
    char why[48];
    snprintf(why, sizeof why, "Unexpected token %c", (c >= 0x20 && c < 0x7F) ? c : '?');
    fail(j, why);
    return false;
}

bool ct_json_parse(ct_arena_t *a, const char *s, size_t n, ctv_t *out, char *err, size_t errn)
{
    jp_t j = { .s = s, .p = s, .end = s + n, .a = a, .err = err, .errn = errn };
    if (err && errn) err[0] = '\0';
    *out = ct_undef();
    /* a byte order mark is skipped, as browsers do */
    if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) j.p += 3;

    frame_t *st = NULL;
    int depth = 0, cap = 0;
    bool ok = false;
    ctv_t v = ct_undef();

    ws(&j);
    for (;;) {
        /* a value starts here */
        if (j.p >= j.end) {
            fail(&j, "a value is missing");
            goto done;
        }
        if (*j.p == '{' || *j.p == '[') {
            bool obj = *j.p == '{';
            if (depth == cap) {
                int nc = cap ? cap * 2 : 32;
                frame_t *ns = ct_psram_realloc(st, (size_t)nc * sizeof(frame_t));
                if (!ns) {
                    fail(&j, "Out of memory");
                    goto done;
                }
                st = ns;
                cap = nc;
            }
            ctv_t c = obj ? ct_obj_new(a, 0) : ct_arr_new(a, 0);
            if (c.t == CT_UNDEF) {
                fail(&j, "Out of memory");
                goto done;
            }
            j.p++;
            ws(&j);
            if (j.p < j.end && *j.p == (obj ? '}' : ']')) {
                j.p++;
                v = c;
                goto got;
            }
            st[depth].c = c;
            st[depth].key = NULL;
            depth++;
            if (obj) {
                if (j.p >= j.end || *j.p != '"') {
                    fail(&j, "Expected a property name");
                    goto done;
                }
                st[depth - 1].key = str(&j);
                if (!st[depth - 1].key) goto done;
                ws(&j);
                if (j.p >= j.end || *j.p != ':') {
                    fail(&j, "Expected ':' after property name");
                    goto done;
                }
                j.p++;
                ws(&j);
            }
            continue;
        }
        if (!scalar(&j, &v)) goto done;
got:
        /* a value is complete: it goes into the container it is in */
        for (;;) {
            if (!depth) {
                ws(&j);
                if (j.p < j.end) {
                    fail(&j, "Unexpected non-whitespace character after JSON");
                    goto done;
                }
                *out = v;
                ok = true;
                goto done;
            }
            frame_t *f = &st[depth - 1];
            bool obj = f->c.t == CT_OBJ;
            if (obj) {
                if (!ct_obj_set(a, f->c.o, f->key->p, f->key->len, v)) {
                    fail(&j, "Out of memory");
                    goto done;
                }
            } else if (!ct_arr_push(a, f->c.a, v)) {
                fail(&j, "Out of memory");
                goto done;
            }
            ws(&j);
            if (j.p >= j.end) {
                fail(&j, obj ? "Expected ',' or '}'" : "Expected ',' or ']'");
                goto done;
            }
            if (*j.p == ',') {
                j.p++;
                ws(&j);
                if (obj) {
                    if (j.p >= j.end || *j.p != '"') {
                        fail(&j, "Expected a property name");
                        goto done;
                    }
                    f->key = str(&j);
                    if (!f->key) goto done;
                    ws(&j);
                    if (j.p >= j.end || *j.p != ':') {
                        fail(&j, "Expected ':' after property name");
                        goto done;
                    }
                    j.p++;
                    ws(&j);
                }
                break;              /* the next value */
            }
            if (*j.p == (obj ? '}' : ']')) {
                j.p++;
                v = f->c;
                depth--;
                continue;           /* the container is the value now */
            }
            fail(&j, obj ? "Expected ',' or '}'" : "Expected ',' or ']'");
            goto done;
        }
    }
done:
    free(st);
    return ok;
}

/* ---- writing ------------------------------------------------------------------------------------- */

void ct_json_str(ct_sb_t *b, const char *s, size_t n)
{
    static const char HEX[] = "0123456789abcdef";
    ct_sb_addc(b, '"');
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[7];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            if (c < 0x20) {
                u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
                u[4] = HEX[c >> 4]; u[5] = HEX[c & 15]; u[6] = '\0';
                esc = u;
            }
        }
        if (!esc) continue;
        if (i > run) ct_sb_add(b, s + run, i - run);
        ct_sb_adds(b, esc);
        run = i + 1;
    }
    if (n > run) ct_sb_add(b, s + run, n - run);
    ct_sb_addc(b, '"');
}

static void nl(ct_sb_t *b, int indent, int level)
{
    if (!indent) return;
    ct_sb_addc(b, '\n');
    for (int i = 0; i < level; i++) {
        if (indent < 0) ct_sb_addc(b, '\t');
        else for (int k = 0; k < indent; k++) ct_sb_addc(b, ' ');
    }
}

static void write_v(ct_sb_t *b, ctv_t v, int indent, int level)
{
    char n[40];
    if (level > 100) {
        ct_sb_adds(b, "null");
        return;
    }
    switch (v.t) {
    case CT_UNDEF: case CT_FN: case CT_NULL: ct_sb_adds(b, "null"); break;
    case CT_BOOL: ct_sb_adds(b, v.b ? "true" : "false"); break;
    case CT_NUM:
        if (v.n != v.n || v.n > 1.7976931348623157e308 || v.n < -1.7976931348623157e308) {
            ct_sb_adds(b, "null");
        } else {
            ct_num_fmt(v.n, n);
            ct_sb_adds(b, n);
        }
        break;
    case CT_STR: ct_json_str(b, v.s->p, v.s->len); break;
    case CT_ARR:
        ct_sb_addc(b, '[');
        for (uint32_t i = 0; i < v.a->n; i++) {
            if (i) ct_sb_addc(b, ',');
            nl(b, indent, level + 1);
            write_v(b, v.a->v[i], indent, level + 1);
        }
        if (v.a->n) nl(b, indent, level);
        ct_sb_addc(b, ']');
        break;
    case CT_OBJ: {
        if (v.o->kind == CT_K_DATE) {
            ctv_t *iso = ct_obj_find(v.o, "__iso", 5);
            if (iso && iso->t == CT_STR) ct_json_str(b, iso->s->p, iso->s->len);
            else ct_sb_adds(b, "null");
            break;
        }
        ct_sb_addc(b, '{');
        int k = 0;
        for (uint32_t i = 0; i < v.o->n; i++) {
            ctv_t x = v.o->p[i].v;
            if (x.t == CT_UNDEF || x.t == CT_FN) continue;
            if (v.o->p[i].k->len >= 2 && v.o->p[i].k->p[0] == '_' && v.o->p[i].k->p[1] == '_' &&
                v.o->kind != CT_K_PLAIN) {
                continue;           /* a system object's own bookkeeping */
            }
            if (k++) ct_sb_addc(b, ',');
            nl(b, indent, level + 1);
            ct_json_str(b, v.o->p[i].k->p, v.o->p[i].k->len);
            ct_sb_adds(b, indent ? ": " : ":");
            write_v(b, x, indent, level + 1);
        }
        if (k) nl(b, indent, level);
        ct_sb_addc(b, '}');
        break;
    }
    }
}

void ct_json_write(ct_sb_t *b, ctv_t v, int indent)
{
    if (v.t == CT_UNDEF || v.t == CT_FN) return;
    write_v(b, v, indent, 0);
}

/* Laid out again without building the values: a response of megabytes is
 * indented in one pass and a few bytes of state. */
bool ct_json_pretty(const char *s, size_t n, ct_sb_t *out, int indent)
{
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    if (i >= n || (s[i] != '{' && s[i] != '[')) return false;
    char stack[256];
    int level = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"') {
            size_t q = i + 1;
            while (q < n && s[q] != '"') {
                if (s[q] == '\\') q++;
                q++;
            }
            if (q >= n) return false;
            ct_sb_add(out, s + i, q - i + 1);
            i = q + 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
            continue;
        }
        if (c == '{' || c == '[') {
            if (level >= (int)sizeof stack) return false;
            stack[level++] = c == '{' ? '}' : ']';
            ct_sb_addc(out, c);
            size_t q = i + 1;
            while (q < n && (s[q] == ' ' || s[q] == '\t' || s[q] == '\n' || s[q] == '\r')) q++;
            if (q < n && s[q] == stack[level - 1]) {
                ct_sb_addc(out, s[q]);
                level--;
                i = q + 1;
                continue;
            }
            nl(out, indent, level);
            i++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (!level || stack[level - 1] != c) return false;
            level--;
            nl(out, indent, level);
            ct_sb_addc(out, c);
            i++;
            if (!level) {
                while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
                return i == n && !out->oom;
            }
            continue;
        }
        if (c == ',') {
            ct_sb_addc(out, ',');
            nl(out, indent, level);
            i++;
            continue;
        }
        if (c == ':') {
            ct_sb_adds(out, ": ");
            i++;
            continue;
        }
        size_t q = i;
        while (q < n && s[q] != ',' && s[q] != '}' && s[q] != ']' && s[q] != ' ' && s[q] != '\n' && s[q] != '\r' &&
               s[q] != '\t' && s[q] != ':') {
            q++;
        }
        ct_sb_add(out, s + i, q - i);
        i = q;
    }
    return false;
}
