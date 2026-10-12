/*
 * P4OS - CAS: sciences.
 *
 * Chemistry (the periodic table, molar mass, balancing with stoichiometry,
 * gases, solutions and pH), physics (a library of formulas that solve for
 * the missing variable, a projectile drawn, the constants) and biology
 * (Punnett squares, Hardy-Weinberg, a DNA sequence, growth and dilutions).
 * The models are cas_chem.c, cas_phys.c and cas_bio.c; this is the view.
 *
 * A relation between a few quantities (PV = nRT, c = m/(M·V), every physics
 * formula) is one "formula card": a field per variable and "=?" on each,
 * solved by cas_phys.c, so chemistry's gases and solutions work the same
 * way as physics.
 */
#include "cas_app.h"
#include "cas_bio.h"
#include "cas_chem.h"
#include "cas_form.h"
#include "cas_grid.h"
#include "cas_num.h"
#include "cas_phys.h"
#include "cas_plot.h"
#include "cas_save.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "mx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { A_CHEM, A_PHYS, A_BIO, A_COUNT };

/* chemistry's relations, in the units of the bench */
static const phys_formula_t F_GAS = {
    "", N_("Gas ideal"), "P*V = n*R*T", "PV = nRT", 5,
    { { "P", "P", N_("presión"), "atm", NAN }, { "V", "V", N_("volumen"), "L", NAN },
      { "n", "n", N_("moles"), "mol", NAN }, { "R", "R", N_("constante de los gases"), "L\xC2\xB7" "atm/(mol\xC2\xB7K)", 0.082057366 },
      { "T", "T", N_("temperatura"), "K", NAN } } };
static const phys_formula_t F_MOLAR = {
    "", N_("Molaridad"), "c = m/(M*V)", "c = m/(M\xC2\xB7V)", 4,
    { { "c", "c", N_("concentración"), "mol/L", NAN }, { "m", "m", N_("masa de soluto"), "g", NAN },
      { "M", "M", N_("masa molar"), "g/mol", NAN }, { "V", "V", N_("volumen de solución"), "L", NAN } } };
static const phys_formula_t F_DIL = {
    "", N_("Dilución"), "C1*V1 = C2*V2", "C\xE2\x82\x81V\xE2\x82\x81 = C\xE2\x82\x82V\xE2\x82\x82", 4,
    { { "C1", "C\xE2\x82\x81", N_("concentración inicial"), "mol/L", NAN }, { "V1", "V\xE2\x82\x81", N_("volumen inicial"), "L", NAN },
      { "C2", "C\xE2\x82\x82", N_("concentración final"), "mol/L", NAN }, { "V2", "V\xE2\x82\x82", N_("volumen final"), "L", NAN } } };

static const uint32_t FAMILY_COLOR[CHEM_FAMILIES] = {
    0xFF6B6B, 0xFFA94D, 0xFCC419, 0x94D82D, 0x38D9A9, 0x4DABF7, 0x748FFC, 0xDA77F2, 0xF783AC, 0xC0A0F0,
};
static const char *const FAMILY_NAME[CHEM_FAMILIES] = {
    N_("Metal alcalino"), N_("Alcalinotérreo"), N_("Metal de transición"), N_("Otro metal"), N_("Metaloide"),
    N_("No metal"), N_("Halógeno"), N_("Gas noble"), N_("Lantánido"), N_("Actínido"),
};

#define NCARD 3

typedef struct {
    const phys_formula_t *f;
    double               *v;
    lv_obj_t             *field[PHYS_MAXV];
} fcard_t;

static struct {
    bool        init, landscape;
    int         area, page[A_COUNT];
    lv_obj_t   *root, *body, *res, *res2, *grid, *table, *detail;
    cas_plot_t  plot;
    fcard_t     card[NCARD];
    /* chemistry */
    int         el;                     /* selected element */
    char        formula[64];
    double      mm_g, mm_mol;
    char        eqtext[160];
    chem_eq_t   eq;
    bool        eq_ok;
    char        eq_err[128];
    int         st_ref;                 /* stoichiometry: the species weighed */
    double      st_g;
    double      gas[5], molar[4], dil[4];
    int         ph_kind;
    double      ph_c, ph_k;
    /* physics */
    int         pf;                     /* the formula open, -1 the list */
    double      pv[PHYS_MAXV];
    double      pj[4];                  /* projectile: v0, angle, h0, g */
    /* biology */
    char        p1[16], p2[16];
    bio_cross_t cross;
    bool        cross_ok;
    char        cross_err[96];
    double      hw[3], q2;
    char        dna[1024];
    int         frame;
    double      gr[4];                  /* N0, r, K, t */
    double      sd[3];                  /* serial dilution: C0, factor, steps */
} C;

static void build_page(void);
static void save(void);
static void fill(void);

/* rebuilt after the event of a button that goes with it */
static void rebuild_async(void *u)
{
    (void)u;
    build_page();
}

/* --- kept between openings --------------------------------------------------------- */

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/cas_sci.txt", aos_hal_path_data());
}

static void nan_fill(double *v, int n)
{
    for (int i = 0; i < n; i++) v[i] = NAN;
}

static void preset(const phys_formula_t *f, double *v)
{
    for (int k = 0; k < f->nv; k++) v[k] = f->v[k].preset;
}

