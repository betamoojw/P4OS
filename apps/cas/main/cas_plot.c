/*
 * P4OS - CAS: a canvas to plot on.
 *
 * Pixels are written straight into the canvas' RGB565 buffer: a full frame
 * of grid and curves is a few milliseconds, where the same through LVGL's
 * line drawing was a tenth of a second. Only text goes through LVGL, on the
 * canvas' layer.
 */
#include "cas_plot.h"

#include "mx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool cas_plot_create(cas_plot_t *p, lv_obj_t *parent, int w, int h)
{
    memset(p, 0, sizeof *p);
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    p->W = w;
    p->H = h;
    p->db = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565, 0);
    if (!p->db) {
        return false;
    }
    p->canvas = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(p->canvas, p->db);
    lv_obj_set_pos(p->canvas, 0, 0);
    p->x0 = -1;
    p->x1 = 1;
    p->y0 = -1;
    p->y1 = 1;
    return true;
}

void cas_plot_free(cas_plot_t *p)
{
    if (p->db) {
        lv_draw_buf_destroy(p->db);
    }
    p->db = NULL;
    p->canvas = NULL;
}

float  cas_plot_sx(const cas_plot_t *p, double x)  { return (float)((x - p->x0) / (p->x1 - p->x0) * p->W); }
float  cas_plot_sy(const cas_plot_t *p, double y)  { return (float)((p->y1 - y) / (p->y1 - p->y0) * p->H); }
double cas_plot_wx(const cas_plot_t *p, float sx)  { return p->x0 + sx / p->W * (p->x1 - p->x0); }
double cas_plot_wy(const cas_plot_t *p, float sy)  { return p->y1 - sy / p->H * (p->y1 - p->y0); }

static inline uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}

static inline uint16_t *px_row(const cas_plot_t *p, int y)
{
    return (uint16_t *)(p->db->data + (size_t)y * p->db->header.stride);
}

static inline void blend(uint16_t *d, uint16_t c, int a)
{
    if (a >= 255) {
        *d = c;
        return;
    }
    if (a <= 0) {
        return;
    }
    uint32_t s = *d;
    uint32_t sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31;
    uint32_t cr = c >> 11, cg = (c >> 5) & 63, cb = c & 31;
    sr += ((cr - sr) * (uint32_t)a + 128) >> 8;
    sg += ((cg - sg) * (uint32_t)a + 128) >> 8;
    sb += ((cb - sb) * (uint32_t)a + 128) >> 8;
    *d = (uint16_t)((sr << 11) | (sg << 5) | sb);
}

void cas_plot_clear(cas_plot_t *p, uint32_t rgb)
{
    uint16_t c = rgb565(rgb);
    for (int y = 0; y < p->H; y++) {
        uint16_t *r = px_row(p, y);
        for (int x = 0; x < p->W; x++) {
            r[x] = c;
        }
    }
}

void cas_plot_vline(cas_plot_t *p, int x, uint32_t rgb, int w)
{
    uint16_t c = rgb565(rgb);
    for (int k = 0; k < w; k++) {
        if (x + k < 0 || x + k >= p->W) continue;
        for (int y = 0; y < p->H; y++) {
            px_row(p, y)[x + k] = c;
        }
    }
}

void cas_plot_hline(cas_plot_t *p, int y, uint32_t rgb, int w)
{
    uint16_t c = rgb565(rgb);
    for (int k = 0; k < w; k++) {
        if (y + k < 0 || y + k >= p->H) continue;
        uint16_t *r = px_row(p, y + k);
        for (int x = 0; x < p->W; x++) {
            r[x] = c;
        }
    }
}

/* An anti-aliased line of width w: along its major axis, one span across
 * per pixel, with soft edges. */
void cas_plot_line(cas_plot_t *p, float x0, float y0, float x1, float y1, uint32_t rgb, float w)
{
    uint16_t c = rgb565(rgb);
    float dx = x1 - x0, dy = y1 - y0;
    bool steep = fabsf(dy) > fabsf(dx);
    if (steep) {
        float t;
        t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
        t = dx; dx = dy; dy = t;
    }
    if (x0 > x1) {
        float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    float g = dx != 0 ? dy / dx : 0;
    float half = w * sqrtf(1 + g * g) / 2;
    int lim_major = steep ? p->H : p->W, lim_minor = steep ? p->W : p->H;
    int xs = (int)floorf(x0), xe = (int)ceilf(x1);
    if (xs < 0) xs = 0;
    if (xe > lim_major - 1) xe = lim_major - 1;
    for (int x = xs; x <= xe; x++) {
        float yc = y0 + g * (x + 0.5f - x0);
        int a0 = (int)floorf(yc - half - 0.5f), a1 = (int)ceilf(yc + half + 0.5f);
        if (a0 < 0) a0 = 0;
        if (a1 > lim_minor - 1) a1 = lim_minor - 1;
        for (int y = a0; y <= a1; y++) {
            float cov = half + 0.5f - fabsf(y + 0.5f - yc);
            if (cov <= 0) continue;
            uint16_t *d = steep ? &px_row(p, x)[y] : &px_row(p, y)[x];
            blend(d, c, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

void cas_plot_rect(cas_plot_t *p, float x0, float y0, float x1, float y1, uint32_t rgb, int alpha)
{
    uint16_t c = rgb565(rgb);
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
    int xa = (int)lroundf(x0), xb = (int)lroundf(x1), ya = (int)lroundf(y0), yb = (int)lroundf(y1);
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > p->W) xb = p->W;
    if (yb > p->H) yb = p->H;
    for (int y = ya; y < yb; y++) {
        uint16_t *r = px_row(p, y);
        for (int x = xa; x < xb; x++) {
            blend(&r[x], c, alpha);
        }
    }
}

void cas_plot_disc(cas_plot_t *p, float cx, float cy, float r, uint32_t rgb)
{
    uint16_t c = rgb565(rgb);
    int y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
    int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= p->H) continue;
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= p->W) continue;
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float cov = r + 0.5f - d;
            if (cov > 0) blend(&px_row(p, y)[x], c, cov >= 1 ? 255 : (int)(cov * 255));
        }
    }
}

