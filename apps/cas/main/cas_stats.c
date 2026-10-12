/*
 * P4OS - CAS: statistics.
 *
 * Five pages under a choice at the top:
 *
 *   Data          four lists L1..L4 in a sheet; a tap on a cell types into it
 *                 and moves down, so a column fills without leaving the
 *                 keypad. Under it the summary of the chosen list.
 *   Chart         histogram or box plots of the chosen list, or L1 against
 *                 L2 with the regression on top.
 *   Regression    six models of L2 on L1, with r and r², and a button that
 *                 sends the curve to the grapher.
 *   Distributions normal, t, χ², F, binomial, Poisson, exponential:
 *                 P(a ≤ X ≤ b) or the inverse, with the area shaded.
 *   Tests         t for a mean, z for a proportion, Welch's t for two means:
 *                 the statistic, the p-value and the interval.
 *
 * Everything is computed here (cas_num.c), as it is typed: no engine. The
 * lists are kept in data/cas_stats.txt.
 */
#include "cas_app.h"
#include "cas_form.h"
#include "cas_grid.h"
#include "cas_num.h"
#include "cas_plot.h"
#include "cas_save.h"
#include "mx_view.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NL   4
#define MAXN 500

static const uint32_t LCOL[NL] = { 0x0A84FF, 0xFF9F0A, 0x30D158, 0xBF5AF2 };

static struct {
    double     *v[NL];          /* MAXN each, PSRAM                 */
    int         n[NL];
    bool        loaded;
    int         page, col, chart, bins;
    num_reg_kind_t rk;
    num_reg_t   reg;
    num_dist_t  dist;
    double      dpar[D_COUNT][2];
    int         dmode;          /* 0 P(a<=X<=b), 1 inverse          */
    double      a, b, p;
    int         test, tail;
    double      t_mean, t_s, t_n, t_mu0, t_conf;
    double      z_x, z_n, z_p0;
    double      w_m1, w_s1, w_n1, w_m2, w_s2, w_n2;
    /* widgets of the page on screen */
    lv_obj_t   *root, *body, *table, *summary, *plotbox, *results;
    cas_plot_t  plot;
    lv_obj_t   *res[8];
    lv_obj_t   *sum_title, *sum_hint, *sum_val[12];
    bool        landscape;
    int         erow, ecol;     /* the cell being typed into        */
} S;

static void build_page(void);
static void save(void);

/* --- the model --------------------------------------------------------------------- */

static void model_init(void)
{
    if (S.v[0]) {
        return;
    }
    for (int i = 0; i < NL; i++) {
        S.v[i] = mx_alloc(sizeof(double) * MAXN);
    }
    S.bins = 0;
    S.dpar[D_NORMAL][0] = 0;  S.dpar[D_NORMAL][1] = 1;
    S.dpar[D_T][0] = 10;
    S.dpar[D_CHI2][0] = 5;
    S.dpar[D_F][0] = 5;       S.dpar[D_F][1] = 10;
    S.dpar[D_BINOM][0] = 10;  S.dpar[D_BINOM][1] = 0.5;
    S.dpar[D_POISSON][0] = 4;
    S.dpar[D_EXPO][0] = 1;
    S.a = -1;
    S.b = 1;
    S.p = 0.95;
    S.t_conf = 0.95;
    S.t_n = 10;
    S.t_s = 1;
    S.z_n = 100;
    S.z_x = 50;
    S.z_p0 = 0.5;
    S.w_n1 = S.w_n2 = 10;
    S.w_s1 = S.w_s2 = 1;
}

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/cas_stats.txt", aos_hal_path_data());
}

static void save(void)
{
    char path[200];
    state_path(path, sizeof path);
    mx_str_t b = { 0 };
    for (int l = 0; l < NL; l++) {
        cas_str_printf(&b, "L%d", l + 1);
        for (int i = 0; i < S.n[l]; i++) {
            cas_str_printf(&b, " %.17g", S.v[l][i]);
        }
        mx_str_addc(&b, '\n');
    }
    cas_save_text(path, &b);
}

static void load(void)
{
    S.loaded = true;
    char path[200];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    static char line[MAXN * 26 + 16];
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'L' || line[1] < '1' || line[1] > '0' + NL) {
            continue;
        }
        int l = line[1] - '1';
        char *p = line + 2;
        S.n[l] = 0;
        while (S.n[l] < MAXN) {
            char *e;
            double v = strtod(p, &e);
            if (e == p) break;
            S.v[l][S.n[l]++] = v;
            p = e;
        }
    }
    fclose(f);
}

/* --- Data -------------------------------------------------------------------------- */

static void fill_table(void);
static void fill_summary(void);

