/*
 * P4OS - Emulador, Lynx core: Retro-Go's log and CRC-32 for Handy (see
 * compat/rg_system.h).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rg_system.h"

static void (*s_log)(const char *fmt, ...);
static int s_lines;

/* Handy logs every illegal opcode it meets: a broken file would flood the
 * board's log (and take seconds a frame doing it). A game gets this many
 * lines, then one saying so. */
#define LYNX_LOG_MAX 40

void lynx_shim_set_log(void (*log)(const char *fmt, ...))
{
    s_log = log;
    s_lines = 0;
}

void rg_system_log(int level, const char *context, const char *fmt, ...)
{
    (void)level;
    (void)context;
    if (!s_log || s_lines > LYNX_LOG_MAX) return;
    if (++s_lines > LYNX_LOG_MAX) {
        s_log("lynx: (further messages left out)");
        return;
    }
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    /* Handy ends its messages (and some lines inside them) with '\n' */
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    for (char *p = line; *p; p++)
        if (*p == '\n') *p = ' ';
    s_log("lynx: %s", line);
}

/* The usual reflected CRC-32 (zlib's), as Retro-Go's rg_crc32 and ESP-IDF's
 * crc32_le compute it: Retro-Go's table of rotated games is keyed on it. */
uint32_t rg_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
