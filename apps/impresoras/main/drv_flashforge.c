/*
 * P4OS - 3D printers: FlashForge.
 *
 * Two ways in, chosen by whether the check code is typed:
 *
 *  - The Adventurer 5M, 5M Pro and AD5X (and newer) serve HTTP on port 8898.
 *    Every request is a POST with the serial number and the "check code"
 *    the printer shows (Settings, Network, "LAN only"):
 *
 *        /detail     everything: temperatures, the job, fans, the light, the
 *                    camera's address
 *        /control    {"cmd":"jobCtl_cmd" | "lightControl_cmd" |
 *                    "printerCtl_cmd", "args":{...}}
 *        /gcodeList, /printGcode    the files, and printing one
 *
 *  - Every FlashForge with a network (Adventurer 3 and 4, Creator, Finder,
 *    Guider, and the 5M too) speaks G-code on TCP port 8899, each line
 *    starting with '~': M601 S1 takes control, M119 the state, M105 the
 *    temperatures, M27 the progress, M115 the machine. That is the whole
 *    driver when there is no check code, and how temperatures, moves and
 *    the console go to the new ones (HTTP has no G-code).
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define HTTP_PORT   8898
#define TCP_PORT    8899
#define REQ_MS      6000
#define TCP_MS      3000

typedef struct {
    p3d_http_t h;
    void      *tcp;             /* the G-code connection, opened when needed */
    bool       http;            /* the new API */
    bool       info;
    char       auth[240];       /* "serialNumber":..,"checkCode":.. */
} ff_t;

/* ---- the G-code connection ------------------------------------------------------- */

static void tcp_close(ff_t *f)
{
    if (f->tcp) p3d_sock_close(f->tcp);
    f->tcp = NULL;
}

/* one command and its answer, up to "ok"; false: no answer */
static bool tcp_cmd(p3d_machine_t *m, ff_t *f, const char *g, char *out, size_t n)
{
    for (int round = 0; round < 2; round++) {
        if (!f->tcp) {
            char why[64];
            f->tcp = p3d_sock_open(m->cfg.host, TCP_PORT, false, 3000, why, sizeof why);
            if (!f->tcp) return false;
            char tmp[256];
            p3d_sock_send(f->tcp, "~M601 S1\r\n", 10, TCP_MS);
            /* the control answer, whatever it says */
            uint64_t until = aos_hal_uptime_ms() + TCP_MS;
            int got = 0;
            while (aos_hal_uptime_ms() < until && got < (int)sizeof tmp - 1) {
                int k = p3d_sock_recv(f->tcp, tmp + got, (int)sizeof tmp - 1 - got, 200);
                if (k < 0) break;
                got += k;
                tmp[got] = '\0';
                if (strstr(tmp, "ok\r\n") || strstr(tmp, "ok\n")) break;
            }
        }
        char line[240];
        int l = snprintf(line, sizeof line, "~%.230s\r\n", g);
        if (!p3d_sock_send(f->tcp, line, l, TCP_MS)) {
            tcp_close(f);
            continue;
        }
        size_t got = 0;
        out[0] = '\0';
        uint64_t until = aos_hal_uptime_ms() + TCP_MS;
        while (aos_hal_uptime_ms() < until && got + 1 < n) {
            int k = p3d_sock_recv(f->tcp, out + got, (int)(n - 1 - got), 200);
            if (k < 0) break;
            got += (size_t)k;
            out[got] = '\0';
            if (strstr(out, "ok\r\n") || strstr(out, "ok\n")) return true;
            if (m->stop) break;
        }
        tcp_close(f);
        if (got) return true;
    }
    return false;
}

/* ---- the HTTP API ------------------------------------------------------------------ */

static int post(ff_t *f, const char *path, const char *extra, p3d_resp_t *r, int max)
{
    char body[600];
    snprintf(body, sizeof body, "{%s%s%s}", f->auth, extra && extra[0] ? "," : "", extra ? extra : "");
    return p3d_http(&f->h, "POST", path, body, -1, "application/json", r, max, REQ_MS);
}

static int state_of(const char *s)
{
    if (!strcmp(s, "printing") || !strcmp(s, "BUILDING_FROM_SD")) return P3D_ST_PRINTING;
    if (!strcmp(s, "paused") || !strcmp(s, "pausing") || !strcmp(s, "PAUSED")) return P3D_ST_PAUSED;
    if (!strcmp(s, "completed") || !strcmp(s, "BUILDING_COMPLETED")) return P3D_ST_DONE;
    if (!strcmp(s, "cancel") || !strcmp(s, "canceled")) return P3D_ST_CANCELLED;
    if (!strcmp(s, "error")) return P3D_ST_ERROR;
    if (!strcmp(s, "busy") || !strcmp(s, "heating") || !strcmp(s, "calibrate_doing") || !strcmp(s, "BUSY")) return P3D_ST_BUSY;
    return P3D_ST_IDLE;
}

