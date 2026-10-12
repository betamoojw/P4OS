/*
 * P4OS - CAS: laying out and drawing the boxes.
 *
 * Sizes come from the font size alone (px), not from the font's own line
 * metrics, which carry leading: a box's ascent is 0.80 px and its descent
 * 0.22 px, the maths axis (where fraction bars and the middle of brackets
 * sit) is 0.30 px above the baseline. Radicals, brackets, integral and sum
 * signs are drawn with lines, so they grow with what they hold and need no
 * glyph.
 *
 * mx_layout() works in two passes: sizes bottom-up with the children's
 * offsets relative to their node, then place() turns everything into
 * absolute positions in the root's frame (x = left edge, y = baseline).
 */
#include "mx.h"

#include <string.h>

#define A(px)    ((px) * 4 / 5)
#define D(px)    ((px) * 22 / 100)
#define AXIS(px) ((px) * 30 / 100)
#define LINE(px) ((px) >= 28 ? 2 : 2)

static int smaller(int px)
{
    if (px >= 48) return 32;
    if (px >= 32) return 24;
    return 20;
}

static int imax(int a, int b) { return a > b ? a : b; }

/* --- what a token shows ---------------------------------------------------- */

const char *mx_sym_shown(const char *tok, int px)
{
    const char *p = tok;
    uint32_t c = mx_utf8_next(&p);
    if (c < 0x100 || mx_font_has(c, px)) {
        return tok;
    }
    return mx_sym_giac(tok);
}

static int sym_pad(const mx_node_t *n, int px, int *right)
{
    if (n->flags & MX_F_OP) {
        if (!strcmp(n->text, ",")) {
            *right = px / 4;
            return 0;
        }
        *right = px / 5;
        return px / 5;
    }
    if (n->flags & MX_F_TEXT) {
        *right = px / 4;
        return px / 4;
    }
    *right = 0;
    return 0;
}

/* --- pass 1: sizes --------------------------------------------------------- */

static void lay_row(mx_row_t *r, int px, int fdepth);

static void lay_text_box(int px, const char *text, int *w)
{
    *w = mx_text_width(mx_font(px), mx_sym_shown(text, px));
}

