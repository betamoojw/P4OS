/*
 * PINBALL - the dot-matrix display over the table: 128 x 32 dots of 16
 * levels, written with a 5 x 7 font (capitals, digits, the Spanish and
 * German letters) at one or two dots a pixel. What is on it is decided
 * by pb_game.c; pb_render.c turns the dots into round lamps of the
 * table's colour.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DMD_W 128
#define DMD_H 32

typedef struct {
    uint8_t px[DMD_H][DMD_W];
} pb_dmd_t;

void dmd_clear(pb_dmd_t *d);
/* the width of a text in dots at 'scale' (1 or 2) */
int  dmd_text_w(const char *utf8, int scale);
void dmd_text(pb_dmd_t *d, int x, int y, const char *utf8, int scale, int level);
/* centred on the middle column; scrolled by 'shift' dots when it does not fit */
void dmd_text_c(pb_dmd_t *d, int y, const char *utf8, int scale, int level, int shift);
void dmd_rect(pb_dmd_t *d, int x, int y, int w, int h, int level);
void dmd_frame(pb_dmd_t *d, int x, int y, int w, int h, int level);
/* 1234567 -> "1,234,567" (the separator follows the language) */
void dmd_number(char *out, int n, uint32_t v);
