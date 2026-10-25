/*
 * P4OS - 3D printers: Duet and RepRapFirmware 3 (docs.duet3d.com, "HTTP
 * requests").
 *
 * A Duet on its own serves the rr_ requests:
 *
 *   GET /rr_connect?password=..   opens a session (err 0); RRF 3.5 hands a
 *                                 sessionKey that later requests carry as
 *                                 X-Session-Key
 *   GET /rr_model?key=..&flags=d99vn   one part of the object model
 *   GET /rr_gcode?gcode=..        and /rr_reply for what it answered
 *   GET /rr_filelist?dir=0:/gcodes
 *
 * With a single-board computer (DSF) the same model is /machine/status and
 * G-code is POST /machine/code (the answer comes back in the body). The
 * driver asks rr_connect first and goes to DSF on a 404.
 *
 * The model's heaters are numbered: the bed is heat.bedHeaters[0], the
 * chamber heat.chamberHeaters[0], and each tool says which heater is its
 * own (tools[n].heaters[0]).
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define REQ_MS      6000
#define BODY_MAX    (64 * 1024)

typedef struct {
    p3d_http_t h;
    bool       dsf;
    bool       session;
    int        bed_h, chamber_h, tool_h[2];
    int        polls;
    char       file[160];
} duet_t;

static int get(duet_t *d, const char *path, p3d_resp_t *r, int max)
{
    return p3d_http(&d->h, "GET", path, NULL, 0, NULL, r, max, REQ_MS);
}

static bool connect_session(p3d_machine_t *m, duet_t *d, char *why, size_t wn)
{
    char enc[200], url[300], tm[64] = "";
    p3d_url_enc(enc, sizeof enc, m->cfg.key);
    if (aos_hal_time_is_valid()) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf(tm, sizeof tm, "&time=%04d-%02d-%02dT%02d:%02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                 lt.tm_hour, lt.tm_min, lt.tm_sec);
    }
    snprintf(url, sizeof url, "/rr_connect?password=%s%s", enc, tm);
    p3d_resp_t r;
    int st = get(d, url, &r, 4096);
    if (st == 404) {
        p3d_resp_free(&r);
        st = get(d, "/machine/status", &r, 8);
        p3d_resp_free(&r);
        if (st == 200 || r.cut) {
            d->dsf = true;
            d->session = true;
            return true;
        }
        p3d_copy(why, wn, _("No es una Duet"));
        return false;
    }
    if (st != 200) {
        p3d_copy(why, wn, st < 0 ? _("No responde") : _("Respuesta inesperada"));
        p3d_resp_free(&r);
        return false;
    }
    int err = 0, key = 0;
    pj_int(r.body, "err", &err);
    if (err == 1) {
        p3d_copy(why, wn, _("Contraseña incorrecta"));
        p3d_resp_free(&r);
        return false;
    }
    if (err == 2) {
        p3d_copy(why, wn, _("La Duet no acepta más sesiones"));
        p3d_resp_free(&r);
        return false;
    }
    d->h.extra[0] = '\0';
    if (pj_int(r.body, "sessionKey", &key)) snprintf(d->h.extra, sizeof d->h.extra, "X-Session-Key: %d\r\n", key);
    p3d_resp_free(&r);
    d->session = true;
    return true;
}

static int state_of(const char *s)
{
    if (!strcmp(s, "processing") || !strcmp(s, "simulating")) return P3D_ST_PRINTING;
    if (!strcmp(s, "paused")) return P3D_ST_PAUSED;
    if (!strcmp(s, "pausing") || !strcmp(s, "resuming") || !strcmp(s, "busy") || !strcmp(s, "changingTool") ||
        !strcmp(s, "starting") || !strcmp(s, "updating")) {
        return P3D_ST_BUSY;
    }
    if (!strcmp(s, "halted")) return P3D_ST_ERROR;
    if (!strcmp(s, "off")) return P3D_ST_OFFLINE;
    return P3D_ST_IDLE;
}

static void heater_at(const char *heat, int i, p3d_temp_t *t)
{
    char p[48];
    float v;
    t->on = false;
    if (i < 0) return;
    snprintf(p, sizeof p, "heaters.%d.current", i);
    if (!pj_num(heat, p, &v)) return;
    t->on = true;
    t->cur = v;
    char st[16] = "";
    snprintf(p, sizeof p, "heaters.%d.state", i);
    pj_str(heat, p, st, sizeof st);
    snprintf(p, sizeof p, !strcmp(st, "standby") ? "heaters.%d.standby" : "heaters.%d.active", i);
    t->target = !strcmp(st, "off") ? 0 : pj_num(heat, p, &v) ? v : 0;
}

/* one key of the model: the "result" of rr_model, or a part of /machine/status */
static const char *part(duet_t *d, const char *js, const char *key)
{
    if (d->dsf) return pj_get(js, key);
    (void)key;
    return pj_get(js, "result");
}

