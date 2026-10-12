/*
 * P4OS - CAS: chemistry, the model.
 *
 * Formulas are read by recursive descent: an element is a capital and
 * maybe a small letter (Co is cobalt, CO is carbon and oxygen), a count
 * follows it or a closing bracket, a dot (·, . or *) starts the next part
 * of a hydrate with its own coefficient, and a charge is written after a
 * caret (SO4^2-) or, alone, at the very end (Na+). Balancing builds the
 * matrix of elements (and charge) by species, products negative, takes it
 * to reduced row echelon form in exact fractions, and reads the one
 * solution off its free column; none or more than one is said as such.
 */
#include "cas_chem.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GEO_HOST_TEST
#define _(s) (s)
#else
#include "aos_i18n.h"
#endif

/* --- the table ---------------------------------------------------------------- */

int chem_find(const char *sym, size_t len)
{
    for (int z = 1; z <= 118; z++) {
        const char *s = CHEM_EL[z - 1].sym;
        if (strlen(s) == len && !strncmp(s, sym, len)) return z;
    }
    return 0;
}

void chem_pos(int z, int *row, int *col)
{
    static const int START[] = { 1, 3, 11, 19, 37, 55, 87, 119 };
    if (z >= 57 && z <= 71) {
        *row = 8;
        *col = 2 + (z - 57);
        return;
    }
    if (z >= 89 && z <= 103) {
        *row = 9;
        *col = 2 + (z - 89);
        return;
    }
    int p = 0;
    while (p < 7 && z >= START[p + 1]) p++;
    *row = p;
    int k = z - START[p];                  /* 0-based within the period */
    if (p == 0) {
        *col = k == 0 ? 0 : 17;
    } else if (p <= 2) {
        *col = k < 2 ? k : k + 10;         /* s-block, then the p-block at 12 */
    } else if (p <= 4) {
        *col = k;
    } else {
        /* 6 and 7: s-block, then lanthanum/actinium's place, then 72.. */
        if (k < 2) *col = k;
        else if (k < 17) *col = 2;         /* the f-block, drawn apart */
        else *col = k - 14;
    }
}

int chem_group(int z)
{
    int r, c;
    chem_pos(z, &r, &c);
    return r >= 8 ? 0 : c + 1;
}

int chem_period(int z)
{
    int r, c;
    chem_pos(z, &r, &c);
    return r == 8 ? 6 : r == 9 ? 7 : r + 1;
}

/* --- formulas ----------------------------------------------------------------- */

typedef struct {
    const char *s;
    double      cnt[119];
    int         order[CHEM_MAXEL], norder;
    char       *err;
    size_t      errn;
} rd_t;

static bool fail(rd_t *r, const char *msg, const char *arg)
{
    if (r->err && !r->err[0]) snprintf(r->err, r->errn, msg, arg);
    return false;
}

static long number(rd_t *r)
{
    long v = 0;
    while (isdigit((unsigned char)*r->s)) v = v * 10 + (*r->s++ - '0');
    return v;
}

static bool is_dot(const char *s, int *len)
{
    if (*s == '.' || *s == '*') { *len = 1; return true; }
    if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xB7) { *len = 2; return true; }   /* · */
    if ((unsigned char)s[0] == 0xE2 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0xA2) { *len = 3; return true; } /* • */
    return false;
}

static bool add(rd_t *r, int z, double k)
{
    if (r->cnt[z] == 0) {
        bool there = false;
        for (int i = 0; i < r->norder; i++) there |= r->order[i] == z;
        if (!there) {
            if (r->norder == CHEM_MAXEL) return fail(r, "%s", _("demasiados elementos"));
            r->order[r->norder++] = z;
        }
    }
    r->cnt[z] += k;
    return true;
}

