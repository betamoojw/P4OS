/*
 * PINBALL - the ball and what it meets (see pb_phys.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "pb_phys.h"

#include "pb_pak.h"

#include <math.h>
#include <string.h>

#define PB_PI       3.14159265f
#define STEP        0.001f      /* seconds a substep                         */
#define CELL        40.0f       /* mm, the grid                              */
#define VMAX        5200.0f     /* mm/s                                      */
#define G_UP        9600.0f     /* gravity across the playfield (ramps)      */
#define FLIP_UP     34.0f       /* rad/s, the stroke (54 degrees in ~28 ms)  */
#define FLIP_DOWN   22.0f
#define REST_E_MIN  110.0f      /* slower than this into a wall: no bounce   */
#define SAUCER_GRAB 1700.0f     /* faster than this, it rolls over the saucer */

static const float MAT_E[MT_N] = { 0.42f, 0.78f, 0.60f, 0.38f, 0.32f, 0.38f };
static const float MAT_F[MT_N] = { 0.04f, 0.07f, 0.05f, 0.04f, 0.03f, 0.10f };

static uint32_t rnd(pb_phys_t *p)
{
    p->rng ^= p->rng << 13;
    p->rng ^= p->rng >> 17;
    p->rng ^= p->rng << 5;
    return p->rng;
}

static float frand(pb_phys_t *p)
{
    return (float)(rnd(p) & 0xFFFF) / 65535.0f;
}

static void event(pb_phys_t *p, int kind, int ball, int obj, float speed)
{
    if (p->nev >= PB_MAX_EV) return;
    pb_event_t *e = &p->ev[p->nev++];
    e->kind = (uint8_t)kind;
    e->ball = (int8_t)ball;
    e->obj = (int16_t)obj;
    e->speed = speed;
}

/* ---- the grid ---------------------------------------------------------- */

static void grid_bbox(const pb_phys_t *p, float x0, float y0, float x1, float y1, int *c0, int *r0, int *c1, int *r1)
{
    float m = p->t->ball_r + 6.0f;
    if (x0 > x1) { float s = x0; x0 = x1; x1 = s; }
    if (y0 > y1) { float s = y0; y0 = y1; y1 = s; }
    *c0 = (int)floorf((x0 - m) / CELL);
    *c1 = (int)floorf((x1 + m) / CELL);
    *r0 = (int)floorf((y0 - m) / CELL);
    *r1 = (int)floorf((y1 + m) / CELL);
    if (*c0 < 0) *c0 = 0;
    if (*r0 < 0) *r0 = 0;
    if (*c1 >= p->gw) *c1 = p->gw - 1;
    if (*r1 >= p->gh) *r1 = p->gh - 1;
}

static bool grid_build(pb_phys_t *p)
{
    const pb_table_t *t = p->t;
    p->gw = (int)(t->w / CELL) + 2;
    p->gh = (int)(t->h / CELL) + 2;
    int ncell = p->gw * p->gh;
    p->cell_at = (uint16_t *)pb_calloc((size_t)(ncell + 1) * sizeof(uint16_t));
    if (!p->cell_at) return false;
    /* two passes: count, then fill */
    uint16_t *cnt = (uint16_t *)pb_calloc((size_t)ncell * sizeof(uint16_t));
    if (!cnt) return false;
    int total = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            int acc = 0;
            for (int i = 0; i < ncell; i++) {
                p->cell_at[i] = (uint16_t)acc;
                acc += cnt[i];
                cnt[i] = 0;
            }
            p->cell_at[ncell] = (uint16_t)acc;
            p->cell_ref = (uint16_t *)pb_malloc((size_t)(acc ? acc : 1) * sizeof(uint16_t));
            if (!p->cell_ref) {
                pb_free(cnt);
                return false;
            }
        }
        for (int i = 0; i < t->nseg + t->ncirc; i++) {
            int c0, r0, c1, r1;
            if (i < t->nseg) {
                const pb_seg_t *s = &t->seg[i];
                grid_bbox(p, s->x0, s->y0, s->x1, s->y1, &c0, &r0, &c1, &r1);
            } else {
                const pb_circ_t *c = &t->circ[i - t->nseg];
                grid_bbox(p, c->x - c->r, c->y - c->r, c->x + c->r, c->y + c->r, &c0, &r0, &c1, &r1);
            }
            for (int r = r0; r <= r1; r++) {
                for (int c = c0; c <= c1; c++) {
                    int k = r * p->gw + c;
                    if (pass == 1) {
                        p->cell_ref[p->cell_at[k] + cnt[k]] =
                            (uint16_t)(i < t->nseg ? i : (0x8000 | (i - t->nseg)));
                    } else {
                        total++;
                    }
                    cnt[k]++;
                }
            }
        }
    }
    pb_free(cnt);
    (void)total;
    return true;
}

