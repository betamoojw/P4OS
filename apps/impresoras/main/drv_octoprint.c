/*
 * P4OS - 3D printers: OctoPrint (REST API, docs.octoprint.org/en/master/api).
 *
 * Every request carries "X-Api-Key"; the key is in OctoPrint's settings,
 * "API" (the global one) or the user's "Application keys". A poll is two
 * GETs on one kept connection:
 *
 *   /api/printer   temperatures and the state's flags; 409 while OctoPrint
 *                  has no serial link with the printer (then /api/connection
 *                  says which state that is)
 *   /api/job       the file, the progress, the times
 *
 * Controls go through printhead (jog, home, feedrate), tool (target,
 * extrude, flowrate), bed, command (any G-code) and job (pause, resume,
 * cancel). The camera is the one in /api/settings: OctoPrint writes it as
 * the Pi sees it ("http://127.0.0.1:8080/..." or "/webcam/..."), so the
 * host is swapped for the machine's. The job's picture comes from the
 * PrusaSlicer Thumbnails plugin when it is installed ("thumbnail" in the
 * file's entry).
 *
 * The same loop serves PrusaLink's first version (the MK3's Raspberry Pi),
 * which spoke this API: drv_prusalink.c calls p3d_octo_run() then.
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define REQ_MS      6000
#define BODY_MAX    (32 * 1024)
#define FILES_MAX   (256 * 1024)
#define THUMB_MAX   (256 * 1024)

void p3d_octo_run(p3d_machine_t *m, p3d_http_t *h);

typedef struct {
    p3d_http_t *h;
    char        job_file[160];
    char        thumb_of[160];
    bool        have_settings;
    bool        operational;
} octo_t;

static int get(octo_t *o, const char *path, p3d_resp_t *r, int max)
{
    return p3d_http(o->h, "GET", path, NULL, 0, NULL, r, max, REQ_MS);
}

static int post_json(octo_t *o, const char *path, const char *json)
{
    p3d_resp_t r;
    int st = p3d_http(o->h, "POST", path, json, -1, "application/json", &r, 4096, REQ_MS);
    p3d_resp_free(&r);
    return st;
}

/* "http://127.0.0.1:8080/?action=snapshot" or "/webcam/..." as the board reaches it */
static void cam_fix(const p3d_machine_t *m, const char *url, char *out, size_t n)
{
    out[0] = '\0';
    if (!url || !url[0]) return;
    int port = p3d_cfg_port(&m->cfg);
    const char *scheme = m->cfg.tls ? "https" : "http";
    if (url[0] == '/') {
        snprintf(out, n, "%s://%s:%d%s", scheme, m->cfg.host, port, url);
        return;
    }
    const char *loc[] = { "http://127.0.0.1", "http://localhost", "https://127.0.0.1", "https://localhost" };
    for (int i = 0; i < 4; i++) {
        size_t l = strlen(loc[i]);
        if (!strncmp(url, loc[i], l)) {
            snprintf(out, n, "%s://%s%s", i >= 2 ? "https" : "http", m->cfg.host, url + l);
            return;
        }
    }
    p3d_copy(out, n, url);
}

/* the profile that is current: its model, else its name */
static bool profile_cb(const char *k, int kl, const char *v, void *ctx)
{
    (void)k;
    (void)kl;
    bool cur = false;
    if (!pj_bool(v, "current", &cur) || !cur) return true;
    char *out = ctx;
    if (!pj_str(v, "model", out, 48) || !out[0]) pj_str(v, "name", out, 48);
    return false;
}

static void read_settings(p3d_machine_t *m, octo_t *o)
{
    p3d_resp_t r;
    if (get(o, "/api/settings", &r, 96 * 1024) == 200) {
        char url[192] = "", fixed[192];
        bool enabled = true;
        pj_bool(r.body, "webcam.webcamEnabled", &enabled);
        /* the MJPEG stream first: the board decodes it as it comes (a
         * snapshot each time was 0.9 fps on a MK3S's Pi); the snapshot when
         * there is no stream */
        if (!pj_str(r.body, "webcam.streamUrl", url, sizeof url) || !url[0]) {
            pj_str(r.body, "webcam.snapshotUrl", url, sizeof url);
        }
        cam_fix(m, url, fixed, sizeof fixed);
        p3d_lock();
        p3d_copy(m->st.cam_url, sizeof m->st.cam_url, enabled ? fixed : "");
        p3d_unlock();
        o->have_settings = true;
    } else if (r.status == 403) {
        o->have_settings = true;        /* a key without the settings permission */
    }
    p3d_resp_free(&r);
    /* the printer profile in use says which machine it is ("Original Prusa i3 MK3S") */
    if (get(o, "/api/printerprofiles", &r, 32 * 1024) == 200) {
        char model[48] = "";
        pj_each(pj_get(r.body, "profiles"), profile_cb, model);
        p3d_lock();
        if (model[0]) p3d_copy(m->st.model, sizeof m->st.model, model);
        p3d_unlock();
    }
    p3d_resp_free(&r);
    if (get(o, "/api/version", &r, 4096) == 200) {
        char text[48] = "";
        pj_str(r.body, "text", text, sizeof text);
        p3d_lock();
        p3d_copy(m->st.fw, sizeof m->st.fw, text);
        p3d_unlock();
    }
    p3d_resp_free(&r);
}