static int poll_http(p3d_machine_t *m, ff_t *f)
{
    p3d_resp_t r;
    int st = post(f, "/detail", NULL, &r, 16 * 1024);
    int code = -1;
    if (st == 200) pj_int(r.body, "code", &code);
    if (st != 200 || code != 0) {
        char msg[64] = "";
        if (st == 200) pj_str(r.body, "message", msg, sizeof msg);
        p3d_offline(m, st < 0 ? _("No responde") : msg[0] ? msg : _("Número de serie o código incorrectos"));
        p3d_resp_free(&r);
        return -1;
    }
    const char *d = pj_get(r.body, "detail");
    p3d_lock();
    p3d_status_t *s = &m->st;
    s->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_TEMP | P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_EXTRUDE |
              P3D_CAP_FAN | P3D_CAP_SPEED | P3D_CAP_GCODE | P3D_CAP_FILES | P3D_CAP_LIGHT | P3D_CAP_MOTORS_OFF |
              P3D_CAP_REPLIES;
    char state[24] = "";
    pj_str(d, "status", state, sizeof state);
    p3d_copy(s->raw, sizeof s->raw, state);
    s->state = state_of(state);
    float v;
    s->tool[0].on = pj_num(d, "rightTemp", &v);
    s->tool[0].cur = v;
    s->tool[0].target = pj_num(d, "rightTargetTemp", &v) ? v : 0;
    s->tool[1].on = pj_num(d, "leftTemp", &v) && v > 0;
    if (s->tool[1].on) {
        s->tool[1].cur = v;
        s->tool[1].target = pj_num(d, "leftTargetTemp", &v) ? v : 0;
    }
    s->bed.on = pj_num(d, "platTemp", &v);
    s->bed.cur = v;
    s->bed.target = pj_num(d, "platTargetTemp", &v) ? v : 0;
    s->chamber.on = pj_num(d, "chamberTemp", &v) && v > 0;
    s->chamber.cur = v;
    bool active = s->state == P3D_ST_PRINTING || s->state == P3D_ST_PAUSED;
    s->progress = active && pj_num(d, "printProgress", &v) ? v * 100 : s->state == P3D_ST_DONE ? 100 : -1;
    s->elapsed_s = active && pj_num(d, "printDuration", &v) ? (int32_t)v : -1;
    float est;
    s->remaining_s = active && pj_num(d, "estimatedTime", &est) ? (int32_t)est : -1;
    int n;
    s->layer = pj_int(d, "printLayer", &n) ? n : 0;
    s->layers = pj_int(d, "targetPrintLayer", &n) ? n : 0;
    s->fan = pj_int(d, "coolingFanSpeed", &n) ? n : -1;
    s->speed = pj_int(d, "printSpeedAdjust", &n) ? n : -1;
    char buf[160];
    s->light = pj_str(d, "lightStatus", buf, sizeof buf) ? !strcmp(buf, "open") : -1;
    if (pj_str(d, "printFileName", buf, sizeof buf)) p3d_copy(s->file, sizeof s->file, buf);
    if (pj_str(d, "name", buf, sizeof buf)) p3d_copy(s->model, sizeof s->model, buf);
    if (pj_str(d, "firmwareVersion", buf, sizeof buf)) snprintf(s->fw, sizeof s->fw, "FlashForge %.36s", buf);
    if (m->cfg.camera[0] == '\0' && pj_str(d, "cameraStreamUrl", buf, sizeof buf) && buf[0]) {
        p3d_copy(s->cam_url, sizeof s->cam_url, buf);
    }
    s->msg[0] = '\0';
    pj_str(d, "errorCode", s->msg, sizeof s->msg);
    if (!strcmp(s->msg, "0") || !strcmp(s->msg, "")) s->msg[0] = '\0';
    p3d_publish(m);
    p3d_unlock();
    p3d_resp_free(&r);
    return 0;
}

/* ---- the G-code-only machines ------------------------------------------------------- */

