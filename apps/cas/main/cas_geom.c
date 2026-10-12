/*
 * P4OS - CAS: dynamic geometry, the model.
 *
 * Objects only ever point at objects before them, so one pass in order
 * recomputes a figure, and deleting one deletes what comes after it and
 * hangs from it. Intersections keep their slot (the first or the second
 * root in a fixed order: along the line, or to the left of the line between
 * the centres) so that a point does not jump to the other crossing while a
 * figure is dragged.
 */
#include "cas_geom.h"

#ifdef GEO_HOST_TEST
#define _(s) (s)
#define N_(s) (s)
#else
#include "aos_i18n.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EPS 1e-12

bool geo_is_point(int k)   { return k >= GK_FREE && k <= GK_INTER; }
bool geo_is_linear(int k)  { return k >= GK_SEG && k <= GK_ABIS; }
bool geo_is_round(int k)   { return k == GK_CIRCLE || k == GK_CIRC3; }
bool geo_is_measure(int k) { return k >= GK_DIST && k <= GK_COORD; }

void geo_range(int kind, double *t0, double *t1)
{
    *t0 = -INFINITY;
    *t1 = INFINITY;
    if (kind == GK_SEG) {
        *t0 = 0;
        *t1 = 1;
    } else if (kind == GK_RAY || kind == GK_ABIS) {
        *t0 = 0;
    }
}

static bool in_range(int kind, double t)
{
    double t0, t1;
    geo_range(kind, &t0, &t1);
    return t >= t0 - 1e-9 && t <= t1 + 1e-9;
}

void geo_clear(geo_t *g)
{
    memset(g, 0, sizeof *g);
}

/* --- intersections and projections --------------------------------------------- */

int geo_intersect(const geo_obj_t *a, const geo_obj_t *b, double out[2][2])
{
    out[0][0] = out[0][1] = out[1][0] = out[1][1] = NAN;
    if (!a->ok || !b->ok) {
        return 0;
    }
    if (geo_is_round(a->kind) && geo_is_linear(b->kind)) {
        const geo_obj_t *t = a;
        a = b;
        b = t;
    }
    if (geo_is_linear(a->kind) && geo_is_linear(b->kind)) {
        double cr = a->dx * b->dy - a->dy * b->dx;
        double la = hypot(a->dx, a->dy), lb = hypot(b->dx, b->dy);
        if (fabs(cr) <= 1e-12 * la * lb) {
            return 1;                       /* parallel: the slot exists, empty */
        }
        double wx = b->px - a->px, wy = b->py - a->py;
        double t = (wx * b->dy - wy * b->dx) / cr;
        double s = (wx * a->dy - wy * a->dx) / cr;
        if (in_range(a->kind, t) && in_range(b->kind, s)) {
            out[0][0] = a->px + t * a->dx;
            out[0][1] = a->py + t * a->dy;
        }
        return 1;
    }
    if (geo_is_linear(a->kind) && geo_is_round(b->kind)) {
        double fx = a->px - b->px, fy = a->py - b->py;
        double A = a->dx * a->dx + a->dy * a->dy;
        double B = 2 * (a->dx * fx + a->dy * fy);
        double C = fx * fx + fy * fy - b->r * b->r;
        double D = B * B - 4 * A * C;
        if (A < EPS) {
            return 2;
        }
        if (D < 0) {
            /* a tangent drawn by hand misses by a rounding: let it touch */
            if (D > -1e-9 * B * B) D = 0;
            else return 2;
        }
        double sq = sqrt(D);
        double t[2] = { (-B - sq) / (2 * A), (-B + sq) / (2 * A) };
        for (int k = 0; k < 2; k++) {
            if (in_range(a->kind, t[k])) {
                out[k][0] = a->px + t[k] * a->dx;
                out[k][1] = a->py + t[k] * a->dy;
            }
        }
        return 2;
    }
    if (geo_is_round(a->kind) && geo_is_round(b->kind)) {
        double ex = b->px - a->px, ey = b->py - a->py;
        double d = hypot(ex, ey);
        if (d < EPS || d > a->r + b->r + 1e-9 * d || d < fabs(a->r - b->r) - 1e-9 * d) {
            return 2;
        }
        double m = (a->r * a->r - b->r * b->r + d * d) / (2 * d);
        double h2 = a->r * a->r - m * m;
        double h = h2 > 0 ? sqrt(h2) : 0;
        double bx = a->px + m * ex / d, by = a->py + m * ey / d;
        double nx = -ey / d, ny = ex / d;
        out[0][0] = bx + h * nx;
        out[0][1] = by + h * ny;
        out[1][0] = bx - h * nx;
        out[1][1] = by - h * ny;
        return 2;
    }
    return 0;
}