/* --- grid and numbers ----------------------------------------------------------- */

static double nice_step(double raw)
{
    double pw = pow(10.0, floor(log10(raw)));
    double m = raw / pw;
    return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * pw;
}

static double xstep(const cas_plot_t *p) { return nice_step(80.0 * (p->x1 - p->x0) / p->W); }
static double ystep(const cas_plot_t *p) { return nice_step(80.0 * (p->y1 - p->y0) / p->H); }

void cas_plot_grid(cas_plot_t *p)
{
    double sx = xstep(p), sy = ystep(p);
    for (double v = ceil(p->x0 / sx) * sx; v <= p->x1; v += sx) {
        cas_plot_vline(p, (int)lroundf(cas_plot_sx(p, v)), 0x1C1C22, 1);
    }
    for (double v = ceil(p->y0 / sy) * sy; v <= p->y1; v += sy) {
        cas_plot_hline(p, (int)lroundf(cas_plot_sy(p, v)), 0x1C1C22, 1);
    }
    int ax = (int)lroundf(cas_plot_sx(p, 0)), ay = (int)lroundf(cas_plot_sy(p, 0));
    if (ax >= 0 && ax < p->W) cas_plot_vline(p, ax, 0x8E8E93, 2);
    if (ay >= 0 && ay < p->H) cas_plot_hline(p, ay, 0x8E8E93, 2);
}

void cas_plot_fmt(char *b, int n, double v, double step)
{
    if (fabs(v) < step * 1e-3) v = 0;
    if (step >= 1 && fabs(v) < 1e7) {
        snprintf(b, (size_t)n, "%.0f", v);
    } else {
        snprintf(b, (size_t)n, "%g", v);
    }
    /* the minus sign of maths, not the hyphen */
    if (b[0] == '-' && (int)strlen(b) + 3 < n) {
        memmove(b + 3, b + 1, strlen(b));
        b[0] = (char)0xE2;
        b[1] = (char)0x88;
        b[2] = (char)0x92;
    }
}

void cas_plot_text(lv_layer_t *layer, int x, int y, const char *s, uint32_t rgb, int align)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s;
    d.text_local = 1;
    d.font = mx_font(20);
    d.color = lv_color_hex(rgb);
    int w = mx_text_width(d.font, s) + 2;
    if (align == 1) x -= w / 2;
    if (align == 2) x -= w;
    lv_area_t a = { x, y, x + w, y + lv_font_get_line_height(d.font) };
    lv_draw_label(layer, &d, &a);
}

void cas_plot_numbers(cas_plot_t *p, lv_layer_t *layer)
{
    double sx = xstep(p), sy = ystep(p);
    int ax = (int)lroundf(cas_plot_sx(p, 0)), ay = (int)lroundf(cas_plot_sy(p, 0));
    int ly = ay + 6;
    if (ly < 4) ly = 4;
    if (ly > p->H - 28) ly = p->H - 28;
    for (double v = ceil(p->x0 / sx) * sx; v <= p->x1; v += sx) {
        if (fabs(v) < sx / 2 && ax >= 0 && ax < p->W) continue;
        int x = (int)lroundf(cas_plot_sx(p, v));
        if (x < 20 || x > p->W - 20) continue;      /* centred on the edge: half of it cut */
        char b[32];
        cas_plot_fmt(b, sizeof b, v, sx);
        cas_plot_text(layer, x, ly, b, 0xAEAEB2, 1);
    }
    int lx = ax + 6;
    if (lx < 4) lx = 4;
    if (lx > p->W - 80) lx = p->W - 80;
    for (double v = ceil(p->y0 / sy) * sy; v <= p->y1; v += sy) {
        if (fabs(v) < sy / 2 && ay >= 0 && ay < p->H) continue;
        char b[32];
        cas_plot_fmt(b, sizeof b, v, sy);
        cas_plot_text(layer, lx, (int)lroundf(cas_plot_sy(p, v)) - 12, b, 0xAEAEB2, 0);
    }
}

void cas_plot_done(cas_plot_t *p)
{
    if (p->canvas) {
        lv_obj_invalidate(p->canvas);
    }
}
