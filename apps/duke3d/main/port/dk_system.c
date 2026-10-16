/*
 * P4OS - Duke Nukem 3D: the game's life cycle on the board.
 *
 * The game runs in the app's worker (core 0): its main() (game.c, renamed
 * dk_game_main) is called once and never returns, since Duke has no way out
 * but exit(). Here exit() is a longjmp back to the top of the worker
 * (dk_compat.h reroutes it), and so are Error() and a stop asked by the
 * app. From there every block the game allocated and every file it opened
 * is given back, since the .so is unloaded but the heap is not.
 *
 * The same shape as Doom's port (apps/doom/main/port/dg_system.c), with two
 * things of Duke's own:
 *
 *   - Files. Duke opens its files by bare name ("DUKE3D.CFG", "GAME.CON",
 *     a save's "GAME0.SAV"), relative to a working directory, and builds
 *     some paths with DOS backslashes. There is no working directory on the
 *     board, so every relative path is made one inside the card's duke3d/
 *     folder here, and backslashes become slashes.
 *   - Low-level files. The engine reads the GRP with open/read/lseek, not
 *     stdio, so the descriptors are tracked as well as the FILEs.
 *
 * This file does NOT include the game's headers: the compat macros would
 * turn its own malloc into dk_malloc. What it needs from the game is
 * declared by hand (dk_port.h).
 */
#include "../duke_port.h"
#include "dk_port.h"
#include "aos_hal.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>

#ifndef AOS_SIM
#include "esp_heap_caps.h"
#endif

void dk_exit(int code) __attribute__((noreturn));
void dk_fatal(const char *msg) __attribute__((noreturn));
void dk_music_reset(void);

/* Duke's deepest frames are the CON interpreter (parseifelse recursing into
 * itself) and the renderer's locals; the original ran on a DOS stack of a
 * few tens of KB. 64 KB in PSRAM, where a worker's stack lives on the P4
 * (aos_hal.h, Worker): it costs no internal RAM. The game never reaches the
 * internal flash, which a task with a PSRAM stack must not do
 * (docs/MEMORY.md). The peak is logged when the game leaves. */
#define WORKER_STACK    (64 * 1024)

/* --------------------------------------------------------------------------
 * Memory: every block on a list, all freed when the game leaves
 * -------------------------------------------------------------------------- */

/* Two pointers: 8 bytes on the board. The payload keeps malloc's alignment. */
typedef struct blk {
    struct blk *prev, *next;
} blk_t;

static blk_t    s_blocks;           /* circular list head */
static size_t   s_live_blocks;
static size_t   s_live_bytes_hint;  /* what was asked, for the log only */

