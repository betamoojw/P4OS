/*
 * P4OS - CAS: a table that draws only what shows.
 *
 * A scrolling container whose only child is an empty spacer as tall as all
 * the rows: the container scrolls over it, and its own draw event paints
 * the rows that fall in view, with the header pinned on top.
 */
#include "cas_grid.h"

#include "aos_theme.h"
#include "mx.h"

#include <stdio.h>
#include <string.h>

#define MAXC 8

typedef struct {
    lv_obj_t          *spacer;
    int                ncols, rows, row_h;
    uint8_t            w[MAXC];
    cas_grid_text_cb_t text;
    void              *user;
    cas_grid_tap_cb_t  tap;
    void              *tap_user;
    int                sel_r, sel_c;
} grid_t;

static grid_t *get(lv_obj_t *o)
{
    return (grid_t *)lv_obj_get_user_data(o);
}

/* the left edge of each column, from the weights */
static void col_x(const grid_t *g, int width, int *x)
{
    int tot = 0;
    for (int c = 0; c < g->ncols; c++) tot += g->w[c];
    int acc = 0;
    for (int c = 0; c <= g->ncols; c++) {
        x[c] = tot ? width * acc / tot : 0;
        if (c < g->ncols) acc += g->w[c];
    }
}

static void cell_text(lv_layer_t *layer, const lv_font_t *f, int x1, int x2, int y, int h,
                      const char *s, uint32_t color, bool right)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s;
    d.text_local = 1;
    d.font = f;
    d.color = lv_color_hex(color);
    d.align = right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
    int lh = lv_font_get_line_height(f);
    lv_area_t a = { x1 + 6, y + (h - lh) / 2, x2 - 10, y + (h - lh) / 2 + lh };
    lv_draw_label(layer, &d, &a);
}

static void rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t c)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = c;
    r.bg_opa = LV_OPA_COVER;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &r, &a);
}

static void draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    grid_t *g = get(o);
    if (!g) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    int width = lv_area_get_width(&c), h = g->row_h;
    int xs[MAXC + 1];
    col_x(g, width, xs);
    int sy = lv_obj_get_scroll_y(o);
    const lv_font_t *f = mx_font(20);
    char buf[64];

    /* the rows in view: row r is at c.y1 + h (header) + r*h - scroll */
    int first = (sy) / h - 1, last = (sy + lv_area_get_height(&c)) / h + 1;
    if (first < 0) first = 0;
    if (last > g->rows - 1) last = g->rows - 1;
    for (int r = first; r <= last; r++) {
        int y = c.y1 + h + r * h - sy;
        if (r % 2) rect(layer, c.x1, y, c.x2, y + h - 1, AOS_C_CARD);
        for (int k = 0; k < g->ncols; k++) {
            if (r == g->sel_r && k == g->sel_c) {
                rect(layer, c.x1 + xs[k] + 2, y + 2, c.x1 + xs[k + 1] - 2, y + h - 3, lv_color_hex(0x0A3A6A));
            }
            uint32_t col = 0xFFFFFF;
            buf[0] = 0;
            g->text(g->user, r, k, buf, sizeof buf, &col);
            if (buf[0]) cell_text(layer, f, c.x1 + xs[k], c.x1 + xs[k + 1], y, h, buf, col, true);
        }
    }
    /* the header, pinned */
    rect(layer, c.x1, c.y1, c.x2, c.y1 + h - 1, AOS_C_CARD2);
    for (int k = 0; k < g->ncols; k++) {
        uint32_t col = 0xAEAEB2;
        buf[0] = 0;
        g->text(g->user, -1, k, buf, sizeof buf, &col);
        if (buf[0]) cell_text(layer, f, c.x1 + xs[k], c.x1 + xs[k + 1], c.y1, h, buf, col, true);
    }
}

static void click_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    grid_t *g = get(o);
    if (!g || !g->tap) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    int y = p.y - c.y1 + lv_obj_get_scroll_y(o) - g->row_h;
    if (p.y - c.y1 < g->row_h) {
        y = -1;                 /* the header */
    }
    int row = y < 0 ? -1 : y / g->row_h;
    int xs[MAXC + 1];
    col_x(g, lv_area_get_width(&c), xs);
    int col = 0;
    for (int k = 0; k < g->ncols; k++) {
        if (p.x - c.x1 >= xs[k]) col = k;
    }
    if (row >= g->rows) return;
    g->tap(g->tap_user, row, col);
}

static void delete_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    mx_free(get(o));
    lv_obj_set_user_data(o, NULL);
}

lv_obj_t *cas_grid_create(lv_obj_t *parent, int ncols, const uint8_t *widths, int row_h,
                          cas_grid_text_cb_t text, void *user)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_set_style_clip_corner(o, true, 0);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    grid_t *g = mx_alloc(sizeof *g);
    if (!g) return o;
    g->ncols = ncols > MAXC ? MAXC : ncols;
    for (int k = 0; k < g->ncols; k++) g->w[k] = widths ? widths[k] : 1;
    g->row_h = row_h;
    g->text = text;
    g->user = user;
    g->sel_r = g->sel_c = -1;
    lv_obj_set_user_data(o, g);
    g->spacer = lv_obj_create(o);
    lv_obj_remove_style_all(g->spacer);
    lv_obj_remove_flag(g->spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(g->spacer, 1);
    lv_obj_set_height(g->spacer, row_h);
    lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(o, click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(o, delete_cb, LV_EVENT_DELETE, NULL);
    return o;
}

void cas_grid_set_rows(lv_obj_t *o, int rows)
{
    grid_t *g = get(o);
    if (!g) return;
    g->rows = rows;
    lv_obj_set_height(g->spacer, (rows + 1) * g->row_h);
    lv_obj_invalidate(o);
}

void cas_grid_on_tap(lv_obj_t *o, cas_grid_tap_cb_t cb, void *user)
{
    grid_t *g = get(o);
    if (g) {
        g->tap = cb;
        g->tap_user = user;
    }
}

void cas_grid_select(lv_obj_t *o, int row, int col)
{
    grid_t *g = get(o);
    if (g) {
        g->sel_r = row;
        g->sel_c = col;
        lv_obj_invalidate(o);
    }
}

void cas_grid_refresh(lv_obj_t *o)
{
    lv_obj_invalidate(o);
}

void cas_grid_scroll_to_row(lv_obj_t *o, int row)
{
    grid_t *g = get(o);
    if (!g) return;
    int h = lv_obj_get_height(o);
    int y = (row + 1) * g->row_h;
    int sy = lv_obj_get_scroll_y(o);
    if (y + g->row_h > sy + h) {
        lv_obj_scroll_to_y(o, y + g->row_h - h, LV_ANIM_OFF);
    } else if (y - g->row_h < sy) {
        lv_obj_scroll_to_y(o, y - g->row_h > 0 ? y - g->row_h : 0, LV_ANIM_OFF);
    }
}
