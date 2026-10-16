/*
 * P4OS - Duke Nukem 3D: what the engine and the game see instead of the C
 * library.
 *
 * Every vendored .c file includes this first (a line marked "P4OS" at its
 * top), so every one of them gets it before its own code. The same five
 * things as Doom's port (apps/doom/main/port/dg_compat.h), for the same
 * reasons, which bite on a board and not on a PC:
 *
 *   - malloc and company: the heap is PSRAM, and every block is on a list
 *     so that leaving the game gives ALL of it back. Duke never frees what
 *     it allocates at startup: a PC process ends, a .so on the board is
 *     unloaded and the heap stays.
 *   - open/close and fopen/fclose: the open files are kept for the same
 *     reason (the GRP stays open for the whole game).
 *   - exit(): there is no process to end. It longjmps back to the worker
 *     that started the game (port/dk_system.c), which cleans up and tells
 *     the app. Duke's Error() ends in exit() too.
 *   - printf and friends: stdout goes to the serial console, which nobody
 *     reads; aos_hal_log reaches /api/log.
 *   - the bits of libc and libm the firmware does not lend.
 *
 * The system headers go first, so their prototypes are declared with the
 * real names before the macros exist.
 */
#ifndef DK_COMPAT_H
#define DK_COMPAT_H

/* int32_t is "long" on this toolchain and "int" on the PCs the code was
 * written on, and the code mixes the two freely: a header declares
 * kread32(int32_t, int32_t *) and the call passes an int *, a definition
 * says int where its declaration said int32_t. On a PC those are the same
 * type; here they "conflict". Both are 32 bits with the same calling
 * convention on RV32, so for the vendored files int32_t is made int, as it
 * was for their authors. This must come before any system header. */
#if defined(__riscv) && defined(__INT32_TYPE__)
#undef  __INT32_TYPE__
#define __INT32_TYPE__ int
#undef  __UINT32_TYPE__
#define __UINT32_TYPE__ unsigned int
#endif

/* Code of 1996-2003, written for Watcom and old gccs: it converts pointers
 * and integers, calls functions it never declared and passes int32 where
 * int is wanted. GCC 14 makes those errors; on this 32-bit target they are
 * what the code always meant, so they stay warnings (as -fpermissive would
 * have them, which the .so's build step cannot pass per file). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic warning "-Wint-conversion"
#pragma GCC diagnostic warning "-Wincompatible-pointer-types"
#pragma GCC diagnostic warning "-Wimplicit-function-declaration"
#pragma GCC diagnostic warning "-Wimplicit-int"
#pragma GCC diagnostic warning "-Wreturn-mismatch"
#pragma GCC diagnostic ignored "-Wpointer-sign"
#endif

/* engine/platform.h picks port/dk_platform.h on this */
#define PLATFORM_P4OS 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <malloc.h>
#include <assert.h>

void *dk_malloc(size_t n);
void *dk_calloc(size_t n, size_t size);
void *dk_realloc(void *p, size_t n);
void  dk_free(void *p);
char *dk_strdup(const char *s);

FILE *dk_fopen(const char *path, const char *mode);
int   dk_fclose(FILE *f);
int   dk_open(const char *path, int flags, ...);
int   dk_close(int fd);

void  dk_exit(int code) __attribute__((noreturn));

int   dk_printf(const char *fmt, ...);
int   dk_vprintf(const char *fmt, va_list ap);
int   dk_fprintf(FILE *f, const char *fmt, ...);
int   dk_vfprintf(FILE *f, const char *fmt, va_list ap);
int   dk_puts(const char *s);
int   dk_putchar(int c);
int   dk_fflush(FILE *f);
int   dk_getchar(void);

