/*
 * P4OS - 3D printers: Creality's own WebSocket (K1, K1C, K1 Max, K2 Plus,
 * Ender-3 V3, Creality Hi, CR-10 SE): what Creality Print talks to.
 *
 * ws://<host>:9999 with JSON objects both ways. The printer pushes its
 * fields as they change (many as strings: "nozzleTemp":"219.96"), and
 * takes {"method":"get","params":{...}} and {"method":"set","params":
 * {...}}:
 *
 *   get  reqPrinterPara       everything at once
 *        reqGcodeFile         the files ("retGcodeFileInfo")
 *   set  pause 1 / 0, stop 1, nozzleTempControl, bedTempControl {num, val},
 *        gcodeCmd, lightSw, setFeedratePct, opGcodeFile "printprt:<path>"
 *
 * Creality did not publish it; this follows what Creality Print sends and
 * what the community integrations read (ha_creality_ws). A K1 rooted with
 * Moonraker is better added as Klipper. The camera is mjpg-streamer on
 * port 8080 (the K2's WebRTC is not something the board can show).
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CONNECT_MS  6000
#define SILENT_MS   30000

typedef struct {
    aos_ws_t *ws;
    uint64_t  last_rx, last_ask;
    int       st, dev, cur;     /* the last of each: they come one at a time */
} crea_t;

static bool set(crea_t *c, const char *params)
{
    char j[400];
    snprintf(j, sizeof j, "{\"method\":\"set\",\"params\":%s}", params);
    return aos_hal_ws_send_text(c->ws, j, -1) == 0;
}

static bool get(crea_t *c, const char *what)
{
    char j[160];
    snprintf(j, sizeof j, "{\"method\":\"get\",\"params\":{\"%s\":1}}", what);
    return aos_hal_ws_send_text(c->ws, j, -1) == 0;
}

static int state_of(int st, int dev, int cur)
{
    /* state: 0 idle, 1 printing, 2 done, 3 failed, 4 stopped, 5 paused */
    switch (st) {
    case 1: return P3D_ST_PRINTING;
    case 2: return P3D_ST_DONE;
    case 3: return P3D_ST_ERROR;
    case 4: return P3D_ST_CANCELLED;
    case 5: return P3D_ST_PAUSED;
    default: return dev != 0 || cur != 0 ? P3D_ST_BUSY : P3D_ST_IDLE;
    }
}

typedef struct {
    p3d_file_t *f;
    int         n;
} flist_t;

static bool file_cb(const char *k, int kl, const char *v, void *ctx)
{
    (void)k;
    (void)kl;
    flist_t *fl = ctx;
    if (fl->n >= P3D_FILES_MAX) return false;
    char path[192] = "", name[96] = "";
    if (!pj_str(v, "path", path, sizeof path)) return true;
    if (!pj_str(v, "name", name, sizeof name)) {
        const char *b = strrchr(path, '/');
        p3d_copy(name, sizeof name, b ? b + 1 : path);
    } else {
        /* some firmware gives the folder in path and the file in name */
        size_t pl = strlen(path), nl = strlen(name);
        if (pl < nl || strcmp(path + pl - nl, name)) {
            char both[192];
            snprintf(both, sizeof both, "%.120s%s%.70s", path, pl && path[pl - 1] == '/' ? "" : "/", name);
            p3d_copy(path, sizeof path, both);
        }
    }
    p3d_file_t *f = &fl->f[fl->n++];
    memset(f, 0, sizeof *f);
    p3d_copy(f->name, sizeof f->name, name);
    p3d_copy(f->path, sizeof f->path, path);
    float x;
    f->size = pj_num(v, "file_size", &x) ? (uint32_t)x : 0;
    f->date = pj_num(v, "create_time", &x) ? (uint32_t)x : 0;
    f->est_s = pj_num(v, "timeCost", &x) ? (int32_t)x : -1;
    return true;
}