static int model(duet_t *d, const char *key, p3d_resp_t *r)
{
    if (d->dsf) {
        /* the whole model, once per poll: the caller reads every key from it */
        return get(d, "/machine/status", r, 256 * 1024);
    }
    char url[96];
    snprintf(url, sizeof url, "/rr_model?key=%s&flags=d99vn", key);
    return get(d, url, r, BODY_MAX);
}

static int poll(p3d_machine_t *m, duet_t *d)
{
    char why[64];
    if (!d->session && !connect_session(m, d, why, sizeof why)) {
        p3d_offline(m, why);
        return -1;
    }
    p3d_resp_t all = { 0 }, rs = { 0 }, rh = { 0 }, rj = { 0 }, rm = { 0 }, rt = { 0 }, rf = { 0 };
    const char *s_state, *s_heat, *s_job, *s_move, *s_tools = NULL, *s_fans = NULL;
    if (d->dsf) {
        int st = model(d, "", &all);
        if (st != 200) {
            p3d_resp_free(&all);
            p3d_offline(m, st < 0 ? _("No responde") : _("Respuesta inesperada"));
            d->session = false;
            return -1;
        }
        s_state = part(d, all.body, "state");
        s_heat = part(d, all.body, "heat");
        s_job = part(d, all.body, "job");
        s_move = part(d, all.body, "move");
        s_tools = part(d, all.body, "tools");
        s_fans = part(d, all.body, "fans");
    } else {
        int st = model(d, "state", &rs);
        if (st != 200) {
            p3d_resp_free(&rs);
            p3d_offline(m, st == 401 ? _("Contraseña incorrecta") : st < 0 ? _("No responde") : _("Respuesta inesperada"));
            d->session = false;
            return -1;
        }
        model(d, "heat", &rh);
        model(d, "job", &rj);
        model(d, "move", &rm);
        if (d->polls % 5 == 0) {
            model(d, "tools", &rt);
            model(d, "fans", &rf);
        }
        s_state = part(d, rs.body, "state");
        s_heat = part(d, rh.body, "heat");
        s_job = part(d, rj.body, "job");
        s_move = part(d, rm.body, "move");
        if (rt.body) s_tools = part(d, rt.body, "tools");
        if (rf.body) s_fans = part(d, rf.body, "fans");
    }
    d->polls++;
    int n;
    if (s_heat) {
        d->bed_h = pj_int(s_heat, "bedHeaters.0", &n) ? n : -1;
        d->chamber_h = pj_int(s_heat, "chamberHeaters.0", &n) ? n : -1;
    }
    if (s_tools) {
        d->tool_h[0] = pj_int(s_tools, "0.heaters.0", &n) ? n : -1;
        d->tool_h[1] = pj_int(s_tools, "1.heaters.0", &n) ? n : -1;
    }

    p3d_lock();
    p3d_status_t *t = &m->st;
    t->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_TEMP | P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_EXTRUDE |
              P3D_CAP_FAN | P3D_CAP_SPEED | P3D_CAP_FLOW | P3D_CAP_GCODE | P3D_CAP_FILES | P3D_CAP_ESTOP |
              P3D_CAP_MOTORS_OFF | P3D_CAP_REPLIES | P3D_CAP_DELETE | P3D_CAP_CHAMBER;
    char status[24] = "";
    pj_str(s_state, "status", status, sizeof status);
    p3d_copy(t->raw, sizeof t->raw, status);
    t->state = state_of(status);
    t->msg[0] = '\0';
    pj_str(s_state, "displayMessage", t->msg, sizeof t->msg);
    heater_at(s_heat, d->tool_h[0], &t->tool[0]);
    heater_at(s_heat, d->tool_h[1], &t->tool[1]);
    heater_at(s_heat, d->bed_h, &t->bed);
    heater_at(s_heat, d->chamber_h, &t->chamber);
    char fname[160] = "";
    pj_str(s_job, "file.fileName", fname, sizeof fname);
    const char *base = strrchr(fname, '/');
    p3d_copy(t->file, sizeof t->file, base ? base + 1 : fname);
    p3d_copy(d->file, sizeof d->file, fname);
    float v, size = 0, pos = 0;
    bool active = t->state == P3D_ST_PRINTING || t->state == P3D_ST_PAUSED || t->state == P3D_ST_BUSY;
    if (active && fname[0] && pj_num(s_job, "file.size", &size) && size > 0 && pj_num(s_job, "filePosition", &pos)) {
        t->progress = pos * 100 / size;
    } else {
        t->progress = -1;
    }
    t->elapsed_s = active && pj_num(s_job, "duration", &v) ? (int32_t)v : -1;
    t->remaining_s = -1;
    if (active) {
        if (pj_num(s_job, "timesLeft.slicer", &v) && v > 0) t->remaining_s = (int32_t)v;
        else if (pj_num(s_job, "timesLeft.file", &v) && v > 0) t->remaining_s = (int32_t)v;
        else if (pj_num(s_job, "timesLeft.filament", &v) && v > 0) t->remaining_s = (int32_t)v;
    }
    t->layer = active && pj_int(s_job, "layer", &n) ? n : 0;
    t->layers = active && pj_int(s_job, "file.numLayers", &n) ? n : 0;
    t->speed = pj_num(s_move, "speedFactor", &v) ? (int)roundf(v * 100) : -1;
    t->flow = pj_num(s_move, "extruders.0.factor", &v) ? (int)roundf(v * 100) : -1;
    t->has_pos = pj_num(s_move, "axes.0.userPosition", &t->pos[0]) && pj_num(s_move, "axes.1.userPosition", &t->pos[1]) &&
                 pj_num(s_move, "axes.2.userPosition", &t->pos[2]);
    if (s_fans) t->fan = pj_num(s_fans, "0.requestedValue", &v) ? (int)roundf(v * 100) : -1;
    if (!t->fw[0]) {
        char fw[32] = "";
        if (pj_str(s_state, "machineMode", fw, sizeof fw)) snprintf(t->fw, sizeof t->fw, "RepRapFirmware%s", d->dsf ? " (SBC)" : "");
    }
    p3d_publish(m);
    p3d_unlock();
    p3d_resp_free(&all);
    p3d_resp_free(&rs);
    p3d_resp_free(&rh);
    p3d_resp_free(&rj);
    p3d_resp_free(&rm);
    p3d_resp_free(&rt);
    p3d_resp_free(&rf);
    return 0;
}

