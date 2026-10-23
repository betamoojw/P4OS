/*
 * CAÑOS - drawing the board (see cn_draw.h)
 */
/* The .so is compiled with -Os (components/elf_loader/elf_loader.cmake) and
 * per-file CMake options do not reach that compile: this is the only way to
 * give the pixel loops -O2. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "cn_draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#define PI_F    3.14159265f
#define PI_4    0.78539816f

/* the pipe, in cells: the channel the liquid fills, the outside of the
 * wall, the coupling at each end */
#define RI      0.165f
#define RO      0.245f
#define RFL     0.290f
#define FLW     0.075f

/* light from the top left, a little in front */
#define LX      (-0.448f)
#define LY      (-0.647f)
#define LZ      0.617f
#define HX      (-0.249f)
#define HY      (-0.360f)
#define HZ      0.899f

/* the preview only needs the straight */
static bool s_straight_only;

void *cn_draw_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
#else
    return malloc(n ? n : 1);
#endif
}

/* --------------------------------------------------------------------------
 * Colour
 * -------------------------------------------------------------------------- */

typedef struct {
    float r, g, b;
} col_t;

static col_t col(uint32_t c)
{
    return (col_t){ (float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f,
                    (float)(c & 255) / 255.0f };
}

static col_t mixc(col_t a, col_t b, float t)
{
    return (col_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static col_t scalec(col_t a, float k)
{
    return (col_t){ a.r * k, a.g * k, a.b * k };
}

static col_t addc(col_t a, float k)
{
    return (col_t){ a.r + k, a.g + k, a.b + k };
}

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static uint16_t to565(col_t c)
{
    int r = (int)(clampf(c.r, 0, 1) * 31.0f + 0.5f);
    int g = (int)(clampf(c.g, 0, 1) * 63.0f + 0.5f);
    int b = (int)(clampf(c.b, 0, 1) * 31.0f + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* lo -> mid -> hi */
static col_t ramp3(col_t lo, col_t mid, col_t hi, float x)
{
    x = clampf(x, 0, 1);
    return x < 0.5f ? mixc(lo, mid, x * 2.0f) : mixc(mid, hi, x * 2.0f - 1.0f);
}

/* fg over bg, a in 0..32 */
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint32_t a)
{
    uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
    uint32_t f = ((uint32_t)fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
    uint32_t r = (b + (((f - b) * a) >> 5)) & 0x07E0F81Fu;
    return (uint16_t)(r | (r >> 16));
}

/* bg darkened, k in 0..32 (32 = untouched) */
static inline uint16_t shade565(uint16_t p, uint32_t k)
{
    uint32_t b = ((uint32_t)p | ((uint32_t)p << 16)) & 0x07E0F81Fu;
    b = ((b * k) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(b | (b >> 16));
}

static inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* --------------------------------------------------------------------------
 * Noise: 128 x 128 that tiles, four octaves of value noise
 * -------------------------------------------------------------------------- */

static void make_noise(uint8_t *nz, uint32_t seed)
{
    float *acc = cn_draw_alloc(128 * 128 * sizeof(float));
    if (!acc) {
        for (int i = 0; i < 128 * 128; i++) nz[i] = (uint8_t)hash32((uint32_t)i ^ seed);
        return;
    }
    memset(acc, 0, 128 * 128 * sizeof(float));
    float amp = 1.0f;
    for (int oct = 0, period = 32; oct < 4; oct++, period >>= 1, amp *= 0.5f) {
        int cells = 128 / period;
        for (int y = 0; y < 128; y++) {
            int iy = y / period;
            float fy = (float)(y % period) / (float)period;
            fy = fy * fy * (3.0f - 2.0f * fy);
            for (int x = 0; x < 128; x++) {
                int ix = x / period;
                float fx = (float)(x % period) / (float)period;
                fx = fx * fx * (3.0f - 2.0f * fx);
                uint32_t s = seed + (uint32_t)oct * 7919u;
                float v00 = (float)(hash32(s + (uint32_t)(iy * 131 + ix)) & 1023);
                float v10 = (float)(hash32(s + (uint32_t)(iy * 131 + (ix + 1) % cells)) & 1023);
                float v01 = (float)(hash32(s + (uint32_t)(((iy + 1) % cells) * 131 + ix)) & 1023);
                float v11 = (float)(hash32(s + (uint32_t)(((iy + 1) % cells) * 131 + (ix + 1) % cells)) & 1023);
                float a = v00 + (v10 - v00) * fx, b = v01 + (v11 - v01) * fx;
                acc[y * 128 + x] += (a + (b - a) * fy) * amp;
            }
        }
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 128 * 128; i++) {
        if (acc[i] < lo) lo = acc[i];
        if (acc[i] > hi) hi = acc[i];
    }
    float k = hi > lo ? 255.0f / (hi - lo) : 0;
    for (int i = 0; i < 128 * 128; i++) nz[i] = (uint8_t)((acc[i] - lo) * k);
    free(acc);
}

/* bilinear, 0..1 */
static float nsamp(const uint8_t *nz, float x, float y)
{
    float fx0 = floorf(x), fy0 = floorf(y);
    int ix = (int)fx0, iy = (int)fy0;
    float fx = x - fx0, fy = y - fy0;
    int x0 = ix & 127, y0 = iy & 127, x1 = (ix + 1) & 127, y1 = (iy + 1) & 127;
    float a = nz[y0 * 128 + x0], b = nz[y0 * 128 + x1], c = nz[y1 * 128 + x0], e = nz[y1 * 128 + x1];
    float t = a + (b - a) * fx, u = c + (e - c) * fx;
    return (t + (u - t) * fy) / 255.0f;
}

/* --------------------------------------------------------------------------
 * Geometry: where a pixel is in each channel of a shape
 * -------------------------------------------------------------------------- */

typedef struct {
    float d;        /* across: signed distance from the centre line, cells */
    float s;        /* along, cells from the channel's entry               */
    float len;
    float ax, ay;   /* the direction 'd' grows, canonical                  */
    int   ok;
} chv_t;

/* Canonical shapes (rot 0): the straight runs N-S, the bend joins N and E,
 * the source and the drain open to the N. x right, y down, cells from the
 * cell's centre. */
static int eval_canon(int shape, float x, float y, chv_t *c)
{
    switch (shape) {
    case CN_SH_STRAIGHT:
        c[0] = (chv_t){ x, y + 0.5f, 1.0f, 1, 0, 1 };
        return 1;
    case CN_SH_BEND: {
        float vx = x - 0.5f, vy = y + 0.5f;
        float rho = sqrtf(vx * vx + vy * vy);
        float inv = rho > 1e-4f ? 1.0f / rho : 0;
        c[0].d = rho - 0.5f;
        c[0].ax = vx * inv;
        c[0].ay = vy * inv;
        c[0].s = atan2f(vy, -vx) * 0.5f;
        c[0].len = PI_4;
        c[0].ok = 1;
        return 1;
    }
    case CN_SH_CROSS:
        c[0] = (chv_t){ x, y + 0.5f, 1.0f, 1, 0, 1 };
        c[1] = (chv_t){ y, 0.5f - x, 1.0f, 0, 1, 1 };
        return 2;
    case CN_SH_SOURCE:
        c[0] = (chv_t){ x, -y, 0.5f, 1, 0, y <= 0.02f };
        return 1;
    case CN_SH_DRAIN:
        c[0] = (chv_t){ x, y + 0.5f, 0.5f, 1, 0, y <= 0.02f };
        return 1;
    default:
        return 0;
    }
}

/* screen point -> canonical: turn it back counterclockwise */
static void to_canon(float *x, float *y, int rot)
{
    for (int i = 0; i < (rot & 3); i++) {
        float t = *x;
        *x = *y;
        *y = -t;
    }
}

/* canonical direction -> screen: clockwise */
static void to_screen(float *x, float *y, int rot)
{
    for (int i = 0; i < (rot & 3); i++) {
        float t = *x;
        *x = -*y;
        *y = t;
    }
}

static int rots_of(int shape)
{
    switch (shape) {
    case CN_SH_STRAIGHT: return 2;
    case CN_SH_BEND:
    case CN_SH_SOURCE:
    case CN_SH_DRAIN:    return 4;
    case CN_SH_CROSS:
    case CN_SH_BLOCK:    return 1;
    default:             return 0;
    }
}

/* The channel pixels of a shape at a turn, as a list. 'out' NULL: count. */
static int make_geo(cn_gpx_t *out, int shape, int rot, int C)
{
    float inv = 1.0f / (float)C, half = 0.5f * inv;
    int n = 0;
    for (int py = 0; py < C; py++) {
        for (int px = 0; px < C; px++) {
            float x = (px + 0.5f) * inv - 0.5f, y = (py + 0.5f) * inv - 0.5f;
            to_canon(&x, &y, rot);
            chv_t c[2];
            int nc = eval_canon(shape, x, y, c);
            for (int k = 0; k < nc; k++) {
                if (!c[k].ok || fabsf(c[k].d) >= RI + half) continue;
                if (out) {
                    cn_gpx_t *o = &out[n];
                    float uu = clampf(c[k].d / RI, -1, 1);
                    float ax = c[k].ax, ay = c[k].ay;
                    to_screen(&ax, &ay, rot);
                    float nx = ax * uu, ny = ay * uu, nzz = sqrtf(fmaxf(0, 1 - uu * uu));
                    float dif = fmaxf(0, nx * LX + ny * LY + nzz * LZ);
                    float sp = powf(fmaxf(0, nx * HX + ny * HY + nzz * HZ), 28.0f);
                    o->x = (uint8_t)px;
                    o->y = (uint8_t)py;
                    o->u = (int8_t)(uu * 127.0f);
                    o->lit = (uint8_t)(clampf(dif, 0, 1) * 255.0f);
                    o->spec = (uint8_t)(clampf(sp, 0, 1) * 255.0f);
                    o->ch = (uint8_t)k;
                    float s = clampf(c[k].s, 0, c[k].len) * (float)C * 16.0f;
                    o->s = (uint16_t)(s > 65535.0f ? 65535.0f : s);
                }
                n++;
            }
        }
    }
    return n;
}

/* --------------------------------------------------------------------------
 * The pipes' sprites
 * -------------------------------------------------------------------------- */

static void over(col_t *c, float *a, col_t c2, float a2)
{
    if (a2 <= 0) return;
    float na = a2 + *a * (1 - a2);
    if (na <= 0) return;
    c->r = (c2.r * a2 + c->r * *a * (1 - a2)) / na;
    c->g = (c2.g * a2 + c->g * *a * (1 - a2)) / na;
    c->b = (c2.b * a2 + c->b * *a * (1 - a2)) / na;
    *a = na;
}

/* a metal surface with normal n */
static col_t metal(const cn_world_t *w, float nx, float ny, float nz, float gloss)
{
    float dif = fmaxf(0, nx * LX + ny * LY + nz * LZ);
    float sp = powf(fmaxf(0, nx * HX + ny * HY + nz * HZ), 20.0f);
    col_t c = ramp3(col(w->p_lo), col(w->p_mid), col(w->p_hi), 0.12f + 0.88f * dif);
    return addc(c, sp * gloss);
}

static col_t accent(const cn_world_t *w, float nx, float ny, float nz)
{
    float dif = fmaxf(0, nx * LX + ny * LY + nz * LZ);
    float sp = powf(fmaxf(0, nx * HX + ny * HY + nz * HZ), 24.0f);
    col_t a = col(w->p_accent);
    return addc(mixc(scalec(a, 0.35f), a, clampf(0.2f + dif, 0, 1.2f)), sp * 0.5f);
}

/* the wall's colour, with the world's material on it; (gx, gy) are cells on
 * the board, for the material's own noise */
static col_t wall_style(const cn_world_t *w, const uint8_t *nz, col_t c, float *alpha, float uo,
                        float sp, float gx, float gy, float along)
{
    switch (w->pipe) {
    case CN_PIPE_GLASS: {
        float e = fabsf(uo);
        e = e * e * e;
        *alpha *= 0.22f + 0.6f * e;
        return addc(mixc(col(w->p_mid), col(0xFFFFFF), e * 0.5f), sp * 0.9f);
    }
    case CN_PIPE_RUST: {
        float n = nsamp(nz, gx * 9.0f, gy * 9.0f);
        float m = nsamp(nz, gx * 31.0f + 50, gy * 31.0f);
        if (n > 0.52f) c = mixc(c, scalec(col(0x9A4A1A), 0.6f + 0.6f * m), clampf((n - 0.52f) * 4, 0, 0.85f));
        if (m > 0.8f) c = scalec(c, 0.7f);
        return c;
    }
    case CN_PIPE_CERAMIC: {
        float n = nsamp(nz, gx * 24.0f, gy * 24.0f);
        c = scalec(c, 0.85f + 0.3f * n);
        float j = along * 4.0f;
        j -= floorf(j);
        if (j < 0.05f || j > 0.95f) c = scalec(c, 0.55f);
        return c;
    }
    case CN_PIPE_FROST: {
        float f = nsamp(nz, gx * 7.0f + 20, gy * 7.0f);
        float g = nsamp(nz, gx * 40.0f, gy * 40.0f + 9);
        if (f > 0.45f) c = mixc(c, col(0xF2F8FF), clampf((f - 0.45f) * 3, 0, 0.85f) * (0.7f + 0.3f * g));
        if (g > 0.86f) c = addc(c, 0.25f);
        return c;
    }
    default:
        return c;
    }
}

static void pipe_px(const cn_world_t *w, const uint8_t *nz, int shape, int rot, float sx, float sy,
                    float px, float gx, float gy, col_t *out, float *alpha)
{
    col_t c = { 0, 0, 0 };
    float a = 0;
    float x = sx, y = sy;
    to_canon(&x, &y, rot);

    if (shape == CN_SH_BLOCK) {
        *out = c;
        *alpha = 0;
        return;
    }

    chv_t ch[2];
    int n = eval_canon(shape, x, y, ch);

    /* the cross's hub: a cast box */
    if (shape == CN_SH_CROSS) {
        float hb = RO + 0.05f, rr = 0.06f;
        float qx = fabsf(sx) - hb + rr, qy = fabsf(sy) - hb + rr;
        float out_d = sqrtf(fmaxf(qx, 0) * fmaxf(qx, 0) + fmaxf(qy, 0) * fmaxf(qy, 0));
        float sdf = out_d + fminf(fmaxf(qx, qy), 0) - rr;
        if (sdf < px) {
            float nx = 0, ny = 0, nzz = 1;
            if (sdf > -0.035f) {
                float t = (sdf + 0.035f) / 0.035f;
                if (qx > qy) nx = (sx > 0 ? 1.0f : -1.0f) * t * 0.7f;
                else ny = (sy > 0 ? 1.0f : -1.0f) * t * 0.7f;
                nzz = sqrtf(fmaxf(0.05f, 1 - nx * nx - ny * ny));
            }
            col_t m = metal(w, nx, ny, nzz, 0.5f);
            float ma = clampf(0.5f - sdf / px, 0, 1);
            if (w->pipe == CN_PIPE_GLASS) {
                m = accent(w, nx, ny, nzz);
            } else if (w->pipe == CN_PIPE_BANDS) {
                float st = (sx + sy) * 9.0f;
                st -= floorf(st);
                if (sdf > -0.035f) m = st < 0.5f ? accent(w, nx, ny, nzz) : scalec(m, 0.25f);
            } else {
                m = wall_style(w, nz, m, &ma, 0, 0, gx, gy, 0.1f);
            }
            over(&c, &a, m, ma);
        }
    }

    /* walls and couplings */
    for (int k = 0; k < n; k++) {
        if (!ch[k].ok) continue;
        float ad = fabsf(ch[k].d);
        float s = ch[k].s, len = ch[k].len;
        bool end0 = shape != CN_SH_SOURCE, end1 = shape != CN_SH_DRAIN;
        bool flange = (end0 && s < FLW) || (end1 && s > len - FLW);
        float lim = flange ? RFL : RO;
        if (ad >= lim + px) continue;
        float ax = ch[k].ax, ay = ch[k].ay;
        to_screen(&ax, &ay, rot);
        float uo = clampf(ch[k].d / lim, -1, 1);
        float nx = ax * uo, ny = ay * uo, nzz = sqrtf(fmaxf(0, 1 - uo * uo));
        float sp = powf(fmaxf(0, nx * HX + ny * HY + nzz * HZ), 20.0f);
        float wa = clampf((lim - ad) / px + 0.5f, 0, 1);
        col_t m;
        if (flange) {
            if (w->pipe == CN_PIPE_BANDS) {
                float st = (sx - sy) * 9.0f;
                st -= floorf(st);
                m = st < 0.5f ? accent(w, nx, ny, nzz) : scalec(metal(w, nx, ny, nzz, 0.3f), 0.25f);
            } else if (w->pipe == CN_PIPE_GLASS) {
                m = accent(w, nx, ny, nzz);
            } else {
                m = scalec(metal(w, nx, ny, nzz, 0.6f), 0.8f);
                m = wall_style(w, nz, m, &wa, uo, sp, gx, gy, s);
            }
            /* two bolts on each coupling */
            float bs = s < FLW ? FLW * 0.5f : len - FLW * 0.5f;
            for (int side = -1; side <= 1; side += 2) {
                float bd = (float)side * (RFL - 0.035f);
                float dx = ch[k].d - bd, dy = s - bs;
                float br = 0.02f, d2 = dx * dx + dy * dy;
                if (d2 < br * br) {
                    float t = sqrtf(d2) / br;
                    float bx = ax * (dx / br), by = ay * (dx / br);
                    m = addc(metal(w, bx * 0.8f, by * 0.8f - dy / br * 0.3f, sqrtf(fmaxf(0.1f, 1 - t * t)), 0.9f), 0.05f);
                }
            }
        } else {
            m = metal(w, nx, ny, nzz, 0.55f);
            m = wall_style(w, nz, m, &wa, uo, sp, gx, gy, s);
        }
        over(&c, &a, m, wa);
    }

    /* the inside of the channel: a dark gutter, lit the other way round */
    for (int k = 0; k < n; k++) {
        if (!ch[k].ok) continue;
        float ad = fabsf(ch[k].d);
        if (ad >= RI + px) continue;
        float ax = ch[k].ax, ay = ch[k].ay;
        to_screen(&ax, &ay, rot);
        float ui = clampf(ch[k].d / RI, -1, 1);
        float nx = -ax * ui, ny = -ay * ui, nzz = sqrtf(fmaxf(0, 1 - ui * ui));
        float dif = fmaxf(0, nx * LX + ny * LY + nzz * LZ);
        float ia = clampf((RI - ad) / px + 0.5f, 0, 1);
        col_t in = scalec(col(w->p_inner), 0.45f + 0.8f * dif);
        if (w->pipe == CN_PIPE_GLASS) {
            in = col(w->p_mid);
            ia *= 0.10f;
        } else if (w->pipe == CN_PIPE_CERAMIC) {
            float r = ch[k].s * 10.0f;
            r -= floorf(r);
            if (r < 0.12f) in = scalec(in, 0.6f);
        }
        over(&c, &a, in, ia);
    }

    /* the source's tank, the drain's bowl */
    if (shape == CN_SH_SOURCE || shape == CN_SH_DRAIN) {
        float rho = sqrtf(sx * sx + sy * sy);
        float r_out = shape == CN_SH_SOURCE ? 0.39f : 0.345f;
        float r_in = shape == CN_SH_SOURCE ? 0.325f : 0.29f;
        if (rho < r_out + px) {
            float dx = rho > 1e-4f ? sx / rho : 0, dy = rho > 1e-4f ? sy / rho : 0;
            if (rho > r_in) {
                float ur = clampf((rho - (r_in + r_out) * 0.5f) / ((r_out - r_in) * 0.5f), -1, 1);
                col_t m = metal(w, dx * ur, dy * ur, sqrtf(fmaxf(0, 1 - ur * ur)), 0.8f);
                if (w->pipe == CN_PIPE_BANDS || w->pipe == CN_PIPE_GLASS) {
                    m = mixc(m, accent(w, dx * ur, dy * ur, sqrtf(fmaxf(0, 1 - ur * ur))), 0.6f);
                }
                over(&c, &a, m, clampf((r_out - rho) / px + 0.5f, 0, 1));
            } else if (shape == CN_SH_SOURCE) {
                over(&c, &a, scalec(col(w->p_inner), 0.6f), 1);
            } else {
                /* a funnel with a grate over the hole */
                col_t f = scalec(col(w->p_inner), 0.25f + 0.75f * (rho / r_in));
                float ang = atan2f(sy, sx);
                float sl = (ang + PI_F) * 6.0f / (2.0f * PI_F);
                sl -= floorf(sl);
                if (rho > 0.07f && rho < r_in - 0.02f && sl < 0.38f) {
                    float bx = (sl - 0.19f) / 0.19f;
                    f = metal(w, -dy * bx, dx * bx, sqrtf(fmaxf(0, 1 - bx * bx)), 0.5f);
                    f = scalec(f, 0.75f);
                }
                if (rho < 0.06f) f = col(0x000000);
                over(&c, &a, f, 1);
            }
        }
    }
    *out = c;
    *alpha = a;
}

/* A crate, a cabinet, a block of ice or a rock: something in the way. */
static void block_px(const cn_world_t *w, const uint8_t *nz, float sx, float sy, float px,
                     float gx, float gy, col_t *out, float *alpha)
{
    col_t c = { 0, 0, 0 };
    float a = 0;
    bool rock = w->ground == CN_GROUND_SAND || w->ground == CN_GROUND_BASALT;
    if (rock) {
        float ang = atan2f(sy, sx);
        float rho = sqrtf(sx * sx + sy * sy);
        float R = 0.34f + 0.09f * (nsamp(nz, (ang + PI_F) * 5.0f, 3.0f) - 0.5f) * 2.0f;
        if (rho < R + px) {
            float t = clampf(rho / R, 0, 1);
            float nzz = sqrtf(fmaxf(0, 1 - t * t));
            float bump = nsamp(nz, sx * 30 + 40, sy * 30) - 0.5f;
            float nx = sx / R + bump * 0.4f, ny = sy / R + bump * 0.4f;
            float dif = fmaxf(0, nx * LX + ny * LY + nzz * LZ);
            col_t base = w->ground == CN_GROUND_BASALT ? col(0x4A4440) : col(0x8A7A62);
            float grain = nsamp(nz, sx * 40 + 9, sy * 40 + 3);
            col_t m = scalec(base, (0.3f + 0.9f * dif) * (0.85f + 0.3f * grain));
            if (w->ground == CN_GROUND_BASALT) {
                float cr = fabsf(nsamp(nz, sx * 7 + 31, sy * 7 + 70) - 0.5f);
                if (cr < 0.012f) m = mixc(m, col(0xFF7020), 0.85f * (1.0f - cr / 0.012f));
            }
            over(&c, &a, m, clampf((R - rho) / px + 0.5f, 0, 1));
        }
    } else {
        float hb = 0.40f, rr = 0.08f;
        float qx = fabsf(sx) - hb + rr, qy = fabsf(sy) - hb + rr;
        float sdf = sqrtf(fmaxf(qx, 0) * fmaxf(qx, 0) + fmaxf(qy, 0) * fmaxf(qy, 0)) +
                    fminf(fmaxf(qx, qy), 0) - rr;
        if (sdf < px) {
            float nx = 0, ny = 0;
            if (sdf > -0.06f) {
                float t = (sdf + 0.06f) / 0.06f * 0.75f;
                if (qx > qy) nx = (sx > 0 ? t : -t);
                else ny = (sy > 0 ? t : -t);
            }
            float nzz = sqrtf(fmaxf(0.05f, 1 - nx * nx - ny * ny));
            float ba = clampf(0.5f - sdf / px, 0, 1);
            col_t m;
            switch (w->ground) {
            case CN_GROUND_LAB: {
                m = scalec(col(0xD8E2DE), 0.35f + 0.75f * fmaxf(0, nx * LX + ny * LY + nzz * LZ));
                /* vents and a status light */
                if (sdf < -0.06f && fabsf(sx) < 0.22f && sy > -0.05f && sy < 0.22f) {
                    float v = sy * 22.0f;
                    v -= floorf(v);
                    if (v < 0.4f) m = scalec(m, 0.55f);
                }
                float lx = sx - 0.2f, ly = sy + 0.2f;
                if (lx * lx + ly * ly < 0.045f * 0.045f) m = col(0x9CFF40);
                break;
            }
            case CN_GROUND_ICE: {
                m = scalec(col(0xB0E4F4), 0.5f + 0.7f * fmaxf(0, nx * LX + ny * LY + nzz * LZ));
                float st = nsamp(nz, sx * 5 + sy * 12, sy * 3);
                if (st > 0.6f) m = addc(m, (st - 0.6f));
                ba *= 0.88f;
                break;
            }
            default: {
                m = metal(w, nx, ny, nzz, 0.4f);
                m = scalec(m, 0.7f);
                if (sdf < -0.06f) {
                    if (w->ground == CN_GROUND_TREAD) {
                        float st = (sx + sy) * 6.0f;
                        st -= floorf(st);
                        m = st < 0.5f ? accent(w, 0, 0, 1) : col(0x1A1A1A);
                    } else {
                        /* a crate's braces */
                        float d1 = fabsf(sx - sy), d2 = fabsf(sx + sy);
                        if (d1 < 0.04f || d2 < 0.04f) m = metal(w, 0, 0, 1, 0.2f);
                        else m = scalec(m, 0.75f);
                    }
                }
                break;
            }
            }
            (void)gx;
            (void)gy;
            over(&c, &a, m, ba);
        }
    }
    *out = c;
    *alpha = a;
}

/* The plate a bolted pipe stands on: it cannot be turned or taken. */
static void plate_px(const cn_world_t *w, float sx, float sy, float px, col_t *out, float *alpha)
{
    float hb = 0.465f, rr = 0.07f;
    float qx = fabsf(sx) - hb + rr, qy = fabsf(sy) - hb + rr;
    float sdf = sqrtf(fmaxf(qx, 0) * fmaxf(qx, 0) + fmaxf(qy, 0) * fmaxf(qy, 0)) +
                fminf(fmaxf(qx, qy), 0) - rr;
    *alpha = 0;
    *out = (col_t){ 0, 0, 0 };
    if (sdf >= px) return;
    float nx = 0, ny = 0;
    if (sdf > -0.03f) {
        float t = (sdf + 0.03f) / 0.03f * 0.7f;
        if (qx > qy) nx = (sx > 0 ? t : -t);
        else ny = (sy > 0 ? t : -t);
    }
    float nzz = sqrtf(fmaxf(0.05f, 1 - nx * nx - ny * ny));
    float dif = fmaxf(0, nx * LX + ny * LY + nzz * LZ);
    col_t base = mixc(col(w->g_line), col(w->p_lo), 0.55f);
    col_t c = scalec(base, 0.55f + 0.7f * dif);
    for (int i = 0; i < 4; i++) {
        float bx = sx - ((i & 1) ? 0.37f : -0.37f), by = sy - ((i & 2) ? 0.37f : -0.37f);
        float r = 0.04f, d2 = bx * bx + by * by;
        if (d2 < r * r) {
            float t = sqrtf(d2) / r;
            c = metal(w, bx / r * 0.8f, by / r * 0.8f, sqrtf(fmaxf(0.1f, 1 - t * t)), 0.9f);
        }
    }
    *out = c;
    *alpha = clampf(0.5f - sdf / px, 0, 1);
}

static void make_sprite(const cn_world_t *w, const uint8_t *nz, int shape, int rot, int C,
                        uint16_t *rgb, uint8_t *alpha)
{
    float inv = 1.0f / (float)C;
    for (int py = 0; py < C; py++) {
        for (int px = 0; px < C; px++) {
            float sx = (px + 0.5f) * inv - 0.5f, sy = (py + 0.5f) * inv - 0.5f;
            col_t c = { 0, 0, 0 };
            float a = 0;
            if (shape == CN_SH_BLOCK) block_px(w, nz, sx, sy, inv, sx, sy, &c, &a);
            else if (shape == CN_SH_NONE) plate_px(w, sx, sy, inv, &c, &a);
            else pipe_px(w, nz, shape, rot, sx, sy, inv, sx + 0.37f * shape, sy + 0.21f * rot, &c, &a);
            rgb[py * C + px] = to565(c);
            alpha[py * C + px] = (uint8_t)(clampf(a, 0, 1) * 255.0f + 0.5f);
        }
    }
}

/* --------------------------------------------------------------------------
 * The floor
 * -------------------------------------------------------------------------- */

static uint16_t floor_px(const cn_world_t *w, const uint8_t *nz, int x, int y, int C)
{
    float gx = (float)x / (float)C, gy = (float)y / (float)C;
    col_t a = col(w->g_a), b = col(w->g_b), line = col(w->g_line);
    float n1 = nsamp(nz, gx * 9.0f, gy * 9.0f);
    float n2 = nsamp(nz, gx * 37.0f + 11, gy * 37.0f + 5);
    col_t c;
    switch (w->ground) {
    case CN_GROUND_TILES: {
        float t = gx * 2.0f, u = gy * 2.0f;
        int ti = (int)floorf(t), tj = (int)floorf(u);
        float ft = t - ti, fu = u - tj;
        float v = (float)(hash32((uint32_t)(ti * 73 + tj * 151)) & 255) / 255.0f;
        c = mixc(a, b, 0.3f + 0.5f * v + 0.3f * (n2 - 0.5f));
        float wet = nsamp(nz, gx * 2.5f + 3, gy * 2.5f);
        if (wet > 0.6f) c = mixc(c, scalec(c, 0.75f), (wet - 0.6f) * 2.5f);
        float e = fminf(fminf(ft, 1 - ft), fminf(fu, 1 - fu)) * (float)C * 0.5f;
        if (e < 1.5f) c = line;
        else if (e < 3.0f) c = mixc(c, scalec(c, 1.15f), 0.6f);
        break;
    }
    case CN_GROUND_TREAD: {
        c = mixc(a, b, n1 * 0.8f + n2 * 0.2f);
        float P = 5.0f;
        float t = gx * P, u = gy * P;
        int ti = (int)floorf(t), tj = (int)floorf(u);
        float du = t - ti - 0.5f, dv = u - tj - 0.5f;
        float ca = ((ti + tj) & 1) ? 0.7071f : -0.7071f;
        float ra = du * 0.7071f + dv * ca, rb = -du * ca + dv * 0.7071f;
        float bump = 1.0f - (fabsf(ra) * 2.4f + fabsf(rb) * 0.75f);
        if (bump > 0.45f) {
            float sh = (ra * 0.7071f - rb * ca) < 0 ? 1.3f : 0.7f;
            c = scalec(c, sh);
        }
        break;
    }
    case CN_GROUND_LAB: {
        float t = gx * 3.0f, u = gy * 3.0f;
        float ft = t - floorf(t), fu = u - floorf(u);
        c = mixc(a, b, 0.4f + 0.4f * (n1 - 0.5f));
        float e = fminf(fminf(ft, 1 - ft), fminf(fu, 1 - fu)) * (float)C / 3.0f;
        if (e < 1.0f) c = line;
        else if (ft < 0.5f && fu < 0.5f) c = addc(c, 0.03f);
        break;
    }
    case CN_GROUND_SAND: {
        c = mixc(a, b, n1 * 0.7f + n2 * 0.3f);
        float g = (float)(hash32((uint32_t)(x * 7919 + y * 104729)) & 255) / 255.0f;
        c = addc(c, (g - 0.5f) * 0.08f);
        float peb = nsamp(nz, gx * 14.0f + 60, gy * 14.0f);
        if (peb > 0.78f) c = scalec(c, 0.75f);
        break;
    }
    case CN_GROUND_BASALT: {
        c = mixc(a, b, n1);
        c = addc(c, (n2 - 0.5f) * 0.06f);
        float cr = fabsf(nsamp(nz, gx * 3.0f + 17, gy * 3.0f) - 0.5f);
        if (cr < 0.025f) c = mixc(c, col(0x701808), (0.025f - cr) * 40.0f);
        else if (cr < 0.04f) c = mixc(c, col(0x2A0C04), 0.4f);
        break;
    }
    default: { /* ice */
        c = mixc(a, b, n1);
        float fr = nsamp(nz, gx * 5.0f, gy * 5.0f + 33);
        if (fr > 0.55f) c = mixc(c, col(0xC8E0F0), (fr - 0.55f) * 1.4f);
        float sc = (gx * 1.7f + gy * 0.6f) * 6.0f;
        float h = (float)(hash32((uint32_t)floorf(sc)) & 255) / 255.0f;
        if (h > 0.85f && fabsf(sc - floorf(sc) - 0.5f) < 0.02f) c = addc(c, 0.12f);
        break;
    }
    }
    return to565(c);
}

/* --------------------------------------------------------------------------
 * Setting up a board
 * -------------------------------------------------------------------------- */

static void make_tables(cn_draw_t *d);

static void make_ramp(cn_draw_t *d)
{
    const cn_world_t *w = d->w;
    col_t deep = col(w->c_deep), mid = col(w->c_mid), light = col(w->c_light), spec = col(w->c_spec);
    for (int i = 0; i < 256; i++) {
        float x = (float)i / 255.0f;
        col_t c;
        if (x < 0.45f) c = mixc(deep, mid, x / 0.45f);
        else if (x < 0.8f) c = mixc(mid, light, (x - 0.45f) / 0.35f);
        else c = mixc(light, spec, (x - 0.8f) / 0.2f);
        d->ramp[i] = to565(c);
    }
}

void cn_draw_free(cn_draw_t *d)
{
    free(d->fb);
    free(d->floor);
    free(d->base);
    free(d->field);
    free(d->row_tmp);
    free(d->pol_a);
    free(d->pol_r);
    free(d->bub);
    free(d->fiz);
    free(d->plate.rgb);
    free(d->plate.a);
    for (int s = 0; s < CN_SHAPES; s++) {
        for (int r = 0; r < 4; r++) {
            free(d->spr[s][r].rgb);
            free(d->spr[s][r].a);
            free(d->geo[s][r].px);
        }
    }
    memset(d, 0, sizeof *d);
}

bool cn_draw_init(cn_draw_t *d, int world, int cols, int rows, int cell)
{
    memset(d, 0, sizeof *d);
    d->world = world;
    d->w = cn_world(world);
    d->C = cell;
    d->cols = cols;
    d->rows = rows;
    d->W = cols * cell;
    d->H = rows * cell;
    d->ri_px = RI * (float)cell;
    size_t px = (size_t)d->W * (size_t)d->H;
    size_t cc = (size_t)cell * (size_t)cell;
    d->fb = cn_draw_alloc(px * 2);
    d->floor = cn_draw_alloc(px * 2);
    d->base = cn_draw_alloc(px * 2);
    d->field = cn_draw_alloc(px);
    d->row_tmp = cn_draw_alloc((size_t)d->W * 2 + 8);
    d->plate.rgb = cn_draw_alloc(cc * 2);
    d->plate.a = cn_draw_alloc(cc);
    d->pol_a = cn_draw_alloc(cc);
    d->pol_r = cn_draw_alloc(cc);
    d->bub = cn_draw_alloc(CN_BUBS * sizeof(cn_bub_t));
    d->fiz = cn_draw_alloc(CN_BUBS * sizeof(cn_bub_t));
    if (!d->fb || !d->floor || !d->base || !d->field || !d->row_tmp || !d->plate.rgb || !d->plate.a ||
        !d->pol_a || !d->pol_r || !d->bub || !d->fiz) {
        cn_draw_free(d);
        return false;
    }
    memset(d->field, 0, px);
    d->fx1 = d->fx0 - 1;
    d->px1 = d->px0 - 1;
    make_noise(d->noise, 0xC0FFEEu + (uint32_t)world * 977u);
    make_ramp(d);
    make_tables(d);
    for (int y = 0; y < cell; y++) {
        for (int x = 0; x < cell; x++) {
            float sx = (x + 0.5f) / (float)cell - 0.5f, sy = (y + 0.5f) / (float)cell - 0.5f;
            float a = atan2f(sy, sx);
            if (a < 0) a += 2.0f * PI_F;
            d->pol_a[y * cell + x] = (uint8_t)((int)(a * (256.0f / (2.0f * PI_F))) & 255);
            float r = sqrtf(sx * sx + sy * sy) * 510.0f;
            d->pol_r[y * cell + x] = (uint8_t)(r > 255.0f ? 255.0f : r);
        }
    }

    for (int s = CN_SH_STRAIGHT; s < CN_SHAPES; s++) {
        if (s_straight_only && s != CN_SH_STRAIGHT) continue;
        for (int r = 0; r < rots_of(s); r++) {
            cn_spr_t *sp = &d->spr[s][r];
            sp->rgb = cn_draw_alloc(cc * 2);
            sp->a = cn_draw_alloc(cc);
            if (!sp->rgb || !sp->a) {
                cn_draw_free(d);
                return false;
            }
            make_sprite(d->w, d->noise, s, r, cell, sp->rgb, sp->a);
            if (s != CN_SH_BLOCK) {
                int n = make_geo(NULL, s, r, cell);
                d->geo[s][r].px = cn_draw_alloc((size_t)n * sizeof(cn_gpx_t));
                if (!d->geo[s][r].px) {
                    cn_draw_free(d);
                    return false;
                }
                d->geo[s][r].n = make_geo(d->geo[s][r].px, s, r, cell);
            }
        }
    }
    make_sprite(d->w, d->noise, CN_SH_NONE, 0, cell, d->plate.rgb, d->plate.a);

    for (int y = 0; y < d->H; y++) {
        for (int x = 0; x < d->W; x++) {
            d->floor[(size_t)y * d->W + x] = floor_px(d->w, d->noise, x, y, cell);
        }
    }
    memcpy(d->base, d->floor, px * 2);
    memcpy(d->fb, d->floor, px * 2);
    return true;
}

static const cn_spr_t *sprite_of(const cn_draw_t *d, int shape, int srot)
{
    int n = rots_of(shape);
    if (!n) return NULL;
    const cn_spr_t *s = &d->spr[shape][srot % n];
    return s->rgb ? s : NULL;
}

static const cn_geo_t *geo_of(const cn_draw_t *d, int shape, int srot)
{
    int n = rots_of(shape);
    if (!n || shape == CN_SH_BLOCK) return NULL;
    const cn_geo_t *g = &d->geo[shape][srot % n];
    return g->px ? g : NULL;
}

/* sprite over a C x C block of 'dst', with its shadow first */
static void put_sprite(const cn_draw_t *d, uint16_t *dst, int stride, const cn_spr_t *sp, bool shadow)
{
    int C = d->C;
    if (shadow) {
        int ox = C * 4 / 100 + 1, oy = C * 6 / 100 + 1;
        for (int y = oy; y < C; y++) {
            const uint8_t *ar = sp->a + (y - oy) * C;
            uint16_t *o = dst + y * stride;
            for (int x = ox; x < C; x++) {
                uint32_t a = ar[x - ox];
                if (a) o[x] = shade565(o[x], 32 - ((a * 14) >> 8));
            }
        }
    }
    for (int y = 0; y < C; y++) {
        const uint16_t *cr = sp->rgb + y * C;
        const uint8_t *ar = sp->a + y * C;
        uint16_t *o = dst + y * stride;
        for (int x = 0; x < C; x++) {
            uint32_t a = ar[x];
            if (a >= 250) o[x] = cr[x];
            else if (a > 4) o[x] = blend565(o[x], cr[x], (a + 4) >> 3);
        }
    }
}

void cn_draw_base(cn_draw_t *d, int sc, int sr, const cn_tile_t *t)
{
    int C = d->C;
    size_t off = (size_t)sr * C * d->W + (size_t)sc * C;
    for (int y = 0; y < C; y++) {
        memcpy(d->base + off + (size_t)y * d->W, d->floor + off + (size_t)y * d->W, (size_t)C * 2);
    }
    if (t->fixed && t->shape != CN_SH_BLOCK && t->shape != CN_SH_NONE) put_sprite(d, d->base + off, d->W, &d->plate, true);
    const cn_spr_t *sp = sprite_of(d, t->shape, t->srot);
    if (sp) put_sprite(d, d->base + off, d->W, sp, true);
}

/* The world's shading as tables: what the liquid loop adds per pixel. */
static void make_tables(cn_draw_t *d)
{
    const cn_world_t *w = d->w;
    for (int i = 0; i < 256; i++) {
        d->litg[i] = (int16_t)((i * 205) >> 8);
        d->texk[i] = (int16_t)(((i - 128) * w->tex) >> 7);
        int cc = 255 - i * 9;
        d->caus[i] = (int16_t)(cc > 0 ? (((((cc * cc) >> 8) * cc) >> 8) * w->caustics) >> 8 : 0);
        d->specg[i] = (int16_t)((w->spec * (192 + (i >> 2))) >> 8);
        float rho = fmaxf((float)i / 510.0f, 0.01f);
        d->logt[i] = (int16_t)(-logf(rho) * 2.4f * (128.0f / PI_F));
    }
    for (int au = 0; au < 128; au++) {
        float uf = (float)au / 127.0f;
        d->curv16[au] = (int32_t)(w->curv * d->ri_px * 16.0f * uf * uf);
        float sa = (float)(127 - au) * d->ri_px / 127.0f;
        d->edge16[au] = (int32_t)(clampf(sa, 0, 1) * 16.0f);
    }
}

/* One table of bubble slots for the whole run: slot k sits around
 * (k + 0.5) * sp + P along it, with a hash deciding if there is one, where
 * across, and how big. The liquid loop then only looks at two slots. */
static void make_slots(cn_bub_t *b, int *k0, int *n, float *inv, float P, float sp, int dens, float rmax,
                       float front, uint32_t salt)
{
    int kmin = (int)floorf((0.0f - P) / sp) - 2;
    int kmax = (int)floorf((front + sp - P) / sp) + 2;
    if (kmax - kmin + 1 > CN_BUBS) kmin = kmax - CN_BUBS + 1;
    *k0 = kmin;
    *n = kmax - kmin + 1;
    *inv = 1.0f / sp;
    for (int i = 0; i < *n; i++) {
        int k = kmin + i;
        uint32_t h = hash32((uint32_t)k * 0x9E3779B1u + salt);
        cn_bub_t *o = &b[i];
        o->on = (uint8_t)((int)(h & 255) < dens);
        if (!o->on) continue;
        float cs = ((float)k + 0.15f + (float)((h >> 8) & 255) * (0.7f / 255.0f)) * sp + P;
        float r = rmax * (0.55f + (float)((h >> 24) & 255) * (0.45f / 255.0f));
        o->cs = (int32_t)(cs * 16.0f);
        o->cu = (int16_t)((int)((h >> 16) & 255) - 128) * 72 / 100;
        o->r2o = (uint32_t)(r * 16.0f * r * 16.0f);
        float ri = r - 1.3f;
        o->r2i = ri > 0 ? (uint32_t)(ri * 16.0f * ri * 16.0f) : 0;
    }
}

void cn_draw_begin(cn_draw_t *d, float t, float phase_px, float front_px, float speed_px)
{
    const cn_world_t *w = d->w;
    float Cf = (float)d->C;
    d->t = t;
    d->phase_px = phase_px;
    d->front_px = front_px;
    d->speed_px = speed_px;
    d->F16 = (int32_t)(front_px * 16.0f);
    float amp = w->wobble * Cf / 100.0f;
    for (int i = 0; i < 64; i++) {
        float u = (float)i / 63.0f * 2.0f - 1.0f;
        float wob = amp * (sinf(t * 7.3f + u * 2.6f) * 0.6f + sinf(t * 11.1f - u * 4.3f) * 0.4f);
        d->wob16[i] = (int32_t)(wob * 16.0f);
    }
    /* the streaks: n1 22 texels a cell along, n2 34 and a little faster */
    float per1 = 128.0f * Cf / 22.0f, per2 = 128.0f * Cf / 34.0f;
    float P2 = phase_px * 1.6f + t * Cf * 0.08f;
    d->Pm1 = (int32_t)((fmodf(phase_px, per1) - per1) * 16.0f);
    d->Pm2 = (int32_t)((fmodf(P2, per2) - per2) * 16.0f);
    d->kq1 = (int32_t)(22.0f / Cf / 16.0f * 65536.0f);
    d->kq2 = (int32_t)(34.0f / Cf / 16.0f * 65536.0f);
    /* bubbles a little faster than the liquid, fizz faster still (it rises) */
    float Pb = phase_px * 1.15f, Pf = phase_px * 1.5f + t * Cf * 0.5f;
    d->Pb16 = (int32_t)(Pb * 16.0f);
    d->Pf16 = (int32_t)(Pf * 16.0f);
    d->bub_n = d->fiz_n = 0;
    if (w->bubbles) {
        make_slots(d->bub, &d->bub_k0, &d->bub_n, &d->bub_inv, Pb, Cf * 0.42f, w->bubbles,
                   (float)w->bubble_r * Cf / 100.0f, front_px, 0x51ED27u);
    }
    if (w->fizz) {
        make_slots(d->fiz, &d->fiz_k0, &d->fiz_n, &d->fiz_inv, Pf, Cf * 0.11f, w->fizz,
                   1.2f * Cf / 100.0f + 0.4f, front_px, 0xA3C59AC3u);
    }
}

/* --------------------------------------------------------------------------
 * The liquid
 * -------------------------------------------------------------------------- */

static const uint16_t RAINBOW[8] = { 0xF81F, 0x801F, 0x041F, 0x07FF, 0x07E0, 0xFFE0, 0xFC00, 0xF800 };

/* 0 none, 1 inside a bubble, 2 its rim. S along the run and P in 1/16 px. */
static inline int bubble_at(const cn_bub_t *tab, int k0, int n, float inv, int32_t S, int32_t P, int u,
                            int32_t riq)
{
    int k = (int)((float)(S - P) * (1.0f / 16.0f) * inv + 4096.0f) - 4096;
    for (int j = k - 1; j <= k; j++) {
        int i = j - k0;
        if (i < 0 || i >= n || !tab[i].on) continue;
        int32_t ds = S - tab[i].cs;
        if ((uint32_t)(ds < 0 ? -ds : ds) * (uint32_t)(ds < 0 ? -ds : ds) >= tab[i].r2o) continue;
        int32_t du = ((u - tab[i].cu) * riq) >> 8;
        uint32_t d2 = (uint32_t)(ds * ds + du * du);
        if (d2 < tab[i].r2o) return d2 >= tab[i].r2i ? 2 : 1;
    }
    return 0;
}

/* The liquid in a cell's channels: integers only, from the shape's list of
 * channel pixels and the frame's tables. */
static void draw_liquid(cn_draw_t *d, uint16_t *dst, const cn_tile_t *t)
{
    const cn_geo_t *geo = geo_of(d, t->shape, t->srot);
    if (!geo) return;
    if (!t->ch[0].on && !t->ch[1].on) return;
    const cn_world_t *w = d->w;
    int W = d->W;
    float Cf = (float)d->C;
    int32_t len16 = (int32_t)(((t->shape == CN_SH_SOURCE || t->shape == CN_SH_DRAIN) ? Cf * 0.5f
                               : t->shape == CN_SH_BEND ? Cf * PI_4 : Cf) * 16.0f);
    int32_t start16[2] = { (int32_t)(t->ch[0].start_px * 16.0f), (int32_t)(t->ch[1].start_px * 16.0f) };
    uint8_t on[2] = { t->ch[0].on, t->ch[1].on }, rev[2] = { t->ch[0].rev, t->ch[1].rev };
    int32_t F = d->F16, Pm1 = d->Pm1, Pm2 = d->Pm2, kq1 = d->kq1, kq2 = d->kq2;
    int32_t fw = (int32_t)(Cf * 0.2f * 16.0f);
    int32_t riq = (int32_t)(d->ri_px * 16.0f / 127.0f * 256.0f);
    int alpha32 = (int)(w->alpha * 32.0f + 0.5f);
    int foam = w->foam, crust = w->crust, sheen = w->sheen, glossy = w->spec > 200;
    const uint8_t *nz = d->noise;

    const cn_gpx_t *e = geo->px;
    for (int i = 0; i < geo->n; i++, e++) {
        int k = e->ch;
        if (!on[k]) continue;
        int32_t S = start16[k] + (rev[k] ? len16 - e->s : e->s);
        int u = e->u, au = u < 0 ? -u : u;
        int32_t dfr = F - d->curv16[au] + d->wob16[(u + 128) >> 2] - S;
        if (dfr <= 0) continue;

        /* two layers of streaks, interpolated along the flow */
        int32_t fx1 = ((S - Pm1) * kq1) >> 8;
        int tx = (fx1 >> 8) & 127, fr1 = fx1 & 255;
        const uint8_t *row1 = nz + (((u + 128) >> 3) << 7);
        int n1 = row1[tx] + (((row1[(tx + 1) & 127] - row1[tx]) * fr1) >> 8);
        int32_t fx2 = ((S - Pm2) * kq2) >> 8;
        int tx2 = (fx2 >> 8) & 127, fr2 = fx2 & 255;
        const uint8_t *row2 = nz + ((((u + 128) / 5 + 40) & 127) << 7);
        int n2 = row2[tx2] + (((row2[(tx2 + 1) & 127] - row2[tx2]) * fr2) >> 8);

        int L = d->litg[e->lit] + d->texk[n1];
        L += d->caus[n1 > n2 ? n1 - n2 : n2 - n1];
        bool crusted = false;
        if (crust) {
            if (n2 > 150) {
                int kk = (n2 - 150) * 7;
                if (kk > 230) kk = 230;
                L -= ((L - 14) * kk) >> 8;
                crusted = true;
            } else if (n2 > 136) {
                L += (n2 - 136) * 5;
            }
        }
        if (!crusted) L += (e->spec * d->specg[n2]) >> 8;
        if (foam && dfr < fw) {
            int f3 = nz[(((((u + 128) >> 3) * 3 + 11) & 127) << 7) | ((tx * 3) & 127)];
            if (f3 > 120) {
                int kf = ((fw - dfr) << 8) / fw;
                L += (((f3 - 120) * foam) >> 7) * ((kf * kf) >> 8) >> 8;
            }
        }
        if (glossy && dfr < 40) L += 60;    /* the glossy rim of a thick front */
        if (d->bub_n) {
            int b = bubble_at(d->bub, d->bub_k0, d->bub_n, d->bub_inv, S, d->Pb16, u, riq);
            if (b == 2) L += 90;
            else if (b == 1) L += 22;
        }
        if (d->fiz_n) {
            int b = bubble_at(d->fiz, d->fiz_k0, d->fiz_n, d->fiz_inv, S, d->Pf16, u + ((n2 - 128) * 3) / 10, riq);
            if (b) L += b == 2 ? 110 : 50;
        }
        if (L < 0) L = 0;
        if (L > 255) L = 255;
        uint16_t c = d->ramp[L];
        if (sheen && n2 > 118 && n2 < 172) {
            int band = 27 - (n2 > 145 ? n2 - 145 : 145 - n2);
            int a = (band * sheen) >> 10;
            if (a > 0) c = blend565(c, RAINBOW[((n1 >> 3) + (S >> 8)) & 7], (uint32_t)a);
        }
        int32_t ea = dfr < 16 ? dfr : 16;
        int a32 = (int)((alpha32 * ea * d->edge16[au]) >> 8);
        uint16_t *o = dst + e->y * W + e->x;
        if (a32 >= 32) *o = c;
        else if (a32 > 0) *o = blend565(*o, c, (uint32_t)a32);
    }
}

/* The source's tank seen from above, full, and the valve's wheel on it.
 * The cell's polar table (angle, radius) stands in for atan2 and sqrt. */
static void draw_source(cn_draw_t *d, uint16_t *dst, const cn_tile_t *t)
{
    const cn_world_t *w = d->w;
    int C = d->C, W = d->W;
    int r_tank = (int)(0.325f * 510.0f);
    int swirl = (int)((t->flowing ? d->t * 1.6f : d->t * 0.15f) * (64.0f / PI_F));
    int twist = t->flowing ? 248 : 41;             /* turns of the texture with the radius */
    int tv0 = (int)(d->t * 2.0f);
    int lo = (int)((0.5f - 0.33f) * (float)C), hi = C - lo;
    float hcx = (0.5f - 0.11f) * (float)C, hcy = (0.5f - 0.13f) * (float)C;
    float hr2 = 0.1f * (float)C * 0.1f * (float)C;
    for (int y = lo; y < hi; y++) {
        uint16_t *o = dst + y * W;
        const uint8_t *pa = d->pol_a + y * C, *pr = d->pol_r + y * C;
        for (int x = lo; x < hi; x++) {
            int r = pr[x];
            if (r >= r_tank + 2) continue;
            int tu = (pa[x] >> 1) + swirl + ((r * twist) >> 8);
            int tv = ((r * 35) >> 8) + tv0;
            int n1 = d->noise[((tv & 127) << 7) | (tu & 127)];
            int n2 = d->noise[(((tv * 7 / 4 + 33) & 127) << 7) | (((tu >> 1) + 50) & 127)];
            int L = 128 + (((n1 - 128) * (w->tex + 40)) >> 7);
            L += d->caus[n1 > n2 ? n1 - n2 : n2 - n1] >> 1;
            bool crusted = w->crust && n2 > 150;
            if (crusted) L = 30 + (L >> 3);
            float hx = (float)x + 0.5f - hcx, hy = (float)y + 0.5f - hcy;
            float hl = 1.0f - (hx * hx + hy * hy) / hr2;
            if (hl > 0 && !crusted) L += (int)(hl * (float)w->spec * 0.6f);
            int e = (r << 8) / r_tank;
            e = (e * e) >> 8;
            L -= (e * e * 60) >> 16;
            if (L < 0) L = 0;
            if (L > 255) L = 255;
            int ea = (r_tank - r) * C * 32 / 510;
            if (ea > 32) ea = 32;
            if (ea > 0) o[x] = blend565(o[x], d->ramp[L], (uint32_t)ea);
        }
    }

    /* the wheel: its shadow, then a rim, three spokes and the hub */
    float A = t->valve * 2.0f * PI_F;
    float cs[3], sn[3];
    for (int k = 0; k < 3; k++) {
        cs[k] = cosf(A + (float)k * (2.0f * PI_F / 3.0f));
        sn[k] = sinf(A + (float)k * (2.0f * PI_F / 3.0f));
    }
    float inv = 1.0f / (float)C;
    int wlo = (int)((0.5f - 0.28f) * (float)C), whi = (int)((0.5f + 0.33f) * (float)C);
    for (int pass = 0; pass < 2; pass++) {
        float ox = pass ? 0 : 0.03f, oy = pass ? 0 : 0.045f;
        for (int y = wlo; y < whi; y++) {
            uint16_t *o = dst + y * W;
            float sy = (y + 0.5f) * inv - 0.5f - oy;
            for (int x = wlo; x < whi; x++) {
                float sx = (x + 0.5f) * inv - 0.5f - ox;
                float r2 = sx * sx + sy * sy;
                if (r2 > 0.27f * 0.27f) continue;
                float rho = sqrtf(r2);
                float nx = 0, ny = 0, cov = 0;
                float ur = (rho - 0.215f) / 0.035f;
                if (fabsf(ur) < 1.0f) {
                    float dx = rho > 1e-4f ? sx / rho : 0, dy = rho > 1e-4f ? sy / rho : 0;
                    nx = dx * ur;
                    ny = dy * ur;
                    cov = clampf((1.0f - fabsf(ur)) * 0.035f * (float)C, 0, 1);
                } else if (rho < 0.065f) {
                    nx = sx / 0.065f;
                    ny = sy / 0.065f;
                    cov = clampf((0.065f - rho) * (float)C, 0, 1);
                } else if (rho < 0.2f) {
                    for (int k = 0; k < 3; k++) {
                        float along = sx * cs[k] + sy * sn[k], perp = -sx * sn[k] + sy * cs[k];
                        if (along > 0 && fabsf(perp) < 0.028f) {
                            float pu = perp / 0.028f;
                            nx = -sn[k] * pu;
                            ny = cs[k] * pu;
                            cov = clampf((0.028f - fabsf(perp)) * (float)C, 0, 1);
                            break;
                        }
                    }
                }
                if (cov <= 0) continue;
                if (!pass) {
                    o[x] = shade565(o[x], 32 - (uint32_t)(cov * 12.0f));
                } else {
                    float nzz = sqrtf(fmaxf(0, 1 - nx * nx - ny * ny));
                    col_t c = accent(w, nx, ny, nzz);
                    o[x] = blend565(o[x], to565(c), (uint32_t)(cov * 32.0f));
                }
            }
        }
    }
}

/* The liquid swirling down the drain: a spiral of the noise, turning. */
static void draw_drain(cn_draw_t *d, uint16_t *dst, const cn_tile_t *t)
{
    if (t->drain <= 0) return;
    int C = d->C, W = d->W;
    float fill = clampf(t->drain, 0, 1);
    int r_bowl = (int)(0.29f * 510.0f), r_hole = (int)(0.05f * 510.0f);
    int spin = (int)(d->t * 5.0f * (128.0f / PI_F));
    int lo = (int)((0.5f - 0.3f) * (float)C), hi = C - lo;
    for (int y = lo; y < hi; y++) {
        uint16_t *o = dst + y * W;
        const uint8_t *pa = d->pol_a + y * C, *pr = d->pol_r + y * C;
        for (int x = lo; x < hi; x++) {
            int r = pr[x];
            if (r >= r_bowl || r < r_hole) continue;
            int tu = ((pa[x] + d->logt[r] + spin) >> 1) & 127, tv = (r * 30 / 255) & 127;
            int n1 = d->noise[(tv << 7) | tu];
            int L = 70 + r * 90 / r_bowl + ((n1 - 128) >> 1);
            if (n1 > 190) L += 70;
            if (L < 0) L = 0;
            if (L > 255) L = 255;
            float ea = clampf((float)(r_bowl - r) * (float)C / 510.0f, 0, 1) *
                       clampf(fill * 1.4f - (float)(r_bowl - r) / 510.0f * 1.5f, 0, 0.92f);
            o[x] = blend565(o[x], d->ramp[L], (uint32_t)(ea * 32.0f));
        }
    }
}

/* --------------------------------------------------------------------------
 * The leak
 * -------------------------------------------------------------------------- */

static float frand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

static uint32_t s_seed = 12345;

static void field_grow(cn_draw_t *d, int x0, int y0, int x1, int y1)
{
    if (x0 < 1) x0 = 1;
    if (y0 < 1) y0 = 1;
    if (x1 > d->W - 2) x1 = d->W - 2;
    if (y1 > d->H - 2) y1 = d->H - 2;
    if (x1 < x0 || y1 < y0) return;
    if (d->fx1 < d->fx0) {
        d->fx0 = x0;
        d->fy0 = y0;
        d->fx1 = x1;
        d->fy1 = y1;
        return;
    }
    if (x0 < d->fx0) d->fx0 = x0;
    if (y0 < d->fy0) d->fy0 = y0;
    if (x1 > d->fx1) d->fx1 = x1;
    if (y1 > d->fy1) d->fy1 = y1;
}

static void splat(cn_draw_t *d, float cx, float cy, float r, int amp)
{
    int x0 = (int)(cx - r), x1 = (int)(cx + r) + 1, y0 = (int)(cy - r), y1 = (int)(cy + r) + 1;
    if (x0 < 1) x0 = 1;
    if (y0 < 1) y0 = 1;
    if (x1 > d->W - 2) x1 = d->W - 2;
    if (y1 > d->H - 2) y1 = d->H - 2;
    if (x1 < x0 || y1 < y0) return;
    float ir2 = 1.0f / (r * r);
    for (int y = y0; y <= y1; y++) {
        uint8_t *f = d->field + (size_t)y * d->W;
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float q = 1.0f - (dx * dx + dy * dy) * ir2;
            if (q <= 0) continue;
            int v = f[x] + (int)((float)amp * q * q);
            f[x] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    field_grow(d, x0, y0, x1, y1);
}

/* one step of spreading: each px takes in an eighth from each side */
static void diffuse(cn_draw_t *d)
{
    if (d->fx1 < d->fx0) return;
    field_grow(d, d->fx0 - 1, d->fy0 - 1, d->fx1 + 1, d->fy1 + 1);
    int W = d->W;
    int x0 = d->fx0, x1 = d->fx1;
    uint8_t *prev = d->row_tmp, *cur = d->row_tmp + W;
    memcpy(prev, d->field + (size_t)(d->fy0 - 1) * W, (size_t)W);
    for (int y = d->fy0; y <= d->fy1; y++) {
        uint8_t *row = d->field + (size_t)y * W;
        const uint8_t *next = row + W;
        memcpy(cur, row, (size_t)W);
        for (int x = x0; x <= x1; x++) {
            int v = (cur[x] * 4 + cur[x - 1] + cur[x + 1] + prev[x] + next[x] + 7) >> 3;
            row[x] = (uint8_t)v;
        }
        uint8_t *t = prev;
        prev = cur;
        cur = t;
    }
}

void cn_draw_spill_clear(cn_draw_t *d)
{
    if (d->fx1 >= d->fx0) {
        for (int y = d->fy0; y <= d->fy1; y++) {
            memset(d->field + (size_t)y * d->W + d->fx0, 0, (size_t)(d->fx1 - d->fx0 + 1));
        }
    }
    d->fx0 = 0;
    d->fx1 = -1;
    d->px0 = 0;
    d->px1 = -1;
    d->npart = 0;
    d->spilling = false;
}

void cn_draw_spill_start(cn_draw_t *d, float x, float y, int dir, bool edge)
{
    static const float VX[4] = { 0, 1, 0, -1 }, VY[4] = { -1, 0, 1, 0 };
    d->spilling = true;
    d->sx = x;
    d->sy = y;
    d->sdx = VX[dir & 3];
    d->sdy = VY[dir & 3];
    d->s_edge = edge;
    d->s_t = d->s_emit = d->s_fume = d->s_diff = 0;
    s_seed ^= (uint32_t)(x * 31.0f + y * 17.0f);
}

static void spawn(cn_draw_t *d, uint8_t kind, float x, float y, float vx, float vy, float r, float life)
{
    if (d->npart >= CN_PARTS) return;
    cn_part_t *p = &d->part[d->npart++];
    p->x = x;
    p->y = y;
    p->vx = vx;
    p->vy = vy;
    p->r = r;
    p->life = p->life0 = life;
    p->kind = kind;
}

void cn_draw_spill_step(cn_draw_t *d, float dt, float flow)
{
    if (!d->spilling) return;
    const cn_world_t *w = d->w;
    float C = (float)d->C;
    d->s_t += dt;
    float ndx = -d->sdx, ndy = -d->sdy;     /* back into the board */
    float pool_x = d->s_edge ? d->sx + ndx * C * 0.22f : d->sx + d->sdx * C * 0.3f;
    float pool_y = d->s_edge ? d->sy + ndy * C * 0.22f : d->sy + d->sdy * C * 0.3f;

    /* drops */
    d->s_emit += dt * (40.0f + 90.0f * flow);
    while (d->s_emit >= 1.0f) {
        d->s_emit -= 1.0f;
        float spread = (frand(&s_seed) + frand(&s_seed) - 1.0f);
        float bx = d->sdx, by = d->sdy;
        float sp = w->drop_speed * C * (0.35f + 0.8f * frand(&s_seed));
        float ang;
        if (d->s_edge) {
            /* over the edge: what we see splashes back along it */
            ang = spread * 1.5f;
            bx = ndx;
            by = ndy;
            sp *= 0.6f;
        } else {
            ang = spread * 0.6f;
        }
        float cs = cosf(ang), sn = sinf(ang);
        float vx = (bx * cs - by * sn) * sp, vy = (bx * sn + by * cs) * sp;
        float r = C * (0.016f + 0.03f * frand(&s_seed));
        float jx = (frand(&s_seed) - 0.5f) * d->ri_px * 1.6f;
        spawn(d, 0, d->sx + (-d->sdy) * jx, d->sy + d->sdx * jx, vx, vy, r,
              0.2f + 0.5f * frand(&s_seed));
    }
    /* fumes, sparks, mist */
    if (w->spill != CN_SPILL_SPLASH) {
        d->s_fume += dt * (w->spill == CN_SPILL_SPARKS ? 34.0f : 14.0f);
        while (d->s_fume >= 1.0f) {
            d->s_fume -= 1.0f;
            float a = frand(&s_seed) * 2.0f * PI_F;
            float rr = C * 0.3f * fminf(1.0f, d->s_t * 0.7f + 0.2f) * frand(&s_seed);
            spawn(d, 1, pool_x + cosf(a) * rr, pool_y + sinf(a) * rr,
                  (frand(&s_seed) - 0.5f) * C * 0.3f, (frand(&s_seed) - 0.5f) * C * 0.3f - C * 0.1f,
                  C * 0.07f, 1.4f + 1.2f * frand(&s_seed));
            if (w->spill == CN_SPILL_SPARKS) {
                float ang = (frand(&s_seed) - 0.5f) * 2.4f;
                float bx = d->s_edge ? ndx : d->sdx, by = d->s_edge ? ndy : d->sdy;
                float sp = C * (1.5f + 2.5f * frand(&s_seed));
                spawn(d, 2, d->sx, d->sy, (bx * cosf(ang) - by * sinf(ang)) * sp,
                      (bx * sinf(ang) + by * cosf(ang)) * sp, 1.2f + C * 0.012f, 0.2f + 0.45f * frand(&s_seed));
                d->part[d->npart - 2].life *= 0.5f;     /* lava's smoke: fewer, shorter */
            }
        }
    }

    /* move them */
    float drag = 2.6f / fmaxf(0.3f, w->spread);
    float kd = expf(-drag * dt), ks = expf(-1.2f * dt);
    for (int i = 0; i < d->npart;) {
        cn_part_t *p = &d->part[i];
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->life -= dt;
        bool dead = p->life <= 0;
        if (p->kind == 0) {
            p->vx *= kd;
            p->vy *= kd;
            if (p->x < 0 || p->y < 0 || p->x >= d->W || p->y >= d->H) {
                dead = true;
            } else if (dead || p->vx * p->vx + p->vy * p->vy < C * C * 0.04f) {
                splat(d, p->x, p->y, p->r * 2.6f, 190);
                dead = true;
            }
        } else if (p->kind == 1) {
            p->r += dt * C * 0.22f;
            p->vx *= ks;
            p->vy *= ks;
        } else {
            p->vx *= ks;
            p->vy *= ks;
        }
        if (dead) *p = d->part[--d->npart];
        else i++;
    }

    /* the pool fills where it comes out */
    float pr = C * (0.12f + 0.2f * fminf(1.0f, d->s_t / 1.5f));
    splat(d, pool_x, pool_y, pr, (int)(26.0f * flow * dt * 30.0f) + 2);
    if (!d->s_edge) splat(d, d->sx, d->sy, C * 0.12f, (int)(20.0f * flow * dt * 30.0f) + 1);

    d->s_diff += dt * w->spread * 30.0f;
    for (int i = 0; i < 2 && d->s_diff >= 1.0f; i++) {
        d->s_diff -= 1.0f;
        diffuse(d);
    }
    if (d->s_diff > 2.0f) d->s_diff = 2.0f;

    /* what the particles cover now, and covered last frame */
    int x0 = d->W, y0 = d->H, x1 = -1, y1 = -1;
    for (int i = 0; i < d->npart; i++) {
        const cn_part_t *p = &d->part[i];
        int a = (int)(p->x - p->r) - 2, b = (int)(p->y - p->r) - 2;
        int c = (int)(p->x + p->r) + 2, e = (int)(p->y + p->r) + 2;
        if (a < x0) x0 = a;
        if (b < y0) y0 = b;
        if (c > x1) x1 = c;
        if (e > y1) y1 = e;
    }
    int ux0 = x0, uy0 = y0, ux1 = x1, uy1 = y1;
    if (d->px1 >= d->px0) {
        if (d->px0 < ux0) ux0 = d->px0;
        if (d->py0 < uy0) uy0 = d->py0;
        if (d->px1 > ux1) ux1 = d->px1;
        if (d->py1 > uy1) uy1 = d->py1;
    }
    d->px0 = x0;
    d->py0 = y0;
    d->px1 = x1;
    d->py1 = y1;
    /* keep the union for spill_rect in the field's box (it is redrawn whole) */
    if (ux1 >= ux0) {
        int fx0 = d->fx0, fy0 = d->fy0, fx1 = d->fx1, fy1 = d->fy1;
        if (fx1 < fx0) {
            fx0 = ux0;
            fy0 = uy0;
            fx1 = ux1;
            fy1 = uy1;
        }
        d->rx0 = ux0 < fx0 ? ux0 : fx0;
        d->ry0 = uy0 < fy0 ? uy0 : fy0;
        d->rx1 = ux1 > fx1 ? ux1 : fx1;
        d->ry1 = uy1 > fy1 ? uy1 : fy1;
    } else {
        d->rx0 = d->fx0;
        d->ry0 = d->fy0;
        d->rx1 = d->fx1;
        d->ry1 = d->fy1;
    }
}

bool cn_draw_spill_rect(const cn_draw_t *d, int *x0, int *y0, int *x1, int *y1)
{
    if (!d->spilling || d->rx1 < d->rx0) return false;
    *x0 = d->rx0 < 0 ? 0 : d->rx0;
    *y0 = d->ry0 < 0 ? 0 : d->ry0;
    *x1 = d->rx1 >= d->W ? d->W - 1 : d->rx1;
    *y1 = d->ry1 >= d->H ? d->H - 1 : d->ry1;
    return *x1 >= *x0 && *y1 >= *y0;
}

static void draw_puddle(cn_draw_t *d, uint16_t *dst, int ox, int oy)
{
    const cn_world_t *w = d->w;
    int C = d->C, W = d->W;
    int x0 = d->fx0 - ox, x1 = d->fx1 - ox, y0 = d->fy0 - oy, y1 = d->fy1 - oy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > C - 1) x1 = C - 1;
    if (y1 > C - 1) y1 = C - 1;
    if (x1 < x0 || y1 < y0) return;
    const int thr = 64;
    int alpha32 = (int)(fmaxf(w->alpha, 0.75f) * 32.0f);
    int cool = w->crust ? (int)(d->s_t * 30.0f) : 0;
    for (int y = y0; y <= y1; y++) {
        const uint8_t *f = d->field + (size_t)(y + oy) * W + ox;
        uint16_t *o = dst + y * W;
        for (int x = x0; x <= x1; x++) {
            int h = f[x];
            if (h < 14) continue;
            if (h < thr - 16) {
                if (w->glow) {
                    int a = ((h - 14) * w->glow) / ((thr - 30) * 20);
                    if (a > 0) o[x] = blend565(o[x], d->ramp[200], (uint32_t)a);
                }
                continue;
            }
            /* lit by its own slope: the edge facing the light shines, a thin
             * line of it where the surface curves down */
            int gx = f[x + 1] - f[x - 1], gy = f[x + W] - f[x - W];
            int sh = (gx * 45 + gy * 65) / 10;
            if (sh > 120) sh = 120;
            if (sh < -120) sh = -120;
            int L = 115 + sh / 3;
            if (sh > 60 && h < thr + 48) L += ((sh - 60) * w->spec) >> 7;
            int n = d->noise[(((y + oy) & 127) << 7) | ((x + ox) & 127)];
            L += ((n - 128) * w->tex) >> 8;
            if (h < thr) L -= 25;
            if (w->crust && n > 200 - cool) L = 24 + (L >> 3);
            if (L < 0) L = 0;
            if (L > 255) L = 255;
            int a = (h - (thr - 16)) * alpha32 / 32;
            if (a > alpha32) a = alpha32;
            o[x] = blend565(o[x], d->ramp[L], (uint32_t)a);
        }
    }
}

static void draw_parts(cn_draw_t *d, uint16_t *dst, int ox, int oy)
{
    const cn_world_t *w = d->w;
    int C = d->C, W = d->W;
    uint16_t fume = w->spill == CN_SPILL_FUMES ? to565(col(0x9AE070))
                  : w->spill == CN_SPILL_SPARKS ? to565(col(0x3C3632)) : to565(col(0xEEF6FF));
    uint16_t spark = to565(col(0xFFE6A0));
    for (int i = 0; i < d->npart; i++) {
        const cn_part_t *p = &d->part[i];
        float r = p->r;
        int x0 = (int)(p->x - r) - ox, x1 = (int)(p->x + r) + 1 - ox;
        int y0 = (int)(p->y - r) - oy, y1 = (int)(p->y + r) + 1 - oy;
        if (x1 < 0 || y1 < 0 || x0 >= C || y0 >= C) continue;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > C - 1) x1 = C - 1;
        if (y1 > C - 1) y1 = C - 1;
        float ir = 1.0f / r, lifek = p->life / p->life0;
        for (int y = y0; y <= y1; y++) {
            uint16_t *o = dst + y * W;
            float dy = ((float)(y + oy) + 0.5f - p->y) * ir;
            for (int x = x0; x <= x1; x++) {
                float dx = ((float)(x + ox) + 0.5f - p->x) * ir;
                float q = 1.0f - (dx * dx + dy * dy);
                if (q <= 0) continue;
                if (p->kind == 0) {
                    float hx = dx + 0.35f, hy = dy + 0.35f;
                    float hl = 1.0f - (hx * hx + hy * hy) * 9.0f;
                    int L = 95 + (int)(q * 50.0f) + (hl > 0 ? (int)(hl * (float)w->spec * 0.6f) : 0);
                    if (L > 255) L = 255;
                    float ea = clampf(q * r, 0, 1);
                    o[x] = blend565(o[x], d->ramp[L], (uint32_t)(ea * 30.0f));
                } else if (p->kind == 1) {
                    float a = q * q * lifek * (w->spill == CN_SPILL_MIST ? 16.0f : 11.0f);
                    if (a >= 1.0f) o[x] = blend565(o[x], fume, (uint32_t)a);
                } else {
                    o[x] = blend565(o[x], spark, (uint32_t)(clampf(q * 2.0f, 0, 1) * lifek * 32.0f));
                }
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * A cell, everything on it
 * -------------------------------------------------------------------------- */

/* A pipe turning or dropping in: the floor, and the sprite sampled through
 * the turn and the scale. */
static void draw_moving(cn_draw_t *d, uint16_t *dst, size_t off, const cn_tile_t *t)
{
    int C = d->C, W = d->W;
    for (int y = 0; y < C; y++) memcpy(dst + y * W, d->floor + off + (size_t)y * W, (size_t)C * 2);
    const cn_spr_t *sp = sprite_of(d, t->shape, t->srot);
    if (!sp) return;
    float a = t->turn * (PI_F / 180.0f);
    float cs = cosf(a), sn = sinf(a);
    float k = 1.0f / (t->scale > 0.1f ? t->scale : 1.0f);
    float h = (float)C * 0.5f;
    int ox = C * 4 / 100 + 1, oy = C * 6 / 100 + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < C; y++) {
            uint16_t *o = dst + y * W;
            float py = ((float)y + 0.5f - h - (pass ? 0 : (float)oy)) * k;
            for (int x = 0; x < C; x++) {
                float px = ((float)x + 0.5f - h - (pass ? 0 : (float)ox)) * k;
                /* sample the final picture turned back by 'turn' */
                int sx = (int)floorf(px * cs + py * sn + h), sy = (int)floorf(-px * sn + py * cs + h);
                if (sx < 0 || sy < 0 || sx >= C || sy >= C) continue;
                uint32_t al = sp->a[sy * C + sx];
                if (!al) continue;
                if (!pass) o[x] = shade565(o[x], 32 - ((al * 14) >> 8));
                else if (al >= 250) o[x] = sp->rgb[sy * C + sx];
                else if (al > 4) o[x] = blend565(o[x], sp->rgb[sy * C + sx], (al + 4) >> 3);
            }
        }
    }
}

void cn_draw_tile(cn_draw_t *d, int sc, int sr, const cn_tile_t *t)
{
    int C = d->C, W = d->W;
    size_t off = (size_t)sr * C * W + (size_t)sc * C;
    uint16_t *dst = d->fb + off;
    if ((t->turn != 0.0f || (t->scale != 0.0f && t->scale != 1.0f)) && t->shape != CN_SH_NONE) {
        draw_moving(d, dst, off, t);
    } else {
        for (int y = 0; y < C; y++) memcpy(dst + y * W, d->base + off + (size_t)y * W, (size_t)C * 2);
        draw_liquid(d, dst, t);
        if (t->shape == CN_SH_SOURCE) draw_source(d, dst, t);
        if (t->shape == CN_SH_DRAIN) draw_drain(d, dst, t);
    }
    int ox = sc * C, oy = sr * C;
    if (d->fx1 >= d->fx0 && d->fx0 <= ox + C && d->fx1 >= ox && d->fy0 <= oy + C && d->fy1 >= oy) {
        draw_puddle(d, dst, ox, oy);
    }
    if (d->npart) draw_parts(d, dst, ox, oy);
}

/* --------------------------------------------------------------------------
 * Pictures for the UI
 * -------------------------------------------------------------------------- */

bool cn_draw_icon(int world, int shape, int srot, int size, uint8_t *out)
{
    uint8_t *nz = cn_draw_alloc(128 * 128);
    if (!nz) return false;
    make_noise(nz, 0xC0FFEEu + (uint32_t)world * 977u);
    uint16_t *rgb = (uint16_t *)out;
    uint8_t *a = out + (size_t)size * size * 2;
    make_sprite(cn_world(world), nz, shape, srot, size, rgb, a);
    free(nz);
    return true;
}

bool cn_draw_preview(int world, int w, int h, float t, uint16_t *out)
{
    static cn_draw_t pd;
    int cols = (w + h - 1) / h;
    s_straight_only = true;
    bool ok = cn_draw_init(&pd, world, cols, 1, h);
    s_straight_only = false;
    if (!ok) return false;
    cn_tile_t tile = { 0 };
    tile.shape = CN_SH_STRAIGHT;
    tile.srot = 1;
    for (int c = 0; c < cols; c++) cn_draw_base(&pd, c, 0, &tile);
    float front = (float)cols * (float)h * 0.78f;
    cn_draw_begin(&pd, t, t * (float)h * 0.8f, front, (float)h * 0.8f);
    for (int c = 0; c < cols; c++) {
        tile.ch[0].on = 1;
        tile.ch[0].rev = 1;
        tile.ch[0].start_px = (float)(c * h);
        cn_draw_tile(&pd, c, 0, &tile);
    }
    int x0 = (pd.W - w) / 2;
    for (int y = 0; y < h; y++) memcpy(out + (size_t)y * w, pd.fb + (size_t)y * pd.W + x0, (size_t)w * 2);
    cn_draw_free(&pd);
    return true;
}
