/*
 * P4OS - CAS: the runtime the engine needs and the firmware does not give.
 *
 * Giac comes from KhiCAS, its port to the NumWorks, and is built as PIC by
 * engine/build_engine.sh. This file is what is left between that build and
 * the firmware:
 *
 *  - The arena. Every allocation of the engine (Giac, GMP, libstdc++) is
 *    renamed to cas_malloc & co. at build time and served by TLSF from
 *    PSRAM pools. When the app closes the pools are freed whole: Giac keeps
 *    things in globals for the life of the program, and here the program
 *    lives as long as the .so is loaded.
 *  - The constructors. The loader does not run a .so's .init_array; the
 *    engine's thread runs it before anything else (cas_rt_start).
 *  - The C library pieces the firmware does not export, the C++ runtime's
 *    hooks (no exceptions are thrown: Giac is built without them), a few of
 *    libgcc's helpers, and KhiCAS's hooks into the NumWorks (keys, screen,
 *    files, MicroPython), which here do nothing or the least they can.
 *
 * Board only: the simulator links every app into one program next to the
 * Mac's C library, and has no engine (cas_rt_sim.c stands in).
 */
#ifndef AOS_SIM
#include "cas_rt.h"

#include "aos_hal.h"
#include "tlsf.h"

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#include "esp_heap_caps.h"

/* ---------------------------------------------------------------------------
 * The arena
 * ------------------------------------------------------------------------- */

#define CAS_POOL_FIRST  (2u * 1024 * 1024)
#define CAS_POOL_NEXT   (2u * 1024 * 1024)
#define CAS_POOLS_MAX   12                  /* up to 24 MB, if PSRAM has it */

typedef struct { uint8_t *mem; size_t size; } cas_pool_t;

static tlsf_t     s_tlsf;
static cas_pool_t s_pools[CAS_POOLS_MAX];
static int        s_npools;
static size_t     s_used, s_peak;
static void      *s_lock;
static volatile int s_oom;

static bool in_arena(const void *p)
{
    for (int i = 0; i < s_npools; i++) {
        if ((const uint8_t *)p >= s_pools[i].mem &&
            (const uint8_t *)p <  s_pools[i].mem + s_pools[i].size) {
            return true;
        }
    }
    return false;
}

static bool arena_grow(size_t at_least)
{
    if (s_npools >= CAS_POOLS_MAX) {
        return false;
    }
    size_t size = s_npools ? CAS_POOL_NEXT : CAS_POOL_FIRST;
    size_t need = at_least + tlsf_pool_overhead() + tlsf_size() + 64;
    if (size < need) {
        size = (need + 0xFFFF) & ~(size_t)0xFFFF;
    }
    uint8_t *mem = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) {
        aos_hal_log("cas", "PSRAM refused a pool of %u KB", (unsigned)(size / 1024));
        return false;
    }
    if (!s_tlsf) {
        s_tlsf = tlsf_create_with_pool(mem, size);
    } else {
        tlsf_add_pool(s_tlsf, mem, size);
    }
    s_pools[s_npools].mem  = mem;
    s_pools[s_npools].size = size;
    s_npools++;
    aos_hal_log("cas", "arena +%u KB (%d pools)", (unsigned)(size / 1024), s_npools);
    return true;
}

void *cas_malloc(size_t n)
{
    if (n == 0) {
        n = 1;
    }
    aos_hal_mutex_lock(s_lock);
    void *p = s_tlsf ? tlsf_malloc(s_tlsf, n) : NULL;
    if (!p && arena_grow(n)) {
        p = tlsf_malloc(s_tlsf, n);
    }
    if (p) {
        s_used += tlsf_block_size(p);
        if (s_used > s_peak) {
            s_peak = s_used;
        }
    } else {
        s_oom = 1;
    }
    aos_hal_mutex_unlock(s_lock);
    return p;
}

