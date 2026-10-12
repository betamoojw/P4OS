/*
 * P4OS - CAS: finance.
 *
 *   TVM        the five of time value of money (N, I%, PV, PMT, FV), as the
 *              financial calculators: fill four, "=?" on the fifth.
 *   Loan       French, German or American amortisation: the payment, the
 *              total interest, a chart of each payment's interest and
 *              principal, and the whole table.
 *   Rates      a rate as TNA (with its compounding period in days), TEA or
 *              TEM, turned into all of them and the daily one; a fixed-term
 *              deposit; the real rate against inflation.
 *   NPV / IRR  cash flows, a discount rate: NPV, IRR and the payback.
 *
 * As statistics, all of it computed here as it is typed (cas_num.c), and
 * kept in data/cas_fin.txt.
 */
#include "cas_app.h"
#include "cas_form.h"
#include "cas_num.h"
#include "cas_plot.h"
#include "mx.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCF 60

static struct {
    bool       init;
    int        page;
    /* TVM */
    num_tvm_t  tvm;
    lv_obj_t  *tvm_f[5];
    /* loan */
    double     amount, tna, months;
    int        system;
    /* rates */
    int        rtype;
    double     rate, rdays, capital, days, inflation;
    /* flows */
    double     disc, cf[NCF];
    int        ncf;
    /* widgets */
    lv_obj_t  *root, *body;
    lv_obj_t  *res[10];
    lv_obj_t  *table, *plotbox;
    cas_plot_t plot;
    bool       landscape;
} F;

static void build_page(void);
static void save(void);

/* --- model and keeping it ------------------------------------------------------------- */

static void model_init(void)
{
    if (F.init) return;
    F.init = true;
    F.tvm = (num_tvm_t){ 360, 6, 200000, 0, 0, 12, 12, false };
    F.amount = 1000000;
    F.tna = 40;
    F.months = 12;
    F.rate = 40;
    F.rdays = 30;
    F.capital = 100000;
    F.days = 30;
    F.inflation = 2;
    F.disc = 10;
    F.ncf = 4;
    F.cf[0] = -1000;
    F.cf[1] = 300;
    F.cf[2] = 400;
    F.cf[3] = 500;
    char path[200];
    snprintf(path, sizeof path, "%s/cas_fin.txt", aos_hal_path_data());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128], key[32];
    double v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s %lf", key, &v) != 2) continue;
        if (!strcmp(key, "n")) F.tvm.n = v;
        else if (!strcmp(key, "iy")) F.tvm.iy = v;
        else if (!strcmp(key, "pv")) F.tvm.pv = v;
        else if (!strcmp(key, "pmt")) F.tvm.pmt = v;
        else if (!strcmp(key, "fv")) F.tvm.fv = v;
        else if (!strcmp(key, "py")) F.tvm.py = v;
        else if (!strcmp(key, "cy")) F.tvm.cy = v;
        else if (!strcmp(key, "begin")) F.tvm.begin = v != 0;
        else if (!strcmp(key, "amount")) F.amount = v;
        else if (!strcmp(key, "tna")) F.tna = v;
        else if (!strcmp(key, "months")) F.months = v;
        else if (!strcmp(key, "system")) F.system = (int)v;
        else if (!strcmp(key, "rtype")) F.rtype = (int)v;
        else if (!strcmp(key, "rate")) F.rate = v;
        else if (!strcmp(key, "rdays")) F.rdays = v;
        else if (!strcmp(key, "capital")) F.capital = v;
        else if (!strcmp(key, "days")) F.days = v;
        else if (!strcmp(key, "inflation")) F.inflation = v;
        else if (!strcmp(key, "disc")) F.disc = v;
        else if (!strcmp(key, "ncf")) F.ncf = (int)v > NCF ? NCF : (int)v;
        else if (key[0] == 'c' && key[1] == 'f') {
            int i = atoi(key + 2);
            if (i >= 0 && i < NCF) F.cf[i] = v;
        }
    }
    fclose(f);
}

