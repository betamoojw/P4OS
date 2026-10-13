/*
 * P4OS - Python: MicroPython on a thread of its own.
 *
 * The thread boots the interpreter on its own stack (in PSRAM, like the
 * heap) and waits for work. One job at a time: the source, its name (for
 * the tracebacks) and the mode; what it prints goes into a buffer the
 * interface empties every frame. The interpreter's state lives only on
 * this thread: nothing else touches the heap or the objects.
 *
 * What MicroPython's embed port asks of the program is here too: the
 * console (mp_hal_stdout_*), the clock (time's ticks_ms...), the random
 * seed, and importing a module from the card's /python folder.
 */
#include "p4py_rt.h"

#include "port/micropython_embed.h"
#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "py/stackctrl.h"

#include "aos_hal.h"
#ifdef AOS_SIM
#include <stdlib.h>
#define big_alloc(n)    malloc(n)
#define big_free(p)     free(p)
#else
#include "esp_heap_caps.h"
#define big_alloc(n)    heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define big_free(p)     heap_caps_free(p)
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STACK_BYTES   (128u * 1024)
#define OUT_MAX       (32u * 1024)          /* printed and not yet shown */

typedef enum { ST_OFF, ST_BOOTING, ST_IDLE, ST_BUSY, ST_QUIT, ST_DONE } state_t;

static volatile state_t s_state = ST_OFF;
static void    *s_lock;
static void    *s_heap;
static size_t   s_heap_bytes;
static uint32_t s_boot_ms, s_last_ms;
static volatile bool s_last_err;
static volatile bool s_job;
static char    *s_src, s_name[64];
static p4py_mode_t s_mode;
static char    *s_out;                       /* OUT_MAX, PSRAM */
static size_t   s_out_n;
static bool     s_out_dropped;

/* --- the console ---------------------------------------------------------------- */

static void out_add(const char *s, size_t n)
{
    if (!s_out) return;
    aos_hal_mutex_lock(s_lock);
    if (s_out_n + n > OUT_MAX - 1) {
        /* nobody is reading fast enough: keep the newest */
        if (n >= OUT_MAX - 1) {
            s += n - (OUT_MAX - 1);
            n = OUT_MAX - 1;
            s_out_n = 0;
        } else {
            size_t drop = s_out_n + n - (OUT_MAX - 1);
            memmove(s_out, s_out + drop, s_out_n - drop);
            s_out_n -= drop;
        }
        s_out_dropped = true;
    }
    memcpy(s_out + s_out_n, s, n);
    s_out_n += n;
    aos_hal_mutex_unlock(s_lock);
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
    out_add(str, len);
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    out_add(str, len);
    return len;
}

void mp_hal_stdout_tx_str(const char *str)
{
    out_add(str, strlen(str));
}

size_t p4py_take_output(char *out, size_t n)
{
    if (!s_out || !n) return 0;
    aos_hal_mutex_lock(s_lock);
    size_t k = s_out_n < n - 1 ? s_out_n : n - 1;
    memcpy(out, s_out, k);
    memmove(s_out, s_out + k, s_out_n - k);
    s_out_n -= k;
    aos_hal_mutex_unlock(s_lock);
    out[k] = 0;
    return k;
}

/* --- the clock and the seed ------------------------------------------------------- */

mp_uint_t mp_hal_ticks_ms(void)  { return aos_hal_uptime_ms(); }
mp_uint_t mp_hal_ticks_us(void)  { return (mp_uint_t)aos_hal_uptime_us(); }
mp_uint_t mp_hal_ticks_cpu(void) { return (mp_uint_t)aos_hal_uptime_us(); }

void mp_hal_delay_ms(mp_uint_t ms)
{
    /* in slices, so Stop is heard during a long sleep */
    uint32_t end = aos_hal_uptime_ms() + ms;
    for (;;) {
        mp_handle_pending(true);
        uint32_t now = aos_hal_uptime_ms();
        if ((int32_t)(end - now) <= 0) break;
        uint32_t step = end - now;
        aos_hal_sleep_ms(step > 20 ? 20 : step);
    }
}

void mp_hal_delay_us(mp_uint_t us)
{
    if (us >= 1000) {
        mp_hal_delay_ms(us / 1000);
        return;
    }
    uint64_t end = aos_hal_uptime_us() + us;
    while (aos_hal_uptime_us() < end) {}
}

unsigned int p4py_random_seed(void)
{
    unsigned int s;
    aos_hal_random(&s, sizeof s);
    return s;
}

/* math.log2: MicroPython's modmath expects the C library to have it */
#undef log2
double log2(double x)
{
    return log(x) * 1.442695040888963407354163704;
}

/* --- imports from the card --------------------------------------------------------- */

/* "mod.py" or "pkg/mod.py", as the importer asks: under /sdcard/python (mod_file.c) */
bool p4py_card_path(const char *name, char *out, size_t n);