void cas_free(void *p)
{
    if (!p) {
        return;
    }
    aos_hal_mutex_lock(s_lock);
    if (in_arena(p)) {
        s_used -= tlsf_block_size(p);
        tlsf_free(s_tlsf, p);
        aos_hal_mutex_unlock(s_lock);
        return;
    }
    aos_hal_mutex_unlock(s_lock);
    free(p);        /* something the firmware allocated and handed over */
}

void *cas_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    if (size && total / size != n) {
        return NULL;
    }
    void *p = cas_malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void *cas_realloc(void *p, size_t n)
{
    if (!p) {
        return cas_malloc(n);
    }
    if (n == 0) {
        cas_free(p);
        return NULL;
    }
    if (!in_arena(p)) {
        return realloc(p, n);
    }
    aos_hal_mutex_lock(s_lock);
    size_t old = tlsf_block_size(p);
    void *q = tlsf_realloc(s_tlsf, p, n);
    if (q) {
        s_used = s_used - old + tlsf_block_size(q);
        if (s_used > s_peak) {
            s_peak = s_used;
        }
        aos_hal_mutex_unlock(s_lock);
        return q;
    }
    aos_hal_mutex_unlock(s_lock);
    /* the pool it is in is full: move it to another one */
    q = cas_malloc(n);
    if (q) {
        memcpy(q, p, old < n ? old : n);
        cas_free(p);
    }
    return q;
}

void *cas_memalign(size_t align, size_t n)
{
    aos_hal_mutex_lock(s_lock);
    void *p = s_tlsf ? tlsf_memalign(s_tlsf, align, n) : NULL;
    if (!p && arena_grow(n + align)) {
        p = tlsf_memalign(s_tlsf, align, n);
    }
    if (p) {
        s_used += tlsf_block_size(p);
    } else {
        s_oom = 1;
    }
    aos_hal_mutex_unlock(s_lock);
    return p;
}

void *cas_aligned_alloc(size_t align, size_t n)
{
    return cas_memalign(align, n);
}

int cas_posix_memalign(void **out, size_t align, size_t n)
{
    *out = cas_memalign(align, n);
    return *out ? 0 : ENOMEM;
}

/* KhiCAS reads the size of the NumWorks' heap */
unsigned _heap_size = CAS_POOLS_MAX * CAS_POOL_NEXT;

void cas_rt_mem(cas_mem_t *m)
{
    size_t total = 0;
    for (int i = 0; i < s_npools; i++) {
        total += s_pools[i].size;
    }
    m->arena = total;
    m->used  = s_used;
    m->peak  = s_peak;
    m->pools = s_npools;
    m->oom   = s_oom;
}


/* ---------------------------------------------------------------------------
 * Constructors
 * ------------------------------------------------------------------------- */

typedef void (*cas_ctor_t)(void);

/* The start of .init_array: priority 0 sorts before everything. The end is
 * engine/cas_end.c, the last object of the link. */
__attribute__((used, section(".init_array.00000")))
static const cas_ctor_t cas_ctor_begin[1] = { 0 };
extern const cas_ctor_t cas_ctor_end[];

static int run_ctors(void)
{
    int n = 0;
    for (const cas_ctor_t *f = cas_ctor_begin + 1; f < cas_ctor_end; f++) {
        if (*f && *f != (cas_ctor_t)-1) {
            (*f)();
            n++;
        }
    }
    return n;
}

/* Static destructors: never run. Everything they would free is in the arena,
 * and the arena goes whole. */
void *__dso_handle = &__dso_handle;

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)fn; (void)arg; (void)dso;
    return 0;
}

int atexit(void (*fn)(void))
{
    (void)fn;
    return 0;
}

/* ---------------------------------------------------------------------------
 * The engine's thread
 * ------------------------------------------------------------------------- */

int  cas_engine_boot(unsigned stack_bytes);
const char *cas_engine_eval(const char *in, int *is_error);
void cas_engine_interrupt(void);

