/*
 * P4OS - Datalog: the engine.
 *
 * A thread of its own (aos_hal_thread_start, PSRAM stack: it touches the
 * card and the sensor service, never the flash nor LVGL) that wakes ten
 * times a second and, once per second of the clock:
 *
 *   - reads each line that is due: every N seconds (the first reading as
 *     soon as the line or the logging starts, then on the clock's grid, so
 *     "every 5 min" lands on :00, :05...), or at the minutes of the day it
 *     lists (only with the clock set);
 *   - appends the reading to its CSV, with a note when it failed or when an
 *     alarm starts, goes on or ends;
 *   - checks the alarms, with hysteresis: a high alarm starts above hi and
 *     ends below hi - hyst, a low one starts below lo and ends above
 *     lo + hyst;
 *   - drives the alarms' GPIO outputs, each at its active level while any
 *     line that names it is in alarm, at the other level otherwise (and
 *     while paused).
 *
 * The pins of the digital inputs and of the outputs are claimed as
 * "Datalog" while the lines that use them exist, so Bus or another app
 * cannot drive them under a relay; a pin someone else holds is tried again
 * every ten seconds.
 *
 * The UI talks to it through the configuration and the latest readings,
 * under one mutex, and through a short queue of alarm events that the app's
 * tick turns into a sound and a message on screen.
 */
#include "dl.h"

static void running_shown(bool on);

#define QMAX 8

typedef struct {
    int8_t  pin;
    uint8_t pull;       /* inputs */
    bool    high;       /* outputs: the active level */
    int8_t  level;      /* outputs: what it is at now, -1 not set */
} dl_pin_t;

static struct {
    void       *mx;
    dl_cfg_t    cfg;                    /* shared, under mx */
    dl_rt_t     rt[DL_MAX_CH];
    uint32_t    seq, cfg_seq;
    dl_alert_t  q[QMAX];
    int         qh, qn;
    volatile bool stop, alive;

    /* the thread's own */
    dl_cfg_t    local;
    uint32_t    local_seq;
    int64_t     next[DL_MAX_CH];
    int32_t     at_done[DL_MAX_CH];
    uint8_t     alarm[DL_MAX_CH];
    char        pin_err[DL_MAX_CH][64];
    dl_pin_t    in[DL_MAX_CH], out[DL_MAX_CH];
    int         nin, nout;
    bool        was_valid, was_running;
    uint32_t    retry_ms;
} E;

void dl_lock(void) { if (E.mx) aos_hal_mutex_lock(E.mx); }
void dl_unlock(void) { if (E.mx) aos_hal_mutex_unlock(E.mx); }
dl_cfg_t *dl_cfg(void) { return &E.cfg; }
dl_rt_t *dl_rt(int i) { return &E.rt[i]; }
uint32_t dl_seq(void) { return E.seq; }