static double seg_project(double ax, double ay, double bx, double by, double x, double y,
                          double *qx, double *qy)
{
    double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > EPS ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *qx = ax + t * dx;
    *qy = ay + t * dy;
    return t;
}

double geo_project(const geo_obj_t *c, double x, double y, double *qx, double *qy)
{
    *qx = x;
    *qy = y;
    if (geo_is_linear(c->kind)) {
        double l2 = c->dx * c->dx + c->dy * c->dy;
        double t = l2 > EPS ? ((x - c->px) * c->dx + (y - c->py) * c->dy) / l2 : 0;
        double t0, t1;
        geo_range(c->kind, &t0, &t1);
        if (t < t0) t = t0;
        if (t > t1) t = t1;
        *qx = c->px + t * c->dx;
        *qy = c->py + t * c->dy;
        return t;
    }
    if (geo_is_round(c->kind)) {
        double t = atan2(y - c->py, x - c->px);
        *qx = c->px + c->r * cos(t);
        *qy = c->py + c->r * sin(t);
        return t;
    }
    return 0;
}

/* a polygon's parameter: side k (from vertex k to k+1) plus the fraction */
static double poly_project(const geo_t *g, const geo_obj_t *c, double x, double y,
                           double *qx, double *qy)
{
    double best = INFINITY, bt = 0;
    for (int k = 0; k < c->n; k++) {
        const geo_obj_t *A = &g->o[c->a[k]], *B = &g->o[c->a[(k + 1) % c->n]];
        double px, py;
        double f = seg_project(A->px, A->py, B->px, B->py, x, y, &px, &py);
        double d = hypot(px - x, py - y);
        if (d < best) {
            best = d;
            bt = k + f;
            *qx = px;
            *qy = py;
        }
    }
    return bt;
}

double geo_dist_to(const geo_t *g, const geo_obj_t *c, double x, double y)
{
    if (!c->ok || c->hidden) {
        return INFINITY;
    }
    double qx, qy;
    if (geo_is_point(c->kind)) {
        return hypot(c->px - x, c->py - y);
    }
    if (geo_is_linear(c->kind) || geo_is_round(c->kind)) {
        geo_project(c, x, y, &qx, &qy);
        return hypot(qx - x, qy - y);
    }
    if (c->kind == GK_POLY) {
        poly_project(g, c, x, y, &qx, &qy);
        return hypot(qx - x, qy - y);
    }
    return INFINITY;
}

/* --- computing --------------------------------------------------------------------- */

static double poly_area(const geo_t *g, const geo_obj_t *o)
{
    double s = 0;
    for (int k = 0; k < o->n; k++) {
        const geo_obj_t *A = &g->o[o->a[k]], *B = &g->o[o->a[(k + 1) % o->n]];
        s += A->px * B->py - B->px * A->py;
    }
    return fabs(s) / 2;
}

static double poly_perim(const geo_t *g, const geo_obj_t *o)
{
    double s = 0;
    for (int k = 0; k < o->n; k++) {
        const geo_obj_t *A = &g->o[o->a[k]], *B = &g->o[o->a[(k + 1) % o->n]];
        s += hypot(B->px - A->px, B->py - A->py);
    }
    return s;
}

