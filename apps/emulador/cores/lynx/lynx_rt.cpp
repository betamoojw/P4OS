/*
 * P4OS - Emulador, Lynx core: the pieces of the C++ runtime Handy needs and
 * the firmware does not export (board only).
 *
 * The firmware has no libstdc++. Handy uses no part of the C++ library: it
 * is built with -fno-exceptions and -fno-rtti, and all it asks for is
 * operator new and delete (its devices are objects it makes with new) and
 * the pure-virtual hook (CLynxBase). They are here, local to the core:
 * board.cmake leaves emu_core_lynx the library's only global.
 *
 * operator new takes PSRAM explicitly. The firmware's malloc keeps blocks
 * under 1 KB in internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL); Handy's
 * small objects have no business there.
 */
#ifndef AOS_SIM

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <new>

extern "C" void *heap_caps_malloc(size_t size, uint32_t caps);
#define LYNX_CAP_SPIRAM (1u << 10)   /* MALLOC_CAP_SPIRAM */
#define LYNX_CAP_8BIT   (1u << 2)    /* MALLOC_CAP_8BIT   */

/* Only a bug in the core gets here: an allocation failing inside Handy (the
 * adapter makes the big ones itself and checks them) or a pure virtual
 * call. */
[[noreturn]] static void lynx_rt_fatal(void)
{
    __builtin_trap();
}

static void *lynx_rt_alloc(size_t n)
{
    if (n == 0) n = 1;
    void *p = heap_caps_malloc(n, LYNX_CAP_SPIRAM | LYNX_CAP_8BIT);
    if (!p) p = malloc(n);
    if (!p) lynx_rt_fatal();
    return p;
}

void *operator new(size_t n) { return lynx_rt_alloc(n); }
void *operator new[](size_t n) { return lynx_rt_alloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual(void) { lynx_rt_fatal(); }

#endif /* AOS_SIM */
