/*
 * P4OS - Python: the console's text.
 *
 * A scrolling container whose only child is a spacer as tall as all the
 * rows; its draw event paints the rows in view. Lines live in a ring (the
 * oldest go past TERM_LINES); each knows its kind and its text, and how
 * many rows it takes is worked out from the width, which a monospaced font
 * makes a division: a row is `cols` characters.
 */
#include "p4py_term.h"

#include "aos_theme.h"

#include <stdlib.h>
#include <string.h>

#define TERM_LINES  2000
#define LINE_MAX    4000            /* a line longer than this is cut into several */

typedef struct {
    char    *s;
    uint16_t len, cap;
    uint8_t  kind;
} line_t;

typedef struct {
    line_t    *l;                   /* TERM_LINES, a ring          */
    int        first, n;            /* the oldest, how many         */
    bool       open;                /* the last line has no newline yet */
    bool       in_err;              /* between 0x01 and 0x02        */
    lv_obj_t  *spacer;
    const lv_font_t *font;
    int        cw, rh, cols;        /* character width, row height, columns */
    int        rows;                /* all the rows, at this width  */
} term_t;

static term_t *get(lv_obj_t *o)
{
    return lv_obj_get_user_data(o);
}

static line_t *line_at(term_t *t, int i)
{
    return &t->l[(t->first + i) % TERM_LINES];
}

/* characters (not bytes) of UTF-8 */
static int u8len(const char *s, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
}

static int line_rows(const term_t *t, const line_t *l)
{
    int c = u8len(l->s, l->len);
    return c <= t->cols ? 1 : (c + t->cols - 1) / t->cols;
}

static void recount(term_t *t)
{
    t->rows = 0;
    for (int i = 0; i < t->n; i++) t->rows += line_rows(t, line_at(t, i));
    lv_obj_set_height(t->spacer, (t->rows + 1) * t->rh);
}

static void fit(lv_obj_t *o, term_t *t)
{
    int w = lv_obj_get_content_width(o);
    int cols = t->cw > 0 ? w / t->cw : 40;
    if (cols < 10) cols = 10;
    if (cols != t->cols) {
        t->cols = cols;
        recount(t);
    }
}

static void line_add(term_t *t, term_kind_t kind)
{
    if (t->n == TERM_LINES) {
        line_t *old = line_at(t, 0);
        t->rows -= line_rows(t, old);
        free(old->s);
        memset(old, 0, sizeof *old);
        t->first = (t->first + 1) % TERM_LINES;
        t->n--;
    }
    line_t *l = line_at(t, t->n++);
    memset(l, 0, sizeof *l);
    l->kind = (uint8_t)kind;
    t->rows += 1;
    t->open = true;
}

static void line_put(term_t *t, const char *s, size_t n)
{
    line_t *l = line_at(t, t->n - 1);
    int before = line_rows(t, l);
    if (l->len + n > LINE_MAX) n = LINE_MAX - l->len;
    if (l->len + n + 1 > l->cap) {
        size_t cap = l->cap ? l->cap : 64;
        while (cap < l->len + n + 1) cap *= 2;
        char *ns = realloc(l->s, cap);
        if (!ns) return;
        l->s = ns;
        l->cap = (uint16_t)cap;
    }
    memcpy(l->s + l->len, s, n);
    l->len += (uint16_t)n;
    l->s[l->len] = 0;
    t->rows += line_rows(t, l) - before;
}

