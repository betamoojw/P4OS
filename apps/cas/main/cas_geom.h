/*
 * P4OS - CAS: dynamic geometry, the model (cas_geom.c).
 *
 * A figure is a list of objects, each built from objects before it: free
 * points, points on a curve, midpoints and intersections; segments, lines,
 * rays, circles, polygons, perpendiculars, parallels and bisectors; and
 * measurements of them. Moving a free point recomputes the list in order,
 * so everything built on it follows. An object that cannot exist for the
 * moment (two circles that no longer meet) is undefined, and so is all
 * that hangs from it, until the figure lets it exist again.
 *
 * No LVGL here: the maths and the list, tested on the Mac.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define GEO_MAX     160         /* objects in a figure                 */
#define GEO_POLY    12          /* vertices of a polygon               */

typedef enum {
    /* points */
    GK_FREE = 1,                /* x, y                                */
    GK_ON,                      /* on a[0] at parameter t              */
    GK_MID,                     /* of a[0], a[1]                       */
    GK_INTER,                   /* of a[0] and a[1]; which = 0 or 1    */
    /* lines and the like: through p, direction d */
    GK_SEG,                     /* a[0] a[1]                           */
    GK_LINE,                    /* a[0] a[1]                           */
    GK_RAY,                     /* from a[0] through a[1]              */
    GK_PERP,                    /* through point a[0], perpendicular to a[1] */
    GK_PAR,                     /* through point a[0], parallel to a[1]      */
    GK_PBIS,                    /* perpendicular bisector of a[0] a[1]       */
    GK_ABIS,                    /* bisector of the angle a[0] a[1] a[2]      */
    /* round */
    GK_CIRCLE,                  /* centre a[0], through a[1]           */
    GK_CIRC3,                   /* through a[0] a[1] a[2]              */
    /* closed */
    GK_POLY,                    /* a[0..n-1]                           */
    /* measurements */
    GK_DIST,                    /* between points a[0] a[1]            */
    GK_PERIM,                   /* of a polygon or circle a[0]         */
    GK_ANGLE,                   /* a[0] a[1] a[2], vertex a[1]         */
    GK_AREA,                    /* of a polygon or circle a[0]         */
    GK_EQ,                      /* equation of a line or circle a[0]   */
    GK_COORD,                   /* coordinates of point a[0]           */
} geo_kind_t;

typedef struct {
    unsigned char kind;         /* geo_kind_t, 0 = deleted slot */
    unsigned char n;            /* how many a[] are used        */
    unsigned char which;        /* GK_INTER                     */
    bool          hidden;
    short         a[GEO_POLY];
    double        x, y;         /* GK_FREE                      */
    double        t;            /* GK_ON                        */
    char          name[8];      /* points: A, B, ... ; others empty */
    /* computed */
    bool          ok;
    double        px, py;       /* a point; a line's base; a circle's centre */
    double        dx, dy;       /* a line's direction (from a[0] to a[1] for segments and rays) */
    double        r;            /* a circle's radius                         */
    double        val;          /* a measurement                             */
} geo_obj_t;

typedef struct {
    geo_obj_t o[GEO_MAX];
    int       n;                /* slots used (some may be deleted) */
} geo_t;

/* families */
bool geo_is_point(int kind);
bool geo_is_linear(int kind);   /* has px,py + dx,dy */
bool geo_is_round(int kind);    /* has px,py + r     */
bool geo_is_measure(int kind);
/* the range of a linear object's parameter: lines -inf..inf, rays 0..inf,
 * segments 0..1 */
void geo_range(int kind, double *t0, double *t1);

void geo_clear(geo_t *g);
/* Adds an object (its inputs in a[]) and computes it; returns its index or
 * -1 when the figure is full. Points get the next free name. */
int  geo_add(geo_t *g, int kind, const short *a, int n);
int  geo_add_free(geo_t *g, double x, double y);
int  geo_add_on(geo_t *g, int curve, double x, double y);  /* the point of the curve nearest (x,y) */
/* Deletes i and everything built on it; returns how many went. */
int  geo_delete(geo_t *g, int i);
void geo_update(geo_t *g);      /* recompute all, in order */
/* Moves a free point or slides a point along its curve, towards (x,y). */
bool geo_move(geo_t *g, int i, double x, double y);
bool geo_depends(const geo_t *g, int i, int on);   /* i is built (directly or not) on 'on' */

/* Intersections of two curves: how many (0..2) and where. */
int  geo_intersect(const geo_obj_t *a, const geo_obj_t *b, double out[2][2]);
/* The parameter of the point of a curve nearest (x,y) and that point. */
double geo_project(const geo_obj_t *c, double x, double y, double *qx, double *qy);
/* Distance from (x,y) to a curve (linear within its range, round, a
 * polygon's sides); +inf when undefined. */
double geo_dist_to(const geo_t *g, const geo_obj_t *c, double x, double y);

/* Text of a measurement: "AB", "Ángulo ABC", the equation... and its value,
 * exact when the numbers allow (3√2, 5/2) with the decimal after. */
void geo_label(const geo_t *g, int i, char *b, size_t n);
void geo_value(const geo_t *g, int i, char *b, size_t n);

/* Numbers for people (shared with the labels on the canvas). */
void geo_fmt(char *b, size_t n, double v);
/* v as p/q with q <= maxq when it is one (to 1e-9); false otherwise */
bool geo_rational(double v, int maxq, long *p, long *q);
/* sqrt(v) as (k/q)·√m when v is a small rational: "3√2", "√5/2"; false otherwise */
bool geo_surd(double v, char *b, size_t n);

/* Saving: one object per line. */
void geo_write_line(const geo_t *g, int i, char *b, size_t n);
bool geo_read_line(geo_t *g, const char *line);
