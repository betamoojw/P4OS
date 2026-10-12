/*
 * P4OS - CAS: the numbers behind statistics and finance.
 *
 * The special functions are the classic ones: the regularised incomplete
 * beta by Lentz's continued fraction and the incomplete gamma by its series
 * and continued fraction, over lgamma() and erfc() from the maths library
 * (openlibm inside the .so on the board). Every distribution's cumulative
 * function is one of those; the inverses are a bisection over it, which is
 * slow by a computer's standards and instant by a person's.
 */
#include "cas_num.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- one variable --------------------------------------------------------------- */

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median_of(const double *v, int n)
{
    if (n <= 0) return NAN;
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

bool num_stats(const double *v, int n, num_stats_t *s)
{
    memset(s, 0, sizeof *s);
    if (n <= 0) {
        return false;
    }
    double *w = malloc(sizeof(double) * (size_t)n);
    if (!w) {
        return false;
    }
    memcpy(w, v, sizeof(double) * (size_t)n);
    qsort(w, (size_t)n, sizeof(double), cmp_d);
    s->n = n;
    for (int i = 0; i < n; i++) {
        s->sum += w[i];
        s->sum2 += w[i] * w[i];
    }
    s->mean = s->sum / n;
    double ss = 0;
    for (int i = 0; i < n; i++) {
        ss += (w[i] - s->mean) * (w[i] - s->mean);
    }
    s->px = sqrt(ss / n);
    s->sx = n > 1 ? sqrt(ss / (n - 1)) : NAN;
    s->min = w[0];
    s->max = w[n - 1];
    s->med = median_of(w, n);
    /* quartiles as the calculators: medians of the lower and upper halves,
     * the middle value left out when n is odd */
    s->q1 = n > 1 ? median_of(w, n / 2) : w[0];
    s->q3 = n > 1 ? median_of(w + (n + 1) / 2, n / 2) : w[0];
    free(w);
    return true;
}

/* --- regressions ------------------------------------------------------------------ */

/* Solves the m x m system a·c = b in place (Gauss with partial pivoting). */
static bool solve(double a[4][4], double *b, int m, double *c)
{
    for (int i = 0; i < m; i++) {
        int p = i;
        for (int r = i + 1; r < m; r++) {
            if (fabs(a[r][i]) > fabs(a[p][i])) p = r;
        }
        if (fabs(a[p][i]) < 1e-300) return false;
        if (p != i) {
            for (int k = 0; k < m; k++) {
                double t = a[i][k]; a[i][k] = a[p][k]; a[p][k] = t;
            }
            double t = b[i]; b[i] = b[p]; b[p] = t;
        }
        for (int r = i + 1; r < m; r++) {
            double f = a[r][i] / a[i][i];
            for (int k = i; k < m; k++) a[r][k] -= f * a[i][k];
            b[r] -= f * b[i];
        }
    }
    for (int i = m - 1; i >= 0; i--) {
        double s = b[i];
        for (int k = i + 1; k < m; k++) s -= a[i][k] * c[k];
        c[i] = s / a[i][i];
    }
    return true;
}

/* least squares polynomial of 'deg' over (x, y); c highest power first */
static bool polyfit(const double *x, const double *y, int n, int deg, double *c, double *r2)
{
    int m = deg + 1;
    if (n < m) return false;
    double a[4][4] = { { 0 } }, b[4] = { 0 }, s[7] = { 0 }, sol[4];
    for (int i = 0; i < n; i++) {
        double p = 1;
        for (int k = 0; k <= 2 * deg; k++) {
            s[k] += p;
            if (k <= deg) b[k] += p * y[i];
            p *= x[i];
        }
    }
    for (int r = 0; r < m; r++) {
        for (int k = 0; k < m; k++) a[r][k] = s[r + k];
    }
    if (!solve(a, b, m, sol)) return false;
    for (int k = 0; k < m; k++) c[k] = sol[deg - k];
    double my = 0, ssr = 0, sst = 0;
    for (int i = 0; i < n; i++) my += y[i];
    my /= n;
    for (int i = 0; i < n; i++) {
        double f = 0;
        for (int k = 0; k < m; k++) f = f * x[i] + c[k];
        ssr += (y[i] - f) * (y[i] - f);
        sst += (y[i] - my) * (y[i] - my);
    }
    *r2 = sst > 0 ? 1 - ssr / sst : 1;
    return true;
}

/* a straight line through (u, v): v = a*u + b, with r */
static bool linfit(const double *u, const double *v, int n, double *a, double *b, double *r)
{
    if (n < 2) return false;
    double su = 0, sv = 0, suu = 0, svv = 0, suv = 0;
    for (int i = 0; i < n; i++) {
        su += u[i]; sv += v[i]; suu += u[i] * u[i]; svv += v[i] * v[i]; suv += u[i] * v[i];
    }
    double du = n * suu - su * su, dv = n * svv - sv * sv;
    if (fabs(du) < 1e-300) return false;
    *a = (n * suv - su * sv) / du;
    *b = (sv - *a * su) / n;
    *r = dv > 0 ? (n * suv - su * sv) / sqrt(du * dv) : 1;
    return true;
}

bool num_regress(num_reg_kind_t k, const double *x, const double *y, int n, num_reg_t *out)
{
    memset(out, 0, sizeof *out);
    out->kind = k;
    out->r = NAN;
    if (n < 2) return false;
    double *u = malloc(sizeof(double) * (size_t)n), *v = malloc(sizeof(double) * (size_t)n);
    bool ok = false;
    if (!u || !v) goto done;
    switch (k) {
    case REG_LIN:
        ok = linfit(x, y, n, &out->c[0], &out->c[1], &out->r);
        out->r2 = out->r * out->r;
        break;
    case REG_QUAD:
        ok = polyfit(x, y, n, 2, out->c, &out->r2);
        break;
    case REG_CUBIC:
        ok = polyfit(x, y, n, 3, out->c, &out->r2);
        break;
    case REG_EXP:       /* ln y = ln a + x ln b */
    case REG_LOG:       /* y = a + b ln x */
    case REG_POW: {     /* ln y = ln a + b ln x */
        for (int i = 0; i < n; i++) {
            if ((k != REG_LOG && y[i] <= 0) || (k != REG_EXP && x[i] <= 0)) goto done;
            u[i] = k == REG_EXP ? x[i] : log(x[i]);
            v[i] = k == REG_LOG ? y[i] : log(y[i]);
        }
        double a, b, r;
        ok = linfit(u, v, n, &a, &b, &r);
        out->r = r;
        out->r2 = r * r;
        if (k == REG_EXP) { out->c[0] = exp(b); out->c[1] = exp(a); }
        if (k == REG_LOG) { out->c[0] = b; out->c[1] = a; }
        if (k == REG_POW) { out->c[0] = exp(b); out->c[1] = a; }
        break;
    }
    default:
        break;
    }
done:
    free(u);
    free(v);
    out->ok = ok;
    return ok;
}

double num_reg_eval(const num_reg_t *r, double x)
{
    const double *c = r->c;
    switch (r->kind) {
    case REG_LIN:   return c[0] * x + c[1];
    case REG_QUAD:  return (c[0] * x + c[1]) * x + c[2];
    case REG_CUBIC: return ((c[0] * x + c[1]) * x + c[2]) * x + c[3];
    case REG_EXP:   return c[0] * pow(c[1], x);
    case REG_LOG:   return x > 0 ? c[0] + c[1] * log(x) : NAN;
    case REG_POW:   return x > 0 ? c[0] * pow(x, c[1]) : NAN;
    default:        return NAN;
    }
}

/* "+ 2.5" or "- 2.5" after a term, a plain number first */
static void term(char *out, size_t n, size_t *len, double v, int digits, const char *tail, bool first)
{
    char num[40];
    double a = first ? v : fabs(v);
    snprintf(num, sizeof num, "%.*g", digits, a);
    *len += (size_t)snprintf(out + *len, *len < n ? n - *len : 0, "%s%s%s",
                             first ? "" : (v < 0 ? "-" : "+"), num, tail);
}

void num_reg_text(const num_reg_t *r, int digits, char *out, size_t n)
{
    size_t len = 0;
    const double *c = r->c;
    out[0] = 0;
    switch (r->kind) {
    case REG_LIN:
        term(out, n, &len, c[0], digits, "*x", true);
        term(out, n, &len, c[1], digits, "", false);
        break;
    case REG_QUAD:
        term(out, n, &len, c[0], digits, "*x^2", true);
        term(out, n, &len, c[1], digits, "*x", false);
        term(out, n, &len, c[2], digits, "", false);
        break;
    case REG_CUBIC:
        term(out, n, &len, c[0], digits, "*x^3", true);
        term(out, n, &len, c[1], digits, "*x^2", false);
        term(out, n, &len, c[2], digits, "*x", false);
        term(out, n, &len, c[3], digits, "", false);
        break;
    case REG_EXP:
        snprintf(out, n, "%.*g*%.*g^x", digits, c[0], digits, c[1]);
        break;
    case REG_LOG:
        term(out, n, &len, c[0], digits, "", true);
        term(out, n, &len, c[1], digits, "*ln(x)", false);
        break;
    case REG_POW:
        snprintf(out, n, "%.*g*x^(%.*g)", digits, c[0], digits, c[1]);
        break;
    default:
        break;
    }
}

/* --- special functions -------------------------------------------------------------- */

#define FPMIN 1e-300

static double betacf(double a, double b, double x)
{
    double qab = a + b, qap = a + 1, qam = a - 1, c = 1, d = 1 - qab * x / qap;
    if (fabs(d) < FPMIN) d = FPMIN;
    d = 1 / d;
    double h = d;
    for (int m = 1; m <= 400; m++) {
        int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1 + aa * d;
        if (fabs(d) < FPMIN) d = FPMIN;
        c = 1 + aa / c;
        if (fabs(c) < FPMIN) c = FPMIN;
        d = 1 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1 + aa * d;
        if (fabs(d) < FPMIN) d = FPMIN;
        c = 1 + aa / c;
        if (fabs(c) < FPMIN) c = FPMIN;
        d = 1 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1) < 1e-15) break;
    }
    return h;
}