static void model_init(void)
{
    if (C.init) return;
    C.init = true;
    C.el = 6;
    snprintf(C.formula, sizeof C.formula, "H2SO4");
    C.mm_g = 10;
    snprintf(C.eqtext, sizeof C.eqtext, "C3H8 + O2 = CO2 + H2O");
    C.st_g = 44;
    preset(&F_GAS, C.gas);
    C.gas[0] = 1;
    C.gas[2] = 1;
    C.gas[4] = 273.15;
    nan_fill(C.molar, 4);
    nan_fill(C.dil, 4);
    C.ph_c = 0.1;
    C.ph_k = 1.8e-5;
    C.pf = -1;
    C.pj[0] = 20;
    C.pj[1] = 45;
    C.pj[2] = 0;
    C.pj[3] = 9.80665;
    snprintf(C.p1, sizeof C.p1, "AaBb");
    snprintf(C.p2, sizeof C.p2, "AaBb");
    C.hw[0] = 360;
    C.hw[1] = 480;
    C.hw[2] = 160;
    C.q2 = 0.04;
    snprintf(C.dna, sizeof C.dna, "ATGGCCATTGTAATGGGCCGCTGAAAGGGTGCCCGATAG");
    C.gr[0] = 10;
    C.gr[1] = 0.5;
    C.gr[2] = 1000;
    C.gr[3] = 10;
    C.sd[0] = 1;
    C.sd[1] = 10;
    C.sd[2] = 5;

    char path[200];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1100], key[24];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        int used = 0;
        if (sscanf(line, "%23s %n", key, &used) < 1) continue;
        const char *val = line + used;
        double v = atof(val);
        int idx = -1;
        char *us = strchr(key, '_');
        if (us) idx = atoi(us + 1);
        if (!strcmp(key, "area")) C.area = (int)v % A_COUNT;
        else if (!strncmp(key, "page_", 5) && idx >= 0 && idx < A_COUNT) C.page[idx] = (int)v;
        else if (!strcmp(key, "el")) C.el = v >= 1 && v <= 118 ? (int)v : 6;
        else if (!strcmp(key, "formula")) snprintf(C.formula, sizeof C.formula, "%s", val);
        else if (!strcmp(key, "mm_g")) C.mm_g = v;
        else if (!strcmp(key, "eq")) snprintf(C.eqtext, sizeof C.eqtext, "%s", val);
        else if (!strcmp(key, "st_ref")) C.st_ref = (int)v;
        else if (!strcmp(key, "st_g")) C.st_g = v;
        else if (!strncmp(key, "gas_", 4) && idx >= 0 && idx < 5) C.gas[idx] = v;
        else if (!strncmp(key, "molar_", 6) && idx >= 0 && idx < 4) C.molar[idx] = v;
        else if (!strncmp(key, "dil_", 4) && idx >= 0 && idx < 4) C.dil[idx] = v;
        else if (!strcmp(key, "ph_kind")) C.ph_kind = (int)v % 4;
        else if (!strcmp(key, "ph_c")) C.ph_c = v;
        else if (!strcmp(key, "ph_k")) C.ph_k = v;
        else if (!strncmp(key, "pj_", 3) && idx >= 0 && idx < 4) C.pj[idx] = v;
        else if (!strcmp(key, "pf")) C.pf = v >= 0 && v < PHYS_NF ? (int)v : -1;
        else if (!strncmp(key, "pv_", 3) && idx >= 0 && idx < PHYS_MAXV) C.pv[idx] = v;
        else if (!strcmp(key, "p1")) snprintf(C.p1, sizeof C.p1, "%s", val);
        else if (!strcmp(key, "p2")) snprintf(C.p2, sizeof C.p2, "%s", val);
        else if (!strncmp(key, "hw_", 3) && idx >= 0 && idx < 3) C.hw[idx] = v;
        else if (!strcmp(key, "q2")) C.q2 = v;
        else if (!strcmp(key, "dna")) snprintf(C.dna, sizeof C.dna, "%s", val);
        else if (!strcmp(key, "frame")) C.frame = (int)v % 4;
        else if (!strncmp(key, "gr_", 3) && idx >= 0 && idx < 4) C.gr[idx] = v;
        else if (!strncmp(key, "sd_", 3) && idx >= 0 && idx < 3) C.sd[idx] = v;
    }
    fclose(f);
}

static void save(void)
{
    if (!C.init) return;
    char path[200];
    state_path(path, sizeof path);
    mx_str_t b = { 0 };
    cas_str_printf(&b, "area %d\n", C.area);
    for (int a = 0; a < A_COUNT; a++) cas_str_printf(&b, "page_%d %d\n", a, C.page[a]);
    cas_str_printf(&b, "el %d\nformula %s\nmm_g %.17g\neq %s\nst_ref %d\nst_g %.17g\n",
                   C.el, C.formula, C.mm_g, C.eqtext, C.st_ref, C.st_g);
    for (int i = 0; i < 5; i++) cas_str_printf(&b, "gas_%d %.17g\n", i, C.gas[i]);
    for (int i = 0; i < 4; i++) cas_str_printf(&b, "molar_%d %.17g\ndil_%d %.17g\n", i, C.molar[i], i, C.dil[i]);
    cas_str_printf(&b, "ph_kind %d\nph_c %.17g\nph_k %.17g\n", C.ph_kind, C.ph_c, C.ph_k);
    for (int i = 0; i < 4; i++) cas_str_printf(&b, "pj_%d %.17g\n", i, C.pj[i]);
    cas_str_printf(&b, "pf %d\n", C.pf);
    for (int i = 0; i < PHYS_MAXV; i++) cas_str_printf(&b, "pv_%d %.17g\n", i, C.pv[i]);
    cas_str_printf(&b, "p1 %s\np2 %s\n", C.p1, C.p2);
    for (int i = 0; i < 3; i++) cas_str_printf(&b, "hw_%d %.17g\n", i, C.hw[i]);
    cas_str_printf(&b, "q2 %.17g\nframe %d\n", C.q2, C.frame);
    mx_str_add(&b, "dna ");
    mx_str_add(&b, C.dna);
    mx_str_addc(&b, '\n');
    for (int i = 0; i < 4; i++) cas_str_printf(&b, "gr_%d %.17g\n", i, C.gr[i]);
    for (int i = 0; i < 3; i++) cas_str_printf(&b, "sd_%d %.17g\n", i, C.sd[i]);
    cas_save_text(path, &b);
}

/* --- small pieces --------------------------------------------------------------------- */

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    return l;
}

static void note(lv_obj_t *parent, const char *text)
{
    label(parent, text, aos_font_small, AOS_C_DIM);
}

static void set_value(lv_obj_t *v, double x, const char *unit)
{
    char b[64], n[48];
    cas_fmt(n, sizeof n, x, 8);
    if (unit && unit[0]) snprintf(b, sizeof b, "%s %s", n, unit);
    else snprintf(b, sizeof b, "%s", n);
    if (v) lv_label_set_text(v, b);
}

static lv_obj_t *result(lv_obj_t *card, const char *name, double x, const char *unit)
{
    lv_obj_t *v;
    cas_result(card, name, &v);
    set_value(v, x, unit);
    return v;
}

static lv_obj_t *result_text(lv_obj_t *card, const char *name, const char *text)
{
    lv_obj_t *v;
    cas_result(card, name, &v);
    lv_label_set_text(v, text);
    return v;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 50);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_center(l);
    return b;
}

/* a row with a label and a text to tap: the text sheet edits it */
typedef void (*text_set_t)(const char *text);

static void text_row_click(lv_event_t *e);

static lv_obj_t *text_row(lv_obj_t *parent, const char *name, const char *shown, int which)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 64, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_add_event_cb(row, text_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)which);
    lv_obj_t *n = label(row, name, aos_font_small, AOS_C_DIM);
    (void)n;
    label(row, shown[0] ? shown : _("tocá para escribir"), mx_font(28), AOS_C_TEXT);
    return row;
}

/* --- the formula card ----------------------------------------------------------------- */

static void card_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
    fill();
}