static void temp_of(const char *js, const char *name, p3d_temp_t *t)
{
    char path[48];
    snprintf(path, sizeof path, "temperature.%s.actual", name);
    float v;
    if (pj_num(js, path, &v)) {
        t->on = true;
        t->cur = v;
        snprintf(path, sizeof path, "temperature.%s.target", name);
        t->target = pj_num(js, path, &v) ? v : 0;
    }
}

/* the job's picture, when the thumbnails plugin gives one */
static void thumb_fetch(p3d_machine_t *m, octo_t *o, const char *origin, const char *path)
{
    p3d_copy(o->thumb_of, sizeof o->thumb_of, path);
    char enc[300], url[400], th[256] = "";
    p3d_url_enc(enc, sizeof enc, path);
    snprintf(url, sizeof url, "/api/files/%s/%s", origin[0] ? origin : "local", enc);
    p3d_resp_t r;
    if (get(o, url, &r, 64 * 1024) == 200) pj_str(r.body, "thumbnail", th, sizeof th);
    p3d_resp_free(&r);
    if (th[0]) {
        /* the plugin writes the file's name as it is ("Lamp Shade.png"): spaces
         * and the like escaped, its query kept */
        char *q = strchr(th, '?');
        if (q) *q = '\0';
        char ep[300];
        p3d_url_enc(ep, sizeof ep, th);
        snprintf(url, sizeof url, "%s%.300s%s%.40s", th[0] == '/' ? "" : "/", ep, q ? "?" : "", q ? q + 1 : "");
        if (get(o, url, &r, THUMB_MAX) == 200 && r.len > 0 && !r.cut) p3d_thumb_store(m, r.body, r.len);
        else p3d_thumb_store(m, NULL, 0);
        p3d_resp_free(&r);
    } else {
        p3d_thumb_store(m, NULL, 0);
    }
}

