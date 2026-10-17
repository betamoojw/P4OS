/*
 * P4OS - Emulador, Atari 2600 core: the pieces of the C++ and C runtimes
 * Stella needs and the firmware does not export (board only).
 *
 * The firmware has no libstdc++: Stella is built with -fno-exceptions and
 * -fno-rtti, std::string is compiled into the core from the headers
 * (C++20, where libstdc++ instantiates it in the user's code), and what is
 * left is here: operator new and delete, the error hooks the headers call
 * (which can only mean a bug in the core: they log and stop), the
 * pure-virtual hook, and the one member of std::string that libstdc++
 * still keeps for its library in C++20.
 *
 * Everything here ends up local to the core: board.cmake leaves
 * emu_core_a2600 the library's only global, so these definitions are seen
 * by Stella's code and by nothing else in the module (a2600.so).
 *
 * operator new takes PSRAM explicitly. The firmware's malloc keeps blocks
 * under 1 KB in internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), and
 * Stella makes many small objects and strings that have no business there.
 */
#ifndef AOS_SIM

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <new>
#include <string>

extern "C" void *heap_caps_malloc(size_t size, uint32_t caps);
#define A26_CAP_SPIRAM (1u << 10)   /* MALLOC_CAP_SPIRAM */
#define A26_CAP_8BIT   (1u << 2)    /* MALLOC_CAP_8BIT   */

[[noreturn]] static void a26_rt_fatal(void)
{
    __builtin_trap();
}

static void *a26_rt_alloc(size_t n)
{
    if (n == 0) n = 1;
    void *p = heap_caps_malloc(n, A26_CAP_SPIRAM | A26_CAP_8BIT);
    if (!p) p = malloc(n);
    if (!p) a26_rt_fatal();
    return p;
}

void *operator new(size_t n) { return a26_rt_alloc(n); }
void *operator new[](size_t n) { return a26_rt_alloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual(void) { a26_rt_fatal(); }

namespace std {
void __throw_bad_alloc(void) { a26_rt_fatal(); }
void __throw_bad_array_new_length(void) { a26_rt_fatal(); }
void __throw_length_error(const char *) { a26_rt_fatal(); }
void __throw_logic_error(const char *) { a26_rt_fatal(); }
void __throw_out_of_range(const char *) { a26_rt_fatal(); }
void __throw_out_of_range_fmt(const char *, ...) { a26_rt_fatal(); }
}

/* libstdc++ declares this one extern even in C++20 (basic_string.tcc). */
template void std::basic_string<char>::_M_replace_cold(char *, size_type, const char *,
                                                       size_type, size_type);

#endif /* AOS_SIM */