/* the regularised incomplete beta I_x(a, b) */
static double betai(double a, double b, double x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double bt = exp(lgamma(a + b) - lgamma(a) - lgamma(b) + a * log(x) + b * log(1 - x));
    if (x < (a + 1) / (a + b + 2)) return bt * betacf(a, b, x) / a;
    return 1 - bt * betacf(b, a, 1 - x) / b;
}

/* the regularised lower incomplete gamma P(a, x) */
static double gammp(double a, double x)
{
    if (x <= 0) return 0;
    if (x < a + 1) {
        double ap = a, del = 1 / a, sum = del;
        for (int n = 0; n < 2000; n++) {
            ap += 1;
            del *= x / ap;
            sum += del;
            if (fabs(del) < fabs(sum) * 1e-15) break;
        }
        return sum * exp(-x + a * log(x) - lgamma(a));
    }
    double b = x + 1 - a, c = 1 / FPMIN, d = 1 / b, h = d;
    for (int i = 1; i < 2000; i++) {
        double an = -i * (i - a);
        b += 2;
        d = an * d + b;
        if (fabs(d) < FPMIN) d = FPMIN;
        c = b + an / c;
        if (fabs(c) < FPMIN) c = FPMIN;
        d = 1 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1) < 1e-15) break;
    }
    return 1 - exp(-x + a * log(x) - lgamma(a)) * h;
}

