/*
 * P4OS - 3D printers: one thread per machine.
 *
 * Each machine of the list gets a thread of its own that runs its driver's
 * loop: connect, poll (or listen, for the ones that push), carry out the
 * commands the UI queued, publish. A machine that does not answer only
 * slows its own thread, never the others or the screen.
 *
 * Everything the UI reads (the status, the history, the files, the
 * console) lives in the machine and is guarded by ONE mutex for the whole
 * app: the HAL has no way to free a mutex, and one per machine would leak
 * one every time the list changes.
 *
 * The memory of a machine belongs to this file, not to its thread: the
 * thread only raises 'done' when its driver returns, and the machine is
 * freed here once it has. That is what lets destroy() wait for every
 * thread before the loader unloads the .so on the next tick - a thread
 * still running in code that is gone would take the board down.
 */
#include "p3d.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define STACK      (12 * 1024)      /* the TLS handshake's certificate check wants ~7 KB */
#define PRIO       3
#define DEAD_MAX   (P3D_MAX * 2)

static void          *s_mx;
static p3d_machine_t *s_m[P3D_MAX];
static int            s_n;
static p3d_machine_t *s_dead[DEAD_MAX];   /* stopped, waiting for their thread to return */

static const p3d_driver_t *const DRIVERS[P3D_KIND_N] = {
    [P3D_OCTOPRINT]  = &p3d_drv_octoprint,
    [P3D_PRUSALINK]  = &p3d_drv_prusalink,
    [P3D_BAMBU]      = &p3d_drv_bambu,
    [P3D_MOONRAKER]  = &p3d_drv_moonraker,
    [P3D_DUET]       = &p3d_drv_duet,
    [P3D_ELEGOO]     = &p3d_drv_elegoo,
    [P3D_FLASHFORGE] = &p3d_drv_flashforge,
    [P3D_CREALITY]   = &p3d_drv_creality,
};

const p3d_driver_t *p3d_driver(int kind)
{
    return DRIVERS[kind >= 0 && kind < P3D_KIND_N ? kind : 0];
}

int p3d_kind_from_id(const char *id)
{
    for (int i = 0; i < P3D_KIND_N; i++) {
        if (!strcasecmp(id, DRIVERS[i]->word)) return i;
    }
    /* the names people write */
    if (!strcasecmp(id, "klipper")) return P3D_MOONRAKER;
    if (!strcasecmp(id, "prusaconnect") || !strcasecmp(id, "prusa")) return P3D_PRUSALINK;
    if (!strcasecmp(id, "rrf") || !strcasecmp(id, "reprapfirmware")) return P3D_DUET;
    if (!strcasecmp(id, "bambulab")) return P3D_BAMBU;
    if (!strcasecmp(id, "sdcp")) return P3D_ELEGOO;
    return -1;
}

void p3d_lock(void)
{
    if (s_mx) aos_hal_mutex_lock(s_mx);
}

void p3d_unlock(void)
{
    if (s_mx) aos_hal_mutex_unlock(s_mx);
}

void p3d_status_reset(p3d_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->progress = -1;
    s->elapsed_s = -1;
    s->remaining_s = -1;
    s->fan = -1;
    s->speed = -1;
    s->flow = -1;
    s->light = -1;
    s->tray_now = -1;
}

/* ---- the threads ----------------------------------------------------------- */

static void thread_fn(void *arg)
{
    p3d_machine_t *m = arg;
    const p3d_driver_t *d = p3d_driver(m->cfg.kind);
    aos_hal_log(P3D_TAG, "%s: %s at %s:%d", m->cfg.name, d->word, m->cfg.host, p3d_cfg_port(&m->cfg));
    d->run(m);
    aos_hal_log(P3D_TAG, "%s: thread out", m->cfg.name);
    __sync_synchronize();
    m->done = true;
}

static void machine_free(p3d_machine_t *m)
{
    free(m->files);
    free(m->thumb_px);
    free(m);
}

/* frees the stopped machines whose thread is gone */
static void reap(void)
{
    for (int i = 0; i < DEAD_MAX; i++) {
        if (s_dead[i] && s_dead[i]->done) {
            machine_free(s_dead[i]);
            s_dead[i] = NULL;
        }
    }
}

static void retire(p3d_machine_t *m)
{
    m->stop = true;
    __sync_synchronize();
    for (int i = 0; i < DEAD_MAX; i++) {
        if (!s_dead[i]) {
            s_dead[i] = m;
            return;
        }
    }
    /* nowhere to keep it: wait for it here (never seen; 24 changes in a row) */
    for (int t = 0; t < 100 && !m->done; t++) aos_hal_sleep_ms(50);
    if (m->done) machine_free(m);
}