static void save(void)
{
    char path[200];
    snprintf(path, sizeof path, "%s/cas_fin.txt", aos_hal_path_data());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "n %.17g\niy %.17g\npv %.17g\npmt %.17g\nfv %.17g\npy %.17g\ncy %.17g\nbegin %d\n",
            F.tvm.n, F.tvm.iy, F.tvm.pv, F.tvm.pmt, F.tvm.fv, F.tvm.py, F.tvm.cy, F.tvm.begin ? 1 : 0);
    fprintf(f, "amount %.17g\ntna %.17g\nmonths %.17g\nsystem %d\n", F.amount, F.tna, F.months, F.system);
    fprintf(f, "rtype %d\nrate %.17g\nrdays %.17g\ncapital %.17g\ndays %.17g\ninflation %.17g\n",
            F.rtype, F.rate, F.rdays, F.capital, F.days, F.inflation);
    fprintf(f, "disc %.17g\nncf %d\n", F.disc, F.ncf);
    for (int i = 0; i < F.ncf; i++) fprintf(f, "cf%d %.17g\n", i, F.cf[i]);
    fclose(f);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user, bool accent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 52);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, accent ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_center(l);
    return b;
}

static void note(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
}

/* --- TVM ------------------------------------------------------------------------------------ */

static void tvm_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
}

static void tvm_solve_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (!num_tvm_solve(&F.tvm, which)) {
        aos_ui_toast(_("No tiene solución con esos valores"), 1800);
        return;
    }
    for (int i = 0; i < 5; i++) {
        if (F.tvm_f[i]) cas_field_refresh(F.tvm_f[i]);
    }
    save();
}

static void tvm_when(void *user, int sel)
{
    (void)user;
    F.tvm.begin = sel == 1;
    save();
}

static void page_tvm(lv_obj_t *body)
{
    lv_obj_t *card = cas_card(body, NULL);
    double *vals[5] = { &F.tvm.n, &F.tvm.iy, &F.tvm.pv, &F.tvm.pmt, &F.tvm.fv };
    static const char *const LBL[5] = { N_("N (períodos)"), N_("I% anual"), N_("VA (valor actual)"),
                                        N_("PAGO (por período)"), N_("VF (valor final)") };
    for (int i = 0; i < 5; i++) {
        F.tvm_f[i] = cas_field(card, _(LBL[i]), vals[i], i, tvm_changed, NULL);
        cas_field_solve_button(F.tvm_f[i], tvm_solve_cb, (void *)(intptr_t)i);
    }
    lv_obj_t *c2 = cas_card(body, NULL);
    cas_field(c2, _("pagos por año"), &F.tvm.py, 5, tvm_changed, NULL);
    cas_field(c2, _("capitalizaciones por año"), &F.tvm.cy, 6, tvm_changed, NULL);
    static const char *const WN[] = { N_("Pago al final"), N_("Pago al inicio") };
    const char *wn[2] = { _(WN[0]), _(WN[1]) };
    cas_seg(c2, wn, 2, F.tvm.begin ? 1 : 0, tvm_when, NULL);
    note(body, _("Completá cuatro y tocá =? en la que falta. El dinero que recibís va positivo, el que pagás negativo: un préstamo de 200 000 se paga con cuotas negativas."));
}

/* --- Loan ---------------------------------------------------------------------------------- */

static void loan_fill(void);

static void loan_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
    loan_fill();
}

static void loan_system(void *user, int sel)
{
    (void)user;
    F.system = sel;
    save();
    loan_fill();
}

static void loan_draw(int n, double r)
{
    cas_plot_t *p = &F.plot;
    if (!p->db || n <= 0) return;
    num_loan_row_t row;
    double top = 0;
    for (int k = 1; k <= n; k++) {
        num_loan_row((num_loan_kind_t)F.system, F.amount, r, n, k, &row);
        if (row.payment > top) top = row.payment;
    }
    p->x0 = 0.3;
    p->x1 = n + 0.7;
    p->y0 = -top * 0.06;
    p->y1 = top * 1.12;
    cas_plot_clear(p, 0x000000);
    cas_plot_grid(p);
    float bw = (float)p->W / (n + 0.4f) * 0.7f;
    if (bw < 1) bw = 1;
    for (int k = 1; k <= n; k++) {
        num_loan_row((num_loan_kind_t)F.system, F.amount, r, n, k, &row);
        float x = cas_plot_sx(p, k);
        float y0 = cas_plot_sy(p, 0), yi = cas_plot_sy(p, row.interest), yt = cas_plot_sy(p, row.payment);
        cas_plot_rect(p, x - bw / 2, yi, x + bw / 2, y0, 0xFF453A, 210);       /* interest  */
        cas_plot_rect(p, x - bw / 2, yt, x + bw / 2, yi, 0x0A84FF, 210);       /* principal */
    }
    lv_layer_t layer;
    lv_canvas_init_layer(p->canvas, &layer);
    cas_plot_numbers(p, &layer);
    cas_plot_text(&layer, 12, 8, _("interés"), 0xFF453A, 0);
    cas_plot_text(&layer, 110, 8, _("capital"), 0x0A84FF, 0);
    lv_canvas_finish_layer(p->canvas, &layer);
    cas_plot_done(p);
}