/* --- distributions ------------------------------------------------------------------- */

bool num_dist_discrete(num_dist_t d)
{
    return d == D_BINOM || d == D_POISSON;
}

int num_dist_npar(num_dist_t d)
{
    switch (d) {
    case D_NORMAL: case D_F: case D_BINOM: return 2;
    default: return 1;
    }
}

double num_pdf(num_dist_t d, const double *p, double x)
{
    switch (d) {
    case D_NORMAL: {
        double s = p[1], z = (x - p[0]) / s;
        return s > 0 ? exp(-z * z / 2) / (s * sqrt(2 * M_PI)) : NAN;
    }
    case D_T: {
        double v = p[0];
        return exp(lgamma((v + 1) / 2) - lgamma(v / 2)) / sqrt(v * M_PI) * pow(1 + x * x / v, -(v + 1) / 2);
    }
    case D_CHI2: {
        double k = p[0];
        if (x <= 0) return 0;
        return exp((k / 2 - 1) * log(x) - x / 2 - (k / 2) * log(2.0) - lgamma(k / 2));
    }
    case D_F: {
        double a = p[0], b = p[1];
        if (x <= 0) return 0;
        return exp(lgamma((a + b) / 2) - lgamma(a / 2) - lgamma(b / 2) + (a / 2) * log(a / b) +
                   (a / 2 - 1) * log(x) - ((a + b) / 2) * log(1 + a * x / b));
    }
    case D_BINOM: {
        double n = floor(p[0]), q = p[1], k = x;
        if (k != floor(k) || k < 0 || k > n) return 0;
        if (q <= 0) return k == 0;
        if (q >= 1) return k == n;
        return exp(lgamma(n + 1) - lgamma(k + 1) - lgamma(n - k + 1) + k * log(q) + (n - k) * log(1 - q));
    }
    case D_POISSON: {
        double l = p[0], k = x;
        if (k != floor(k) || k < 0) return 0;
        return exp(k * log(l) - l - lgamma(k + 1));
    }
    case D_EXPO:
        return x < 0 ? 0 : p[0] * exp(-p[0] * x);
    default:
        return NAN;
    }
}

