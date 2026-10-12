/*
 * P4OS - CAS: the engine's stand-in for the simulator.
 *
 * Giac is built for the board only (engine/build_engine.sh), so in the
 * simulator the app runs against this: it answers every expression with a
 * note after a short pause, which is enough to lay out the interface.
 */
#ifdef AOS_SIM

#include "cas_rt.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool     s_on;
static char    *s_job;
static uint32_t s_due;

void *cas_malloc(size_t n) { return malloc(n); }
void  cas_free(void *p)    { free(p); }

bool cas_rt_start(void)       { s_on = true; return true; }
void cas_rt_stop(void)        { s_on = false; free(s_job); s_job = NULL; }
bool cas_rt_ready(void)       { return s_on; }
bool cas_rt_busy(void)        { return s_job != NULL; }
uint32_t cas_rt_boot_ms(void) { return 0; }
void cas_rt_interrupt(void)   { }

bool cas_rt_submit(const char *expr)
{
    if (!s_on || s_job) {
        return false;
    }
    s_job = strdup(expr);
    s_due = aos_hal_uptime_ms() + 150;
    return s_job != NULL;
}

char *cas_rt_take(int *is_error, uint32_t *ms)
{
    if (!s_job || (int32_t)(aos_hal_uptime_ms() - s_due) < 0) {
        return NULL;
    }
    size_t n = strlen(s_job) + 48;
    char *r = malloc(n);
    if (r) {
        snprintf(r, n, "[%s: sin motor en el simulador]", s_job);
    }
    free(s_job);
    s_job = NULL;
    *is_error = 0;
    *ms = 150;
    return r;
}

void cas_rt_mem(cas_mem_t *m)
{
    memset(m, 0, sizeof *m);
}

#endif /* AOS_SIM */
