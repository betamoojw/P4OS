/*
 * P4OS - CAS: physics, the model.
 *
 * The formulas are data: their equation in the evaluator's syntax, how it
 * prints, and their variables with name, unit and, for the constants that
 * appear in them (g, G, k, c, h), the value already filled in. Solving
 * substitutes text and compiles it, so a formula is added by writing one
 * more entry and nothing else.
 */
#include "cas_phys.h"

#include "mx.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define G0 9.80665

/* variables used again and again */
#define V_M     { "m", "m", N_("masa"), "kg", NAN }
#define V_V     { "v", "v", N_("velocidad"), "m/s", NAN }
#define V_V0    { "v0", "v\xE2\x82\x80", N_("velocidad inicial"), "m/s", NAN }
#define V_A     { "a", "a", N_("aceleración"), "m/s\xC2\xB2", NAN }
#define V_T     { "t", "t", N_("tiempo"), "s", NAN }
#define V_G     { "g", "g", N_("gravedad"), "m/s\xC2\xB2", G0 }
#define V_H     { "h", "h", N_("altura"), "m", NAN }
#define V_F     { "F", "F", N_("fuerza"), "N", NAN }
#define V_D     { "d", "d", N_("distancia"), "m", NAN }
#define V_R     { "r", "r", N_("radio"), "m", NAN }
#define V_K     { "k", "k", N_("constante del resorte"), "N/m", NAN }
#define V_X     { "x", "x", N_("estiramiento"), "m", NAN }
#define V_RHO   { "rho", "\xCF\x81", N_("densidad"), "kg/m\xC2\xB3", NAN }
#define V_VOL   { "V", "V", N_("volumen"), "m\xC2\xB3", NAN }

