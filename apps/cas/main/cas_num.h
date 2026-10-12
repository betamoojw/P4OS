/*
 * P4OS - CAS: the numbers behind statistics and finance (cas_num.c).
 *
 * Plain C over double, no LVGL and no engine: these answer as a value is
 * typed, which a round trip through Giac would not, and they run the same
 * in the simulator. Giac stays for what is symbolic.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* --- one variable ------------------------------------------------------------ */

typedef struct {
    int    n;
    double sum, sum2, mean;
    double sx, px;              /* sample and population standard deviations */
    double min, q1, med, q3, max;
} num_stats_t;

bool num_stats(const double *v, int n, num_stats_t *s);

/* --- regressions (least squares; exp, log and power by linearising, as the
 * calculators do) -------------------------------------------------------------- */

typedef enum { REG_LIN, REG_QUAD, REG_CUBIC, REG_EXP, REG_LOG, REG_POW, REG_COUNT } num_reg_kind_t;

typedef struct {
    num_reg_kind_t kind;
    double c[4];                /* lin: a,b (ax+b); quad/cubic: highest first;
                                 * exp: a,b (a*b^x); log: a,b (a+b*ln x);
                                 * pow: a,b (a*x^b) */
    double r2, r;               /* r only for the linear ones */
    bool   ok;
} num_reg_t;

bool   num_regress(num_reg_kind_t k, const double *x, const double *y, int n, num_reg_t *out);
double num_reg_eval(const num_reg_t *r, double x);
/* The model as text Giac and mx_parse read: digits = significant digits. */
void   num_reg_text(const num_reg_t *r, int digits, char *out, size_t n);

/* --- distributions -------------------------------------------------------------- */

typedef enum { D_NORMAL, D_T, D_CHI2, D_F, D_BINOM, D_POISSON, D_EXPO, D_COUNT } num_dist_t;

bool   num_dist_discrete(num_dist_t d);
int    num_dist_npar(num_dist_t d);
double num_pdf(num_dist_t d, const double *par, double x);
double num_cdf(num_dist_t d, const double *par, double x);       /* P(X <= x) */
double num_inv(num_dist_t d, const double *par, double p);       /* the x with P(X <= x) = p */
/* P(a <= X <= b); either end may be ±INFINITY */
double num_prob(num_dist_t d, const double *par, double a, double b);
/* where most of the density is, for drawing it */
void   num_dist_range(num_dist_t d, const double *par, double *lo, double *hi);

/* --- intervals and tests ------------------------------------------------------- */

typedef enum { TAIL_TWO, TAIL_LEFT, TAIL_RIGHT } num_tail_t;

typedef struct {
    double stat;                /* t or z                    */
    double df;                  /* degrees of freedom; 0 = z */
    double p;                   /* the p-value               */
    double lo, hi;              /* the interval              */
    double se;                  /* standard error            */
} num_test_t;

void num_t_mean(double mean, double s, int n, double mu0, double conf, num_tail_t tail, num_test_t *out);
void num_z_prop(int x, int n, double p0, double conf, num_tail_t tail, num_test_t *out);
void num_t_two(double m1, double s1, int n1, double m2, double s2, int n2, double conf,
               num_tail_t tail, num_test_t *out);  /* Welch */

/* --- money ---------------------------------------------------------------------- */

/* Time value of money, as the financial calculators: money in is positive,
 * money out negative; iy is the nominal annual rate in percent, py the
 * payments and cy the compounding periods a year, begin = payments at the
 * start of each period. */
typedef struct {
    double n, iy, pv, pmt, fv, py, cy;
    bool   begin;
} num_tvm_t;

enum { TVM_N, TVM_IY, TVM_PV, TVM_PMT, TVM_FV };
bool num_tvm_solve(num_tvm_t *t, int which);

typedef enum { LOAN_FRENCH, LOAN_GERMAN, LOAN_AMERICAN } num_loan_kind_t;

typedef struct {
    double payment, interest, principal, balance;
} num_loan_row_t;

/* Row k (1..n) of a loan of 'amount' at 'rate' per period, n periods. */
void   num_loan_row(num_loan_kind_t kind, double amount, double rate, int n, int k, num_loan_row_t *out);
double num_loan_total_interest(num_loan_kind_t kind, double amount, double rate, int n);

double num_npv(double rate, const double *cf, int n);           /* cf[0] at time 0 */
bool   num_irr(const double *cf, int n, double *irr);
double num_payback(const double *cf, int n);                     /* periods, -1 never */
