/*
 * P4OS - CAS: dynamic geometry, the view.
 *
 * The figure (cas_geom.c) drawn on a canvas that takes the screen, a tool
 * chosen from a sheet, and the measurements in a list under it. A tap
 * builds with the tool in hand: it takes the point under the finger, or
 * makes one (on a curve if it lands on one, at a crossing if it lands on
 * two, on the grid's nearest node when snapping is on). Dragging a point
 * that can move moves it, and the figure follows; dragging anywhere else
 * pans, and two fingers zoom, as in the grapher.
 *
 * Every change is a snapshot first, so "undo" is taking the last one back.
 */
#include "cas_app.h"
#include "cas_geom.h"
#include "cas_plot.h"
#include "cas_save.h"

#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "mx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define UNDO_MAX   24
#define HIT_POINT  34           /* px: a finger on a point            */
#define HIT_CURVE  24           /* px: a finger on a line or a circle */

#define C_CURVE  0x64D2FF
#define C_FILL   0x0A84FF
#define C_FREE   0x0A84FF
#define C_ON     0x30D158
#define C_FIXED  0xAEAEB2
#define C_PICK   0xFFD60A
#define C_MEAS   0xFFD60A

typedef enum {
    T_MOVE, T_POINT, T_MID, T_INTER,
    T_SEG, T_LINE, T_RAY, T_PERP, T_PAR, T_PBIS, T_ABIS,
    T_CIRCLE, T_CIRC3, T_POLY,
    T_LEN, T_ANGLE, T_AREA, T_EQ, T_COORD,
    T_DELETE,
    T_COUNT
} tool_t;

static const char *const TOOL_NAME[T_COUNT] = {
    N_("Mover"), N_("Punto"), N_("Punto medio"), N_("Intersección"),
    N_("Segmento"), N_("Recta"), N_("Semirrecta"), N_("Perpendicular"), N_("Paralela"), N_("Mediatriz"), N_("Bisectriz"),
    N_("Circunferencia"), N_("Circ. por 3 puntos"), N_("Polígono"),
    N_("Longitud"), N_("Ángulo"), N_("Área"), N_("Ecuación"), N_("Coordenadas"),
    N_("Eliminar"),
};

typedef struct {
    int     n;
    geo_obj_t *o;
} snap_t;

static struct {
    geo_t      *g;
    bool        init;
    float       cx, cy, scale;
    bool        grid, snap;
    tool_t      tool;
    short       pend[GEO_POLY];
    int         npend;
    int         sel;            /* Move: the object shown in the pill */
    int         drag;           /* the point under the finger, -1 none */
    bool        panning;
    bool        moved;          /* this touch dragged or pinched: not a tap */
    snap_t      undo[UNDO_MAX];
    int         nundo;
    /* widgets */
    lv_obj_t   *parent, *box, *canvas, *panel, *list, *hint, *toolbtn, *sheet;
    lv_obj_t   *vals[GEO_MAX];  /* the value label of each measurement row, by object */
    cas_plot_t  plot;
    int         W, H;
    bool        landscape, dirty, shown, list_dirty, panel_hidden;
    lv_timer_t *timer;
} V;

static void save(void);
static void build(void);
static void list_build(void);

/* --- the window -------------------------------------------------------------------- */

static float  wx_to_sx(double x) { return (float)(V.W / 2.0 + (x - V.cx) * V.scale); }
static float  wy_to_sy(double y) { return (float)(V.H / 2.0 - (y - V.cy) * V.scale); }
static double sx_to_wx(float sx) { return V.cx + (sx - V.W / 2.0) / V.scale; }
static double sy_to_wy(float sy) { return V.cy + (V.H / 2.0 - sy) / V.scale; }

/* the grid's step, as cas_plot draws it: snapping lands on its lines */
static double grid_step(void)
{
    double raw = 80.0 / V.scale;
    double pw = pow(10.0, floor(log10(raw)));
    double m = raw / pw;
    return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * pw;
}

static void snap_xy(double *x, double *y)
{
    if (!V.snap) return;
    double s = grid_step();
    *x = round(*x / s) * s;
    *y = round(*y / s) * s;
    if (fabs(*x) < s * 1e-9) *x = 0;
    if (fabs(*y) < s * 1e-9) *y = 0;
}

/* --- undo ------------------------------------------------------------------------- */

static void snap_free(snap_t *s)
{
    mx_free(s->o);
    s->o = NULL;
    s->n = 0;
}

static void push_undo(void)
{
    if (V.nundo == UNDO_MAX) {
        snap_free(&V.undo[0]);
        memmove(&V.undo[0], &V.undo[1], sizeof V.undo[0] * (UNDO_MAX - 1));
        V.nundo--;
    }
    snap_t *s = &V.undo[V.nundo];
    s->n = V.g->n;
    s->o = s->n ? mx_alloc(sizeof(geo_obj_t) * (size_t)s->n) : NULL;
    if (s->n && !s->o) {
        s->n = 0;
        return;
    }
    if (s->n) memcpy(s->o, V.g->o, sizeof(geo_obj_t) * (size_t)s->n);
    V.nundo++;
}

static void pop_undo(void)
{
    if (!V.nundo) {
        aos_ui_toast(_("Nada para deshacer"), 1200);
        return;
    }
    snap_t *s = &V.undo[--V.nundo];
    V.g->n = s->n;
    if (s->n) memcpy(V.g->o, s->o, sizeof(geo_obj_t) * (size_t)s->n);
    snap_free(s);
    geo_update(V.g);
    V.npend = 0;
    V.sel = -1;
    V.dirty = V.list_dirty = true;
    save();
}

/* --- kept between openings ------------------------------------------------------------
 * data/cas_geo.txt: "V cx cy scale grid snap tool", then one "O ..." per object. */

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/cas_geo.txt", aos_hal_path_data());
}

static void save(void)
{
    if (!V.init) return;
    char path[200];
    state_path(path, sizeof path);
    mx_str_t b = { 0 };
    cas_str_printf(&b, "V %g %g %g %d %d %d\n", (double)V.cx, (double)V.cy, (double)V.scale,
                   V.grid ? 1 : 0, V.snap ? 1 : 0, (int)V.tool);
    char line[256];
    for (int i = 0; i < V.g->n; i++) {
        geo_write_line(V.g, i, line, sizeof line);
        mx_str_add(&b, line);
        mx_str_addc(&b, '\n');
    }
    cas_save_text(path, &b);
}

