/*
 * P4OS (from AmoledOS) - Cameras: YUV and RGB565 to RGB565, scaled (cam_conv.h).
 *
 * The colour maths is tables, as on the watch: each chroma sample becomes
 * three offsets, and each output pixel is three table reads already shifted
 * into place, OR-ed together, with the clamp inside the tables. Two changes
 * from AmoledOS: the output is native RGB565 (LVGL's order, not the watch
 * panel's big-endian), and Y is taken as limited range (16-235, BT.601),
 * which is what H.264 cameras send; the watch treated it as full range and
 * showed the blacks a little grey.
 *
 * Nearest neighbour and not bilinear: at 720x1280 the pixels are small, and
 * the P4's PPA is the place for a filter if the board shows the need.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "cam_conv.h"

#include <stdlib.h>
#include <string.h>

#define CLAMP_OFF 384

typedef struct {
    int16_t  y[256], rv[256], gu[256], gv[256], bu[256];
    uint16_t r[1024], g[1024], b[1024];         /* index = value + CLAMP_OFF */
} tables_t;

static tables_t *s_t;

bool cam_conv_init(void)
{
    if (s_t) {
        return true;
    }
    tables_t *t = malloc(sizeof(*t));
    if (!t) {
        return false;
    }
    for (int i = 0; i < 256; i++) {
        int c = i - 128;
        t->y[i]  = (int16_t)((298 * (i - 16)) >> 8);
        t->rv[i] = (int16_t)((409 * c) >> 8);
        t->gu[i] = (int16_t)((100 * c) >> 8);
        t->gv[i] = (int16_t)((208 * c) >> 8);
        t->bu[i] = (int16_t)((516 * c) >> 8);
    }
    for (int i = 0; i < 1024; i++) {
        int v = i - CLAMP_OFF;
        uint16_t c = (uint16_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        t->r[i] = (uint16_t)((c & 0xF8) << 8);
        t->g[i] = (uint16_t)((c & 0xFC) << 3);
        t->b[i] = (uint16_t)(c >> 3);
    }
    s_t = t;
    return true;
}

void cam_geometry(int w, int h, int box_w, int box_h, cam_mode_t mode, cam_geom_t *g)
{
    memset(g, 0, sizeof(*g));
    if (w <= 0 || h <= 0 || box_w <= 0 || box_h <= 0) {
        return;
    }
    if (mode == CAM_FIT) {
        g->sw = w;
        g->sh = h;
        g->ow = box_w;
        g->oh = (int)(((int64_t)h * box_w + w / 2) / w);
        if (g->oh > box_h) {
            g->oh = box_h;
            g->ow = (int)(((int64_t)w * box_h + h / 2) / h);
        }
    } else {
        g->ow = box_w;
        g->oh = box_h;
        if ((int64_t)w * box_h > (int64_t)h * box_w) {       /* wider: crop the sides */
            g->sh = h;
            g->sw = (int)(((int64_t)h * box_w + box_h / 2) / box_h);
            if (g->sw > w) g->sw = w;
            g->sx = (w - g->sw) / 2;
        } else {                                             /* taller: crop top and bottom */
            g->sw = w;
            g->sh = (int)(((int64_t)w * box_h + box_w / 2) / box_w);
            if (g->sh > h) g->sh = h;
            g->sy = (h - g->sh) / 2;
        }
    }
    g->ow &= ~1;
    g->oh &= ~1;
    if (g->ow < 2) g->ow = 2;
    if (g->oh < 2) g->oh = 2;
}

void cam_conv_release(cam_conv_t *c)
{
    free(c->xmap);
    c->xmap = NULL;
    c->cap = 0;
}

static bool prepare(cam_conv_t *c, const cam_geom_t *g)
{
    if (g->ow > c->cap) {
        free(c->xmap);
        c->xmap = malloc((size_t)g->ow * sizeof(uint16_t));
        c->cap = c->xmap ? g->ow : 0;
        if (!c->xmap) {
            return false;
        }
    }
    for (int x = 0; x < g->ow; x++) {
        c->xmap[x] = (uint16_t)(g->sx + (x * g->sw + g->sw / 2) / g->ow);
    }
    return s_t != NULL;
}

bool cam_conv_i420(cam_conv_t *c, const aos_h264_pic_t *pic, const cam_geom_t *g, uint16_t *out)
{
    return cam_conv_i420_to(c, pic, g, out, g->ow);
}

bool cam_conv_i420_to(cam_conv_t *c, const aos_h264_pic_t *pic, const cam_geom_t *g, uint16_t *out, int stride)
{
    if (!prepare(c, g)) {
        return false;
    }
    const tables_t *t = s_t;
    const uint16_t *R = t->r + CLAMP_OFF, *G = t->g + CLAMP_OFF, *B = t->b + CLAMP_OFF;
    const uint16_t *xm = c->xmap;
    const int ow = g->ow;
    int last_sy = -1;
    uint16_t *prev = NULL;
    for (int y = 0; y < g->oh; y++) {
        int sy = g->sy + (y * g->sh + g->sh / 2) / g->oh;
        uint16_t *o = out + (size_t)y * stride;
        if (sy == last_sy) {                /* enlarging: the same source row again */
            memcpy(o, prev, (size_t)ow * 2);
            continue;
        }
        const uint8_t *ry = pic->y + (size_t)sy * pic->stride_y;
        const uint8_t *ru = pic->u + (size_t)(sy >> 1) * pic->stride_uv;
        const uint8_t *rv = pic->v + (size_t)(sy >> 1) * pic->stride_uv;
        /* The chroma is one sample for two columns of the source: its three
         * terms are looked up again only when the source column moves on
         * to another pair (2026-10-22: a third fewer reads). */
        int lc = -1, cr = 0, cg = 0, cb = 0;
#define CHROMA(sx) do { int ci_ = (sx) >> 1; if (ci_ != lc) { lc = ci_; int u_ = ru[ci_], v_ = rv[ci_]; \
            cr = t->rv[v_]; cg = t->gu[u_] + t->gv[v_]; cb = t->bu[u_]; } } while (0)
#define PIX(sx) ({ int Y_ = t->y[ry[(sx)]]; (uint32_t)(R[Y_ + cr] | G[Y_ - cg] | B[Y_ + cb]); })
        int x = 0;
        if ((uintptr_t)o & 2) {             /* to a 4-byte boundary for the pairs */
            int sx = xm[0];
            CHROMA(sx);
            o[0] = (uint16_t)PIX(sx);
            x = 1;
        }
        /* two pixels a write */
        for (; x + 1 < ow; x += 2) {
            int s0 = xm[x], s1 = xm[x + 1];
            CHROMA(s0);
            uint32_t p0 = PIX(s0);
            CHROMA(s1);
            uint32_t p1 = PIX(s1);
            *(uint32_t *)(o + x) = p0 | p1 << 16;
        }
        if (x < ow) {
            int sx = xm[x];
            CHROMA(sx);
            o[x] = (uint16_t)PIX(sx);
        }
#undef PIX
#undef CHROMA
        last_sy = sy;
        prev = o;
    }
    return true;
}