static void card_solve(lv_event_t *e)
{
    int u = (int)(intptr_t)lv_event_get_user_data(e);
    fcard_t *c = &C.card[u >> 8];
    int k = u & 0xFF;
    if (!c->f) return;
    for (int j = 0; j < c->f->nv; j++) {
        if (j != k && !isfinite(c->v[j])) {
            char b[96];
            snprintf(b, sizeof b, _("Falta %s"), c->f->v[j].shown);
            aos_ui_toast(b, 1500);
            return;
        }
    }
    double r, o;
    int n = phys_solve(c->f, c->v, k, &r, &o);
    if (!n) {
        aos_ui_toast(_("No tiene solución con esos valores"), 1800);
        return;
    }
    c->v[k] = r;
    if (n == 2 && isfinite(o)) {
        char b[96], v[48];
        cas_fmt(v, sizeof v, o, 8);
        snprintf(b, sizeof b, _("También es solución: %s = %s"), c->f->v[k].shown, v);
        aos_ui_toast(b, 2600);
    }
    for (int j = 0; j < c->f->nv; j++) {
        if (c->field[j]) cas_field_refresh(c->field[j]);
    }
    save();
    fill();
}

static lv_obj_t *formula_card(lv_obj_t *parent, int slot, const phys_formula_t *f, double *v, bool title)
{
    fcard_t *c = &C.card[slot];
    memset(c, 0, sizeof *c);
    c->f = f;
    c->v = v;
    lv_obj_t *card = cas_card(parent, title ? _(f->name) : NULL);
    label(card, f->shown, mx_font(32), lv_color_hex(0x64D2FF));
    for (int k = 0; k < f->nv; k++) {
        char lab[96];
        const phys_var_t *pv = &f->v[k];
        if (pv->unit[0]) snprintf(lab, sizeof lab, "%s  %s (%s)", pv->shown, _(pv->name), pv->unit);
        else snprintf(lab, sizeof lab, "%s  %s", pv->shown, _(pv->name));
        c->field[k] = cas_field(card, lab, &v[k], k, card_changed, NULL);
        cas_field_solve_button(c->field[k], card_solve, (void *)(intptr_t)(slot << 8 | k));
    }
    return card;
}

/* --- chemistry: the periodic table ---------------------------------------------------------- */

static void table_geom(lv_obj_t *o, float *cw, float *ch)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *cw = lv_area_get_width(&a) / 18.0f;
    *ch = lv_area_get_height(&a) / 10.0f;
}

static void table_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float cw, ch;
    table_geom(o, &cw, &ch);
    const lv_font_t *f = mx_font(20);
    int lh = lv_font_get_line_height(f);
    for (int z = 1; z <= 118; z++) {
        int r, c;
        chem_pos(z, &r, &c);
        float y = r * ch;          /* row 7 stays empty: the f-block apart */
        lv_area_t cell = { a.x1 + (int)(c * cw) + 1, a.y1 + (int)y + 1, a.x1 + (int)((c + 1) * cw) - 2, a.y1 + (int)(y + ch) - 2 };
        const chem_el_t *el = &CHEM_EL[z - 1];
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = lv_color_hex(FAMILY_COLOR[el->family]);
        rd.bg_opa = z == C.el ? LV_OPA_COVER : LV_OPA_40;
        rd.radius = 4;
        if (z == C.el) {
            rd.border_color = lv_color_white();
            rd.border_width = 2;
        }
        lv_draw_rect(layer, &rd, &cell);
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.text = el->sym;
        ld.font = f;
        ld.color = z == C.el ? lv_color_black() : lv_color_white();
        ld.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t t = { cell.x1, cell.y1 + (lv_area_get_height(&cell) - lh) / 2, cell.x2, cell.y1 + (lv_area_get_height(&cell) - lh) / 2 + lh };
        lv_draw_label(layer, &ld, &t);
    }
}

static void element_detail(void);

static void table_click(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float cw, ch;
    table_geom(o, &cw, &ch);
    float x = p.x - a.x1, y = p.y - a.y1;
    int c = (int)(x / cw);
    for (int z = 1; z <= 118; z++) {
        int r, cc;
        chem_pos(z, &r, &cc);
        float top = r * ch;
        if (cc == c && y >= top && y < top + ch) {
            C.el = z;
            lv_obj_invalidate(o);
            element_detail();
            save();
            return;
        }
    }
}

static void to_formula_cb(lv_event_t *e)
{
    (void)e;
    size_t l = strlen(C.formula);
    snprintf(C.formula + l, sizeof C.formula - l, "%s", CHEM_EL[C.el - 1].sym);
    C.page[A_CHEM] = 1;
    save();
    lv_async_call(rebuild_async, NULL);
}

static void element_detail(void)
{
    if (!C.detail) return;
    lv_obj_clean(C.detail);
    const chem_el_t *el = &CHEM_EL[C.el - 1];
    char b[160], v[64];

    lv_obj_t *head = lv_obj_create(C.detail);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 16, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sym = lv_label_create(head);
    lv_label_set_text(sym, el->sym);
    lv_obj_set_style_text_font(sym, mx_font(48), 0);
    lv_obj_set_style_text_color(sym, lv_color_hex(FAMILY_COLOR[el->family]), 0);
    lv_obj_t *col = lv_obj_create(head);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    snprintf(b, sizeof b, "%s", _(el->name));
    label(col, b, aos_font_body, AOS_C_TEXT);
    snprintf(b, sizeof b, "%s \xC2\xB7 Z = %d", _(FAMILY_NAME[el->family]), C.el);
    label(col, b, aos_font_small, AOS_C_DIM);

    if (el->radio) snprintf(v, sizeof v, "[%d] g/mol", (int)el->mass);
    else {
        cas_fmt(v, sizeof v, el->mass, 6);
        strncat(v, " g/mol", sizeof v - strlen(v) - 1);
    }
    result_text(C.detail, _("Masa atómica"), v);
    result_text(C.detail, _("Configuración"), el->config);
    int g = chem_group(C.el);
    if (g) snprintf(b, sizeof b, "%d \xC2\xB7 %d", g, chem_period(C.el));
    else snprintf(b, sizeof b, "\xE2\x80\x94 \xC2\xB7 %d", chem_period(C.el));
    result_text(C.detail, _("Grupo · período"), b);
    static const char *const ST[] = { N_("sólido"), N_("líquido"), N_("gas") };
    snprintf(b, sizeof b, "%s%s", _(ST[el->state & 3]), el->state & 4 ? _(" (previsto)") : "");
    result_text(C.detail, _("Estado a 25 °C"), b);
    if (isfinite(el->en)) {
        cas_fmt(v, sizeof v, el->en, 3);
        result_text(C.detail, _("Electronegatividad"), v);
    }
    if (isfinite(el->melt)) {
        char m[32], bo[32] = "\xE2\x80\x94";
        cas_fmt(m, sizeof m, el->melt - 273.15, 5);
        if (isfinite(el->boil)) cas_fmt(bo, sizeof bo, el->boil - 273.15, 5);
        snprintf(b, sizeof b, "%s \xC2\xB0" "C \xC2\xB7 %s \xC2\xB0" "C", m, bo);
        result_text(C.detail, _("Fusión · ebullición"), b);
    }
    if (isfinite(el->density)) {
        cas_fmt(v, sizeof v, el->density, 5);
        strncat(v, " g/cm\xC2\xB3", sizeof v - strlen(v) - 1);
        result_text(C.detail, _("Densidad"), v);
    }
    if (el->ox[0]) result_text(C.detail, _("Estados de oxidación"), el->ox);
    lv_obj_t *bar = lv_obj_create(C.detail);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    snprintf(b, sizeof b, _("Sumar %s a la fórmula"), el->sym);
    small_button(bar, b, to_formula_cb, NULL);
}

