/*
 * PINBALL - the picture (see pb_render.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "pb_render.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PB_PI 3.14159265f

/* the DMD's window over the table: 128 x 32 dots */
#define DMD_X0 112
#define DMD_X1 712
#define DMD_Y0 5
#define DMD_Y1 155

#define MASK565 0x07E0F81Fu

static inline uint16_t blend(uint16_t d, uint16_t s, unsigned a)
{
    /* a 0..255 */
    uint32_t dd = (d | ((uint32_t)d << 16)) & MASK565;
    uint32_t ss = (s | ((uint32_t)s << 16)) & MASK565;
    uint32_t k = (a + 4) >> 3;
    uint32_t r = (dd + (((ss - dd) * k) >> 5)) & MASK565;
    return (uint16_t)(r | (r >> 16));
}

static inline uint16_t darken(uint16_t d, unsigned k)
{
    /* k 0..32: how much of the colour stays */
    uint32_t dd = (d | ((uint32_t)d << 16)) & MASK565;
    uint32_t r = ((dd * k) >> 5) & MASK565;
    return (uint16_t)(r | (r >> 16));
}

static inline uint16_t add_sat(uint16_t d, uint16_t s)
{
    unsigned r = (d >> 11) + (s >> 11);
    unsigned g = ((d >> 5) & 63) + ((s >> 5) & 63);
    unsigned b = (d & 31) + (s & 31);
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t rgb(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

static uint16_t rgb_scale(uint32_t c, float k)
{
    int r = (int)(((c >> 16) & 255) * k), g = (int)(((c >> 8) & 255) * k), b = (int)((c & 255) * k);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return rgb(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

/* ---- rectangles ----------------------------------------------------------- */

static bool rc_empty(const pb_rect_t *a)
{
    return a->x1 <= a->x0 || a->y1 <= a->y0;
}

static bool rc_touch(const pb_rect_t *a, const pb_rect_t *b, int m)
{
    return a->x0 <= b->x1 + m && b->x0 <= a->x1 + m && a->y0 <= b->y1 + m && b->y0 <= a->y1 + m;
}

static void rc_union(pb_rect_t *a, const pb_rect_t *b)
{
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

static void rc_clip(pb_rect_t *a, const pb_rect_t *c)
{
    if (a->x0 < c->x0) a->x0 = c->x0;
    if (a->y0 < c->y0) a->y0 = c->y0;
    if (a->x1 > c->x1) a->x1 = c->x1;
    if (a->y1 > c->y1) a->y1 = c->y1;
}

bool pb_render_inside(const pb_rect_t *rc, int x, int y)
{
    return x >= rc->x0 && x < rc->x1 && y >= rc->y0 && y < rc->y1;
}

static void dirty(pb_render_t *r, pb_rect_t a)
{
    static const pb_rect_t SCREEN = { 0, 0, PB_SW, PB_SH };
    rc_clip(&a, &SCREEN);
    if (rc_empty(&a)) return;
    /* merge with what it touches, repeatedly */
    bool again = true;
    while (again) {
        again = false;
        for (int i = 0; i < r->ndirty; i++) {
            if (rc_touch(&r->dirty[i], &a, 6)) {
                rc_union(&a, &r->dirty[i]);
                r->dirty[i] = r->dirty[--r->ndirty];
                again = true;
                break;
            }
        }
    }
    if (r->ndirty >= PB_MAX_DIRTY) {
        rc_union(&r->dirty[r->ndirty - 1], &a);
        return;
    }
    r->dirty[r->ndirty++] = a;
}

static pb_rect_t spr_rect(const pb_spr_t *s)
{
    pb_rect_t a = { s->x, s->y, (int16_t)(s->x + s->w), (int16_t)(s->y + s->h) };
    return a;
}

/* ---- loading -------------------------------------------------------------- */

static bool load_sprite(pb_render_t *r, pb_pak_t *pak, const char *name, pb_spr_t *s)
{
    const pb_entry_t *e = pb_pak_find(pak, name);
    memset(s, 0, sizeof *s);
    if (!e || (e->type != PK_SPRITE && e->type != PK_SPRITEZ)) return false;
    uint8_t *raw = (uint8_t *)pb_pak_load(pak, e);
    if (!raw) return false;
    s->x = e->x;
    s->y = e->y;
    s->w = (int16_t)e->w;
    s->h = (int16_t)e->h;
    s->px = (uint16_t *)raw;
    s->a = raw + (size_t)e->w * e->h * 2;
    if (e->type == PK_SPRITEZ) s->z = (uint16_t *)(raw + (size_t)e->w * e->h * 3);
    r->bytes += e->rawlen;
    return true;
}

static bool load_patch(pb_render_t *r, pb_pak_t *pak, const char *name, pb_patch_t *p)
{
    const pb_entry_t *e = pb_pak_find(pak, name);
    memset(p, 0, sizeof *p);
    if (!e || (e->type != PK_ADD && e->type != PK_PATCH)) return false;
    uint8_t *raw = (uint8_t *)pb_pak_load(pak, e);
    if (!raw) return false;
    size_t n = (size_t)e->w * e->h;
    p->x = e->x;
    p->y = (int16_t)(e->y - PB_TOP);
    p->w = (int16_t)e->w;
    p->h = (int16_t)e->h;
    p->kind = e->type;
    p->px = (uint16_t *)raw;
    if (e->type == PK_PATCH) {
        p->z = (uint16_t *)(raw + n * 2);
        p->m = raw + n * 4;
    }
    r->bytes += e->rawlen;
    return true;
}

/* the chrome ball at every diameter, from the pack's large one */
static void make_balls(pb_render_t *r, const pb_spr_t *src)
{
    for (int d = PB_BALL_MIN; d <= PB_BALL_MAX; d++) {
        pb_spr_t *s = &r->ball[d];
        size_t n = (size_t)d * d;
        uint8_t *mem = (uint8_t *)pb_malloc(n * 4);
        if (!mem) continue;
        s->w = s->h = (int16_t)d;
        s->px = (uint16_t *)mem;
        s->a = mem + n * 2;
        s->bump = mem + n * 3;
        r->bytes += (uint32_t)n * 4;
        for (int j = 0; j < d; j++) {
            for (int i = 0; i < d; i++) {
                float u = ((float)i + 0.5f) / d * 2 - 1, v = ((float)j + 0.5f) / d * 2 - 1;
                float rr = u * u + v * v;
                s->bump[j * d + i] = rr < 1 ? (uint8_t)(sqrtf(1 - rr) * 255) : 0;
                if (src && src->px) {
                    /* box filter over the source's pixels under this one */
                    int sx0 = i * src->w / d, sx1 = (i + 1) * src->w / d;
                    int sy0 = j * src->h / d, sy1 = (j + 1) * src->h / d;
                    if (sx1 <= sx0) sx1 = sx0 + 1;
                    if (sy1 <= sy0) sy1 = sy0 + 1;
                    uint32_t ar = 0, ag = 0, ab = 0, aa = 0, cnt = 0;
                    for (int y = sy0; y < sy1; y++) {
                        for (int x = sx0; x < sx1; x++) {
                            uint16_t c = src->px[y * src->w + x];
                            uint32_t a = src->a[y * src->w + x];
                            ar += ((c >> 11) & 31) * a;
                            ag += ((c >> 5) & 63) * a;
                            ab += (c & 31) * a;
                            aa += a;
                            cnt++;
                        }
                    }
                    uint16_t col = 0;
                    if (aa) col = (uint16_t)(((ar / aa) << 11) | ((ag / aa) << 5) | (ab / aa));
                    s->px[j * d + i] = col;
                    s->a[j * d + i] = (uint8_t)(aa / cnt);
                } else {
                    /* no ball in the pack: a shaded sphere */
                    float a = rr < 1 ? 1.0f : 0.0f;
                    float edge = 1 - sqrtf(rr);
                    if (rr < 1 && edge < 0.08f) a = edge / 0.08f;
                    float lit = rr < 1 ? 0.35f + 0.65f * sqrtf(1 - rr) : 0;
                    float hx = u + 0.35f, hy = v + 0.45f;
                    float spec = expf(-(hx * hx + hy * hy) * 18.0f);
                    float k = lit * 0.8f + spec;
                    s->px[j * d + i] = rgb_scale(0xC8CCD4, k);
                    s->a[j * d + i] = (uint8_t)(a * 255);
                }
            }
        }
    }
}

static void dmd_tables(pb_render_t *r)
{
    const float pitch = (float)(DMD_X1 - DMD_X0) / DMD_W;
    for (int x = 0; x < PB_SW; x++) {
        r->dmd_cx[x] = -1;
        r->dmd_wx[x] = 0;
        if (x < DMD_X0 || x >= DMD_X1) continue;
        float pos = ((float)(x - DMD_X0) + 0.5f) / pitch;
        int ix = (int)pos;
        float f = pos - ix;
        float w = 1.25f - fabsf(f - 0.5f) * 2.0f / 0.9f;
        if (w < 0) w = 0;
        if (w > 1) w = 1;
        r->dmd_cx[x] = (int16_t)(ix < DMD_W ? ix : DMD_W - 1);
        r->dmd_wx[x] = (uint8_t)(w * 7.0f + 0.5f);
    }
    for (int y = 0; y < PB_TOP; y++) {
        r->dmd_cy[y] = -1;
        r->dmd_wy[y] = 0;
        if (y < DMD_Y0 || y >= DMD_Y1) continue;
        float pos = ((float)(y - DMD_Y0) + 0.5f) / pitch;
        int iy = (int)pos;
        float f = pos - iy;
        float w = 1.25f - fabsf(f - 0.5f) * 2.0f / 0.9f;
        if (w < 0) w = 0;
        if (w > 1) w = 1;
        r->dmd_cy[y] = (int16_t)(iy < DMD_H ? iy : DMD_H - 1);
        r->dmd_wy[y] = (uint8_t)(w * 7.0f + 0.5f);
    }
    uint32_t c = r->t->c_dmd ? r->t->c_dmd : 0xFF8020;
    for (int l = 0; l < 16; l++) {
        float k = l == 0 ? 0.08f : 0.22f + 0.78f * (float)l / 15.0f;
        for (int w = 0; w < 8; w++) {
            /* the middle of a lit dot a little hotter */
            float kk = k * (0.7f + 0.3f * (float)w / 7.0f);
            if (l >= 12 && w >= 6) kk *= 1.15f;
            r->dmd_lut[l][w] = rgb_scale(c, kk);
        }
    }
}

bool pb_render_init(pb_render_t *r, const pb_table_t *t, pb_pak_t *pak)
{
    memset(r, 0, sizeof *r);
    r->t = t;
    size_t n = (size_t)PB_SW * PB_TH;
    r->fb = (uint16_t *)pb_calloc((size_t)PB_SW * PB_SH * 2);
    r->zfb = (uint16_t *)pb_malloc(n * 2);
    if (!r->fb || !r->zfb) goto fail;
    r->bytes += (uint32_t)(PB_SW * PB_SH * 2 + n * 2);

    for (int y = 0; y < PB_SH; y++) {
        float z0 = pb_plane_depth(&t->cam, (float)y + 0.5f, 0) * 10.0f;
        float zf = pb_plane_depth(&t->cam, (float)y + 0.5f, 22.0f) * 10.0f;
        r->zrow0[y] = (uint16_t)(z0 > 65000 ? 65000 : z0 < 0 ? 0 : z0);
        r->zrow_flip[y] = (uint16_t)(zf > 65000 ? 65000 : zf < 0 ? 0 : zf);
    }

    const pb_entry_t *e = pb_pak_find(pak, "bg");
    if (e && e->type == PK_RGB565 && e->w == PB_SW && e->h == PB_TH) {
        r->base = (uint16_t *)pb_pak_load(pak, e);
        r->bytes += e->rawlen;
    }
    if (!r->base) {
        aos_hal_log("pinball", "the pack has no table picture");
        goto fail;
    }
    e = pb_pak_find(pak, "depth");
    if (e && e->type == PK_DEPTH && e->w == PB_SW && e->h == PB_TH) {
        r->zbase = (uint16_t *)pb_pak_load(pak, e);
        r->bytes += e->rawlen;
    }
    if (!r->zbase) {
        /* no depth: everything is the playfield */
        r->zbase = (uint16_t *)pb_malloc(n * 2);
        if (!r->zbase) goto fail;
        for (int y = 0; y < PB_TH; y++) {
            for (int x = 0; x < PB_SW; x++) r->zbase[y * PB_SW + x] = r->zrow0[y + PB_TOP];
        }
    }
    e = pb_pak_find(pak, "dmd");
    if (e && e->type == PK_RGB565 && e->w == PB_SW && e->h == PB_TOP) {
        r->dmd_bg = (uint16_t *)pb_pak_load(pak, e);
        r->bytes += e->rawlen;
    }

    char name[PB_NAME_LEN];
    for (int i = 0; i < t->nlamp; i++) {
        snprintf(name, sizeof name, "L_%s", t->lamp[i].name);
        load_patch(r, pak, name, &r->lamp[i]);
    }
    r->spin_obj = -1;
    for (int i = 0; i < t->nobj; i++) {
        if (t->obj[i].type == OT_DROP) {
            snprintf(name, sizeof name, "D_%s", t->obj[i].name);
            load_patch(r, pak, name, &r->drop[i]);
        } else if (t->obj[i].type == OT_SPINNER && r->spin_obj < 0) {
            r->spin_obj = i;
        }
    }
    for (int s = 0; s < t->nflip; s++) {
        r->flip_area[s].x0 = r->flip_area[s].y0 = 32000;
        for (int f = 0; f < PB_FLIP_FRAMES; f++) {
            snprintf(name, sizeof name, "F%d_%02d", s, f);
            if (!load_sprite(r, pak, name, &r->flip[s][f])) break;
            pb_rect_t a = spr_rect(&r->flip[s][f]);
            if (f == 0) r->flip_area[s] = a;
            else rc_union(&r->flip_area[s], &a);
            r->nflip[s]++;
        }
    }
    for (int k = 0; k < t->ntoy; k++) {
        for (int f = 0; f < PB_TOY_FRAMES; f++) {
            snprintf(name, sizeof name, "T%d_%02d", k, f);
            if (!load_sprite(r, pak, name, &r->toy[k][f])) break;
            pb_rect_t a = spr_rect(&r->toy[k][f]);
            if (f == 0) r->toy_area[k] = a;
            else rc_union(&r->toy_area[k], &a);
            r->ntoy[k]++;
        }
        r->toy_drawn[k] = -1;
    }
    for (int f = 0; f < PB_SPIN_FRAMES; f++) {
        snprintf(name, sizeof name, "S_%02d", f);
        if (!load_sprite(r, pak, name, &r->spin[f])) break;
        pb_rect_t a = spr_rect(&r->spin[f]);
        if (f == 0) r->spin_area = a;
        else rc_union(&r->spin_area, &a);
        r->nspin++;
    }
    pb_spr_t src;
    bool have_ball = load_sprite(r, pak, "ball", &src);
    make_balls(r, have_ball ? &src : NULL);
    if (have_ball) pb_free(src.px);
    load_sprite(r, pak, "launch", &r->launch[0]);
    load_sprite(r, pak, "launch_on", &r->launch[1]);
    load_sprite(r, pak, "pause", &r->pause);
    if (r->launch[0].px) {
        r->launch_rc = spr_rect(&r->launch[0]);
    } else {
        pb_rect_t d = { 560, 1076, 704, 1220 };
        r->launch_rc = d;
    }
    /* room round it for the charge ring */
    r->launch_rc.x0 -= 14;
    r->launch_rc.y0 -= 14;
    r->launch_rc.x1 += 14;
    r->launch_rc.y1 += 14;
    if (r->pause.px) {
        r->pause_rc = spr_rect(&r->pause);
    } else {
        pb_rect_t d = { 12, 34, 100, 126 };
        r->pause_rc = d;
    }
    dmd_tables(r);
    r->launch_drawn = -1;
    pb_render_full(r);
    return true;
fail:
    pb_render_free(r);
    return false;
}

void pb_render_free(pb_render_t *r)
{
    pb_free(r->fb);
    pb_free(r->zfb);
    pb_free(r->base);
    pb_free(r->zbase);
    pb_free(r->dmd_bg);
    for (int s = 0; s < PB_MAX_FLIP; s++) {
        for (int f = 0; f < r->nflip[s]; f++) pb_free(r->flip[s][f].px);
    }
    for (int k = 0; k < PB_MAX_TOY; k++) {
        for (int f = 0; f < r->ntoy[k]; f++) pb_free(r->toy[k][f].px);
    }
    for (int f = 0; f < r->nspin; f++) pb_free(r->spin[f].px);
    for (int d = 0; d <= PB_BALL_MAX; d++) pb_free(r->ball[d].px);
    pb_free(r->launch[0].px);
    pb_free(r->launch[1].px);
    pb_free(r->pause.px);
    for (int i = 0; i < PB_MAX_LAMP; i++) pb_free(r->lamp[i].px);
    for (int i = 0; i < PB_MAX_OBJ; i++) pb_free(r->drop[i].px);
    memset(r, 0, sizeof *r);
}

void pb_render_full(pb_render_t *r)
{
    r->ndirty = 0;
    pb_rect_t a = { 0, 0, PB_SW, PB_SH };
    r->dirty[r->ndirty++] = a;
    r->dmd_valid = false;
}

/* ---- the table's picture as its lamps and targets stand -----------------
 *
 * Into the frame and its depth, for a rectangle of the screen: the picture
 * with every lamp off, the drop targets that are down, the light of the
 * lamps that are on. Nothing keeps the result between frames: a copy of the
 * whole table as it stands would be 3 MB more of PSRAM, and a rectangle is
 * cheap to build again (few patches touch it). */

static void compose_table(pb_render_t *r, const pb_rect_t *rc)
{
    pb_rect_t a = { rc->x0, (int16_t)(rc->y0 - PB_TOP), rc->x1, (int16_t)(rc->y1 - PB_TOP) };
    pb_rect_t all = { 0, 0, PB_SW, PB_TH };
    rc_clip(&a, &all);
    if (rc_empty(&a)) return;
    const pb_table_t *t = r->t;
    for (int y = a.y0; y < a.y1; y++) {
        size_t o = (size_t)y * PB_SW + a.x0;
        memcpy(r->fb + (size_t)(y + PB_TOP) * PB_SW + a.x0, r->base + o, (size_t)(a.x1 - a.x0) * 2);
        memcpy(r->zfb + o, r->zbase + o, (size_t)(a.x1 - a.x0) * 2);
    }
    for (int i = 0; i < t->nobj; i++) {
        const pb_patch_t *p = &r->drop[i];
        if (!p->px || !r->drop_vis[i]) continue;
        pb_rect_t pr = { p->x, p->y, (int16_t)(p->x + p->w), (int16_t)(p->y + p->h) };
        rc_clip(&pr, &a);
        for (int y = pr.y0; y < pr.y1; y++) {
            for (int x = pr.x0; x < pr.x1; x++) {
                int k = (y - p->y) * p->w + (x - p->x);
                if (!p->m[k]) continue;
                size_t o = (size_t)y * PB_SW + x;
                r->fb[o + (size_t)PB_TOP * PB_SW] = p->px[k];
                r->zfb[o] = p->z[k];
            }
        }
    }
    for (int i = 0; i < t->nlamp; i++) {
        const pb_patch_t *p = &r->lamp[i];
        if (!p->px || !r->lamp_vis[i]) continue;
        pb_rect_t pr = { p->x, p->y, (int16_t)(p->x + p->w), (int16_t)(p->y + p->h) };
        rc_clip(&pr, &a);
        for (int y = pr.y0; y < pr.y1; y++) {
            const uint16_t *src = p->px + (y - p->y) * p->w + (pr.x0 - p->x);
            uint16_t *dst = r->fb + (size_t)(y + PB_TOP) * PB_SW + pr.x0;
            for (int x = pr.x0; x < pr.x1; x++, src++, dst++) {
                if (*src) *dst = add_sat(*dst, *src);
            }
        }
    }
}

static void patch_changed(pb_render_t *r, const pb_patch_t *p)
{
    pb_rect_t a = { p->x, (int16_t)(p->y + PB_TOP), (int16_t)(p->x + p->w), (int16_t)(p->y + p->h + PB_TOP) };
    dirty(r, a);
}

/* ---- drawing into the frame ---------------------------------------------- */

static void draw_sprite_opa(pb_render_t *r, const pb_spr_t *s, const pb_rect_t *clip, const uint16_t *zrow,
                            unsigned opa);

static void draw_sprite(pb_render_t *r, const pb_spr_t *s, const pb_rect_t *clip, const uint16_t *zrow)
{
    draw_sprite_opa(r, s, clip, zrow, 255);
}

/* a frame with its own depth (a toy): it hides what is behind it, and is
 * hidden by what is in front (a ramp over it) */
static void draw_spritez(pb_render_t *r, const pb_spr_t *s, const pb_rect_t *clip, unsigned opa)
{
    if (!s->px || !s->z) return;
    pb_rect_t a = spr_rect(s);
    pb_rect_t tab = { 0, PB_TOP, PB_SW, PB_SH };
    rc_clip(&a, clip);
    rc_clip(&a, &tab);
    if (rc_empty(&a)) return;
    for (int y = a.y0; y < a.y1; y++) {
        int k0 = (y - s->y) * s->w + (a.x0 - s->x);
        uint16_t *d = r->fb + (size_t)y * PB_SW + a.x0;
        uint16_t *z = r->zfb + (size_t)(y - PB_TOP) * PB_SW + a.x0;
        for (int x = a.x0; x < a.x1; x++, k0++, d++, z++) {
            unsigned al = s->a[k0];
            if (!al) continue;
            uint16_t sz = s->z[k0];
            if (sz > *z + 20) continue;
            al = (al * opa) >> 8;
            *d = al >= 250 ? s->px[k0] : blend(*d, s->px[k0], al);
            if (al >= 128) *z = sz;
        }
    }
}

static void draw_sprite_opa(pb_render_t *r, const pb_spr_t *s, const pb_rect_t *clip, const uint16_t *zrow,
                            unsigned opa)
{
    if (!s->px) return;
    pb_rect_t a = spr_rect(s);
    rc_clip(&a, clip);
    if (rc_empty(&a)) return;
    for (int y = a.y0; y < a.y1; y++) {
        const uint16_t *sp = s->px + (y - s->y) * s->w + (a.x0 - s->x);
        const uint8_t *sa = s->a + (y - s->y) * s->w + (a.x0 - s->x);
        uint16_t *d = r->fb + (size_t)y * PB_SW + a.x0;
        uint16_t *z = y >= PB_TOP ? r->zfb + (size_t)(y - PB_TOP) * PB_SW + a.x0 : NULL;
        uint16_t zr = zrow ? zrow[y] : 0;
        for (int x = a.x0; x < a.x1; x++, sp++, sa++, d++) {
            unsigned al = opa >= 255 ? *sa : (*sa * opa) >> 8;
            if (al) {
                *d = al >= 250 ? *sp : blend(*d, *sp, al);
                /* only the flipper itself hides the ball, not its shadow
                 * (the shadow catcher's pixels are never this opaque) */
                if (zrow && z && al >= 245) z[x - a.x0] = zr;
            }
        }
    }
}

typedef struct {
    float   sx, sy;             /* centre on the screen                       */
    float   depth;              /* mm                                         */
    int     d;                  /* diameter, pixels                           */
    pb_rect_t rc, shadow;
    float   shx, shy;
    int     shd;
    bool    on_ramp;
} ball_view_t;

static void ball_view(const pb_render_t *r, const pb_ball_t *b, ball_view_t *v)
{
    const pb_table_t *t = r->t;
    pb_project(&t->cam, b->x, b->y, b->z + t->ball_r, &v->sx, &v->sy, &v->depth);
    int d = (int)(t->cam.f * 2 * t->ball_r / v->depth + 0.5f);
    if (d < PB_BALL_MIN) d = PB_BALL_MIN;
    if (d > PB_BALL_MAX) d = PB_BALL_MAX;
    v->d = d;
    int x0 = (int)floorf(v->sx - d * 0.5f + 0.5f), y0 = (int)floorf(v->sy - d * 0.5f + 0.5f);
    pb_rect_t rc = { (int16_t)x0, (int16_t)y0, (int16_t)(x0 + d), (int16_t)(y0 + d) };
    v->rc = rc;
    v->on_ramp = b->z > 2.0f;
    /* the shadow: the light is above and a little behind the camera */
    float dd;
    pb_project(&t->cam, b->x + 4.0f, b->y + 6.0f, 0, &v->shx, &v->shy, &dd);
    v->shd = (int)(d * 1.25f);
    int h = v->shd / 2 + 1;
    pb_rect_t sr = { (int16_t)(v->shx - h), (int16_t)(v->shy - h * 0.7f), (int16_t)(v->shx + h + 1),
                     (int16_t)(v->shy + h * 0.7f + 1) };
    v->shadow = sr;
}

static void draw_shadow(pb_render_t *r, const ball_view_t *v, const pb_rect_t *clip)
{
    if (v->on_ramp) return;
    pb_rect_t a = v->shadow;
    pb_rect_t tab = { 0, PB_TOP, PB_SW, PB_SH };
    rc_clip(&a, clip);
    rc_clip(&a, &tab);
    if (rc_empty(&a)) return;
    float rx = v->shd * 0.5f, ry = rx * 0.7f;
    for (int y = a.y0; y < a.y1; y++) {
        uint16_t *d = r->fb + (size_t)y * PB_SW;
        const uint16_t *z = r->zfb + (size_t)(y - PB_TOP) * PB_SW;
        float vy = ((float)y + 0.5f - v->shy) / ry;
        uint16_t zfloor = r->zrow0[y] > 40 ? r->zrow0[y] - 40 : 0;
        for (int x = a.x0; x < a.x1; x++) {
            float vx = ((float)x + 0.5f - v->shx) / rx;
            float q = vx * vx + vy * vy;
            if (q >= 1.0f || z[x] < zfloor) continue;
            /* soft at the edge, dark in the middle */
            float k = 1.0f - q;
            unsigned keep = 32 - (unsigned)(k * k * 15.0f);
            d[x] = darken(d[x], keep);
        }
    }
}

static void draw_ball(pb_render_t *r, const ball_view_t *v, const pb_rect_t *clip)
{
    const pb_spr_t *s = &r->ball[v->d];
    if (!s->px) return;
    pb_rect_t a = v->rc;
    pb_rect_t tab = { 0, PB_TOP, PB_SW, PB_SH };
    rc_clip(&a, clip);
    rc_clip(&a, &tab);
    if (rc_empty(&a)) return;
    int zc = (int)(v->depth * 10.0f);
    int zr = (int)(r->t->ball_r * 10.0f);
    for (int y = a.y0; y < a.y1; y++) {
        int j = y - v->rc.y0;
        uint16_t *d = r->fb + (size_t)y * PB_SW;
        const uint16_t *z = r->zfb + (size_t)(y - PB_TOP) * PB_SW;
        for (int x = a.x0; x < a.x1; x++) {
            int k = j * s->w + (x - v->rc.x0);
            unsigned al = s->a[k];
            if (!al) continue;
            int zb = zc - ((zr * s->bump[k]) >> 8);
            if (zb >= (int)z[x]) continue;
            d[x] = al >= 250 ? s->px[k] : blend(d[x], s->px[k], al);
        }
    }
}

static void draw_dmd(pb_render_t *r, const pb_rect_t *clip)
{
    pb_rect_t a = { 0, 0, PB_SW, PB_TOP };
    rc_clip(&a, clip);
    if (rc_empty(&a)) return;
    for (int y = a.y0; y < a.y1; y++) {
        uint16_t *d = r->fb + (size_t)y * PB_SW;
        const uint16_t *bg = r->dmd_bg ? r->dmd_bg + (size_t)y * PB_SW : NULL;
        int cy = r->dmd_cy[y];
        unsigned wy = r->dmd_wy[y];
        for (int x = a.x0; x < a.x1; x++) {
            uint16_t b = bg ? bg[x] : 0x0000;
            int cx = r->dmd_cx[x];
            if (cy < 0 || cx < 0) {
                d[x] = b;
                continue;
            }
            /* inside the window the glass is dark */
            b = darken(b, 6);
            unsigned w = (wy * r->dmd_wx[x] + 3) / 7;
            if (!w) {
                d[x] = b;
                continue;
            }
            uint16_t c = r->dmd_lut[r->dmd.px[cy][cx]][w > 7 ? 7 : w];
            d[x] = blend(b, c, (w > 7 ? 7 : w) * 36 + 3);
        }
    }
    /* the pause button over it */
    if (r->pause.px) {
        draw_sprite(r, &r->pause, clip, NULL);
    } else {
        pb_rect_t p = r->pause_rc;
        rc_clip(&p, clip);
        uint16_t c = rgb(r->t->c_accent ? r->t->c_accent : 0xFFFFFF);
        for (int y = p.y0; y < p.y1; y++) {
            for (int x = p.x0; x < p.x1; x++) {
                int lx = x - r->pause_rc.x0, ly = y - r->pause_rc.y0;
                bool bar = ly > 26 && ly < 66 && ((lx > 26 && lx < 38) || (lx > 50 && lx < 62));
                if (bar) r->fb[(size_t)y * PB_SW + x] = c;
            }
        }
    }
}

/* only the dots that changed: a video mode animates a few things at a time,
 * and the whole window again every frame (115 000 pixels) cost the board
 * 10 ms a frame. The window is cut in strips of 16 columns, each with the
 * box of its changed dots */
static void dmd_dirty(pb_render_t *r)
{
    const float pitch = (float)(DMD_X1 - DMD_X0) / DMD_W;
    for (int s0 = 0; s0 < DMD_W; s0 += 16) {
        int c0 = DMD_W, c1 = -1, r0 = DMD_H, r1 = -1;
        for (int y = 0; y < DMD_H; y++) {
            const uint8_t *a = &r->dmd.px[y][s0], *b = &r->dmd_shown.px[y][s0];
            if (!memcmp(a, b, 16)) continue;
            if (y < r0) r0 = y;
            r1 = y;
            for (int x = 0; x < 16; x++) {
                if (a[x] == b[x]) continue;
                if (s0 + x < c0) c0 = s0 + x;
                if (s0 + x > c1) c1 = s0 + x;
            }
        }
        if (c1 < 0) continue;
        pb_rect_t d = { (int16_t)(DMD_X0 + (int)(c0 * pitch) - 1), (int16_t)(DMD_Y0 + (int)(r0 * pitch) - 1),
                        (int16_t)(DMD_X0 + (int)((c1 + 1) * pitch) + 2), (int16_t)(DMD_Y0 + (int)((r1 + 1) * pitch) + 2) };
        pb_rect_t win = { DMD_X0, DMD_Y0, DMD_X1, DMD_Y1 };
        rc_clip(&d, &win);
        dirty(r, d);
    }
}

static void draw_launch(pb_render_t *r, const pb_ui_t *ui, const pb_rect_t *clip)
{
    if (!ui->launch_show) return;
    const pb_spr_t *s = &r->launch[ui->launch_down && r->launch[1].px ? 1 : 0];
    float cx, cy, rad;
    if (s->px) {
        draw_sprite(r, s, clip, NULL);
        cx = s->x + s->w * 0.5f;
        cy = s->y + s->h * 0.5f;
        rad = s->w * 0.5f;
    } else {
        cx = (r->launch_rc.x0 + r->launch_rc.x1) * 0.5f;
        cy = (r->launch_rc.y0 + r->launch_rc.y1) * 0.5f;
        rad = 72;
        pb_rect_t a = r->launch_rc;
        rc_clip(&a, clip);
        uint16_t c = rgb(ui->launch_down ? 0xFFFFFF : (r->t->c_accent ? r->t->c_accent : 0xFF4020));
        for (int y = a.y0; y < a.y1; y++) {
            for (int x = a.x0; x < a.x1; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                if (dx * dx + dy * dy < rad * rad) r->fb[(size_t)y * PB_SW + x] = c;
            }
        }
    }
    if (!ui->launch_down) return;
    /* the charge: a ring that fills clockwise from the top */
    pb_rect_t a = r->launch_rc;
    rc_clip(&a, clip);
    float r0 = rad + 3, r1 = rad + 12;
    float lim = ui->charge * 2 * PB_PI;
    uint16_t on = rgb(r->t->c_accent ? r->t->c_accent : 0xFFD040), off = rgb(0x303030);
    for (int y = a.y0; y < a.y1; y++) {
        for (int x = a.x0; x < a.x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float q = dx * dx + dy * dy;
            if (q < r0 * r0 || q > r1 * r1) continue;
            float ang = atan2f(dx, -dy);
            if (ang < 0) ang += 2 * PB_PI;
            r->fb[(size_t)y * PB_SW + x] = ang <= lim ? on : off;
        }
    }
}

static void debug_line(pb_render_t *r, float x0, float y0, float z0, float x1, float y1, float z1, uint16_t c)
{
    float ax, ay, bx, by, d;
    pb_project(&r->t->cam, x0, y0, z0, &ax, &ay, &d);
    pb_project(&r->t->cam, x1, y1, z1, &bx, &by, &d);
    int n = (int)(fabsf(bx - ax) > fabsf(by - ay) ? fabsf(bx - ax) : fabsf(by - ay)) + 1;
    for (int i = 0; i <= n; i++) {
        int x = (int)(ax + (bx - ax) * i / n), y = (int)(ay + (by - ay) * i / n);
        if (x >= 0 && x < PB_SW && y >= PB_TOP && y < PB_SH) r->fb[(size_t)y * PB_SW + x] = c;
    }
}

static void draw_debug(pb_render_t *r, const pb_phys_t *ph)
{
    const pb_table_t *t = r->t;
    for (int i = 0; i < t->nseg; i++) {
        const pb_seg_t *s = &t->seg[i];
        debug_line(r, s->x0, s->y0, 0, s->x1, s->y1, 0, s->obj >= 0 ? 0xFC00 : 0xFFFF);
    }
    for (int i = 0; i < t->ncirc; i++) {
        const pb_circ_t *c = &t->circ[i];
        for (int k = 0; k < 12; k++) {
            float a0 = k * PB_PI / 6, a1 = (k + 1) * PB_PI / 6;
            debug_line(r, c->x + c->r * cosf(a0), c->y + c->r * sinf(a0), 0, c->x + c->r * cosf(a1),
                       c->y + c->r * sinf(a1), 0, 0xFFE0);
        }
    }
    for (int i = 0; i < t->ntrig; i++) {
        const pb_trig_t *g = &t->trig[i];
        debug_line(r, g->x0, g->y0, 0, g->x1, g->y1, 0, 0x041F);
    }
    for (int i = 0; i < t->nramp; i++) {
        const pb_ramp_t *rp = &t->ramp[i];
        for (int k = 1; k < rp->n; k++)
            debug_line(r, rp->x[k - 1], rp->y[k - 1], rp->z[k - 1], rp->x[k], rp->y[k], rp->z[k], 0xF81F);
    }
    for (int s = 0; s < t->nflip; s++) {
        const pb_flipdef_t *f = &t->flip[s];
        float a = ph->flip[s].ang;
        debug_line(r, f->x, f->y, 0, f->x + cosf(a) * f->len, f->y + sinf(a) * f->len, 0, 0x07E0);
    }
}

/* ---- a frame -------------------------------------------------------------- */

void pb_render_frame(pb_render_t *r, const pb_game_t *g, const pb_phys_t *ph, const pb_ui_t *ui)
{
    r->frames++;
    const pb_table_t *t = r->t;
    /* what changed since the frame before */
    for (int i = 0; i < t->nlamp; i++) {
        bool on = g->lamp_on[i];
        if (on != r->lamp_vis[i]) {
            r->lamp_vis[i] = on;
            if (r->lamp[i].px) patch_changed(r, &r->lamp[i]);
        }
    }
    for (int i = 0; i < t->nobj; i++) {
        if (ph->down[i] != r->drop_vis[i]) {
            r->drop_vis[i] = ph->down[i];
            if (r->drop[i].px) patch_changed(r, &r->drop[i]);
        }
    }
    int ff[PB_MAX_FLIP];
    int tf[PB_MAX_TOY], to[PB_MAX_TOY];
    for (int k = 0; k < t->ntoy; k++) {
        tf[k] = -1;
        to[k] = 255;
        int n = r->ntoy[k];
        if (!n) continue;
        const pb_toydef_t *d = &t->toy[k];
        const pb_toy_t *y = &ph->toy[k];
        switch (d->kind) {
        case TK_BASH:
            if (y->level >= 0.98f || n <= 4) {
                static const uint8_t WOB[7] = { 1, 2, 3, 3, 2, 1, 0 };
                int i = (int)(y->anim / 0.06f);
                tf[k] = i < 7 ? WOB[i] : 0;
                if (tf[k] >= n) tf[k] = 0;
            } else {
                int i = 4 + (int)((1.0f - y->level) * (n - 4));
                tf[k] = i >= n ? n - 1 : i;
            }
            break;
        case TK_MOVER: {
            float kp = 0.5f - 0.5f * cosf(2 * PB_PI * y->phase);
            tf[k] = (int)(kp * (n - 1) + 0.5f);
            to[k] = (int)(y->level * 255);
            break;
        }
        case TK_DISC: {
            /* the frames cover one repeat of the disc's pattern (a1 of them
             * in a turn): a smooth turn from few frames */
            float rep = 2 * PB_PI / (d->a1 >= 1 ? d->a1 : 1);
            float u = fmodf(y->phase, rep) / rep;
            int i = (int)(u * n + 0.5f);
            tf[k] = ((i % n) + n) % n;
            break;
        }
        case TK_CANNON: {
            float deg = y->phase * 180.0f / PB_PI;
            float u = d->a1 != d->a0 ? (deg - d->a0) / (d->a1 - d->a0) : 0;
            if (u < 0) u = 0;
            if (u > 1) u = 1;
            tf[k] = (int)(u * (n - 1) + 0.5f);
            break;
        }
        default:
            break;
        }
        int key = tf[k] < 0 ? -1 : tf[k] * 256 + to[k];
        /* a disc turns all the time and is big (the roulette, ~17 000
         * pixels): a new frame of it every other frame is still smooth and
         * halves what it costs */
        if (d->kind == TK_DISC && key != r->toy_drawn[k] && r->toy_drawn[k] >= 0 &&
            r->frames - r->toy_when[k] < 2) {
            key = r->toy_drawn[k];
            tf[k] = key >> 8;
        }
        if (key != r->toy_drawn[k]) {
            r->toy_drawn[k] = key;
            r->toy_when[k] = r->frames;
            dirty(r, r->toy_area[k]);
        }
    }
    for (int s = 0; s < t->nflip; s++) {
        int n = r->nflip[s];
        ff[s] = n ? (int)(pb_phys_flip_t(ph, s) * (n - 1) + 0.5f) : 0;
        if (ff[s] != r->flip_drawn[s]) {
            r->flip_drawn[s] = ff[s];
            dirty(r, r->flip_area[s]);
        }
    }
    int sf = 0;
    if (r->nspin && r->spin_obj >= 0) {
        float a = fmodf(ph->spin_ang[r->spin_obj], 2 * PB_PI);
        if (a < 0) a += 2 * PB_PI;
        sf = (int)(a / (2 * PB_PI) * r->nspin + 0.5f) % r->nspin;
        if (sf != r->spin_drawn) {
            r->spin_drawn = sf;
            dirty(r, r->spin_area);
        }
    }
    ball_view_t bv[PB_MAX_BALLS];
    int order[PB_MAX_BALLS], nb = 0;
    for (int i = 0; i < r->nprev; i++) dirty(r, r->prev[i]);
    r->nprev = 0;
    for (int i = 0; i < PB_MAX_BALLS; i++) {
        if (ph->ball[i].state == BS_OFF) continue;
        ball_view(r, &ph->ball[i], &bv[nb]);
        dirty(r, bv[nb].rc);
        dirty(r, bv[nb].shadow);
        r->prev[r->nprev++] = bv[nb].rc;
        r->prev[r->nprev++] = bv[nb].shadow;
        order[nb] = nb;
        nb++;
    }
    /* far balls first */
    for (int i = 1; i < nb; i++) {
        for (int j = i; j > 0 && bv[order[j]].depth > bv[order[j - 1]].depth; j--) {
            int s = order[j];
            order[j] = order[j - 1];
            order[j - 1] = s;
        }
    }
    int ls = ui->launch_show ? (ui->launch_down ? 100 + (int)(ui->charge * 40) : 0) : -1;
    if (ls != r->launch_drawn) {
        r->launch_drawn = ls;
        dirty(r, r->launch_rc);
    }
    if (ui->pause_down != r->pause_drawn) {
        r->pause_drawn = ui->pause_down;
        dirty(r, r->pause_rc);
    }
    pb_game_dmd((pb_game_t *)g, &r->dmd);
    if (!r->dmd_valid) {
        pb_rect_t a = { DMD_X0, DMD_Y0, DMD_X1, DMD_Y1 };
        dirty(r, a);
    } else {
        dmd_dirty(r);
    }
    r->dmd_shown = r->dmd;
    r->dmd_valid = true;
    if (ui->debug != r->debug) {
        r->debug = ui->debug;
        pb_render_full(r);
    }
    if (r->debug) pb_render_full(r);

    /* draw them */
    for (int k = 0; k < r->ndirty; k++) {
        const pb_rect_t *c = &r->dirty[k];
        if (c->y0 < PB_TOP) draw_dmd(r, c);
        if (c->y1 <= PB_TOP) continue;
        compose_table(r, c);
        for (int i = 0; i < nb; i++) draw_shadow(r, &bv[i], c);
        /* the toys flat on the playfield (discs) first, then the rest */
        for (int pass = 0; pass < 2; pass++) {
            for (int k = 0; k < t->ntoy; k++) {
                if (tf[k] < 0 || (t->toy[k].kind == TK_DISC) != (pass == 0)) continue;
                const pb_spr_t *sp = &r->toy[k][tf[k]];
                if (sp->z) draw_spritez(r, sp, c, (unsigned)to[k]);
                else draw_sprite_opa(r, sp, c, r->zrow_flip, (unsigned)to[k]);
            }
            if (pass == 0) {
                for (int s = 0; s < t->nflip; s++) {
                    if (r->nflip[s]) draw_sprite(r, &r->flip[s][ff[s]], c, r->zrow_flip);
                }
            }
        }
        if (r->nspin) draw_sprite(r, &r->spin[sf], c, r->zrow_flip);
        for (int i = 0; i < nb; i++) draw_ball(r, &bv[order[i]], c);
        draw_launch(r, ui, c);
    }
    if (r->debug) draw_debug(r, ph);
}