#define CAS_STACK       (256u * 1024)
#define CAS_TIMEOUT_MS  60000u

typedef enum { ENG_OFF, ENG_BOOTING, ENG_IDLE, ENG_BUSY, ENG_QUIT, ENG_DONE } eng_state_t;

static volatile eng_state_t s_state = ENG_OFF;
static volatile bool        s_job, s_result;
static char                *s_in;               /* the job (cas_malloc'd)  */
static char                *s_out;              /* the result (malloc'd)   */
static int                  s_out_err;
static uint32_t             s_out_ms;
static uint32_t             s_boot_ms;
static int                  s_ctors;
static bool                 s_used_once;
static volatile bool        s_stop;             /* Stop was pressed        */
static uint32_t             s_eval_t0, s_last_yield;

static void engine_main(void *arg)
{
    (void)arg;
    uint32_t t0 = aos_hal_uptime_ms();
    s_eval_t0 = s_last_yield = t0;      /* back_key_pressed() may run in here */
    s_ctors = run_ctors();
    cas_engine_boot(CAS_STACK);
    s_boot_ms = aos_hal_uptime_ms() - t0;
    aos_hal_log("cas", "engine up in %u ms (%d constructors)", (unsigned)s_boot_ms, s_ctors);
    s_state = ENG_IDLE;

    while (s_state != ENG_QUIT) {
        if (!s_job) {
            aos_hal_sleep_ms(10);
            continue;
        }
        s_state = ENG_BUSY;
        s_stop = false;
        uint32_t t = aos_hal_uptime_ms();
        s_eval_t0 = s_last_yield = t;
        int err = 0;
        const char *r = cas_engine_eval(s_in, &err);
        uint32_t ms = aos_hal_uptime_ms() - t;
        /* the result leaves the arena: the UI frees it with free() */
        size_t len = strlen(r);
        char *copy = malloc(len + 1);
        if (copy) {
            memcpy(copy, r, len + 1);
        }
        aos_hal_mutex_lock(s_lock);
        free(s_out);
        s_out     = copy;
        s_out_err = err;
        s_out_ms  = ms;
        s_result  = true;
        s_job     = false;
        aos_hal_mutex_unlock(s_lock);
        if (s_state == ENG_BUSY) {
            s_state = ENG_IDLE;
        }
    }
    s_state = ENG_DONE;
}

bool cas_rt_start(void)
{
    if (s_used_once) {
        /* The .so was not unloaded between a close and this open (it happens
         * when the app is reopened within the same tick): Giac's globals
         * still point into the arena that was freed. Better to refuse than to
         * run on that. */
        return false;
    }
    s_used_once = true;
    if (!s_lock) {
        s_lock = aos_hal_mutex_create();
    }
    if (!arena_grow(0)) {
        return false;
    }
    s_state = ENG_BOOTING;
    if (!aos_hal_thread_start("cas", engine_main, NULL, CAS_STACK, 3)) {
        s_state = ENG_OFF;
        return false;
    }
    return true;
}

bool cas_rt_ready(void)
{
    return s_state == ENG_IDLE || s_state == ENG_BUSY;
}

bool cas_rt_busy(void)
{
    return s_job || s_state == ENG_BUSY || s_state == ENG_BOOTING;
}

uint32_t cas_rt_boot_ms(void)
{
    return s_boot_ms;
}

bool cas_rt_submit(const char *expr)
{
    if (!cas_rt_ready() || s_job) {
        return false;
    }
    cas_free(s_in);
    size_t len = strlen(expr);
    s_in = cas_malloc(len + 1);
    if (!s_in) {
        return false;
    }
    memcpy(s_in, expr, len + 1);
    s_result = false;
    s_job = true;
    return true;
}