static void lay_node(mx_node_t *n, int px, int fdepth, const mx_node_t *prev)
{
    n->px = (uint8_t)px;
    int sp = smaller(px);
    switch (n->kind) {
    case MX_SYM: {
        int r, l = sym_pad(n, px, &r), w;
        lay_text_box(px, n->text, &w);
        n->w = (int16_t)(l + w + r);
        n->asc = (int16_t)A(px);
        n->dsc = (int16_t)D(px);
        break;
    }
    case MX_FRAC: {
        int kp = fdepth ? sp : px;
        mx_row_t *nu = n->kid[0], *de = n->kid[1];
        lay_row(nu, kp, fdepth + 1);
        lay_row(de, kp, fdepth + 1);
        int gap = imax(3, px / 8), t = LINE(px), m = px / 10, pad = px / 8;
        int ax = AXIS(px);
        int w = imax(nu->w, de->w) + 2 * pad;
        nu->x = (int16_t)(m + (w - nu->w) / 2);
        nu->y = (int16_t)-(ax + t / 2 + gap + nu->dsc);
        de->x = (int16_t)(m + (w - de->w) / 2);
        de->y = (int16_t)(-ax + t / 2 + gap + de->asc);
        n->w = (int16_t)(w + 2 * m);
        n->asc = (int16_t)(ax + t / 2 + gap + nu->dsc + nu->asc);
        n->dsc = (int16_t)imax(D(px), -ax + t / 2 + gap + de->asc + de->dsc);
        break;
    }
    case MX_POW: {
        mx_row_t *e = n->kid[0];
        lay_row(e, sp, fdepth);
        int pa = prev ? prev->asc : A(px);
        int raise = imax(A(px) * 45 / 100, pa - e->asc / 2);
        e->x = (int16_t)(px / 24);
        e->y = (int16_t)-raise;
        n->w = (int16_t)(e->w + px / 12);
        n->asc = (int16_t)(raise + e->asc);
        n->dsc = (int16_t)imax(0, e->dsc - raise);
        break;
    }
    case MX_SUB: {
        mx_row_t *s = n->kid[0];
        lay_row(s, sp, fdepth);
        int drop = px * 22 / 100;
        s->x = 1;
        s->y = (int16_t)drop;
        n->w = (int16_t)(s->w + 2);
        n->asc = (int16_t)imax(0, s->asc - drop);
        n->dsc = (int16_t)(drop + s->dsc);
        break;
    }
    case MX_ROOT: {
        mx_row_t *rad = n->kid[0], *idx = n->nkid > 1 ? n->kid[1] : NULL;
        lay_row(rad, px, fdepth);
        int gap = imax(2, px / 10), t = LINE(px);
        int sw = px * 55 / 100;
        int asc = imax(rad->asc, A(px)) + gap + t;
        int ox = 0;
        if (idx) {
            lay_row(idx, smaller(sp), fdepth);
            ox = imax(0, idx->w - sw / 2);
            idx->x = (int16_t)imax(0, ox + sw / 2 - idx->w);
            idx->y = (int16_t)-(asc * 45 / 100 + idx->dsc);
            asc = imax(asc, asc * 45 / 100 + idx->dsc + idx->asc);
        }
        rad->x = (int16_t)(ox + sw + px / 12);
        rad->y = 0;
        n->w = (int16_t)(rad->x + rad->w + px / 10);
        n->asc = (int16_t)asc;
        n->dsc = (int16_t)(imax(rad->dsc, D(px)) + px / 12);
        break;
    }
    case MX_PAREN: {
        mx_row_t *in = n->kid[0];
        lay_row(in, px, fdepth);
        int ia = imax(in->asc, A(px)), id = imax(in->dsc, D(px));
        int h = ia + id;
        int pw;
        /* one line high: the font's own bracket, which matches the text;
         * taller: drawn, so it grows with what it holds */
        n->flags &= (uint8_t)~MX_F_GLYPH;
        if (n->text[0] != '|' && in->asc <= A(px) + 2 && in->dsc <= D(px) + 2) {
            char g[2] = { n->text[0], 0 };
            n->flags |= MX_F_GLYPH;
            pw = mx_text_width(mx_font(px), g) + px / 16;
            in->x = (int16_t)pw;
            in->y = 0;
            n->w = (int16_t)(2 * pw + in->w);
            n->asc = (int16_t)A(px);
            n->dsc = (int16_t)D(px);
            break;
        }
        switch (n->text[0]) {
        case '|': pw = px / 4; break;
        case '[': pw = px * 28 / 100; break;
        case '{': pw = px * 38 / 100; break;
        default:  pw = px * 30 / 100 + h / 30; break;
        }
        in->x = (int16_t)pw;
        in->y = 0;
        n->w = (int16_t)(2 * pw + in->w);
        n->asc = (int16_t)(ia + px / 16);
        n->dsc = (int16_t)(id + px / 16);
        break;
    }
    case MX_INT: {
        bool def = n->nkid == 4;
        mx_row_t *body = n->kid[def ? 2 : 0], *var = n->kid[def ? 3 : 1];
        lay_row(body, px, fdepth);
        lay_row(var, px, fdepth);
        int sa = imax(body->asc, A(px)) + px / 4, sd = imax(body->dsc, D(px)) + px / 5;
        int sw = px * 45 / 100;
        int bx = sw + px / 8;
        if (def) {
            mx_row_t *lo = n->kid[0], *hi = n->kid[1];
            lay_row(lo, sp, fdepth);
            lay_row(hi, sp, fdepth);
            hi->x = (int16_t)(sw * 85 / 100);
            hi->y = (int16_t)(-sa + hi->asc * 85 / 100);
            lo->x = (int16_t)(sw * 45 / 100);
            lo->y = (int16_t)(sd - lo->dsc / 2);
            bx = imax(hi->x + hi->w, lo->x + lo->w) + px / 8;
            sa = imax(sa, -hi->y + hi->asc);
            sd = imax(sd, lo->y + lo->dsc);
        }
        body->x = (int16_t)bx;
        body->y = 0;
        int dw = mx_text_width(mx_font(px), "d");
        var->x = (int16_t)(bx + body->w + px / 6 + dw);
        var->y = 0;
        n->w = (int16_t)(var->x + var->w + px / 10);
        n->asc = (int16_t)imax(sa, var->asc);
        n->dsc = (int16_t)imax(sd, var->dsc);
        break;
    }
    case MX_SUM:
    case MX_PROD: {
        mx_row_t *var = n->kid[0], *lo = n->kid[1], *hi = n->kid[2], *body = n->kid[3];
        lay_row(var, sp, fdepth);
        lay_row(lo, sp, fdepth);
        lay_row(hi, sp, fdepth);
        lay_row(body, px, fdepth);
        int ax = AXIS(px), ws = px, hs = px * 115 / 100, gap = px / 8;
        int eq = mx_text_width(mx_font(sp), "=");
        int bottom = var->w + eq + lo->w;
        int col = imax(ws, imax(bottom, hi->w));
        hi->x = (int16_t)((col - hi->w) / 2);
        hi->y = (int16_t)(-(ax + hs / 2) - gap - hi->dsc);
        var->x = (int16_t)((col - bottom) / 2);
        var->y = (int16_t)(hs / 2 - ax + gap + imax(var->asc, lo->asc));
        lo->x = (int16_t)(var->x + var->w + eq);
        lo->y = var->y;
        body->x = (int16_t)(col + px / 6);
        body->y = 0;
        n->w = (int16_t)(body->x + body->w + px / 10);
        n->asc = (int16_t)imax(body->asc, ax + hs / 2 + gap + hi->dsc + hi->asc);
        n->dsc = (int16_t)imax(body->dsc, var->y + imax(var->dsc, lo->dsc));
        break;
    }
    case MX_DERIV: {
        mx_row_t *var = n->kid[0], *body = n->kid[1];
        lay_row(var, px, fdepth + 1);
        lay_row(body, px, fdepth);
        int dw = mx_text_width(mx_font(px), "d");
        int ax = AXIS(px), gap = imax(3, px / 8), t = LINE(px), pad = px / 10;
        int denw = dw + var->w;
        int fw = imax(dw, denw) + 2 * pad;
        var->x = (int16_t)(pad + (fw - denw) / 2 + dw);
        var->y = (int16_t)(-ax + t / 2 + gap + imax(var->asc, A(px)));
        bool paren = body->n > 1;
        int pw = paren ? px * 30 / 100 + (body->asc + body->dsc) / 30 : 0;
        body->x = (int16_t)(fw + px / 10 + pw);
        body->y = 0;
        n->w = (int16_t)(body->x + body->w + pw + px / 10);
        n->asc = (int16_t)imax(body->asc + (paren ? px / 16 : 0), ax + t / 2 + gap + D(px) + A(px));
        n->dsc = (int16_t)imax(body->dsc + (paren ? px / 16 : 0), var->y + imax(var->dsc, D(px)));
        break;
    }
    case MX_LIMIT: {
        mx_row_t *var = n->kid[0], *to = n->kid[1], *body = n->kid[2];
        lay_row(var, sp, fdepth);
        lay_row(to, sp, fdepth);
        lay_row(body, px, fdepth);
        int lw = mx_text_width(mx_font(px), "lim");
        int aw = mx_text_width(mx_font(sp), mx_sym_shown("\xE2\x86\x92", sp)) + sp / 4;
        int bottom = var->w + aw + to->w;
        int col = imax(lw, bottom);
        var->x = (int16_t)((col - bottom) / 2);
        var->y = (int16_t)(D(px) + imax(var->asc, A(sp)) + 2);
        to->x = (int16_t)(var->x + var->w + aw);
        to->y = var->y;
        body->x = (int16_t)(col + px / 5);
        body->y = 0;
        n->w = (int16_t)(body->x + body->w + px / 10);
        n->asc = (int16_t)imax(body->asc, A(px));
        n->dsc = (int16_t)imax(body->dsc, var->y + imax(var->dsc, to->dsc));
        break;
    }
    case MX_MAT: {
        int R = n->rows, C = n->cols;
        int cw[8] = { 0 }, ra[8] = { 0 }, rd[8] = { 0 };
        if (R > 8) R = 8;
        if (C > 8) C = 8;
        for (int r = 0; r < R; r++) {
            for (int c = 0; c < C; c++) {
                mx_row_t *k = n->kid[r * n->cols + c];
                lay_row(k, px, fdepth);
                cw[c] = imax(cw[c], k->w);
                ra[r] = imax(ra[r], k->asc);
                rd[r] = imax(rd[r], k->dsc);
            }
        }
        int gx = px * 6 / 10, gy = px / 4, bw = px / 4, pad = px / 6;
        int H = 0;
        for (int r = 0; r < R; r++) {
            H += ra[r] + rd[r] + (r ? gy : 0);
        }
        int ax = AXIS(px);
        int top = -(ax + H / 2);
        int y = top;
        for (int r = 0; r < R; r++) {
            int x = bw + pad;
            int base = y + ra[r];
            for (int c = 0; c < C; c++) {
                mx_row_t *k = n->kid[r * n->cols + c];
                k->x = (int16_t)(x + (cw[c] - k->w) / 2);
                k->y = (int16_t)base;
                x += cw[c] + gx;
            }
            y = base + rd[r] + gy;
        }
        int w = 2 * (bw + pad);
        for (int c = 0; c < C; c++) {
            w += cw[c] + (c ? gx : 0);
        }
        n->w = (int16_t)w;
        n->asc = (int16_t)(-top + px / 16);
        n->dsc = (int16_t)(H + top + px / 16);
        break;
    }
    }
}