const phys_formula_t PHYS_F[] = {
    /* kinematics */
    { N_("Cinemática"), N_("Movimiento uniforme"), "x = x0 + v*t", "x = x\xE2\x82\x80 + v\xC2\xB7t", 4,
      { { "x", "x", N_("posición"), "m", NAN }, { "x0", "x\xE2\x82\x80", N_("posición inicial"), "m", NAN }, V_V, V_T } },
    { N_("Cinemática"), N_("Velocidad (MRUV)"), "v = v0 + a*t", "v = v\xE2\x82\x80 + a\xC2\xB7t", 4, { V_V, V_V0, V_A, V_T } },
    { N_("Cinemática"), N_("Posición (MRUV)"), "x = x0 + v0*t + a*t^2/2", "x = x\xE2\x82\x80 + v\xE2\x82\x80t + \xC2\xBD" "at\xC2\xB2", 5,
      { { "x", "x", N_("posición"), "m", NAN }, { "x0", "x\xE2\x82\x80", N_("posición inicial"), "m", NAN }, V_V0, V_A, V_T } },
    { N_("Cinemática"), N_("Torricelli"), "v^2 = v0^2 + 2*a*d", "v\xC2\xB2 = v\xE2\x82\x80\xC2\xB2 + 2ad", 4, { V_V, V_V0, V_A, V_D } },
    { N_("Cinemática"), N_("Caída libre"), "h = g*t^2/2", "h = \xC2\xBD" "gt\xC2\xB2", 3, { V_H, V_G, V_T } },
    /* dynamics */
    { N_("Dinámica"), N_("Segunda ley de Newton"), "F = m*a", "F = m\xC2\xB7" "a", 3, { V_F, V_M, V_A } },
    { N_("Dinámica"), N_("Peso"), "P = m*g", "P = m\xC2\xB7g", 3, { { "P", "P", N_("peso"), "N", NAN }, V_M, V_G } },
    { N_("Dinámica"), N_("Rozamiento"), "Fr = mu*N", "Fr = \xCE\xBC\xC2\xB7N", 3,
      { { "Fr", "Fr", N_("fuerza de rozamiento"), "N", NAN }, { "mu", "\xCE\xBC", N_("coeficiente de rozamiento"), "", NAN },
        { "N", "N", N_("fuerza normal"), "N", NAN } } },
    { N_("Dinámica"), N_("Ley de Hooke"), "F = k*x", "F = k\xC2\xB7x", 3, { V_F, V_K, V_X } },
    { N_("Dinámica"), N_("Cantidad de movimiento"), "p = m*v", "p = m\xC2\xB7v", 3,
      { { "p", "p", N_("cantidad de movimiento"), "kg\xC2\xB7m/s", NAN }, V_M, V_V } },
    { N_("Dinámica"), N_("Impulso"), "J = F*t", "J = F\xC2\xB7t", 3, { { "J", "J", N_("impulso"), "N\xC2\xB7s", NAN }, V_F, V_T } },
    /* energy */
    { N_("Energía"), N_("Energía cinética"), "Ec = m*v^2/2", "E\xE2\x82\x96 = \xC2\xBD" "mv\xC2\xB2", 3,
      { { "Ec", "E\xE2\x82\x96", N_("energía cinética"), "J", NAN }, V_M, V_V } },
    { N_("Energía"), N_("Energía potencial"), "Ep = m*g*h", "E\xE2\x82\x9A = mgh", 4,
      { { "Ep", "E\xE2\x82\x9A", N_("energía potencial"), "J", NAN }, V_M, V_G, V_H } },
    { N_("Energía"), N_("Energía elástica"), "Ee = k*x^2/2", "E\xE2\x82\x91 = \xC2\xBD" "kx\xC2\xB2", 3,
      { { "Ee", "E\xE2\x82\x91", N_("energía elástica"), "J", NAN }, V_K, V_X } },
    { N_("Energía"), N_("Trabajo"), "W = F*d*cos(th*pi/180)", "W = F\xC2\xB7" "d\xC2\xB7" "cos \xCE\xB8", 4,
      { { "W", "W", N_("trabajo"), "J", NAN }, V_F, V_D, { "th", "\xCE\xB8", N_("ángulo"), "\xC2\xB0", NAN } } },
    { N_("Energía"), N_("Potencia"), "P = W/t", "P = W/t", 3,
      { { "P", "P", N_("potencia"), "W", NAN }, { "W", "W", N_("trabajo"), "J", NAN }, V_T } },
    /* circular motion and gravitation */
    { N_("Circular y gravitación"), N_("Aceleración centrípeta"), "ac = v^2/r", "a\xE2\x82\x99 = v\xC2\xB2/r", 3,
      { { "ac", "a\xE2\x82\x99", N_("aceleración centrípeta"), "m/s\xC2\xB2", NAN }, V_V, V_R } },
    { N_("Circular y gravitación"), N_("Velocidad angular"), "v = om*r", "v = \xCF\x89\xC2\xB7r", 3,
      { V_V, { "om", "\xCF\x89", N_("velocidad angular"), "rad/s", NAN }, V_R } },
    { N_("Circular y gravitación"), N_("Período"), "T = 2*pi/om", "T = 2\xCF\x80/\xCF\x89", 2,
      { { "T", "T", N_("período"), "s", NAN }, { "om", "\xCF\x89", N_("velocidad angular"), "rad/s", NAN } } },
    { N_("Circular y gravitación"), N_("Gravitación universal"), "F = G*m1*m2/r^2", "F = G\xC2\xB7m\xE2\x82\x81m\xE2\x82\x82/r\xC2\xB2", 5,
      { V_F, { "G", "G", N_("constante de gravitación"), "N\xC2\xB7m\xC2\xB2/kg\xC2\xB2", 6.67430e-11 },
        { "m1", "m\xE2\x82\x81", N_("masa 1"), "kg", NAN }, { "m2", "m\xE2\x82\x82", N_("masa 2"), "kg", NAN }, V_R } },
    { N_("Circular y gravitación"), N_("Péndulo simple"), "T = 2*pi*sqrt(L/g)", "T = 2\xCF\x80\xE2\x88\x9A(L/g)", 3,
      { { "T", "T", N_("período"), "s", NAN }, { "L", "L", N_("largo"), "m", NAN }, V_G } },
    /* fluids */
    { N_("Fluidos"), N_("Densidad"), "rho = m/V", "\xCF\x81 = m/V", 3, { V_RHO, V_M, V_VOL } },
    { N_("Fluidos"), N_("Presión"), "p = F/A", "p = F/A", 3,
      { { "p", "p", N_("presión"), "Pa", NAN }, V_F, { "A", "A", N_("área"), "m\xC2\xB2", NAN } } },
    { N_("Fluidos"), N_("Presión hidrostática"), "p = rho*g*h", "p = \xCF\x81gh", 4,
      { { "p", "p", N_("presión"), "Pa", NAN }, V_RHO, V_G, { "h", "h", N_("profundidad"), "m", NAN } } },
    { N_("Fluidos"), N_("Empuje (Arquímedes)"), "E = rho*g*V", "E = \xCF\x81gV", 4,
      { { "E", "E", N_("empuje"), "N", NAN }, V_RHO, V_G, V_VOL } },
    { N_("Fluidos"), N_("Caudal"), "Q = A*v", "Q = A\xC2\xB7v", 3,
      { { "Q", "Q", N_("caudal"), "m\xC2\xB3/s", NAN }, { "A", "A", N_("sección"), "m\xC2\xB2", NAN }, V_V } },
    /* waves */
    { N_("Ondas"), N_("Velocidad de una onda"), "v = lam*f", "v = \xCE\xBB\xC2\xB7" "f", 3,
      { V_V, { "lam", "\xCE\xBB", N_("longitud de onda"), "m", NAN }, { "f", "f", N_("frecuencia"), "Hz", NAN } } },
    { N_("Ondas"), N_("Frecuencia y período"), "f = 1/T", "f = 1/T", 2,
      { { "f", "f", N_("frecuencia"), "Hz", NAN }, { "T", "T", N_("período"), "s", NAN } } },
    /* electricity */
    { N_("Electricidad"), N_("Ley de Ohm"), "V = I*R", "V = I\xC2\xB7R", 3,
      { { "V", "V", N_("tensión"), "V", NAN }, { "I", "I", N_("corriente"), "A", NAN }, { "R", "R", N_("resistencia"), "\xCE\xA9", NAN } } },
    { N_("Electricidad"), N_("Potencia eléctrica"), "P = V*I", "P = V\xC2\xB7I", 3,
      { { "P", "P", N_("potencia"), "W", NAN }, { "V", "V", N_("tensión"), "V", NAN }, { "I", "I", N_("corriente"), "A", NAN } } },
    { N_("Electricidad"), N_("Ley de Coulomb"), "F = k*q1*q2/r^2", "F = k\xC2\xB7q\xE2\x82\x81q\xE2\x82\x82/r\xC2\xB2", 5,
      { V_F, { "k", "k", N_("constante de Coulomb"), "N\xC2\xB7m\xC2\xB2/C\xC2\xB2", 8.9875517923e9 },
        { "q1", "q\xE2\x82\x81", N_("carga 1"), "C", NAN }, { "q2", "q\xE2\x82\x82", N_("carga 2"), "C", NAN }, V_R } },
    { N_("Electricidad"), N_("Resistencias en paralelo"), "1/Rt = 1/R1 + 1/R2", "1/R\xE2\x82\x9C = 1/R\xE2\x82\x81 + 1/R\xE2\x82\x82", 3,
      { { "Rt", "R\xE2\x82\x9C", N_("resistencia total"), "\xCE\xA9", NAN }, { "R1", "R\xE2\x82\x81", N_("resistencia 1"), "\xCE\xA9", NAN },
        { "R2", "R\xE2\x82\x82", N_("resistencia 2"), "\xCE\xA9", NAN } } },
    { N_("Electricidad"), N_("Capacitor"), "Q = C*V", "Q = C\xC2\xB7V", 3,
      { { "Q", "Q", N_("carga"), "C", NAN }, { "C", "C", N_("capacidad"), "F", NAN }, { "V", "V", N_("tensión"), "V", NAN } } },
    { N_("Electricidad"), N_("Energía en un capacitor"), "U = C*V^2/2", "U = \xC2\xBD" "CV\xC2\xB2", 3,
      { { "U", "U", N_("energía"), "J", NAN }, { "C", "C", N_("capacidad"), "F", NAN }, { "V", "V", N_("tensión"), "V", NAN } } },
    /* heat */
    { N_("Calor"), N_("Calor sensible"), "Q = m*c*dT", "Q = m\xC2\xB7" "c\xC2\xB7\xCE\x94T", 4,
      { { "Q", "Q", N_("calor"), "J", NAN }, V_M, { "c", "c", N_("calor específico"), "J/(kg\xC2\xB7K)", NAN },
        { "dT", "\xCE\x94T", N_("cambio de temperatura"), "K", NAN } } },
    { N_("Calor"), N_("Gas ideal"), "P*V = n*R*T", "PV = nRT", 5,
      { { "P", "P", N_("presión"), "Pa", NAN }, V_VOL, { "n", "n", N_("moles"), "mol", NAN },
        { "R", "R", N_("constante de los gases"), "J/(mol\xC2\xB7K)", 8.314462618 }, { "T", "T", N_("temperatura"), "K", NAN } } },
    { N_("Calor"), N_("Dilatación lineal"), "dL = alpha*L0*dT", "\xCE\x94L = \xCE\xB1\xC2\xB7L\xE2\x82\x80\xC2\xB7\xCE\x94T", 4,
      { { "dL", "\xCE\x94L", N_("cambio de largo"), "m", NAN }, { "alpha", "\xCE\xB1", N_("coeficiente de dilatación"), "1/K", NAN },
        { "L0", "L\xE2\x82\x80", N_("largo inicial"), "m", NAN }, { "dT", "\xCE\x94T", N_("cambio de temperatura"), "K", NAN } } },
    /* optics */
    { N_("Óptica"), N_("Ley de Snell"), "n1*sin(th1*pi/180) = n2*sin(th2*pi/180)", "n\xE2\x82\x81 sen \xCE\xB8\xE2\x82\x81 = n\xE2\x82\x82 sen \xCE\xB8\xE2\x82\x82", 4,
      { { "n1", "n\xE2\x82\x81", N_("índice 1"), "", NAN }, { "th1", "\xCE\xB8\xE2\x82\x81", N_("ángulo de incidencia"), "\xC2\xB0", NAN },
        { "n2", "n\xE2\x82\x82", N_("índice 2"), "", NAN }, { "th2", "\xCE\xB8\xE2\x82\x82", N_("ángulo de refracción"), "\xC2\xB0", NAN } } },
    { N_("Óptica"), N_("Lente delgada"), "1/f = 1/do + 1/di", "1/f = 1/s + 1/s\xE2\x80\xB2", 3,
      { { "f", "f", N_("distancia focal"), "m", NAN }, { "do", "s", N_("distancia al objeto"), "m", NAN },
        { "di", "s\xE2\x80\xB2", N_("distancia a la imagen"), "m", NAN } } },
    /* modern */
    { N_("Moderna"), N_("Masa y energía"), "E = m*c^2", "E = mc\xC2\xB2", 3,
      { { "E", "E", N_("energía"), "J", NAN }, V_M, { "c", "c", N_("velocidad de la luz"), "m/s", 299792458.0 } } },
    { N_("Moderna"), N_("Energía de un fotón"), "E = h*f", "E = h\xC2\xB7" "f", 3,
      { { "E", "E", N_("energía"), "J", NAN }, { "h", "h", N_("constante de Planck"), "J\xC2\xB7s", 6.62607015e-34 },
        { "f", "f", N_("frecuencia"), "Hz", NAN } } },
};
const int PHYS_NF = sizeof PHYS_F / sizeof PHYS_F[0];