char *cas_rt_take(int *is_error, uint32_t *ms)
{
    if (!s_result) {
        return NULL;
    }
    aos_hal_mutex_lock(s_lock);
    char *r = s_out;
    s_out = NULL;
    *is_error = s_out_err;
    *ms = s_out_ms;
    s_result = false;
    aos_hal_mutex_unlock(s_lock);
    return r;
}

void cas_rt_interrupt(void)
{
    if (s_state == ENG_BUSY) {
        s_stop = true;
        cas_engine_interrupt();
    }
}

void cas_rt_stop(void)
{
    if (s_state == ENG_OFF) {
        return;
    }
    /* The thread has to be out of the .so's code before the runtime unloads
     * it, and out of the arena before it is freed. */
    for (int i = 0; i < 300 && (s_state == ENG_BOOTING); i++) {
        aos_hal_sleep_ms(10);
    }
    s_stop = true;
    cas_engine_interrupt();
    s_state = ENG_QUIT;
    int waited = 0;
    while (s_state != ENG_DONE && waited < 5000) {
        aos_hal_sleep_ms(10);
        waited += 10;
        if (waited % 500 == 0) {
            cas_engine_interrupt();
        }
    }
    if (s_state != ENG_DONE) {
        /* Freeing the arena under a running computation would crash it; the
         * memory is lost instead. */
        aos_hal_log("cas", "the engine did not stop in 5 s; its arena is left behind");
        return;
    }
    free(s_out);
    s_out = NULL;
    for (int i = 0; i < s_npools; i++) {
        heap_caps_free(s_pools[i].mem);
    }
    aos_hal_log("cas", "arena freed (%d pools, peak %u KB)", s_npools, (unsigned)(s_peak / 1024));
    s_npools = 0;
    s_tlsf = NULL;
    s_in = NULL;
    s_state = ENG_OFF;
}

/* ---------------------------------------------------------------------------
 * C library pieces the firmware does not export
 * ------------------------------------------------------------------------- */

void __assert_func(const char *file, int line, const char *fn, const char *expr)
{
    aos_hal_log("cas", "assertion '%s' failed (%s:%d %s)", expr, file ? file : "?", line, fn ? fn : "");
    abort();
}

void exit(int code)
{
    aos_hal_log("cas", "exit(%d) from the engine", code);
    abort();
}

int raise(int sig)            { (void)sig; return 0; }
int setenv(const char *n, const char *v, int o) { (void)n; (void)v; (void)o; return 0; }
int system(const char *c)     { (void)c; return -1; }
uid_t getuid(void)            { return 0; }
int isatty(int fd)            { (void)fd; return 0; }
#undef fileno
int fileno(FILE *f)           { (void)f; return -1; }
FILE *fdopen(int fd, const char *m) { (void)fd; (void)m; return NULL; }
int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return -1; }

int access(const char *path, int mode)
{
    (void)mode;
    struct stat st;
    return stat(path, &st) == 0 ? 0 : -1;
}

#undef clearerr
void clearerr(FILE *f) { (void)f; }

#undef getc
#undef fgetc
int fgetc(FILE *f)
{
    unsigned char c;
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}

#undef putc
int putc(int c, FILE *f)
{
    unsigned char b = (unsigned char)c;
    return fwrite(&b, 1, 1, f) == 1 ? b : EOF;
}

int ungetc(int c, FILE *f)
{
    if (c == EOF || fseek(f, -1, SEEK_CUR) != 0) {
        return EOF;
    }
    return c;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        tv->tv_sec  = time(NULL);
        tv->tv_usec = (suseconds_t)((aos_hal_uptime_ms() % 1000) * 1000);
    }
    return 0;
}

