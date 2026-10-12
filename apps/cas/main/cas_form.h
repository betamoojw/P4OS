/*
 * P4OS - CAS: forms (cas_form.c).
 *
 * The pieces statistics and finance are built from: a numeric field (a row
 * with its label and value; tapping it opens the entry sheet), the entry
 * sheet itself (the maths editor and keypad, so 1/3, 2π or √2 are values
 * too), a segmented choice and a read-only result row.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

/* The entry sheet. cb gets what was typed and its value; it returns true
 * to keep the sheet open for the next value (a column of data), false to
 * close it. */
typedef bool (*cas_entry_cb_t)(void *user, const char *text, double value);

void cas_entry_open(const char *title, double initial, bool has_initial, cas_entry_cb_t cb, void *user);
void cas_entry_title(const char *title);
void cas_entry_close(void);

/* A numeric field. 'value' is the model and stays the caller's; cb runs
 * after it changed. */
typedef void (*cas_field_cb_t)(void *user, int id);

lv_obj_t *cas_field(lv_obj_t *parent, const char *label, double *value, int id,
                    cas_field_cb_t cb, void *user);
void      cas_field_refresh(lv_obj_t *field);
void      cas_field_solve_button(lv_obj_t *field, lv_event_cb_t cb, void *user);

/* A choice among a few: buttons side by side, one lit. */
typedef void (*cas_seg_cb_t)(void *user, int sel);
lv_obj_t *cas_seg(lv_obj_t *parent, const char *const *labels, int n, int sel, cas_seg_cb_t cb, void *user);

/* A card: a rounded column with an optional title. */
lv_obj_t *cas_card(lv_obj_t *parent, const char *title);

/* label ........ value, the value a label to update */
lv_obj_t *cas_result(lv_obj_t *parent, const char *label, lv_obj_t **value);

/* Numbers for people: up to 'digits' significant digits, spaces between
 * thousands, the maths minus sign; ±∞ and "—" for NaN. */
void cas_fmt(char *b, size_t n, double v, int digits);
/* money: two decimals */
void cas_fmt_money(char *b, size_t n, double v);