static void page_table(lv_obj_t *body)
{
    lv_obj_update_layout(body);
    int w = lv_obj_get_width(body), th = (int)(w / 18.0f * 1.25f * 10);
    /* lying down the width would make cells taller than the screen: the
     * table takes the height there is, its cells wider than tall */
    if (C.landscape && th > lv_obj_get_height(body)) th = lv_obj_get_height(body);
    C.table = lv_obj_create(body);
    lv_obj_remove_style_all(C.table);
    lv_obj_set_size(C.table, w, th);
    lv_obj_add_flag(C.table, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(C.table, table_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(C.table, table_click, LV_EVENT_CLICKED, NULL);
    C.detail = cas_card(body, NULL);
    element_detail();
}

/* --- chemistry: molar mass ----------------------------------------------------------------- */

static void mm_changed(void *user, int id)
{
    (void)user;
    chem_comp_t c;
    char err[8];
    if (chem_formula(C.formula, &c, err, sizeof err) && c.mass > 0) {
        if (id == 0) C.mm_mol = C.mm_g / c.mass;
        else C.mm_g = C.mm_mol * c.mass;
    }
    save();
    build_page();
}

static void page_molar(lv_obj_t *body)
{
    char pretty[128];
    chem_pretty(C.formula, pretty, sizeof pretty);
    text_row(body, _("Fórmula (Ca(OH)2, CuSO4.5H2O, SO4^2-)"), pretty, 0);
    chem_comp_t c;
    char err[96];
    if (!chem_formula(C.formula, &c, err, sizeof err)) {
        label(body, err, aos_font_small, AOS_C_RED);
        return;
    }
    lv_obj_t *card = cas_card(body, NULL);
    char v[64];
    cas_fmt(v, sizeof v, c.mass, 7);
    strncat(v, " g/mol", sizeof v - strlen(v) - 1);
    lv_obj_t *m = label(card, v, mx_font(48), AOS_C_TEXT);
    (void)m;
    note(card, _("masa molar"));
    for (int i = 0; i < c.n; i++) {
        const chem_el_t *el = &CHEM_EL[c.z[i] - 1];
        char name[64], val[64], cnt[24], part[24], pct[24];
        cas_fmt(cnt, sizeof cnt, c.count[i], 6);
        snprintf(name, sizeof name, "%s \xC3\x97 %s", el->sym, cnt);
        cas_fmt(part, sizeof part, c.count[i] * el->mass, 6);
        cas_fmt(pct, sizeof pct, 100 * c.count[i] * el->mass / c.mass, 4);
        snprintf(val, sizeof val, "%s g \xC2\xB7 %s %%", part, pct);
        result_text(card, name, val);
    }
    C.mm_mol = C.mm_g / c.mass;
    lv_obj_t *conv = cas_card(body, _("Gramos y moles"));
    cas_field(conv, _("masa (g)"), &C.mm_g, 0, mm_changed, NULL);
    cas_field(conv, _("cantidad (mol)"), &C.mm_mol, 1, mm_changed, NULL);
    char n[48];
    cas_fmt(n, sizeof n, C.mm_mol * 6.02214076e23, 6);
    result_text(conv, _("partículas"), n);
}

/* --- chemistry: balancing and stoichiometry ------------------------------------------------- */

static void balance(void)
{
    C.eq_ok = chem_balance(C.eqtext, &C.eq, C.eq_err, sizeof C.eq_err) == CHEM_BAL_OK;
    if (C.st_ref >= C.eq.n) C.st_ref = 0;
}

static void st_text(void *user, int row, int col, char *buf, size_t n, uint32_t *color)
{
    (void)user;
    if (row < 0) {
        static const char *const H[] = { N_("Sustancia"), N_("Coef."), "g/mol", "mol", "g" };
        snprintf(buf, n, "%s", _(H[col]));
        return;
    }
    if (row >= C.eq.n) return;
    double m = C.eq.comp[C.st_ref].mass;
    double ref_mol = m > 0 ? C.st_g / m / (double)C.eq.coef[C.st_ref] : NAN;
    double mol = ref_mol * (double)C.eq.coef[row];
    switch (col) {
    case 0: chem_pretty(C.eq.sp[row], buf, n); break;
    case 1: snprintf(buf, n, "%ld", C.eq.coef[row]); break;
    case 2: cas_fmt(buf, n, C.eq.comp[row].mass, 6); break;
    case 3: cas_fmt(buf, n, mol, 5); break;
    default: cas_fmt(buf, n, mol * C.eq.comp[row].mass, 5); break;
    }
    if (row == C.st_ref) *color = 0xFFD60A;
}

static bool st_entered(void *user, const char *text, double v)
{
    (void)user;
    (void)text;
    C.st_g = v;
    save();
    if (C.grid) cas_grid_refresh(C.grid);
    return false;
}

static void st_tap(void *user, int row, int col)
{
    (void)user;
    (void)col;
    if (row < 0 || row >= C.eq.n) return;
    C.st_ref = row;
    if (C.grid) cas_grid_refresh(C.grid);
    char t[96], p[64];
    chem_pretty(C.eq.sp[row], p, sizeof p);
    snprintf(t, sizeof t, _("Gramos de %s"), p);
    cas_entry_open(t, C.st_g, true, st_entered, NULL);
}

static void page_balance(lv_obj_t *body)
{
    text_row(body, _("Ecuación (Fe + O2 = Fe2O3; iones: Fe^3+)"), C.eqtext, 1);
    balance();
    if (!C.eq_ok) {
        label(body, C.eq_err, aos_font_small, AOS_C_RED);
        return;
    }
    char b[512];
    chem_eq_text(&C.eq, b, sizeof b);
    lv_obj_t *card = cas_card(body, NULL);
    label(card, b, mx_font(32), AOS_C_TEXT);
    note(body, _("Estequiometría: tocá una sustancia para escribir cuántos gramos hay; el resto sale de ahí."));
    static const uint8_t W[] = { 4, 2, 3, 3, 3 };
    C.grid = cas_grid_create(body, 5, W, 52, st_text, NULL);
    lv_obj_set_size(C.grid, LV_PCT(100), 52 * (C.eq.n + 1) + 4);
    cas_grid_set_rows(C.grid, C.eq.n);
    cas_grid_on_tap(C.grid, st_tap, NULL);
}

/* --- chemistry: gases and solutions ----------------------------------------------------------- */

static void page_gas(lv_obj_t *body)
{
    formula_card(body, 0, &F_GAS, C.gas, false);
    note(body, _("Llená cuatro y tocá =? en el que falta. La temperatura va en kelvin: 0 °C son 273,15 K."));
    C.res = cas_card(body, NULL);
}

static void ph_kind_cb(void *user, int sel)
{
    (void)user;
    C.ph_kind = sel;
    save();
    build_page();
}

static void ph_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
    fill();
}

static void page_solutions(lv_obj_t *body)
{
    formula_card(body, 0, &F_MOLAR, C.molar, true);
    formula_card(body, 1, &F_DIL, C.dil, true);
    lv_obj_t *card = cas_card(body, "pH");
    static const char *const K[] = { N_("Ácido fuerte"), N_("Base fuerte"), N_("Ácido débil"), N_("Base débil") };
    const char *k[4];
    for (int i = 0; i < 4; i++) k[i] = _(K[i]);
    cas_seg(card, k, 4, C.ph_kind, ph_kind_cb, NULL);
    cas_field(card, _("concentración (mol/L)"), &C.ph_c, 0, ph_changed, NULL);
    if (C.ph_kind >= 2) cas_field(card, C.ph_kind == 2 ? "K\xE2\x82\x90" : "Kb", &C.ph_k, 1, ph_changed, NULL);
    C.res = card;
    C.res2 = lv_obj_create(card);
    lv_obj_remove_style_all(C.res2);
    lv_obj_set_size(C.res2, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(C.res2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(C.res2, 6, 0);
    note(body, _("A 25 °C (Kw = 10⁻¹⁴)."));
}

static void fill_ph(void)
{
    if (!C.res2) return;
    lv_obj_clean(C.res2);
    const double kw = 1e-14, c = C.ph_c, k = C.ph_k;
    if (!(c > 0) || (C.ph_kind >= 2 && !(k > 0))) return;
    double x;                   /* [H+] for an acid, [OH-] for a base */
    if (C.ph_kind < 2) x = (c + sqrt(c * c + 4 * kw)) / 2;
    else x = (-k + sqrt(k * k + 4 * k * c)) / 2;
    bool base = C.ph_kind == 1 || C.ph_kind == 3;
    double h = base ? kw / x : x, oh = kw / h;
    double ph = -log10(h);
    result(C.res2, "pH", ph, "");
    result(C.res2, "pOH", 14 - ph, "");
    result(C.res2, "[H\xE2\x81\xBA]", h, "mol/L");
    result(C.res2, "[OH\xE2\x81\xBB]", oh, "mol/L");
    if (C.ph_kind >= 2) result(C.res2, _("ionizado"), 100 * x / c, "%");
}

/* --- physics -------------------------------------------------------------------------------------- */

static void open_formula(lv_event_t *e)
{
    C.pf = (int)(intptr_t)lv_event_get_user_data(e);
    preset(&PHYS_F[C.pf], C.pv);
    save();
    lv_async_call(rebuild_async, NULL);
}

static void back_to_list(lv_event_t *e)
{
    (void)e;
    C.pf = -1;
    save();
    lv_async_call(rebuild_async, NULL);
}

static void page_formulas(lv_obj_t *body)
{
    if (C.pf >= 0 && C.pf < PHYS_NF) {
        lv_obj_t *bar = lv_obj_create(body);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        char b[96];
        snprintf(b, sizeof b, "%s  %s", LV_SYMBOL_LEFT, _("Fórmulas"));
        small_button(bar, b, back_to_list, NULL);
        formula_card(body, 0, &PHYS_F[C.pf], C.pv, true);
        note(body, _("Llená los datos y tocá =? en el que querés saber. Los ángulos van en grados."));
        return;
    }
    const char *group = NULL;
    lv_obj_t *card = NULL;
    for (int i = 0; i < PHYS_NF; i++) {
        const phys_formula_t *f = &PHYS_F[i];
        if (!group || strcmp(group, f->group)) {
            group = f->group;
            card = cas_card(body, _(group));
        }
        lv_obj_t *row = lv_button_create(card);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, open_formula, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *n = lv_label_create(row);
        lv_label_set_text(n, _(f->name));
        lv_obj_set_style_text_font(n, aos_font_small, 0);
        lv_obj_t *q = lv_label_create(row);
        lv_label_set_text(q, f->shown);
        lv_obj_set_style_text_font(q, aos_font_small, 0);
        lv_obj_set_style_text_color(q, lv_color_hex(0x64D2FF), 0);
    }
}

static void pj_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
    fill();
}

static void draw_projectile(double tf, double range, double hmax)
{
    cas_plot_t *p = &C.plot;
    if (!p->db) return;
    double v0 = C.pj[0], th = C.pj[1] * M_PI / 180, h0 = C.pj[2], g = C.pj[3];
    double vx = v0 * cos(th), vy = v0 * sin(th);
    /* the same scale on both axes: the shape as it is */
    double w = fmax(range, 1e-9) * 1.1, h = fmax(hmax, 1e-9) * 1.25;
    double s = fmin(p->W / w, p->H / h);
    p->x0 = -range * 0.05;
    p->x1 = p->x0 + p->W / s;
    p->y0 = -h * 0.1 + fmin(0, h0);
    p->y1 = p->y0 + p->H / s;
    cas_plot_clear(p, 0x000000);
    cas_plot_grid(p);
    float px = 0, py = 0;
    for (int k = 0; k <= 200; k++) {
        double t = tf * k / 200;
        float x = cas_plot_sx(p, vx * t), y = cas_plot_sy(p, h0 + vy * t - g * t * t / 2);
        if (k) cas_plot_line(p, px, py, x, y, 0xFF9F0A, 3.2f);
        px = x;
        py = y;
    }
    double tt = vy / g;
    if (tt > 0 && tt < tf) cas_plot_disc(p, cas_plot_sx(p, vx * tt), cas_plot_sy(p, hmax), 6, 0xFFD60A);
    cas_plot_disc(p, cas_plot_sx(p, range), cas_plot_sy(p, 0), 6, 0xFF453A);
    lv_layer_t layer;
    lv_canvas_init_layer(p->canvas, &layer);
    cas_plot_numbers(p, &layer);
    lv_canvas_finish_layer(p->canvas, &layer);
    cas_plot_done(p);
}

static void fill_projectile(void)
{
    if (!C.res) return;
    lv_obj_clean(C.res);
    double v0 = C.pj[0], th = C.pj[1] * M_PI / 180, h0 = C.pj[2], g = C.pj[3];
    if (!(v0 >= 0) || !(g > 0) || !isfinite(th) || !isfinite(h0)) return;
    double vx = v0 * cos(th), vy = v0 * sin(th);
    double disc = vy * vy + 2 * g * h0;
    if (disc < 0) {
        note(C.res, _("Con esa altura no llega al suelo."));
        return;
    }
    double tf = (vy + sqrt(disc)) / g, range = vx * tf;
    double hmax = vy > 0 ? h0 + vy * vy / (2 * g) : h0;
    double vyf = vy - g * tf, vf = hypot(vx, vyf);
    result(C.res, _("Tiempo de vuelo"), tf, "s");
    result(C.res, _("Alcance"), range, "m");
    result(C.res, _("Altura máxima"), hmax, "m");
    result(C.res, _("Velocidad al caer"), vf, "m/s");
    result(C.res, _("Ángulo al caer"), atan2(-vyf, vx) * 180 / M_PI, "\xC2\xB0");
    draw_projectile(tf, range, hmax);
}

static void page_projectile(lv_obj_t *body)
{
    lv_obj_t *card = cas_card(body, NULL);
    static const char *const L[] = { N_("v₀ velocidad inicial (m/s)"), N_("θ ángulo (°)"),
                                     N_("h₀ altura inicial (m)"), N_("g gravedad (m/s²)") };
    for (int i = 0; i < 4; i++) cas_field(card, _(L[i]), &C.pj[i], i, pj_changed, NULL);
    lv_obj_update_layout(body);
    int w = lv_obj_get_width(body);
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, C.landscape ? 300 : 380);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    cas_plot_create(&C.plot, box, w, C.landscape ? 300 : 380);
    C.res = cas_card(body, NULL);
}

static void page_constants(lv_obj_t *body)
{
    lv_obj_t *card = cas_card(body, NULL);
    for (int i = 0; i < PHYS_NC; i++) {
        const phys_const_t *k = &PHYS_C[i];
        char name[96], val[80], n[40];
        snprintf(name, sizeof name, "%s  %s", k->sym, _(k->name));
        cas_fmt(n, sizeof n, k->value, 11);
        snprintf(val, sizeof val, "%s %s", n, k->unit);
        result_text(card, name, val);
    }
    note(body, _("CODATA 2018. Desde 2019 c, h, e, Nₐ y kB son exactas por definición."));
}

/* --- biology ------------------------------------------------------------------------------------- */

static void cross(void)
{
    C.cross_ok = bio_cross(C.p1, C.p2, &C.cross, C.cross_err, sizeof C.cross_err);
}

static void pn_text(void *user, int row, int col, char *buf, size_t n, uint32_t *color)
{
    (void)user;
    const bio_cross_t *x = &C.cross;
    if (row < 0) {
        if (col == 0) snprintf(buf, n, "\xC3\x97");
        else snprintf(buf, n, "%s", x->g2[col - 1]);
        *color = 0x64D2FF;
        return;
    }
    if (col == 0) {
        snprintf(buf, n, "%s", x->g1[row]);
        *color = 0x64D2FF;
        return;
    }
    snprintf(buf, n, "%s", x->cell[row][col - 1]);
}

static void page_punnett(lv_obj_t *body)
{
    text_row(body, _("Padre 1"), C.p1, 2);
    text_row(body, _("Padre 2"), C.p2, 3);
    cross();
    if (!C.cross_ok) {
        label(body, C.cross_err, aos_font_small, AOS_C_RED);
        return;
    }
    const bio_cross_t *x = &C.cross;
    int cols = x->ng2 + 1;
    uint8_t W[BIO_GAM + 1];
    for (int i = 0; i < cols; i++) W[i] = 1;
    C.grid = cas_grid_create(body, cols, W, 50, pn_text, NULL);
    lv_obj_set_size(C.grid, LV_PCT(100), 50 * (x->ng1 + 1) + 4);
    cas_grid_set_rows(C.grid, x->ng1);

    lv_obj_t *card = cas_card(body, _("Fenotipos"));
    for (int i = 0; i < x->npheno; i++) {
        char v[48];
        snprintf(v, sizeof v, "%d/%d \xC2\xB7 %.4g %%", x->pheno_n[i], x->total, 100.0 * x->pheno_n[i] / x->total);
        result_text(card, x->pheno[i], v);
    }
    card = cas_card(body, _("Genotipos"));
    for (int i = 0; i < x->ngeno; i++) {
        char v[48];
        snprintf(v, sizeof v, "%d/%d \xC2\xB7 %.4g %%", x->geno_n[i], x->total, 100.0 * x->geno_n[i] / x->total);
        result_text(card, x->geno[i], v);
    }
    note(body, _("Mayúscula: el alelo dominante. A_ quiere decir AA o Aa."));
}

static void hw_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
    fill();
}