static int poll(p3d_machine_t *m, octo_t *o)
{
    p3d_resp_t pr, jr;
    int st = get(o, "/api/printer?history=false", &pr, BODY_MAX);
    if (st < 0) {
        p3d_offline(m, _("No responde"));
        p3d_resp_free(&pr);
        return -1;
    }
    if (st == 401 || st == 403) {
        p3d_offline(m, _("Clave de la API incorrecta"));
        p3d_resp_free(&pr);
        return -1;
    }
    if (!o->have_settings) read_settings(m, o);
    if (st == 409) {
        /* OctoPrint answers but has no printer behind it */
        p3d_resp_free(&pr);
        char state[48] = "";
        if (get(o, "/api/connection", &pr, 8192) == 200) pj_str(pr.body, "current.state", state, sizeof state);
        p3d_resp_free(&pr);
        p3d_lock();
        p3d_status_t *s = &m->st;
        uint32_t caps = s->caps;
        char cam[192];
        p3d_copy(cam, sizeof cam, s->cam_url);
        p3d_status_reset(s);
        s->caps = caps | P3D_CAP_CONNECT;
        p3d_copy(s->cam_url, sizeof s->cam_url, cam);
        s->state = strstr(state, "Error") ? P3D_ST_ERROR : !strncmp(state, "Connecting", 10) || !strncmp(state, "Detecting", 9) ? P3D_ST_CONNECTING : P3D_ST_OFFLINE;
        p3d_copy(s->raw, sizeof s->raw, state);
        p3d_copy(s->msg, sizeof s->msg, _("OctoPrint no está conectado a la impresora"));
        p3d_publish(m);
        p3d_unlock();
        o->operational = false;
        return 0;
    }
    if (st != 200) {
        char why[64];
        snprintf(why, sizeof why, _("Contestó %d"), st);
        p3d_offline(m, why);
        p3d_resp_free(&pr);
        return -1;
    }
    o->operational = true;
    int js = get(o, "/api/job", &jr, BODY_MAX);

    p3d_lock();
    p3d_status_t *s = &m->st;
    s->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_TEMP | P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_EXTRUDE |
              P3D_CAP_FAN | P3D_CAP_SPEED | P3D_CAP_FLOW | P3D_CAP_GCODE | P3D_CAP_FILES | P3D_CAP_CONNECT |
              P3D_CAP_MOTORS_OFF | P3D_CAP_DELETE | P3D_CAP_ESTOP;
    s->tool[0].on = s->tool[1].on = s->bed.on = s->chamber.on = false;
    temp_of(pr.body, "tool0", &s->tool[0]);
    temp_of(pr.body, "tool1", &s->tool[1]);
    temp_of(pr.body, "bed", &s->bed);
    temp_of(pr.body, "chamber", &s->chamber);
    bool printing = false, paused = false, pausing = false, error = false, cancelling = false, ready = false;
    pj_bool(pr.body, "state.flags.printing", &printing);
    pj_bool(pr.body, "state.flags.paused", &paused);
    pj_bool(pr.body, "state.flags.pausing", &pausing);
    pj_bool(pr.body, "state.flags.error", &error);
    pj_bool(pr.body, "state.flags.cancelling", &cancelling);
    pj_bool(pr.body, "state.flags.ready", &ready);
    s->raw[0] = '\0';
    pj_str(pr.body, "state.text", s->raw, sizeof s->raw);
    s->msg[0] = '\0';
    if (error) {
        s->state = P3D_ST_ERROR;
        pj_str(pr.body, "state.error", s->msg, sizeof s->msg);
    } else if (paused || pausing) s->state = P3D_ST_PAUSED;
    else if (printing) s->state = P3D_ST_PRINTING;
    else if (cancelling) s->state = P3D_ST_BUSY;
    else s->state = P3D_ST_IDLE;

    char file[128] = "", origin[16] = "", path[160] = "";
    if (js == 200) {
        if (!pj_str(jr.body, "job.file.display", file, sizeof file)) pj_str(jr.body, "job.file.name", file, sizeof file);
        pj_str(jr.body, "job.file.origin", origin, sizeof origin);
        pj_str(jr.body, "job.file.path", path, sizeof path);
        float v;
        s->progress = pj_num(jr.body, "progress.completion", &v) ? v : -1;
        s->elapsed_s = pj_num(jr.body, "progress.printTime", &v) ? (int32_t)v : -1;
        s->remaining_s = pj_num(jr.body, "progress.printTimeLeft", &v) ? (int32_t)v : -1;
        /* OctoPrint has no estimate of its own for the first minutes (null):
         * the slicer's minus what has printed, else what the progress projects */
        float est;
        if (s->remaining_s < 0 && s->elapsed_s >= 0 && (s->state == P3D_ST_PRINTING || s->state == P3D_ST_PAUSED)) {
            if (pj_num(jr.body, "job.estimatedPrintTime", &est) && est > s->elapsed_s) {
                s->remaining_s = (int32_t)(est - (float)s->elapsed_s);
            } else if (s->progress >= 0.5f) {
                s->remaining_s = (int32_t)((float)s->elapsed_s * (100 - s->progress) / s->progress);
            }
        }
        /* a finished job keeps 100 % until the next one: OctoPrint is "Operational" again */
        if (s->state == P3D_ST_IDLE && s->progress >= 99.9f && file[0]) {
            s->state = P3D_ST_DONE;
            s->remaining_s = 0;
        }
        char jstate[32] = "";
        pj_str(jr.body, "state", jstate, sizeof jstate);
        if (s->state == P3D_ST_IDLE && strstr(jstate, "Cancel")) s->state = P3D_ST_CANCELLED;
    }
    p3d_copy(s->file, sizeof s->file, file);
    p3d_publish(m);
    p3d_unlock();
    p3d_resp_free(&pr);
    p3d_resp_free(&jr);
    if (path[0] && strcmp(path, o->thumb_of)) thumb_fetch(m, o, origin, path);
    else if (!path[0] && o->thumb_of[0]) {
        o->thumb_of[0] = '\0';
        p3d_thumb_store(m, NULL, 0);
    }
    return 0;
}

/* the files tree, flattened: folders become "folder/name" */
typedef struct {
    p3d_file_t *f;
    int         n;
} flist_t;

static bool file_cb(const char *k, int kl, const char *v, void *ctx);