static void *raw_alloc(size_t n)
{
#ifdef AOS_SIM
    return malloc(n);
#else
    /* PSRAM always: Duke allocates crumbs too, and those would go to
     * internal RAM under ALWAYSINTERNAL */
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static void *raw_realloc(void *p, size_t n)
{
#ifdef AOS_SIM
    return realloc(p, n);
#else
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static void raw_free(void *p)
{
#ifdef AOS_SIM
    free(p);
#else
    heap_caps_free(p);
#endif
}

void *dk_raw_alloc(size_t n, bool internal)
{
#ifdef AOS_SIM
    (void)internal;
    return malloc(n);
#else
    if (internal) {
        void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    return raw_alloc(n);
#endif
}

void dk_raw_free(void *p)
{
    if (p) raw_free(p);
}

static void list_init(void)
{
    s_blocks.prev = s_blocks.next = &s_blocks;
    s_live_blocks = 0;
    s_live_bytes_hint = 0;
}

static void link_blk(blk_t *b)
{
    b->next = s_blocks.next;
    b->prev = &s_blocks;
    s_blocks.next->prev = b;
    s_blocks.next = b;
    s_live_blocks++;
}

static void unlink_blk(blk_t *b)
{
    b->prev->next = b->next;
    b->next->prev = b->prev;
    s_live_blocks--;
}

void *dk_malloc(size_t n)
{
    blk_t *b = raw_alloc(sizeof(blk_t) + n);
    if (!b) {
        aos_hal_log("duke3d", "malloc(%u) failed", (unsigned)n);
        return NULL;
    }
    link_blk(b);
    s_live_bytes_hint += n;
    return b + 1;
}

void *dk_calloc(size_t n, size_t size)
{
    size_t t = n * size;
    void *p = dk_malloc(t);
    if (p) memset(p, 0, t);
    return p;
}

void *dk_realloc(void *p, size_t n)
{
    if (!p) return dk_malloc(n);
    blk_t *b = (blk_t *)p - 1;
    unlink_blk(b);
    blk_t *nb = raw_realloc(b, sizeof(blk_t) + n);
    if (!nb) {
        link_blk(b);
        return NULL;
    }
    link_blk(nb);
    return nb + 1;
}

void dk_free(void *p)
{
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    unlink_blk(b);
    raw_free(b);
}

char *dk_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = dk_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void free_everything(void)
{
    size_t n = 0;
    while (s_blocks.next != &s_blocks) {
        blk_t *b = s_blocks.next;
        unlink_blk(b);
        raw_free(b);
        n++;
    }
    if (n) aos_hal_log("duke3d", "gave back %u blocks", (unsigned)n);
}

/* The Build engine's tile cache is one block sized to hold every tile of
 * the GRP's art: 6.8 MB for the shareware, much more for the full game.
 * Whatever it is, it leaves this much PSRAM for what the game allocates
 * after it (the sounds, the level, the music) and for the system. */
#define CACHE_MARGIN    (3 * 1024 * 1024)
#define CACHE_MIN       (2 * 1024 * 1024)

int32_t dk_cache_budget(int32_t want)
{
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    int32_t room = (int32_t)fp - CACHE_MARGIN;
    int32_t got = want < room ? want : room;
    if (got < CACHE_MIN) got = CACHE_MIN;
    aos_hal_log("duke3d", "tile cache %d KB of %d KB of art (psram free %u KB)",
                (int)(got / 1024), (int)(want / 1024), (unsigned)(fp / 1024));
    return got;
}

/* --------------------------------------------------------------------------
 * Paths and files
 * -------------------------------------------------------------------------- */

static char s_dir[96];              /* <card>/duke3d/ */

const char *dk_data_dir(void)
{
    if (!s_dir[0]) {
        const char *root = aos_hal_path_sd_root();
        snprintf(s_dir, sizeof s_dir, "%s/duke3d/", root ? root : ".");
    }
    return s_dir;
}

/* Duke's path as the board's: slashes, and inside the data folder unless
 * it already starts at the root. "./x" and ".\x" lose their dot. */
static const char *fix_path(const char *path, char *out, size_t len)
{
    char tmp[160];
    size_t i = 0;
    for (; path[i] && i < sizeof tmp - 1; i++) tmp[i] = path[i] == '\\' ? '/' : path[i];
    tmp[i] = 0;
    const char *p = tmp;
    while (p[0] == '.' && p[1] == '/') p += 2;
    /* "a//b" from game.c's "%s//%s" */
    char clean[160];
    size_t k = 0;
    for (const char *s = p; *s && k < sizeof clean - 1; s++) {
        if (*s == '/' && k && clean[k - 1] == '/') continue;
        clean[k++] = *s;
    }
    clean[k] = 0;
    if (clean[0] == '/') snprintf(out, len, "%s", clean);
    else snprintf(out, len, "%s%s", dk_data_dir(), clean);
    return out;
}

#define MAX_FILES 16
static FILE *s_files[MAX_FILES];
#define MAX_FDS   16
static int   s_fds[MAX_FDS];

FILE *dk_fopen(const char *path, const char *mode)
{
    char full[200];
    FILE *f = fopen(fix_path(path, full, sizeof full), mode);
    if (!f) return NULL;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!s_files[i]) {
            s_files[i] = f;
            return f;
        }
    }
    return f;       /* untracked: only leaks if the game is cut mid-use */
}

int dk_fclose(FILE *f)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i] == f) s_files[i] = NULL;
    }
    return fclose(f);
}

int dk_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    char full[200];
    int fd = open(fix_path(path, full, sizeof full), flags, mode);
    if (fd < 0) return fd;
    for (int i = 0; i < MAX_FDS; i++) {
        if (s_fds[i] < 0) {
            s_fds[i] = fd;
            return fd;
        }
    }
    return fd;
}

int dk_close(int fd)
{
    for (int i = 0; i < MAX_FDS; i++) {
        if (s_fds[i] == fd) s_fds[i] = -1;
    }
    return close(fd);
}

static void close_everything(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i]) {
            fclose(s_files[i]);
            s_files[i] = NULL;
        }
    }
    for (int i = 0; i < MAX_FDS; i++) {
        if (s_fds[i] >= 0) {
            close(s_fds[i]);
            s_fds[i] = -1;
        }
    }
}

/* --------------------------------------------------------------------------
 * stdout: to the system's log (the portal's /api/log), a line at a time
 * -------------------------------------------------------------------------- */

static char s_line[160];
static int  s_line_n;