/* a run of elements and groups, all multiplied by mult */
static bool group(rd_t *r, double mult, char close)
{
    bool any = false;
    for (;;) {
        char c = *r->s;
        if (c == ' ') {
            r->s++;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            char want = c == '(' ? ')' : c == '[' ? ']' : '}';
            r->s++;
            /* read the inside once to find its count, then again with it */
            int depth = 1;
            const char *p = r->s;
            while (*p && depth) {
                if (*p == '(' || *p == '[' || *p == '{') depth++;
                if (*p == ')' || *p == ']' || *p == '}') depth--;
                p++;
            }
            if (depth) return fail(r, "%s", _("falta cerrar un paréntesis"));
            const char *after = p;
            long k = 1;
            if (isdigit((unsigned char)*after)) {
                k = 0;
                while (isdigit((unsigned char)*after)) k = k * 10 + (*after++ - '0');
            }
            if (!group(r, mult * (double)k, want)) return false;
            if (*r->s != want) return fail(r, "%s", _("paréntesis mal cerrado"));
            r->s++;
            number(r);              /* its count, already used */
            any = true;
            continue;
        }
        if (isupper((unsigned char)c)) {
            const char *p = r->s + 1;
            size_t len = 1;
            if (islower((unsigned char)*p)) {
                len = 2;
                if (islower((unsigned char)p[1])) len = 3;
            }
            int z = 0;
            while (len && !(z = chem_find(r->s, len))) len--;
            if (!z) {
                char sym[4] = { c, 0, 0, 0 };
                if (islower((unsigned char)r->s[1])) sym[1] = r->s[1];
                return fail(r, _("no hay elemento %s"), sym);
            }
            r->s += len;
            if (islower((unsigned char)*r->s)) {
                char sym[5];
                snprintf(sym, sizeof sym, "%.*s%c", (int)len, r->s - len, *r->s);
                return fail(r, _("no hay elemento %s"), sym);
            }
            long k = isdigit((unsigned char)*r->s) ? number(r) : 1;
            if (k == 0) return fail(r, "%s", _("un subíndice no puede ser 0"));
            if (!add(r, z, mult * (double)k)) return false;
            any = true;
            continue;
        }
        break;
    }
    if (close && !*r->s) return fail(r, "%s", _("falta cerrar un paréntesis"));
    if (!any) {
        char b[8] = "";
        if (*r->s) snprintf(b, sizeof b, "%.4s", r->s);
        return *r->s ? fail(r, _("no se entiende: %s"), b) : fail(r, "%s", _("falta la fórmula"));
    }
    return true;
}

/* ^2-, ^+, ^3+ ; or a lone + or - at the very end */
static bool charge(rd_t *r, int *q)
{
    *q = 0;
    const char *s = r->s;
    while (*s == ' ') s++;
    bool caret = *s == '^';
    if (caret) s++;
    long k = 0;
    bool digits = false;
    while (isdigit((unsigned char)*s)) {
        k = k * 10 + (*s++ - '0');
        digits = true;
    }
    if (*s == '+' || *s == '-') {
        if (!caret && digits) return true;          /* not a charge */
        *q = (int)((digits ? k : 1) * (*s == '+' ? 1 : -1));
        s++;
        /* "2+" after a caret; "++" counts twice */
        while (*s == '+' || *s == '-') {
            *q += *s == '+' ? 1 : -1;
            s++;
        }
        r->s = s;
        return true;
    }
    if (caret) return fail(r, "%s", _("la carga va así: ^2+ o ^-"));
    return true;
}

bool chem_formula(const char *text, chem_comp_t *out, char *err, size_t errn)
{
    static rd_t r;
    memset(&r, 0, sizeof r);
    memset(out, 0, sizeof *out);
    if (err && errn) err[0] = 0;
    r.err = err;
    r.errn = errn;
    r.s = text;
    while (*r.s == ' ') r.s++;
    if (isdigit((unsigned char)*r.s)) number(&r);       /* a coefficient */
    if (!group(&r, 1, 0)) return false;
    int dl;
    while (is_dot(r.s, &dl)) {
        r.s += dl;
        while (*r.s == ' ') r.s++;
        long k = isdigit((unsigned char)*r.s) ? number(&r) : 1;
        /* the part after the dot is read, then multiplied by its coefficient */
        double before[119];
        memcpy(before, r.cnt, sizeof before);
        if (!group(&r, 1, 0)) return false;
        for (int z = 1; z <= 118; z++) r.cnt[z] = before[z] + (r.cnt[z] - before[z]) * (double)k;
    }
    if (!charge(&r, &out->charge)) return false;
    while (*r.s == ' ') r.s++;
    if (*r.s) {
        char b[8];
        snprintf(b, sizeof b, "%.4s", r.s);
        return fail(&r, _("no se entiende: %s"), b);
    }
    for (int i = 0; i < r.norder; i++) {
        int z = r.order[i];
        out->z[out->n] = z;
        out->count[out->n] = r.cnt[z];
        out->mass += r.cnt[z] * CHEM_EL[z - 1].mass;
        out->n++;
    }
    return out->n > 0 || fail(&r, "%s", _("falta la fórmula"));
}

static const char *const SUB[] = { "\xE2\x82\x80", "\xE2\x82\x81", "\xE2\x82\x82", "\xE2\x82\x83", "\xE2\x82\x84",
                                   "\xE2\x82\x85", "\xE2\x82\x86", "\xE2\x82\x87", "\xE2\x82\x88", "\xE2\x82\x89" };
static const char *const SUP[] = { "\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4",
                                   "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9" };