static void file_one(const char *v, flist_t *fl)
{
    char type[24] = "";
    pj_str(v, "type", type, sizeof type);
    if (!strcmp(type, "folder")) {
        pj_each(pj_get(v, "children"), file_cb, fl);
        return;
    }
    if (strcmp(type, "machinecode") || fl->n >= P3D_FILES_MAX) return;
    p3d_file_t *f = &fl->f[fl->n];
    memset(f, 0, sizeof *f);
    char origin[16] = "local", path[180] = "";
    pj_str(v, "origin", origin, sizeof origin);
    pj_str(v, "path", path, sizeof path);
    if (!pj_str(v, "display", f->name, sizeof f->name)) pj_str(v, "name", f->name, sizeof f->name);
    if (strchr(path, '/')) p3d_copy(f->name, sizeof f->name, path);
    snprintf(f->path, sizeof f->path, "%.12s/%.170s", origin, path);
    float x;
    f->size = pj_num(v, "size", &x) ? (uint32_t)x : 0;
    f->date = pj_num(v, "date", &x) ? (uint32_t)x : 0;
    f->est_s = pj_num(v, "gcodeAnalysis.estimatedPrintTime", &x) ? (int32_t)x : -1;
    fl->n++;
}

static bool file_cb(const char *k, int kl, const char *v, void *ctx)
{
    (void)k;
    (void)kl;
    file_one(v, ctx);
    return true;
}

