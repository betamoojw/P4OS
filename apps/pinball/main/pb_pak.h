/*
 * PINBALL - a table's pack (tools/pack_p4.py): pinball_<id>_p4.pak in /apps
 *
 *   "PBPK" u16 version u16 count
 *   count x entry (52 bytes):
 *       char name[24]  u8 type  u8 flags  u16 w  u16 h  i16 x  i16 y
 *       u32 off  u32 clen  u32 rawlen  u32 extra  u16 spare
 *   data: one LZ4 block per entry (clen == rawlen: stored as is)
 *
 * x, y place an image on the screen (its top-left pixel), so most entries
 * are drawn where they were rendered.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    PK_TEXT = 0,        /* bytes (the table's definition)                     */
    PK_RGB565 = 1,      /* w*h RGB565, opaque                                 */
    PK_SPRITE = 2,      /* w*h RGB565 then w*h alpha                          */
    PK_DEPTH = 3,       /* w*h u16, depth from the camera in 0.1 mm           */
    PK_ADD = 4,         /* a lamp: w*h RGB565 of light added to the picture   */
    PK_PATCH = 5,       /* a state: w*h RGB565, w*h u16 depth, w*h mask bytes */
    PK_PCM = 6,         /* mu-law 8-bit mono at 'extra' Hz                     */
    PK_ALPHA = 7,       /* w*h alpha (a shadow, a mask)                       */
};

#define PB_NAME_LEN 24

typedef struct {
    char     name[PB_NAME_LEN];
    uint8_t  type, flags;
    uint16_t w, h;
    int16_t  x, y;
    uint32_t off, clen, rawlen, extra;
} pb_entry_t;

typedef struct {
    FILE       *fp;
    int         n;
    pb_entry_t *e;
} pb_pak_t;

bool pb_pak_open(pb_pak_t *p, const char *path);
void pb_pak_close(pb_pak_t *p);
const pb_entry_t *pb_pak_find(const pb_pak_t *p, const char *name);
/* the entry unpacked into a new PSRAM buffer (pb_free), NULL on error */
void *pb_pak_load(pb_pak_t *p, const pb_entry_t *e);

void *pb_malloc(size_t n);          /* PSRAM */
void *pb_calloc(size_t n);
void  pb_free(void *p);