static void compute(geo_t *g, int i)
{
    geo_obj_t *o = &g->o[i];
    o->ok = false;
    if (!o->kind) {
        return;
    }
    for (int k = 0; k < o->n; k++) {
        if (o->a[k] < 0 || o->a[k] >= i || !g->o[o->a[k]].ok) {
            return;
        }
    }
    const geo_obj_t *A = o->n > 0 ? &g->o[o->a[0]] : NULL;
    const geo_obj_t *B = o->n > 1 ? &g->o[o->a[1]] : NULL;
    const geo_obj_t *C = o->n > 2 ? &g->o[o->a[2]] : NULL;
    switch (o->kind) {
    case GK_FREE:
        o->px = o->x;
        o->py = o->y;
        o->ok = isfinite(o->x) && isfinite(o->y);
        break;
    case GK_ON:
        if (geo_is_linear(A->kind)) {
            double t0, t1, t = o->t;
            geo_range(A->kind, &t0, &t1);
            if (t < t0) t = t0;
            if (t > t1) t = t1;
            o->px = A->px + t * A->dx;
            o->py = A->py + t * A->dy;
            o->ok = true;
        } else if (geo_is_round(A->kind)) {
            o->px = A->px + A->r * cos(o->t);
            o->py = A->py + A->r * sin(o->t);
            o->ok = true;
        } else if (A->kind == GK_POLY) {
            int k = (int)floor(o->t);
            double f = o->t - k;
            k = ((k % A->n) + A->n) % A->n;
            const geo_obj_t *P = &g->o[A->a[k]], *Q = &g->o[A->a[(k + 1) % A->n]];
            o->px = P->px + f * (Q->px - P->px);
            o->py = P->py + f * (Q->py - P->py);
            o->ok = true;
        }
        break;
    case GK_MID:
        o->px = (A->px + B->px) / 2;
        o->py = (A->py + B->py) / 2;
        o->ok = true;
        break;
    case GK_INTER: {
        double p[2][2];
        int n = geo_intersect(A, B, p);
        if (o->which < n && isfinite(p[o->which][0])) {
            o->px = p[o->which][0];
            o->py = p[o->which][1];
            o->ok = true;
        }
        break;
    }
    case GK_SEG:
    case GK_LINE:
    case GK_RAY:
        o->px = A->px;
        o->py = A->py;
        o->dx = B->px - A->px;
        o->dy = B->py - A->py;
        o->ok = hypot(o->dx, o->dy) > EPS;
        break;
    case GK_PERP:
    case GK_PAR: {
        /* through the point a[0], the direction of the line a[1] */
        o->px = A->px;
        o->py = A->py;
        double l = hypot(B->dx, B->dy);
        if (l < EPS) break;
        o->dx = o->kind == GK_PAR ? B->dx / l : -B->dy / l;
        o->dy = o->kind == GK_PAR ? B->dy / l : B->dx / l;
        o->ok = true;
        break;
    }
    case GK_PBIS: {
        o->px = (A->px + B->px) / 2;
        o->py = (A->py + B->py) / 2;
        double ex = B->px - A->px, ey = B->py - A->py, l = hypot(ex, ey);
        if (l < EPS) break;
        o->dx = -ey / l;
        o->dy = ex / l;
        o->ok = true;
        break;
    }
    case GK_ABIS: {
        double ux = A->px - B->px, uy = A->py - B->py, vx = C->px - B->px, vy = C->py - B->py;
        double lu = hypot(ux, uy), lv = hypot(vx, vy);
        if (lu < EPS || lv < EPS) break;
        double bx = ux / lu + vx / lv, by = uy / lu + vy / lv, lb = hypot(bx, by);
        if (lb < 1e-9) {            /* a straight angle: its bisector stands up */
            bx = -uy / lu;
            by = ux / lu;
            lb = 1;
        }
        o->px = B->px;
        o->py = B->py;
        o->dx = bx / lb;
        o->dy = by / lb;
        o->ok = true;
        break;
    }
    case GK_CIRCLE:
        o->px = A->px;
        o->py = A->py;
        o->r = hypot(B->px - A->px, B->py - A->py);
        o->ok = o->r > EPS;
        break;
    case GK_CIRC3: {
        double ax = A->px, ay = A->py, bx = B->px, by = B->py, cx = C->px, cy = C->py;
        double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
        double scale = fabs(bx - ax) + fabs(by - ay) + fabs(cx - ax) + fabs(cy - ay);
        if (fabs(d) < 1e-12 * scale * scale) break;       /* in a row: no circle */
        double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
        o->px = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d;
        o->py = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d;
        o->r = hypot(ax - o->px, ay - o->py);
        o->ok = true;
        break;
    }
    case GK_POLY:
        o->ok = o->n >= 3;
        break;
    case GK_DIST:
        o->val = hypot(B->px - A->px, B->py - A->py);
        o->ok = true;
        break;
    case GK_PERIM:
        if (geo_is_round(A->kind)) {
            o->val = 2 * M_PI * A->r;
            o->ok = true;
        } else if (A->kind == GK_POLY) {
            o->val = poly_perim(g, A);
            o->ok = true;
        }
        break;
    case GK_AREA:
        if (geo_is_round(A->kind)) {
            o->val = M_PI * A->r * A->r;
            o->ok = true;
        } else if (A->kind == GK_POLY) {
            o->val = poly_area(g, A);
            o->ok = true;
        }
        break;
    case GK_ANGLE: {
        double ux = A->px - B->px, uy = A->py - B->py, vx = C->px - B->px, vy = C->py - B->py;
        if (hypot(ux, uy) < EPS || hypot(vx, vy) < EPS) break;
        o->val = fabs(atan2(ux * vy - uy * vx, ux * vx + uy * vy)) * 180 / M_PI;
        o->ok = true;
        break;
    }
    case GK_EQ:
    case GK_COORD:
        o->ok = true;
        break;
    default:
        break;
    }
}