double num_cdf(num_dist_t d, const double *p, double x)
{
    if (isinf(x)) return x > 0 ? 1 : 0;
    switch (d) {
    case D_NORMAL:
        return 0.5 * erfc(-(x - p[0]) / (p[1] * sqrt(2.0)));
    case D_T: {
        double v = p[0], tail = 0.5 * betai(v / 2, 0.5, v / (v + x * x));
        return x > 0 ? 1 - tail : tail;
    }
    case D_CHI2:
        return gammp(p[0] / 2, x / 2);
    case D_F:
        return x <= 0 ? 0 : betai(p[0] / 2, p[1] / 2, p[0] * x / (p[0] * x + p[1]));
    case D_BINOM: {
        double n = floor(p[0]), k = floor(x);
        if (k < 0) return 0;
        if (k >= n) return 1;
        return betai(n - k, k + 1, 1 - p[1]);
    }
    case D_POISSON: {
        double k = floor(x);
        return k < 0 ? 0 : 1 - gammp(k + 1, p[0]);
    }
    case D_EXPO:
        return x <= 0 ? 0 : 1 - exp(-p[0] * x);
    default:
        return NAN;
    }
}

double num_prob(num_dist_t d, const double *p, double a, double b)
{
    if (b < a) return 0;
    if (num_dist_discrete(d)) {
        double lo = isinf(a) ? -1 : ceil(a) - 1;
        return num_cdf(d, p, b) - (isinf(a) ? 0 : num_cdf(d, p, lo));
    }
    return num_cdf(d, p, b) - num_cdf(d, p, a);
}

void num_dist_range(num_dist_t d, const double *p, double *lo, double *hi)
{
    switch (d) {
    case D_NORMAL:  *lo = p[0] - 4 * p[1]; *hi = p[0] + 4 * p[1]; break;
    case D_T:       *hi = 4 + 8 / p[0]; *lo = -*hi; break;
    case D_CHI2:    *lo = 0; *hi = p[0] + 6 * sqrt(2 * p[0]) + 4; break;
    case D_F:       *lo = 0; *hi = 6 + 20 / p[1]; break;
    case D_BINOM:   *lo = 0; *hi = floor(p[0]); break;
    case D_POISSON: *lo = 0; *hi = ceil(p[0] + 5 * sqrt(p[0]) + 5); break;
    case D_EXPO:    *lo = 0; *hi = 5 / p[0]; break;
    default:        *lo = -5; *hi = 5; break;
    }
}

double num_inv(num_dist_t d, const double *p, double prob)
{
    if (!(prob > 0) || !(prob < 1)) {
        if (prob == 0) return num_dist_discrete(d) ? 0 : -INFINITY;
        if (prob == 1) return INFINITY;
        return NAN;
    }
    double lo, hi;
    num_dist_range(d, p, &lo, &hi);
    if (num_dist_discrete(d)) {
        /* the smallest k with P(X <= k) >= prob */
        double a = -1, b = hi;
        while (num_cdf(d, p, b) < prob && b < 1e12) b = b * 2 + 1;
        while (b - a > 1) {
            double m = floor((a + b) / 2);
            if (num_cdf(d, p, m) >= prob) b = m; else a = m;
        }
        return b;
    }
    while (num_cdf(d, p, lo) > prob) lo -= (hi - lo);
    while (num_cdf(d, p, hi) < prob) hi += (hi - lo);
    for (int i = 0; i < 200 && hi - lo > 1e-12 * (1 + fabs(lo)); i++) {
        double m = (lo + hi) / 2;
        if (num_cdf(d, p, m) < prob) lo = m; else hi = m;
    }
    return (lo + hi) / 2;
}

