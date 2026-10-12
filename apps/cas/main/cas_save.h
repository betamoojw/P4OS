/*
 * P4OS - CAS: writing state files off the UI's thread (cas_save.c).
 */
#pragma once

#include "mx.h"

/* Writes 'text' to 'path' on a thread of its own; takes the buffer. */
void cas_save_text(const char *path, mx_str_t *text);
/* Waits for the writes already started (before the app closes). */
void cas_save_wait(void);
/* printf into a growing string */
void cas_str_printf(mx_str_t *b, const char *fmt, ...);