static void page_hardy(lv_obj_t *body)
{
    lv_obj_t *card = cas_card(body, _("Con los genotipos contados"));
    cas_field(card, "AA", &C.hw[0], 0, hw_changed, NULL);
    cas_field(card, "Aa", &C.hw[1], 1, hw_changed, NULL);
    cas_field(card, "aa", &C.hw[2], 2, hw_changed, NULL);
    C.res = cas_card(body, NULL);
    card = cas_card(body, _("Con la frecuencia del fenotipo recesivo"));
    cas_field(card, "q\xC2\xB2", &C.q2, 3, hw_changed, NULL);
    C.res2 = cas_card(body, NULL);
}

static void fill_hardy(void)
{
    bio_hw_t h;
    if (C.res) {
        lv_obj_clean(C.res);
        if (bio_hardy(C.hw[0], C.hw[1], C.hw[2], &h)) {
            result(C.res, "p", h.p, "");
            result(C.res, "q", h.q, "");
            char b[96], e0[24], e1[24], e2[24];
            cas_fmt(e0, sizeof e0, h.exp[0], 5);
            cas_fmt(e1, sizeof e1, h.exp[1], 5);
            cas_fmt(e2, sizeof e2, h.exp[2], 5);
            snprintf(b, sizeof b, "%s \xC2\xB7 %s \xC2\xB7 %s", e0, e1, e2);
            result_text(C.res, _("Esperados (AA, Aa, aa)"), b);
            if (h.chi2 < 1e-9) h.chi2 = 0;      /* exactly in equilibrium, but for rounding */
            result(C.res, "\xCF\x87\xC2\xB2", h.chi2, "");
            double par[2] = { 1, 0 };
            double pv = 1 - num_cdf(D_CHI2, par, h.chi2);
            result(C.res, _("valor p"), pv, "");
            lv_obj_t *l = label(C.res, pv < 0.05 ? _("No está en equilibrio (p < 0,05)") : _("Compatible con el equilibrio (p ≥ 0,05)"),
                                aos_font_small, lv_color_hex(0xFF9F0A));
            (void)l;
        }
    }
    if (C.res2) {
        lv_obj_clean(C.res2);
        if (C.q2 >= 0 && C.q2 <= 1) {
            double q = sqrt(C.q2), p = 1 - q;
            result(C.res2, "q", q, "");
            result(C.res2, "p", p, "");
            result(C.res2, _("AA (p²)"), p * p, "");
            result(C.res2, _("portadores Aa (2pq)"), 2 * p * q, "");
        }
    }
}