static void lay_row(mx_row_t *r, int px, int fdepth)
{
    r->px = (uint8_t)px;
    if (!r->n) {
        r->w = (int16_t)(px * 6 / 10);
        r->asc = (int16_t)(px * 7 / 10);
        r->dsc = (int16_t)(px * 15 / 100);
        return;
    }
    int x = 0, asc = 0, dsc = 0;
    for (int i = 0; i < r->n; i++) {
        mx_node_t *n = r->it[i];
        lay_node(n, px, fdepth, i ? r->it[i - 1] : NULL);
        n->x = (int16_t)x;
        x += n->w;
        asc = imax(asc, n->asc);
        dsc = imax(dsc, n->dsc);
    }
    r->w = (int16_t)x;
    r->asc = (int16_t)asc;
    r->dsc = (int16_t)dsc;
}

/* --- pass 2: absolute positions -------------------------------------------- */

static void place_row(mx_row_t *r, int x, int y)
{
    r->x = (int16_t)x;
    r->y = (int16_t)y;
    for (int i = 0; i < r->n; i++) {
        mx_node_t *n = r->it[i];
        int nx = x + n->x;
        n->x = (int16_t)nx;
        n->y = (int16_t)y;
        for (int k = 0; k < n->nkid; k++) {
            mx_row_t *c = n->kid[k];
            place_row(c, nx + c->x, y + c->y);
        }
    }
}