static bool cell_entered(void *user, const char *text, double v)
{
    (void)user;
    (void)text;
    int l = S.ecol, r = S.erow;
    if (r > S.n[l]) r = S.n[l];
    if (r >= MAXN) return false;
    S.v[l][r] = v;
    if (r == S.n[l]) S.n[l]++;
    S.erow = r + 1;
    save();
    fill_table();
    if (S.table) {
        cas_grid_select(S.table, S.erow, l + 1);
        cas_grid_scroll_to_row(S.table, S.erow);
    }
    fill_summary();
    char t[32];
    snprintf(t, sizeof t, "L%d[%d]", l + 1, S.erow + 1);
    cas_entry_title(t);
    return S.erow < MAXN;
}

static void table_text(void *user, int row, int col, char *buf, size_t n, uint32_t *color)
{
    (void)user;
    if (row < 0) {
        if (col == 0) {
            snprintf(buf, n, "#");
        } else {
            snprintf(buf, n, "L%d", col);
            if (col - 1 == S.col) *color = LCOL[S.col];
        }
        return;
    }
    if (col == 0) {
        snprintf(buf, n, "%d", row + 1);
        *color = 0x8E8E93;
    } else if (row < S.n[col - 1]) {
        cas_fmt(buf, n, S.v[col - 1][row], 8);
    }
}

static void table_tap(void *user, int row, int col)
{
    (void)user;
    if (col == 0) return;
    if (row < 0) {
        S.col = col - 1;
        cas_grid_refresh(S.table);
        fill_summary();
        return;
    }
    S.ecol = col - 1;
    S.erow = row;
    if (S.erow > S.n[S.ecol]) S.erow = S.n[S.ecol];
    if (S.col != S.ecol) {
        S.col = S.ecol;
        fill_summary();
    }
    cas_grid_select(S.table, S.erow, col);
    char title[32];
    snprintf(title, sizeof title, "L%d[%d]", S.ecol + 1, S.erow + 1);
    bool has = S.erow < S.n[S.ecol];
    cas_entry_open(title, has ? S.v[S.ecol][S.erow] : 0, has, cell_entered, NULL);
}

/* the sheet draws its cells itself (cas_grid.c): this only says how many rows */
static void fill_table(void)
{
    if (!S.table) return;
    int rows = 0;
    for (int l = 0; l < NL; l++) {
        if (S.n[l] > rows) rows = S.n[l];
    }
    cas_grid_set_rows(S.table, rows + 1 < MAXN ? rows + 1 : MAXN);
}

static const char *const SUM_K[12] = {
    "n", N_("media"), "\xCE\xA3x", "\xCE\xA3x\xC2\xB2", "s", "\xCF\x83",
    "min", "Q1", N_("Med"), "Q3", "max", N_("Rango"),
};

/* the summary's widgets, built once per page: a value typed only changes
 * their texts */