struct tm *localtime(const time_t *t)
{
    static struct tm tm;
    return localtime_r(t, &tm);
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
    /* only what Giac prints with it: dates and times */
    size_t n = 0;
    for (; *fmt && n + 1 < max; fmt++) {
        if (*fmt != '%' || !fmt[1]) {
            s[n++] = *fmt;
            continue;
        }
        fmt++;
        int w = 0;
        switch (*fmt) {
        case 'Y': w = snprintf(s + n, max - n, "%d", tm->tm_year + 1900); break;
        case 'm': w = snprintf(s + n, max - n, "%02d", tm->tm_mon + 1); break;
        case 'd': w = snprintf(s + n, max - n, "%02d", tm->tm_mday); break;
        case 'H': w = snprintf(s + n, max - n, "%02d", tm->tm_hour); break;
        case 'M': w = snprintf(s + n, max - n, "%02d", tm->tm_min); break;
        case 'S': w = snprintf(s + n, max - n, "%02d", tm->tm_sec); break;
        case '%': s[n] = '%'; w = 1; break;
        default:  break;
        }
        if (w < 0 || (size_t)w >= max - n) {
            break;
        }
        n += (size_t)w;
    }
    s[n] = 0;
    return n;
}

static uint32_t s_rand = 1;
void srand(unsigned seed)     { s_rand = seed ? seed : 1; }
int rand(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return (int)((s_rand >> 1) & RAND_MAX);
}
uint32_t arc4random(void)     { return ((uint32_t)rand() << 16) ^ (uint32_t)rand(); }
int getentropy(void *buf, size_t n)
{
    uint8_t *b = buf;
    while (n--) {
        *b++ = (uint8_t)rand();
    }
    return 0;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = cas_malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *p = d + strlen(d);
    while (n-- && *s) {
        *p++ = *s++;
    }
    *p = 0;
    return d;
}

long long strtoll(const char *s, char **end, int base)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    int neg = 0;
    if (*s == '+' || *s == '-') {
        neg = *s++ == '-';
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = s;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end) {
        *end = (char *)(s == start ? start : s);
    }
    return neg ? -(long long)v : (long long)v;
}

size_t strxfrm(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) {
        strncpy(d, s, n);
        d[n - 1] = 0;
    }
    return len;
}

double atof(const char *s) { return strtod(s, NULL); }

div_t div(int a, int b)
{
    div_t r = { a / b, a % b };
    return r;
}

lldiv_t lldiv(long long a, long long b)
{
    lldiv_t r = { a / b, a % b };
    return r;
}

#undef tolower
#undef toupper
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

int vsprintf(char *s, const char *fmt, va_list ap)
{
    return vsnprintf(s, 1 << 20, fmt, ap);
}

char *setlocale(int cat, const char *loc) { (void)cat; (void)loc; return "C"; }
int __locale_mb_cur_max(void) { return 1; }

int __xpg_strerror_r(int err, char *buf, size_t n)
{
    snprintf(buf, n, "error %d", err);
    return 0;
}

/* The wide-character side of libstdc++'s locale: compiled in, never used
 * with anything but ASCII. */
