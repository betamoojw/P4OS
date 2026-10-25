/*
 * P4OS - 3D printers: reading JSON by path.
 *
 * The machines answer with documents from a few hundred bytes (PrusaLink's
 * status) to tens of KB (a Bambu X1's full report, an OctoPrint file list),
 * and the drivers want a handful of values out of each. So there is no tree:
 * pj_get() walks the text to the value a path names and returns a pointer
 * into it, and the readers below turn that into a number or a string. A
 * value is skipped by counting brackets outside strings, which is all the
 * structure a skip needs.
 *
 * Numbers come back as float (the board's FPU is single precision, and a
 * temperature or a percentage needs no more). Some machines send numbers as
 * strings (Creality's "219.96", Bambu's fan "15"): pj_num() takes both.
 */
#include "p3d.h"

#include <stdlib.h>

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char *skip_str(const char *p)
{
    /* p at the opening quote */
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return *p ? p + 1 : p;
}

const char *pj_skip(const char *p)
{
    p = ws(p);
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = skip_str(p);
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return p;
    }
    /* a number, true, false, null */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') p++;
    return p;
}

/* the member 'key' (klen bytes) of the object at p, or the element number
 * key of the array at p */
static const char *child(const char *p, const char *key, int klen)
{
    p = ws(p);
    if (*p == '{') {
        p = ws(p + 1);
        while (*p == '"') {
            const char *k = p + 1;
            const char *ke = skip_str(p) - 1;
            p = ws(ke + 1);
            if (*p != ':') return NULL;
            p = ws(p + 1);
            if (ke - k == klen && !memcmp(k, key, (size_t)klen)) return p;
            p = ws(pj_skip(p));
            if (*p != ',') return NULL;
            p = ws(p + 1);
        }
        return NULL;
    }
    if (*p == '[') {
        int want = 0;
        for (int i = 0; i < klen; i++) {
            if (key[i] < '0' || key[i] > '9') return NULL;
            want = want * 10 + (key[i] - '0');
        }
        p = ws(p + 1);
        for (int i = 0; *p && *p != ']'; i++) {
            if (i == want) return p;
            p = ws(pj_skip(p));
            if (*p != ',') return NULL;
            p = ws(p + 1);
        }
    }
    return NULL;
}

const char *pj_get(const char *js, const char *path)
{
    if (!js) return NULL;
    const char *p = ws(js);
    while (p && path && *path) {
        const char *dot = strchr(path, '.');
        int klen = dot ? (int)(dot - path) : (int)strlen(path);
        p = child(p, path, klen);
        path = dot ? dot + 1 : NULL;
    }
    return p && *p ? p : NULL;
}

bool pj_null(const char *v)
{
    return !v || !strncmp(v, "null", 4);
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
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

bool pj_strv(const char *v, char *out, size_t n)
{
    if (!v || !n) return false;
    v = ws(v);
    if (*v != '"') {
        /* a number or a word: copied as it is written */
        const char *e = pj_skip(v);
        size_t k = (size_t)(e - v);
        if (!k || !strncmp(v, "null", 4)) return false;
        if (k >= n) k = n - 1;
        memcpy(out, v, k);
        out[k] = '\0';
        return true;
    }
    size_t o = 0;
    for (v++; *v && *v != '"';) {
        char tmp[4];
        size_t k = 1;
        if (*v == '\\' && v[1]) {
            v++;
            switch (*v) {
            case 'n': tmp[0] = '\n'; break;
            case 't': tmp[0] = '\t'; break;
            case 'r': tmp[0] = '\r'; break;
            case 'b': tmp[0] = '\b'; break;
            case 'f': tmp[0] = '\f'; break;
            case 'u': {
                uint32_t cp = 0;
                int i;
                for (i = 1; i <= 4 && hexv(v[i]) >= 0; i++) cp = cp * 16 + (uint32_t)hexv(v[i]);
                if (i <= 4) { tmp[0] = '?'; break; }
                v += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && v[1] == '\\' && v[2] == 'u') {
                    uint32_t lo = 0;
                    for (i = 3; i <= 6 && hexv(v[i]) >= 0; i++) lo = lo * 16 + (uint32_t)hexv(v[i]);
                    if (i > 6 && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        v += 6;
                    }
                }
                k = put_utf8(tmp, cp);
                break;
            }
            default: tmp[0] = *v; break;
            }
            v++;
        } else {
            tmp[0] = *v++;
        }
        if (o + k >= n) break;
        memcpy(out + o, tmp, k);
        o += k;
    }
    out[o] = '\0';
    return true;
}

bool pj_numv(const char *v, float *out)
{
    if (!v) return false;
    v = ws(v);
    if (*v == '"') v++;
    if (!strncmp(v, "true", 4)) { *out = 1; return true; }
    if (!strncmp(v, "false", 5)) { *out = 0; return true; }
    char *end;
    float f = strtof(v, &end);
    if (end == v) return false;
    *out = f;
    return true;
}

bool pj_num(const char *js, const char *path, float *out)
{
    return pj_numv(pj_get(js, path), out);
}

bool pj_int(const char *js, const char *path, int *out)
{
    float f;
    if (!pj_num(js, path, &f)) return false;
    *out = (int)(f < 0 ? f - 0.5f : f + 0.5f);
    return true;
}

bool pj_str(const char *js, const char *path, char *out, size_t n)
{
    const char *v = pj_get(js, path);
    if (pj_null(v)) return false;
    return pj_strv(v, out, n);
}

bool pj_bool(const char *js, const char *path, bool *out)
{
    float f;
    if (!pj_num(js, path, &f)) return false;
    *out = f != 0;
    return true;
}

bool pj_each(const char *v, pj_cb_t cb, void *ctx)
{
    if (!v) return false;
    const char *p = ws(v);
    if (*p == '{') {
        p = ws(p + 1);
        while (*p == '"') {
            const char *k = p + 1;
            const char *ke = skip_str(p) - 1;
            p = ws(ke + 1);
            if (*p != ':') return false;
            p = ws(p + 1);
            if (!cb(k, (int)(ke - k), p, ctx)) return true;
            p = ws(pj_skip(p));
            if (*p != ',') break;
            p = ws(p + 1);
        }
        return true;
    }
    if (*p == '[') {
        p = ws(p + 1);
        while (*p && *p != ']') {
            if (!cb(NULL, 0, p, ctx)) return true;
            p = ws(pj_skip(p));
            if (*p != ',') break;
            p = ws(p + 1);
        }
        return true;
    }
    return false;
}