/* --- intervals and tests ---------------------------------------------------------------- */

static double tail_p(double cdf_stat, num_tail_t tail)
{
    switch (tail) {
    case TAIL_LEFT:  return cdf_stat;
    case TAIL_RIGHT: return 1 - cdf_stat;
    default: {
        double t = cdf_stat < 0.5 ? cdf_stat : 1 - cdf_stat;
        return 2 * t;
    }
    }
}

void num_t_mean(double mean, double s, int n, double mu0, double conf, num_tail_t tail, num_test_t *o)
{
    memset(o, 0, sizeof *o);
    o->df = n - 1;
    o->se = s / sqrt((double)n);
    o->stat = (mean - mu0) / o->se;
    double par[1] = { o->df };
    o->p = tail_p(num_cdf(D_T, par, o->stat), tail);
    double tc = num_inv(D_T, par, (1 + conf) / 2);
    o->lo = mean - tc * o->se;
    o->hi = mean + tc * o->se;
}

void num_z_prop(int x, int n, double p0, double conf, num_tail_t tail, num_test_t *o)
{
    memset(o, 0, sizeof *o);
    double ph = (double)x / n;
    double par[2] = { 0, 1 };
    o->se = sqrt(ph * (1 - ph) / n);
    double z0 = sqrt(p0 * (1 - p0) / n);
    o->stat = z0 > 0 ? (ph - p0) / z0 : NAN;
    o->p = tail_p(num_cdf(D_NORMAL, par, o->stat), tail);
    double zc = num_inv(D_NORMAL, par, (1 + conf) / 2);
    o->lo = ph - zc * o->se;
    o->hi = ph + zc * o->se;
}

void num_t_two(double m1, double s1, int n1, double m2, double s2, int n2, double conf,
               num_tail_t tail, num_test_t *o)
{
    memset(o, 0, sizeof *o);
    double a = s1 * s1 / n1, b = s2 * s2 / n2;
    o->se = sqrt(a + b);
    o->df = (a + b) * (a + b) / (a * a / (n1 - 1) + b * b / (n2 - 1));
    o->stat = (m1 - m2) / o->se;
    double par[1] = { o->df };
    o->p = tail_p(num_cdf(D_T, par, o->stat), tail);
    double tc = num_inv(D_T, par, (1 + conf) / 2);
    o->lo = (m1 - m2) - tc * o->se;
    o->hi = (m1 - m2) + tc * o->se;
}

/* --- money -------------------------------------------------------------------------------- */

/* the rate per payment period, from the nominal annual one */
static double tvm_i(const num_tvm_t *t)
{
    if (t->iy == 0) return 0;
    return pow(1 + t->iy / (100 * t->cy), t->cy / t->py) - 1;
}

static double tvm_f(const num_tvm_t *t, double i)
{
    double k = t->begin ? 1 : 0;
    if (fabs(i) < 1e-14) return t->pv + t->pmt * t->n + t->fv;
    double g = pow(1 + i, t->n);
    return t->pv * g + t->pmt * (1 + i * k) * (g - 1) / i + t->fv;
}