wint_t btowc(int c)                 { return (c >= 0 && c < 0x80) ? (wint_t)c : WEOF; }
int wctob(wint_t c)                 { return c < 0x80 ? (int)c : EOF; }
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
    (void)ps;
    if (!s) {
        return 0;
    }
    if (!n) {
        return (size_t)-2;
    }
    if (pwc) {
        *pwc = (unsigned char)*s;
    }
    return *s ? 1 : 0;
}
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    (void)ps;
    if (s) {
        *s = (char)wc;
    }
    return 1;
}
size_t wcslen(const wchar_t *s)     { size_t n = 0; while (s[n]) n++; return n; }
int wcscoll(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t len = wcslen(s);
    for (size_t i = 0; i < n; i++) {
        d[i] = i < len ? s[i] : 0;
    }
    return len;
}
size_t wcsftime(wchar_t *s, size_t n, const wchar_t *f, const struct tm *t)
{
    (void)f; (void)t;
    if (n) {
        s[0] = 0;
    }
    return 0;
}
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    for (; n--; s++) if (*s == c) return (wchar_t *)s;
    return NULL;
}
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n--; a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n)  { return memcpy(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n) { return memmove(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++) d[i] = c;
    return d;
}
wctype_t wctype(const char *name)   { (void)name; return 0; }
int iswctype(wint_t c, wctype_t t)  { (void)c; (void)t; return 0; }
wint_t towlower(wint_t c)           { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
wint_t towupper(wint_t c)           { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
#undef getwc
#undef putwc
wint_t getwc(FILE *f)               { int c = fgetc(f); return c == EOF ? WEOF : (wint_t)c; }
wint_t putwc(wchar_t c, FILE *f)    { return putc((int)c, f) == EOF ? WEOF : (wint_t)c; }
wint_t ungetwc(wint_t c, FILE *f)   { return ungetc((int)c, f) == EOF ? WEOF : c; }

/* ---------------------------------------------------------------------------
 * libgcc helpers (the toolchain's libgcc.a is not PIC)
 * ------------------------------------------------------------------------- */

int __clzsi2(unsigned a)
{
    if (!a) {
        return 32;
    }
    int n = 0;
    if (!(a & 0xFFFF0000u)) { n += 16; a <<= 16; }
    if (!(a & 0xFF000000u)) { n += 8;  a <<= 8; }
    if (!(a & 0xF0000000u)) { n += 4;  a <<= 4; }
    if (!(a & 0xC0000000u)) { n += 2;  a <<= 2; }
    if (!(a & 0x80000000u)) { n += 1; }
    return n;
}

int __ffssi2(int a)
{
    unsigned u = (unsigned)a;
    if (!u) {
        return 0;
    }
    int n = 1;
    while (!(u & 1)) {
        u >>= 1;
        n++;
    }
    return n;
}

typedef union { long long ll; struct { uint32_t lo; int32_t hi; } w; } dw_t;

long long __ashrdi3(long long a, int b)
{
    dw_t u = { .ll = a }, r;
    if (b == 0) {
        return a;
    }
    if (b >= 32) {
        r.w.hi = u.w.hi >> 31;
        r.w.lo = (uint32_t)(u.w.hi >> (b - 32));
    } else {
        r.w.hi = u.w.hi >> b;
        r.w.lo = (u.w.lo >> b) | ((uint32_t)u.w.hi << (32 - b));
    }
    return r.ll;
}

/* Complex multiply and divide as the compiler calls them. The naive forms:
 * Giac does its own complex arithmetic on its own types; these come in
 * through std::complex<double> in a few corners. */
double _Complex __muldc3(double a, double b, double c, double d)
{
    return __builtin_complex(a * c - b * d, a * d + b * c);
}

float _Complex __mulsc3(float a, float b, float c, float d)
{
    return __builtin_complex(a * c - b * d, a * d + b * c);
}

long double _Complex __multc3(long double a, long double b, long double c, long double d)
{
    return __builtin_complex(a * c - b * d, a * d + b * c);
}

double _Complex __divdc3(double a, double b, double c, double d)
{
    /* Smith's algorithm */
    if ((c < 0 ? -c : c) >= (d < 0 ? -d : d)) {
        double r = d / c, den = c + d * r;
        return __builtin_complex((a + b * r) / den, (b - a * r) / den);
    }
    double r = c / d, den = c * r + d;
    return __builtin_complex((a * r + b) / den, (b * r - a) / den);
}

long double _Complex __divtc3(long double a, long double b, long double c, long double d)
{
    if ((c < 0 ? -c : c) >= (d < 0 ? -d : d)) {
        long double r = d / c, den = c + d * r;
        return __builtin_complex((a + b * r) / den, (b - a * r) / den);
    }
    long double r = c / d, den = c * r + d;
    return __builtin_complex((a * r + b) / den, (b * r - a) / den);
}

/* ---------------------------------------------------------------------------
 * The C++ runtime's hooks
 * ------------------------------------------------------------------------- */

void __cxa_pure_virtual(void)
{
    aos_hal_log("cas", "pure virtual call");
    abort();
}

/* Nothing is thrown (Giac is built with -fno-exceptions), but libstdc++'s
 * __throw_* functions are: an unwinder that finds no handler makes
 * __cxa_throw call std::terminate, which aborts with a message. */
typedef int cas_urc_t;
cas_urc_t _Unwind_RaiseException(void *e)            { (void)e; return 5; /* END_OF_STACK */ }
cas_urc_t _Unwind_Resume_or_Rethrow(void *e)         { (void)e; return 5; }
void _Unwind_Resume(void *e)                         { (void)e; abort(); }
void _Unwind_DeleteException(void *e)                { (void)e; }
uintptr_t _Unwind_GetDataRelBase(void *c)            { (void)c; return 0; }
uintptr_t _Unwind_GetTextRelBase(void *c)            { (void)c; return 0; }
uintptr_t _Unwind_GetRegionStart(void *c)            { (void)c; return 0; }
uintptr_t _Unwind_GetLanguageSpecificData(void *c)   { (void)c; return 0; }
uintptr_t _Unwind_GetIPInfo(void *c, int *before)    { (void)c; if (before) *before = 0; return 0; }
void _Unwind_SetGR(void *c, int i, uintptr_t v)      { (void)c; (void)i; (void)v; }
void _Unwind_SetIP(void *c, uintptr_t v)             { (void)c; (void)v; }

/* Transactional memory clones in libstdc++: never entered. */
uint8_t  _ITM_RU1(const uint8_t *p)                  { return *p; }
uint32_t _ITM_RU4(const uint32_t *p)                 { return *p; }
void _ITM_memcpyRnWt(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void _ITM_memcpyRtWn(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void _ITM_addUserCommitAction(void (*fn)(void *), uint64_t tid, void *arg) { (void)tid; fn(arg); }
void *cas_tm_new_array(size_t n) __asm__("_ZGTtnaj");
void *cas_tm_new_array(size_t n)                     { return cas_malloc(n); }
void cas_tm_delete(void *p) __asm__("_ZGTtdlPv");
void cas_tm_delete(void *p)                          { cas_free(p); }

/* libsupc++ asks for these weakly; there is a single thread here. */
typedef struct { int done; } cas_once_t;
int pthread_once(void *once, void (*fn)(void))
{
    cas_once_t *o = once;
    if (!o->done) {
        o->done = 1;
        fn();
    }
    return 0;
}
static void *s_keys[8];
static int   s_nkeys;
int pthread_key_create(unsigned *key, void (*d)(void *)) { (void)d; if (s_nkeys >= 8) return -1; *key = (unsigned)s_nkeys++; return 0; }
int pthread_key_delete(unsigned key)                 { (void)key; return 0; }
void *pthread_getspecific(unsigned key)              { return key < 8 ? s_keys[key] : NULL; }
int pthread_setspecific(unsigned key, const void *v) { if (key >= 8) return -1; s_keys[key] = (void *)v; return 0; }
int pthread_mutex_init(void *m, const void *a)       { (void)m; (void)a; return 0; }
int pthread_mutex_lock(void *m)                      { (void)m; return 0; }
int pthread_mutex_unlock(void *m)                    { (void)m; return 0; }
int pthread_mutexattr_init(void *a)                  { (void)a; return 0; }
int pthread_mutexattr_destroy(void *a)               { (void)a; return 0; }
int pthread_mutexattr_settype(void *a, int t)        { (void)a; (void)t; return 0; }
int pthread_cond_wait(void *c, void *m)              { (void)c; (void)m; return 0; }
int pthread_cond_broadcast(void *c)                  { (void)c; return 0; }
int pthread_cancel(unsigned long t)                  { (void)t; return 0; }

/* ---------------------------------------------------------------------------
 * KhiCAS's hooks into the NumWorks
 *
 * Its user interface (kdisplay.cc) is left out of the build; what Giac asks
 * of it is in engine/cas_kshim.cc. What is left here is what Giac's core and
 * KhiCAS's smaller files call on the calculator itself: these answer
 * "nothing pressed, nothing to show". A key read says EXIT, so any of
 * KhiCAS's own loops that got started returns at once.
 * ------------------------------------------------------------------------- */

#define KHI_KEY_EXIT 5

/* Giac's control_c() asks this on every check for an interruption (on the
 * NumWorks it is the Back key). It is the one place where a long
 * computation can be made to share the core and to stop: here it yields
 * every 100 ms - the engine's thread would otherwise keep core 0's idle
 * task out for as long as it runs - and says "pressed" on Stop or after
 * CAS_TIMEOUT_MS. */
int back_key_pressed(void)
{
    uint32_t now = aos_hal_uptime_ms();
    if (now - s_last_yield >= 100) {
        s_last_yield = now;
        aos_hal_sleep_ms(1);
    }
    return s_stop || now - s_eval_t0 > CAS_TIMEOUT_MS;
}

void GetKey(int *key)                    { *key = KHI_KEY_EXIT; }
int  getkey(int allow_suspend)           { (void)allow_suspend; return KHI_KEY_EXIT; }
void disable_back_interrupt(void)        { }
void enable_back_interrupt(void)         { }
int  inexammode(void)                    { return 0; }
void statuslinemsg(const char *msg)      { (void)msg; }
void sync_screen(void)                   { }
int  waitforvblank(void)                 { return 0; }
double millis(void)                      { return (double)aos_hal_uptime_ms(); }
void numworks_wait_1ms(int ms)           { aos_hal_sleep_ms((uint32_t)ms); }
void numworks_fill_rect(int x, int y, int w, int h, int c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void numworks_set_pixel(int x, int y, int c)               { (void)x; (void)y; (void)c; }
int  numworks_get_pixel(int x, int y)                      { (void)x; (void)y; return 0; }
void numworks_hide_graph(void)           { }
int  numworks_draw_string(int x, int y, int c, int bg, const char *s, int fake)
{
    (void)y; (void)c; (void)bg; (void)fake;
    return x + 10 * (int)strlen(s);
}
int  numworks_draw_string_small(int x, int y, int c, int bg, const char *s, int fake)
{
    (void)y; (void)c; (void)bg; (void)fake;
    return x + 7 * (int)strlen(s);
}


/* Files: KhiCAS keeps sessions and scripts in the NumWorks' storage. Here
 * they would go to the card; for now only what Giac asks while evaluating. */
int file_exists(const char *name)
{
    struct stat st;
    return stat(name, &st) == 0;
}


const char *read_file(const char *name)
{
    static char *buf;
    FILE *f = fopen(name, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    cas_free(buf);
    buf = cas_malloc((size_t)n + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)n, f);
        buf[got] = 0;
    }
    fclose(f);
    return buf;
}

int write_file(const char *name, const char *s, int len)
{
    FILE *f = fopen(name, "wb");
    if (!f) {
        return 0;
    }
    size_t n = len ? (size_t)len : strlen(s);
    size_t put = fwrite(s, 1, n, f);
    fclose(f);
    return put == n;
}

const char *extapp_fileRead(const char *name, size_t *len, int storage)
{
    (void)storage;
    const char *s = read_file(name);
    if (len) {
        *len = s ? strlen(s) : 0;
    }
    return s;
}
bool extapp_erasesector(void *p)         { (void)p; return false; }
bool extapp_writememory(unsigned char *d, const unsigned char *s, size_t n) { (void)d; (void)s; (void)n; return false; }


/* MicroPython, which KhiCAS links in for its Python mode: not here. */
int   mp_token(const char *line)         { (void)line; return 0; }

#endif /* !AOS_SIM */