mp_import_stat_t mp_import_stat(const char *path)
{
    char full[300];
    struct stat st;
    if (!p4py_card_path(path, full, sizeof full) || stat(full, &st) != 0) return MP_IMPORT_STAT_NO_EXIST;
    return S_ISDIR(st.st_mode) ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    char full[300];
    const char *name = qstr_str(filename);
    FILE *f = p4py_card_path(name, full, sizeof full) ? fopen(full, "rb") : NULL;
    if (!f) mp_raise_OSError(MP_ENOENT);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > 512 * 1024) {
        fclose(f);
        mp_raise_OSError(MP_EFBIG);
    }
    char *buf = m_new(char, len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    return mp_lexer_new_from_str_len(filename, buf, got, len + 1);
}

/* --- the thread ---------------------------------------------------------------------- */

static void run_one(void)
{
    nlr_buf_t nlr;
    uint32_t t0 = aos_hal_uptime_ms();
    bool err = false;
    if (nlr_push(&nlr) == 0) {
        qstr name = qstr_from_str(s_name[0] ? s_name : "<stdin>");
        mp_lexer_t *lex = mp_lexer_new_from_str_len(name, s_src, strlen(s_src), 0);
        mp_parse_tree_t tree = mp_parse(lex, s_mode == P4PY_SINGLE ? MP_PARSE_SINGLE_INPUT : MP_PARSE_FILE_INPUT);
        mp_obj_t fun = mp_compile(&tree, name, s_mode == P4PY_SINGLE);
        mp_call_function_0(fun);
        nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        err = true;
    }
    /* a Stop that came after the code ended is not for the next run */
    MP_STATE_THREAD(mp_pending_exception) = MP_OBJ_NULL;
    s_last_ms = aos_hal_uptime_ms() - t0;
    s_last_err = err;
}

static void py_main(void *arg)
{
    (void)arg;
    uint32_t t0 = aos_hal_uptime_ms();
    int top;
    mp_embed_init(s_heap, s_heap_bytes, &top);
    /* the stack's limit, with room for what runs after the check */
    mp_stack_set_limit(STACK_BYTES - 16 * 1024);
    s_boot_ms = aos_hal_uptime_ms() - t0;
    aos_hal_log("python", "MicroPython up in %u ms, %u KB heap", (unsigned)s_boot_ms, (unsigned)(s_heap_bytes / 1024));
    s_state = ST_IDLE;
    while (s_state != ST_QUIT) {
        if (!s_job) {
            aos_hal_sleep_ms(10);
            continue;
        }
        s_state = ST_BUSY;
        run_one();
        free(s_src);
        s_src = NULL;
        s_job = false;
        if (s_state == ST_BUSY) s_state = ST_IDLE;
    }
    mp_embed_deinit();
    s_state = ST_DONE;
}

bool p4py_start(size_t heap_bytes)
{
    if (s_state != ST_OFF) return false;
    if (!s_lock) s_lock = aos_hal_mutex_create();
    s_out = big_alloc(OUT_MAX);
    s_heap = big_alloc(heap_bytes);
    if (!s_out || !s_heap) {
        big_free(s_out);
        big_free(s_heap);
        s_out = s_heap = NULL;
        return false;
    }
    s_heap_bytes = heap_bytes;
    s_out_n = 0;
    s_state = ST_BOOTING;
    if (!aos_hal_thread_start("python", py_main, NULL, STACK_BYTES, 3)) {
        s_state = ST_OFF;
        return false;
    }
    return true;
}

bool p4py_ready(void)    { return s_state == ST_IDLE && !s_job; }
bool p4py_busy(void)     { return s_job || s_state == ST_BUSY || s_state == ST_BOOTING; }
uint32_t p4py_boot_ms(void) { return s_boot_ms; }
uint32_t p4py_last_ms(void) { return s_last_ms; }
bool p4py_last_error(void)  { return s_last_err; }

bool p4py_run(const char *src, const char *name, p4py_mode_t mode)
{
    if (!p4py_ready()) return false;
    size_t n = strlen(src);
    s_src = malloc(n + 1);
    if (!s_src) return false;
    memcpy(s_src, src, n + 1);
    snprintf(s_name, sizeof s_name, "%s", name ? name : "");
    s_mode = mode;
    s_job = true;
    return true;
}

void p4py_interrupt(void)
{
    if (s_state == ST_BUSY) {
        /* raised at the next bytecode the script runs (the scheduler) */
        mp_sched_keyboard_interrupt();
    }
}

void p4py_mem(size_t *used, size_t *free_)
{
    *used = *free_ = 0;
    if (s_state != ST_IDLE && s_state != ST_BUSY) return;
    gc_info_t info;
    gc_info(&info);
    *used = info.used;
    *free_ = info.free;
}

void p4py_stop(void)
{
    if (s_state == ST_OFF) return;
    for (int i = 0; i < 300 && s_state == ST_BOOTING; i++) aos_hal_sleep_ms(10);
    for (int i = 0; i < 50 && s_state == ST_BUSY; i++) {
        p4py_interrupt();
        aos_hal_sleep_ms(20);
    }
    s_state = ST_QUIT;
    for (int i = 0; i < 300 && s_state != ST_DONE; i++) aos_hal_sleep_ms(10);
    if (s_state == ST_DONE) {
        big_free(s_heap);
        big_free(s_out);
    } else {
        aos_hal_log("python", "the interpreter did not stop: its memory is left");
    }
    s_heap = s_out = NULL;
    s_state = ST_OFF;
}
