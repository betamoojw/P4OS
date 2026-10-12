/*
 * P4OS - CAS: biology, the model (cas_bio.c).
 *
 * Punnett squares for up to three genes with simple dominance, Hardy-
 * Weinberg from genotype counts, and the arithmetic of a DNA sequence:
 * complement, transcription, translation (the standard code), GC content,
 * melting temperature and mass. No LVGL here; tested on the Mac.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define BIO_GENES 3
#define BIO_GAM   8             /* 2^BIO_GENES */

typedef struct {
    int  ngenes;
    int  ng1, ng2;              /* gametes of each parent (distinct) */
    char g1[BIO_GAM][BIO_GENES + 1], g2[BIO_GAM][BIO_GENES + 1];
    char cell[BIO_GAM][BIO_GAM][2 * BIO_GENES + 1];     /* [row: g1][col: g2] */
    /* the offspring, grouped: genotypes and phenotypes with their counts */
    int  ngeno, npheno, total;
    char geno[64][2 * BIO_GENES + 1];
    int  geno_n[64];
    char pheno[BIO_GAM][3 * BIO_GENES + 1];               /* "A_ bb" */
    int  pheno_n[BIO_GAM];
} bio_cross_t;

/* "AaBb" x "Aabb"; false with err set */
bool bio_cross(const char *p1, const char *p2, bio_cross_t *out, char *err, size_t errn);

typedef struct {
    double p, q;                /* allele frequencies             */
    double exp[3];              /* expected AA, Aa, aa            */
    double chi2;                /* against the observed, 1 d.f.   */
} bio_hw_t;

bool bio_hardy(double AA, double Aa, double aa, bio_hw_t *out);

/* --- DNA -------------------------------------------------------------------- */

/* Keeps the letters of a sequence (ACGTU, upper case), drops the rest; how
 * many there were that are not bases goes in *bad. */
size_t bio_clean(const char *in, char *out, size_t n, int *bad);
void   bio_complement(const char *s, char *out, size_t n, bool reverse);
void   bio_transcribe(const char *s, char *out, size_t n);          /* T -> U */
/* one-letter amino acids, '*' for a stop; frame 0..2; from_atg: start at
 * the first ATG and end at the first stop */
void   bio_translate(const char *s, int frame, bool from_atg, char *out, size_t n);

typedef struct {
    int    len, a, c, g, t;     /* t counts U too */
    double gc;                  /* %                 */
    double tm;                  /* °C                */
    double mw;                  /* g/mol, single strand */
} bio_dna_t;

void bio_dna(const char *s, bio_dna_t *out);
