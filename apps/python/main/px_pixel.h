/*
 * PX_PIXEL - the pixel engine the Python apps draw with (the aos module)
 *
 * Copied from apps/lua/main/lx_pixel.[ch] (which came from Burbujas, Topos,
 * Claude Jump and Arkanos, proven on the board) with the prefix changed and
 * nothing taken out. The prefix is what keeps the
 * simulator linking: it compiles every app's sources into one binary, so two
 * engines with the same names would collide.
 *
 * Here it has a second job it did not have in a game: every one of these
 * primitives is reachable from a script, so every one of them is a place
 * where a wrong argument could walk off the buffer. They already clip -that
 * is the point of the clip- and px_api.c does not trust the script either.
 *
 * The lip (a floor per column, Topos's mole coming out of its hole) is left
 * in so the engines stay the same file; nothing here ever sets one.
 *
 * P4OS: the buffer is the OS's retro canvas (aos_retro.h), whose size the
 * script chooses -240x426 by default, x3 to fill the 720x1280 screen- and
 * the OS scales it. So nothing here knows a size at compile time any more:
 * the buffer carries its own, and so does the dirty list. The upscale that
 * lived here (px_expand) is gone; it is the OS's job now.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Buffer, clip and lip
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t      *px;
    int16_t        w, h;
    int16_t        cx0, cy0, cx1, cy1;  /* clip; x1/y1 exclusive              */

    /* Lip. With lip != NULL a pixel of column x is drawn only when its row is
     * ABOVE lip[x - lip_x0] (exclusive); outside the table the floor is
     * lip_def. NULL means no lip. */
    const int16_t *lip;
    int16_t        lip_x0, lip_n, lip_def;
} px_buf_t;

void px_buf_init(px_buf_t *b, uint16_t *px, int w, int h);
void px_clip(px_buf_t *b, int x0, int y0, int x1, int y1);
void px_clip_none(px_buf_t *b);
void px_lip(px_buf_t *b, const int16_t *tab, int x0, int n, int def);
void px_lip_off(px_buf_t *b);

/* 0xRRGGBB -> RGB565 (the canvas's and the panel's format) */
static inline uint16_t px_rgb(uint32_t hex)
{
    return (uint16_t)(((hex >> 19) & 0x1F) << 11 |
                      ((hex >> 10) & 0x3F) << 5  |
                      ((hex >> 3)  & 0x1F));
}

/* Blends two RGB565s. 'f' goes from 0 (all a) to 16 (all b). */
uint16_t px_mix(uint16_t a, uint16_t b, int f);
/* Darkens (f<0) or lightens (f>0) a colour, in sixteenths. */
uint16_t px_tone(uint16_t c, int f);

/* --------------------------------------------------------------------------
 * Dirty rectangles
 * -------------------------------------------------------------------------- */

#define PX_MAX_DIRTY    18

typedef struct {
    int16_t x0, y0, x1, y1;     /* x1/y1 exclusive */
} px_rect_t;

typedef struct {
    px_rect_t r[PX_MAX_DIRTY];
    uint8_t   n;
    bool      all;              /* the whole screen, no list */
    int16_t   w, h;             /* the canvas it clamps to    */
} px_dirty_t;

/* Sets the size the list clamps to, and empties it. */
void px_dirty_init(px_dirty_t *d, int w, int h);
void px_dirty_reset(px_dirty_t *d);
void px_dirty_all(px_dirty_t *d);
/* Adds a rectangle, merging it with one already there when that is cheap. */
void px_dirty_add(px_dirty_t *d, int x, int y, int w, int h);
int  px_dirty_area(const px_dirty_t *d);

static inline bool px_rect_hit(const px_rect_t *a, const px_rect_t *b)
{
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

/* Copies from one buffer to another of the same size, rows 'stride' pixels
 * apart, only inside the rectangle. */
void px_restore(uint16_t *dst, const uint16_t *src, int stride, const px_rect_t *r);

/* --------------------------------------------------------------------------
 * Primitives. All of them honour the clip and the lip.
 * -------------------------------------------------------------------------- */

void px_px(px_buf_t *b, int x, int y, uint16_t c);
/* blends c over what is there, f in sixteenths */
void px_px_mix(px_buf_t *b, int x, int y, uint16_t c, int f);
void px_fill(px_buf_t *b, uint16_t c);
void px_rect(px_buf_t *b, int x, int y, int w, int h, uint16_t c);
void px_frame(px_buf_t *b, int x, int y, int w, int h, uint16_t c);
void px_hline(px_buf_t *b, int x, int y, int len, uint16_t c);
void px_vline(px_buf_t *b, int x, int y, int len, uint16_t c);
void px_line(px_buf_t *b, int x0, int y0, int x1, int y1, uint16_t c);
void px_disc(px_buf_t *b, int cx, int cy, int r, uint16_t c);
void px_ring(px_buf_t *b, int cx, int cy, int r, uint16_t c);
/* filled axis-aligned ellipse */
void px_ellipse(px_buf_t *b, int cx, int cy, int rx, int ry, uint16_t c);
/* rectangle with bitten corners */
void px_round(px_buf_t *b, int x, int y, int w, int h, int cut, uint16_t c);
/* darkens (f<0) or lightens (f>0) an area, in sixteenths */
void px_shade(px_buf_t *b, int x, int y, int w, int h, int f);
/* disc blended with what is already there, fading towards its edge */
void px_glow(px_buf_t *b, int cx, int cy, int r, uint16_t c, int f);
/* disc blended evenly: smoke, shadows */
void px_disc_mix(px_buf_t *b, int cx, int cy, int r, uint16_t c, int f);
/* thick blended ring: an explosion's wave */
void px_wave(px_buf_t *b, int cx, int cy, int r, int thick, uint16_t c, int f);

/* --------------------------------------------------------------------------
 * Integer trigonometry. Angles in brads (256 per turn), result in 1/256.
 * -------------------------------------------------------------------------- */
int px_sin(int brad);
int px_cos(int brad);
int px_isqrt(int v);

/* --------------------------------------------------------------------------
 * 5x7 font, upper case, digits and a few signs. ASCII only: NO TRANSLATABLE
 * TEXT GOES THROUGH HERE (see topos.h). Numbers and signs only.
 * -------------------------------------------------------------------------- */

#define PX_CH_W         5
#define PX_CH_H         7
#define PX_CH_ADV       6

int  px_text_w(const char *s, int scale);
void px_text(px_buf_t *b, int x, int y, const char *s, uint16_t c, int scale);
/* with a one-pixel outline all round, in the buffer's pixels */
void px_text_ol(px_buf_t *b, int x, int y, const char *s,
                uint16_t fill, uint16_t outline, int scale);

/* Integers to text without snprintf: the HUD is drawn often and newlib takes
 * several hundred bytes of stack, which here is the LVGL task's. */
char *px_num(char *dst, uint32_t v, int min_digits);