static p3d_machine_t *machine_start(const p3d_cfg_t *cfg, int index)
{
    p3d_machine_t *m = p3d_psram(sizeof *m);
    if (!m) return NULL;
    memset(m, 0, sizeof *m);
    m->index = index;
    m->cfg = *cfg;
    m->poll_ms = 2500;
    p3d_status_reset(&m->st);
    m->st.state = P3D_ST_CONNECTING;
    char name[16];
    snprintf(name, sizeof name, "p3d%d", index);
    if (!aos_hal_thread_start(name, thread_fn, m, STACK, PRIO)) {
        free(m);
        return NULL;
    }
    return m;
}

void p3d_core_init(void)
{
    if (!s_mx) s_mx = aos_hal_mutex_create();
    s_n = 0;
    memset(s_m, 0, sizeof s_m);
    memset(s_dead, 0, sizeof s_dead);
}

void p3d_core_sync(const p3d_cfgs_t *cfg)
{
    reap();
    p3d_machine_t *keep[P3D_MAX] = { 0 };
    /* the same machine (byte for byte) carries on, wherever it moved */
    for (int i = 0; i < cfg->count; i++) {
        for (int j = 0; j < s_n; j++) {
            if (s_m[j] && !memcmp(&s_m[j]->cfg, &cfg->m[i], sizeof cfg->m[i])) {
                keep[i] = s_m[j];
                s_m[j] = NULL;
                break;
            }
        }
    }
    for (int j = 0; j < s_n; j++) {
        if (s_m[j]) retire(s_m[j]);
    }
    p3d_lock();
    for (int i = 0; i < cfg->count; i++) {
        if (keep[i]) keep[i]->index = i;
    }
    p3d_unlock();
    for (int i = 0; i < cfg->count; i++) {
        s_m[i] = keep[i] ? keep[i] : machine_start(&cfg->m[i], i);
    }
    s_n = cfg->count;
}

void p3d_core_stop_all(int max_ms)
{
    for (int j = 0; j < s_n; j++) {
        if (s_m[j]) retire(s_m[j]);
        s_m[j] = NULL;
    }
    s_n = 0;
    uint64_t until = aos_hal_uptime_ms() + (uint64_t)max_ms;
    for (;;) {
        reap();
        bool any = false;
        for (int i = 0; i < DEAD_MAX; i++) any |= s_dead[i] != NULL;
        if (!any) break;
        if (aos_hal_uptime_ms() > until) {
            aos_hal_log(P3D_TAG, "threads still running after %d ms", max_ms);
            break;
        }
        aos_hal_sleep_ms(20);
    }
}

int p3d_core_count(void)
{
    return s_n;
}

p3d_machine_t *p3d_core_at(int i)
{
    return i >= 0 && i < s_n ? s_m[i] : NULL;
}

/* ---- the UI's side ------------------------------------------------------------ */

bool p3d_snapshot(int i, p3d_status_t *out, uint32_t *gen)
{
    p3d_machine_t *m = p3d_core_at(i);
    if (!m) return false;
    bool changed = false;
    p3d_lock();
    if (!gen || *gen != m->gen) {
        *out = m->st;
        if (gen) *gen = m->gen;
        changed = true;
    }
    p3d_unlock();
    return changed;
}

bool p3d_send(int i, const p3d_cmd_t *c)
{
    p3d_machine_t *m = p3d_core_at(i);
    if (!m) return false;
    bool ok = false;
    p3d_lock();
    int next = (m->qt + 1) % (int)(sizeof m->q / sizeof m->q[0]);
    if (next != m->qh) {
        m->q[m->qt] = *c;
        m->qt = next;
        ok = true;
    }
    p3d_unlock();
    return ok;
}

void p3d_set_poll(int ms_detail, int detail_index, int ms_others)
{
    for (int i = 0; i < s_n; i++) {
        if (!s_m[i]) continue;
        int want = i == detail_index ? ms_detail : ms_others;
        if (s_m[i]->poll_ms > want) s_m[i]->wake = true;   /* faster now: poll at once */
        s_m[i]->poll_ms = want;
    }
}

void p3d_want_console(int i, bool on)
{
    p3d_machine_t *m = p3d_core_at(i);
    if (m) {
        if (on && !m->want_console) m->wake = true;
        m->want_console = on;
    }
}

int p3d_history(int i, int16_t *tool, int16_t *bed, int max)
{
    p3d_machine_t *m = p3d_core_at(i);
    if (!m) return 0;
    p3d_lock();
    int n = m->hist_n < max ? m->hist_n : max;
    int start = (m->hist_head - n + P3D_HIST) % P3D_HIST;
    for (int k = 0; k < n; k++) {
        tool[k] = m->hist[0][(start + k) % P3D_HIST];
        bed[k] = m->hist[1][(start + k) % P3D_HIST];
    }
    p3d_unlock();
    return n;
}

