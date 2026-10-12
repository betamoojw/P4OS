/*
 * P4OS - CAS: the engine as the app sees it (cas_rt.c).
 *
 * One computation at a time, on the engine's own thread: submit, then poll
 * take() from an LVGL timer. Nothing here touches LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t arena;       /* PSRAM taken by the pools */
    size_t used;        /* allocated in them now    */
    size_t peak;
    int    pools;
    int    oom;         /* an allocation failed     */
} cas_mem_t;

bool     cas_rt_start(void);        /* constructors, Giac's context: in the background */
void     cas_rt_stop(void);         /* stops the thread and frees the arena            */
bool     cas_rt_ready(void);
bool     cas_rt_busy(void);
uint32_t cas_rt_boot_ms(void);
bool     cas_rt_submit(const char *expr);
char    *cas_rt_take(int *is_error, uint32_t *ms);  /* malloc'd; NULL until done */
void     cas_rt_interrupt(void);
void     cas_rt_mem(cas_mem_t *m);

void *cas_malloc(size_t n);
void  cas_free(void *p);
