/*
 * P4OS - Python: MicroPython on a thread of its own (p4py_rt.c).
 *
 * The interpreter runs on the app's thread, never in the LVGL task: a
 * script that loops for ever only stops answering itself. The interface
 * hands it source to run and takes back what it printed; Stop raises
 * KeyboardInterrupt inside the script, as Ctrl-C does on a computer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    P4PY_FILE,                  /* a whole program                        */
    P4PY_SINGLE,                /* a REPL line: an expression's value is printed */
} p4py_mode_t;

bool     p4py_start(size_t heap_bytes);
void     p4py_stop(void);                   /* waits for the thread, frees the heap */
bool     p4py_ready(void);                  /* booted, nothing running */
bool     p4py_busy(void);
uint32_t p4py_boot_ms(void);

/* Runs src (copied). false when something is still running. */
bool     p4py_run(const char *src, const char *name, p4py_mode_t mode);
/* KeyboardInterrupt in whatever is running. */
void     p4py_interrupt(void);
/* What was printed since the last call, up to n-1 bytes; 0 when nothing. */
size_t   p4py_take_output(char *out, size_t n);
/* The last run: how long it took and whether it ended in an exception. */
uint32_t p4py_last_ms(void);
bool     p4py_last_error(void);
/* The heap: used and free bytes (from the GC's point of view). */
void     p4py_mem(size_t *used, size_t *free_);

/* input(): the script asks the console for a line. While it waits,
 * p4py_wants_input() is true and gives its prompt; the console answers with
 * p4py_give_input(). Stop while it waits is a KeyboardInterrupt as usual. */
bool     p4py_wants_input(void);
void     p4py_give_input(const char *line);
