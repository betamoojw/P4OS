/*
 * P4OS - CAS: physics, the model (cas_phys.c).
 *
 * A library of formulas that solve for whichever of their variables is
 * missing, and the physical constants (CODATA 2018). A formula is an
 * equation in the evaluator's syntax ("v = v0 + a*t"); to solve it, the
 * known values replace their names, the unknown becomes x, and the roots
 * of lhs - rhs are found numerically (a scan for sign changes over twenty
 * orders of magnitude each way, then bisection). Uses mx_parse/mx_compile;
 * no LVGL of its own, tested on the Mac.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifndef N_
#define N_(s) (s)
#endif

#define PHYS_MAXV 6

typedef struct {
    const char *id;             /* the name in the equation: v0, th, rho  */
    const char *shown;          /* as it prints: v₀, θ, ρ                 */
    const char *name;           /* Spanish, translated                    */
    const char *unit;
    double      preset;         /* a constant's value (g, G, k...), NAN none */
} phys_var_t;

typedef struct {
    const char *group;
    const char *name;
    const char *eq;             /* evaluator syntax; angles in degrees      */
    const char *shown;          /* as it prints                             */
    int         nv;
    phys_var_t  v[PHYS_MAXV];
} phys_formula_t;

extern const phys_formula_t PHYS_F[];
extern const int PHYS_NF;

typedef struct {
    const char *sym;
    const char *name;
    double      value;
    const char *unit;
    bool        exact;          /* exact by definition since 2019 */
} phys_const_t;

extern const phys_const_t PHYS_C[];
extern const int PHYS_NC;

/* Solves formula f for variable 'unknown' given val[] (the others).
 * Returns how many roots it found (0..2): root is the one to show (the
 * smallest positive, or the smallest in size), other the next one. */
int phys_solve(const phys_formula_t *f, const double *val, int unknown, double *root, double *other);

/* The roots of g(x) = 0 found by the same scan, for other users (up to max). */
typedef double (*phys_fn_t)(void *user, double x);
int phys_roots(phys_fn_t fn, void *user, double *out, int max);