int p3d_console(int i, char *out, size_t n, uint32_t *gen)
{
    p3d_machine_t *m = p3d_core_at(i);
    if (!m || !n) return 0;
    out[0] = '\0';
    size_t o = 0;
    p3d_lock();
    if (gen) *gen = m->con_gen;
    int start = (m->con_head - m->con_n + P3D_CON_LINES) % P3D_CON_LINES;
    for (int k = 0; k < m->con_n; k++) {
        const char *l = m->con[(start + k) % P3D_CON_LINES];
        size_t len = strlen(l);
        if (o + len + 2 >= n) break;
        if (o) out[o++] = '\n';
        memcpy(out + o, l, len);
        o += len;
    }
    out[o] = '\0';
    p3d_unlock();
    return (int)o;
}

int p3d_files(int i, p3d_file_t *out, int max, uint32_t *gen, bool *busy, char *err, size_t errn)
{
    p3d_machine_t *m = p3d_core_at(i);
    if (!m) return 0;
    p3d_lock();
    int n = m->nfiles < max ? m->nfiles : max;
    if (out && m->files) memcpy(out, m->files, sizeof *out * (size_t)n);
    if (gen) *gen = m->files_gen;
    if (busy) *busy = m->files_busy;
    if (err) p3d_copy(err, errn, m->files_err);
    p3d_unlock();
    return n;
}

/* ---- the driver's side --------------------------------------------------------- */

bool p3d_wait(p3d_machine_t *m, int ms)
{
    uint64_t until = aos_hal_uptime_ms() + (uint64_t)(ms > 0 ? ms : 0);
    for (;;) {
        if (m->stop) return false;
        if (m->qh != m->qt || m->wake) {
            m->wake = false;
            return true;
        }
        if (aos_hal_uptime_ms() >= until) return true;
        aos_hal_sleep_ms(40);
    }
}

bool p3d_take(p3d_machine_t *m, p3d_cmd_t *c)
{
    bool ok = false;
    p3d_lock();
    if (m->qh != m->qt) {
        *c = m->q[m->qh];
        m->qh = (m->qh + 1) % (int)(sizeof m->q / sizeof m->q[0]);
        ok = true;
    }
    p3d_unlock();
    return ok;
}

static int16_t tenths(float c)
{
    if (c < -100 || c > 3000 || c != c) return 0;
    return (int16_t)roundf(c * 10);
}

void p3d_publish(p3d_machine_t *m)
{
    uint64_t now = aos_hal_uptime_ms();
    m->st.updated_ms = (uint32_t)now;
    m->gen++;
    if (m->st.state > P3D_ST_CONNECTING && (m->st.tool[0].on || m->st.bed.on) &&
        (m->hist_n == 0 || now - m->hist_ms >= P3D_HIST_MS)) {
        m->hist_ms = now;
        m->hist[0][m->hist_head] = tenths(m->st.tool[0].cur);
        m->hist[1][m->hist_head] = tenths(m->st.bed.cur);
        m->hist_head = (m->hist_head + 1) % P3D_HIST;
        if (m->hist_n < P3D_HIST) m->hist_n++;
    }
}

void p3d_offline(p3d_machine_t *m, const char *why)
{
    p3d_lock();
    p3d_status_t keep = m->st;
    p3d_status_reset(&m->st);
    m->st.state = P3D_ST_OFFLINE;
    m->st.caps = keep.caps;
    p3d_copy(m->st.model, sizeof m->st.model, keep.model);
    p3d_copy(m->st.fw, sizeof m->st.fw, keep.fw);
    p3d_copy(m->st.cam_url, sizeof m->st.cam_url, keep.cam_url);
    p3d_copy(m->st.msg, sizeof m->st.msg, why ? why : "");
    p3d_publish(m);
    p3d_unlock();
}

void p3d_con_add(p3d_machine_t *m, const char *prefix, const char *text)
{
    /* one ring line per text line, cut to fit */
    p3d_lock();
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && p[len - 1] == '\r') len--;
        if (len) {
            char *l = m->con[m->con_head];
            snprintf(l, P3D_CON_LEN, "%s%.*s", prefix ? prefix : "", (int)len, p);
            m->con_head = (m->con_head + 1) % P3D_CON_LINES;
            if (m->con_n < P3D_CON_LINES) m->con_n++;
        }
        p = e ? e + 1 : NULL;
    }
    m->con_gen++;
    p3d_unlock();
}

void p3d_files_begin(p3d_machine_t *m)
{
    p3d_lock();
    m->files_busy = true;
    m->files_err[0] = '\0';
    m->files_gen++;
    p3d_unlock();
}