const phys_const_t PHYS_C[] = {
    { "c", N_("velocidad de la luz"), 299792458.0, "m/s", true },
    { "h", N_("constante de Planck"), 6.62607015e-34, "J\xC2\xB7s", true },
    { "\xC4\xA7", N_("constante de Planck reducida"), 1.054571817e-34, "J\xC2\xB7s", false },
    { "e", N_("carga elemental"), 1.602176634e-19, "C", true },
    { "N\xE2\x82\x90", N_("número de Avogadro"), 6.02214076e23, "1/mol", true },
    { "kB", N_("constante de Boltzmann"), 1.380649e-23, "J/K", true },
    { "R", N_("constante de los gases"), 8.314462618, "J/(mol\xC2\xB7K)", false },
    { "R", N_("constante de los gases"), 0.082057366, "L\xC2\xB7" "atm/(mol\xC2\xB7K)", false },
    { "F", N_("constante de Faraday"), 96485.33212, "C/mol", false },
    { "G", N_("constante de gravitación"), 6.67430e-11, "N\xC2\xB7m\xC2\xB2/kg\xC2\xB2", false },
    { "g", N_("gravedad estándar"), 9.80665, "m/s\xC2\xB2", true },
    { "m\xE2\x82\x91", N_("masa del electrón"), 9.1093837015e-31, "kg", false },
    { "m\xE2\x82\x9A", N_("masa del protón"), 1.67262192369e-27, "kg", false },
    { "m\xE2\x82\x99", N_("masa del neutrón"), 1.67492749804e-27, "kg", false },
    { "u", N_("unidad de masa atómica"), 1.66053906660e-27, "kg", false },
    { "\xCE\xB5\xE2\x82\x80", N_("permitividad del vacío"), 8.8541878128e-12, "F/m", false },
    { "\xCE\xBC\xE2\x82\x80", N_("permeabilidad del vacío"), 1.25663706212e-6, "N/A\xC2\xB2", false },
    { "k", N_("constante de Coulomb"), 8.9875517923e9, "N\xC2\xB7m\xC2\xB2/C\xC2\xB2", false },
    { "\xCF\x83", N_("constante de Stefan-Boltzmann"), 5.670374419e-8, "W/(m\xC2\xB2\xC2\xB7K\xE2\x81\xB4)", false },
    { "a\xE2\x82\x80", N_("radio de Bohr"), 5.29177210903e-11, "m", false },
    { "V\xE2\x82\x98", N_("volumen molar (0 °C, 1 atm)"), 22.41396954, "L/mol", false },
    { "atm", N_("atmósfera estándar"), 101325.0, "Pa", true },
    { "eV", N_("electronvoltio"), 1.602176634e-19, "J", true },
    { "M\xE2\x82\x9C", N_("masa de la Tierra"), 5.9722e24, "kg", false },
    { "R\xE2\x82\x9C", N_("radio medio de la Tierra"), 6.371e6, "m", false },
};
const int PHYS_NC = sizeof PHYS_C / sizeof PHYS_C[0];

