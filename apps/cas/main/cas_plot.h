/*
 * P4OS - CAS: a canvas to plot on (cas_plot.c).
 *
 * An RGB565 buffer drawn into directly (anti-aliased thick lines, discs,
 * translucent rectangles, a grid with its numbers) for the grapher and the
 * charts of statistics and finance. Coordinates are screen pixels unless a
 * function says otherwise; x0..x1 and y0..y1 are the window in the plot's
 * own units, and sx()/sy() map them.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    lv_obj_t      *canvas;
    lv_draw_buf_t *db;
    int            W, H;
    double         x0, x1, y0, y1;
} cas_plot_t;

bool   cas_plot_create(cas_plot_t *p, lv_obj_t *parent, int w, int h);
void   cas_plot_free(cas_plot_t *p);     /* the buffer; the canvas goes with its parent */
/* Where on the screen the biggest plot in view is (for the portal's page
 * to cut it out of a screenshot); false when none is. */
bool   cas_plot_on_screen(lv_area_t *out);

float  cas_plot_sx(const cas_plot_t *p, double x);
float  cas_plot_sy(const cas_plot_t *p, double y);
double cas_plot_wx(const cas_plot_t *p, float sx);
double cas_plot_wy(const cas_plot_t *p, float sy);

void cas_plot_clear(cas_plot_t *p, uint32_t rgb);
void cas_plot_line(cas_plot_t *p, float x0, float y0, float x1, float y1, uint32_t rgb, float w);
void cas_plot_rect(cas_plot_t *p, float x0, float y0, float x1, float y1, uint32_t rgb, int alpha);
void cas_plot_disc(cas_plot_t *p, float x, float y, float r, uint32_t rgb);
void cas_plot_hline(cas_plot_t *p, int y, uint32_t rgb, int w);
void cas_plot_vline(cas_plot_t *p, int x, uint32_t rgb, int w);

/* The grid every 'nice' step (1, 2 or 5 times a power of ten, about 80 px
 * apart), the axes where they cross the window, and their numbers. */
void cas_plot_grid(cas_plot_t *p);
void cas_plot_numbers(cas_plot_t *p, lv_layer_t *layer);

/* Text and the end of a frame */
void cas_plot_text(lv_layer_t *layer, int x, int y, const char *s, uint32_t rgb, int align);
void cas_plot_fmt(char *b, int n, double v, double step);   /* a number for an axis */
void cas_plot_done(cas_plot_t *p);
