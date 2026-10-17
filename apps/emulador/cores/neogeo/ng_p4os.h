/*
 * P4OS - Emulador, Neo Geo core: forced into every file of the core
 * (-include), gngeo's and ours.
 *
 * gngeo was written for a desktop with SDL and RetroESP32-P4 carries it as
 * a firmware of its own: it prints to the console, calls exit() when
 * memory runs out and uses a few C library functions the P4OS firmware does
 * not lend to apps. This header points those at the core's own versions
 * (ng_platform.c), so the vendored files keep their calls as they are:
 *
 *   printf      -> ng_printf: to the host's log, a line at a time, and only
 *                  so many lines per game (gngeo reports every invalid
 *                  write, which would flood the log);
 *   strdup      -> ng_strdup (not in the firmware's table);
 *   exit        -> ng_fatal: never returns to the caller; it jumps back to
 *                  the core's entry point that armed it (load), which
 *                  fails cleanly. Outside load it only logs.
 */
#ifndef NG_P4OS_H
#define NG_P4OS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int ng_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char *ng_strdup(const char *s);
void ng_fatal(int code);

#define printf ng_printf
#define strdup ng_strdup
#define exit ng_fatal

#endif
