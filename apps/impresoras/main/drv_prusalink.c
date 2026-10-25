/*
 * P4OS - 3D printers: PrusaLink (Prusa MK4, MK4S, MK3.9, MK3.5, MINI, XL,
 * CORE One; the MK3S with a Raspberry Pi and PrusaLink).
 *
 * PrusaLink is the printer's own local API (github.com/prusa3d/Prusa-Link-Web,
 * spec/openapi.yaml). Prusa Connect, the cloud, talks to the same printers
 * through it, but its own API is not published: from the board the machine
 * is reached on the LAN, with the user and password that the printer shows
 * in Settings, Network, PrusaLink (user "maker" by default). Authentication
 * is HTTP Digest; older firmware took the API key in X-Api-Key instead, and
 * both are tried.
 *
 *   GET  /api/v1/status          printer temperatures, axes, speed, flow, the job
 *   GET  /api/v1/job             the file and its pictures (refs.thumbnail)
 *   PUT  /api/v1/job/{id}/pause  and /resume; DELETE /api/v1/job/{id} stops
 *   GET  /api/v1/storage, /api/v1/files/<storage>/<folder>    the files
 *   POST /api/v1/files/<path>    prints that file
 *
 * PrusaLink has no G-code endpoint, so there is no jogging, temperatures or
 * console: those are the printer's own screen. The first PrusaLink (0.7, on
 * the MK3's Pi) has no /api/v1 and spoke OctoPrint's API; when /api/v1/status
 * is not there the OctoPrint loop takes over.
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>

#define REQ_MS      6000
#define BODY_MAX    (16 * 1024)
#define THUMB_MAX   (256 * 1024)

void p3d_octo_run(p3d_machine_t *m, p3d_http_t *h);

typedef struct {
    p3d_http_t h;
    int        job_id;
    int        thumb_job;
    bool       info;
} plink_t;

static int req(plink_t *p, const char *method, const char *path, p3d_resp_t *r, int max)
{
    return p3d_http(&p->h, method, path, NULL, 0, NULL, r, max, REQ_MS);
}

static int state_of(const char *s)
{
    if (!strcmp(s, "PRINTING")) return P3D_ST_PRINTING;
    if (!strcmp(s, "PAUSED")) return P3D_ST_PAUSED;
    if (!strcmp(s, "FINISHED")) return P3D_ST_DONE;
    if (!strcmp(s, "STOPPED")) return P3D_ST_CANCELLED;
    if (!strcmp(s, "ERROR")) return P3D_ST_ERROR;
    if (!strcmp(s, "ATTENTION")) return P3D_ST_PAUSED;     /* waiting for the user: filament, a crash */
    if (!strcmp(s, "BUSY")) return P3D_ST_BUSY;
    return P3D_ST_IDLE;                                      /* IDLE, READY */
}

static void job_read(p3d_machine_t *m, plink_t *p)
{
    p3d_resp_t r;
    char name[128] = "", thumb[200] = "";
    if (req(p, "GET", "/api/v1/job", &r, BODY_MAX) == 200) {
        if (!pj_str(r.body, "file.display_name", name, sizeof name)) pj_str(r.body, "file.name", name, sizeof name);
        if (!pj_str(r.body, "file.refs.thumbnail", thumb, sizeof thumb)) pj_str(r.body, "file.refs.icon", thumb, sizeof thumb);
    }
    p3d_resp_free(&r);
    p3d_lock();
    p3d_copy(m->st.file, sizeof m->st.file, name);
    p3d_unlock();
    if (thumb[0] && req(p, "GET", thumb, &r, THUMB_MAX) == 200 && r.len > 0 && !r.cut) p3d_thumb_store(m, r.body, r.len);
    else p3d_thumb_store(m, NULL, 0);
    p3d_resp_free(&r);
    p->thumb_job = p->job_id;
}