int64_t dl_now(bool *valid)
{
    if (aos_hal_time_is_valid()) {
        struct tm tm;
        aos_hal_time_now(&tm);
        if (valid) *valid = true;
        return dl_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    if (valid) *valid = false;
    return (int64_t)(aos_hal_uptime_ms() / 1000);
}

void dl_fmt_when(char *out, size_t n)
{
    bool valid;
    int64_t t = dl_now(&valid);
    if (!valid) { snprintf(out, n, "+%u s", (unsigned)t); return; }
    int y, mo, d, hh, mi, ss;
    dl_uncivil(t, &y, &mo, &d, &hh, &mi, &ss);
    snprintf(out, n, "%02d/%02d %02d:%02d:%02d", d, mo, hh, mi, ss);
}

const char *dl_pin_owner(int gpio)
{
    const char *o = aos_io_owner(gpio);
    return o && strcmp(o, DL_OWNER) ? o : NULL;
}

/* ---- the latest readings ---- */

static void rt_fill(dl_rt_t *r, const dl_ch_t *c)
{
    memset(r, 0, sizeof *r);
    float last = NAN;
    int n = dl_csv_tail(c->id, r->hist, DL_HIST, &last, r->when, sizeof r->when);
    r->hn = n;
    r->hhead = n % DL_HIST;
    if (n) { r->have = r->ok = true; r->v = last; }
}

static void push_hist(dl_rt_t *r, float v)
{
    r->hist[r->hhead] = v;
    r->hhead = (r->hhead + 1) % DL_HIST;
    if (r->hn < DL_HIST) r->hn++;
}

static void push_alert(const dl_alert_t *a)
{
    if (E.qn == QMAX) { E.qh = (E.qh + 1) % QMAX; E.qn--; }
    E.q[(E.qh + E.qn) % QMAX] = *a;
    E.qn++;
}

bool dl_alert_pop(dl_alert_t *out)
{
    dl_lock();
    bool any = E.qn > 0;
    if (any) {
        *out = E.q[E.qh];
        E.qh = (E.qh + 1) % QMAX;
        E.qn--;
    }
    dl_unlock();
    return any;
}

int dl_alarms_on(void)
{
    int n = 0;
    dl_lock();
    for (int i = 0; i < E.cfg.n; i++) n += E.rt[i].alarm != DL_AL_NONE;
    dl_unlock();
    return n;
}

/* ---- pins ---- */

static int find_pin(const dl_pin_t *a, int n, int pin)
{
    for (int i = 0; i < n; i++) if (a[i].pin == pin) return i;
    return -1;
}

static void set_out(dl_pin_t *o, bool active)
{
    int level = active == o->high ? 1 : 0;
    if (o->level == level) return;
    if (aos_io_gpio_set(o->pin, level)) o->level = (int8_t)level;
}

/* Claims what the lines need now and lets go of what they no longer do */
static void pins_sync(bool retry)
{
    const dl_cfg_t *c = &E.local;
    /* let go */
    for (int i = 0; i < E.nin;) {
        bool want = false;
        for (int k = 0; k < c->n; k++) {
            const dl_ch_t *ch = &c->ch[k];
            want |= ch->kind == DL_K_GPIO && !ch->analog && ch->pin == E.in[i].pin && ch->pull == E.in[i].pull;
        }
        if (want) { i++; continue; }
        aos_io_release(E.in[i].pin, DL_OWNER);
        E.in[i] = E.in[--E.nin];
    }
    for (int i = 0; i < E.nout;) {
        bool want = false;
        for (int k = 0; k < c->n; k++) {
            const dl_ch_t *ch = &c->ch[k];
            want |= (ch->lo_on || ch->hi_on) && ch->out_pin == E.out[i].pin && ch->out_high == E.out[i].high;
        }
        if (want) { i++; continue; }
        set_out(&E.out[i], false);
        aos_io_release(E.out[i].pin, DL_OWNER);
        E.out[i] = E.out[--E.nout];
    }
    /* take */
    for (int k = 0; k < c->n; k++) {
        const dl_ch_t *ch = &c->ch[k];
        if (ch->kind == DL_K_GPIO && !ch->analog && ch->pin >= 0) {
            if (find_pin(E.in, E.nin, ch->pin) >= 0) E.pin_err[k][0] = 0;
            else if (retry || !E.pin_err[k][0]) {
                aos_gpio_mode_t m = ch->pull == DL_PULL_UP ? AOS_GPIO_INPUT_PULLUP
                                  : ch->pull == DL_PULL_DOWN ? AOS_GPIO_INPUT_PULLDOWN : AOS_GPIO_INPUT;
                if (find_pin(E.out, E.nout, ch->pin) < 0 && aos_io_gpio_mode(ch->pin, m, DL_OWNER)) {
                    E.in[E.nin++] = (dl_pin_t){ .pin = ch->pin, .pull = ch->pull, .level = -1 };
                    E.pin_err[k][0] = 0;
                } else {
                    const char *o = dl_pin_owner(ch->pin);
                    if (o) snprintf(E.pin_err[k], sizeof E.pin_err[k], _("El GPIO%d lo tiene %s"), ch->pin, o);
                    else snprintf(E.pin_err[k], sizeof E.pin_err[k], _("No se pudo tomar el GPIO%d"), ch->pin);
                }
            }
        } else {
            E.pin_err[k][0] = 0;
        }
        if ((ch->lo_on || ch->hi_on) && ch->out_pin >= 0 && find_pin(E.out, E.nout, ch->out_pin) < 0
            && find_pin(E.in, E.nin, ch->out_pin) < 0 && E.nout < DL_MAX_CH) {
            if (aos_io_gpio_mode(ch->out_pin, AOS_GPIO_OUTPUT, DL_OWNER)) {
                E.out[E.nout] = (dl_pin_t){ .pin = ch->out_pin, .high = ch->out_high, .level = -1 };
                set_out(&E.out[E.nout], false);
                E.nout++;
            } else if (retry) {
                aos_hal_log("datalog", "alarm output GPIO%d not taken (%s)", ch->out_pin,
                            dl_pin_owner(ch->out_pin) ? dl_pin_owner(ch->out_pin) : "?");
            }
        }
    }
}

static void pins_drive(void)
{
    for (int i = 0; i < E.nout; i++) {
        bool active = false;
        if (E.local.running)
            for (int k = 0; k < E.local.n; k++) {
                const dl_ch_t *ch = &E.local.ch[k];
                active |= ch->out_pin == E.out[i].pin && E.alarm[k] != DL_AL_NONE;
            }
        set_out(&E.out[i], active);
    }
}

static void pins_release_all(void)
{
    for (int i = 0; i < E.nout; i++) {
        set_out(&E.out[i], false);
        aos_io_release(E.out[i].pin, DL_OWNER);
    }
    for (int i = 0; i < E.nin; i++) aos_io_release(E.in[i].pin, DL_OWNER);
    E.nin = E.nout = 0;
}

/* ---- the thread ---- */

/* A new copy of the lines; the schedule and the alarm of each line follow
 * it by id when lines come, go or move */
static void cfg_sync(void)
{
    if (E.local_seq == E.cfg_seq) return;
    static dl_cfg_t old;
    static int64_t next[DL_MAX_CH];
    static int32_t at_done[DL_MAX_CH];
    static uint8_t alarm[DL_MAX_CH];
    old = E.local;
    memcpy(next, E.next, sizeof next);
    memcpy(at_done, E.at_done, sizeof at_done);
    memcpy(alarm, E.alarm, sizeof alarm);
    dl_lock();
    E.local = E.cfg;
    E.local_seq = E.cfg_seq;
    dl_unlock();
    for (int i = 0; i < E.local.n; i++) {
        E.next[i] = 0;
        E.at_done[i] = -1;
        E.alarm[i] = DL_AL_NONE;
        for (int j = 0; j < old.n; j++) {
            if (strcmp(old.ch[j].id, E.local.ch[i].id)) continue;
            E.alarm[i] = alarm[j];
            E.at_done[i] = at_done[j];
            /* a line that changed reads again at once, to show the change */
            if (!memcmp(&old.ch[j], &E.local.ch[i], sizeof old.ch[j])) E.next[i] = next[j];
        }
        if (!E.local.ch[i].lo_on && !E.local.ch[i].hi_on) E.alarm[i] = DL_AL_NONE;
    }
    if (E.local.running && !E.was_running) for (int i = 0; i < E.local.n; i++) E.next[i] = 0;
    E.was_running = E.local.running;
    pins_sync(true);
}

static bool due(int i, int64_t now, bool valid)
{
    const dl_ch_t *c = &E.local.ch[i];
    if (c->sched_at) {
        if (!valid) return false;
        int32_t key = (int32_t)(now / 60);
        int m = (int)(now % 86400) / 60;
        for (int k = 0; k < c->n_at; k++)
            if (c->at[k] == m && E.at_done[i] != key) { E.at_done[i] = key; return true; }
        return false;
    }
    int64_t every = c->every_s ? c->every_s : 1;
    if (E.next[i] && now < E.next[i]) return false;
    E.next[i] = (now / every + 1) * every;
    return true;
}

static void read_line(int i, int64_t now, bool valid)
{
    const dl_ch_t *c = &E.local.ch[i];
    float v = NAN;
    char err[64] = "";
    bool ok;
    if (E.pin_err[i][0]) { dl_scpy(err, sizeof err, E.pin_err[i]); ok = false; }
    else ok = dl_src_read(c, &v, err, sizeof err);

    char date[16], tim[16];
    if (valid) {
        int y, mo, d, hh, mi, ss;
        dl_uncivil(now, &y, &mo, &d, &hh, &mi, &ss);
        snprintf(date, sizeof date, "%04d-%02d-%02d", y, mo, d);
        snprintf(tim, sizeof tim, "%02d:%02d:%02d", hh, mi, ss);
    } else {
        dl_scpy(date, sizeof date, _("sin hora"));
        snprintf(tim, sizeof tim, "+%u", (unsigned)now);
    }

    /* the alarm */
    uint8_t prev = E.alarm[i], now_al = prev;
    char note[96] = "";
    if (ok && (c->lo_on || c->hi_on)) {
        float h = c->hyst > 0 ? c->hyst : 0;
        if (prev == DL_AL_HIGH) now_al = c->hi_on && v >= c->hi - h ? DL_AL_HIGH : DL_AL_NONE;
        else if (prev == DL_AL_LOW) now_al = c->lo_on && v <= c->lo + h ? DL_AL_LOW : DL_AL_NONE;
        if (now_al == DL_AL_NONE) {
            if (c->hi_on && v > c->hi) now_al = DL_AL_HIGH;
            else if (c->lo_on && v < c->lo) now_al = DL_AL_LOW;
        }
        char lim[32];
        if (now_al != prev) {
            if (now_al == DL_AL_HIGH) { dl_fmt_value(c, c->hi, true, lim, sizeof lim); snprintf(note, sizeof note, _("ALTA: más de %s"), lim); }
            else if (now_al == DL_AL_LOW) { dl_fmt_value(c, c->lo, true, lim, sizeof lim); snprintf(note, sizeof note, _("BAJA: menos de %s"), lim); }
            else dl_scpy(note, sizeof note, _("Vuelve a lo normal"));
        } else if (now_al == DL_AL_HIGH) dl_scpy(note, sizeof note, _("alta"));
        else if (now_al == DL_AL_LOW) dl_scpy(note, sizeof note, _("baja"));
    } else if (!c->lo_on && !c->hi_on) {
        now_al = DL_AL_NONE;
    }
    if (!ok) dl_scpy(note, sizeof note, err);
    E.alarm[i] = now_al;

    bool wrote = dl_csv_append(E.local.semicolon, c, date, tim, ok, v, note);

    dl_lock();
    /* the line may have gone while it was read: find it again by id */
    int k = -1;
    for (int j = 0; j < E.cfg.n; j++) if (!strcmp(E.cfg.ch[j].id, c->id)) k = j;
    if (k >= 0) {
        dl_rt_t *r = &E.rt[k];
        r->ok = ok;
        if (ok) { r->have = true; r->v = v; r->reads++; push_hist(r, v); }
        else r->errors++;
        dl_fmt_when(r->when, sizeof r->when);
        if (!ok) dl_scpy(r->err, sizeof r->err, err);
        else if (!wrote) dl_scpy(r->err, sizeof r->err, _("No se pudo escribir en la tarjeta"));
        else r->err[0] = 0;
        r->alarm = now_al;
        if (now_al != prev) {
            dl_alert_t a = { .ch = k, .alarm = now_al, .v = v };
            dl_scpy(a.name, sizeof a.name, c->name);
            char val[32];
            dl_fmt_value(c, v, true, val, sizeof val);
            snprintf(a.text, sizeof a.text, "%.24s · %.64s", val, note);
            if (c->sound || now_al == DL_AL_NONE) push_alert(&a);
        }
        E.seq++;
    }
    dl_unlock();
}

static void thread_fn(void *arg)
{
    (void)arg;
    int64_t last = -1;
    E.was_valid = aos_hal_time_is_valid();
    E.retry_ms = (uint32_t)aos_hal_uptime_ms();
    while (!E.stop) {
        cfg_sync();
        uint32_t ms = (uint32_t)aos_hal_uptime_ms();
        if ((int32_t)(ms - E.retry_ms) > 10000) { E.retry_ms = ms; pins_sync(true); }
        bool valid;
        int64_t now = dl_now(&valid);
        if (valid != E.was_valid) {
            /* the clock came (or went): the grid starts again on it */
            E.was_valid = valid;
            for (int i = 0; i < E.local.n; i++) E.next[i] = 0;
            last = -1;
        }
        if (now != last) {
            last = now;
            if (E.local.running)
                for (int i = 0; i < E.local.n && !E.stop; i++)
                    if (dl_src_ready(&E.local.ch[i]) && due(i, now, valid)) read_line(i, now, valid);
        }
        pins_drive();
        aos_hal_sleep_ms(100);
    }
    pins_release_all();
    E.alive = false;
}

/* ---- the UI's calls ---- */

static void changed(void)
{
    E.cfg_seq++;
    E.seq++;
    if (!dl_cfg_save(&E.cfg)) aos_hal_log("datalog", "cannot write datalog.txt");
}

void dl_engine_start(void)
{
    if (!E.mx) E.mx = aos_hal_mutex_create();
    if (E.alive) return;
    static dl_cfg_t cfg;
    if (!dl_cfg_load(&cfg)) cfg.running = false;
    dl_lock();
    E.cfg = cfg;
    for (int i = 0; i < E.cfg.n; i++) rt_fill(&E.rt[i], &E.cfg.ch[i]);
    E.qn = E.qh = 0;
    E.cfg_seq++;
    E.seq++;
    dl_unlock();
    memset(&E.local, 0, sizeof E.local);
    E.local_seq = 0;
    E.was_running = false;
    E.nin = E.nout = 0;
    aos_sensors_start();
    running_shown(E.cfg.running);
    E.stop = false;
    E.alive = true;
    if (!aos_hal_thread_start("datalog", thread_fn, NULL, 8192, 3)) {
        E.alive = false;
        aos_hal_log("datalog", "no thread");
    }
}

void dl_engine_stop(void)
{
    E.stop = true;
    /* the .so goes away after destroy(): the thread must be out of it */
    for (int i = 0; i < 300 && E.alive; i++) aos_hal_sleep_ms(20);
    if (E.alive) aos_hal_log("datalog", "the thread did not stop");
}

bool dl_ch_put(int i, const dl_ch_t *c)
{
    dl_ch_t n = *c;
    dl_lock();
    if (i < 0) {
        if (E.cfg.n >= DL_MAX_CH) { dl_unlock(); return false; }
        /* a file name nobody has, among the lines and the old CSVs */
        char base[24];
        dl_cfg_slug(n.name, base, 21);
        dl_scpy(n.id, sizeof n.id, base);
        for (int k = 2;; k++) {
            bool taken = dl_csv_size(n.id) >= 0;
            for (int j = 0; j < E.cfg.n; j++) taken |= !strcmp(E.cfg.ch[j].id, n.id);
            if (!taken || k > 99) break;
            snprintf(n.id, sizeof n.id, "%.20s_%d", base, k % 100);
        }
        i = E.cfg.n++;
        memset(&E.rt[i], 0, sizeof E.rt[i]);
    } else if (i >= E.cfg.n) {
        dl_unlock();
        return false;
    } else {
        dl_scpy(n.id, sizeof n.id, E.cfg.ch[i].id);
        /* another source: what is in memory was of the old one */
        const dl_ch_t *o = &E.cfg.ch[i];
        if (o->kind != n.kind || o->analog != n.analog || strcmp(o->sensor, n.sensor) || strcmp(o->key, n.key)
            || o->pin != n.pin || o->sys != n.sys || o->an_mode != n.an_mode)
            memset(&E.rt[i], 0, sizeof E.rt[i]);
        E.rt[i].alarm = DL_AL_NONE;
    }
    E.cfg.ch[i] = n;
    changed();
    dl_unlock();
    return true;
}

void dl_ch_delete(int i)
{
    dl_lock();
    if (i >= 0 && i < E.cfg.n) {
        memmove(&E.cfg.ch[i], &E.cfg.ch[i + 1], sizeof E.cfg.ch[0] * (size_t)(E.cfg.n - i - 1));
        memmove(&E.rt[i], &E.rt[i + 1], sizeof E.rt[0] * (size_t)(E.cfg.n - i - 1));
        E.cfg.n--;
        changed();
    }
    dl_unlock();
}

/* Logging or not, as the system sees it: the app starts with the board
 * while it logs, and a red dot in the status bar says so */
static void running_shown(bool on)
{
    aos_ui_boot_start_set(DL_APP_ID, on);
    aos_ui_status_icon_set(DL_APP_ID, on ? AOS_SYM_RECORD : NULL, 0xFF453A);
}

void dl_set_running(bool on)
{
    dl_lock();
    bool was = E.cfg.running;
    E.cfg.running = on;
    if (was != on) changed();
    dl_unlock();
    /* a board that boots goes on logging, unless it was paused */
    running_shown(on);
}

void dl_set_semicolon(bool on)
{
    dl_lock();
    if (E.cfg.semicolon != on) { E.cfg.semicolon = on; changed(); }
    dl_unlock();
}

void dl_reload(void)
{
    static dl_cfg_t cfg;
    static dl_rt_t rt[DL_MAX_CH];
    if (!dl_cfg_load(&cfg)) return;
    dl_lock();
    for (int i = 0; i < cfg.n; i++) {
        int j = -1;
        for (int k = 0; k < E.cfg.n; k++) if (!strcmp(E.cfg.ch[k].id, cfg.ch[i].id)) j = k;
        if (j >= 0) rt[i] = E.rt[j];
        else memset(&rt[i], 0, sizeof rt[i]);
    }
    bool run_changed = cfg.running != E.cfg.running;
    E.cfg = cfg;
    memcpy(E.rt, rt, sizeof rt);
    E.cfg_seq++;
    E.seq++;
    dl_unlock();
    if (run_changed) running_shown(cfg.running);
}