void mx_layout(mx_row_t *r, int px)
{
    lay_row(r, px, 0);
    place_row(r, 0, r->asc);
}

/* --- drawing --------------------------------------------------------------- */

typedef struct {
    lv_layer_t       *layer;
    int               ox, oy;
    const mx_style_t *st;
} dc_t;

static void line(const dc_t *d, int x1, int y1, int x2, int y2, int w, lv_color_t c)
{
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.p1.x = d->ox + x1;
    l.p1.y = d->oy + y1;
    l.p2.x = d->ox + x2;
    l.p2.y = d->oy + y2;
    l.width = w;
    l.color = c;
    l.round_start = 1;
    l.round_end = 1;
    lv_draw_line(d->layer, &l);
}

static void text(const dc_t *d, int x, int y, const char *s, int px, lv_color_t c)
{
    const lv_font_t *f = mx_font(px);
    int h = lv_font_get_line_height(f);
    lv_draw_label_dsc_t t;
    lv_draw_label_dsc_init(&t);
    t.text = s;
    t.text_local = 1;
    t.font = f;
    t.color = c;
    lv_area_t a;
    a.x1 = d->ox + x;
    a.y1 = d->oy + y - (h - f->base_line);
    a.x2 = a.x1 + mx_text_width(f, s) + 4;
    a.y2 = a.y1 + h;
    lv_draw_label(d->layer, &t, &a);
}