void p3d_files_set(p3d_machine_t *m, p3d_file_t *f, int n, const char *err)
{
    p3d_lock();
    free(m->files);
    m->files = f;
    m->nfiles = f ? n : 0;
    m->files_busy = false;
    p3d_copy(m->files_err, sizeof m->files_err, err ? err : "");
    m->files_gen++;
    p3d_unlock();
}

void p3d_thumb_store(p3d_machine_t *m, const void *data, int len)
{
    uint16_t *px = NULL;
    int w = 0, h = 0;
    if (data && len > 0) {
        /* aos_hal_image_decode() reads a file: the picture goes by the card */
        char path[180];
        snprintf(path, sizeof path, "%s/imp3d_%d.img", aos_hal_path_data(), m->index);
        if (p3d_save_file(path, data, len)) {
            uint16_t *d = aos_hal_image_decode(path, 0, 0, 220, 220, false, &w, &h, NULL, NULL);
            if (d && w > 0 && h > 0) {
                px = p3d_psram((size_t)w * h * 2);
                if (px) memcpy(px, d, (size_t)w * h * 2);
            }
            if (d) aos_hal_image_free(d);
            remove(path);
        }
    }
    p3d_lock();
    free(m->thumb_px);
    m->thumb_px = px;
    m->thumb_w = px ? w : 0;
    m->thumb_h = px ? h : 0;
    m->st.thumb_gen++;
    p3d_publish(m);
    p3d_unlock();
}

bool p3d_thumb_take(int i, uint16_t **px, int *w, int *h, uint32_t *gen)
{
    p3d_machine_t *m = p3d_core_at(i);
    if (!m) return false;
    bool got = false;
    p3d_lock();
    if (*gen != m->st.thumb_gen) {
        *gen = m->st.thumb_gen;
        free(*px);
        *px = NULL;
        *w = *h = 0;
        if (m->thumb_px) {
            size_t bytes = (size_t)m->thumb_w * m->thumb_h * 2;
            *px = p3d_psram(bytes);
            if (*px) {
                memcpy(*px, m->thumb_px, bytes);
                *w = m->thumb_w;
                *h = m->thumb_h;
            }
        }
        got = true;
    }
    p3d_unlock();
    return got;
}

int p3d_backoff(p3d_machine_t *m, int fails)
{
    static const int STEPS[] = { 2000, 3000, 5000, 10000, 20000, 30000 };
    int i = fails < 1 ? 0 : fails - 1;
    if (i > 5) i = 5;
    int ms = STEPS[i];
    /* the one being looked at is tried more often */
    if (m->poll_ms <= 1500 && ms > 5000) ms = 5000;
    return ms;
}

/* ---- G-code ---------------------------------------------------------------------- */

int p3d_gcode_of(const p3d_cmd_t *c, char *out, size_t n)
{
    switch (c->op) {
    case P3D_C_TEMP: {
        int t = (int)roundf(c->v[0]);
        if (c->a == 2) return snprintf(out, n, "M140 S%d", t);
        if (c->a == 3) return snprintf(out, n, "M141 S%d", t);
        return snprintf(out, n, "M104 T%d S%d", c->a, t);
    }
    case P3D_C_JOG: {
        int k = snprintf(out, n, "G91\nG1");
        static const char AX[] = "XYZ";
        bool z = false;
        for (int i = 0; i < 3; i++) {
            if (c->v[i] != 0) {
                k += snprintf(out + k, n - (size_t)k, " %c%.2f", AX[i], (double)c->v[i]);
                if (i == 2) z = true;
            }
        }
        k += snprintf(out + k, n - (size_t)k, " F%d\nG90", z ? 600 : 6000);
        return k;
    }
    case P3D_C_HOME: {
        if (c->a == 0 || c->a == 7) return snprintf(out, n, "G28");
        return snprintf(out, n, "G28%s%s%s", c->a & 1 ? " X" : "", c->a & 2 ? " Y" : "", c->a & 4 ? " Z" : "");
    }
    case P3D_C_EXTRUDE:
        return snprintf(out, n, "M83\nG1 E%.1f F%d", (double)c->v[0], c->v[0] < 0 ? 1800 : 300);
    case P3D_C_FAN: {
        int s = (int)roundf(c->v[0] * 255 / 100);
        if (s <= 0) return snprintf(out, n, "M107");
        return snprintf(out, n, "M106 S%d", s > 255 ? 255 : s);
    }
    case P3D_C_SPEED:
        return snprintf(out, n, "M220 S%d", (int)roundf(c->v[0]));
    case P3D_C_FLOW:
        return snprintf(out, n, "M221 S%d", (int)roundf(c->v[0]));
    case P3D_C_MOTORS_OFF:
        return snprintf(out, n, "M84");
    case P3D_C_ESTOP:
        return snprintf(out, n, "M112");
    case P3D_C_GCODE:
        return snprintf(out, n, "%s", c->text);
    }
    if (n) out[0] = '\0';
    return 0;
}
