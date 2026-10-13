/*
 * P4OS - Python: the console's keyboard (p4py_kbd.c).
 *
 * Letters and, a page away, the digits and every sign Python uses, with a
 * bar of the words typed most (print(, def, for, range(...) that slides.
 * A key hands over the text it types, or one of these:
 *   "/enter" "/bs" "/tab" "/left" "/right" "/up" "/down"
 */
#pragma once

#include "lvgl.h"

typedef void (*kbd_cb_t)(void *user, const char *key);

lv_obj_t *kbd_create(lv_obj_t *parent, bool landscape, kbd_cb_t cb, void *user);