/* A smooth bracket: a quadratic curve from (x0,top) through (xm,mid) to
 * (x0,bottom), as eight segments. */
static void curve(const dc_t *d, int x0, int top, int xm, int bot, int w, lv_color_t c)
{
    int mid = (top + bot) / 2;
    int px = x0, py = top;
    for (int i = 1; i <= 8; i++) {
        float t = i / 8.0f, u = 1 - t;
        float x = u * u * x0 + 2 * u * t * xm + t * t * x0;
        float y = u * u * top + 2 * u * t * mid + t * t * bot;
        line(d, px, py, (int)x, (int)y, w, c);
        px = (int)x;
        py = (int)y;
    }
}

static void draw_row(const dc_t *d, const mx_row_t *r);

static void bracket(const dc_t *d, char kind, int x, int w, int top, int bot, bool right, int t, lv_color_t c)
{
    switch (kind) {
    case '|':
        line(d, x + w / 2, top, x + w / 2, bot, t, c);
        break;
    case '[': {
        int a = right ? x + w / 4 : x + w * 3 / 4, b = right ? x + w * 3 / 4 : x + w / 4;
        line(d, a, top, b, top, t, c);
        line(d, b, top, b, bot, t, c);
        line(d, b, bot, a, bot, t, c);
        break;
    }
    case '{': {
        int a = right ? x + w / 5 : x + w * 4 / 5, m = x + w / 2, b = right ? x + w * 4 / 5 : x + w / 5;
        int mid = (top + bot) / 2;
        curve(d, a, top, m - (a - m) / 3, mid - (bot - top) / 8, t, c);
        line(d, m, mid - (bot - top) / 8, b, mid, t, c);
        line(d, b, mid, m, mid + (bot - top) / 8, t, c);
        curve(d, m, mid + (bot - top) / 8, m, bot, t, c);
        line(d, m, bot, a, bot, t, c);
        break;
    }
    default: {
        int edge = right ? x + w / 5 : x + w * 4 / 5;
        int belly = right ? x + w * 4 / 5 : x + w / 5;
        curve(d, edge, top, belly, bot, t, c);
        break;
    }
    }
}

