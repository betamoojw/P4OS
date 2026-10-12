/*
 * P4OS - CAS: chemistry, the model (cas_chem.c, cas_elements.c).
 *
 * The periodic table, formulas read as people write them (Ca(OH)2,
 * CuSO4·5H2O, [Fe(CN)6]^3-), their molar mass and composition, and
 * equations balanced by the null space of their element matrix, in exact
 * fractions: Fe + O2 = Fe2O3 gives 4, 3, 2. No LVGL here; tested on the Mac.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifndef N_
#define N_(s) (s)
#endif

enum {
    CHEM_ALKALI, CHEM_ALKALINE, CHEM_TRANSITION, CHEM_POST, CHEM_METALLOID,
    CHEM_NONMETAL, CHEM_HALOGEN, CHEM_NOBLE, CHEM_LANTHANIDE, CHEM_ACTINIDE,
    CHEM_FAMILIES
};

typedef struct {
    const char *sym;
    const char *name;           /* Spanish: the key of the translation */
    double      mass;           /* g/mol; a mass number when radio      */
    unsigned char radio;        /* no stable isotope: shown as [mass]  */
    unsigned char family;
    unsigned char state;        /* 0 solid, 1 liquid, 2 gas; | 4 predicted */
    const char *config;         /* [Ne] 3s² 3p⁵                         */
    float       en;             /* Pauling; NAN unknown                 */
    float       melt, boil;     /* K                                    */
    float       density;        /* g/cm³                                */
    const char *ox;             /* oxidation states                     */
} chem_el_t;

extern const chem_el_t CHEM_EL[118];

int  chem_find(const char *sym, size_t len);    /* atomic number, 0 if none */
/* Where element z sits in the usual drawing: 18 columns; rows 0..6 the
 * periods, 8 and 9 the lanthanides and actinides. */
void chem_pos(int z, int *row, int *col);
int  chem_group(int z);                         /* 1..18, 0 for the f-block */
int  chem_period(int z);

/* --- formulas -------------------------------------------------------------- */

#define CHEM_MAXEL 24

typedef struct {
    int    n;                   /* elements, in the order they first appear */
    int    z[CHEM_MAXEL];
    double count[CHEM_MAXEL];   /* hydrates and groups can make halves      */
    int    charge;
    double mass;                /* g/mol                                     */
} chem_comp_t;

/* false with err set ("unknown element Xx", ...). A leading number is a
 * coefficient and is left out of the composition. */
bool chem_formula(const char *text, chem_comp_t *out, char *err, size_t errn);
/* the formula as it prints: H₂SO₄, CuSO₄·5H₂O, Fe³⁺ */
void chem_pretty(const char *text, char *out, size_t n);

/* --- equations ------------------------------------------------------------------ */

#define CHEM_MAXSP 12

typedef struct {
    int         n, nleft;               /* species; the first nleft react  */
    char        sp[CHEM_MAXSP][48];     /* as typed, without coefficients   */
    chem_comp_t comp[CHEM_MAXSP];
    long        coef[CHEM_MAXSP];       /* the balanced coefficients        */
} chem_eq_t;

typedef enum { CHEM_BAL_OK, CHEM_BAL_SYNTAX, CHEM_BAL_NONE, CHEM_BAL_MANY } chem_bal_t;

/* "Fe + O2 = Fe2O3", with =, -> or → between the sides. */
chem_bal_t chem_balance(const char *text, chem_eq_t *eq, char *err, size_t errn);
/* "4 Fe + 3 O₂ → 2 Fe₂O₃" */
void chem_eq_text(const chem_eq_t *eq, char *out, size_t n);
