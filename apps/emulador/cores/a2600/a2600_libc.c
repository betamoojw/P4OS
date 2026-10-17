/*
 * P4OS - Emulador, Atari 2600 core: the C library functions Stella calls
 * that the firmware's export table (components/aos_dynapp/aos_symbols.c)
 * does not have (board only). Local to the core, as everything else in it
 * but emu_core_a2600 (see board.cmake).
 */
#ifndef AOS_SIM

#include <stdint.h>

/* Props.cpp upper-cases property values with it (ASCII is all they hold). */
int toupper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

/* Console::fry(), which nothing here calls; a plain LCG. */
static uint32_t s_rand = 1;
int rand(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return (int)((s_rand >> 16) & 0x7FFF);
}

/* Destructors of function-local statics: never run (they would run when the
 * firmware exits, which it does not). */
int atexit(void (*fn)(void))
{
    (void)fn;
    return 0;
}

#endif /* AOS_SIM */