static void draw_node(const dc_t *d, const mx_node_t *n)
{
    int px = n->px, t = LINE(px);
    lv_color_t c = d->st->text;
    switch (n->kind) {
    case MX_SYM: {
        int r, l = sym_pad(n, px, &r);
        text(d, n->x + l, n->y, mx_sym_shown(n->text, px), px,
             (n->flags & MX_F_DIM) ? d->st->dim : c);
        break;
    }
    case MX_FRAC: {
        int m = px / 10, ax = AXIS(px);
        line(d, n->x + m, n->y - ax, n->x + n->w - m, n->y - ax, t, c);
        draw_row(d, n->kid[0]);
        draw_row(d, n->kid[1]);
        break;
    }
    case MX_POW:
    case MX_SUB:
        draw_row(d, n->kid[0]);
        break;
    case MX_ROOT: {
        const mx_row_t *rad = n->kid[0];
        int sw = px * 55 / 100;
        int ox = rad->x - px / 12 - sw;
        int top = n->y - n->asc + t / 2, bot = n->y + n->dsc - t;
        int tickx = ox, ticky = n->y - AXIS(px);
        line(d, tickx, ticky + px / 12, ox + sw * 30 / 100, ticky, t, c);
        line(d, ox + sw * 30 / 100, ticky, ox + sw * 58 / 100, bot, t + 1, c);
        line(d, ox + sw * 58 / 100, bot, ox + sw, top, t, c);
        line(d, ox + sw, top, n->x + n->w - px / 20, top, t, c);
        draw_row(d, rad);
        if (n->nkid > 1) {
            draw_row(d, n->kid[1]);
        }
        break;
    }
    case MX_PAREN: {
        const mx_row_t *in = n->kid[0];
        int pw = in->x - n->x;
        if (n->flags & MX_F_GLYPH) {
            static const char *const OPEN[] = { "(", "[", "{" }, *const CLOSE[] = { ")", "]", "}" };
            int k = n->text[0] == '[' ? 1 : n->text[0] == '{' ? 2 : 0;
            text(d, n->x, n->y, OPEN[k], px, c);
            text(d, in->x + in->w + px / 16, n->y, CLOSE[k], px, c);
            draw_row(d, in);
            break;
        }
        int top = n->y - n->asc, bot = n->y + n->dsc;
        bracket(d, n->text[0], n->x, pw, top, bot, false, t, c);
        bracket(d, n->text[0], in->x + in->w, pw, top, bot, true, t, c);
        draw_row(d, in);
        break;
    }
    case MX_INT: {
        bool def = n->nkid == 4;
        const mx_row_t *body = n->kid[def ? 2 : 0], *var = n->kid[def ? 3 : 1];
        int sw = px * 45 / 100;
        int top = n->y - imax(body->asc, A(px)) - px / 4;
        int bot = n->y + imax(body->dsc, D(px)) + px / 5;
        int x = n->x, h = sw * 30 / 100;
        line(d, x + sw * 95 / 100, top + h, x + sw * 80 / 100, top, t, c);
        line(d, x + sw * 80 / 100, top, x + sw * 62 / 100, top + h / 2, t, c);
        line(d, x + sw * 62 / 100, top + h / 2, x + sw * 38 / 100, bot - h / 2, t + 1, c);
        line(d, x + sw * 38 / 100, bot - h / 2, x + sw * 20 / 100, bot, t, c);
        line(d, x + sw * 20 / 100, bot, x + sw * 5 / 100, bot - h, t, c);
        if (def) {
            draw_row(d, n->kid[0]);
            draw_row(d, n->kid[1]);
        }
        draw_row(d, body);
        int dw = mx_text_width(mx_font(px), "d");
        text(d, var->x - dw, var->y, "d", px, c);
        draw_row(d, var);
        break;
    }
    case MX_SUM:
    case MX_PROD: {
        const mx_row_t *var = n->kid[0], *lo = n->kid[1];
        int ws = px, hs = px * 115 / 100, ax = AXIS(px);
        int col = n->kid[3]->x - px / 6 - n->x;
        int x0 = n->x + (col - ws) / 2, x1 = x0 + ws;
        int top = n->y - ax - hs / 2, bot = n->y - ax + hs / 2;
        if (n->kind == MX_SUM) {
            line(d, x1, top, x0, top, t + 1, c);
            line(d, x0, top, x0 + ws * 55 / 100, (top + bot) / 2, t + 1, c);
            line(d, x0 + ws * 55 / 100, (top + bot) / 2, x0, bot, t + 1, c);
            line(d, x0, bot, x1, bot, t + 1, c);
        } else {
            line(d, x0, top, x1, top, t + 1, c);
            line(d, x0 + ws / 5, top, x0 + ws / 5, bot, t + 1, c);
            line(d, x1 - ws / 5, top, x1 - ws / 5, bot, t + 1, c);
        }
        draw_row(d, n->kid[2]);
        draw_row(d, var);
        text(d, var->x + var->w, var->y, "=", var->px, c);
        draw_row(d, lo);
        draw_row(d, n->kid[3]);
        break;
    }
    case MX_DERIV: {
        const mx_row_t *var = n->kid[0], *body = n->kid[1];
        int dw = mx_text_width(mx_font(px), "d");
        int ax = AXIS(px), gap = imax(3, px / 8), pad = px / 10;
        int denw = dw + var->w;
        int fw = imax(dw, denw) + 2 * pad;
        line(d, n->x + pad / 2, n->y - ax, n->x + fw - pad / 2, n->y - ax, t, c);
        text(d, n->x + (fw - dw) / 2, n->y - ax - t / 2 - gap - D(px), "d", px, c);
        text(d, var->x - dw, var->y, "d", px, c);
        draw_row(d, var);
        if (body->n > 1) {
            int pw = px * 30 / 100 + (body->asc + body->dsc) / 30;
            int top = n->y - body->asc - px / 16, bot = n->y + body->dsc + px / 16;
            bracket(d, '(', body->x - pw, pw, top, bot, false, t, c);
            bracket(d, '(', body->x + body->w, pw, top, bot, true, t, c);
        }
        draw_row(d, body);
        break;
    }
    case MX_LIMIT: {
        const mx_row_t *var = n->kid[0], *to = n->kid[1];
        int lw = mx_text_width(mx_font(px), "lim");
        int col = n->kid[2]->x - px / 5 - n->x;
        text(d, n->x + (col - lw) / 2, n->y, "lim", px, c);
        draw_row(d, var);
        text(d, var->x + var->w + var->px / 8, var->y, mx_sym_shown("\xE2\x86\x92", var->px), var->px, c);
        draw_row(d, to);
        draw_row(d, n->kid[2]);
        break;
    }
    case MX_MAT: {
        int bw = px / 4;
        int top = n->y - n->asc, bot = n->y + n->dsc;
        bracket(d, '[', n->x, bw, top, bot, false, t, c);
        bracket(d, '[', n->x + n->w - bw, bw, top, bot, true, t, c);
        for (int i = 0; i < n->nkid; i++) {
            draw_row(d, n->kid[i]);
        }
        break;
    }
    }
}