static void put(char *out, size_t n, size_t *k, const char *s)
{
    size_t l = strlen(s);
    if (*k + l < n) {
        memcpy(out + *k, s, l);
        *k += l;
        out[*k] = 0;
    }
}

void chem_pretty(const char *text, char *out, size_t n)
{
    size_t k = 0;
    if (n) out[0] = 0;
    const char *s = text;
    bool coef = true;               /* digits here are a coefficient */
    while (*s) {
        int dl;
        if (*s == ' ') {
            s++;
            continue;
        }
        if (is_dot(s, &dl)) {
            put(out, n, &k, "\xC2\xB7");
            s += dl;
            coef = true;
            continue;
        }
        if (*s == '^' || ((*s == '+' || *s == '-') && !s[1])) {
            /* the charge: digits and sign up */
            if (*s == '^') s++;
            while (isdigit((unsigned char)*s)) put(out, n, &k, SUP[*s++ - '0']);
            while (*s == '+' || *s == '-') put(out, n, &k, *s++ == '+' ? "\xE2\x81\xBA" : "\xE2\x81\xBB");
            continue;
        }
        if (isdigit((unsigned char)*s)) {
            if (coef) {
                while (isdigit((unsigned char)*s)) {
                    char c[2] = { *s++, 0 };
                    put(out, n, &k, c);
                }
            } else {
                while (isdigit((unsigned char)*s)) put(out, n, &k, SUB[*s++ - '0']);
            }
            continue;
        }
        coef = false;
        char c[2] = { *s++, 0 };
        put(out, n, &k, c);
    }
}

/* --- balancing --------------------------------------------------------------------- */

typedef struct {
    long long n, d;
} frac_t;