/* Turned a quarter clockwise: the geometry was worked out on the turned
 * picture (src_h x src_w), so its crop and output are the turned ones. The
 * turned pixel (rx, ry) is the source's (ry, src_h - 1 - rx). The output
 * goes in bands of BAND rows, column by column inside a band: the source is
 * read along its rows (rx runs down a source column, ry along a row), which
 * keeps the reads in a few cache lines at a time. */
#define BAND 16

bool cam_conv_i420_rot(cam_conv_t *c, const aos_h264_pic_t *pic, const cam_geom_t *g, uint16_t *out, int stride)
{
    if (!prepare(c, g)) {
        return false;
    }
    const tables_t *t = s_t;
    const uint16_t *R = t->r + CLAMP_OFF, *G = t->g + CLAMP_OFF, *B = t->b + CLAMP_OFF;
    const uint16_t *xm = c->xmap;           /* output column -> turned rx */
    const int ow = g->ow, H = pic->height;
    const bool odd = ((uintptr_t)out & 2) != 0;   /* pairs start one column in */
    for (int y0 = 0; y0 < g->oh; y0 += BAND) {
        int n = g->oh - y0 < BAND ? g->oh - y0 : BAND;
        int ry[BAND];
        for (int k = 0; k < n; k++) ry[k] = g->sy + ((y0 + k) * g->sh + g->sh / 2) / g->oh;
        /* Two output columns at a time, so each row gets them in one 32-bit
         * write; each column is a source row, and inside it the chroma is
         * looked up once for each pair of source columns. */
        int x = 0;
        for (;;) {
            bool single = (x == 0 && odd) || x + 1 >= ow;
            if (x >= ow) break;
            int sy0 = H - 1 - xm[x];
            int sy1 = single ? sy0 : H - 1 - xm[x + 1];
            if (sy0 < 0) sy0 = 0;
            if (sy1 < 0) sy1 = 0;
            const uint8_t *y0p = pic->y + (size_t)sy0 * pic->stride_y, *y1p = pic->y + (size_t)sy1 * pic->stride_y;
            const uint8_t *u0 = pic->u + (size_t)(sy0 >> 1) * pic->stride_uv, *v0 = pic->v + (size_t)(sy0 >> 1) * pic->stride_uv;
            const uint8_t *u1 = pic->u + (size_t)(sy1 >> 1) * pic->stride_uv, *v1 = pic->v + (size_t)(sy1 >> 1) * pic->stride_uv;
            bool same_c = (sy0 >> 1) == (sy1 >> 1);
            uint16_t *o = out + (size_t)y0 * stride + x;
            int lc = -1, cr0 = 0, cg0 = 0, cb0 = 0, cr1 = 0, cg1 = 0, cb1 = 0;
            for (int k = 0; k < n; k++) {
                int sx = ry[k], ci = sx >> 1;
                if (ci != lc) {
                    lc = ci;
                    int u = u0[ci], v = v0[ci];
                    cr0 = t->rv[v]; cg0 = t->gu[u] + t->gv[v]; cb0 = t->bu[u];
                    if (same_c) {
                        cr1 = cr0; cg1 = cg0; cb1 = cb0;
                    } else {
                        u = u1[ci]; v = v1[ci];
                        cr1 = t->rv[v]; cg1 = t->gu[u] + t->gv[v]; cb1 = t->bu[u];
                    }
                }
                int Y0 = t->y[y0p[sx]];
                uint32_t p0 = R[Y0 + cr0] | G[Y0 - cg0] | B[Y0 + cb0];
                if (single) {
                    o[(size_t)k * stride] = (uint16_t)p0;
                } else {
                    int Y1 = t->y[y1p[sx]];
                    uint32_t p1 = R[Y1 + cr1] | G[Y1 - cg1] | B[Y1 + cb1];
                    *(uint32_t *)(o + (size_t)k * stride) = p0 | p1 << 16;
                }
            }
            x += single ? 1 : 2;
        }
    }
    return true;
}

bool cam_conv_rgb565(cam_conv_t *c, const uint16_t *src, int src_stride, const cam_geom_t *g, uint16_t *out)
{
    if (!prepare(c, g)) {
        return false;
    }
    const uint16_t *xm = c->xmap;
    const int ow = g->ow;
    const bool straight = g->sw == g->ow;
    int last_sy = -1;
    uint16_t *prev = NULL;
    for (int y = 0; y < g->oh; y++) {
        int sy = g->sy + (y * g->sh + g->sh / 2) / g->oh;
        uint16_t *o = out + (size_t)y * ow;
        if (sy == last_sy) {
            memcpy(o, prev, (size_t)ow * 2);
            continue;
        }
        const uint16_t *row = src + (size_t)sy * src_stride;
        if (straight) {
            memcpy(o, row + g->sx, (size_t)ow * 2);
        } else {
            for (int x = 0; x < ow; x++) {
                o[x] = row[xm[x]];
            }
        }
        last_sy = sy;
        prev = o;
    }
    return true;
}