static void out_char(char c)
{
    if (c == '\n' || c == '\r' || s_line_n >= (int)sizeof s_line - 1) {
        s_line[s_line_n] = 0;
        bool empty = true;
        for (int i = 0; i < s_line_n; i++) {
            if (s_line[i] != ' ' && s_line[i] != '=' && s_line[i] != '*') { empty = false; break; }
        }
        if (!empty) aos_hal_log("duke3d", "%s", s_line);
        s_line_n = 0;
        if (c == '\n' || c == '\r') return;
    }
    s_line[s_line_n++] = c;
}

int dk_vprintf(const char *fmt, va_list ap)
{
    char buf[640];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    for (const char *p = buf; *p; p++) out_char(*p);
    return n;
}

int dk_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = dk_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int dk_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (f == stdout || f == stderr) return dk_vprintf(fmt, ap);
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) fwrite(buf, 1, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1), f);
    return n;
}

int dk_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = dk_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int dk_puts(const char *s)
{
    while (*s) out_char(*s++);
    out_char('\n');
    return 1;
}

int dk_putchar(int c)
{
    out_char((char)c);
    return c;
}

int dk_fflush(FILE *f)
{
    if (f == stdout || f == stderr) return 0;
    return fflush(f);
}

/* Only the DOS-era "press a key" prompts read stdin, and there is none:
 * a 'n' answers "continue anyway?" with no, which leaves through Error. */
int dk_getchar(void)
{
    return 'n';
}

/* --------------------------------------------------------------------------
 * The small libc and libm pieces the firmware does not lend
 * -------------------------------------------------------------------------- */

int dk_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int dk_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int dk_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int dk_isdigit(int c) { return c >= '0' && c <= '9'; }
int dk_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int dk_isalnum(int c) { return dk_isdigit(c) || dk_isalpha(c); }
int dk_isprint(int c) { return c >= 0x20 && c < 0x7F; }
int dk_isxdigit(int c) { return dk_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* rand(): the C library's own, an LCG, so a game seeded the same plays the
 * same; Duke's game logic has its own generator (krand) anyway. */
static uint32_t s_rand = 1;
int  dk_rand(void) { s_rand = s_rand * 1103515245u + 12345u; return (int)((s_rand >> 16) & 0x7FFF); }
void dk_srand(unsigned s) { s_rand = s; }

/* The few doubles the game asks for are table set-up at start (the engine's
 * sine and arctangent tables, the pitch table): float precision does, and
 * the float functions are lent. */
double dk_atof(const char *s) { return strtod(s, NULL); }
double dk_sqrt(double x) { return (double)sqrtf((float)x); }
double dk_sin(double x) { return (double)sinf((float)x); }
double dk_cos(double x) { return (double)cosf((float)x); }
double dk_atan(double x) { return (double)atan2f((float)x, 1.0f); }
double dk_pow(double x, double y) { return (double)powf((float)x, (float)y); }
double dk_fabs(double x) { return x < 0 ? -x : x; }
double dk_floor(double x) { return (double)floorf((float)x); }
char  *dk_getenv(const char *name) { (void)name; return NULL; }
int    dk_system(const char *cmd) { (void)cmd; return -1; }
char  *dk_getcwd(char *buf, size_t n)
{
    snprintf(buf, n, "%s", dk_data_dir());
    return buf;
}

int dk_access(const char *path, int mode)
{
    (void)mode;
    char full[200];
    struct stat st;
    return stat(fix_path(path, full, sizeof full), &st) == 0 ? 0 : -1;
}

struct tm *dk_localtime(const time_t *t)
{
    static struct tm tm;
    return localtime_r(t, &tm);
}

char *dk_strncat(char *d, const char *s, size_t n)
{
    char *p = d + strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}

char *dk_strtok(char *s, const char *delim)
{
    static char *next;
    if (s) next = s;
    if (!next) return NULL;
    while (*next && strchr(delim, *next)) next++;
    if (!*next) {
        next = NULL;
        return NULL;
    }
    char *tok = next;
    while (*next && !strchr(delim, *next)) next++;
    if (*next) *next++ = 0;
    else next = NULL;
    return tok;
}

/* the callers' buffers are big enough for what they print, by design of
 * the time; 1 KB is more than any of them */
int dk_vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, 1024, fmt, ap);
}

int32_t dk_filelength(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? (int32_t)st.st_size : -1;
}

#ifndef AOS_SIM
/* Neither the P4's RISC-V (no Zbb) nor the firmware lends a byte swap; see
 * apps/doom/main/port/dg_system.c for why this is built at -O0 (written
 * with shifts, gcc turns the body into a call to itself). */