void geo_update(geo_t *g)
{
    for (int i = 0; i < g->n; i++) {
        compute(g, i);
    }
}

/* --- building -------------------------------------------------------------------------- */

static bool name_used(const geo_t *g, const char *s)
{
    for (int i = 0; i < g->n; i++) {
        if (g->o[i].kind && !strcmp(g->o[i].name, s)) return true;
    }
    return false;
}

static void next_name(const geo_t *g, char *out)
{
    static const char *const SUB[] = { "", "\xE2\x82\x81", "\xE2\x82\x82", "\xE2\x82\x83", "\xE2\x82\x84",
                                       "\xE2\x82\x85", "\xE2\x82\x86", "\xE2\x82\x87", "\xE2\x82\x88", "\xE2\x82\x89" };
    for (int s = 0; s < 10; s++) {
        for (char c = 'A'; c <= 'Z'; c++) {
            snprintf(out, 8, "%c%s", c, SUB[s]);
            if (!name_used(g, out)) return;
        }
    }
    out[0] = 0;
}

int geo_add(geo_t *g, int kind, const short *a, int n)
{
    if (g->n >= GEO_MAX || n > GEO_POLY || n < 0) {
        return -1;
    }
    geo_obj_t *o = &g->o[g->n];
    memset(o, 0, sizeof *o);
    o->kind = (unsigned char)kind;
    o->n = (unsigned char)n;
    for (int k = 0; k < n; k++) o->a[k] = a[k];
    if (geo_is_point(kind)) {
        next_name(g, o->name);
    }
    int i = g->n++;
    compute(g, i);
    return i;
}

int geo_add_free(geo_t *g, double x, double y)
{
    if (g->n >= GEO_MAX) return -1;
    int i = geo_add(g, GK_FREE, NULL, 0);
    g->o[i].x = x;
    g->o[i].y = y;
    compute(g, i);
    return i;
}

int geo_add_on(geo_t *g, int curve, double x, double y)
{
    short a[1] = { (short)curve };
    int i = geo_add(g, GK_ON, a, 1);
    if (i < 0) return -1;
    double qx, qy;
    const geo_obj_t *c = &g->o[curve];
    g->o[i].t = c->kind == GK_POLY ? poly_project(g, c, x, y, &qx, &qy) : geo_project(c, x, y, &qx, &qy);
    compute(g, i);
    return i;
}

