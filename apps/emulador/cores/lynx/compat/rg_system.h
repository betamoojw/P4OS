/*
 * P4OS - Emulador, Lynx core: the two Retro-Go functions Handy calls (its
 * log and its CRC-32), standing in for Retro-Go's rg_system.h. Handy is
 * built with RETRO_GO, so system.h includes this file. lynx_shim.c has the
 * functions.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RG_LOG_PRINTF 0

/* To the host's log, without Handy's trailing newlines. */
void rg_system_log(int level, const char *context, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
uint32_t rg_crc32(uint32_t crc, const uint8_t *buf, size_t len);

/* Where rg_system_log() goes (NULL: nowhere). */
void lynx_shim_set_log(void (*log)(const char *fmt, ...));

#ifdef __cplusplus
}
#endif