static int poll_tcp(p3d_machine_t *m, ff_t *f)
{
    char a[512], t[256], p[256];
    if (!tcp_cmd(m, f, "M119", a, sizeof a)) {
        p3d_offline(m, _("No responde"));
        return -1;
    }
    tcp_cmd(m, f, "M105", t, sizeof t);
    tcp_cmd(m, f, "M27", p, sizeof p);
    if (!f->info) {
        char i[512];
        if (tcp_cmd(m, f, "M115", i, sizeof i)) {
            char *mt = strstr(i, "Machine Type:");
            char *fw = strstr(i, "Firmware:");
            p3d_lock();
            if (mt) sscanf(mt + 13, " %47[^\r\n]", m->st.model);
            if (fw) sscanf(fw + 9, " %47[^\r\n]", m->st.fw);
            p3d_unlock();
        }
        f->info = true;
    }
    p3d_lock();
    p3d_status_t *s = &m->st;
    s->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_TEMP | P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_EXTRUDE |
              P3D_CAP_FAN | P3D_CAP_GCODE | P3D_CAP_MOTORS_OFF | P3D_CAP_REPLIES;
    char ms[32] = "";
    char *x = strstr(a, "MachineStatus:");
    if (x) sscanf(x + 14, " %31s", ms);
    p3d_copy(s->raw, sizeof s->raw, ms);
    s->state = state_of(ms);
    x = strstr(a, "CurrentFile:");
    s->file[0] = '\0';
    if (x) sscanf(x + 12, " %127[^\r\n]", s->file);
    /* T0:210 /210 B:60 /60 */
    float c, tg;
    if ((x = strstr(t, "T0:")) && sscanf(x + 3, "%f /%f", &c, &tg) == 2) {
        s->tool[0] = (p3d_temp_t){ c, tg, true };
    } else if ((x = strstr(t, "T:")) && sscanf(x + 2, "%f /%f", &c, &tg) == 2) {
        s->tool[0] = (p3d_temp_t){ c, tg, true };
    }
    if ((x = strstr(t, "B:")) && sscanf(x + 2, "%f /%f", &c, &tg) == 2) s->bed = (p3d_temp_t){ c, tg, true };
    /* SD printing byte 1234/5678 */
    unsigned long done = 0, all = 0;
    s->progress = -1;
    if ((x = strstr(p, "byte")) && sscanf(x + 4, " %lu/%lu", &done, &all) == 2 && all > 0) {
        s->progress = (float)((double)done * 100.0 / (double)all);
    }
    int l = 0, ls = 0;
    if ((x = strstr(p, "Layer:")) && sscanf(x + 6, " %d/%d", &l, &ls) == 2) {
        s->layer = l;
        s->layers = ls;
    }
    s->remaining_s = s->elapsed_s = -1;
    p3d_publish(m);
    p3d_unlock();
    return 0;
}

/* ---- files and commands ---------------------------------------------------------------- */

static void files_read(p3d_machine_t *m, ff_t *f)
{
    p3d_files_begin(m);
    if (!f->http) {
        p3d_files_set(m, NULL, 0, _("Esta impresora no da la lista de archivos por la red"));
        return;
    }
    p3d_resp_t r;
    int st = post(f, "/gcodeList", NULL, &r, 64 * 1024);
    if (st != 200) {
        p3d_resp_free(&r);
        p3d_files_set(m, NULL, 0, st < 0 ? _("No responde") : _("No se pudo leer la lista"));
        return;
    }
    p3d_file_t *fl = p3d_psram(sizeof(p3d_file_t) * P3D_FILES_MAX);
    int n = 0;
    const char *arr = pj_get(r.body, "gcodeList");
    for (int i = 0; fl && arr && n < P3D_FILES_MAX; i++) {
        char p[16], name[96] = "";
        snprintf(p, sizeof p, "%d", i);
        const char *v = pj_get(arr, p);
        if (!v) break;
        if (*v == '{') {
            if (!pj_str(v, "gcodeFileName", name, sizeof name)) pj_str(v, "fileName", name, sizeof name);
        } else {
            pj_strv(v, name, sizeof name);
        }
        if (!name[0]) continue;
        memset(&fl[n], 0, sizeof fl[n]);
        p3d_copy(fl[n].name, sizeof fl[n].name, name);
        p3d_copy(fl[n].path, sizeof fl[n].path, name);
        fl[n].est_s = -1;
        float x;
        if (*v == '{' && pj_num(v, "printingTime", &x)) fl[n].est_s = (int32_t)x;
        n++;
    }
    p3d_resp_free(&r);
    p3d_files_set(m, fl, n, NULL);
}

static void control(ff_t *f, const char *cmd, const char *args)
{
    char extra[400];
    snprintf(extra, sizeof extra, "\"payload\":{\"cmd\":\"%s\",\"args\":%s}", cmd, args);
    p3d_resp_t r;
    post(f, "/control", extra, &r, 4096);
    p3d_resp_free(&r);
}

static void gcode(p3d_machine_t *m, ff_t *f, const char *g, bool echo)
{
    if (echo) p3d_con_add(m, "> ", g);
    const char *p = g;
    while (*p) {
        const char *e = strchr(p, '\n');
        char line[200], ans[512];
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l);
        line[l] = '\0';
        if (line[0]) {
            if (tcp_cmd(m, f, line, ans, sizeof ans)) {
                if (echo) p3d_con_add(m, "", ans);
            } else {
                p3d_con_add(m, "! ", _("No responde"));
                break;
            }
        }
        p = e ? e + 1 : p + l;
    }
}

