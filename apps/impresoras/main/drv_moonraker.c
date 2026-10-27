/*
 * P4OS - 3D printers: Klipper through Moonraker (moonraker.readthedocs.io).
 *
 * The API that Mainsail and Fluidd use, on port 7125 (or 80 behind their
 * nginx): Voron, RatRig, Sovol SV06/SV07/SV08, Elegoo Neptune 4, Qidi,
 * Creality K1/K2 with Moonraker, Anycubic Kobra with Kobra-Klipper, a
 * Prusa with Klipper... A key is only needed when Moonraker asks for one
 * ([authorization]): X-Api-Key.
 *
 *   GET  /printer/objects/query?...   one request for everything; objects
 *                                     the printer does not have are left out
 *   GET  /server/files/metadata       the slicer's estimate and pictures
 *   GET  /server/gcode_store          the console, answers included
 *   POST /printer/gcode/script        any G-code
 *   POST /printer/print/pause|resume|cancel|start
 *
 * The remaining time is the slicer's estimate minus what has printed when
 * the file has one, else what the progress so far projects.
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define REQ_MS      6000
#define BODY_MAX    (48 * 1024)
#define THUMB_MAX   (256 * 1024)

static const char QUERY[] =
    "/printer/objects/query?webhooks&print_stats&virtual_sdcard&display_status&extruder&extruder1&heater_bed"
    "&heater_generic%20chamber&temperature_sensor%20chamber&fan&gcode_move&toolhead&pause_resume";

typedef struct {
    p3d_http_t h;
    char       meta_of[160];
    float      est_s;           /* the slicer's, < 0 none */
    int        layers;
    double     con_last;        /* the gcode_store's newest time seen */
    bool       info;
} moon_t;

static int get(moon_t *k, const char *path, p3d_resp_t *r, int max)
{
    return p3d_http(&k->h, "GET", path, NULL, 0, NULL, r, max, REQ_MS);
}

/* Klipper's reason for a refused command goes to the console */
static int post(p3d_machine_t *m, moon_t *k, const char *path)
{
    p3d_resp_t r;
    int st = p3d_http(&k->h, "POST", path, "", 0, "application/json", &r, 8192, 15000);
    char msg[120] = "";
    if (st >= 400 && r.body && pj_str(r.body, "error.message", msg, sizeof msg) && msg[0]) p3d_con_add(m, "! ", msg);
    p3d_resp_free(&r);
    return st;
}

static void heater(const char *st, const char *name, p3d_temp_t *t)
{
    char path[64];
    snprintf(path, sizeof path, "%s.temperature", name);
    float v;
    if (pj_num(st, path, &v)) {
        t->on = true;
        t->cur = v;
        snprintf(path, sizeof path, "%s.target", name);
        t->target = pj_num(st, path, &v) ? v : 0;
    }
}

static void meta_read(p3d_machine_t *m, moon_t *k, const char *file)
{
    p3d_copy(k->meta_of, sizeof k->meta_of, file);
    k->est_s = -1;
    k->layers = 0;
    char enc[300], url[340], rel[200] = "";
    p3d_url_enc(enc, sizeof enc, file);
    snprintf(url, sizeof url, "/server/files/metadata?filename=%s", enc);
    p3d_resp_t r;
    if (get(k, url, &r, 32 * 1024) == 200) {
        float v;
        if (pj_num(r.body, "result.estimated_time", &v)) k->est_s = v;
        int n;
        if (pj_int(r.body, "result.layer_count", &n)) k->layers = n;
        /* the biggest picture up to 400 px */
        const char *th = pj_get(r.body, "result.thumbnails");
        int best = 0;
        for (int i = 0; th && i < 8; i++) {
            char p[24];
            snprintf(p, sizeof p, "%d.width", i);
            int w;
            if (!pj_int(th, p, &w)) break;
            if (w > best && w <= 400) {
                best = w;
                snprintf(p, sizeof p, "%d.relative_path", i);
                pj_str(th, p, rel, sizeof rel);
            }
        }
    }
    p3d_resp_free(&r);
    bool got = false;
    if (rel[0]) {
        /* relative to the file's folder */
        char dir[160];
        p3d_copy(dir, sizeof dir, file);
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = '\0';
        else dir[0] = '\0';
        char both[360];
        snprintf(both, sizeof both, "%s%s", dir, rel);
        p3d_url_enc(enc, sizeof enc, both);
        snprintf(url, sizeof url, "/server/files/gcodes/%s", enc);
        if (get(k, url, &r, THUMB_MAX) == 200 && r.len > 0 && !r.cut) {
            p3d_thumb_store(m, r.body, r.len);
            got = true;
        }
        p3d_resp_free(&r);
    }
    if (!got) p3d_thumb_store(m, NULL, 0);
}