/* --- solving ------------------------------------------------------------------- */

/* one bracket [a, b]: its root by bisection, or NAN (no sign change, or a
 * pole, where the sign changes too but |f| is anything but small) */
static double bracket(phys_fn_t fn, void *user, double a, double b, double fa, double fb)
{
    if (!isfinite(fa) || !isfinite(fb)) return NAN;
    if (fa == 0) return a;
    if ((fa < 0) == (fb < 0)) return NAN;
    double fa0 = fa, fb0 = fb;
    for (int k = 0; k < 200; k++) {
        double m = (a + b) / 2;
        if (m == a || m == b) break;
        double fm = fn(user, m);
        if (!isfinite(fm)) return NAN;
        if ((fm < 0) == (fa < 0)) {
            a = m;
            fa = fm;
        } else {
            b = m;
            fb = fm;
        }
    }
    double r = fabs(fa) < fabs(fb) ? a : b;
    double fr_ = fn(user, r);
    if (!isfinite(fr_) || fabs(fr_) > 1e-6 * (1 + fabs(fa0) + fabs(fb0))) return NAN;
    return r;
}

int phys_roots(phys_fn_t fn, void *user, double *out, int max)
{
    /* brackets from zero outwards, both sides in turn, on a grid of
     * 10^k for k from -45 to 45 in steps of 0.05: the roots come out
     * smallest first, which is what matters when an angle has infinitely
     * many */
    enum { N = 1801 };
    double f0 = fn(user, 0);
    double xp = pow(10.0, -45), fp = fn(user, xp), fm = fn(user, -xp);
    int found = 0;
    for (int i = 0; i <= N && found < max; i++) {
        double r[2];
        if (i == 0) {
            r[0] = bracket(fn, user, 0, xp, f0, fp);
            r[1] = bracket(fn, user, -xp, 0, fm, f0);
        } else {
            double x2 = pow(10.0, -45 + i * 0.05);
            double fp2 = fn(user, x2), fm2 = fn(user, -x2);
            r[0] = bracket(fn, user, xp, x2, fp, fp2);
            r[1] = bracket(fn, user, -x2, -xp, fm2, fm);
            xp = x2;
            fp = fp2;
            fm = fm2;
        }
        for (int k = 0; k < 2 && found < max; k++) {
            if (!isfinite(r[k])) continue;
            bool dup = false;
            for (int j = 0; j < found; j++) dup |= fabs(out[j] - r[k]) <= 1e-9 * (fabs(r[k]) + 1e-300);
            if (!dup) out[found++] = r[k];
        }
    }
    return found;
}