static void load(void)
{
    char path[200];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'V') {
            float cx, cy, sc;
            int gr, sn, tl;
            if (sscanf(line + 1, "%f %f %f %d %d %d", &cx, &cy, &sc, &gr, &sn, &tl) == 6 && sc > 0) {
                V.cx = cx;
                V.cy = cy;
                V.scale = sc;
                V.grid = gr != 0;
                V.snap = sn != 0;
                if (tl >= 0 && tl < T_COUNT) V.tool = (tool_t)tl;
            }
        } else if (line[0] == 'O') {
            if (!geo_read_line(V.g, line)) break;      /* a broken tail: keep what came before */
        }
    }
    fclose(f);
    geo_update(V.g);
}

static bool model_init(void)
{
    if (V.init) return true;
    V.g = mx_alloc(sizeof *V.g);
    if (!V.g) return false;
    V.init = true;
    V.cx = 0;
    V.cy = 0;
    V.scale = 60;
    V.grid = true;
    V.snap = true;
    V.tool = T_POINT;
    V.sel = -1;
    V.drag = -1;
    load();
    return true;
}

/* --- finding what is under a finger ------------------------------------------------------ */

static int hit_point(float sx, float sy)
{
    int best = -1;
    float bd = HIT_POINT;
    for (int i = 0; i < V.g->n; i++) {
        const geo_obj_t *o = &V.g->o[i];
        if (!geo_is_point(o->kind) || !o->ok || o->hidden) continue;
        float d = hypotf(wx_to_sx(o->px) - sx, wy_to_sy(o->py) - sy);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

enum { F_LINEAR = 1, F_ROUND = 2, F_POLY = 4, F_SEG = 8 };

static bool fits(int kind, int filter)
{
    return ((filter & F_LINEAR) && geo_is_linear(kind)) || ((filter & F_ROUND) && geo_is_round(kind))
        || ((filter & F_POLY) && kind == GK_POLY) || ((filter & F_SEG) && kind == GK_SEG);
}

/* the curves near the finger, nearest first (up to two) */
static int hit_curves(float sx, float sy, int filter, int out[2])
{
    double wx = sx_to_wx(sx), wy = sy_to_wy(sy);
    double lim = HIT_CURVE / V.scale;
    double d0 = lim, d1 = lim;
    out[0] = out[1] = -1;
    for (int i = 0; i < V.g->n; i++) {
        const geo_obj_t *o = &V.g->o[i];
        if (!fits(o->kind, filter)) continue;
        double d = geo_dist_to(V.g, o, wx, wy);
        if (d < d0) {
            d1 = d0;
            out[1] = out[0];
            d0 = d;
            out[0] = i;
        } else if (d < d1) {
            d1 = d;
            out[1] = i;
        }
    }
    return (out[0] >= 0) + (out[1] >= 0);
}

/* inside a polygon, or a circle too (Area: a tap in the middle of it;
 * Delete leaves circles out, they cover too much) */
static int hit_inside(double x, double y, bool circles)
{
    for (int i = V.g->n - 1; i >= 0; i--) {
        const geo_obj_t *o = &V.g->o[i];
        if (!o->ok || o->hidden) continue;
        if (circles && geo_is_round(o->kind) && hypot(x - o->px, y - o->py) < o->r) return i;
        if (o->kind == GK_POLY) {
            bool in = false;
            for (int k = 0, j = o->n - 1; k < o->n; j = k++) {
                const geo_obj_t *A = &V.g->o[o->a[k]], *B = &V.g->o[o->a[j]];
                if ((A->py > y) != (B->py > y) && x < (B->px - A->px) * (y - A->py) / (B->py - A->py) + A->px) in = !in;
            }
            if (in) return i;
        }
    }
    return -1;
}

static int find_same(int kind, const short *a, int n, int which)
{
    for (int i = 0; i < V.g->n; i++) {
        const geo_obj_t *o = &V.g->o[i];
        if (o->kind != kind || o->n != n || (kind == GK_INTER && o->which != which)) continue;
        bool same = true;
        for (int k = 0; k < n && same; k++) same = o->a[k] == a[k];
        if (same) return i;
    }
    return -1;
}

static int add_unique(int kind, const short *a, int n)
{
    int i = find_same(kind, a, n, 0);
    if (i >= 0) return i;
    i = geo_add(V.g, kind, a, n);
    if (i < 0) aos_ui_toast(_("La figura está llena"), 1500);
    return i;
}

/* An intersection slot of two curves, made if it is not there yet. */
static int add_inter(int c0, int c1, int which)
{
    short a[2] = { (short)(c0 < c1 ? c0 : c1), (short)(c0 < c1 ? c1 : c0) };
    int i = find_same(GK_INTER, a, 2, which);
    if (i >= 0) return i;
    i = geo_add(V.g, GK_INTER, a, 2);
    if (i < 0) return -1;
    V.g->o[i].which = (unsigned char)which;
    geo_update(V.g);
    return i;
}

/* The point a tap means: one already there, a crossing of the two curves
 * under the finger, a point on the curve under it, or a new free point. */
static int take_point(float sx, float sy)
{
    int p = hit_point(sx, sy);
    if (p >= 0) return p;
    int c[2];
    int nc = hit_curves(sx, sy, F_LINEAR | F_ROUND | F_POLY, c);
    if (nc == 2 && V.g->o[c[0]].kind != GK_POLY && V.g->o[c[1]].kind != GK_POLY) {
        double pts[2][2];
        int ns = geo_intersect(&V.g->o[c[0]], &V.g->o[c[1]], pts);
        for (int k = 0; k < ns; k++) {
            if (isfinite(pts[k][0]) && hypotf(wx_to_sx(pts[k][0]) - sx, wy_to_sy(pts[k][1]) - sy) < HIT_POINT) {
                return add_inter(c[0], c[1], k);
            }
        }
    }
    if (nc >= 1) {
        return geo_add_on(V.g, c[0], sx_to_wx(sx), sy_to_wy(sy));
    }
    double x = sx_to_wx(sx), y = sy_to_wy(sy);
    snap_xy(&x, &y);
    return geo_add_free(V.g, x, y);
}

/* --- the tools ---------------------------------------------------------------------- */

static const char *step_hint(void)
{
    int n = V.npend;
    switch (V.tool) {
    case T_MOVE:   return _("Arrastrá un punto, o tocá algo para verlo");
    case T_POINT:  return _("Tocá donde va el punto");
    case T_MID:
    case T_PBIS:   return n ? _("Tocá el segundo punto") : _("Tocá un segmento o el primer punto");
    case T_INTER:  return n ? _("Tocá la otra línea") : _("Tocá dos líneas, o donde se cruzan");
    case T_SEG:
    case T_LINE:
    case T_RAY:    return n ? _("Tocá el segundo punto") : _("Tocá el primer punto");
    case T_PERP:
    case T_PAR:    return n ? _("Ahora el otro: la recta o el punto") : _("Tocá una recta y un punto");
    case T_ABIS:
    case T_ANGLE:  return n == 0 ? _("Tocá un punto del primer lado") : n == 1 ? _("Tocá el vértice") : _("Tocá un punto del otro lado");
    case T_CIRCLE: return n ? _("Tocá un punto de la circunferencia") : _("Tocá el centro");
    case T_CIRC3:  return n == 0 ? _("Tocá el primer punto") : n == 1 ? _("Tocá el segundo punto") : _("Tocá el tercer punto");
    case T_POLY:   return n < 3 ? _("Tocá los vértices") : _("Tocá el primero para cerrarlo");
    case T_LEN:    return n ? _("Tocá el segundo punto") : _("Tocá dos puntos, un segmento o una figura");
    case T_AREA:   return _("Tocá un polígono o un círculo");
    case T_EQ:     return _("Tocá una recta o una circunferencia");
    case T_COORD:  return _("Tocá un punto");
    case T_DELETE: return _("Tocá lo que querés borrar");
    default:       return "";
    }
}

static void set_hint(const char *s)
{
    if (V.hint) lv_label_set_text(V.hint, s);
}

static void tool_hint(void)
{
    set_hint(step_hint());
}

/* the object built: the figure changed */
static void built(int i)
{
    V.npend = 0;
    V.dirty = true;
    if (i >= 0 && geo_is_measure(V.g->o[i].kind)) {
        V.list_dirty = true;
        if (V.panel && V.panel_hidden) {
            V.panel_hidden = false;
            lv_obj_remove_flag(V.panel, LV_OBJ_FLAG_HIDDEN);
            build();
        }
    }
    save();
    tool_hint();
}

static void pend(int i)
{
    if (V.npend < GEO_POLY && i >= 0) V.pend[V.npend++] = (short)i;
    V.dirty = true;
    tool_hint();
}

/* a pending point the tap means, taken back if the tool is not done with it */
static int take_new_point(float sx, float sy)
{
    int p = take_point(sx, sy);
    for (int k = 0; k < V.npend; k++) {
        if (V.pend[k] == p) return -2;              /* the same point twice */
    }
    return p;
}

static void select_obj(float sx, float sy)
{
    int p = hit_point(sx, sy);
    int c[2];
    if (p < 0 && hit_curves(sx, sy, F_LINEAR | F_ROUND | F_POLY, c)) p = c[0];
    V.sel = p;
    V.dirty = true;
    if (p < 0) {
        tool_hint();
        return;
    }
    /* what it is, said like a measurement: coordinates, an equation */
    char l[64], v[96], b[180];
    const geo_obj_t *o = &V.g->o[p];
    geo_label(V.g, p, l, sizeof l);
    if (geo_is_point(o->kind)) {
        geo_value(V.g, p, v, sizeof v);
    } else if (geo_is_linear(o->kind) || geo_is_round(o->kind)) {
        /* borrow a measurement's text without keeping it */
        geo_t *g = V.g;
        if (g->n < GEO_MAX) {
            short a[1] = { (short)p };
            int m = geo_add(g, GK_EQ, a, 1);
            geo_value(g, m, v, sizeof v);
            g->n--;
        } else {
            v[0] = 0;
        }
    } else {
        v[0] = 0;
    }
    snprintf(b, sizeof b, "%s   %s", l, v);
    set_hint(b);
}

static void tap(float sx, float sy)
{
    geo_t *g = V.g;
    int c[2], i = -1;
    if (V.tool == T_MOVE) {
        select_obj(sx, sy);
        return;
    }
    push_undo();
    int n0 = g->n;
    switch (V.tool) {
    case T_POINT:
        i = take_point(sx, sy);
        built(i);
        break;

    case T_SEG:
    case T_LINE:
    case T_RAY:
    case T_CIRCLE: {
        int p = take_new_point(sx, sy);
        if (p == -2) break;
        if (!V.npend) {
            pend(p);
            break;
        }
        static const int K[] = { [T_SEG] = GK_SEG, [T_LINE] = GK_LINE, [T_RAY] = GK_RAY, [T_CIRCLE] = GK_CIRCLE };
        short a[2] = { V.pend[0], (short)p };
        built(add_unique(K[V.tool], a, 2));
        break;
    }

    case T_MID:
    case T_PBIS: {
        int kind = V.tool == T_MID ? GK_MID : GK_PBIS;
        if (!V.npend && hit_point(sx, sy) < 0 && hit_curves(sx, sy, F_SEG, c)) {
            short a[2] = { g->o[c[0]].a[0], g->o[c[0]].a[1] };
            built(add_unique(kind, a, 2));
            break;
        }
        int p = take_new_point(sx, sy);
        if (p == -2) break;
        if (!V.npend) {
            pend(p);
            break;
        }
        short a[2] = { V.pend[0], (short)p };
        built(add_unique(kind, a, 2));
        break;
    }

    case T_CIRC3:
    case T_ABIS:
    case T_ANGLE: {
        int p = take_new_point(sx, sy);
        if (p == -2) break;
        if (V.npend < 2) {
            pend(p);
            break;
        }
        int kind = V.tool == T_CIRC3 ? GK_CIRC3 : V.tool == T_ABIS ? GK_ABIS : GK_ANGLE;
        short a[3] = { V.pend[0], V.pend[1], (short)p };
        built(add_unique(kind, a, 3));
        break;
    }

    case T_POLY: {
        int p = hit_point(sx, sy);
        if (V.npend >= 3 && p == V.pend[0]) {
            built(add_unique(GK_POLY, V.pend, V.npend));
            break;
        }
        p = take_new_point(sx, sy);
        if (p == -2) break;
        pend(p);
        if (V.npend == GEO_POLY) built(add_unique(GK_POLY, V.pend, V.npend));
        break;
    }

    case T_INTER: {
        int nc = hit_curves(sx, sy, F_LINEAR | F_ROUND, c);
        if (!nc) break;
        int c0, c1;
        if (V.npend) {
            c0 = V.pend[0];
            c1 = c[0] == c0 ? c[1] : c[0];
            if (c1 < 0) break;
        } else if (nc == 2) {
            c0 = c[0];
            c1 = c[1];
        } else {
            pend(c[0]);
            break;
        }
        double pts[2][2];
        int ns = geo_intersect(&g->o[c0], &g->o[c1], pts);
        int made = -1;
        for (int k = 0; k < ns; k++) {
            if (isfinite(pts[k][0])) made = add_inter(c0, c1, k);
        }
        if (made < 0) aos_ui_toast(_("No se cruzan"), 1500);
        built(made);
        break;
    }

    case T_PERP:
    case T_PAR: {
        int p = hit_point(sx, sy), line = -1, point = -1;
        if (p >= 0) point = p;
        else if (hit_curves(sx, sy, F_LINEAR, c)) line = c[0];
        else point = take_point(sx, sy);
        if (!V.npend) {
            pend(line >= 0 ? line : point);
            break;
        }
        int other = V.pend[0];
        if (geo_is_linear(g->o[other].kind)) {
            if (point < 0) break;               /* a second line: still the point to come */
            line = other;
        } else {
            if (line < 0) break;
            point = other;
        }
        short a[2] = { (short)point, (short)line };
        built(add_unique(V.tool == T_PERP ? GK_PERP : GK_PAR, a, 2));
        break;
    }

    case T_LEN: {
        int p = hit_point(sx, sy);
        if (!V.npend && p < 0) {
            if (hit_curves(sx, sy, F_SEG, c)) {
                short a[2] = { g->o[c[0]].a[0], g->o[c[0]].a[1] };
                built(add_unique(GK_DIST, a, 2));
            } else if (hit_curves(sx, sy, F_ROUND | F_POLY, c)) {
                short a[1] = { (short)c[0] };
                built(add_unique(GK_PERIM, a, 1));
            }
            break;
        }
        p = take_new_point(sx, sy);
        if (p == -2) break;
        if (!V.npend) {
            pend(p);
            break;
        }
        short a[2] = { V.pend[0], (short)p };
        built(add_unique(GK_DIST, a, 2));
        break;
    }

    case T_AREA: {
        int s = hit_curves(sx, sy, F_ROUND | F_POLY, c) ? c[0] : hit_inside(sx_to_wx(sx), sy_to_wy(sy), true);
        if (s >= 0) {
            short a[1] = { (short)s };
            built(add_unique(GK_AREA, a, 1));
        }
        break;
    }

    case T_EQ:
        if (hit_curves(sx, sy, F_LINEAR | F_ROUND, c)) {
            short a[1] = { (short)c[0] };
            built(add_unique(GK_EQ, a, 1));
        }
        break;

    case T_COORD: {
        int p = hit_point(sx, sy);
        if (p >= 0) {
            short a[1] = { (short)p };
            built(add_unique(GK_COORD, a, 1));
        }
        break;
    }

    case T_DELETE: {
        int p = hit_point(sx, sy);
        if (p < 0 && hit_curves(sx, sy, F_LINEAR | F_ROUND | F_POLY, c)) p = c[0];
        if (p < 0) p = hit_inside(sx_to_wx(sx), sy_to_wy(sy), false);
        if (p >= 0) {
            geo_delete(g, p);
            V.list_dirty = true;
            built(-1);
        }
        break;
    }

    default:
        break;
    }
    /* nothing changed: the snapshot was for nothing */
    if (g->n == n0 && V.nundo) {
        snap_free(&V.undo[--V.nundo]);
    }
    V.dirty = true;
}

/* --- drawing ------------------------------------------------------------------------------ */

/* the part of a line inside the window (a little beyond): Liang-Barsky */
static bool clip(const geo_obj_t *o, double *ta, double *tb)
{
    double m = 40 / V.scale;
    double x0 = sx_to_wx(0) - m, x1 = sx_to_wx((float)V.W) + m;
    double y0 = sy_to_wy((float)V.H) - m, y1 = sy_to_wy(0) + m;
    geo_range(o->kind, ta, tb);
    double p[4] = { -o->dx, o->dx, -o->dy, o->dy };
    double q[4] = { o->px - x0, x1 - o->px, o->py - y0, y1 - o->py };
    for (int k = 0; k < 4; k++) {
        if (fabs(p[k]) < 1e-15) {
            if (q[k] < 0) return false;
            continue;
        }
        double r = q[k] / p[k];
        if (p[k] < 0) {
            if (r > *ta) *ta = r;
        } else if (r < *tb) {
            *tb = r;
        }
    }
    return *ta <= *tb && isfinite(*ta) && isfinite(*tb);
}

static bool picked(int i)
{
    if (i == V.sel) return true;
    for (int k = 0; k < V.npend; k++) {
        if (V.pend[k] == i) return true;
    }
    return false;
}

static void draw_linear(cas_plot_t *p, const geo_obj_t *o, uint32_t col, float w)
{
    double ta, tb;
    if (!clip(o, &ta, &tb)) return;
    cas_plot_line(p, wx_to_sx(o->px + ta * o->dx), wy_to_sy(o->py + ta * o->dy),
                  wx_to_sx(o->px + tb * o->dx), wy_to_sy(o->py + tb * o->dy), col, w);
}

static void draw_round(cas_plot_t *p, const geo_obj_t *o, uint32_t col, float w)
{
    float cx = wx_to_sx(o->px), cy = wy_to_sy(o->py), r = (float)(o->r * V.scale);
    if (cx + r < -10 || cx - r > V.W + 10 || cy + r < -10 || cy - r > V.H + 10) return;
    int n = (int)(r * 0.6f);
    if (n < 32) n = 32;
    if (n > 900) n = 900;
    float px = cx + r, py = cy;
    for (int k = 1; k <= n; k++) {
        float a = 2 * (float)M_PI * k / n;
        float x = cx + r * cosf(a), y = cy - r * sinf(a);
        cas_plot_line(p, px, py, x, y, col, w);
        px = x;
        py = y;
    }
}

/* a polygon filled by scanlines, then its sides */
static void draw_poly(cas_plot_t *p, const geo_obj_t *o, uint32_t col, float w)
{
    float xs[GEO_POLY], ys[GEO_POLY];
    float ymin = 1e9f, ymax = -1e9f;
    for (int k = 0; k < o->n; k++) {
        xs[k] = wx_to_sx(V.g->o[o->a[k]].px);
        ys[k] = wy_to_sy(V.g->o[o->a[k]].py);
        if (ys[k] < ymin) ymin = ys[k];
        if (ys[k] > ymax) ymax = ys[k];
    }
    int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
    if (y0 < 0) y0 = 0;
    if (y1 > V.H - 1) y1 = V.H - 1;
    for (int y = y0; y <= y1; y++) {
        float yc = y + 0.5f, cut[GEO_POLY];
        int nc = 0;
        for (int k = 0, j = o->n - 1; k < o->n; j = k++) {
            if ((ys[k] > yc) != (ys[j] > yc)) {
                cut[nc++] = xs[k] + (yc - ys[k]) * (xs[j] - xs[k]) / (ys[j] - ys[k]);
            }
        }
        for (int a = 1; a < nc; a++) {          /* a handful: insertion sort */
            float v = cut[a];
            int b = a - 1;
            while (b >= 0 && cut[b] > v) {
                cut[b + 1] = cut[b];
                b--;
            }
            cut[b + 1] = v;
        }
        for (int a = 0; a + 1 < nc; a += 2) {
            cas_plot_rect(p, cut[a], (float)y, cut[a + 1], (float)y + 1, C_FILL, 46);
        }
    }
    for (int k = 0; k < o->n; k++) {
        int j = (k + 1) % o->n;
        cas_plot_line(p, xs[k], ys[k], xs[j], ys[j], col, w);
    }
}

/* the arc of an angle at its vertex, a little square for a right one */
static void draw_angle(cas_plot_t *p, const geo_obj_t *o)
{
    const geo_obj_t *A = &V.g->o[o->a[0]], *B = &V.g->o[o->a[1]], *C = &V.g->o[o->a[2]];
    float bx = wx_to_sx(B->px), by = wy_to_sy(B->py);
    float a0 = atan2f(-(wy_to_sy(A->py) - by), wx_to_sx(A->px) - bx);
    float a1 = atan2f(-(wy_to_sy(C->py) - by), wx_to_sx(C->px) - bx);
    float da = a1 - a0;
    while (da > (float)M_PI) da -= 2 * (float)M_PI;
    while (da < -(float)M_PI) da += 2 * (float)M_PI;
    const float R = 34;
    if (fabs(o->val - 90) < 1e-7) {
        float ux = cosf(a0), uy = -sinf(a0), vx = cosf(a1), vy = -sinf(a1), s = R * 0.62f;
        cas_plot_line(p, bx + ux * s, by + uy * s, bx + (ux + vx) * s, by + (uy + vy) * s, C_MEAS, 2.4f);
        cas_plot_line(p, bx + vx * s, by + vy * s, bx + (ux + vx) * s, by + (uy + vy) * s, C_MEAS, 2.4f);
        return;
    }
    int n = 24;
    float px = bx + R * cosf(a0), py = by - R * sinf(a0);
    for (int k = 1; k <= n; k++) {
        float a = a0 + da * k / n;
        float x = bx + R * cosf(a), y = by - R * sinf(a);
        cas_plot_line(p, px, py, x, y, C_MEAS, 2.4f);
        px = x;
        py = y;
    }
}

/* where a measurement writes its value on the figure; false: nowhere */
static bool meas_anchor(const geo_obj_t *o, float *x, float *y)
{
    const geo_t *g = V.g;
    switch (o->kind) {
    case GK_DIST: {
        const geo_obj_t *A = &g->o[o->a[0]], *B = &g->o[o->a[1]];
        float ax = wx_to_sx(A->px), ay = wy_to_sy(A->py), bx = wx_to_sx(B->px), by = wy_to_sy(B->py);
        float l = hypotf(bx - ax, by - ay);
        if (l < 1) return false;
        float nx = -(by - ay) / l, ny = (bx - ax) / l;
        if (ny > 0) {           /* above the segment, where it reads better */
            nx = -nx;
            ny = -ny;
        }
        *x = (ax + bx) / 2 + nx * 22;
        *y = (ay + by) / 2 + ny * 22 - 10;
        return true;
    }
    case GK_ANGLE: {
        const geo_obj_t *A = &g->o[o->a[0]], *B = &g->o[o->a[1]], *C = &g->o[o->a[2]];
        float bx = wx_to_sx(B->px), by = wy_to_sy(B->py);
        float ux = wx_to_sx(A->px) - bx, uy = wy_to_sy(A->py) - by, vx = wx_to_sx(C->px) - bx, vy = wy_to_sy(C->py) - by;
        float lu = hypotf(ux, uy), lv = hypotf(vx, vy);
        if (lu < 1 || lv < 1) return false;
        float dx = ux / lu + vx / lv, dy = uy / lu + vy / lv, l = hypotf(dx, dy);
        if (l < 1e-3f) {
            dx = -uy / lu;
            dy = ux / lu;
            l = 1;
        }
        *x = bx + dx / l * 62;
        *y = by + dy / l * 62 - 10;
        return true;
    }
    case GK_AREA:
    case GK_PERIM: {
        const geo_obj_t *s = &g->o[o->a[0]];
        double cx = 0, cy = 0;
        if (geo_is_round(s->kind)) {
            cx = s->px;
            cy = s->py;
        } else {
            for (int k = 0; k < s->n; k++) {
                cx += g->o[s->a[k]].px;
                cy += g->o[s->a[k]].py;
            }
            cx /= s->n;
            cy /= s->n;
        }
        *x = wx_to_sx(cx);
        *y = wy_to_sy(cy) + (o->kind == GK_PERIM ? 14 : -12);
        return true;
    }
    default:
        return false;
    }
}

static bool fit_canvas(void);

static void redraw(void)
{
    if (!V.canvas || !V.plot.db) return;
    /* the list first: a row more or less changes the canvas' room */
    if (V.list_dirty) {
        list_build();
        if (!fit_canvas()) return;
    }
    V.dirty = false;
    cas_plot_t *p = &V.plot;
    p->x0 = sx_to_wx(0);
    p->x1 = sx_to_wx((float)V.W);
    p->y0 = sy_to_wy((float)V.H);
    p->y1 = sy_to_wy(0);
    cas_plot_clear(p, 0x000000);
    if (V.grid) cas_plot_grid(p);
    geo_t *g = V.g;

    for (int i = 0; i < g->n; i++) {
        const geo_obj_t *o = &g->o[i];
        if (!o->ok || o->hidden) continue;
        uint32_t col = picked(i) ? C_PICK : C_CURVE;
        float w = picked(i) ? 4.5f : 3.0f;
        if (geo_is_linear(o->kind)) draw_linear(p, o, col, w);
        else if (geo_is_round(o->kind)) draw_round(p, o, col, w);
        else if (o->kind == GK_POLY) draw_poly(p, o, col, w);
        else if (o->kind == GK_ANGLE) draw_angle(p, o);
    }
    /* the points on top of the lines */
    for (int i = 0; i < g->n; i++) {
        const geo_obj_t *o = &g->o[i];
        if (!geo_is_point(o->kind) || !o->ok || o->hidden) continue;
        float x = wx_to_sx(o->px), y = wy_to_sy(o->py);
        if (x < -20 || x > V.W + 20 || y < -20 || y > V.H + 20) continue;
        uint32_t c = o->kind == GK_FREE ? C_FREE : o->kind == GK_ON ? C_ON : C_FIXED;
        bool pk = picked(i) || i == V.drag;
        cas_plot_disc(p, x, y, pk ? 11 : 8, pk ? C_PICK : 0xFFFFFF);
        cas_plot_disc(p, x, y, pk ? 7 : 5.5f, c);
    }

    lv_layer_t layer;
    lv_canvas_init_layer(V.canvas, &layer);
    if (V.grid) cas_plot_numbers(p, &layer);
    char b[96];
    for (int i = 0; i < g->n; i++) {
        const geo_obj_t *o = &g->o[i];
        if (!o->ok || o->hidden) continue;
        if (geo_is_point(o->kind) && o->name[0]) {
            float x = wx_to_sx(o->px), y = wy_to_sy(o->py);
            if (x > -20 && x < V.W + 20 && y > -20 && y < V.H + 20) {
                cas_plot_text(&layer, (int)x + 12, (int)y - 34, o->name, 0xFFFFFF, 0);
            }
        } else if (geo_is_measure(o->kind)) {
            float x, y;
            if (!meas_anchor(o, &x, &y)) continue;
            if (o->kind == GK_ANGLE) {
                double v = fabs(o->val - round(o->val)) < 1e-7 ? round(o->val) : o->val;
                geo_fmt(b, sizeof b, v);
                strncat(b, "\xC2\xB0", sizeof b - strlen(b) - 1);
            } else {
                geo_fmt(b, sizeof b, o->val);
            }
            cas_plot_text(&layer, (int)x, (int)y, b, C_MEAS, 1);
        }
    }
    lv_canvas_finish_layer(V.canvas, &layer);
    cas_plot_done(p);

    /* the values of the list follow the figure */
    {
        for (int i = 0; i < g->n; i++) {
            if (V.vals[i] && geo_is_measure(g->o[i].kind)) {
                geo_value(g, i, b, sizeof b);
                if (strcmp(lv_label_get_text(V.vals[i]), b)) lv_label_set_text(V.vals[i], b);
            }
        }
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (V.dirty && V.shown) redraw();
}

/* --- gestures ------------------------------------------------------------------------------- */

static void local(const aos_gesture_event_t *ev, float *x, float *y)
{
    lv_area_t a;
    lv_obj_get_coords(V.canvas, &a);
    *x = ev->x - a.x1;
    *y = ev->y - a.y1;
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    float x, y;
    switch (ev->type) {
    case AOS_GESTURE_DRAG_BEGIN: {
        V.moved = true;
        local(ev, &x, &y);
        int p = hit_point(x, y);
        V.drag = -1;
        V.panning = true;
        if (p >= 0 && (V.g->o[p].kind == GK_FREE || V.g->o[p].kind == GK_ON)) {
            push_undo();
            V.drag = p;
            V.panning = false;
        }
        break;
    }
    case AOS_GESTURE_DRAG:
        if (V.drag >= 0) {
            local(ev, &x, &y);
            double wx = sx_to_wx(x), wy = sy_to_wy(y);
            if (V.g->o[V.drag].kind == GK_FREE) snap_xy(&wx, &wy);
            geo_move(V.g, V.drag, wx, wy);
        } else if (V.panning) {
            V.cx -= ev->dx / V.scale;
            V.cy += ev->dy / V.scale;
        }
        V.dirty = true;
        break;
    case AOS_GESTURE_DRAG_END:
        if (V.drag >= 0) {
            V.drag = -1;
            save();
            if (V.sel >= 0) select_obj(wx_to_sx(V.g->o[V.sel].px), wy_to_sy(V.g->o[V.sel].py));
        }
        V.panning = false;
        V.dirty = true;
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        V.moved = true;
        break;
    case AOS_GESTURE_PINCH: {
        local(ev, &x, &y);
        double mx = sx_to_wx(x), my = sy_to_wy(y);
        float s = V.scale * ev->scale;
        if (s < 2) s = 2;
        if (s > 20000) s = 20000;
        V.scale = s;
        V.cx = mx - (x + ev->dx - V.W / 2.0f) / V.scale;
        V.cy = my - (V.H / 2.0f - (y + ev->dy)) / V.scale;
        V.dirty = true;
        break;
    }
    case AOS_GESTURE_PINCH_END:
        save();
        break;
    default:
        break;
    }
}

/* Taps come through LVGL's click, not the recogniser: the recogniser reads
 * the touch chip, and the portal's taps (and a remote session's) only go
 * through LVGL. A touch that dragged or pinched is not a tap. */
static void press_cb(lv_event_t *e)
{
    (void)e;
    V.moved = false;
}

static void click_cb(lv_event_t *e)
{
    (void)e;
    if (V.moved || !V.canvas) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(V.canvas, &a);
    tap((float)(p.x - a.x1), (float)(p.y - a.y1));
}

/* --- the tool sheet --------------------------------------------------------------------------- */

static void sheet_close(void)
{
    if (V.sheet) {
        lv_obj_delete(V.sheet);
        V.sheet = NULL;
    }
}

static void tool_label(void)
{
    if (!V.toolbtn) return;
    lv_obj_t *l = lv_obj_get_child(V.toolbtn, 0);
    char b[64];
    snprintf(b, sizeof b, "%s  %s", LV_SYMBOL_EDIT, _(TOOL_NAME[V.tool]));
    lv_label_set_text(l, b);
}

static void choose(tool_t t)
{
    V.tool = t;
    V.npend = 0;
    V.sel = -1;
    V.dirty = true;
    tool_label();
    tool_hint();
    save();
}

static void sheet_async_close(void *u)
{
    (void)u;
    sheet_close();
}

static void tool_cb(lv_event_t *e)
{
    choose((tool_t)(intptr_t)lv_event_get_user_data(e));
    lv_async_call(sheet_async_close, NULL);
}

static void opt_cb(lv_event_t *e)
{
    const char *what = lv_event_get_user_data(e);
    if (!strcmp(what, "grid")) {
        V.grid = !V.grid;
    } else if (!strcmp(what, "snap")) {
        V.snap = !V.snap;
    } else if (!strcmp(what, "clear")) {
        if (V.g->n) {
            push_undo();
            V.g->n = 0;
            V.npend = 0;
            V.sel = -1;
            V.list_dirty = true;
        }
    }
    V.dirty = true;
    save();
    lv_async_call(sheet_async_close, NULL);
}

static void scrim_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) lv_async_call(sheet_async_close, NULL);
}

static lv_obj_t *sheet_button(lv_obj_t *parent, const char *text, bool lit)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 58);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_set_style_bg_color(b, lit ? AOS_C_ACCENT : AOS_C_CARD, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    const lv_font_t *f = mx_font(20);
    lv_obj_set_style_text_font(l, f, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, LV_PCT(100), lv_font_get_line_height(f));      /* one line, dots past it */
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

static void sheet_section(lv_obj_t *box, const char *title, const tool_t *tools, int n, int cols)
{
    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(t, aos_font_small, 0);
    lv_obj_t *grid = lv_obj_create(box);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 8, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for (int k = 0; k < n; k++) {
        lv_obj_t *b = sheet_button(grid, _(TOOL_NAME[tools[k]]), tools[k] == V.tool);
        lv_obj_set_width(b, cols == 3 ? LV_PCT(32) : LV_PCT(24));
        lv_obj_add_event_cb(b, tool_cb, LV_EVENT_CLICKED, (void *)(intptr_t)tools[k]);
    }
}

static void tools_open(lv_event_t *e)
{
    (void)e;
    sheet_close();
    V.sheet = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(V.sheet);
    lv_obj_set_size(V.sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(V.sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(V.sheet, LV_OPA_50, 0);
    lv_obj_add_flag(V.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(V.sheet, scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(V.sheet);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, LV_PCT(86), 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(box, AOS_C_CARD2, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 24, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_style_pad_bottom(box, 36, 0);
    lv_obj_set_style_pad_row(box, 10, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);

    static const tool_t PTS[] = { T_MOVE, T_POINT, T_MID, T_INTER };
    static const tool_t LIN[] = { T_SEG, T_LINE, T_RAY, T_PERP, T_PAR, T_PBIS, T_ABIS };
    static const tool_t FIG[] = { T_CIRCLE, T_CIRC3, T_POLY };
    static const tool_t MEA[] = { T_LEN, T_ANGLE, T_AREA, T_EQ, T_COORD, T_DELETE };
    sheet_section(box, _("Puntos"), PTS, 4, 4);
    sheet_section(box, _("Líneas"), LIN, 7, 3);
    sheet_section(box, _("Figuras"), FIG, 3, 3);
    sheet_section(box, _("Medir y editar"), MEA, 6, 3);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    static const char *const OPT[][2] = {
        { "grid", N_("Cuadrícula") }, { "snap", N_("Ajustar a la grilla") }, { "clear", N_("Borrar todo") },
    };
    for (int k = 0; k < 3; k++) {
        bool lit = (k == 0 && V.grid) || (k == 1 && V.snap);
        lv_obj_t *b = sheet_button(row, _(OPT[k][1]), lit);
        lv_obj_set_width(b, LV_PCT(32));
        if (k == 2) lv_obj_set_style_bg_color(b, AOS_C_RED, 0);
        lv_obj_add_event_cb(b, opt_cb, LV_EVENT_CLICKED, (void *)OPT[k][0]);
    }
}

/* --- the list of measurements ----------------------------------------------------------- */

static void meas_del(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= V.g->n) return;
    push_undo();
    geo_delete(V.g, i);
    V.list_dirty = true;
    V.dirty = true;
    save();
}

static void list_build(void)
{
    V.list_dirty = false;
    memset(V.vals, 0, sizeof V.vals);
    if (!V.list) return;
    lv_obj_clean(V.list);
    int count = 0;
    char l[96], v[128];
    for (int i = 0; i < V.g->n; i++) {
        if (!geo_is_measure(V.g->o[i].kind)) continue;
        count++;
        lv_obj_t *row = lv_obj_create(V.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 54, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        geo_label(V.g, i, l, sizeof l);
        lv_obj_t *lab = lv_label_create(row);
        lv_label_set_text(lab, l);
        lv_obj_set_style_text_color(lab, AOS_C_DIM, 0);
        lv_obj_set_style_text_font(lab, aos_font_small, 0);
        geo_value(V.g, i, v, sizeof v);
        V.vals[i] = lv_label_create(row);
        lv_label_set_text(V.vals[i], v);
        lv_obj_set_flex_grow(V.vals[i], 1);
        lv_obj_set_style_text_align(V.vals[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(V.vals[i], aos_font_small, 0);
        lv_obj_t *x = lv_label_create(row);
        lv_label_set_text(x, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(x, AOS_C_DIM, 0);
        lv_obj_set_ext_click_area(x, 16);
        lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(x, meas_del, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (!count) {
        lv_obj_t *e = lv_label_create(V.list);
        lv_label_set_text(e, _("Las medidas aparecen acá: elegí Longitud, Ángulo, Área o Ecuación."));
        lv_label_set_long_mode(e, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(e, LV_PCT(100));
        lv_obj_set_style_text_color(e, AOS_C_DIM, 0);
        lv_obj_set_style_text_font(e, aos_font_small, 0);
    }
}

/* --- the view ----------------------------------------------------------------------------- */

static lv_obj_t *round_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 64, 64);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static void undo_cb(lv_event_t *e)
{
    (void)e;
    pop_undo();
    tool_hint();
}

static void home_cb(lv_event_t *e)
{
    (void)e;
    V.cx = V.cy = 0;
    V.scale = 60;
    V.dirty = true;
    save();
}

static void panel_cb(lv_event_t *e)
{
    (void)e;
    V.panel_hidden = !V.panel_hidden;
    build();
}

/* The canvas as big as its box. The list growing or going away resizes
 * the box: the figure stays where it was, its top-left corner fixed. */
static bool fit_canvas(void)
{
    lv_obj_update_layout(lv_obj_get_parent(V.box));
    int w = lv_obj_get_width(V.box), h = lv_obj_get_height(V.box);
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (V.plot.db && w == V.W && h == V.H) return true;
    if (V.W && V.H && (w != V.W || h != V.H)) {
        V.cx += (w - V.W) / 2.0f / V.scale;
        V.cy -= (h - V.H) / 2.0f / V.scale;
    }
    V.W = w;
    V.H = h;
    if (V.canvas) {
        lv_obj_delete(V.canvas);        /* the recogniser goes with it */
        V.canvas = NULL;
    }
    cas_plot_free(&V.plot);
    if (!cas_plot_create(&V.plot, V.box, V.W, V.H)) return false;
    V.canvas = V.plot.canvas;
    lv_obj_move_to_index(V.canvas, 0);  /* under the hint and the buttons */
    aos_gesture_attach(V.canvas, AOS_GESTURE_FLAG_FAST_TAP, gesture_cb, NULL);
    lv_obj_add_event_cb(V.canvas, press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(V.canvas, click_cb, LV_EVENT_CLICKED, NULL);
    if (V.hint) lv_obj_set_style_max_width(V.hint, V.W - 24, 0);
    V.dirty = true;
    return true;
}

static void build(void)
{
    lv_obj_t *parent = V.parent;
    if (!parent) return;
    sheet_close();
    lv_obj_clean(parent);
    V.canvas = V.box = V.panel = V.list = V.hint = V.toolbtn = NULL;
    memset(V.vals, 0, sizeof V.vals);
    cas_plot_free(&V.plot);

    bool land = V.landscape;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(root, 10, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    V.box = lv_obj_create(root);
    lv_obj_remove_style_all(V.box);
    lv_obj_set_flex_grow(V.box, 1);
    if (land) lv_obj_set_height(V.box, LV_PCT(100));
    else lv_obj_set_width(V.box, LV_PCT(100));
    lv_obj_set_style_radius(V.box, 16, 0);
    lv_obj_set_style_clip_corner(V.box, true, 0);
    lv_obj_remove_flag(V.box, LV_OBJ_FLAG_SCROLLABLE);

    V.panel = lv_obj_create(root);
    lv_obj_remove_style_all(V.panel);
    lv_obj_set_flex_flow(V.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(V.panel, LV_OBJ_FLAG_SCROLLABLE);
    if (land) {
        lv_obj_set_size(V.panel, LV_PCT(40), LV_PCT(100));
    } else {
        lv_obj_set_size(V.panel, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(V.panel, 280, 0);
    }
    V.list = lv_obj_create(V.panel);
    lv_obj_remove_style_all(V.list);
    lv_obj_set_size(V.list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(V.list, land ? LV_PCT(100) : 280, 0);
    if (land) lv_obj_set_flex_grow(V.list, 1);
    lv_obj_set_flex_flow(V.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(V.list, 8, 0);
    lv_obj_set_scroll_dir(V.list, LV_DIR_VER);
    list_build();
    if (V.panel_hidden) lv_obj_add_flag(V.panel, LV_OBJ_FLAG_HIDDEN);

    if (!fit_canvas()) {
        lv_obj_t *l = lv_label_create(V.box);
        lv_label_set_text(l, _("Sin memoria para la figura"));
        lv_obj_center(l);
        return;
    }

    V.hint = lv_label_create(V.box);
    lv_obj_set_style_bg_color(V.hint, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(V.hint, LV_OPA_90, 0);
    lv_obj_set_style_radius(V.hint, 18, 0);
    lv_obj_set_style_pad_hor(V.hint, 16, 0);
    lv_obj_set_style_pad_ver(V.hint, 8, 0);
    lv_obj_set_style_text_font(V.hint, aos_font_small, 0);
    lv_obj_set_style_max_width(V.hint, V.W - 24, 0);
    lv_label_set_long_mode(V.hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(V.hint, LV_ALIGN_TOP_MID, 0, 10);
    tool_hint();

    V.toolbtn = lv_button_create(V.box);
    lv_obj_set_height(V.toolbtn, 64);
    lv_obj_set_style_radius(V.toolbtn, 32, 0);
    lv_obj_set_style_pad_hor(V.toolbtn, 22, 0);
    lv_obj_set_style_bg_color(V.toolbtn, AOS_C_ACCENT, 0);
    lv_obj_set_style_shadow_width(V.toolbtn, 0, 0);
    lv_obj_add_event_cb(V.toolbtn, tools_open, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl = lv_label_create(V.toolbtn);
    lv_obj_set_style_text_font(tl, aos_font_small, 0);
    lv_obj_center(tl);
    lv_obj_align(V.toolbtn, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    tool_label();

    lv_obj_t *pb = round_button(V.box, LV_SYMBOL_LIST, panel_cb);
    lv_obj_align(pb, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_t *hb = round_button(V.box, LV_SYMBOL_HOME, home_cb);
    lv_obj_align(hb, LV_ALIGN_BOTTOM_RIGHT, -88, -12);
    lv_obj_t *ub = round_button(V.box, LV_SYMBOL_LEFT, undo_cb);
    lv_obj_align(ub, LV_ALIGN_BOTTOM_RIGHT, -164, -12);

    redraw();
}

void cas_geo_build(lv_obj_t *parent, bool landscape)
{
    if (!model_init()) {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, _("Sin memoria para la figura"));
        lv_obj_center(l);
        return;
    }
    V.parent = parent;
    V.landscape = landscape;
    V.npend = 0;
    V.drag = -1;
    if (!V.timer) V.timer = lv_timer_create(timer_cb, 30, NULL);
    build();
}

void cas_geo_shown(bool shown)
{
    if (V.shown && !shown) {
        sheet_close();
        save();
        V.parent = NULL;
        V.canvas = V.box = V.panel = V.list = V.hint = V.toolbtn = NULL;
        memset(V.vals, 0, sizeof V.vals);
        cas_plot_free(&V.plot);
    }
    V.shown = shown;
    cas_app_set_gestures(shown);
}

void cas_geo_free(void)
{
    sheet_close();
    lv_async_call_cancel(sheet_async_close, NULL);
    if (V.init) save();
    if (V.timer) {
        lv_timer_delete(V.timer);
        V.timer = NULL;
    }
    cas_plot_free(&V.plot);
    for (int k = 0; k < V.nundo; k++) snap_free(&V.undo[k]);
    V.nundo = 0;
    mx_free(V.g);
    V.g = NULL;
    V.init = false;
    V.parent = NULL;
    V.shown = false;
}