static void frame_cb(void *user, int sel)
{
    (void)user;
    C.frame = sel;
    save();
    build_page();
}

static void seq_card(lv_obj_t *body, const char *title, const char *seq)
{
    lv_obj_t *card = cas_card(body, title);
    label(card, seq[0] ? seq : "\xE2\x80\x94", aos_font_small, AOS_C_TEXT);
}

static void page_dna(lv_obj_t *body)
{
    static char clean[1024], out[1024];
    int bad;
    bio_clean(C.dna, clean, sizeof clean, &bad);
    text_row(body, _("Secuencia (ADN o ARN)"), clean, 4);
    if (bad) {
        char b[96];
        snprintf(b, sizeof b, _("Se dejaron afuera %d letras que no son bases."), bad);
        label(body, b, aos_font_small, lv_color_hex(0xFF9F0A));
    }
    bio_dna_t d;
    bio_dna(clean, &d);
    lv_obj_t *card = cas_card(body, NULL);
    char b[64];
    snprintf(b, sizeof b, "%d", d.len);
    result_text(card, _("Largo"), b);
    result(card, "GC", d.gc, "%");
    result(card, "Tm", d.tm, "\xC2\xB0" "C");
    result(card, _("Masa (hebra simple)"), d.mw, "g/mol");
    bio_complement(clean, out, sizeof out, false);
    seq_card(body, _("Complementaria"), out);
    bio_complement(clean, out, sizeof out, true);
    seq_card(body, _("Reversa complementaria"), out);
    bio_transcribe(clean, out, sizeof out);
    seq_card(body, _("ARN mensajero"), out);
    static const char *const FR[] = { N_("Marco 1"), N_("Marco 2"), N_("Marco 3"), N_("Desde ATG") };
    const char *fr[4];
    for (int i = 0; i < 4; i++) fr[i] = _(FR[i]);
    cas_seg(body, fr, 4, C.frame, frame_cb, NULL);
    bio_translate(clean, C.frame < 3 ? C.frame : 0, C.frame == 3, out, sizeof out);
    seq_card(body, _("Proteína (código estándar; * es stop)"), out);
    note(body, _("Tm: regla de Wallace hasta 13 bases, la fórmula de GC desde 14. Una estimación: no reemplaza a la de la sal y la concentración."));
}