/* sends G-code; the answer goes to the console */
static int gcode(p3d_machine_t *m, duet_t *d, const char *g, bool echo)
{
    if (echo) p3d_con_add(m, "> ", g);
    p3d_resp_t r;
    int st;
    if (d->dsf) {
        st = p3d_http(&d->h, "POST", "/machine/code", g, -1, "text/plain", &r, 16 * 1024, 20000);
        if (st == 200 && r.body && r.body[0]) p3d_con_add(m, "", r.body);
        p3d_resp_free(&r);
        return st;
    }
    char enc[600], url[640];
    p3d_url_enc(enc, sizeof enc, g);
    snprintf(url, sizeof url, "/rr_gcode?gcode=%s", enc);
    st = get(d, url, &r, 4096);
    p3d_resp_free(&r);
    if (st == 200) {
        /* the reply comes when the command is done: a moment later */
        for (int i = 0; i < 6 && !m->stop; i++) {
            aos_hal_sleep_ms(250);
            if (get(d, "/rr_reply", &r, 16 * 1024) == 200 && r.len > 0) {
                p3d_con_add(m, "", r.body);
                p3d_resp_free(&r);
                break;
            }
            p3d_resp_free(&r);
        }
    }
    return st;
}

typedef struct {
    p3d_file_t *f;
    int         n;
    bool        dsf;
} flist_t;

static bool file_cb(const char *k, int kl, const char *v, void *ctx)
{
    (void)k;
    (void)kl;
    flist_t *fl = ctx;
    char type[8] = "", name[96] = "";
    pj_str(v, "type", type, sizeof type);
    pj_str(v, "name", name, sizeof name);
    if (strcmp(type, "f") || !name[0] || name[0] == '.' || fl->n >= P3D_FILES_MAX) return true;
    p3d_file_t *f = &fl->f[fl->n++];
    memset(f, 0, sizeof *f);
    p3d_copy(f->name, sizeof f->name, name);
    snprintf(f->path, sizeof f->path, "0:/gcodes/%.170s", name);
    float x;
    f->size = pj_num(v, "size", &x) ? (uint32_t)x : 0;
    f->est_s = -1;
    return true;
}