typedef struct {
    mx_prog_t prog;
} solve_t;

static double eval_fn(void *user, double x)
{
    return mx_eval_d(&((solve_t *)user)->prog, x);
}

/* the equation as text, with values for names, x for the unknown */
static bool substitute(const phys_formula_t *f, const double *val, int unknown, mx_str_t *b)
{
    const char *eq = strchr(f->eq, '=');
    if (!eq) return false;
    mx_str_add(b, "(");
    for (const char *s = f->eq; *s; ) {
        if (s == eq) {
            mx_str_add(b, ")-(");
            s++;
            continue;
        }
        if (isalpha((unsigned char)*s) || *s == '_') {
            const char *e = s;
            while (isalnum((unsigned char)*e) || *e == '_') e++;
            int k;
            for (k = 0; k < f->nv; k++) {
                if (strlen(f->v[k].id) == (size_t)(e - s) && !strncmp(f->v[k].id, s, (size_t)(e - s))) break;
            }
            if (k == f->nv) {
                mx_str_addn(b, s, (size_t)(e - s));             /* sin, sqrt, pi */
            } else if (k == unknown) {
                mx_str_add(b, "x");
            } else {
                if (!isfinite(val[k])) return false;
                char num[40];
                snprintf(num, sizeof num, "(%.17g)", val[k]);
                mx_str_add(b, num);
            }
            s = e;
            continue;
        }
        mx_str_addc(b, *s++);
    }
    mx_str_add(b, ")");
    return true;
}