static long long gcdll(long long a, long long b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

static frac_t fr(long long n, long long d)
{
    if (d < 0) {
        n = -n;
        d = -d;
    }
    long long g = gcdll(n, d);
    frac_t f = { n / g, d / g };
    return f;
}

static frac_t fsub(frac_t a, frac_t b) { return fr(a.n * b.d - b.n * a.d, a.d * b.d); }
static frac_t fmul(frac_t a, frac_t b) { return fr(a.n * b.n, a.d * b.d); }
static frac_t fdiv(frac_t a, frac_t b) { return fr(a.n * b.d, a.d * b.n); }

/* the species of one side: split at '+' that is not a charge */
static bool split_side(const char *a, const char *b, chem_eq_t *eq, char *err, size_t errn)
{
    const char *s = a;
    while (s < b) {
        const char *e = s;
        while (e < b) {
            if (*e == '+') {
                /* a charge: after a caret (with digits), or doubled */
                const char *p = e - 1;
                while (p >= s && isdigit((unsigned char)*p)) p--;
                if (p >= s && *p == '^') {
                    e++;
                    continue;
                }
            }
            if (*e == '+') break;
            e++;
        }
        /* trim, drop a typed coefficient */
        const char *x = s, *y = e;
        while (x < y && *x == ' ') x++;
        while (y > x && y[-1] == ' ') y--;
        while (x < y && isdigit((unsigned char)*x)) x++;
        while (x < y && *x == ' ') x++;
        if (x == y) {
            snprintf(err, errn, "%s", _("falta una sustancia"));
            return false;
        }
        if (eq->n == CHEM_MAXSP) {
            snprintf(err, errn, "%s", _("demasiadas sustancias"));
            return false;
        }
        snprintf(eq->sp[eq->n], sizeof eq->sp[0], "%.*s", (int)(y - x), x);
        if (!chem_formula(eq->sp[eq->n], &eq->comp[eq->n], err, errn)) return false;
        eq->n++;
        s = e < b ? e + 1 : e;
    }
    return true;
}

chem_bal_t chem_balance(const char *text, chem_eq_t *eq, char *err, size_t errn)
{
    memset(eq, 0, sizeof *eq);
    err[0] = 0;
    const char *arrow = NULL;
    int alen = 0;
    for (const char *p = text; *p && !arrow; p++) {
        if (*p == '=') { arrow = p; alen = 1; }
        else if (p[0] == '-' && p[1] == '>') { arrow = p; alen = 2; }
        else if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x86 && (unsigned char)p[2] == 0x92) { arrow = p; alen = 3; }
    }
    if (!arrow) {
        snprintf(err, errn, "%s", _("separá los lados con = o ->"));
        return CHEM_BAL_SYNTAX;
    }
    /* "=>" or "==" count as one arrow */
    const char *right = arrow + alen;
    while (*right == '=' || *right == '>') right++;
    if (!split_side(text, arrow, eq, err, errn)) return CHEM_BAL_SYNTAX;
    eq->nleft = eq->n;
    if (!split_side(right, right + strlen(right), eq, err, errn)) return CHEM_BAL_SYNTAX;
    if (!eq->nleft || eq->n == eq->nleft) {
        snprintf(err, errn, "%s", _("falta un lado"));
        return CHEM_BAL_SYNTAX;
    }

    /* the matrix: one row per element, and charge if any */
    static frac_t m[CHEM_MAXEL * 2 + 1][CHEM_MAXSP];
    int zs[CHEM_MAXEL * 2], nz = 0;
    bool charged = false;
    for (int j = 0; j < eq->n; j++) {
        charged |= eq->comp[j].charge != 0;
        for (int k = 0; k < eq->comp[j].n; k++) {
            int z = eq->comp[j].z[k], i;
            for (i = 0; i < nz && zs[i] != z; i++) {}
            if (i == nz) {
                if (nz == CHEM_MAXEL * 2) {
                    snprintf(err, errn, "%s", _("demasiados elementos"));
                    return CHEM_BAL_SYNTAX;
                }
                zs[nz++] = z;
            }
        }
    }
    int rows = nz + (charged ? 1 : 0), cols = eq->n;
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            double c = 0;
            const chem_comp_t *cp = &eq->comp[j];
            if (i < nz) {
                for (int k = 0; k < cp->n; k++) {
                    if (cp->z[k] == zs[i]) c = cp->count[k];
                }
            } else {
                c = cp->charge;
            }
            /* hydrates can give halves: counts are kept as fractions of 2 */
            long long num = llround(c * 2);
            m[i][j] = fr(j < eq->nleft ? num : -num, 2);
        }
    }
    /* reduced row echelon form */
    int piv[CHEM_MAXSP], rank = 0;
    for (int j = 0; j < cols && rank < rows; j++) {
        int p = -1;
        for (int i = rank; i < rows; i++) {
            if (m[i][j].n) {
                p = i;
                break;
            }
        }
        if (p < 0) continue;
        for (int c = 0; c < cols; c++) {
            frac_t t = m[p][c];
            m[p][c] = m[rank][c];
            m[rank][c] = t;
        }
        frac_t d = m[rank][j];
        for (int c = 0; c < cols; c++) m[rank][c] = fdiv(m[rank][c], d);
        for (int i = 0; i < rows; i++) {
            if (i == rank || !m[i][j].n) continue;
            frac_t f = m[i][j];
            for (int c = 0; c < cols; c++) m[i][c] = fsub(m[i][c], fmul(f, m[rank][c]));
        }
        piv[rank++] = j;
    }
    int nfree = cols - rank;
    if (nfree == 0) {
        snprintf(err, errn, "%s", _("no se puede balancear: revisá las fórmulas"));
        return CHEM_BAL_NONE;
    }
    if (nfree > 1) {
        snprintf(err, errn, "%s", _("tiene más de una forma de balancearse: son varias reacciones juntas"));
        return CHEM_BAL_MANY;
    }
    int f = 0;
    {
        bool is_piv[CHEM_MAXSP] = { 0 };
        for (int r = 0; r < rank; r++) is_piv[piv[r]] = true;
        while (is_piv[f]) f++;
    }
    frac_t x[CHEM_MAXSP];
    for (int j = 0; j < cols; j++) x[j] = fr(0, 1);
    x[f] = fr(1, 1);
    for (int r = 0; r < rank; r++) x[piv[r]] = fr(-m[r][f].n, m[r][f].d);
    long long l = 1;
    for (int j = 0; j < cols; j++) l = l / gcdll(l, x[j].d) * x[j].d;
    long long g = 0;
    for (int j = 0; j < cols; j++) {
        long long v = x[j].n * (l / x[j].d);
        eq->coef[j] = (long)v;
        g = gcdll(g, v);
    }
    bool neg = eq->coef[0] < 0;
    for (int j = 0; j < cols; j++) {
        eq->coef[j] /= (long)g;
        if (neg) eq->coef[j] = -eq->coef[j];
        if (eq->coef[j] <= 0) {
            snprintf(err, errn, "%s", _("no se puede balancear: revisá las fórmulas"));
            return CHEM_BAL_NONE;
        }
    }
    return CHEM_BAL_OK;
}

void chem_eq_text(const chem_eq_t *eq, char *out, size_t n)
{
    size_t k = 0;
    if (n) out[0] = 0;
    for (int j = 0; j < eq->n; j++) {
        if (j) put(out, n, &k, j == eq->nleft ? " \xE2\x86\x92 " : " + ");
        if (eq->coef[j] != 1) {
            char c[24];
            snprintf(c, sizeof c, "%ld ", eq->coef[j]);
            put(out, n, &k, c);
        }
        char p[96];
        chem_pretty(eq->sp[j], p, sizeof p);
        put(out, n, &k, p);
    }
}
