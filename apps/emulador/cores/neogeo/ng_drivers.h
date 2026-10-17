/*
 * P4OS - Emulador, Neo Geo core: the games it knows.
 *
 * gngeo describes each game (which ROM chips, where each goes) in a .drv
 * file inside its gngeo_data.zip; RetroESP32-P4 asks the player to copy
 * that zip to the card. Here the same descriptions are compiled in
 * (ng_drivers.c, made by tools/gen_drivers.py from gngeo_data.zip), with
 * the two CMC decryption tables the same zip carries.
 */
#pragma once

#include <stdint.h>

/* gngeo's region numbers (roms.h) */
typedef struct {
    const char *file;
    uint8_t     region;
    uint32_t    src, dest, size, crc;
} ng_drvrom_t;

typedef struct {
    const char        *name;        /* MAME's short name: the zip's */
    const char        *parent;      /* "neogeo" for a parent set */
    const char        *longname;
    uint16_t           year;
    uint8_t            nrom;
    uint32_t           romsize[10]; /* bytes per region */
    const ng_drvrom_t *rom;
} ng_driver_t;

extern const ng_driver_t ng_drivers[];
extern const int ng_n_drivers;

/* gngeo's rom/cmc42.xor and rom/cmc50.xor */
extern const uint8_t ng_cmc42_xor[];
extern const uint8_t ng_cmc50_xor[];
extern const unsigned ng_cmc42_xor_len, ng_cmc50_xor_len;

const ng_driver_t *ng_driver_find(const char *name);
