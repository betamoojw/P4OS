/*
 * P4OS - Emulador, NES core: the few Retro-Go functions nofrendo calls.
 *
 * Retro-Go's nofrendo logs through rg_system_log() and finds a game in its
 * database by rg_crc32() of the ROM. Both are given here instead of pulling
 * Retro-Go's system code: the log goes to the host's log (set by the
 * adapter at load) and the CRC is the usual zlib one, which the database's
 * entries are made with. strdup() is here too, because the firmware's
 * symbol table does not export it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>  /* rg_system.h brings it, and nes.c and state.c count on it */

#define RG_LOG_PRINTF 0

/* nofrendo's levels: 1 error, 2 warning, 3 info, 4 debug. */
void nes_rg_log(int level, const char *context, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
uint32_t nes_rg_crc32(uint32_t crc, const uint8_t *buf, size_t len);
char *nes_rg_strdup(const char *s);

/* Where the log goes (NULL: nowhere), and how much of it: messages up to
 * 'max_level' pass. The count of messages is reset too: a broken game that
 * logs on every frame is cut off after a few dozen lines. */
void nes_rg_shim_init(void (*log)(const char *fmt, ...), int max_level);

#define rg_system_log nes_rg_log
#define rg_crc32      nes_rg_crc32

/* Retro-Go puts the CPU loop in IRAM; a card app has no IRAM of its own and
 * runs from PSRAM through the MMU. */
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