static void loan_fill(void)
{
    if (!F.res[0]) return;
    int n = (int)F.months;
    if (n < 1) n = 1;
    if (n > 600) n = 600;
    double r = F.tna / 100 / 12;
    num_loan_row_t first, last;
    num_loan_row((num_loan_kind_t)F.system, F.amount, r, n, 1, &first);
    num_loan_row((num_loan_kind_t)F.system, F.amount, r, n, n, &last);
    double ti = num_loan_total_interest((num_loan_kind_t)F.system, F.amount, r, n);
    char b[64];
    cas_fmt_money(b, sizeof b, first.payment);
    lv_label_set_text(F.res[0], b);
    cas_fmt_money(b, sizeof b, last.payment);
    lv_label_set_text(F.res[1], b);
    cas_fmt_money(b, sizeof b, ti);
    lv_label_set_text(F.res[2], b);
    cas_fmt_money(b, sizeof b, F.amount + ti);
    lv_label_set_text(F.res[3], b);
    loan_draw(n, r);
    if (F.table) {
        lv_table_set_row_count(F.table, (uint32_t)n + 1);
        for (int k = 1; k <= n; k++) {
            num_loan_row_t row;
            num_loan_row((num_loan_kind_t)F.system, F.amount, r, n, k, &row);
            snprintf(b, sizeof b, "%d", k);
            lv_table_set_cell_value(F.table, (uint32_t)k, 0, b);
            cas_fmt_money(b, sizeof b, row.payment);
            lv_table_set_cell_value(F.table, (uint32_t)k, 1, b);
            cas_fmt_money(b, sizeof b, row.interest);
            lv_table_set_cell_value(F.table, (uint32_t)k, 2, b);
            cas_fmt_money(b, sizeof b, row.balance);
            lv_table_set_cell_value(F.table, (uint32_t)k, 3, b);
        }
    }
}