void term_write(lv_obj_t *o, const char *s, size_t n, term_kind_t kind)
{
    term_t *t = get(o);
    if (!t) return;
    fit(o, t);
    /* at the bottom now: stay there after the new rows */
    bool follow = lv_obj_get_scroll_bottom(o) <= t->rh;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == 0x01) { t->in_err = true; continue; }
        if (c == 0x02) { t->in_err = false; continue; }
        term_kind_t k = t->in_err ? TERM_ERR : kind;
        if (c == '\r') continue;
        /* a line's colour is that of what started it (a prompt and what
         * was typed after it share one) */
        if (!t->open) line_add(t, k);
        else if (line_at(t, t->n - 1)->len == 0) line_at(t, t->n - 1)->kind = (uint8_t)k;
        if (c == '\n') {
            t->open = false;
            continue;
        }
        if (c == '\t') {
            line_t *l = line_at(t, t->n - 1);
            int col = u8len(l->s ? l->s : "", l->len);
            static const char SP[] = "        ";
            line_put(t, SP, 4 - col % 4);
            continue;
        }
        /* a run of plain bytes at once */
        size_t j = i;
        while (j < n && s[j] != '\n' && s[j] != '\r' && s[j] != '\t' && s[j] != 0x01 && s[j] != 0x02) j++;
        line_put(t, s + i, j - i);
        i = j - 1;
    }
    lv_obj_set_height(t->spacer, (t->rows + 1) * t->rh);
    lv_obj_invalidate(o);
    if (follow) {
        lv_obj_update_layout(o);
        lv_obj_scroll_to_y(o, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void term_clear(lv_obj_t *o)
{
    term_t *t = get(o);
    if (!t) return;
    for (int i = 0; i < t->n; i++) free(line_at(t, i)->s);
    memset(t->l, 0, sizeof(line_t) * TERM_LINES);
    t->first = t->n = t->rows = 0;
    t->open = t->in_err = false;
    lv_obj_set_height(t->spacer, t->rh);
    lv_obj_scroll_to_y(o, 0, LV_ANIM_OFF);
    lv_obj_invalidate(o);
}

char *term_text(lv_obj_t *o)
{
    term_t *t = get(o);
    if (!t) return NULL;
    size_t total = 1;
    for (int i = 0; i < t->n; i++) total += line_at(t, i)->len + 1;
    char *out = malloc(total), *p = out;
    if (!out) return NULL;
    for (int i = 0; i < t->n; i++) {
        line_t *l = line_at(t, i);
        if (l->len) memcpy(p, l->s, l->len);
        p += l->len;
        *p++ = '\n';
    }
    *p = 0;
    return out;
}

static uint32_t kind_color(int k)
{
    switch (k) {
    case TERM_IN:   return 0x64D2FF;
    case TERM_ERR:  return 0xFF6B6B;
    case TERM_INFO: return 0x8E8E93;
    default:        return 0xF2F2F7;
    }
}

/* the byte offset of character c in s */
static int u8off(const char *s, int len, int c)
{
    int i = 0;
    while (i < len && c > 0) {
        i++;
        while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
        c--;
    }
    return i;
}

static void draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    term_t *t = get(o);
    if (!t) return;
    fit(o, t);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_content_coords(o, &c);
    int sy = lv_obj_get_scroll_y(o);
    int top_row = sy / t->rh - 1, bottom_row = (sy + lv_area_get_height(&c)) / t->rh + 1;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = t->font;
    d.text_local = 1;
    static char row[LINE_MAX + 1];
    int r = 0;
    for (int i = 0; i < t->n && r <= bottom_row; i++) {
        line_t *l = line_at(t, i);
        int nr = line_rows(t, l);
        if (r + nr <= top_row) {
            r += nr;
            continue;
        }
        d.color = lv_color_hex(kind_color(l->kind));
        for (int k = 0; k < nr; k++, r++) {
            if (r < top_row || r > bottom_row) continue;
            int a = l->s ? u8off(l->s, l->len, k * t->cols) : 0;
            int b = l->s ? u8off(l->s, l->len, (k + 1) * t->cols) : 0;
            if (b <= a) continue;
            memcpy(row, l->s + a, b - a);
            row[b - a] = 0;
            d.text = row;
            int y = c.y1 + r * t->rh - sy;
            lv_area_t a2 = { c.x1, y, c.x2, y + t->rh };
            lv_draw_label(layer, &d, &a2);
        }
    }
}

static void delete_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    term_t *t = get(o);
    if (!t) return;
    for (int i = 0; i < t->n; i++) free(line_at(t, i)->s);
    free(t->l);
    free(t);
    lv_obj_set_user_data(o, NULL);
}

static void size_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    term_t *t = get(o);
    if (t) fit(o, t);
}

lv_obj_t *term_create(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x0B0B0D), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_pad_all(o, 10, 0);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    term_t *t = calloc(1, sizeof *t);
    if (!t) return o;
    t->l = calloc(TERM_LINES, sizeof(line_t));
    if (!t->l) {
        free(t);
        return o;
    }
    t->font = font;
    t->cw = lv_font_get_glyph_width(font, 'M', 0);
    t->rh = lv_font_get_line_height(font) + 2;
    lv_obj_set_user_data(o, t);
    t->spacer = lv_obj_create(o);
    lv_obj_remove_style_all(t->spacer);
    lv_obj_remove_flag(t->spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(t->spacer, 1, t->rh);
    lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(o, delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(o, size_cb, LV_EVENT_SIZE_CHANGED, NULL);
    return o;
}