int   dk_toupper(int c);
int   dk_tolower(int c);
int   dk_isspace(int c);
int   dk_isdigit(int c);
int   dk_isalpha(int c);
int   dk_isalnum(int c);
int   dk_isprint(int c);
int   dk_isxdigit(int c);
int   dk_rand(void);
void  dk_srand(unsigned s);
double dk_atof(const char *s);
double dk_sqrt(double x);
double dk_sin(double x);
double dk_cos(double x);
double dk_atan(double x);
double dk_pow(double x, double y);
double dk_fabs(double x);
double dk_floor(double x);
char *dk_getenv(const char *name);
int   dk_system(const char *cmd);
char *dk_getcwd(char *buf, size_t n);
int   dk_access(const char *path, int mode);
struct tm *dk_localtime(const time_t *t);
char *dk_strncat(char *d, const char *s, size_t n);
char *dk_strtok(char *s, const char *delim);
int   dk_vsprintf(char *buf, const char *fmt, va_list ap);
int32_t dk_filelength(int fd);

#define malloc      dk_malloc
#define calloc      dk_calloc
#define realloc     dk_realloc
#define free        dk_free
#undef  strdup
#define strdup      dk_strdup
#define fopen       dk_fopen
#define fclose      dk_fclose
#define open        dk_open
#define close       dk_close
#define exit        dk_exit
#undef  printf
#define printf      dk_printf
#define vprintf     dk_vprintf
#define fprintf     dk_fprintf
#define vfprintf    dk_vfprintf
#undef  puts
#define puts        dk_puts
#undef  putchar
#define putchar     dk_putchar
#define fflush      dk_fflush
#undef  getchar
#define getchar     dk_getchar

#undef  toupper
#define toupper     dk_toupper
#undef  tolower
#define tolower     dk_tolower
#undef  isspace
#define isspace     dk_isspace
#undef  isdigit
#define isdigit     dk_isdigit
#undef  isalpha
#define isalpha     dk_isalpha
#undef  isalnum
#define isalnum     dk_isalnum
#undef  isprint
#define isprint     dk_isprint
#undef  isxdigit
#define isxdigit    dk_isxdigit
#define rand        dk_rand
#define srand       dk_srand
#define atof        dk_atof
#define sqrt        dk_sqrt
#define sin         dk_sin
#define cos         dk_cos
#define atan        dk_atan
#define pow         dk_pow
#define fabs        dk_fabs
#define floor       dk_floor
#define getenv      dk_getenv
#define system      dk_system
#define getcwd      dk_getcwd
#define access      dk_access
#define localtime   dk_localtime
#define strncat     dk_strncat
#define strtok      dk_strtok
#define vsprintf    dk_vsprintf
#define filelength  dk_filelength

/* game/global.c carries its own strlwr, itoa and company, with uint8_t *
 * where newlib declares char *: renamed, so the two never meet. */
#define strlwr      dk_strlwr
#define strupr      dk_strupr
#define itoa        dk_itoa
#define ltoa        dk_ltoa
#define ultoa       dk_ultoa

/* the Build engine's own spellings (unix_compat.h had them) */
#define kmalloc(x)  dk_malloc(x)
#define kkmalloc(x) dk_malloc(x)
#define kfree(x)    dk_free(x)
#define kkfree(x)   dk_free(x)

/* ---- the rest of the port, called from patched engine and game files ---- */

/* <card>/duke3d/ with the trailing slash: the GRP, the config, the saves. */
const char *dk_data_dir(void);

/* Error()'s way out: keeps the message for the app and leaves. */
void dk_fatal(const char *msg) __attribute__((noreturn));

/* How much of 'want' bytes the engine's tile cache may take (tiles.c). */
int32_t dk_cache_budget(int32_t want);

/* The worker was asked to stop: leave now, from wherever the game is.
 * Called from the places the game loops waiting (the timer, the frame,
 * the key polls), so that closing the app never waits on the engine. */
void dk_poll_stop(void);

/* While the app holds the game paused (something of the system's over the
 * picture), wait here: the timer does not count the time spent. */
void dk_pause_wait(void);

/* Milliseconds for the game's 120 Hz clock, stopped while paused. */
uint32_t dk_ticks_ms(void);

#endif