/* 1 ok, 0 no /api/v1 (the old PrusaLink), -1 failed */
static int poll(p3d_machine_t *m, plink_t *p)
{
    p3d_resp_t r;
    int st = req(p, "GET", "/api/v1/status", &r, BODY_MAX);
    if (st == 401 && p->h.extra[0] == '\0' && m->cfg.key[0] && !p->h.have_digest) {
        /* no Digest challenge: the API key, as older firmware wanted */
        snprintf(p->h.extra, sizeof p->h.extra, "X-Api-Key: %.120s\r\n", m->cfg.key);
        st = req(p, "GET", "/api/v1/status", &r, BODY_MAX);
    }
    if (st == 404) {
        p3d_resp_free(&r);
        return 0;
    }
    if (st != 200) {
        p3d_offline(m, st == 401 || st == 403 ? _("Usuario o contraseña incorrectos") : st < 0 ? _("No responde") : _("Respuesta inesperada"));
        p3d_resp_free(&r);
        return -1;
    }
    if (!p->info) {
        p3d_resp_t v;
        char text[48] = "", model[48] = "";
        if (req(p, "GET", "/api/version", &v, 4096) == 200) {
            pj_str(v.body, "server", text, sizeof text);
            if (!pj_str(v.body, "printer.type", model, sizeof model)) pj_str(v.body, "hostname", model, sizeof model);
        }
        p3d_resp_free(&v);
        p3d_lock();
        snprintf(m->st.fw, sizeof m->st.fw, "PrusaLink %.36s", text);
        if (model[0]) p3d_copy(m->st.model, sizeof m->st.model, model);
        p3d_unlock();
        p->info = true;
    }
    p3d_lock();
    p3d_status_t *s = &m->st;
    s->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_FILES | P3D_CAP_DELETE;
    char state[24] = "";
    pj_str(r.body, "printer.state", state, sizeof state);
    p3d_copy(s->raw, sizeof s->raw, state);
    s->state = state_of(state);
    s->msg[0] = '\0';
    float v;
    s->tool[0].on = pj_num(r.body, "printer.temp_nozzle", &v);
    s->tool[0].cur = v;
    s->tool[0].target = pj_num(r.body, "printer.target_nozzle", &v) ? v : 0;
    s->bed.on = pj_num(r.body, "printer.temp_bed", &v);
    s->bed.cur = v;
    s->bed.target = pj_num(r.body, "printer.target_bed", &v) ? v : 0;
    s->chamber.on = pj_num(r.body, "printer.temp_chamber", &v);
    if (s->chamber.on) s->chamber.cur = v;
    s->has_pos = pj_num(r.body, "printer.axis_z", &v);
    if (s->has_pos) {
        s->pos[2] = v;
        s->pos[0] = pj_num(r.body, "printer.axis_x", &v) ? v : 0;
        s->pos[1] = pj_num(r.body, "printer.axis_y", &v) ? v : 0;
    }
    s->speed = pj_num(r.body, "printer.speed", &v) ? (int)v : -1;
    s->flow = pj_num(r.body, "printer.flow", &v) ? (int)v : -1;
    s->fan = -1;
    int id = 0;
    bool job = pj_int(r.body, "job.id", &id);
    s->progress = job && pj_num(r.body, "job.progress", &v) ? v : -1;
    s->remaining_s = job && pj_num(r.body, "job.time_remaining", &v) ? (int32_t)v : -1;
    s->elapsed_s = job && pj_num(r.body, "job.time_printing", &v) ? (int32_t)v : -1;
    if (!job) s->file[0] = '\0';
    p3d_publish(m);
    p3d_unlock();
    p3d_resp_free(&r);
    p->job_id = job ? id : 0;
    if (job && id != p->thumb_job) job_read(m, p);
    if (!job && p->thumb_job) {
        p->thumb_job = 0;
        p3d_thumb_store(m, NULL, 0);
    }
    return 1;
}

typedef struct {
    p3d_file_t *f;
    int         n;
    const char *dir;
    char        sub[8][160];     /* folders still to read */
    int         nsub;
} flist_t;