static void info_read(p3d_machine_t *m, moon_t *k)
{
    p3d_resp_t r;
    char ver[48] = "", host[48] = "", cam[192] = "";
    if (get(k, "/printer/info", &r, 8192) == 200) {
        pj_str(r.body, "result.software_version", ver, sizeof ver);
        pj_str(r.body, "result.hostname", host, sizeof host);
    }
    p3d_resp_free(&r);
    if (get(k, "/server/webcams/list", &r, 16 * 1024) == 200) {
        char url[180] = "";
        bool en = true;
        for (int i = 0; i < 4 && !cam[0]; i++) {
            char p[48];
            snprintf(p, sizeof p, "result.webcams.%d.enabled", i);
            en = true;
            pj_bool(r.body, p, &en);
            /* the stream first, as with OctoPrint */
            snprintf(p, sizeof p, "result.webcams.%d.stream_url", i);
            if (!pj_str(r.body, p, url, sizeof url) || !url[0]) {
                snprintf(p, sizeof p, "result.webcams.%d.snapshot_url", i);
                if (!pj_str(r.body, p, url, sizeof url)) break;
            }
            if (!en || !url[0]) continue;
            if (url[0] == '/') snprintf(cam, sizeof cam, "%s://%.80s%.100s", k->h.tls ? "https" : "http", m->cfg.host, url);
            else if (!strncmp(url, "http", 4)) p3d_copy(cam, sizeof cam, url);
        }
    }
    p3d_resp_free(&r);
    p3d_lock();
    snprintf(m->st.fw, sizeof m->st.fw, "Klipper %.38s", ver);
    if (host[0]) p3d_copy(m->st.model, sizeof m->st.model, host);
    p3d_copy(m->st.cam_url, sizeof m->st.cam_url, cam);
    p3d_unlock();
    k->info = true;
}

static void console_read(p3d_machine_t *m, moon_t *k)
{
    p3d_resp_t r;
    if (get(k, "/server/gcode_store?count=60", &r, 32 * 1024) == 200) {
        const char *arr = pj_get(r.body, "result.gcode_store");
        for (int i = 0; arr && i < 60; i++) {
            char p[32], msg[160], type[16] = "", tt[32];
            snprintf(p, sizeof p, "%d.time", i);
            if (!pj_str(arr, p, tt, sizeof tt)) break;
            /* epoch seconds with decimals: a float has 128 s of resolution there */
            double t = strtod(tt, NULL);
            if (t <= k->con_last) continue;
            k->con_last = t;
            snprintf(p, sizeof p, "%d.message", i);
            if (!pj_str(arr, p, msg, sizeof msg)) continue;
            snprintf(p, sizeof p, "%d.type", i);
            pj_str(arr, p, type, sizeof type);
            p3d_con_add(m, !strcmp(type, "command") ? "> " : "", msg);
        }
    }
    p3d_resp_free(&r);
}

static int state_of(const char *ps, const char *wh)
{
    if (strcmp(wh, "ready")) return !strcmp(wh, "startup") ? P3D_ST_BUSY : P3D_ST_ERROR;
    if (!strcmp(ps, "printing")) return P3D_ST_PRINTING;
    if (!strcmp(ps, "paused")) return P3D_ST_PAUSED;
    if (!strcmp(ps, "complete")) return P3D_ST_DONE;
    if (!strcmp(ps, "cancelled")) return P3D_ST_CANCELLED;
    if (!strcmp(ps, "error")) return P3D_ST_ERROR;
    return P3D_ST_IDLE;
}