static void draw_row(const dc_t *d, const mx_row_t *r)
{
    if (!r->n) {
        if (d->st->edit) {
            lv_draw_rect_dsc_t b;
            lv_draw_rect_dsc_init(&b);
            b.bg_opa = LV_OPA_TRANSP;
            b.border_color = d->st->placeholder;
            b.border_width = 2;
            b.border_opa = LV_OPA_COVER;
            b.radius = 3;
            lv_area_t a = {
                d->ox + r->x + 2, d->oy + r->y - r->asc + 2,
                d->ox + r->x + r->w - 2, d->oy + r->y + r->dsc - 2,
            };
            lv_draw_rect(d->layer, &b, &a);
        }
        return;
    }
    for (int i = 0; i < r->n; i++) {
        draw_node(d, r->it[i]);
    }
}

void mx_draw(lv_layer_t *layer, const mx_row_t *r, int ox, int oy, const mx_style_t *st)
{
    dc_t d = { layer, ox, oy, st };
    draw_row(&d, r);
}

void mx_draw_cursor(lv_layer_t *layer, const mx_cur_t *c, int ox, int oy, lv_color_t col)
{
    const mx_row_t *r = c->row;
    if (!r) {
        return;
    }
    int x;
    if (!r->n) {
        x = r->x + r->w / 2;
    } else if (c->pos < r->n) {
        x = r->it[c->pos]->x;
    } else {
        x = r->x + r->w;
    }
    int top = r->y - imax(r->asc, A(r->px)), bot = r->y + imax(r->dsc, D(r->px));
    dc_t d = { layer, ox, oy, NULL };
    line(&d, x, top, x, bot, 3, col);
}
