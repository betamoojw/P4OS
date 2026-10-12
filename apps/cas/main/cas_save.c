/*
 * P4OS - CAS: writing state files off the UI's thread.
 *
 * Opening a file for writing on the card's FAT, truncating it and closing
 * it can take hundreds of milliseconds, and the views save after every
 * change: on the LVGL task that was a second's freeze per value typed into
 * the statistics sheet (measured on the board). The text is built in
 * memory where it is cheap, and a short-lived thread writes it; a lock
 * keeps two writes of the same moment from interleaving.
 */
#include "cas_save.h"

#include "aos_hal.h"
#include "mx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char  path[200];
    char *text;
    size_t len;
} job_t;

static void *s_lock;
static int s_active;             /* writes started and not finished (atomic) */

static void write_job(void *arg)
{
    job_t *j = arg;
    aos_hal_mutex_lock(s_lock);
    FILE *f = fopen(j->path, "w");
    if (f) {
        fwrite(j->text, 1, j->len, f);
        fclose(f);
    }
    aos_hal_mutex_unlock(s_lock);
    mx_free(j->text);
    mx_free(j);
    __atomic_sub_fetch(&s_active, 1, __ATOMIC_SEQ_CST);
}

void cas_save_text(const char *path, mx_str_t *text)
{
    if (!s_lock) {
        s_lock = aos_hal_mutex_create();
    }
    job_t *j = mx_alloc(sizeof *j);
    if (!j) {
        mx_str_free(text);
        return;
    }
    snprintf(j->path, sizeof j->path, "%s", path);
    j->len = text->n;
    j->text = mx_str_take(text);
    __atomic_add_fetch(&s_active, 1, __ATOMIC_SEQ_CST);
    if (!aos_hal_thread_start("cas-save", write_job, j, 6144, 2)) {
        write_job(j);           /* no thread: write it here, late but whole */
    }
}

void cas_save_wait(void)
{
    /* the writer threads run code of this .so: they have to be done before
     * the runtime unloads it */
    for (int i = 0; i < 600 && __atomic_load_n(&s_active, __ATOMIC_SEQ_CST) > 0; i++) {
        aos_hal_sleep_ms(5);
    }
}

void cas_str_json(mx_str_t *b, const char *s)
{
    if (!s) {
        mx_str_add(b, "null");
        return;
    }
    mx_str_addc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            mx_str_addc(b, '\\');
            mx_str_addc(b, (char)c);
        } else if (c < 0x20) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04x", c);
            mx_str_add(b, u);
        } else {
            mx_str_addc(b, (char)c);
        }
    }
    mx_str_addc(b, '"');
}

void cas_str_printf(mx_str_t *b, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        mx_str_addn(b, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    }
}
