/*
 * P4OS - CAS: the maths keypad (cas_keypad.c).
 *
 * Four pages under a strip of tabs: numbers and the everyday templates,
 * functions, calculus and algebra, letters. A key sends a string to the
 * callback, the same strings mx_ed_key() takes, plus "/enter" and "/ans",
 * which the owner handles.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

typedef void (*cas_keypad_cb_t)(const char *key, void *user);

lv_obj_t *cas_keypad_create(lv_obj_t *parent, cas_keypad_cb_t cb, void *user);

/* A key of a physical keyboard (aos_ui_hwkbd_handler) as the keypad's
 * string: "/" a fraction, "^" an exponent, arrows, Enter... false for a key
 * the maths has no use for. */
bool cas_hwkey_str(uint32_t key, uint8_t mods, char out[8]);
