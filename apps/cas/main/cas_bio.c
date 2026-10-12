/*
 * P4OS - CAS: biology, the model.
 *
 * A genotype is pairs of letters, one pair a gene (AaBb); a capital is the
 * dominant allele. Gametes take one letter of each pair; the square crosses
 * every gamete of one parent with every gamete of the other, and the
 * offspring are counted by genotype (each pair written capital first) and
 * by phenotype (A_ when a capital is there, aa when not).
 */
#include "cas_bio.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef GEO_HOST_TEST
#define _(s) (s)
#else
#include "aos_i18n.h"
#endif

/* --- Punnett ----------------------------------------------------------------- */

static bool genotype(const char *s, char pairs[BIO_GENES][2], int *n, char *err, size_t errn)
{
    *n = 0;
    char clean[16];
    size_t k = 0;
    for (; *s; s++) {
        if (isalpha((unsigned char)*s)) {
            if (k + 1 >= sizeof clean) {
                snprintf(err, errn, _("hasta %d genes"), BIO_GENES);
                return false;
            }
            clean[k++] = *s;
        } else if (*s != ' ') {
            snprintf(err, errn, "%s", _("sólo letras: AaBb"));
            return false;
        }
    }
    if (!k || k % 2) {
        snprintf(err, errn, "%s", _("cada gen son dos letras: AaBb"));
        return false;
    }
    if (k / 2 > BIO_GENES) {
        snprintf(err, errn, _("hasta %d genes"), BIO_GENES);
        return false;
    }
    for (size_t i = 0; i < k; i += 2) {
        if (tolower((unsigned char)clean[i]) != tolower((unsigned char)clean[i + 1])) {
            snprintf(err, errn, _("%c y %c no son el mismo gen"), clean[i], clean[i + 1]);
            return false;
        }
        pairs[*n][0] = clean[i];
        pairs[*n][1] = clean[i + 1];
        (*n)++;
    }
    return true;
}

static int gametes(char pairs[BIO_GENES][2], int n, char out[BIO_GAM][BIO_GENES + 1])
{
    int m = 0;
    for (int mask = 0; mask < (1 << n); mask++) {
        char g[BIO_GENES + 1];
        for (int i = 0; i < n; i++) g[i] = pairs[i][(mask >> (n - 1 - i)) & 1];
        g[n] = 0;
        bool dup = false;
        for (int k = 0; k < m; k++) dup |= !strcmp(out[k], g);
        if (!dup) strcpy(out[m++], g);
    }
    return m;
}

bool bio_cross(const char *p1, const char *p2, bio_cross_t *o, char *err, size_t errn)
{
    memset(o, 0, sizeof *o);
    err[0] = 0;
    char a[BIO_GENES][2], b[BIO_GENES][2];
    int na, nb;
    if (!genotype(p1, a, &na, err, errn) || !genotype(p2, b, &nb, err, errn)) return false;
    if (na != nb) {
        snprintf(err, errn, "%s", _("los dos padres necesitan los mismos genes"));
        return false;
    }
    for (int i = 0; i < na; i++) {
        if (tolower((unsigned char)a[i][0]) != tolower((unsigned char)b[i][0])) {
            snprintf(err, errn, "%s", _("los dos padres necesitan los mismos genes, en el mismo orden"));
            return false;
        }
    }
    o->ngenes = na;
    o->ng1 = gametes(a, na, o->g1);
    o->ng2 = gametes(b, nb, o->g2);
    for (int r = 0; r < o->ng1; r++) {
        for (int c = 0; c < o->ng2; c++) {
            char *cell = o->cell[r][c];
            char ph[3 * BIO_GENES + 1] = "";
            for (int i = 0; i < na; i++) {
                char x = o->g1[r][i], y = o->g2[c][i];
                if (islower((unsigned char)x) && isupper((unsigned char)y)) {
                    char t = x;
                    x = y;
                    y = t;
                }
                cell[2 * i] = x;
                cell[2 * i + 1] = y;
                char p[4];
                if (isupper((unsigned char)x)) snprintf(p, sizeof p, "%c_", x);
                else snprintf(p, sizeof p, "%c%c", x, y);
                if (i) strcat(ph, " ");
                strcat(ph, p);
            }
            cell[2 * na] = 0;
            int k;
            for (k = 0; k < o->ngeno && strcmp(o->geno[k], cell); k++) {}
            if (k == o->ngeno) strcpy(o->geno[o->ngeno++], cell);
            o->geno_n[k]++;
            for (k = 0; k < o->npheno && strcmp(o->pheno[k], ph); k++) {}
            if (k == o->npheno) strcpy(o->pheno[o->npheno++], ph);
            o->pheno_n[k]++;
            o->total++;
        }
    }
    /* the usual order: more dominant first */
    for (int i = 1; i < o->npheno; i++) {
        for (int j = i; j > 0; j--) {
            if (o->pheno_n[j] <= o->pheno_n[j - 1]) break;
            char t[3 * BIO_GENES + 1];
            strcpy(t, o->pheno[j]);
            strcpy(o->pheno[j], o->pheno[j - 1]);
            strcpy(o->pheno[j - 1], t);
            int tn = o->pheno_n[j];
            o->pheno_n[j] = o->pheno_n[j - 1];
            o->pheno_n[j - 1] = tn;
        }
    }
    return true;
}