bool geo_depends(const geo_t *g, int i, int on)
{
    if (i == on) return true;
    if (i < on) return false;
    const geo_obj_t *o = &g->o[i];
    for (int k = 0; k < o->n; k++) {
        if (geo_depends(g, o->a[k], on)) return true;
    }
    return false;
}

int geo_delete(geo_t *g, int i)
{
    if (i < 0 || i >= g->n) return 0;
    static short map[GEO_MAX];
    bool gone[GEO_MAX] = { 0 };
    gone[i] = true;
    for (int j = i + 1; j < g->n; j++) {
        for (int k = 0; k < g->o[j].n; k++) {
            if (gone[g->o[j].a[k]]) gone[j] = true;
        }
    }
    int w = 0, count = 0;
    for (int j = 0; j < g->n; j++) {
        if (gone[j]) {
            map[j] = -1;
            count++;
            continue;
        }
        map[j] = (short)w;
        if (w != j) g->o[w] = g->o[j];
        geo_obj_t *o = &g->o[w];
        for (int k = 0; k < o->n; k++) o->a[k] = map[o->a[k]];
        w++;
    }
    g->n = w;
    geo_update(g);
    return count;
}

bool geo_move(geo_t *g, int i, double x, double y)
{
    if (i < 0 || i >= g->n) return false;
    geo_obj_t *o = &g->o[i];
    if (o->kind == GK_FREE) {
        o->x = x;
        o->y = y;
    } else if (o->kind == GK_ON) {
        const geo_obj_t *c = &g->o[o->a[0]];
        if (!c->ok) return false;
        double qx, qy;
        o->t = c->kind == GK_POLY ? poly_project(g, c, x, y, &qx, &qy) : geo_project(c, x, y, &qx, &qy);
    } else {
        return false;
    }
    geo_update(g);
    return true;
}

/* --- numbers -------------------------------------------------------------------------- */