__attribute__((optimize("O0")))
unsigned int __bswapsi2(unsigned int x)
{
    volatile unsigned char b[4];
    b[0] = (unsigned char)x;
    b[1] = (unsigned char)(x >> 8);
    b[2] = (unsigned char)(x >> 16);
    b[3] = (unsigned char)(x >> 24);
    return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) |
           ((unsigned int)b[2] << 8) | (unsigned int)b[3];
}

/* 64-bit arithmetic shift right: the engine's fixed-point helpers shift
 * int64 values and gcc calls libgcc for it, which the firmware does not
 * lend. Written on the two 32-bit halves, so it cannot turn into a call to
 * itself (the __bswapsi2 trap above). */
long long __ashrdi3(long long a, int b)
{
    union { long long ll; struct { uint32_t lo; int32_t hi; } w; } u, r;
    u.ll = a;
    if (b == 0) return a;
    if (b >= 32) {
        r.w.hi = u.w.hi >> 31;
        r.w.lo = (uint32_t)(u.w.hi >> (b - 32));
    } else {
        r.w.hi = u.w.hi >> b;
        r.w.lo = (u.w.lo >> b) | ((uint32_t)u.w.hi << (32 - b));
    }
    return r.ll;
}

/* The game asserts here and there. newlib's __assert_func is not lent, and
 * a failed assert would restart the board: here it is an Error, shown on
 * the screen, back to the OS. */
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    char msg[160];
    const char *base = strrchr(file, '/');
    snprintf(msg, sizeof msg, "assert %s (%s:%d %s)", expr, base ? base + 1 : file, line,
             func ? func : "");
    dk_fatal(msg);
}
#endif

/* --------------------------------------------------------------------------
 * Leaving: exit(), Error() and the app's stop all land here
 * -------------------------------------------------------------------------- */

static jmp_buf              s_jmp;
static volatile bool        s_armed;
static volatile dk_state_t  s_state = DK_IDLE;
static char                 s_error[240];
static char                 s_grp[128];
static volatile bool        s_stopping;
static bool                 s_autowalk;         /* see dk_autowalk() */

enum { JMP_QUIT = 1, JMP_ERROR, JMP_STOP };

void dk_exit(int code)
{
    if (s_armed) {
        s_armed = false;
        longjmp(s_jmp, s_stopping ? JMP_STOP : code == 0 ? JMP_QUIT : JMP_ERROR);
    }
    abort();
}

void dk_fatal(const char *msg)
{
    snprintf(s_error, sizeof s_error, "%s", msg);
    aos_hal_log("duke3d", "Error: %s", msg);
    dk_exit(1);
}

/* Duke's Error(): a message, then exit. EXIT_SUCCESS ones are the normal
 * ways out ("Quick Exit"), the others are failures shown to the player. */
void dk_error_exit(int code, const char *msg)
{
    if (msg && msg[0]) {
        snprintf(s_error, sizeof s_error, "%s", msg);
        /* the messages end in newlines: drop them for the screen */
        size_t n = strlen(s_error);
        while (n && (s_error[n - 1] == '\n' || s_error[n - 1] == '\r')) s_error[--n] = 0;
        aos_hal_log("duke3d", "Error(%d): %s", code, s_error);
    }
    dk_exit(code);
}

void dk_poll_stop(void)
{
    if (!aos_hal_worker_should_stop() || !s_armed) return;
    s_stopping = true;
    dk_exit(0);
}

/* The pause (dk_pause): asked from LVGL's task, a flag the game reads in
 * its waits. While it holds, nothing runs or mixes, and the game's clock
 * stops: the time spent here is taken off dk_ticks_ms, so Duke does not
 * run the missed tics in a burst when it comes back. */
static volatile bool s_pause;
static uint32_t      s_paused_ms;       /* the game's only */

void dk_pause(bool on)
{
    s_pause = on;
}

void dk_pause_wait(void)
{
    dk_poll_stop();
    if (!s_pause) return;
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    while (s_pause) {
        dk_poll_stop();
        aos_hal_worker_sleep(20);
    }
    s_paused_ms += (uint32_t)aos_hal_uptime_ms() - t0;
}

uint32_t dk_ticks_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms() - s_paused_ms;
}

/* Extra command-line arguments, from <card>/duke3d/args.txt: one line,
 * the DOS executable's switches ("/v1 /l1 /s2" starts episode 1, level 1,
 * skill 2; see the app's README). For whoever wants them; none by default. */