static void message(p3d_machine_t *m, crea_t *c, const char *js)
{
    const char *files = pj_get(js, "retGcodeFileInfo.fileInfo");
    if (!files) files = pj_get(js, "retGcodeFileInfo");
    if (files && (*files == '[' || *files == '{')) {
        flist_t fl = { p3d_psram(sizeof(p3d_file_t) * P3D_FILES_MAX), 0 };
        if (fl.f) pj_each(files, file_cb, &fl);
        p3d_files_set(m, fl.f, fl.n, NULL);
        return;
    }
    p3d_lock();
    p3d_status_t *s = &m->st;
    s->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_TEMP | P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_EXTRUDE |
              P3D_CAP_FAN | P3D_CAP_SPEED | P3D_CAP_FLOW | P3D_CAP_GCODE | P3D_CAP_FILES | P3D_CAP_LIGHT |
              P3D_CAP_MOTORS_OFF;
    float v;
    int n;
    if (pj_num(js, "nozzleTemp", &v)) { s->tool[0].on = true; s->tool[0].cur = v; }
    if (pj_num(js, "targetNozzleTemp", &v)) s->tool[0].target = v;
    if (pj_num(js, "bedTemp0", &v)) { s->bed.on = true; s->bed.cur = v; }
    if (pj_num(js, "targetBedTemp0", &v)) s->bed.target = v;
    if (pj_num(js, "boxTemp", &v) && v > 0) { s->chamber.on = true; s->chamber.cur = v; }
    bool any = false;
    if (pj_int(js, "state", &n)) { c->st = n; any = true; }
    if (pj_int(js, "deviceState", &n)) { c->dev = n; any = true; }
    if (pj_int(js, "printStatus", &n)) { c->cur = n; any = true; }
    if (any || s->state <= P3D_ST_CONNECTING) {
        s->state = state_of(c->st, c->dev, 0);
        snprintf(s->raw, sizeof s->raw, "%d/%d/%d", c->st, c->dev, c->cur);
    }
    if (pj_num(js, "printProgress", &v)) s->progress = v;
    if (pj_num(js, "printJobTime", &v)) s->elapsed_s = (int32_t)v;
    if (pj_num(js, "printLeftTime", &v)) s->remaining_s = (int32_t)v;
    if (pj_int(js, "layer", &n)) s->layer = n;
    if (pj_int(js, "TotalLayer", &n)) s->layers = n;
    char buf[160];
    if (pj_str(js, "printFileName", buf, sizeof buf)) {
        const char *b = strrchr(buf, '/');
        p3d_copy(s->file, sizeof s->file, b ? b + 1 : buf);
    }
    if (pj_str(js, "curPosition", buf, sizeof buf)) {
        const char *x = strstr(buf, "X:"), *y = strstr(buf, "Y:"), *z = strstr(buf, "Z:");
        if (x && y && z) {
            s->pos[0] = strtof(x + 2, NULL);
            s->pos[1] = strtof(y + 2, NULL);
            s->pos[2] = strtof(z + 2, NULL);
            s->has_pos = true;
        }
    }
    if (pj_int(js, "curFeedratePct", &n)) s->speed = n;
    if (pj_int(js, "curFlowratePct", &n)) s->flow = n;
    if (pj_int(js, "modelFanPct", &n)) s->fan = n;
    else if (pj_int(js, "fan", &n)) s->fan = n ? 100 : 0;
    if (pj_int(js, "lightSw", &n)) s->light = n ? 1 : 0;
    if (pj_str(js, "model", buf, sizeof buf)) p3d_copy(s->model, sizeof s->model, buf);
    if (pj_str(js, "modelVersion", buf, sizeof buf)) snprintf(s->fw, sizeof s->fw, "Creality %.38s", buf);
    if (pj_int(js, "err.errcode", &n)) {
        if (n) snprintf(s->msg, sizeof s->msg, _("Error %d"), n);
        else s->msg[0] = '\0';
    }
    if (s->state != P3D_ST_PRINTING && s->state != P3D_ST_PAUSED && s->state != P3D_ST_DONE) {
        s->progress = -1;
        s->remaining_s = -1;
        s->elapsed_s = -1;
    }
    if (!s->cam_url[0] && m->cfg.camera[0] == '\0') {
        snprintf(s->cam_url, sizeof s->cam_url, "http://%.96s:8080/?action=snapshot", m->cfg.host);
    }
    p3d_publish(m);
    p3d_unlock();
}