static int poll(p3d_machine_t *m, moon_t *k)
{
    p3d_resp_t r;
    int st = get(k, QUERY, &r, BODY_MAX);
    if (st != 200) {
        char why[64];
        if (st < 0) p3d_copy(why, sizeof why, _("No responde"));
        else if (st == 401 || st == 403) p3d_copy(why, sizeof why, _("Moonraker pide una clave"));
        else snprintf(why, sizeof why, _("Contestó %d"), st);
        p3d_offline(m, why);
        p3d_resp_free(&r);
        return -1;
    }
    if (!k->info) info_read(m, k);
    const char *s = pj_get(r.body, "result.status");
    char ps[24] = "", wh[24] = "ready", file[160] = "";
    pj_str(s, "print_stats.state", ps, sizeof ps);
    pj_str(s, "webhooks.state", wh, sizeof wh);
    pj_str(s, "print_stats.filename", file, sizeof file);
    if (file[0] && strcmp(file, k->meta_of)) meta_read(m, k, file);
    if (!file[0] && k->meta_of[0]) {
        k->meta_of[0] = '\0';
        p3d_thumb_store(m, NULL, 0);
    }

    p3d_lock();
    p3d_status_t *t = &m->st;
    t->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_TEMP | P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_EXTRUDE |
              P3D_CAP_FAN | P3D_CAP_SPEED | P3D_CAP_FLOW | P3D_CAP_GCODE | P3D_CAP_FILES | P3D_CAP_ESTOP |
              P3D_CAP_MOTORS_OFF | P3D_CAP_REPLIES | P3D_CAP_DELETE;
    t->state = state_of(ps, wh);
    snprintf(t->raw, sizeof t->raw, "%s", strcmp(wh, "ready") ? wh : ps);
    t->msg[0] = '\0';
    if (strcmp(wh, "ready")) pj_str(s, "webhooks.state_message", t->msg, sizeof t->msg);
    else pj_str(s, "print_stats.message", t->msg, sizeof t->msg);
    t->tool[0].on = t->tool[1].on = t->bed.on = t->chamber.on = false;
    heater(s, "extruder", &t->tool[0]);
    heater(s, "extruder1", &t->tool[1]);
    heater(s, "heater_bed", &t->bed);
    heater(s, "heater_generic chamber", &t->chamber);
    if (!t->chamber.on) heater(s, "temperature_sensor chamber", &t->chamber);
    if (t->chamber.on && pj_get(s, "heater_generic chamber")) t->caps |= P3D_CAP_CHAMBER;
    p3d_copy(t->file, sizeof t->file, file);
    float v, dur = 0;
    bool active = t->state == P3D_ST_PRINTING || t->state == P3D_ST_PAUSED;
    float prog = -1;
    if (pj_num(s, "display_status.progress", &v) && v > 0) prog = v * 100;
    else if (pj_num(s, "virtual_sdcard.progress", &v)) prog = v * 100;
    t->progress = active || t->state == P3D_ST_DONE ? (t->state == P3D_ST_DONE ? 100 : prog) : -1;
    if (pj_num(s, "print_stats.print_duration", &v)) dur = v;
    t->elapsed_s = active || t->state == P3D_ST_DONE ? (int32_t)dur : -1;
    t->remaining_s = -1;
    if (active) {
        if (k->est_s > 0 && dur <= k->est_s) t->remaining_s = (int32_t)(k->est_s - dur);
        else if (prog > 1) t->remaining_s = (int32_t)(dur * (100 - prog) / prog);
    } else if (t->state == P3D_ST_DONE) t->remaining_s = 0;
    int n;
    t->layer = pj_int(s, "print_stats.info.current_layer", &n) ? n : 0;
    t->layers = pj_int(s, "print_stats.info.total_layer", &n) && n > 0 ? n : k->layers;
    t->fan = pj_num(s, "fan.speed", &v) ? (int)roundf(v * 100) : -1;
    t->speed = pj_num(s, "gcode_move.speed_factor", &v) ? (int)roundf(v * 100) : -1;
    t->flow = pj_num(s, "gcode_move.extrude_factor", &v) ? (int)roundf(v * 100) : -1;
    const char *pos = pj_get(s, "toolhead.position");
    t->has_pos = pos && pj_num(pos, "0", &t->pos[0]) && pj_num(pos, "1", &t->pos[1]) && pj_num(pos, "2", &t->pos[2]);
    p3d_publish(m);
    p3d_unlock();
    p3d_resp_free(&r);
    if (m->want_console) console_read(m, k);
    return 0;
}

static bool file_cb(const char *key, int kl, const char *v, void *ctx)
{
    (void)key;
    (void)kl;
    struct { p3d_file_t *f; int n; } *fl = ctx;
    if (fl->n >= P3D_FILES_MAX) return false;
    p3d_file_t *f = &fl->f[fl->n];
    memset(f, 0, sizeof *f);
    char path[192] = "";
    if (!pj_str(v, "path", path, sizeof path) && !pj_str(v, "filename", path, sizeof path)) return true;
    if (path[0] == '.' || strstr(path, "/.")) return true;
    p3d_copy(f->path, sizeof f->path, path);
    p3d_copy(f->name, sizeof f->name, path);
    float x;
    f->size = pj_num(v, "size", &x) ? (uint32_t)x : 0;
    f->date = pj_num(v, "modified", &x) ? (uint32_t)x : 0;
    f->est_s = -1;
    fl->n++;
    return true;
}

static void files_read(p3d_machine_t *m, moon_t *k)
{
    p3d_files_begin(m);
    p3d_resp_t r;
    int st = get(k, "/server/files/list?root=gcodes", &r, 256 * 1024);
    if (st != 200) {
        p3d_resp_free(&r);
        p3d_files_set(m, NULL, 0, st < 0 ? _("No responde") : _("No se pudo leer la lista"));
        return;
    }
    struct { p3d_file_t *f; int n; } fl = { p3d_psram(sizeof(p3d_file_t) * P3D_FILES_MAX), 0 };
    if (fl.f) pj_each(pj_get(r.body, "result"), file_cb, &fl);
    p3d_resp_free(&r);
    for (int i = 1; i < fl.n; i++) {
        p3d_file_t t = fl.f[i];
        int j = i - 1;
        while (j >= 0 && fl.f[j].date < t.date) {
            fl.f[j + 1] = fl.f[j];
            j--;
        }
        fl.f[j + 1] = t;
    }
    p3d_files_set(m, fl.f, fl.n, NULL);
}