static void command(p3d_machine_t *m, ff_t *f, const p3d_cmd_t *c)
{
    char g[260], a[300], esc[220];
    if (f->http) {
        switch (c->op) {
        case P3D_C_PAUSE: control(f, "jobCtl_cmd", "{\"jobID\":\"\",\"action\":\"pause\"}"); return;
        case P3D_C_RESUME: control(f, "jobCtl_cmd", "{\"jobID\":\"\",\"action\":\"continue\"}"); return;
        case P3D_C_CANCEL: control(f, "jobCtl_cmd", "{\"jobID\":\"\",\"action\":\"cancel\"}"); return;
        case P3D_C_LIGHT:
            control(f, "lightControl_cmd", c->a ? "{\"status\":\"open\"}" : "{\"status\":\"close\"}");
            return;
        case P3D_C_SPEED:
            snprintf(a, sizeof a, "{\"zAxisCompensation\":0,\"speed\":%d,\"chamberFan\":100,\"coolingFan\":%d,\"coolingLeftFan\":0}",
                     (int)roundf(c->v[0]), m->st.fan >= 0 ? m->st.fan : 100);
            control(f, "printerCtl_cmd", a);
            return;
        case P3D_C_FAN:
            snprintf(a, sizeof a, "{\"zAxisCompensation\":0,\"speed\":%d,\"chamberFan\":100,\"coolingFan\":%d,\"coolingLeftFan\":0}",
                     m->st.speed > 0 ? m->st.speed : 100, (int)roundf(c->v[0]));
            control(f, "printerCtl_cmd", a);
            return;
        case P3D_C_START: {
            p3d_json_esc(esc, sizeof esc, c->text);
            snprintf(a, sizeof a, "\"fileName\":\"%s\",\"levelingBeforePrint\":true", esc);
            p3d_resp_t r;
            post(f, "/printGcode", a, &r, 4096);
            p3d_resp_free(&r);
            return;
        }
        case P3D_C_FILES: files_read(m, f); return;
        default: break;
        }
    }
    switch (c->op) {
    case P3D_C_PAUSE: gcode(m, f, "M25", false); break;
    case P3D_C_RESUME: gcode(m, f, "M24", false); break;
    case P3D_C_CANCEL: gcode(m, f, "M26", false); break;
    case P3D_C_FILES: files_read(m, f); break;
    case P3D_C_START:
    case P3D_C_DELETE:
        p3d_con_add(m, "! ", _("Esta impresora no lo acepta por la red"));
        break;
    default:
        if (p3d_gcode_of(c, g, sizeof g) > 0) gcode(m, f, g, c->op == P3D_C_GCODE);
        break;
    }
}

static void run(p3d_machine_t *m)
{
    ff_t *f = p3d_psram(sizeof *f);
    if (!f) return;
    memset(f, 0, sizeof *f);
    f->http = m->cfg.key[0] != '\0';
    p3d_http_init(&f->h, m->cfg.host, m->cfg.port > 0 ? m->cfg.port : HTTP_PORT, false);
    f->h.stop = &m->stop;
    char es[64], ek[130];
    p3d_json_esc(es, sizeof es, m->cfg.serial);
    p3d_json_esc(ek, sizeof ek, m->cfg.key);
    snprintf(f->auth, sizeof f->auth, "\"serialNumber\":\"%.60s\",\"checkCode\":\"%.120s\"", es, ek);
    int fails = 0;
    uint64_t next = 0;
    while (!m->stop) {
        p3d_cmd_t c;
        bool did = false;
        while (p3d_take(m, &c)) {
            command(m, f, &c);
            did = true;
        }
        if (did || aos_hal_uptime_ms() >= next) {
            int rc = f->http ? poll_http(m, f) : poll_tcp(m, f);
            fails = rc < 0 ? fails + 1 : 0;
            next = aos_hal_uptime_ms() + (uint64_t)(fails ? p3d_backoff(m, fails) : m->poll_ms);
        }
        int wait = (int)(next - aos_hal_uptime_ms());
        if (!p3d_wait(m, wait > 0 ? wait : 0)) break;
    }
    tcp_close(f);
    p3d_http_close(&f->h);
    free(f);
}

const p3d_driver_t p3d_drv_flashforge = {
    .word      = "flashforge",
    .label     = "FlashForge",
    .makers    = N_("Adventurer 5M, 5M Pro y AD5X (con código); Adventurer 3 y 4, Creator, Finder (sin código)"),
    .key_label = N_("Código de verificación (check code)"),
    .key_hint  = N_("En la impresora: Ajustes, Red, Sólo LAN. Vacío: las viejas, por el puerto 8899"),
    .port      = HTTP_PORT,
    .tls_port  = HTTP_PORT,
    .fields    = P3D_F_KEY | P3D_F_SERIAL,
    .run       = run,
};