static void command(p3d_machine_t *m, crea_t *c, const p3d_cmd_t *cmd)
{
    char p[300], g[260], esc[300];
    switch (cmd->op) {
    case P3D_C_PAUSE: set(c, "{\"pause\":1}"); break;
    case P3D_C_RESUME: set(c, "{\"pause\":0}"); break;
    case P3D_C_CANCEL: set(c, "{\"stop\":1}"); break;
    case P3D_C_LIGHT: set(c, cmd->a ? "{\"lightSw\":1}" : "{\"lightSw\":0}"); break;
    case P3D_C_FILES:
        p3d_files_begin(m);
        get(c, "reqGcodeFile");
        break;
    case P3D_C_START:
        p3d_json_esc(esc, sizeof esc, cmd->text);
        snprintf(p, sizeof p, "{\"opGcodeFile\":\"printprt:%.250s\"}", esc);
        set(c, p);
        break;
    case P3D_C_DELETE:
        p3d_json_esc(esc, sizeof esc, cmd->text);
        snprintf(p, sizeof p, "{\"opGcodeFile\":\"deleteprt:%.250s\"}", esc);
        set(c, p);
        get(c, "reqGcodeFile");
        break;
    case P3D_C_TEMP:
        if (cmd->a == 2) snprintf(p, sizeof p, "{\"bedTempControl\":{\"num\":0,\"val\":%d}}", (int)roundf(cmd->v[0]));
        else snprintf(p, sizeof p, "{\"nozzleTempControl\":%d}", (int)roundf(cmd->v[0]));
        set(c, p);
        break;
    case P3D_C_SPEED:
        snprintf(p, sizeof p, "{\"setFeedratePct\":%d}", (int)roundf(cmd->v[0]));
        set(c, p);
        break;
    default:
        if (p3d_gcode_of(cmd, g, sizeof g) > 0) {
            p3d_json_esc(esc, sizeof esc, g);
            snprintf(p, sizeof p, "{\"gcodeCmd\":\"%.280s\"}", esc);
            set(c, p);
            if (cmd->op == P3D_C_GCODE) p3d_con_add(m, "> ", g);
        }
        break;
    }
}

static void run(p3d_machine_t *m)
{
    crea_t c = { 0 };
    int fails = 0;
    char url[160];
    snprintf(url, sizeof url, "ws://%.96s:%d", m->cfg.host, p3d_cfg_port(&m->cfg));
    while (!m->stop) {
        memset(&c, 0, sizeof c);
        c.ws = aos_hal_ws_open(url, NULL, CONNECT_MS);
        if (!c.ws) {
            fails++;
            p3d_offline(m, _("No responde"));
            if (!p3d_wait(m, p3d_backoff(m, fails))) break;
            continue;
        }
        fails = 0;
        c.last_rx = c.last_ask = aos_hal_uptime_ms();
        get(&c, "reqPrinterPara");
        while (!m->stop && aos_hal_ws_is_open(c.ws)) {
            p3d_cmd_t cmd;
            while (p3d_take(m, &cmd)) command(m, &c, &cmd);
            const char *msg;
            int n = aos_hal_ws_recv_text(c.ws, &msg, 200);
            if (n < 0) break;
            uint64_t now = aos_hal_uptime_ms();
            if (n > 0) {
                c.last_rx = now;
                if (msg[0] == '{') {
                    if (pj_get(msg, "ModeCode")) aos_hal_ws_send_text(c.ws, "{\"ModeCode\":\"heart_beat\",\"msg\":\"ok\"}", -1);
                    else message(m, &c, msg);
                }
            }
            if (now - c.last_ask >= (uint64_t)(m->poll_ms > 3000 ? m->poll_ms : 3000)) {
                c.last_ask = now;
                get(&c, "reqPrinterPara");
            }
            if (now - c.last_rx > SILENT_MS) break;
        }
        aos_hal_ws_close(c.ws);
        c.ws = NULL;
        if (!m->stop) {
            p3d_offline(m, _("Se cortó la conexión"));
            p3d_wait(m, 2000);
        }
    }
}

const p3d_driver_t p3d_drv_creality = {
    .word      = "creality",
    .label     = "Creality",
    .makers    = N_("K1, K1C, K1 Max, K2 Plus, Ender-3 V3 y Creality Hi (sin root)"),
    .key_label = NULL,
    .key_hint  = NULL,
    .port      = 9999,
    .tls_port  = 9999,
    .fields    = 0,
    .run       = run,
};