static void gcode(p3d_machine_t *m, moon_t *k, const char *g, bool echo)
{
    char enc[700], url[740];
    p3d_url_enc(enc, sizeof enc, g);
    snprintf(url, sizeof url, "/printer/gcode/script?script=%s", enc);
    if (echo && !m->want_console) p3d_con_add(m, "> ", g);
    int st = post(m, k, url);
    if (st < 0) p3d_con_add(m, "! ", _("No responde"));
    if (m->want_console) console_read(m, k);
}

static void command(p3d_machine_t *m, moon_t *k, const p3d_cmd_t *c)
{
    char g[260], enc[300], url[340];
    int st = 0;
    switch (c->op) {
    case P3D_C_PAUSE: st = post(m, k, "/printer/print/pause"); break;
    case P3D_C_RESUME: st = post(m, k, "/printer/print/resume"); break;
    case P3D_C_CANCEL: st = post(m, k, "/printer/print/cancel"); break;
    case P3D_C_ESTOP: st = post(m, k, "/printer/emergency_stop"); break;
    case P3D_C_START:
        p3d_url_enc(enc, sizeof enc, c->text);
        snprintf(url, sizeof url, "/printer/print/start?filename=%s", enc);
        st = post(m, k, url);
        break;
    case P3D_C_DELETE: {
        p3d_url_enc(enc, sizeof enc, c->text);
        snprintf(url, sizeof url, "/server/files/gcodes/%s", enc);
        p3d_resp_t r;
        st = p3d_http(&k->h, "DELETE", url, NULL, 0, NULL, &r, 8192, REQ_MS);
        p3d_resp_free(&r);
        files_read(m, k);
        break;
    }
    case P3D_C_FILES: files_read(m, k); return;
    case P3D_C_TEMP:
        /* SET_HEATER_TEMPERATURE takes Klipper's own names */
        snprintf(g, sizeof g, "SET_HEATER_TEMPERATURE HEATER=%s TARGET=%d",
                 c->a == 2 ? "heater_bed" : c->a == 3 ? "chamber" : c->a == 1 ? "extruder1" : "extruder",
                 (int)roundf(c->v[0]));
        gcode(m, k, g, false);
        return;
    default:
        if (p3d_gcode_of(c, g, sizeof g) > 0) gcode(m, k, g, c->op == P3D_C_GCODE);
        return;
    }
    if (st < 0) p3d_con_add(m, "! ", _("No responde"));
    else if (st >= 400) p3d_con_add(m, "! ", _("La impresora no está lista para eso"));
}

static void run(p3d_machine_t *m)
{
    moon_t *k = p3d_psram(sizeof *k);
    if (!k) return;
    memset(k, 0, sizeof *k);
    k->est_s = -1;
    p3d_http_init(&k->h, m->cfg.host, p3d_cfg_port(&m->cfg), m->cfg.tls);
    k->h.stop = &m->stop;
    if (m->cfg.key[0]) snprintf(k->h.extra, sizeof k->h.extra, "X-Api-Key: %.120s\r\n", m->cfg.key);
    int fails = 0;
    uint64_t next = 0;
    while (!m->stop) {
        p3d_cmd_t c;
        bool did = false;
        while (p3d_take(m, &c)) {
            command(m, k, &c);
            did = true;
        }
        if (did || aos_hal_uptime_ms() >= next) {
            fails = poll(m, k) < 0 ? fails + 1 : 0;
            if (fails) k->info = false;
            next = aos_hal_uptime_ms() + (uint64_t)(fails ? p3d_backoff(m, fails) : m->poll_ms);
        }
        int wait = (int)(next - aos_hal_uptime_ms());
        if (!p3d_wait(m, wait > 0 ? wait : 0)) break;
    }
    p3d_http_close(&k->h);
    free(k);
}

const p3d_driver_t p3d_drv_moonraker = {
    .word      = "moonraker",
    .label     = "Klipper (Moonraker)",
    .makers    = N_("Voron, Sovol, Elegoo Neptune 4, Qidi, RatRig y toda impresora con Klipper, Mainsail o Fluidd"),
    .key_label = N_("Clave de la API (opcional)"),
    .key_hint  = N_("Sólo si Moonraker tiene [authorization]: la clave de /access/api_key"),
    .port      = 7125,
    .tls_port  = 443,
    .fields    = P3D_F_KEY | P3D_F_TLS,
    .run       = run,
};
