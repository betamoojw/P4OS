/*
 * P4OS - CAS: memory, strings, UTF-8 and the fonts.
 *
 * The maths is drawn with the firmware's Inter, which carries Greek, the
 * arrows and the Mathematical Operators block (components/aos_fonts): no
 * font of its own to load.
 */
#include "mx.h"

#include "aos_fonts.h"

#include <stdlib.h>
#include <string.h>

#ifndef AOS_SIM
#include "esp_heap_caps.h"
#endif

/* --- memory --------------------------------------------------------------- */

void *mx_alloc(size_t n)
{
    if (!n) {
        n = 1;
    }
#ifdef AOS_SIM
    return calloc(1, n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(n);
    }
    if (p) {
        memset(p, 0, n);
    }
    return p;
#endif
}

void *mx_realloc(void *p, size_t n)
{
#ifdef AOS_SIM
    return realloc(p, n);
#else
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
#endif
}

void mx_free(void *p)
{
    free(p);
}

char *mx_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = mx_alloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

void mx_str_addn(mx_str_t *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->n + n + 1) {
            cap *= 2;
        }
        char *q = mx_realloc(b->s, cap);
        if (!q) {
            return;
        }
        b->s = q;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

void mx_str_add(mx_str_t *b, const char *s)  { mx_str_addn(b, s, strlen(s)); }
void mx_str_addc(mx_str_t *b, char c)        { mx_str_addn(b, &c, 1); }

void mx_str_free(mx_str_t *b)
{
    mx_free(b->s);
    b->s = NULL;
    b->n = b->cap = 0;
}

char *mx_str_take(mx_str_t *b)
{
    char *s = b->s ? b->s : mx_strdup("");
    b->s = NULL;
    b->n = b->cap = 0;
    return s;
}

/* --- UTF-8 ---------------------------------------------------------------- */

uint32_t mx_utf8_next(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    int n = 1;
    if (c >= 0xF0 && s[1] && s[2] && s[3]) {
        c = ((c & 7) << 18) | ((s[1] & 63u) << 12) | ((s[2] & 63u) << 6) | (s[3] & 63u);
        n = 4;
    } else if (c >= 0xE0 && s[1] && s[2]) {
        c = ((c & 15) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u);
        n = 3;
    } else if (c >= 0xC0 && s[1]) {
        c = ((c & 31) << 6) | (s[1] & 63u);
        n = 2;
    }
    *p += n;
    return c;
}

int mx_text_width(const lv_font_t *f, const char *s)
{
    int w = 0;
    while (*s) {
        uint32_t c = mx_utf8_next(&s);
        const char *q = s;
        uint32_t next = *q ? mx_utf8_next(&q) : 0;
        w += lv_font_get_glyph_width(f, c, next);
    }
    return w;
}

/* --- fonts ------------------------------------------------------------------ */

const lv_font_t *mx_font(int px)
{
    if (px <= 22) return &aos_inter_20;
    if (px <= 26) return &aos_inter_24;
    if (px <= 30) return &aos_inter_28;
    if (px <= 40) return &aos_inter_32;
    return &aos_inter_48;
}

bool mx_font_has(uint32_t cp, int px)
{
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(mx_font(px), &g, cp, 0) && !g.is_placeholder;
}