static void page_loan(lv_obj_t *body)
{
    static const char *const SN[] = { N_("Francés"), N_("Alemán"), N_("Americano") };
    const char *sn[3] = { _(SN[0]), _(SN[1]), _(SN[2]) };
    cas_seg(body, sn, 3, F.system, loan_system, NULL);
    lv_obj_t *card = cas_card(body, NULL);
    cas_field(card, _("Monto"), &F.amount, 0, loan_changed, NULL);
    cas_field(card, _("TNA %"), &F.tna, 1, loan_changed, NULL);
    cas_field(card, _("Plazo (meses)"), &F.months, 2, loan_changed, NULL);
    lv_obj_t *r = cas_card(body, NULL);
    cas_result(r, F.system == LOAN_GERMAN ? _("Primera cuota") : _("Cuota"), &F.res[0]);
    cas_result(r, _("Última cuota"), &F.res[1]);
    cas_result(r, _("Total de intereses"), &F.res[2]);
    cas_result(r, _("Total a pagar"), &F.res[3]);
    F.plotbox = lv_obj_create(body);
    lv_obj_remove_style_all(F.plotbox);
    lv_obj_set_size(F.plotbox, LV_PCT(100), F.landscape ? 280 : 320);
    lv_obj_set_style_radius(F.plotbox, 16, 0);
    lv_obj_set_style_clip_corner(F.plotbox, true, 0);
    lv_obj_remove_flag(F.plotbox, LV_OBJ_FLAG_SCROLLABLE);
    F.table = lv_table_create(body);
    lv_obj_set_width(F.table, LV_PCT(100));
    lv_obj_set_height(F.table, LV_SIZE_CONTENT);
    lv_table_set_column_count(F.table, 4);
    lv_obj_update_layout(body);
    int w = lv_obj_get_content_width(body);
    lv_table_set_column_width(F.table, 0, 70);
    for (int c = 1; c < 4; c++) lv_table_set_column_width(F.table, (uint32_t)c, (w - 74) / 3);
    lv_table_set_cell_value(F.table, 0, 0, "#");
    lv_table_set_cell_value(F.table, 0, 1, _("Cuota"));
    lv_table_set_cell_value(F.table, 0, 2, _("Interés"));
    lv_table_set_cell_value(F.table, 0, 3, _("Saldo"));
    lv_obj_set_style_text_font(F.table, mx_font(20), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(F.table, AOS_C_CARD, LV_PART_ITEMS);
    lv_obj_set_style_border_color(F.table, AOS_C_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(F.table, AOS_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(F.table, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(F.table, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_align(F.table, LV_TEXT_ALIGN_RIGHT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(F.table, AOS_C_BG, 0);
    lv_obj_set_style_border_width(F.table, 0, 0);
    lv_obj_remove_flag(F.table, LV_OBJ_FLAG_SCROLLABLE);
    cas_plot_create(&F.plot, F.plotbox, lv_obj_get_width(F.plotbox), lv_obj_get_height(F.plotbox));
    note(body, _("La tasa del período es la TNA dividida por 12. Sin IVA ni gastos: el costo financiero total del banco puede ser mayor."));
    loan_fill();
}

/* --- Rates ------------------------------------------------------------------------------------ */

static void rates_fill(void)
{
    if (!F.res[0]) return;
    double d = F.rdays > 0 ? F.rdays : 30;
    double tea;
    switch (F.rtype) {
    case 0:  tea = pow(1 + F.rate / 100 * d / 365, 365 / d) - 1; break;     /* TNA, cap. every d days */
    case 1:  tea = F.rate / 100; break;                                       /* TEA */
    default: tea = pow(1 + F.rate / 100, 365.0 / 30) - 1; break;              /* TEM */
    }
    double tna = (pow(1 + tea, d / 365) - 1) * 365 / d;
    double tem = pow(1 + tea, 30.0 / 365) - 1;
    double ted = pow(1 + tea, 1.0 / 365) - 1;
    char b[64];
    double show[4] = { tna, tea, tem, ted };
    for (int i = 0; i < 4; i++) {
        cas_fmt(b, sizeof b, show[i] * 100, 8);
        strncat(b, " %", sizeof b - strlen(b) - 1);
        lv_label_set_text(F.res[i], b);
    }
    /* a fixed-term deposit: simple interest at the TNA, as banks quote it */
    double interest = F.capital * tna * F.days / 365;
    cas_fmt_money(b, sizeof b, interest);
    lv_label_set_text(F.res[4], b);
    cas_fmt_money(b, sizeof b, F.capital + interest);
    lv_label_set_text(F.res[5], b);
    double real = (1 + tem) / (1 + F.inflation / 100) - 1;
    cas_fmt(b, sizeof b, real * 100, 6);
    strncat(b, " %", sizeof b - strlen(b) - 1);
    lv_label_set_text(F.res[6], b);
}

static void rates_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
    rates_fill();
}

static void rates_type(void *user, int sel)
{
    (void)user;
    F.rtype = sel;
    save();
    build_page();
}

static void page_rates(lv_obj_t *body)
{
    static const char *const TN[] = { "TNA", "TEA", "TEM" };
    cas_seg(body, TN, 3, F.rtype, rates_type, NULL);
    lv_obj_t *c = cas_card(body, NULL);
    cas_field(c, _("Tasa %"), &F.rate, 0, rates_changed, NULL);
    if (F.rtype == 0) {
        cas_field(c, _("capitaliza cada (días)"), &F.rdays, 1, rates_changed, NULL);
    }
    lv_obj_t *r = cas_card(body, _("Equivalentes"));
    char lbl[48];
    snprintf(lbl, sizeof lbl, _("TNA (cada %.0f días)"), F.rdays > 0 ? F.rdays : 30);
    cas_result(r, lbl, &F.res[0]);
    cas_result(r, "TEA", &F.res[1]);
    cas_result(r, _("TEM (30 días)"), &F.res[2]);
    cas_result(r, _("Diaria efectiva"), &F.res[3]);
    lv_obj_t *pf = cas_card(body, _("Plazo fijo"));
    cas_field(pf, _("Capital"), &F.capital, 2, rates_changed, NULL);
    cas_field(pf, _("Días"), &F.days, 3, rates_changed, NULL);
    cas_result(pf, _("Interés"), &F.res[4]);
    cas_result(pf, _("Monto al vencer"), &F.res[5]);
    lv_obj_t *in = cas_card(body, _("Contra la inflación"));
    cas_field(in, _("Inflación mensual %"), &F.inflation, 4, rates_changed, NULL);
    cas_result(in, _("Tasa real mensual"), &F.res[6]);
    rates_fill();
}

/* --- NPV / IRR ----------------------------------------------------------------------------------- */

static void flows_fill(void)
{
    if (!F.res[0]) return;
    char b[64];
    cas_fmt_money(b, sizeof b, num_npv(F.disc / 100, F.cf, F.ncf));
    lv_label_set_text(F.res[0], b);
    double irr;
    if (num_irr(F.cf, F.ncf, &irr)) {
        cas_fmt(b, sizeof b, irr * 100, 8);
        strncat(b, " %", sizeof b - strlen(b) - 1);
    } else {
        snprintf(b, sizeof b, "%s", _("no hay"));
    }
    lv_label_set_text(F.res[1], b);
    double pb = num_payback(F.cf, F.ncf);
    if (pb >= 0) cas_fmt(b, sizeof b, pb, 4); else snprintf(b, sizeof b, "%s", _("nunca"));
    lv_label_set_text(F.res[2], b);
}

static void flows_changed(void *user, int id)
{
    (void)user;
    (void)id;
    save();
    flows_fill();
}

static void flow_add(lv_event_t *e)
{
    (void)e;
    if (F.ncf < NCF) {
        F.cf[F.ncf] = F.ncf ? F.cf[F.ncf - 1] : 0;
        F.ncf++;
        save();
        build_page();
    }
}

static void flow_del(lv_event_t *e)
{
    (void)e;
    if (F.ncf > 1) {
        F.ncf--;
        save();
        build_page();
    }
}

static void page_flows(lv_obj_t *body)
{
    lv_obj_t *c = cas_card(body, NULL);
    cas_field(c, _("Tasa de descuento %"), &F.disc, 100, flows_changed, NULL);
    lv_obj_t *r = cas_card(body, NULL);
    cas_result(r, _("VAN"), &F.res[0]);
    cas_result(r, _("TIR"), &F.res[1]);
    cas_result(r, _("Recupero (períodos)"), &F.res[2]);
    lv_obj_t *fl = cas_card(body, _("Flujos"));
    for (int i = 0; i < F.ncf; i++) {
        char lbl[24];
        snprintf(lbl, sizeof lbl, i ? _("período %d") : _("inversión (0)"), i);
        cas_field(fl, lbl, &F.cf[i], i, flows_changed, NULL);
    }
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    button(bar, _("+ período"), flow_add, NULL, true);
    button(bar, _("Quitar el último"), flow_del, NULL, false);
    flows_fill();
}

/* --- the view ------------------------------------------------------------------------------------- */

static void page_choose(void *user, int sel)
{
    (void)user;
    F.page = sel;
    build_page();
}

static void build_page(void)
{
    if (!F.root) return;
    cas_entry_close();
    cas_plot_free(&F.plot);
    lv_obj_clean(F.root);
    memset(F.res, 0, sizeof F.res);
    memset(F.tvm_f, 0, sizeof F.tvm_f);
    F.table = F.plotbox = NULL;
    static const char *const PG[] = { "TVM", N_("Préstamo"), N_("Tasas"), N_("VAN / TIR") };
    const char *pg[4];
    for (int i = 0; i < 4; i++) pg[i] = _(PG[i]);
    cas_seg(F.root, pg, 4, F.page, page_choose, NULL);
    F.body = lv_obj_create(F.root);
    lv_obj_remove_style_all(F.body);
    lv_obj_set_width(F.body, LV_PCT(100));
    lv_obj_set_flex_grow(F.body, 1);
    lv_obj_set_flex_flow(F.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(F.body, 10, 0);
    lv_obj_set_scroll_dir(F.body, LV_DIR_VER);
    lv_obj_update_layout(F.root);
    switch (F.page) {
    case 0: page_tvm(F.body); break;
    case 1: page_loan(F.body); break;
    case 2: page_rates(F.body); break;
    default: page_flows(F.body); break;
    }
}

void cas_fin_build(lv_obj_t *parent, bool landscape)
{
    model_init();
    F.landscape = landscape;
    F.root = lv_obj_create(parent);
    lv_obj_remove_style_all(F.root);
    lv_obj_set_size(F.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(F.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(F.root, 10, 0);
    lv_obj_remove_flag(F.root, LV_OBJ_FLAG_SCROLLABLE);
    build_page();
}

void cas_fin_hidden(void)
{
    cas_entry_close();
    cas_plot_free(&F.plot);
    F.root = F.body = F.table = F.plotbox = NULL;
    memset(F.res, 0, sizeof F.res);
    memset(F.tvm_f, 0, sizeof F.tvm_f);
}

void cas_fin_free(void)
{
    cas_fin_hidden();
    F.init = false;
}