static long gcdl(long a, long b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) {
        long t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

bool geo_rational(double v, int maxq, long *p, long *q)
{
    if (!isfinite(v) || fabs(v) > 1e9) return false;
    for (long d = 1; d <= maxq; d++) {
        double n = round(v * d);
        if (fabs(v - n / d) <= 1e-9 * (fabs(v) > 1 ? fabs(v) : 1)) {
            long ln = (long)n, gg = gcdl(ln, d);
            *p = ln / gg;
            *q = d / gg;
            return true;
        }
    }
    return false;
}

/* "−" (the maths minus) instead of '-' at the start */
static void minus_fix(char *b, size_t n)
{
    if (b[0] == '-' && strlen(b) + 3 < n) {
        memmove(b + 3, b + 1, strlen(b));
        b[0] = (char)0xE2;
        b[1] = (char)0x88;
        b[2] = (char)0x92;
    }
}

void geo_fmt(char *b, size_t n, double v)
{
    if (!isfinite(v)) {
        snprintf(b, n, "\xE2\x80\x94");
        return;
    }
    if (fabs(v) < 5e-10) v = 0;
    snprintf(b, n, "%.6g", v);
    minus_fix(b, n);
}

static void fmt_frac(char *b, size_t n, long p, long q)
{
    if (q == 1) snprintf(b, n, "%ld", p);
    else snprintf(b, n, "%ld/%ld", p, q);
    minus_fix(b, n);
}

/* exact if it is a small fraction, a decimal otherwise */
static void fmt_num(char *b, size_t n, double v)
{
    long p, q;
    if (geo_rational(v, 64, &p, &q)) fmt_frac(b, n, p, q);
    else geo_fmt(b, n, v);
}

bool geo_surd(double v, char *b, size_t n)
{
    long p, q;
    if (v < 0 || !geo_rational(v, 100, &p, &q)) return false;
    /* sqrt(p/q) = sqrt(p*q)/q */
    long N = p * q;
    if (N > 100000000L) return false;
    long k = 1, m = N;
    for (long f = 2; f * f <= m; f++) {
        while (m % (f * f) == 0) {
            m /= f * f;
            k *= f;
        }
    }
    long gg = gcdl(k, q);
    k /= gg;
    q /= gg;
    if (m == 0) {
        snprintf(b, n, "0");
    } else if (m == 1) {
        fmt_frac(b, n, k, q);
    } else {
        char kk[24] = "", qq[24] = "";
        if (k != 1) snprintf(kk, sizeof kk, "%ld", k);
        if (q != 1) snprintf(qq, sizeof qq, "/%ld", q);
        snprintf(b, n, "%s\xE2\x88\x9A%ld%s", kk, m, qq);
    }
    return true;
}

/* a decimal that says the same as the fraction (25/2 = 12.5) or not (1/3 ≈ 0.333333) */
static bool exact_decimal(long q)
{
    while (q % 2 == 0) q /= 2;
    while (q % 5 == 0) q /= 5;
    return q == 1;
}

/* --- text ------------------------------------------------------------------------------- */

static const char *pname(const geo_t *g, int i)
{
    return i >= 0 && i < g->n && g->o[i].name[0] ? g->o[i].name : "?";
}

/* the name of a curve: its points ("AB", "ABC"), "Circ. A" for a circle */
static void curve_name(const geo_t *g, int i, char *b, size_t n)
{
    const geo_obj_t *o = &g->o[i];
    b[0] = 0;
    switch (o->kind) {
    case GK_CIRCLE:
        snprintf(b, n, "%s %s", _("Circ."), pname(g, o->a[0]));
        break;
    case GK_CIRC3:
        snprintf(b, n, "%s %s%s%s", _("Circ."), pname(g, o->a[0]), pname(g, o->a[1]), pname(g, o->a[2]));
        break;
    case GK_POLY:
        if (o->n > 6) {
            snprintf(b, n, "%s%s\xE2\x80\xA6%s", pname(g, o->a[0]), pname(g, o->a[1]), pname(g, o->a[o->n - 1]));
            break;
        }
        for (int k = 0; k < o->n; k++) {
            size_t l = strlen(b);
            snprintf(b + l, n - l, "%s", pname(g, o->a[k]));
        }
        break;
    case GK_SEG:
    case GK_LINE:
    case GK_RAY:
        snprintf(b, n, "%s%s", pname(g, o->a[0]), pname(g, o->a[1]));
        break;
    default:
        snprintf(b, n, "%s", geo_is_point(o->kind) ? pname(g, i) : "");
        break;
    }
}

void geo_label(const geo_t *g, int i, char *b, size_t n)
{
    const geo_obj_t *o = &g->o[i];
    char c[48];
    b[0] = 0;
    switch (o->kind) {
    case GK_DIST:
        snprintf(b, n, "%s%s", pname(g, o->a[0]), pname(g, o->a[1]));
        break;
    case GK_ANGLE:
        snprintf(b, n, "%s %s%s%s", _("Ángulo"), pname(g, o->a[0]), pname(g, o->a[1]), pname(g, o->a[2]));
        break;
    case GK_PERIM:
        curve_name(g, o->a[0], c, sizeof c);
        snprintf(b, n, "%s %s", _("Perímetro"), c);
        break;
    case GK_AREA:
        curve_name(g, o->a[0], c, sizeof c);
        snprintf(b, n, "%s %s", _("Área"), c);
        break;
    case GK_EQ: {
        const geo_obj_t *s = &g->o[o->a[0]];
        if (geo_is_round(s->kind)) {
            curve_name(g, o->a[0], c, sizeof c);
            snprintf(b, n, "%s", c);
        } else if (s->kind == GK_SEG || s->kind == GK_LINE || s->kind == GK_RAY) {
            curve_name(g, o->a[0], c, sizeof c);
            snprintf(b, n, "%s %s", _("Recta"), c);
        } else {
            static const char *const W[] = { N_("Perpendicular"), N_("Paralela"), N_("Mediatriz"), N_("Bisectriz") };
            int w = s->kind - GK_PERP;
            snprintf(b, n, "%s", w >= 0 && w < 4 ? _(W[w]) : _("Recta"));
        }
        break;
    }
    case GK_COORD:
        snprintf(b, n, "%s", pname(g, o->a[0]));
        break;
    default:
        curve_name(g, i, b, n);
        break;
    }
}

/* "+ 3" / "− 3" for a term after the first */
static void term(char *b, size_t n, double v)
{
    char t[40];
    fmt_num(t, sizeof t, fabs(v));
    snprintf(b, n, " %s %s", v < 0 ? "\xE2\x88\x92" : "+", t);
}

/* (x − h)² with h exact when it can be */
static void square(char *b, size_t n, const char *var, double h)
{
    if (fabs(h) < 1e-12) {
        snprintf(b, n, "%s\xC2\xB2", var);
        return;
    }
    char t[40];
    fmt_num(t, sizeof t, fabs(h));
    snprintf(b, n, "(%s %s %s)\xC2\xB2", var, h > 0 ? "\xE2\x88\x92" : "+", t);
}

void geo_value(const geo_t *g, int i, char *b, size_t n)
{
    const geo_obj_t *o = &g->o[i];
    b[0] = 0;
    if (!o->ok) {
        snprintf(b, n, "%s", _("no existe"));
        return;
    }
    char ex[64], dec[40];
    switch (o->kind) {
    case GK_DIST: {
        const geo_obj_t *A = &g->o[o->a[0]], *B = &g->o[o->a[1]];
        double d2 = (B->px - A->px) * (B->px - A->px) + (B->py - A->py) * (B->py - A->py);
        geo_fmt(dec, sizeof dec, o->val);
        if (geo_surd(d2, ex, sizeof ex) && strstr(ex, "\xE2\x88\x9A")) snprintf(b, n, "%s \xE2\x89\x88 %s", ex, dec);
        else if (geo_surd(d2, ex, sizeof ex) && strchr(ex, '/')) snprintf(b, n, "%s = %s", ex, dec);
        else snprintf(b, n, "%s", dec);
        break;
    }
    case GK_PERIM:
    case GK_AREA: {
        const geo_obj_t *s = &g->o[o->a[0]];
        geo_fmt(dec, sizeof dec, o->val);
        long p, q;
        if (geo_is_round(s->kind)) {
            /* in π: 2πr and πr², with r or r² a fraction */
            double k = o->kind == GK_AREA ? s->r * s->r : 2 * s->r;
            if (geo_rational(k, 64, &p, &q)) {
                char f[32];
                if (p == 1 && q == 1) snprintf(f, sizeof f, "\xCF\x80");
                else if (q == 1) snprintf(f, sizeof f, "%ld\xCF\x80", p);
                else if (p == 1) snprintf(f, sizeof f, "\xCF\x80/%ld", q);
                else snprintf(f, sizeof f, "%ld\xCF\x80/%ld", p, q);
                snprintf(b, n, "%s \xE2\x89\x88 %s", f, dec);
                break;
            }
        } else if (o->kind == GK_AREA && geo_rational(o->val, 64, &p, &q) && q != 1) {
            fmt_frac(ex, sizeof ex, p, q);
            snprintf(b, n, "%s %s %s", ex, exact_decimal(q) ? "=" : "\xE2\x89\x88", dec);
            break;
        }
        snprintf(b, n, "%s", dec);
        break;
    }
    case GK_ANGLE: {
        double v = o->val;
        if (fabs(v - round(v)) < 1e-7) v = round(v);
        geo_fmt(dec, sizeof dec, v);
        snprintf(b, n, "%s\xC2\xB0", dec);
        break;
    }
    case GK_COORD: {
        const geo_obj_t *P = &g->o[o->a[0]];
        char x[40], y[40];
        fmt_num(x, sizeof x, P->px);
        fmt_num(y, sizeof y, P->py);
        snprintf(b, n, "(%s ; %s)", x, y);
        break;
    }
    case GK_EQ: {
        const geo_obj_t *s = &g->o[o->a[0]];
        if (geo_is_round(s->kind)) {
            char sx[48], sy[48], r2[40];
            square(sx, sizeof sx, "x", s->px);
            square(sy, sizeof sy, "y", s->py);
            fmt_num(r2, sizeof r2, s->r * s->r);
            snprintf(b, n, "%s + %s = %s", sx, sy, r2);
        } else if (geo_is_linear(s->kind)) {
            /* a·x + b·y = c, said as y = m·x + k or x = c */
            double A = -s->dy, B = s->dx, C = A * s->px + B * s->py;
            double scale = hypot(A, B);
            if (fabs(B) < 1e-12 * scale) {
                fmt_num(ex, sizeof ex, C / A);
                snprintf(b, n, "x = %s", ex);
                break;
            }
            double m = -A / B, k = C / B;
            if (fabs(m) < 1e-12) {
                fmt_num(ex, sizeof ex, k);
                snprintf(b, n, "y = %s", ex);
                break;
            }
            char ms[80], ks[48] = "";
            long p, q;
            if (fabs(m - 1) < 1e-12) snprintf(ms, sizeof ms, "x");
            else if (fabs(m + 1) < 1e-12) snprintf(ms, sizeof ms, "\xE2\x88\x92x");
            else if (geo_rational(m, 64, &p, &q) && q != 1) {
                fmt_frac(ex, sizeof ex, p, q);
                snprintf(ms, sizeof ms, "(%s)x", ex);
            } else {
                fmt_num(ex, sizeof ex, m);
                snprintf(ms, sizeof ms, "%sx", ex);
            }
            if (fabs(k) > 1e-12) term(ks, sizeof ks, k);
            snprintf(b, n, "y = %s%s", ms, ks);
        }
        break;
    }
    default:
        if (geo_is_point(o->kind)) {
            char x[40], y[40];
            fmt_num(x, sizeof x, o->px);
            fmt_num(y, sizeof y, o->py);
            snprintf(b, n, "(%s ; %s)", x, y);
        }
        break;
    }
}

/* --- saving ------------------------------------------------------------------------------- */

void geo_write_line(const geo_t *g, int i, char *b, size_t n)
{
    const geo_obj_t *o = &g->o[i];
    int l = snprintf(b, n, "O %d %d %d %.17g %.17g %.17g %s", o->kind, o->which, o->hidden ? 1 : 0,
                     o->x, o->y, o->t, o->name[0] ? o->name : "-");
    for (int k = 0; k < o->n && l > 0 && (size_t)l < n; k++) {
        l += snprintf(b + l, n - (size_t)l, " %d", o->a[k]);
    }
}

bool geo_read_line(geo_t *g, const char *line)
{
    if (line[0] != 'O' || g->n >= GEO_MAX) return false;
    int kind, which, hidden, used = 0;
    double x, y, t;
    char name[16];
    if (sscanf(line + 1, "%d %d %d %lf %lf %lf %15s%n", &kind, &which, &hidden, &x, &y, &t, name, &used) < 7) {
        return false;
    }
    if (kind < GK_FREE || kind > GK_COORD) return false;
    geo_obj_t *o = &g->o[g->n];
    memset(o, 0, sizeof *o);
    o->kind = (unsigned char)kind;
    o->which = (unsigned char)(which & 1);
    o->hidden = hidden != 0;
    o->x = x;
    o->y = y;
    o->t = t;
    if (strcmp(name, "-")) snprintf(o->name, sizeof o->name, "%.7s", name);
    const char *s = line + 1 + used;
    int v, k = 0, m;
    while (k < GEO_POLY && sscanf(s, "%d%n", &v, &m) == 1) {
        if (v < 0 || v >= g->n) return false;      /* only objects before it */
        o->a[k++] = (short)v;
        s += m;
    }
    o->n = (unsigned char)k;
    g->n++;
    return true;
}