static void files_read(p3d_machine_t *m, octo_t *o)
{
    p3d_files_begin(m);
    p3d_resp_t r;
    int st = get(o, "/api/files?recursive=true", &r, FILES_MAX);
    if (st != 200) {
        p3d_resp_free(&r);
        p3d_files_set(m, NULL, 0, st < 0 ? _("No responde") : _("No se pudo leer la lista"));
        return;
    }
    flist_t fl = { .f = p3d_psram(sizeof(p3d_file_t) * P3D_FILES_MAX), .n = 0 };
    if (fl.f) pj_each(pj_get(r.body, "files"), file_cb, &fl);
    p3d_resp_free(&r);
    /* newest first */
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

static void command(p3d_machine_t *m, octo_t *o, const p3d_cmd_t *c)
{
    char b[400], g[220];
    int st = 0;
    switch (c->op) {
    case P3D_C_PAUSE: st = post_json(o, "/api/job", "{\"command\":\"pause\",\"action\":\"pause\"}"); break;
    case P3D_C_RESUME: st = post_json(o, "/api/job", "{\"command\":\"pause\",\"action\":\"resume\"}"); break;
    case P3D_C_CANCEL: st = post_json(o, "/api/job", "{\"command\":\"cancel\"}"); break;
    case P3D_C_TEMP:
        if (c->a == 2) snprintf(b, sizeof b, "{\"command\":\"target\",\"target\":%d}", (int)roundf(c->v[0]));
        else if (c->a == 3) snprintf(b, sizeof b, "{\"command\":\"target\",\"target\":%d}", (int)roundf(c->v[0]));
        else snprintf(b, sizeof b, "{\"command\":\"target\",\"targets\":{\"tool%d\":%d}}", c->a, (int)roundf(c->v[0]));
        st = post_json(o, c->a == 2 ? "/api/printer/bed" : c->a == 3 ? "/api/printer/chamber" : "/api/printer/tool", b);
        break;
    case P3D_C_JOG:
        snprintf(b, sizeof b, "{\"command\":\"jog\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}", (double)c->v[0], (double)c->v[1],
                 (double)c->v[2]);
        st = post_json(o, "/api/printer/printhead", b);
        break;
    case P3D_C_HOME: {
        int a = c->a ? c->a : 7;
        snprintf(b, sizeof b, "{\"command\":\"home\",\"axes\":[%s%s%s%s%s]}", a & 1 ? "\"x\"" : "",
                 (a & 1) && (a & 6) ? "," : "", a & 2 ? "\"y\"" : "", (a & 2) && (a & 4) ? "," : "", a & 4 ? "\"z\"" : "");
        st = post_json(o, "/api/printer/printhead", b);
        break;
    }
    case P3D_C_EXTRUDE:
        snprintf(b, sizeof b, "{\"command\":\"extrude\",\"amount\":%.1f}", (double)c->v[0]);
        st = post_json(o, "/api/printer/tool", b);
        break;
    case P3D_C_SPEED:
        snprintf(b, sizeof b, "{\"command\":\"feedrate\",\"factor\":%d}", (int)roundf(c->v[0]));
        st = post_json(o, "/api/printer/printhead", b);
        break;
    case P3D_C_FLOW:
        snprintf(b, sizeof b, "{\"command\":\"flowrate\",\"factor\":%d}", (int)roundf(c->v[0]));
        st = post_json(o, "/api/printer/tool", b);
        break;
    case P3D_C_CONNECT:
        st = post_json(o, "/api/connection", c->a ? "{\"command\":\"connect\"}" : "{\"command\":\"disconnect\"}");
        break;
    case P3D_C_START: {
        /* path is "origin/path" */
        char enc[300], url[320];
        p3d_url_enc(enc, sizeof enc, c->text);
        snprintf(url, sizeof url, "/api/files/%s", enc);
        st = post_json(o, url, "{\"command\":\"select\",\"print\":true}");
        break;
    }
    case P3D_C_DELETE: {
        char enc[300], url[320];
        p3d_url_enc(enc, sizeof enc, c->text);
        snprintf(url, sizeof url, "/api/files/%s", enc);
        p3d_resp_t r;
        st = p3d_http(o->h, "DELETE", url, NULL, 0, NULL, &r, 4096, REQ_MS);
        p3d_resp_free(&r);
        if (st >= 200 && st < 300) files_read(m, o);
        break;
    }
    case P3D_C_FILES: files_read(m, o); return;
    default:
        if (p3d_gcode_of(c, g, sizeof g) > 0) {
            /* "commands": one per line */
            int k = snprintf(b, sizeof b, "{\"commands\":[");
            const char *p = g;
            bool first = true;
            while (*p && k < (int)sizeof b - 8) {
                const char *e = strchr(p, '\n');
                char line[200], esc[300];
                size_t l = e ? (size_t)(e - p) : strlen(p);
                if (l >= sizeof line) l = sizeof line - 1;
                memcpy(line, p, l);
                line[l] = '\0';
                p3d_json_esc(esc, sizeof esc, line);
                k += snprintf(b + k, sizeof b - (size_t)k, "%s\"%s\"", first ? "" : ",", esc);
                first = false;
                p = e ? e + 1 : p + l;
            }
            snprintf(b + k, sizeof b - (size_t)k, "]}");
            st = post_json(o, "/api/printer/command", b);
            if (c->op == P3D_C_GCODE) p3d_con_add(m, "> ", g);
        }
        break;
    }
    if (st == 409) p3d_con_add(m, "! ", _("La impresora no está lista para eso"));
    else if (st < 0) p3d_con_add(m, "! ", _("No responde"));
    else if (st >= 400) {
        char e[48];
        snprintf(e, sizeof e, _("Contestó %d"), st);
        p3d_con_add(m, "! ", e);
    }
}

void p3d_octo_run(p3d_machine_t *m, p3d_http_t *h)
{
    octo_t o = { .h = h };
    int fails = 0;
    uint64_t next = 0;
    while (!m->stop) {
        p3d_cmd_t c;
        bool did = false;
        while (p3d_take(m, &c)) {
            command(m, &o, &c);
            did = true;
        }
        uint64_t now = aos_hal_uptime_ms();
        if (did || now >= next) {
            fails = poll(m, &o) < 0 ? fails + 1 : 0;
            next = aos_hal_uptime_ms() + (uint64_t)(fails ? p3d_backoff(m, fails) : m->poll_ms);
        }
        int wait = (int)(next - aos_hal_uptime_ms());
        if (!p3d_wait(m, wait > 0 ? wait : 0)) break;
    }
    p3d_http_close(h);
}

static void run(p3d_machine_t *m)
{
    p3d_http_t h;
    p3d_http_init(&h, m->cfg.host, p3d_cfg_port(&m->cfg), m->cfg.tls);
    h.stop = &m->stop;
    if (m->cfg.key[0]) snprintf(h.extra, sizeof h.extra, "X-Api-Key: %.120s\r\n", m->cfg.key);
    p3d_octo_run(m, &h);
}

const p3d_driver_t p3d_drv_octoprint = {
    .word      = "octoprint",
    .label     = "OctoPrint",
    .makers    = N_("Cualquier impresora con OctoPrint (Prusa, Ender, Anet...)"),
    .key_label = N_("Clave de la API"),
    .key_hint  = N_("En OctoPrint: Ajustes, API (o la clave de una aplicación del usuario)"),
    .port      = 80,
    .tls_port  = 443,
    .fields    = P3D_F_KEY | P3D_F_TLS | P3D_F_KEY_NEEDED,
    .run       = run,
};
