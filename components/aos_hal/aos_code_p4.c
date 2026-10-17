/*
 * P4OS - code an app writes at run time (a recompiler's output), made
 * visible to the instruction fetch.
 *
 * On the P4 the PSRAM heap is executable: the app loader itself puts every
 * .so's code there (components/elf_loader, esp_elf_arch_flush_code). What is
 * written goes in through the data cache, and the instruction fetch reads
 * through its own cache, so a block of fresh code needs both: its lines
 * written back to PSRAM, and whatever the instruction cache kept of those
 * addresses dropped. Then fence.i, so the core's own pipeline does not run
 * what it fetched before.
 *
 * Only the block's lines, through IDF's ranged calls; never the ROM's
 * Cache_*_All, which drop every core's whole cache while the other runs and
 * hung the boot (2026-10-06, docs/MEMORY.md). The range is widened to whole
 * L2 lines (128 B): writing back and invalidating a neighbour's line of
 * instructions is harmless, and the calls want aligned ranges.
 */
#include "aos_hal.h"

#include <stdint.h>
#include "esp_cache.h"

#define LINE 128u

bool aos_hal_code_sync(void *code, size_t size)
{
    if (!code || !size) return false;
    uintptr_t a = (uintptr_t)code & ~(uintptr_t)(LINE - 1);
    uintptr_t b = ((uintptr_t)code + size + LINE - 1) & ~(uintptr_t)(LINE - 1);
    if (esp_cache_msync((void *)a, b - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK)
        return false;
    if (esp_cache_msync((void *)a, b - a, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST) != ESP_OK)
        return false;
    __asm__ volatile("fence.i" ::: "memory");
    return true;
}