static void gr_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
    fill();
}

static void draw_growth(void)
{
    cas_plot_t *p = &C.plot;
    if (!p->db) return;
    double n0 = C.gr[0], r = C.gr[1], k = C.gr[2], tmax = C.gr[3] * 2;
    if (!(tmax > 0)) return;
    double top = fmax(k, n0) * 1.25;
    double e = n0 * exp(r * C.gr[3]);
    if (isfinite(e) && e > top && e < 4 * top) top = e * 1.1;
    p->x0 = -tmax * 0.05;
    p->x1 = tmax * 1.05;
    p->y0 = -top * 0.06;
    p->y1 = top;
    cas_plot_clear(p, 0x000000);
    cas_plot_grid(p);
    float ex = 0, ey = 0, lx = 0, ly = 0;
    for (int s = 0; s <= 300; s++) {
        double t = tmax * s / 300;
        double ne = n0 * exp(r * t), nl = k / (1 + (k - n0) / n0 * exp(-r * t));
        float x = cas_plot_sx(p, t), ye = cas_plot_sy(p, fmin(ne, top * 10)), yl = cas_plot_sy(p, nl);
        if (s) {
            cas_plot_line(p, ex, ey, x, ye, 0xFF453A, 2.6f);
            cas_plot_line(p, lx, ly, x, yl, 0x30D158, 3.2f);
        }
        ex = lx = x;
        ey = ye;
        ly = yl;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(p->canvas, &layer);
    cas_plot_numbers(p, &layer);
    cas_plot_text(&layer, 12, 8, _("exponencial"), 0xFF453A, 0);
    cas_plot_text(&layer, 150, 8, _("logística"), 0x30D158, 0);
    lv_canvas_finish_layer(p->canvas, &layer);
    cas_plot_done(p);
}

static void fill_growth(void)
{
    if (C.res) {
        lv_obj_clean(C.res);
        double n0 = C.gr[0], r = C.gr[1], k = C.gr[2], t = C.gr[3];
        if (n0 > 0 && k > 0 && isfinite(r) && isfinite(t)) {
            result(C.res, _("Exponencial N(t)"), n0 * exp(r * t), "");
            result(C.res, _("Logística N(t)"), k / (1 + (k - n0) / n0 * exp(-r * t)), "");
            if (r > 0) result(C.res, _("Tiempo de duplicación"), log(2) / r, "");
            draw_growth();
        }
    }
    if (C.res2) {
        lv_obj_clean(C.res2);
        double c = C.sd[0], f = C.sd[1];
        int steps = (int)C.sd[2];
        if (f > 0 && steps >= 1 && steps <= 30) {
            for (int i = 1; i <= steps; i++) {
                char name[32];
                snprintf(name, sizeof name, _("Tubo %d"), i);
                result(C.res2, name, c / pow(f, i), "");
            }
            char b[48], fac[24];
            cas_fmt(fac, sizeof fac, pow(f, steps), 8);
            snprintf(b, sizeof b, "1 : %s", fac);
            result_text(C.res2, _("Dilución total"), b);
        }
    }
}

static void page_growth(lv_obj_t *body)
{
    lv_obj_t *card = cas_card(body, _("Crecimiento de una población"));
    static const char *const L[] = { N_("N₀ población inicial"), N_("r tasa de crecimiento"),
                                     N_("K capacidad de carga"), N_("t tiempo") };
    for (int i = 0; i < 4; i++) cas_field(card, _(L[i]), &C.gr[i], i, gr_changed, NULL);
    C.res = cas_card(body, NULL);
    lv_obj_update_layout(body);
    int w = lv_obj_get_width(body);
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, 300);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    cas_plot_create(&C.plot, box, w, 300);
    card = cas_card(body, _("Diluciones seriadas"));
    static const char *const D[] = { N_("concentración inicial"), N_("factor por paso"), N_("pasos") };
    for (int i = 0; i < 3; i++) cas_field(card, _(D[i]), &C.sd[i], 10 + i, gr_changed, NULL);
    C.res2 = cas_card(body, NULL);
}