bool bio_hardy(double AA, double Aa, double aa, bio_hw_t *o)
{
    memset(o, 0, sizeof *o);
    double n = AA + Aa + aa;
    if (!(n > 0) || AA < 0 || Aa < 0 || aa < 0) return false;
    o->p = (2 * AA + Aa) / (2 * n);
    o->q = 1 - o->p;
    o->exp[0] = o->p * o->p * n;
    o->exp[1] = 2 * o->p * o->q * n;
    o->exp[2] = o->q * o->q * n;
    double obs[3] = { AA, Aa, aa };
    for (int i = 0; i < 3; i++) {
        if (o->exp[i] > 0) o->chi2 += (obs[i] - o->exp[i]) * (obs[i] - o->exp[i]) / o->exp[i];
    }
    return true;
}

/* --- DNA ------------------------------------------------------------------------ */

size_t bio_clean(const char *in, char *out, size_t n, int *bad)
{
    size_t k = 0;
    *bad = 0;
    for (; *in; in++) {
        char c = (char)toupper((unsigned char)*in);
        if (c == 'A' || c == 'C' || c == 'G' || c == 'T' || c == 'U') {
            if (k + 1 < n) out[k++] = c;
        } else if (isalpha((unsigned char)c)) {
            (*bad)++;
        }
    }
    if (n) out[k] = 0;
    return k;
}

static char comp(char c)
{
    switch (c) {
    case 'A': return 'T';
    case 'T': return 'A';
    case 'U': return 'A';
    case 'C': return 'G';
    case 'G': return 'C';
    default:  return 'N';
    }
}

void bio_complement(const char *s, char *out, size_t n, bool reverse)
{
    size_t l = strlen(s), k = 0;
    for (size_t i = 0; i < l && k + 1 < n; i++) out[k++] = comp(s[reverse ? l - 1 - i : i]);
    if (n) out[k] = 0;
}

void bio_transcribe(const char *s, char *out, size_t n)
{
    size_t k = 0;
    for (; *s && k + 1 < n; s++) out[k++] = *s == 'T' ? 'U' : *s;
    if (n) out[k] = 0;
}

static int base_index(char c)
{
    switch (c) {
    case 'T': case 'U': return 0;
    case 'C': return 1;
    case 'A': return 2;
    case 'G': return 3;
    default:  return -1;
    }
}

/* the standard code, in the order TCAG for each of the three positions */
static const char CODE[] = "FFLLSSSSYY**CC*WLLLLPPPPHHQQRRRRIIIMTTTTNNKKSSRRVVVVAAAADDEEGGGG";

void bio_translate(const char *s, int frame, bool from_atg, char *out, size_t n)
{
    size_t l = strlen(s), k = 0, i = (size_t)frame;
    if (from_atg) {
        const char *p = s;
        i = l;
        for (size_t j = 0; j + 2 < l; j++) {
            if (p[j] == 'A' && (p[j + 1] == 'T' || p[j + 1] == 'U') && p[j + 2] == 'G') {
                i = j;
                break;
            }
        }
    }
    for (; i + 3 <= l && k + 1 < n; i += 3) {
        int a = base_index(s[i]), b = base_index(s[i + 1]), c = base_index(s[i + 2]);
        char aa = a < 0 || b < 0 || c < 0 ? 'X' : CODE[a * 16 + b * 4 + c];
        out[k++] = aa;
        if (from_atg && aa == '*') break;
    }
    if (n) out[k] = 0;
}

void bio_dna(const char *s, bio_dna_t *o)
{
    memset(o, 0, sizeof *o);
    for (; *s; s++) {
        switch (*s) {
        case 'A': o->a++; break;
        case 'C': o->c++; break;
        case 'G': o->g++; break;
        case 'T': case 'U': o->t++; break;
        default: break;
        }
    }
    o->len = o->a + o->c + o->g + o->t;
    if (!o->len) return;
    o->gc = 100.0 * (o->g + o->c) / o->len;
    /* Wallace's rule for a short primer, the basic GC formula past 13 */
    if (o->len < 14) o->tm = 2.0 * (o->a + o->t) + 4.0 * (o->g + o->c);
    else o->tm = 64.9 + 41.0 * (o->g + o->c - 16.4) / o->len;
    /* single strand, anhydrous nucleotides, without the 5' phosphate */
    o->mw = o->a * 313.21 + o->t * 304.2 + o->c * 289.18 + o->g * 329.21 - 61.96;
}