#define MAX_ARGS 16
static char  s_args_buf[256];
static char *s_argv[MAX_ARGS + 4];

static int build_argv(void)
{
    int argc = 0;
    s_argv[argc++] = "duke3d";
    char path[200];
    snprintf(path, sizeof path, "%sargs.txt", dk_data_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(s_args_buf, 1, sizeof s_args_buf - 1, f);
        fclose(f);
        s_args_buf[n] = 0;
        char *p = s_args_buf;
        while (*p && argc < MAX_ARGS) {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (!*p || *p == '#') break;
            s_argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = 0;
        }
        aos_hal_log("duke3d", "args.txt: %d argument(s)", argc - 1);
    }
    s_argv[argc] = NULL;
    return argc;
}

static void worker(void *arg)
{
    (void)arg;
    int r = setjmp(s_jmp);
    if (r == 0) {
        s_armed = true;
        s_state = DK_LOADING;
        int argc = build_argv();
        {
            char path[200];
            struct stat st;
            snprintf(path, sizeof path, "%sautowalk.txt", dk_data_dir());
            s_autowalk = stat(path, &st) == 0;
            if (s_autowalk) aos_hal_log("duke3d", "autowalk.txt: walking forward on its own");
        }
        dk_game_main(argc, s_argv);
        /* main() returned: Duke's own end is gameexit() -> exit() */
        dk_exit(0);
    }
    s_armed = false;
    dk_sound_shutdown();
    dk_music_reset();
    close_everything();
    free_everything();
    s_state = r == JMP_QUIT ? DK_QUIT : r == JMP_STOP ? DK_STOPPED : DK_ERROR;
    if (s_state == DK_ERROR && !s_error[0]) snprintf(s_error, sizeof s_error, "Duke exited");
    aos_hal_log("duke3d", "game out (%s)", s_state == DK_QUIT ? "quit" :
                s_state == DK_STOPPED ? "stopped" : s_error);
}

/* The game calls this once its loading is done and the first frame is up. */
void dk_running(void)
{
    if (s_state == DK_LOADING) s_state = DK_RUNNING;
}

const char *dk_grp_path(void)
{
    return s_grp;
}

/* A test aid: with <card>/duke3d/autowalk.txt present, the stick is held
 * forward while playing, so walking can be checked with nobody at the
 * board (read once, when the game starts). */
bool dk_autowalk(void)
{
    return s_autowalk;
}

/* --------------------------------------------------------------------------
 * The app's side
 * -------------------------------------------------------------------------- */

bool dk_find_grp(char *out, int len)
{
    /* the full game's and the shareware's are both DUKE3D.GRP; the card's
     * file system is FAT, which does not care about case, but the
     * simulator's does */
    static const char *names[] = { "DUKE3D.GRP", "duke3d.grp", "Duke3D.grp", "DUKE3D.grp" };
    struct stat st;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(out, (size_t)len, "%s%s", dk_data_dir(), names[i]);
        if (stat(out, &st) == 0 && st.st_size > 0) return true;
    }
    return false;
}

bool dk_start(const char *grp_path)
{
    if (s_state == DK_LOADING || s_state == DK_RUNNING) return false;
    list_init();
    memset(s_files, 0, sizeof s_files);
    for (int i = 0; i < MAX_FDS; i++) s_fds[i] = -1;
    s_error[0] = 0;
    s_stopping = false;
    s_line_n = 0;
    s_pause = false;
    s_paused_ms = 0;
    snprintf(s_grp, sizeof s_grp, "%s", grp_path);
    char dir[96];
    snprintf(dir, sizeof dir, "%s", dk_data_dir());
    size_t n = strlen(dir);
    if (n && dir[n - 1] == '/') dir[n - 1] = 0;
    mkdir(dir, 0755);

    if (!dk_video_alloc()) return false;
    dk_input_reset();
    s_state = DK_LOADING;
    /* core 0 at priority 3, as Doom: LVGL is pinned to core 1 and blits
     * the frames; a worker above 3 on core 0 starves the portal (aos_hal.h,
     * Worker). The game sleeps whenever a frame is not due. */
    if (!aos_hal_worker_start_on("duke3d", worker, NULL, WORKER_STACK, 0, 3)) {
        dk_video_release();
        s_state = DK_ERROR;
        snprintf(s_error, sizeof s_error, "No memory for the game's task");
        return false;
    }
    return true;
}

void dk_stop(void)
{
    aos_hal_worker_stop();
    dk_video_release();
}

dk_state_t dk_state(void)
{
    return s_state;
}

const char *dk_error(void)
{
    return s_error;
}
