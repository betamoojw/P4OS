/*
 * P4OS - Emulador, NES core: the Retro-Go functions nofrendo calls (see
 * nes_rg_shim.h).
 */
#include "nes_rg_shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A broken game can log a bogus mapping on every frame: after this many
 * lines the rest of the load is quiet. */
#define NES_LOG_BUDGET 40

static void (*s_log)(const char *fmt, ...);
static int s_max_level;
static int s_count;

void nes_rg_shim_init(void (*log)(const char *fmt, ...), int max_level)
{
    s_log = log;
    s_max_level = max_level;
    s_count = 0;
}

void nes_rg_log(int level, const char *context, const char *fmt, ...)
{
    (void)context;
    if (!s_log || level > s_max_level || s_count > NES_LOG_BUDGET) return;
    if (++s_count > NES_LOG_BUDGET) {
        s_log("nes: too many messages, the rest are dropped");
        return;
    }

    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* nofrendo ends its lines with a newline and the host's log does not
     * want one. */
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    s_log("nes: %s", line);
}

/* zlib's CRC-32 (reflected 0xEDB88320), half a byte at a time: a 64-byte
 * table instead of a kilobyte, and still fast enough for a 2 MB ROM once
 * per load. */
uint32_t nes_rg_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    static const uint32_t tab[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        crc = (crc >> 4) ^ tab[crc & 15];
        crc = (crc >> 4) ^ tab[crc & 15];
    }
    return ~crc;
}

char *nes_rg_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
