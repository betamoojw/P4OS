/*
 * P4OS - Emulador, Neo Geo core: what gngeo expects from its platform.
 *
 * RetroESP32-P4 has esp32_platform.c for this (screen, sound task, input,
 * menus, configuration items); on P4OS the app does all of that, so this
 * is only the log, the error jump, the allocator with the core's PSRAM
 * budget, the save state buffer and gngeo's progress-bar calls (to the
 * log).
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ng_core.h"

static const emu_host_t *s_host;

/* ---- log ---------------------------------------------------------------- */

#define NG_PRINTF_LINES 80

static char s_line[192];
static size_t s_line_len;
static int s_lines;

void ng_platform_init(const emu_host_t *host)
{
    s_host = host;
    s_line_len = 0;
}

void ng_printf_reset(void)
{
    s_lines = 0;
    s_line_len = 0;
}

void ng_log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_host && s_host->log) s_host->log("neogeo: %s", buf);
}

/* gngeo's printf: whole lines to the host's log, without the newline, the
 * first NG_PRINTF_LINES of each game only. */
int ng_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    for (const char *p = buf; *p; p++) {
        if (*p == '\n' || s_line_len == sizeof(s_line) - 1) {
            s_line[s_line_len] = 0;
            if (s_line_len && s_lines < NG_PRINTF_LINES) {
                s_lines++;
                if (s_host && s_host->log) {
                    if (s_lines == NG_PRINTF_LINES)
                        s_host->log("gngeo: %s (and no more of these for this game)", s_line);
                    else
                        s_host->log("gngeo: %s", s_line);
                }
            }
            s_line_len = 0;
            if (*p == '\n') continue;
        }
        s_line[s_line_len++] = *p;
    }
    return n;
}

char *ng_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- the error jump ------------------------------------------------------ */

jmp_buf *ng_fatal_jump;

void ng_fatal(int code)
{
    if (ng_fatal_jump) longjmp(*ng_fatal_jump, code ? code : 1);
    /* Not armed: only the decryption routines call this, and only while a
     * game loads, when it is armed. */
    ng_log("fatal error %d outside load", code);
}

/* ---- the budget ----------------------------------------------------------- */

/* Each block carries its size in front, so ng_big_free() can give it back
 * to the budget. 16 bytes keep the block's alignment. */
#define HDR 16

/* The 68000's tables are pointers: twice as big on the Mac. The budget
 * grows by the difference so the simulator decides as the board does. */
#if UINTPTR_MAX > 0xFFFFFFFFu
#define BUDGET (NG_BUDGET + 1100u * 1024)
#else
#define BUDGET NG_BUDGET
#endif

static size_t s_used;
static size_t s_budget = BUDGET;

/* P4OS: the budget from what the board has free when a game loads, minus
 * what the system keeps (its own tasks, the frontend's frames), between
 * NG_BUDGET_MIN and NG_BUDGET_MAX. With nothing else open the board has
 * ~17.5 MB of PSRAM free; another app alive in the background takes from
 * it, and then the sprite cache is what gets smaller. */
void ng_big_budget(size_t free_psram)
{
    /* what the core already holds is no longer in free_psram but is part
     * of what it may use */
    size_t b = s_used + (free_psram > NG_SYSTEM_KEEP ? free_psram - NG_SYSTEM_KEEP : 0);
    if (b < NG_BUDGET_MIN) b = NG_BUDGET_MIN;
    if (b > NG_BUDGET_MAX) b = NG_BUDGET_MAX;
#if UINTPTR_MAX > 0xFFFFFFFFu
    b += 1100u * 1024;
#endif
    s_budget = b;
}

void *ng_big_alloc(size_t n)
{
    if (n > s_budget || s_used + n + HDR > s_budget) return NULL;
    uint8_t *p = calloc(1, n + HDR);
    if (!p) return NULL;
    *(size_t *)p = n;
    s_used += n + HDR;
    return p + HDR;
}

void ng_big_free(void *ptr)
{
    if (!ptr) return;
    uint8_t *p = (uint8_t *)ptr - HDR;
    s_used -= *(size_t *)p + HDR;
    free(p);
}

size_t ng_big_used(void)
{
    return s_used;
}

size_t ng_big_left(void)
{
    return s_used < s_budget ? s_budget - s_used : 0;
}

/* ---- save states ------------------------------------------------------------ */

void ng_st_data(ng_st_t *st, void *p, uint32_t n)
{
    if (st->failed) return;
    if (st->writing) {
        if (st->pos + n > st->cap) {
            uint32_t cap = st->cap ? st->cap : 65536;
            while (cap < st->pos + n) cap *= 2;
            uint8_t *nb = realloc(st->buf, cap);
            if (!nb) {
                st->failed = true;
                return;
            }
            st->buf = nb;
            st->cap = cap;
        }
        memcpy(st->buf + st->pos, p, n);
        st->pos += n;
        if (st->pos > st->len) st->len = st->pos;
    } else {
        if (st->pos + n > st->len) {
            st->failed = true;
            return;
        }
        memcpy(p, st->buf + st->pos, n);
        st->pos += n;
    }
}

/* ---- gngeo's progress bar (decryption) ------------------------------------- */

void gn_init_pbar(char *name, int size)
{
    (void)size;
    ng_log("%s", name ? name : "...");
}

void gn_update_pbar(int pos)
{
    (void)pos;
}

void gn_terminate_pbar(void)
{
}