bool pb_phys_init(pb_phys_t *p, const pb_table_t *t)
{
    memset(p, 0, sizeof *p);
    p->t = t;
    p->rng = 0x9E3779B9u;
    p->saucer_hold = 1.2f;
    if (!grid_build(p)) {
        pb_phys_free(p);
        return false;
    }
    pb_phys_reset(p);
    return true;
}

void pb_phys_free(pb_phys_t *p)
{
    pb_free(p->cell_at);
    pb_free(p->cell_ref);
    p->cell_at = NULL;
    p->cell_ref = NULL;
}

void pb_phys_reset(pb_phys_t *p)
{
    memset(p->ball, 0, sizeof p->ball);
    memset(p->down, 0, sizeof p->down);
    memset(p->cool, 0, sizeof p->cool);
    memset(p->spin_vel, 0, sizeof p->spin_vel);
    for (int i = 0; i < 2; i++) {
        p->flip[i].ang = p->t->flip[i].rest;
        p->flip[i].omega = 0;
        p->flip[i].held = false;
    }
    p->kickback_lit = false;
    p->tilted = false;
    p->nev = 0;
    p->acc = 0;
}

/* ---- serving and launching -------------------------------------------- */

int pb_phys_on_plunger(const pb_phys_t *p)
{
    for (int i = 0; i < PB_MAX_BALLS; i++) {
        if (p->ball[i].state == BS_PLUNGER) return i;
    }
    return -1;
}

int pb_phys_balls(const pb_phys_t *p)
{
    int n = 0;
    for (int i = 0; i < PB_MAX_BALLS; i++) n += p->ball[i].state != BS_OFF;
    return n;
}

int pb_phys_serve(pb_phys_t *p)
{
    if (pb_phys_on_plunger(p) >= 0) return -1;
    /* the lane must be clear: a ball still rolling down it would overlap */
    for (int i = 0; i < PB_MAX_BALLS; i++) {
        const pb_ball_t *b = &p->ball[i];
        if (b->state == BS_FREE && b->x > p->t->plunger_x - 30 && b->y > p->t->plunger_y - 60) return -1;
    }
    for (int i = 0; i < PB_MAX_BALLS; i++) {
        pb_ball_t *b = &p->ball[i];
        if (b->state != BS_OFF) continue;
        memset(b, 0, sizeof *b);
        b->state = BS_PLUNGER;
        b->x = b->px = p->t->plunger_x;
        b->y = b->py = p->t->plunger_y;
        b->saucer = b->saucer_skip = -1;
        b->ramp = -1;
        return i;
    }
    return -1;
}

bool pb_phys_launch(pb_phys_t *p, float power)
{
    int i = pb_phys_on_plunger(p);
    if (i < 0) return false;
    if (power < 0) power = 0;
    if (power > 1) power = 1;
    pb_ball_t *b = &p->ball[i];
    b->state = BS_FREE;
    b->vx = 0;
    b->vy = -(1450.0f + 2350.0f * power) * (0.985f + 0.03f * frand(p));
    b->still = 0;
    event(p, EV_LAUNCH, i, -1, -b->vy);
    return true;
}

void pb_phys_nudge(pb_phys_t *p, float dx, float dy)
{
    for (int i = 0; i < PB_MAX_BALLS; i++) {
        pb_ball_t *b = &p->ball[i];
        if (b->state != BS_FREE) continue;
        b->vx += dx;
        b->vy += dy;
    }
}

void pb_phys_flipper(pb_phys_t *p, int side, bool held)
{
    if (p->tilted) held = false;
    if (held && !p->flip[side].held) event(p, EV_FLIP, -1, side, 0);
    p->flip[side].held = held;
}