int phys_solve(const phys_formula_t *f, const double *val, int unknown, double *root, double *other)
{
    *root = *other = NAN;
    mx_str_t b = { 0 };
    if (!substitute(f, val, unknown, &b)) {
        mx_str_free(&b);
        return 0;
    }
    mx_ast_t *t = mx_parse(b.s, NULL);
    mx_str_free(&b);
    solve_t s;
    memset(&s, 0, sizeof s);
    if (!t || !mx_compile(t, "x", &s.prog)) {
        mx_ast_free(t);
        return 0;
    }
    mx_ast_free(t);
    double r[8];
    int n = phys_roots(eval_fn, &s, r, 8);
    mx_prog_free(&s.prog);
    if (!n) return 0;
    /* the smallest positive first: lengths, times and masses are */
    int best = -1;
    for (int k = 0; k < n; k++) {
        if (r[k] > 0 && (best < 0 || r[k] < r[best])) best = k;
    }
    if (best < 0) {
        best = 0;
        for (int k = 1; k < n; k++) {
            if (fabs(r[k]) < fabs(r[best])) best = k;
        }
    }
    *root = r[best];
    int alt = -1;
    for (int k = 0; k < n; k++) {
        if (k != best && (alt < 0 || fabs(r[k]) < fabs(r[alt]))) alt = k;
    }
    if (alt >= 0) *other = r[alt];
    return alt >= 0 ? 2 : 1;
}