bool num_tvm_solve(num_tvm_t *t, int which)
{
    if (t->py <= 0 || t->cy <= 0) return false;
    double i = tvm_i(t), k = t->begin ? 1 : 0;
    double g = pow(1 + i, t->n);
    double A = fabs(i) < 1e-14 ? t->n : (g - 1) / i;
    switch (which) {
    case TVM_PV:
        t->pv = -(t->pmt * (1 + i * k) * A + t->fv) / g;
        return isfinite(t->pv);
    case TVM_PMT:
        if (A == 0) return false;
        t->pmt = -(t->pv * g + t->fv) / ((1 + i * k) * A);
        return isfinite(t->pmt);
    case TVM_FV:
        t->fv = -(t->pv * g + t->pmt * (1 + i * k) * A);
        return isfinite(t->fv);
    case TVM_N: {
        if (fabs(i) < 1e-14) {
            if (t->pmt == 0) return false;
            t->n = -(t->pv + t->fv) / t->pmt;
        } else {
            double a = t->pmt * (1 + i * k);
            double num = a - t->fv * i, den = a + t->pv * i;
            if (num / den <= 0) return false;
            t->n = log(num / den) / log(1 + i);
        }
        return isfinite(t->n) && t->n > 0;
    }
    case TVM_IY: {
        /* the rate per period that zeroes the equation: a sign change is
         * looked for along a grid, then bisected */
        num_tvm_t u = *t;
        double prev = NAN, pi = 0;
        double lo = NAN, hi = NAN;
        for (double r = -0.99; r < 10; r = r < 0.1 ? r + 0.0025 : r * 1.05) {
            double f = tvm_f(&u, r);
            if (isfinite(prev) && isfinite(f) && (f == 0 || (f > 0) != (prev > 0))) {
                lo = pi;
                hi = r;
                /* prefer the root nearest zero: the first positive one */
                if (r > 0) break;
            }
            prev = f;
            pi = r;
        }
        if (isnan(lo)) return false;
        double flo = tvm_f(&u, lo);
        for (int it = 0; it < 200; it++) {
            double m = (lo + hi) / 2, fm = tvm_f(&u, m);
            if ((fm > 0) == (flo > 0)) { lo = m; flo = fm; } else hi = m;
        }
        double ip = (lo + hi) / 2;
        /* back to the nominal annual rate */
        t->iy = 100 * t->cy * (pow(1 + ip, t->py / t->cy) - 1);
        return isfinite(t->iy);
    }
    default:
        return false;
    }
}

void num_loan_row(num_loan_kind_t kind, double P, double r, int n, int k, num_loan_row_t *o)
{
    memset(o, 0, sizeof *o);
    if (n <= 0 || k < 1 || k > n) return;
    switch (kind) {
    case LOAN_FRENCH: {
        double g = pow(1 + r, n);
        double c = r == 0 ? P / n : P * r / (1 - 1 / g);
        double before = r == 0 ? P * (n - (k - 1)) / n : P * (g - pow(1 + r, k - 1)) / (g - 1);
        o->interest = before * r;
        o->payment = c;
        o->principal = c - o->interest;
        o->balance = before - o->principal;
        break;
    }
    case LOAN_GERMAN: {
        double a = P / n, before = P - a * (k - 1);
        o->interest = before * r;
        o->principal = a;
        o->payment = a + o->interest;
        o->balance = before - a;
        break;
    }
    case LOAN_AMERICAN:
        o->interest = P * r;
        o->principal = k == n ? P : 0;
        o->payment = o->interest + o->principal;
        o->balance = k == n ? 0 : P;
        break;
    }
    if (fabs(o->balance) < 1e-9 * (1 + P)) o->balance = 0;
}

double num_loan_total_interest(num_loan_kind_t kind, double P, double r, int n)
{
    switch (kind) {
    case LOAN_FRENCH: {
        if (r == 0) return 0;
        double c = P * r / (1 - pow(1 + r, -n));
        return c * n - P;
    }
    case LOAN_GERMAN:   return r * P * (n + 1) / 2;
    case LOAN_AMERICAN: return r * P * n;
    }
    return NAN;
}

double num_npv(double rate, const double *cf, int n)
{
    double v = 0, d = 1;
    for (int t = 0; t < n; t++) {
        v += cf[t] / d;
        d *= 1 + rate;
    }
    return v;
}

bool num_irr(const double *cf, int n, double *irr)
{
    double prev = NAN, pr = 0, lo = NAN, hi = NAN;
    for (double r = -0.99; r < 100; r = r < 0.5 ? r + 0.001 : r * 1.05) {
        double f = num_npv(r, cf, n);
        if (isfinite(prev) && isfinite(f) && (f == 0 || (f > 0) != (prev > 0))) {
            lo = pr;
            hi = r;
            break;
        }
        prev = f;
        pr = r;
    }
    if (isnan(lo)) return false;
    double flo = num_npv(lo, cf, n);
    for (int it = 0; it < 200; it++) {
        double m = (lo + hi) / 2, fm = num_npv(m, cf, n);
        if ((fm > 0) == (flo > 0)) { lo = m; flo = fm; } else hi = m;
    }
    *irr = (lo + hi) / 2;
    return true;
}

double num_payback(const double *cf, int n)
{
    double acc = 0;
    for (int t = 0; t < n; t++) {
        double next = acc + cf[t];
        if (t > 0 && acc < 0 && next >= 0) {
            return (t - 1) + (-acc) / cf[t];
        }
        acc = next;
    }
    return -1;
}