void pb_phys_drops_up(pb_phys_t *p, const char *bank)
{
    size_t n = strlen(bank);
    for (int i = 0; i < p->t->nobj; i++) {
        const pb_obj_t *o = &p->t->obj[i];
        if (o->type == OT_DROP && !strncmp(o->name, bank, n)) p->down[i] = 0;
    }
}

void pb_phys_saucer_release(pb_phys_t *p)
{
    for (int i = 0; i < PB_MAX_BALLS; i++) {
        if (p->ball[i].state == BS_SAUCER) p->ball[i].hold = 0;
    }
}

float pb_phys_flip_t(const pb_phys_t *p, int side)
{
    const pb_flipdef_t *f = &p->t->flip[side];
    float t = (p->flip[side].ang - f->rest) / (f->up - f->rest);
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

/* ---- contacts ---------------------------------------------------------- */

/* the ball b against a surface at contact normal (nx, ny), the surface
 * moving at (sx, sy): bounce with restitution e and friction f */
static float bounce(pb_ball_t *b, float nx, float ny, float sx, float sy, float e, float f)
{
    float rvx = b->vx - sx, rvy = b->vy - sy;
    float vn = rvx * nx + rvy * ny;
    if (vn >= 0) return 0;
    if (-vn < REST_E_MIN) e = 0;
    /* friction takes from the tangent only in a knock: a ball resting on a
     * flipper or rolling along a wall touches it every substep and must
     * still roll freely */
    float k = -vn / 600.0f;
    f *= k > 1.0f ? 1.0f : k;
    float tx = rvx - vn * nx, ty = rvy - vn * ny;
    rvx = tx * (1.0f - f) - e * vn * nx;
    rvy = ty * (1.0f - f) - e * vn * ny;
    b->vx = rvx + sx;
    b->vy = rvy + sy;
    return -vn;
}

static void obj_hit(pb_phys_t *p, int bi, int obj, float nx, float ny, float vn)
{
    const pb_table_t *t = p->t;
    pb_ball_t *b = &p->ball[bi];
    if (obj < 0) {
        if (vn > 900) event(p, EV_WALL, bi, -1, vn);
        return;
    }
    const pb_obj_t *o = &t->obj[obj];
    switch (o->type) {
    case OT_BUMPER:
        if (p->tilted || p->cool[obj] > 0) break;
        /* the skirt slams the ball away whatever it came in at */
        {
            float k = o->a > 0 ? o->a : 2000.0f;
            float vn2 = b->vx * nx + b->vy * ny;
            if (vn2 < k) {
                b->vx += (k - vn2) * nx;
                b->vy += (k - vn2) * ny;
            }
            p->cool[obj] = 0.06f;
            event(p, EV_HIT, bi, obj, vn);
        }
        break;
    case OT_SLING:
        if (vn < 260 || p->tilted || p->cool[obj] > 0) break;
        {
            /* the kick leans up the table: square to the face it sends the
             * ball across into the other outlane */
            float k = o->a > 0 ? o->a : 1300.0f;
            float kx = nx, ky = ny - 0.45f;
            float kl = sqrtf(kx * kx + ky * ky);
            b->vx += k * kx / kl;
            b->vy += k * ky / kl;
            p->cool[obj] = 0.12f;
            event(p, EV_HIT, bi, obj, vn);
        }
        break;
    case OT_DROP:
        if (vn > 160) {
            p->down[obj] = 1;
            event(p, EV_HIT, bi, obj, vn);
        }
        break;
    case OT_TARGET:
    case OT_POST:
        if (vn > 140 && p->cool[obj] <= 0) {
            p->cool[obj] = 0.1f;
            event(p, EV_HIT, bi, obj, vn);
        }
        break;
    default:
        if (vn > 900) event(p, EV_WALL, bi, -1, vn);
        break;
    }
}

static void collide_seg(pb_phys_t *p, int bi, const pb_seg_t *s)
{
    pb_ball_t *b = &p->ball[bi];
    const float R = p->t->ball_r;
    if (s->obj >= 0) {
        const pb_obj_t *o = &p->t->obj[s->obj];
        if (o->type == OT_DROP && p->down[s->obj]) return;
        if (o->type == OT_GATE) {
            /* one way: the ball passes going to the left of a -> b */
            float nlx = s->dy, nly = -s->dx;
            if ((b->x - s->x0) * nlx + (b->y - s->y0) * nly < 0) return;
        }
    }
    float qx = b->x - s->x0, qy = b->y - s->y0;
    float u = qx * s->dx + qy * s->dy;
    if (u < 0) u = 0;
    else if (u > s->len) u = s->len;
    float cx = s->x0 + s->dx * u, cy = s->y0 + s->dy * u;
    float dx = b->x - cx, dy = b->y - cy;
    float d2 = dx * dx + dy * dy;
    if (d2 >= R * R) return;
    float d = sqrtf(d2);
    float nx, ny;
    if (d < 1e-4f) {
        nx = s->dy;
        ny = -s->dx;
        d = 0;
    } else {
        nx = dx / d;
        ny = dy / d;
    }
    b->x += nx * (R - d);
    b->y += ny * (R - d);
    float vn = bounce(b, nx, ny, 0, 0, MAT_E[s->mat], MAT_F[s->mat]);
    if (vn > 0) obj_hit(p, bi, s->obj, nx, ny, vn);
}

static void collide_circ(pb_phys_t *p, int bi, const pb_circ_t *c)
{
    pb_ball_t *b = &p->ball[bi];
    const float R = p->t->ball_r + c->r;
    float dx = b->x - c->x, dy = b->y - c->y;
    float d2 = dx * dx + dy * dy;
    if (d2 >= R * R) return;
    float d = sqrtf(d2);
    float nx = d > 1e-4f ? dx / d : 0, ny = d > 1e-4f ? dy / d : -1;
    b->x = c->x + nx * R;
    b->y = c->y + ny * R;
    float vn = bounce(b, nx, ny, 0, 0, MAT_E[c->mat], MAT_F[c->mat]);
    /* a bumper fires on a touch, even a slow one */
    if (vn > 0 || (c->obj >= 0 && p->t->obj[c->obj].type == OT_BUMPER)) obj_hit(p, bi, c->obj, nx, ny, vn);
}

static void collide_flipper(pb_phys_t *p, int bi, int side)
{
    pb_ball_t *b = &p->ball[bi];
    const pb_flipdef_t *f = &p->t->flip[side];
    const pb_flip_t *fs = &p->flip[side];
    const float R = p->t->ball_r;
    float ex = cosf(fs->ang), ey = sinf(fs->ang);
    float qx = b->x - f->x, qy = b->y - f->y;
    if (qx * qx + qy * qy > (f->len + f->r0 + R) * (f->len + f->r0 + R)) return;
    float u = qx * ex + qy * ey;
    if (u < 0) u = 0;
    else if (u > f->len) u = f->len;
    float rr = f->r0 + (f->r1 - f->r0) * (u / f->len);
    float cx = f->x + ex * u, cy = f->y + ey * u;
    float dx = b->x - cx, dy = b->y - cy;
    float d2 = dx * dx + dy * dy;
    float lim = R + rr;
    if (d2 >= lim * lim) return;
    float d = sqrtf(d2);
    float nx = d > 1e-4f ? dx / d : -ey, ny = d > 1e-4f ? dy / d : ex;
    b->x = cx + nx * lim;
    b->y = cy + ny * lim;
    /* the surface under the ball moves with the flipper */
    float px = cx + nx * rr - f->x, py = cy + ny * rr - f->y;
    float sx = -fs->omega * py, sy = fs->omega * px;
    bounce(b, nx, ny, sx, sy, MAT_E[MT_FLIPPER], MAT_F[MT_FLIPPER]);
}

static void collide_balls(pb_phys_t *p)
{
    const float R2 = p->t->ball_r * 2;
    for (int i = 0; i < PB_MAX_BALLS; i++) {
        pb_ball_t *a = &p->ball[i];
        if (a->state != BS_FREE && a->state != BS_PLUNGER && a->state != BS_SAUCER) continue;
        for (int j = i + 1; j < PB_MAX_BALLS; j++) {
            pb_ball_t *c = &p->ball[j];
            if (c->state != BS_FREE && c->state != BS_PLUNGER && c->state != BS_SAUCER) continue;
            float dx = c->x - a->x, dy = c->y - a->y;
            float d2 = dx * dx + dy * dy;
            if (d2 >= R2 * R2 || d2 < 1e-6f) continue;
            float d = sqrtf(d2), nx = dx / d, ny = dy / d;
            float push = (R2 - d) * 0.5f;
            bool am = a->state == BS_FREE, cm = c->state == BS_FREE;
            if (am && cm) {
                a->x -= nx * push;
                a->y -= ny * push;
                c->x += nx * push;
                c->y += ny * push;
            } else if (am) {
                a->x -= nx * push * 2;
                a->y -= ny * push * 2;
            } else if (cm) {
                c->x += nx * push * 2;
                c->y += ny * push * 2;
            }
            float rv = (c->vx - a->vx) * nx + (c->vy - a->vy) * ny;
            if (rv >= 0) continue;
            float j2 = -(1.0f + 0.9f) * rv * 0.5f;
            if (am && cm) {
                a->vx -= j2 * nx;
                a->vy -= j2 * ny;
                c->vx += j2 * nx;
                c->vy += j2 * ny;
            } else if (am) {
                a->vx -= 2 * j2 * nx;
                a->vy -= 2 * j2 * ny;
            } else if (cm) {
                c->vx += 2 * j2 * nx;
                c->vy += 2 * j2 * ny;
            }
        }
    }
}

/* ---- triggers ---------------------------------------------------------- */

/* did the ball's centre cross the trigger line this substep? +1 to the
 * left of a -> b (y down), -1 the other way, 0 not at all */
static int crossed(const pb_trig_t *g, const pb_ball_t *b)
{
    float nlx = g->dy, nly = -g->dx;
    float s0 = (b->px - g->x0) * nlx + (b->py - g->y0) * nly;
    float s1 = (b->x - g->x0) * nlx + (b->y - g->y0) * nly;
    if ((s0 < 0) == (s1 < 0)) return 0;
    float k = s0 / (s0 - s1);
    float cx = b->px + (b->x - b->px) * k, cy = b->py + (b->y - b->py) * k;
    float u = (cx - g->x0) * g->dx + (cy - g->y0) * g->dy;
    if (u < -2 || u > g->len + 2) return 0;
    return s1 > 0 ? 1 : -1;
}

static void ramp_pos(const pb_ramp_t *r, float s, float *x, float *y, float *z, float *dx, float *dy, float *dz)
{
    int i = 1;
    while (i < r->n - 1 && r->s[i] < s) i++;
    float l = r->s[i] - r->s[i - 1];
    float k = l > 1e-3f ? (s - r->s[i - 1]) / l : 0;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    *x = r->x[i - 1] + (r->x[i] - r->x[i - 1]) * k;
    *y = r->y[i - 1] + (r->y[i] - r->y[i - 1]) * k;
    *z = r->z[i - 1] + (r->z[i] - r->z[i - 1]) * k;
    float il = l > 1e-3f ? 1.0f / l : 0;
    *dx = (r->x[i] - r->x[i - 1]) * il;
    *dy = (r->y[i] - r->y[i - 1]) * il;
    *dz = (r->z[i] - r->z[i - 1]) * il;
}

static void triggers(pb_phys_t *p, int bi)
{
    const pb_table_t *t = p->t;
    pb_ball_t *b = &p->ball[bi];
    for (int i = 0; i < t->ntrig; i++) {
        const pb_trig_t *g = &t->trig[i];
        int c = crossed(g, b);
        if (!c) continue;
        const pb_obj_t *o = &t->obj[g->obj];
        float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
        if (o->type == OT_SPINNER) {
            float v = fabsf(b->vx * g->dy - b->vy * g->dx);
            p->spin_vel[g->obj] = (c > 0 ? 1.0f : -1.0f) * (v * 0.012f + 4.0f);
        } else if (o->type == OT_RAMP && c > 0) {
            int ri = -1;
            for (int k = 0; k < t->nramp; k++) {
                if (t->ramp[k].obj == g->obj) ri = k;
            }
            if (ri < 0) continue;
            b->state = BS_RAMP;
            b->ramp = (int8_t)ri;
            b->rs = 0;
            b->rv = sp;
            event(p, EV_RAMP_IN, bi, g->obj, sp);
            return;
        } else if (o->type == OT_KICKBACK && b->vy > 0) {
            if (p->kickback_lit && !p->tilted) {
                b->vx = o->a;
                b->vy = o->b;
                b->x = (g->x0 + g->x1) * 0.5f;
                p->kickback_lit = false;
                event(p, EV_KICKBACK, bi, g->obj, sp);
            }
        }
    }
    for (int i = 0; i < t->nroll; i++) {
        const pb_roll_t *r = &t->roll[i];
        float dx = b->x - r->x, dy = b->y - r->y;
        uint32_t bit = 1u << (i & 31);
        uint32_t *w = &b->inside[i >> 5];
        bool in = dx * dx + dy * dy < r->r * r->r;
        if (!in) {
            *w &= ~bit;
            if (b->saucer_skip == r->obj && dx * dx + dy * dy > (r->r + 8) * (r->r + 8)) b->saucer_skip = -1;
            continue;
        }
        if (*w & bit) continue;
        *w |= bit;
        const pb_obj_t *o = &t->obj[r->obj];
        float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
        if (o->type == OT_SAUCER) {
            if (b->saucer_skip == r->obj || sp > SAUCER_GRAB) continue;
            b->state = BS_SAUCER;
            b->saucer = r->obj;
            b->x = r->x;
            b->y = r->y;
            b->vx = b->vy = 0;
            b->hold = p->saucer_hold;
            event(p, EV_SAUCER, bi, r->obj, sp);
            return;
        }
        event(p, EV_ROLL, bi, r->obj, sp);
    }
}

/* ---- one substep -------------------------------------------------------- */

static void step_ball(pb_phys_t *p, int bi)
{
    const pb_table_t *t = p->t;
    pb_ball_t *b = &p->ball[bi];
    switch (b->state) {
    case BS_PLUNGER:
        b->x = b->px = t->plunger_x;
        b->y = b->py = t->plunger_y;
        b->vx = b->vy = 0;
        return;
    case BS_SAUCER: {
        b->hold -= STEP;
        if (b->hold > 0) return;
        const pb_obj_t *o = &t->obj[b->saucer];
        float a = (o->a + (frand(p) - 0.5f) * 8.0f) * PB_PI / 180.0f;
        float v = o->b > 0 ? o->b : 1800.0f;
        b->state = BS_FREE;
        b->vx = cosf(a) * v;
        b->vy = sinf(a) * v;
        b->saucer_skip = b->saucer;
        event(p, EV_EJECT, bi, b->saucer, v);
        b->saucer = -1;
        return;
    }
    case BS_RAMP: {
        const pb_ramp_t *r = &t->ramp[b->ramp];
        float x, y, z, dx, dy, dz;
        ramp_pos(r, b->rs, &x, &y, &z, &dx, &dy, &dz);
        float a = t->grav * dy - G_UP * dz - (b->rv > 0 ? 260.0f : -260.0f);
        b->rv += a * STEP;
        b->rs += b->rv * STEP;
        if (b->rs >= r->total) {
            ramp_pos(r, r->total, &x, &y, &z, &dx, &dy, &dz);
            float v = r->exit_speed > 0 ? r->exit_speed : b->rv;
            float l = sqrtf(dx * dx + dy * dy);
            if (l < 1e-3f) l = 1;
            b->state = BS_FREE;
            b->x = b->px = x;
            b->y = b->py = y;
            b->z = 0;
            b->vx = dx / l * v;
            b->vy = dy / l * v;
            event(p, EV_RAMP_DONE, bi, r->obj, v);
            return;
        }
        if (b->rs <= 0) {
            /* not enough: back down the mouth the way it came */
            ramp_pos(r, 0, &x, &y, &z, &dx, &dy, &dz);
            float l = sqrtf(dx * dx + dy * dy);
            if (l < 1e-3f) l = 1;
            float v = -b->rv;
            b->state = BS_FREE;
            b->x = b->px = x - dx / l * 3.0f;
            b->y = b->py = y - dy / l * 3.0f;
            b->z = 0;
            b->vx = -dx / l * v;
            b->vy = -dy / l * v;
            event(p, EV_RAMP_FAIL, bi, r->obj, v);
            return;
        }
        ramp_pos(r, b->rs, &x, &y, &z, &dx, &dy, &dz);
        b->x = b->px = x;
        b->y = b->py = y;
        b->z = z;
        b->vx = dx * b->rv;
        b->vy = dy * b->rv;
        b->roll += fabsf(b->rv) * STEP;
        return;
    }
    case BS_FREE:
        break;
    default:
        return;
    }
    b->px = b->x;
    b->py = b->y;
    b->vy += t->grav * STEP;
    /* rolling: a little drag */
    b->vx *= 1.0f - 0.10f * STEP;
    b->vy *= 1.0f - 0.10f * STEP;
    float sp2 = b->vx * b->vx + b->vy * b->vy;
    if (sp2 > VMAX * VMAX) {
        float k = VMAX / sqrtf(sp2);
        b->vx *= k;
        b->vy *= k;
    }
    b->x += b->vx * STEP;
    b->y += b->vy * STEP;
    b->roll += sqrtf(sp2) * STEP;

    for (int it = 0; it < 2; it++) {
        int cx = (int)(b->x / CELL), cy = (int)(b->y / CELL);
        if (cx >= 0 && cy >= 0 && cx < p->gw && cy < p->gh) {
            int k = cy * p->gw + cx;
            for (int j = p->cell_at[k]; j < p->cell_at[k + 1]; j++) {
                uint16_t r = p->cell_ref[j];
                if (r & 0x8000) collide_circ(p, bi, &t->circ[r & 0x7FFF]);
                else collide_seg(p, bi, &t->seg[r]);
            }
        }
        if (b->y > t->flip[0].y - 120) {
            collide_flipper(p, bi, 0);
            collide_flipper(p, bi, 1);
        }
    }
    triggers(p, bi);
    if (b->state != BS_FREE) return;
    /* back to the plunger after a launch too weak to leave the lane */
    if (b->x > t->plunger_x - 12 && b->y > t->plunger_y - 3 && sp2 < 80.0f * 80.0f) {
        b->state = BS_PLUNGER;
        return;
    }
    if (b->y > t->h + 30 || b->x < -40 || b->x > t->w + 40 || b->y < -60) {
        b->state = BS_OFF;
        event(p, EV_DRAIN, bi, -1, 0);
        return;
    }
    /* stuck somewhere: shake it loose */
    if (sp2 < 25.0f * 25.0f && b->x < t->plunger_x - 12) {
        b->still += STEP;
        if (b->still > 4.0f) {
            b->still = 0;
            p->kicks++;
            p->kick_x = b->x;
            p->kick_y = b->y;
            b->vx += (frand(p) - 0.5f) * 600.0f;
            b->vy -= 300.0f + frand(p) * 300.0f;
        }
    } else {
        b->still = 0;
    }
}

void pb_phys_step(pb_phys_t *p, float dt)
{
    const pb_table_t *t = p->t;
    if (dt > 0.05f) dt = 0.05f;
    p->acc += dt;
    while (p->acc >= STEP) {
        p->acc -= STEP;
        for (int s = 0; s < 2; s++) {
            pb_flip_t *f = &p->flip[s];
            const pb_flipdef_t *d = &t->flip[s];
            float target = f->held ? d->up : d->rest;
            float dir = target > f->ang ? 1.0f : -1.0f;
            float sp = f->held ? FLIP_UP : FLIP_DOWN;
            float na = f->ang + dir * sp * STEP;
            if ((dir > 0 && na >= target) || (dir < 0 && na <= target)) {
                na = target;
                f->omega = 0;
            } else {
                f->omega = dir * sp;
            }
            f->ang = na;
        }
        for (int i = 0; i < t->nobj; i++) {
            if (p->cool[i] > 0) p->cool[i] -= STEP;
            if (p->spin_vel[i] != 0) {
                float before = p->spin_ang[i];
                p->spin_ang[i] += p->spin_vel[i] * STEP;
                float v = p->spin_vel[i];
                v -= (v > 0 ? 1.0f : -1.0f) * (2.5f + fabsf(v) * 0.9f) * STEP;
                if ((v > 0) != (p->spin_vel[i] > 0)) v = 0;
                p->spin_vel[i] = v;
                if ((int)floorf(before / PB_PI) != (int)floorf(p->spin_ang[i] / PB_PI)) event(p, EV_SPIN, -1, i, 0);
                if (v == 0) {
                    /* it comes to rest hanging down */
                    float k = roundf(p->spin_ang[i] / (2 * PB_PI));
                    p->spin_ang[i] = k * 2 * PB_PI;
                }
            }
        }
        for (int i = 0; i < PB_MAX_BALLS; i++) {
            if (p->ball[i].state != BS_OFF) step_ball(p, i);
        }
        collide_balls(p);
    }
}