/* --- text entry ------------------------------------------------------------------------------- */

static void text_entered(void *user, const char *text)
{
    switch ((int)(intptr_t)user) {
    case 0: snprintf(C.formula, sizeof C.formula, "%s", text); break;
    case 1: snprintf(C.eqtext, sizeof C.eqtext, "%s", text); break;
    case 2: snprintf(C.p1, sizeof C.p1, "%s", text); break;
    case 3: snprintf(C.p2, sizeof C.p2, "%s", text); break;
    default: snprintf(C.dna, sizeof C.dna, "%s", text); break;
    }
    save();
    build_page();
}

static void text_row_click(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    static const char *const T[] = { N_("Fórmula"), N_("Ecuación"), N_("Padre 1"), N_("Padre 2"), N_("Secuencia") };
    const char *cur = which == 0 ? C.formula : which == 1 ? C.eqtext : which == 2 ? C.p1 : which == 3 ? C.p2 : C.dna;
    cas_text_open(_(T[which]), cur, which == 4, text_entered, (void *)(intptr_t)which);
}

/* --- the view ----------------------------------------------------------------------------------- */

static void fill(void)
{
    if (C.area == A_CHEM && C.page[A_CHEM] == 3 && C.res) {
        lv_obj_clean(C.res);
        result(C.res, _("Temperatura"), C.gas[4] - 273.15, "\xC2\xB0" "C");
        result(C.res, _("Presión"), C.gas[0] * 101.325, "kPa");
        result(C.res, _("Presión"), C.gas[0] * 760, "mmHg");
    }
    if (C.area == A_CHEM && C.page[A_CHEM] == 4) fill_ph();
    if (C.area == A_PHYS && C.page[A_PHYS] == 1) fill_projectile();
    if (C.area == A_BIO && C.page[A_BIO] == 1) fill_hardy();
    if (C.area == A_BIO && C.page[A_BIO] == 3) fill_growth();
}

static void area_cb(void *user, int sel)
{
    (void)user;
    C.area = sel;
    save();
    build_page();
}

static void page_cb(void *user, int sel)
{
    (void)user;
    C.page[C.area] = sel;
    if (C.area == A_PHYS) C.pf = -1;
    save();
    build_page();
}

static void build_page(void)
{
    if (!C.root) return;
    cas_entry_close();
    cas_text_close();
    cas_plot_free(&C.plot);
    lv_obj_clean(C.root);
    C.body = C.res = C.res2 = C.grid = C.table = C.detail = NULL;
    memset(C.card, 0, sizeof C.card);

    static const char *const AR[] = { N_("Química"), N_("Física"), N_("Biología") };
    const char *ar[A_COUNT];
    for (int i = 0; i < A_COUNT; i++) ar[i] = _(AR[i]);
    cas_seg(C.root, ar, A_COUNT, C.area, area_cb, NULL);

    static const char *const PCHEM[] = { N_("Tabla"), N_("Masa molar"), N_("Balancear"), N_("Gases"), N_("Soluciones") };
    static const char *const PPHYS[] = { N_("Fórmulas"), N_("Tiro oblicuo"), N_("Constantes") };
    static const char *const PBIO[] = { N_("Punnett"), N_("Hardy-W."), N_("ADN"), N_("Crecimiento") };
    const char *const *P = C.area == A_CHEM ? PCHEM : C.area == A_PHYS ? PPHYS : PBIO;
    int np = C.area == A_CHEM ? 5 : C.area == A_PHYS ? 3 : 4;
    if (C.page[C.area] >= np) C.page[C.area] = 0;
    const char *pg[5];
    for (int i = 0; i < np; i++) pg[i] = _(P[i]);
    cas_seg(C.root, pg, np, C.page[C.area], page_cb, NULL);

    C.body = lv_obj_create(C.root);
    lv_obj_remove_style_all(C.body);
    lv_obj_set_width(C.body, LV_PCT(100));
    lv_obj_set_flex_grow(C.body, 1);
    lv_obj_set_flex_flow(C.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(C.body, 10, 0);
    lv_obj_set_scroll_dir(C.body, LV_DIR_VER);
    lv_obj_update_layout(C.root);

    int pgi = C.page[C.area];
    switch (C.area) {
    case A_CHEM:
        switch (pgi) {
        case 0: page_table(C.body); break;
        case 1: page_molar(C.body); break;
        case 2: page_balance(C.body); break;
        case 3: page_gas(C.body); break;
        default: page_solutions(C.body); break;
        }
        break;
    case A_PHYS:
        switch (pgi) {
        case 0: page_formulas(C.body); break;
        case 1: page_projectile(C.body); break;
        default: page_constants(C.body); break;
        }
        break;
    default:
        switch (pgi) {
        case 0: page_punnett(C.body); break;
        case 1: page_hardy(C.body); break;
        case 2: page_dna(C.body); break;
        default: page_growth(C.body); break;
        }
        break;
    }
    fill();
}

void cas_sci_build(lv_obj_t *parent, bool landscape)
{
    model_init();
    C.landscape = landscape;
    C.root = lv_obj_create(parent);
    lv_obj_remove_style_all(C.root);
    lv_obj_set_size(C.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(C.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(C.root, 10, 0);
    lv_obj_remove_flag(C.root, LV_OBJ_FLAG_SCROLLABLE);
    build_page();
}

void cas_sci_hidden(void)
{
    lv_async_call_cancel(rebuild_async, NULL);
    cas_text_close();
    cas_plot_free(&C.plot);
    C.root = C.body = C.res = C.res2 = C.grid = C.table = C.detail = NULL;
    memset(C.card, 0, sizeof C.card);
}

void cas_sci_free(void)
{
    cas_sci_hidden();
    if (C.init) save();
    C.init = false;
}
