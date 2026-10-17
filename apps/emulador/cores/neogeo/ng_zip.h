/*
 * P4OS - Emulador, Neo Geo core: reading MAME-style .zip sets.
 *
 * A Neo Geo game is a zip of ROM chips (and the BIOS another one). This
 * reads the zip's central directory once and then streams any file in it:
 * stored or deflated (tinfl), with a CRC-32 of what came out. Each stream
 * opens the zip on its own FILE, so two can be read side by side (the
 * sprite ROMs come in pairs whose bytes interleave).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char     name[48];      /* as in the zip, cut at 47 characters */
    uint32_t crc;
    uint32_t csize, usize;  /* compressed and real size */
    uint32_t lho;           /* where its local header is */
    uint16_t method;        /* 0 stored, 8 deflated */
} ng_zent_t;

typedef struct {
    char      *path;
    ng_zent_t *ent;
    int        n;
} ng_zip_t;

typedef struct ng_zstream ng_zstream_t;

/* NULL when the file is not there or is not a zip. */
ng_zip_t *ng_zip_open(const char *path);
void ng_zip_close(ng_zip_t *z);

/* A file of the set: the one with this CRC (MAME's renamed files keep it),
 * else this name (case aside), else one whose name differs only in the
 * extension ("201-p1.bin" is "201-p1.p1" in newer sets). crc 0: by name. */
const ng_zent_t *ng_zip_find(const ng_zip_t *z, const char *name, uint32_t crc);
/* The first file whose name starts with 'prefix' (case aside). */
const ng_zent_t *ng_zip_find_prefix(const ng_zip_t *z, const char *prefix);

ng_zstream_t *ng_zs_open(const ng_zip_t *z, const ng_zent_t *e);
/* Up to n bytes; fewer only at the end of the file. -1 on a damaged zip
 * or a read error. */
int ng_zs_read(ng_zstream_t *s, void *dst, uint32_t n);
/* Reads and drops n bytes. false on an error or the end of the file. */
bool ng_zs_skip(ng_zstream_t *s, uint32_t n);
/* The CRC-32 of what was read so far. */
uint32_t ng_zs_crc(const ng_zstream_t *s);
void ng_zs_close(ng_zstream_t *s);

/* The whole file into dst (at most max bytes). The bytes read, or -1. */
int ng_zip_read(const ng_zip_t *z, const ng_zent_t *e, void *dst, uint32_t max);

uint32_t ng_crc32(uint32_t crc, const uint8_t *p, uint32_t n);
