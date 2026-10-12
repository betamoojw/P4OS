/*
 * P4OS - CAS: the LVGL widget for a tree of boxes (mx_view.c).
 */
#pragma once

#include "mx.h"

typedef void (*mx_view_tap_cb_t)(lv_obj_t *view, void *user);

lv_obj_t *mx_view_create(lv_obj_t *parent, int px);
void      mx_view_set_row(lv_obj_t *view, mx_row_t *row);   /* takes it */
void      mx_view_set_editor(lv_obj_t *view, mx_ed_t *ed);  /* does not */
void      mx_view_refresh(lv_obj_t *view);                  /* after the tree changed */
void      mx_view_set_color(lv_obj_t *view, lv_color_t text);
void      mx_view_set_align_right(lv_obj_t *view, bool right);
void      mx_view_set_focus(lv_obj_t *view, bool focused);
void      mx_view_set_placeholders(lv_obj_t *view, bool show);
void      mx_view_on_tap(lv_obj_t *view, mx_view_tap_cb_t cb, void *user);
const mx_row_t *mx_view_row(lv_obj_t *view);
int       mx_view_cursor_x(lv_obj_t *view);                 /* in the view, px */
