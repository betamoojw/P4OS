/*
 * P4OS - Emulador, Mega Drive core: forced into every file of the core
 * (-include, from board.cmake and sim.cmake).
 *
 * Gwenesis prints with printf() in a few places (bad VDP accesses, the
 * Z80's unknown opcodes). An app's printf never reaches the board's log,
 * so each of those goes to the host's log through md_core.c instead. The
 * standard headers are included first, so that their own declaration of
 * printf is seen before the name is taken over.
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void md_core_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* function-like, so that __attribute__((format(printf, ...))) is left alone */
#define printf(...) md_core_printf(__VA_ARGS__)
