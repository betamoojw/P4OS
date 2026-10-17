/*
 * Minimal SDL shim for GnGeo (from RetroESP32-P4's, which has no SDL
 * either). P4OS: cut down to what the vendored files use: the integer
 * types, SDL_Rect, a surface that is only a pixel buffer, the byte swaps
 * and SDL_FillRect; the timing, audio, event, joystick and file stubs that
 * called ESP-IDF and FreeRTOS are gone (the core has no threads and no
 * clock of its own).
 */
#ifndef _GNGEO_SDL_SHIM_H_
#define _GNGEO_SDL_SHIM_H_

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef uint8_t   Uint8;
typedef uint16_t  Uint16;
typedef uint32_t  Uint32;
typedef int8_t    Sint8;
typedef int16_t   Sint16;
typedef int32_t   Sint32;

typedef struct SDL_Rect {
    int16_t x, y;
    uint16_t w, h;
} SDL_Rect;

typedef struct SDL_PixelFormat {
    uint8_t  BitsPerPixel;
    uint8_t  BytesPerPixel;
} SDL_PixelFormat;

typedef struct SDL_Surface {
    SDL_PixelFormat *format;
    int w, h;
    uint16_t pitch;
    void *pixels;
    SDL_Rect clip_rect;
} SDL_Surface;

#define SDL_Swap16(x) __builtin_bswap16(x)
#define SDL_Swap32(x) __builtin_bswap32(x)

static inline int SDL_LockSurface(SDL_Surface *s) { (void)s; return 0; }
static inline void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }

static inline void SDL_SetClipRect(SDL_Surface *s, const SDL_Rect *r) {
    if (s && r) s->clip_rect = *r;
}

/* 16-bit surfaces only, which is all the core makes. */
static inline int SDL_FillRect(SDL_Surface *dst, SDL_Rect *r, uint32_t color) {
    if (!dst || !dst->pixels) return -1;
    uint16_t c16 = (uint16_t)color;
    uint32_t c32 = ((uint32_t)c16 << 16) | c16;
    if (!r) {
        uint32_t *p32 = (uint32_t *)dst->pixels;
        int count32 = (dst->pitch >> 2) * dst->h;
        for (int i = 0; i < count32; i++) p32[i] = c32;
        return 0;
    }
    for (int y = r->y; y < r->y + r->h && y < dst->h; y++) {
        if (y < 0) continue;
        uint16_t *row = (uint16_t *)((uint8_t *)dst->pixels + y * dst->pitch);
        int x = r->x;
        int xend = r->x + r->w;
        if (xend > dst->w) xend = dst->w;
        if ((x & 1) && x < xend) { row[x] = c16; x++; }
        uint32_t *p32 = (uint32_t *)&row[x];
        int n32 = (xend - x) >> 1;
        for (int i = 0; i < n32; i++) p32[i] = c32;
        x += n32 << 1;
        if (x < xend) row[x] = c16;
    }
    return 0;
}

#endif /* _GNGEO_SDL_SHIM_H_ */
