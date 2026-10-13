/*
 * P4OS - Python: the sample apps carried in python.so (py_samples.c,
 * written by tools/gen_samples.py from scripts/). py_script.c writes them
 * to the card's /python/apps the first time it finds no such folder.
 */
#pragma once

#include <stddef.h>

typedef struct {
    const char *name;           /* "hola.py"; NULL ends the table */
    size_t      len;
    const char *text;
} py_sample_t;

extern const py_sample_t py_samples[];
