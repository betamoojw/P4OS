/*
 * P4OS - CAS: a table that draws only what shows (cas_grid.c).
 *
 * lv_table keeps a string per cell and measures every cell's text when one
 * changes: 1440 cells of a 360-month loan took a quarter of a second to
 * fill on the board. This grid keeps nothing: it asks for a cell's text as
 * it draws it, only for the rows on screen, so changing the data is an
 * invalidation, whatever the number of rows.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The text of cell (row, col) into buf; row -1 is the header. *color may
 * be changed (it comes in as the default). */
typedef void (*cas_grid_text_cb_t)(void *user, int row, int col, char *buf, size_t n, uint32_t *color);
typedef void (*cas_grid_tap_cb_t)(void *user, int row, int col);

/* widths: ncols weights (relative). */
lv_obj_t *cas_grid_create(lv_obj_t *parent, int ncols, const uint8_t *widths, int row_h,
                          cas_grid_text_cb_t text, void *user);
void      cas_grid_set_rows(lv_obj_t *grid, int rows);
void      cas_grid_on_tap(lv_obj_t *grid, cas_grid_tap_cb_t cb, void *user);
void      cas_grid_select(lv_obj_t *grid, int row, int col);   /* -1: none */
void      cas_grid_refresh(lv_obj_t *grid);
void      cas_grid_scroll_to_row(lv_obj_t *grid, int row);