static bool child_cb(const char *k, int kl, const char *v, void *ctx)
{
    (void)k;
    (void)kl;
    flist_t *fl = ctx;
    char type[24] = "", name[96] = "", disp[96] = "";
    pj_str(v, "type", type, sizeof type);
    pj_str(v, "name", name, sizeof name);
    if (!pj_str(v, "display_name", disp, sizeof disp)) p3d_copy(disp, sizeof disp, name);
    if (!name[0] || name[0] == '.') return true;
    if (!strcmp(type, "FOLDER")) {
        if (fl->nsub < 8) snprintf(fl->sub[fl->nsub++], sizeof fl->sub[0], "%.100s/%.56s", fl->dir, name);
        return true;
    }
    if (strcmp(type, "PRINT_FILE") || fl->n >= P3D_FILES_MAX) return true;
    p3d_file_t *f = &fl->f[fl->n++];
    memset(f, 0, sizeof *f);
    p3d_copy(f->name, sizeof f->name, disp);
    snprintf(f->path, sizeof f->path, "%.120s/%.70s", fl->dir, name);
    float x;
    f->size = pj_num(v, "size", &x) ? (uint32_t)x : 0;
    f->date = pj_num(v, "m_timestamp", &x) ? (uint32_t)x : 0;
    f->est_s = pj_num(v, "meta.estimated print time", &x) ? (int32_t)x : -1;
    return true;
}

static bool storage_cb(const char *k, int kl, const char *v, void *ctx)
{
    (void)k;
    (void)kl;
    flist_t *fl = ctx;
    bool avail = true;
    pj_bool(v, "available", &avail);
    char path[64] = "";
    pj_str(v, "path", path, sizeof path);
    if (avail && path[0] && fl->nsub < 8) p3d_copy(fl->sub[fl->nsub++], sizeof fl->sub[0], path);
    return true;
}

static void files_read(p3d_machine_t *m, plink_t *p)
{
    p3d_files_begin(m);
    flist_t *fl = p3d_psram(sizeof *fl);
    if (!fl) return;
    memset(fl, 0, sizeof *fl);
    fl->f = p3d_psram(sizeof(p3d_file_t) * P3D_FILES_MAX);
    p3d_resp_t r;
    int st = req(p, "GET", "/api/v1/storage", &r, BODY_MAX);
    if (st == 200) pj_each(pj_get(r.body, "storage_list"), storage_cb, fl);
    p3d_resp_free(&r);
    if (st != 200 || !fl->f) {
        free(fl->f);
        free(fl);
        p3d_files_set(m, NULL, 0, st < 0 ? _("No responde") : _("No se pudo leer la lista"));
        return;
    }
    for (int done = 0; done < fl->nsub && done < 8 && !m->stop; done++) {
        char dir[160], enc[220], url[240];
        p3d_copy(dir, sizeof dir, fl->sub[done]);
        p3d_url_enc(enc, sizeof enc, dir);
        snprintf(url, sizeof url, "/api/v1/files%s", enc);
        fl->dir = dir;
        if (req(p, "GET", url, &r, 128 * 1024) == 200) pj_each(pj_get(r.body, "children"), child_cb, fl);
        p3d_resp_free(&r);
    }
    for (int i = 1; i < fl->n; i++) {
        p3d_file_t t = fl->f[i];
        int j = i - 1;
        while (j >= 0 && fl->f[j].date < t.date) {
            fl->f[j + 1] = fl->f[j];
            j--;
        }
        fl->f[j + 1] = t;
    }
    p3d_files_set(m, fl->f, fl->n, NULL);
    free(fl);
}

