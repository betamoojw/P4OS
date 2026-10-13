/*
 * P4OS - Python: the console's text (p4py_term.c).
 *
 * A terminal of lines in a monospaced font: what is written is kept as
 * lines (the last one open until a newline comes), wrapped to the width at
 * drawing time, and only the rows in view are drawn, so a long session
 * costs nothing to scroll. It follows the bottom while it is there.
 *
 * In what is written, byte 0x01 starts an error and 0x02 ends it (the
 * runtime marks its tracebacks so): the rows between are red.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TERM_OUT,                   /* what the program printed  */
    TERM_IN,                    /* what was typed, echoed     */
    TERM_ERR,                   /* a traceback                */
    TERM_INFO,                  /* the console's own notes    */
} term_kind_t;

lv_obj_t *term_create(lv_obj_t *parent, const lv_font_t *font);
void      term_write(lv_obj_t *term, const char *s, size_t n, term_kind_t kind);
void      term_clear(lv_obj_t *term);
/* the text, the newest lines that fit in max bytes with the terminating
 * zero (malloc'd, the caller frees) */
char     *term_text(lv_obj_t *term, size_t max);
/* changes at every write or clear: whether there is anything new */
uint32_t  term_seq(lv_obj_t *term);