static void files_read(p3d_machine_t *m, duet_t *d)
{
    p3d_files_begin(m);
    p3d_resp_t r;
    int st = get(d, d->dsf ? "/machine/directory/0:/gcodes" : "/rr_filelist?dir=0:/gcodes", &r, 128 * 1024);
    if (st != 200) {
        p3d_resp_free(&r);
        p3d_files_set(m, NULL, 0, st < 0 ? _("No responde") : _("No se pudo leer la lista"));
        return;
    }
    flist_t fl = { .f = p3d_psram(sizeof(p3d_file_t) * P3D_FILES_MAX), .dsf = d->dsf };
    if (fl.f) pj_each(d->dsf ? r.body : pj_get(r.body, "files"), file_cb, &fl);
    p3d_resp_free(&r);
    p3d_files_set(m, fl.f, fl.n, NULL);
}

static void command(p3d_machine_t *m, duet_t *d, const p3d_cmd_t *c)
{
    char g[260];
    int st = 0;
    switch (c->op) {
    case P3D_C_PAUSE: st = gcode(m, d, "M25", false); break;
    case P3D_C_RESUME: st = gcode(m, d, "M24", false); break;
    case P3D_C_CANCEL:
        /* RRF cancels a paused print with M0; one that runs is paused first */
        if (m->st.state != P3D_ST_PAUSED) gcode(m, d, "M25", false);
        st = gcode(m, d, "M0", false);
        break;
    case P3D_C_START:
        snprintf(g, sizeof g, "M32 \"%.200s\"", c->text);
        st = gcode(m, d, g, false);
        break;
    case P3D_C_DELETE: {
        char enc[300], url[340];
        p3d_url_enc(enc, sizeof enc, c->text);
        p3d_resp_t r;
        if (d->dsf) {
            snprintf(url, sizeof url, "/machine/file/%s", enc);
            st = p3d_http(&d->h, "DELETE", url, NULL, 0, NULL, &r, 4096, REQ_MS);
        } else {
            snprintf(url, sizeof url, "/rr_delete?name=%s", enc);
            st = get(d, url, &r, 4096);
        }
        p3d_resp_free(&r);
        files_read(m, d);
        break;
    }
    case P3D_C_FILES: files_read(m, d); return;
    case P3D_C_TEMP:
        if (c->a == 2) snprintf(g, sizeof g, "M140 S%d", (int)roundf(c->v[0]));
        else if (c->a == 3) snprintf(g, sizeof g, "M141 S%d", (int)roundf(c->v[0]));
        else snprintf(g, sizeof g, "G10 P%d S%d", c->a, (int)roundf(c->v[0]));
        st = gcode(m, d, g, false);
        break;
    default:
        if (p3d_gcode_of(c, g, sizeof g) > 0) st = gcode(m, d, g, c->op == P3D_C_GCODE);
        break;
    }
    if (st < 0) p3d_con_add(m, "! ", _("No responde"));
}

static void run(p3d_machine_t *m)
{
    duet_t *d = p3d_psram(sizeof *d);
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->bed_h = d->chamber_h = d->tool_h[1] = -1;
    d->tool_h[0] = 1;
    p3d_http_init(&d->h, m->cfg.host, p3d_cfg_port(&m->cfg), m->cfg.tls);
    d->h.stop = &m->stop;
    int fails = 0;
    uint64_t next = 0;
    while (!m->stop) {
        p3d_cmd_t c;
        bool did = false;
        while (p3d_take(m, &c)) {
            command(m, d, &c);
            did = true;
        }
        if (did || aos_hal_uptime_ms() >= next) {
            fails = poll(m, d) < 0 ? fails + 1 : 0;
            next = aos_hal_uptime_ms() + (uint64_t)(fails ? p3d_backoff(m, fails) : m->poll_ms);
        }
        int wait = (int)(next - aos_hal_uptime_ms());
        if (!p3d_wait(m, wait > 0 ? wait : 0)) break;
    }
    if (d->session && !d->dsf) {
        p3d_resp_t r;
        get(d, "/rr_disconnect", &r, 1024);
        p3d_resp_free(&r);
    }
    p3d_http_close(&d->h);
    free(d);
}

const p3d_driver_t p3d_drv_duet = {
    .word      = "duet",
    .label     = "Duet (RepRapFirmware)",
    .makers    = N_("Placas Duet 2 y 3 con RepRapFirmware 3, solas o con SBC"),
    .key_label = N_("Contraseña (si tiene)"),
    .key_hint  = N_("La de M551; vacía si la Duet no pide"),
    .port      = 80,
    .tls_port  = 443,
    .fields    = P3D_F_KEY,
    .run       = run,
};