static void command(p3d_machine_t *m, plink_t *p, const p3d_cmd_t *c)
{
    char url[300], enc[260];
    p3d_resp_t r = { 0 };
    int st = 0;
    switch (c->op) {
    case P3D_C_PAUSE:
    case P3D_C_RESUME:
        snprintf(url, sizeof url, "/api/v1/job/%d/%s", p->job_id, c->op == P3D_C_PAUSE ? "pause" : "resume");
        st = req(p, "PUT", url, &r, 4096);
        break;
    case P3D_C_CANCEL:
        snprintf(url, sizeof url, "/api/v1/job/%d", p->job_id);
        st = req(p, "DELETE", url, &r, 4096);
        break;
    case P3D_C_START:
        p3d_url_enc(enc, sizeof enc, c->text);
        snprintf(url, sizeof url, "/api/v1/files%s", enc);
        st = p3d_http(&p->h, "POST", url, "", 0, "application/json", &r, 4096, REQ_MS);
        break;
    case P3D_C_DELETE:
        p3d_url_enc(enc, sizeof enc, c->text);
        snprintf(url, sizeof url, "/api/v1/files%s", enc);
        st = req(p, "DELETE", url, &r, 4096);
        p3d_resp_free(&r);
        files_read(m, p);
        break;
    case P3D_C_FILES:
        files_read(m, p);
        return;
    default:
        p3d_con_add(m, "! ", _("PrusaLink no acepta G-code: usá la pantalla de la impresora"));
        return;
    }
    p3d_resp_free(&r);
    if (st == 409) p3d_con_add(m, "! ", _("La impresora no está lista para eso"));
    else if (st < 0 || st >= 400) p3d_con_add(m, "! ", _("No se pudo"));
}

static void run(p3d_machine_t *m)
{
    plink_t *p = p3d_psram(sizeof *p);
    if (!p) return;
    memset(p, 0, sizeof *p);
    p3d_http_init(&p->h, m->cfg.host, p3d_cfg_port(&m->cfg), m->cfg.tls);
    p->h.stop = &m->stop;
    p3d_copy(p->h.user, sizeof p->h.user, m->cfg.user[0] ? m->cfg.user : "maker");
    p3d_copy(p->h.pass, sizeof p->h.pass, m->cfg.key);
    p->h.digest_only = true;
    int fails = 0;
    uint64_t next = 0;
    while (!m->stop) {
        p3d_cmd_t c;
        bool did = false;
        while (p3d_take(m, &c)) {
            command(m, p, &c);
            did = true;
        }
        if (did || aos_hal_uptime_ms() >= next) {
            int rc = poll(m, p);
            if (rc == 0) {
                /* the first PrusaLink: OctoPrint's API with the key */
                aos_hal_log(P3D_TAG, "%s: no /api/v1, OctoPrint's API", m->cfg.name);
                p3d_http_t h = p->h;
                h.have_digest = false;
                h.user[0] = '\0';
                if (m->cfg.key[0]) snprintf(h.extra, sizeof h.extra, "X-Api-Key: %.120s\r\n", m->cfg.key);
                free(p);
                p3d_octo_run(m, &h);
                return;
            }
            fails = rc < 0 ? fails + 1 : 0;
            next = aos_hal_uptime_ms() + (uint64_t)(fails ? p3d_backoff(m, fails) : m->poll_ms);
        }
        int wait = (int)(next - aos_hal_uptime_ms());
        if (!p3d_wait(m, wait > 0 ? wait : 0)) break;
    }
    p3d_http_close(&p->h);
    free(p);
}

const p3d_driver_t p3d_drv_prusalink = {
    .word      = "prusalink",
    .label     = "Prusa (PrusaLink)",
    .makers    = N_("Prusa MK4, MK3.9, MINI, XL, CORE One y MK3S con PrusaLink (las de Prusa Connect)"),
    .key_label = N_("Contraseña de PrusaLink"),
    .key_hint  = N_("En la impresora: Ajustes, Red, PrusaLink (usuario maker)"),
    .port      = 80,
    .tls_port  = 443,
    .fields    = P3D_F_KEY | P3D_F_USER | P3D_F_KEY_NEEDED,
    .run       = run,
};