static void build_summary(void)
{
    S.sum_title = lv_label_create(S.summary);
    lv_obj_set_width(S.sum_title, LV_PCT(100));
    lv_obj_set_style_text_font(S.sum_title, aos_font_small, 0);
    S.sum_hint = lv_label_create(S.summary);
    lv_label_set_text(S.sum_hint, _("Sin datos: tocá una celda para escribir"));
    lv_obj_set_style_text_color(S.sum_hint, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(S.sum_hint, aos_font_small, 0);
    for (int i = 0; i < 12; i++) {
        lv_obj_t *cell = lv_obj_create(S.summary);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, LV_PCT(S.landscape ? 31 : 48), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *k = lv_label_create(cell);
        lv_label_set_text(k, _(SUM_K[i]));
        lv_obj_set_style_text_color(k, AOS_C_DIM, 0);
        lv_obj_set_style_text_font(k, mx_font(24), 0);
        S.sum_val[i] = lv_label_create(cell);
        lv_obj_set_style_text_font(S.sum_val[i], mx_font(24), 0);
    }
}

static void fill_summary(void)
{
    if (!S.summary) return;
    char title[48];
    snprintf(title, sizeof title, _("Resumen de L%d"), S.col + 1);
    lv_label_set_text(S.sum_title, title);
    lv_obj_set_style_text_color(S.sum_title, lv_color_hex(LCOL[S.col]), 0);
    num_stats_t s;
    bool have = num_stats(S.v[S.col], S.n[S.col], &s);
    if (have) lv_obj_add_flag(S.sum_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(S.sum_hint, LV_OBJ_FLAG_HIDDEN);
    double v[12] = { s.n, s.mean, s.sum, s.sum2, s.sx, s.px, s.min, s.q1, s.med, s.q3, s.max, s.max - s.min };
    for (int i = 0; i < 12; i++) {
        lv_obj_t *cell = lv_obj_get_parent(S.sum_val[i]);
        if (!have) {
            lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_HIDDEN);
        char b[48];
        cas_fmt(b, sizeof b, v[i], 8);
        lv_label_set_text(S.sum_val[i], b);
    }
}

static void list_choose(void *user, int sel)
{
    (void)user;
    S.col = sel;
    fill_summary();
    if (S.table) cas_grid_refresh(S.table);
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    S.n[S.col] = 0;
    save();
    fill_table();
    fill_summary();
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void sort_cb(lv_event_t *e)
{
    (void)e;
    qsort(S.v[S.col], (size_t)S.n[S.col], sizeof(double), cmp_d);
    save();
    fill_table();
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

static const char *const LNAMES[NL] = { "L1", "L2", "L3", "L4" };

static void page_data(lv_obj_t *body)
{
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *seg = cas_seg(bar, LNAMES, NL, S.col, list_choose, NULL);
    lv_obj_set_width(seg, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(seg, 1);
    small_button(bar, _("Ordenar"), sort_cb, NULL);
    small_button(bar, _("Borrar"), clear_cb, NULL);

    static const uint8_t W[NL + 1] = { 2, 4, 4, 4, 4 };
    S.table = cas_grid_create(body, NL + 1, W, 54, table_text, NULL);
    lv_obj_set_width(S.table, LV_PCT(100));
    lv_obj_set_flex_grow(S.table, 1);
    cas_grid_on_tap(S.table, table_tap, NULL);
    fill_table();

    S.summary = cas_card(body, NULL);
    lv_obj_set_flex_flow(S.summary, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(S.summary, 14, 0);
    build_summary();
    fill_summary();
}

/* --- Chart ---------------------------------------------------------------------------- */

static void draw_chart(void);

static void chart_choose(void *user, int sel)
{
    (void)user;
    S.chart = sel;
    build_page();
}

static void bins_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int b = S.bins ? S.bins : 1;
    b += d;
    if (b < 1) b = 1;
    if (b > 60) b = 60;
    S.bins = b;
    draw_chart();
}

static int auto_bins(int n)
{
    int b = (int)ceil(log2((double)n)) + 1;         /* Sturges */
    return b < 1 ? 1 : b;
}

static void chart_hist(cas_plot_t *p, lv_layer_t *layer)
{
    const double *v = S.v[S.col];
    int n = S.n[S.col];
    num_stats_t s;
    if (!num_stats(v, n, &s)) return;
    if (!S.bins) S.bins = auto_bins(n);
    int k = S.bins;
    double w = (s.max - s.min) / k;
    if (w <= 0) w = 1;
    int cnt[64] = { 0 }, top = 1;
    for (int i = 0; i < n; i++) {
        int b = (int)((v[i] - s.min) / w);
        if (b >= k) b = k - 1;
        if (b < 0) b = 0;
        cnt[b]++;
        if (cnt[b] > top) top = cnt[b];
    }
    p->x0 = s.min - w * 0.6;
    p->x1 = s.min + w * (k + 0.6);
    p->y0 = -top * 0.08;
    p->y1 = top * 1.12;
    cas_plot_grid(p);
    uint32_t c = LCOL[S.col];
    for (int b = 0; b < k; b++) {
        float x0 = cas_plot_sx(p, s.min + b * w), x1 = cas_plot_sx(p, s.min + (b + 1) * w);
        float y0 = cas_plot_sy(p, 0), y1 = cas_plot_sy(p, cnt[b]);
        cas_plot_rect(p, x0 + 1, y1, x1 - 1, y0, c, 150);
        cas_plot_line(p, x0 + 1, y1, x1 - 1, y1, c, 2.5f);
    }
    cas_plot_numbers(p, layer);
    char b[64];
    snprintf(b, sizeof b, _("L%d: %d clases de ancho %.4g"), S.col + 1, k, w);
    cas_plot_text(layer, p->W - 12, 8, b, 0xFFFFFF, 2);
}

static void chart_box(cas_plot_t *p, lv_layer_t *layer)
{
    double lo = INFINITY, hi = -INFINITY;
    int shown = 0;
    for (int l = 0; l < NL; l++) {
        num_stats_t s;
        if (num_stats(S.v[l], S.n[l], &s)) {
            lo = fmin(lo, s.min);
            hi = fmax(hi, s.max);
            shown++;
        }
    }
    if (!shown) return;
    double pad = (hi - lo) * 0.08 + (hi == lo ? 1 : 0);
    p->x0 = lo - pad;
    p->x1 = hi + pad;
    p->y0 = 0;
    p->y1 = NL + 1;
    cas_plot_grid(p);
    for (int l = 0; l < NL; l++) {
        num_stats_t s;
        if (!num_stats(S.v[l], S.n[l], &s)) continue;
        double yc = NL - l;
        float y = cas_plot_sy(p, yc), h = (float)p->H / (NL + 1) * 0.28f;
        uint32_t c = LCOL[l];
        double iqr = s.q3 - s.q1, lf = s.q1 - 1.5 * iqr, uf = s.q3 + 1.5 * iqr;
        double wl = s.q1, wh = s.q3;
        for (int i = 0; i < S.n[l]; i++) {
            double x = S.v[l][i];
            if (x >= lf && x < wl) wl = x;
            if (x <= uf && x > wh) wh = x;
            if (x < lf || x > uf) cas_plot_disc(p, cas_plot_sx(p, x), y, 5, c);
        }
        float xq1 = cas_plot_sx(p, s.q1), xq3 = cas_plot_sx(p, s.q3);
        cas_plot_rect(p, xq1, y - h, xq3, y + h, c, 90);
        cas_plot_line(p, xq1, y - h, xq3, y - h, c, 2.5f);
        cas_plot_line(p, xq1, y + h, xq3, y + h, c, 2.5f);
        cas_plot_line(p, xq1, y - h, xq1, y + h, c, 2.5f);
        cas_plot_line(p, xq3, y - h, xq3, y + h, c, 2.5f);
        float xm = cas_plot_sx(p, s.med);
        cas_plot_line(p, xm, y - h, xm, y + h, 0xFFFFFF, 3);
        cas_plot_line(p, cas_plot_sx(p, wl), y, xq1, y, c, 2.5f);
        cas_plot_line(p, xq3, y, cas_plot_sx(p, wh), y, c, 2.5f);
        cas_plot_line(p, cas_plot_sx(p, wl), y - h / 2, cas_plot_sx(p, wl), y + h / 2, c, 2.5f);
        cas_plot_line(p, cas_plot_sx(p, wh), y - h / 2, cas_plot_sx(p, wh), y + h / 2, c, 2.5f);
        cas_plot_text(layer, 10, (int)(y - h - 30), LNAMES[l], c, 0);
    }
    cas_plot_numbers(p, layer);
}

static void chart_scatter(cas_plot_t *p, lv_layer_t *layer)
{
    int n = S.n[0] < S.n[1] ? S.n[0] : S.n[1];
    if (n < 1) {
        cas_plot_text(layer, 12, 8, _("Dispersión: hacen falta datos en L1 (x) y L2 (y)"), 0xAEAEB2, 0);
        return;
    }
    double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    for (int i = 0; i < n; i++) {
        x0 = fmin(x0, S.v[0][i]); x1 = fmax(x1, S.v[0][i]);
        y0 = fmin(y0, S.v[1][i]); y1 = fmax(y1, S.v[1][i]);
    }
    double px = (x1 - x0) * 0.1 + (x1 == x0), py = (y1 - y0) * 0.1 + (y1 == y0);
    p->x0 = x0 - px; p->x1 = x1 + px; p->y0 = y0 - py; p->y1 = y1 + py;
    cas_plot_grid(p);
    if (S.reg.ok) {
        float lx = 0, ly = 0;
        bool have = false;
        for (int s = 0; s <= p->W; s += 2) {
            double y = num_reg_eval(&S.reg, cas_plot_wx(p, (float)s));
            if (!isfinite(y)) { have = false; continue; }
            float sy = cas_plot_sy(p, y);
            if (have && fabsf(sy - ly) < p->H) cas_plot_line(p, lx, ly, (float)s, sy, 0xFF453A, 3);
            lx = (float)s;
            ly = sy;
            have = true;
        }
    }
    for (int i = 0; i < n; i++) {
        cas_plot_disc(p, cas_plot_sx(p, S.v[0][i]), cas_plot_sy(p, S.v[1][i]), 6, LCOL[0]);
    }
    cas_plot_numbers(p, layer);
    cas_plot_text(layer, p->W - 12, 8, "L1 (x)  /  L2 (y)", 0xFFFFFF, 2);
}

static void draw_chart(void)
{
    cas_plot_t *p = &S.plot;
    if (!p->db) return;
    cas_plot_clear(p, 0x000000);
    lv_layer_t layer;
    lv_canvas_init_layer(p->canvas, &layer);
    if (S.chart == 0 && S.n[S.col] == 0) {
        cas_plot_text(&layer, 12, 8, _("Sin datos en esta lista"), 0xAEAEB2, 0);
    } else if (S.chart == 0) {
        chart_hist(p, &layer);
    } else if (S.chart == 1) {
        chart_box(p, &layer);
    } else {
        chart_scatter(p, &layer);
    }
    lv_canvas_finish_layer(p->canvas, &layer);
    cas_plot_done(p);
}

static void page_chart(lv_obj_t *body)
{
    static const char *const CH[] = { N_("Histograma"), N_("Cajas"), N_("Dispersión") };
    const char *ch[3] = { _(CH[0]), _(CH[1]), _(CH[2]) };
    cas_seg(body, ch, 3, S.chart, chart_choose, NULL);
    if (S.chart == 0) {
        lv_obj_t *bar = lv_obj_create(body);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(bar, 8, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *seg = cas_seg(bar, LNAMES, NL, S.col, list_choose, NULL);
        lv_obj_set_width(seg, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(seg, 1);
        small_button(bar, "\xE2\x88\x92", bins_cb, (void *)(intptr_t)-1);
        small_button(bar, "+", bins_cb, (void *)(intptr_t)1);
    }
    S.plotbox = lv_obj_create(body);
    lv_obj_remove_style_all(S.plotbox);
    lv_obj_set_width(S.plotbox, LV_PCT(100));
    lv_obj_set_flex_grow(S.plotbox, 1);
    lv_obj_set_style_radius(S.plotbox, 16, 0);
    lv_obj_set_style_clip_corner(S.plotbox, true, 0);
    lv_obj_remove_flag(S.plotbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(body);
    if (cas_plot_create(&S.plot, S.plotbox, lv_obj_get_width(S.plotbox), lv_obj_get_height(S.plotbox))) {
        draw_chart();
    }
}

/* --- Regression ------------------------------------------------------------------------- */

static void fill_regression(void)
{
    int n = S.n[0] < S.n[1] ? S.n[0] : S.n[1];
    num_regress(S.rk, S.v[0], S.v[1], n, &S.reg);
    if (!S.results) return;
    lv_obj_clean(S.results);
    if (!S.reg.ok) {
        lv_obj_t *l = lv_label_create(S.results);
        lv_label_set_text(l, n < 2 ? _("Hacen falta al menos dos pares en L1 (x) y L2 (y)")
                                   : _("Este modelo no se ajusta a estos datos (¿valores negativos o cero?)"));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        return;
    }
    char text[160];
    num_reg_text(&S.reg, 6, text, sizeof text);
    lv_obj_t *row = lv_obj_create(S.results);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *y = lv_label_create(row);
    lv_label_set_text(y, "y =");
    lv_obj_set_style_text_font(y, mx_font(28), 0);
    lv_obj_t *v = mx_view_create(row, 32);
    mx_view_set_row(v, mx_text_to_row(text));
    static const char *const CN[] = { "a", "b", "c", "d" };
    int nc = S.rk == REG_QUAD ? 3 : S.rk == REG_CUBIC ? 4 : 2;
    for (int i = 0; i < nc; i++) {
        lv_obj_t *val;
        cas_result(S.results, CN[i], &val);
        char b[48];
        cas_fmt(b, sizeof b, S.reg.c[i], 10);
        lv_label_set_text(val, b);
    }
    lv_obj_t *val;
    if (isfinite(S.reg.r)) {
        cas_result(S.results, "r", &val);
        char b[48];
        cas_fmt(b, sizeof b, S.reg.r, 8);
        lv_label_set_text(val, b);
    }
    cas_result(S.results, "r\xC2\xB2", &val);
    char b[48];
    cas_fmt(b, sizeof b, S.reg.r2, 8);
    lv_label_set_text(val, b);
}

static void reg_choose(void *user, int sel)
{
    (void)user;
    S.rk = (num_reg_kind_t)sel;
    fill_regression();
}

static void reg_graph_cb(lv_event_t *e)
{
    (void)e;
    if (!S.reg.ok) return;
    char text[160];
    num_reg_text(&S.reg, 10, text, sizeof text);
    int i = cas_graph_put(text);
    char b[64];
    if (i >= 0) {
        snprintf(b, sizeof b, _("Va al gráfico como f%d(x)"), (i + 1) % 10);
    } else {
        snprintf(b, sizeof b, "%s", _("El gráfico ya tiene seis funciones"));
    }
    aos_ui_toast(b, 1800);
    if (i >= 0) {
        cas_app_show(CAS_V_GRAPH);
    }
}

static void reg_scatter_cb(lv_event_t *e)
{
    (void)e;
    S.page = 1;
    S.chart = 2;
    build_page();
}

static void page_regression(lv_obj_t *body)
{
    static const char *const RN[] = { N_("Lineal"), N_("Cuadr."), N_("Cúbica"), "Exp", "Log", N_("Pot.") };
    const char *rn[REG_COUNT];
    for (int i = 0; i < REG_COUNT; i++) rn[i] = _(RN[i]);
    cas_seg(body, rn, REG_COUNT, S.rk, reg_choose, NULL);
    lv_obj_t *note = lv_label_create(body);
    lv_label_set_text(note, _("x en L1, y en L2"));
    lv_obj_set_style_text_color(note, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(note, aos_font_small, 0);
    S.results = cas_card(body, NULL);
    fill_regression();
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *g = small_button(bar, _("Al gráfico como f(x)"), reg_graph_cb, NULL);
    lv_obj_set_style_bg_color(g, AOS_C_ACCENT, 0);
    small_button(bar, _("Ver los puntos"), reg_scatter_cb, NULL);
}

/* --- Distributions ----------------------------------------------------------------------- */

static lv_obj_t *s_dplot_box;
static cas_plot_t s_dplot;

static void draw_dist(void)
{
    cas_plot_t *p = &s_dplot;
    if (!p->db) return;
    const double *par = S.dpar[S.dist];
    double lo, hi;
    num_dist_range(S.dist, par, &lo, &hi);
    bool disc = num_dist_discrete(S.dist);
    if (disc) { lo -= 0.8; hi += 0.8; }
    double top = 0;
    for (int s = 0; s <= 200; s++) {
        double x = lo + (hi - lo) * s / 200;
        if (disc) x = round(x);
        double y = num_pdf(S.dist, par, x);
        if (isfinite(y) && y > top) top = y;
    }
    if (top <= 0) top = 1;
    p->x0 = lo;
    p->x1 = hi;
    p->y0 = -top * 0.08;
    p->y1 = top * 1.15;
    cas_plot_clear(p, 0x000000);
    cas_plot_grid(p);
    double a = S.a, b = S.b;
    if (S.dmode == 1) {
        a = -INFINITY;
        b = num_inv(S.dist, par, S.p);
    }
    uint32_t c = 0x0A84FF, sh = 0xFF9F0A;
    if (disc) {
        for (double k = ceil(lo); k <= hi; k += 1) {
            double y = num_pdf(S.dist, par, k);
            float x0 = cas_plot_sx(p, k - 0.4), x1 = cas_plot_sx(p, k + 0.4);
            bool in = k >= a && k <= b;
            cas_plot_rect(p, x0, cas_plot_sy(p, y), x1, cas_plot_sy(p, 0), in ? sh : c, in ? 200 : 120);
        }
    } else {
        /* the area first, then the curve on top */
        for (int s = 0; s < p->W; s++) {
            double x = cas_plot_wx(p, s + 0.5f);
            if (x < a || x > b) continue;
            double y = num_pdf(S.dist, par, x);
            if (!isfinite(y)) continue;
            cas_plot_rect(p, (float)s, cas_plot_sy(p, y), (float)s + 1, cas_plot_sy(p, 0), sh, 110);
        }
        float lx = 0, ly = 0;
        bool have = false;
        for (int s = 0; s <= p->W; s++) {
            double y = num_pdf(S.dist, par, cas_plot_wx(p, (float)s));
            if (!isfinite(y)) { have = false; continue; }
            float sy = cas_plot_sy(p, y);
            if (have) cas_plot_line(p, lx, ly, (float)s, sy, c, 3);
            lx = (float)s;
            ly = sy;
            have = true;
        }
    }
    lv_layer_t layer;
    lv_canvas_init_layer(p->canvas, &layer);
    cas_plot_numbers(p, &layer);
    lv_canvas_finish_layer(p->canvas, &layer);
    cas_plot_done(p);
}

static void fill_dist(void)
{
    if (!S.res[0]) return;
    const double *par = S.dpar[S.dist];
    char b[64];
    if (S.dmode == 0) {
        cas_fmt(b, sizeof b, num_prob(S.dist, par, S.a, S.b), 10);
    } else {
        cas_fmt(b, sizeof b, num_inv(S.dist, par, S.p), 10);
    }
    lv_label_set_text(S.res[0], b);
    draw_dist();
}

static void dist_changed(void *user, int id)
{
    (void)user;
    (void)id;
    fill_dist();
}

static void dist_choose(void *user, int sel)
{
    (void)user;
    S.dist = (num_dist_t)sel;
    build_page();
}

static void dmode_choose(void *user, int sel)
{
    (void)user;
    S.dmode = sel;
    build_page();
}

static void page_dist(lv_obj_t *body)
{
    static const char *const DN[] = { "Normal", "t", "\xCF\x87\xC2\xB2", "F", N_("Binomial"), "Poisson", "Exp" };
    const char *dn[D_COUNT];
    for (int i = 0; i < D_COUNT; i++) dn[i] = _(DN[i]);
    cas_seg(body, dn, D_COUNT, S.dist, dist_choose, NULL);
    static const char *const PN[D_COUNT][2] = {
        { "\xCE\xBC", "\xCF\x83" }, { N_("grados de libertad"), NULL }, { N_("grados de libertad"), NULL },
        { "d1", "d2" }, { "n", "p" }, { "\xCE\xBB", NULL }, { "\xCE\xBB", NULL },
    };
    lv_obj_t *card = cas_card(body, NULL);
    for (int i = 0; i < 2; i++) {
        if (PN[S.dist][i]) {
            cas_field(card, _(PN[S.dist][i]), &S.dpar[S.dist][i], i, dist_changed, NULL);
        }
    }
    static const char *const MN[] = { "P(a \xE2\x89\xA4 X \xE2\x89\xA4 b)", N_("Inversa") };
    const char *mn[2] = { MN[0], _(MN[1]) };
    cas_seg(body, mn, 2, S.dmode, dmode_choose, NULL);
    lv_obj_t *c2 = cas_card(body, NULL);
    if (S.dmode == 0) {
        cas_field(c2, "a", &S.a, 0, dist_changed, NULL);
        cas_field(c2, "b", &S.b, 1, dist_changed, NULL);
        cas_result(c2, "P(a \xE2\x89\xA4 X \xE2\x89\xA4 b)", &S.res[0]);
    } else {
        cas_field(c2, "P(X \xE2\x89\xA4 x)", &S.p, 0, dist_changed, NULL);
        cas_result(c2, "x", &S.res[0]);
    }
    s_dplot_box = lv_obj_create(body);
    lv_obj_remove_style_all(s_dplot_box);
    lv_obj_set_width(s_dplot_box, LV_PCT(100));
    lv_obj_set_height(s_dplot_box, S.landscape ? 300 : 380);
    lv_obj_set_style_radius(s_dplot_box, 16, 0);
    lv_obj_set_style_clip_corner(s_dplot_box, true, 0);
    lv_obj_remove_flag(s_dplot_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(body);
    cas_plot_create(&s_dplot, s_dplot_box, lv_obj_get_width(s_dplot_box), lv_obj_get_height(s_dplot_box));
    fill_dist();
}

/* --- Tests --------------------------------------------------------------------------------- */

static void fill_test(void)
{
    if (!S.res[0]) return;
    num_test_t o;
    double conf = S.t_conf;
    if (S.test == 0) {
        num_t_mean(S.t_mean, S.t_s, (int)S.t_n, S.t_mu0, conf, (num_tail_t)S.tail, &o);
    } else if (S.test == 1) {
        num_z_prop((int)S.z_x, (int)S.z_n, S.z_p0, conf, (num_tail_t)S.tail, &o);
    } else {
        num_t_two(S.w_m1, S.w_s1, (int)S.w_n1, S.w_m2, S.w_s2, (int)S.w_n2, conf, (num_tail_t)S.tail, &o);
    }
    char b[96];
    cas_fmt(b, sizeof b, o.stat, 6);
    lv_label_set_text(S.res[0], b);
    if (o.df > 0) cas_fmt(b, sizeof b, o.df, 6); else snprintf(b, sizeof b, "\xE2\x80\x94");
    lv_label_set_text(S.res[1], b);
    cas_fmt(b, sizeof b, o.p, 6);
    lv_label_set_text(S.res[2], b);
    char lo[40], hi[40];
    cas_fmt(lo, sizeof lo, o.lo, 6);
    cas_fmt(hi, sizeof hi, o.hi, 6);
    snprintf(b, sizeof b, "[%s ; %s]", lo, hi);
    lv_label_set_text(S.res[3], b);
    double alpha = 1 - conf;
    if (isfinite(o.p)) {
        snprintf(b, sizeof b, o.p < alpha ? _("Se rechaza H₀ (p < %.2g)") : _("No se rechaza H₀ (p ≥ %.2g)"),
                 alpha);
    } else {
        b[0] = 0;
    }
    lv_label_set_text(S.res[4], b);
}

static void test_changed(void *user, int id)
{
    (void)user;
    (void)id;
    fill_test();
}

static void test_choose(void *user, int sel)
{
    (void)user;
    S.test = sel;
    build_page();
}

static void tail_choose(void *user, int sel)
{
    (void)user;
    S.tail = sel;
    build_page();       /* the hypothesis line says which */
}

static void from_list(int l, double *m, double *s, double *n)
{
    num_stats_t st;
    if (num_stats(S.v[l], S.n[l], &st)) {
        *m = st.mean;
        *s = st.sx;
        *n = st.n;
    }
}

static void fromL_cb(lv_event_t *e)
{
    (void)e;
    if (S.test == 0) {
        from_list(S.col, &S.t_mean, &S.t_s, &S.t_n);
    } else if (S.test == 2) {
        from_list(0, &S.w_m1, &S.w_s1, &S.w_n1);
        from_list(1, &S.w_m2, &S.w_s2, &S.w_n2);
    }
    build_page();
}

static void page_tests(lv_obj_t *body)
{
    static const char *const TN[] = { N_("t media"), N_("z proporción"), N_("t dos medias") };
    const char *tn[3] = { _(TN[0]), _(TN[1]), _(TN[2]) };
    cas_seg(body, tn, 3, S.test, test_choose, NULL);
    static const char *const TL[] = { "\xE2\x89\xA0", "<", ">" };
    lv_obj_t *card = cas_card(body, NULL);
    lv_obj_t *h = lv_label_create(card);
    char hyp[64];
    snprintf(hyp, sizeof hyp, S.test == 2 ? "H\xE2\x82\x81: \xCE\xBC\xE2\x82\x81 %s \xCE\xBC\xE2\x82\x82" :
                              S.test == 1 ? "H\xE2\x82\x81: p %s p\xE2\x82\x80" : "H\xE2\x82\x81: \xCE\xBC %s \xCE\xBC\xE2\x82\x80",
             TL[S.tail]);
    lv_label_set_text(h, hyp);
    lv_obj_set_style_text_font(h, mx_font(24), 0);
    lv_obj_set_style_text_color(h, AOS_C_DIM, 0);
    cas_seg(card, TL, 3, S.tail, tail_choose, NULL);
    if (S.test == 0) {
        cas_field(card, _("media"), &S.t_mean, 0, test_changed, NULL);
        cas_field(card, "s", &S.t_s, 1, test_changed, NULL);
        cas_field(card, "n", &S.t_n, 2, test_changed, NULL);
        cas_field(card, "\xCE\xBC\xE2\x82\x80", &S.t_mu0, 3, test_changed, NULL);
    } else if (S.test == 1) {
        cas_field(card, _("éxitos x"), &S.z_x, 0, test_changed, NULL);
        cas_field(card, "n", &S.z_n, 1, test_changed, NULL);
        cas_field(card, "p\xE2\x82\x80", &S.z_p0, 2, test_changed, NULL);
    } else {
        cas_field(card, _("media ₁"), &S.w_m1, 0, test_changed, NULL);
        cas_field(card, "s\xE2\x82\x81", &S.w_s1, 1, test_changed, NULL);
        cas_field(card, "n\xE2\x82\x81", &S.w_n1, 2, test_changed, NULL);
        cas_field(card, _("media ₂"), &S.w_m2, 3, test_changed, NULL);
        cas_field(card, "s\xE2\x82\x82", &S.w_s2, 4, test_changed, NULL);
        cas_field(card, "n\xE2\x82\x82", &S.w_n2, 5, test_changed, NULL);
    }
    cas_field(card, _("nivel de confianza"), &S.t_conf, 9, test_changed, NULL);
    if (S.test != 1) {
        char b[48];
        if (S.test == 0) snprintf(b, sizeof b, _("Tomar media, s y n de L%d"), S.col + 1);
        else snprintf(b, sizeof b, "%s", _("Tomar de L1 y L2"));
        small_button(card, b, fromL_cb, NULL);
    }
    lv_obj_t *r = cas_card(body, NULL);
    cas_result(r, S.test == 1 ? "z" : "t", &S.res[0]);
    cas_result(r, _("gl"), &S.res[1]);
    cas_result(r, _("valor p"), &S.res[2]);
    cas_result(r, _("intervalo"), &S.res[3]);
    S.res[4] = lv_label_create(r);
    lv_obj_set_style_text_font(S.res[4], aos_font_small, 0);
    lv_obj_set_style_text_color(S.res[4], AOS_C_ORANGE, 0);
    fill_test();
}

/* --- the view ------------------------------------------------------------------------------- */

static void page_choose(void *user, int sel)
{
    (void)user;
    S.page = sel;
    build_page();
}

static void free_plots(void)
{
    cas_plot_free(&S.plot);
    cas_plot_free(&s_dplot);
}

static void build_page(void)
{
    if (!S.root) return;
    cas_entry_close();
    free_plots();
    lv_obj_clean(S.root);
    S.table = S.summary = S.plotbox = S.results = NULL;
    memset(S.res, 0, sizeof S.res);

    static const char *const PG[] = { N_("Datos"), N_("Gráfico"), N_("Regresión"), N_("Distrib."), N_("Pruebas") };
    const char *pg[5];
    for (int i = 0; i < 5; i++) pg[i] = _(PG[i]);
    cas_seg(S.root, pg, 5, S.page, page_choose, NULL);

    S.body = lv_obj_create(S.root);
    lv_obj_remove_style_all(S.body);
    lv_obj_set_width(S.body, LV_PCT(100));
    lv_obj_set_flex_grow(S.body, 1);
    lv_obj_set_flex_flow(S.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.body, 10, 0);
    bool fixed = S.page == 0 || S.page == 1;
    if (fixed) {
        lv_obj_remove_flag(S.body, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_set_scroll_dir(S.body, LV_DIR_VER);
    }
    lv_obj_update_layout(S.root);
    switch (S.page) {
    case 0: page_data(S.body); break;
    case 1: page_chart(S.body); break;
    case 2: page_regression(S.body); break;
    case 3: page_dist(S.body); break;
    default: page_tests(S.body); break;
    }
}

void cas_stats_build(lv_obj_t *parent, bool landscape)
{
    model_init();
    if (!S.loaded) {
        load();
    }
    /* the regression is computed whenever the data may have changed */
    int n = S.n[0] < S.n[1] ? S.n[0] : S.n[1];
    num_regress(S.rk, S.v[0], S.v[1], n, &S.reg);
    S.landscape = landscape;
    S.root = lv_obj_create(parent);
    lv_obj_remove_style_all(S.root);
    lv_obj_set_size(S.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(S.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.root, 10, 0);
    lv_obj_remove_flag(S.root, LV_OBJ_FLAG_SCROLLABLE);
    build_page();
}

void cas_stats_hidden(void)
{
    cas_entry_close();
    free_plots();
    S.root = S.body = S.table = S.summary = S.plotbox = S.results = NULL;
    memset(S.res, 0, sizeof S.res);
}

void cas_stats_reload(void)
{
    model_init();
    for (int i = 0; i < NL; i++) S.n[i] = 0;
    load();
    int n = S.n[0] < S.n[1] ? S.n[0] : S.n[1];
    num_regress(S.rk, S.v[0], S.v[1], n, &S.reg);
    if (S.root) {
        build_page();
    }
}

void cas_stats_free(void)
{
    cas_stats_hidden();
    for (int i = 0; i < NL; i++) {
        mx_free(S.v[i]);
        S.v[i] = NULL;
        S.n[i] = 0;
    }
    S.loaded = false;
}
