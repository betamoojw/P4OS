/*
 * P4OS - 3D printers: Elegoo's SDCP 3 (github.com/cbd-tech/SDCP-Smart-Device-
 * Control-Protocol-V3.0.0): the Centauri Carbon, and the resin Saturn and
 * Mars with Wi-Fi.
 *
 * A WebSocket on port 3030 (ws://<host>:3030/websocket) with JSON both
 * ways. The printer pushes "sdcp/status/<mainboard>" when something
 * changes and answers on "sdcp/response/<mainboard>"; a request names the
 * mainboard in its topic, which is learnt from the first message (or typed
 * in the serial field). Requests are numbered commands:
 *
 *     0 status   1 attributes   128 print a file   129 pause   130 stop
 *   131 resume 258 list files  259 delete files   386 the camera's stream
 *   401 move an axis   402 home   403 temperatures, fans, speed, light
 *
 * 401-403 are the Centauri's (an FDM machine), not in the resin spec.
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CONNECT_MS  6000
#define STATUS_MS   5000            /* ask even if it pushes: some firmware only answers */
#define PING_MS     20000
#define SILENT_MS   40000

typedef struct {
    aos_ws_t *ws;
    char      board[48];
    uint32_t  rid;
    bool      attrs, video;
    uint64_t  last_rx, last_ask, last_ping;
    bool      want_files;
} elegoo_t;

static bool send_cmd(elegoo_t *e, int cmd, const char *data)
{
    char j[700];
    uint32_t ts = aos_hal_time_is_valid() ? (uint32_t)time(NULL) : (uint32_t)(aos_hal_uptime_ms() / 1000);
    snprintf(j, sizeof j,
             "{\"Id\":\"\",\"Data\":{\"Cmd\":%d,\"Data\":%s,\"RequestID\":\"%016x\",\"MainboardID\":\"%s\","
             "\"TimeStamp\":%u,\"From\":1},\"Topic\":\"sdcp/request/%s\"}",
             cmd, data && data[0] ? data : "{}", (unsigned)++e->rid, e->board, (unsigned)ts, e->board);
    return aos_hal_ws_send_text(e->ws, j, -1) == 0;
}

static int state_of(int cur, int ps)
{
    switch (ps) {
    case 6: return P3D_ST_PAUSED;
    case 5: case 7: return P3D_ST_BUSY;
    case 8: return P3D_ST_CANCELLED;
    case 9: return P3D_ST_DONE;
    case 0: return cur == 1 ? P3D_ST_BUSY : P3D_ST_IDLE;
    case 2: case 3: case 4: case 13: return P3D_ST_PRINTING;
    default: return cur == 1 ? P3D_ST_PRINTING : P3D_ST_BUSY;
    }
}

static void status(p3d_machine_t *m, const char *js)
{
    const char *s = pj_get(js, "Status");
    if (!s) return;
    p3d_lock();
    p3d_status_t *t = &m->st;
    t->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_TEMP | P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_FAN |
              P3D_CAP_SPEED | P3D_CAP_FILES | P3D_CAP_LIGHT | P3D_CAP_DELETE;
    float v;
    int cur = 0, ps = 0, n;
    pj_int(s, "CurrentStatus.0", &cur);
    pj_int(s, "PrintInfo.Status", &ps);
    t->state = state_of(cur, ps);
    snprintf(t->raw, sizeof t->raw, "%d/%d", cur, ps);
    if (pj_num(s, "TempOfNozzle", &v)) { t->tool[0].on = true; t->tool[0].cur = v; }
    if (pj_num(s, "TempTargetNozzle", &v)) t->tool[0].target = v;
    if (pj_num(s, "TempOfHotbed", &v)) { t->bed.on = true; t->bed.cur = v; }
    if (pj_num(s, "TempTargetHotbed", &v)) t->bed.target = v;
    if (pj_num(s, "TempOfBox", &v) && v > 0) { t->chamber.on = true; t->chamber.cur = v; }
    if (pj_num(s, "TempTargetBox", &v)) t->chamber.target = v;
    char coord[48];
    if (pj_str(s, "CurrenCoord", coord, sizeof coord)) {
        t->has_pos = sscanf(coord, "%f,%f,%f", &t->pos[0], &t->pos[1], &t->pos[2]) == 3;
    }
    if (pj_int(s, "CurrentFanSpeed.ModelFan", &n)) t->fan = n;
    if (pj_int(s, "LightStatus.SecondLight", &n)) t->light = n ? 1 : 0;
    if (pj_int(s, "PrintInfo.PrintSpeedPct", &n) || pj_int(s, "PrintSpeedPct", &n) || pj_int(s, "PrintSpeed", &n)) t->speed = n;
    char name[128];
    if (pj_str(s, "PrintInfo.Filename", name, sizeof name)) {
        const char *b = strrchr(name, '/');
        p3d_copy(t->file, sizeof t->file, b ? b + 1 : name);
    }
    int layer = 0, layers = 0;
    pj_int(s, "PrintInfo.CurrentLayer", &layer);
    pj_int(s, "PrintInfo.TotalLayer", &layers);
    t->layer = layer;
    t->layers = layers;
    bool active = t->state == P3D_ST_PRINTING || t->state == P3D_ST_PAUSED || t->state == P3D_ST_BUSY;
    if (pj_num(s, "PrintInfo.Progress", &v)) t->progress = v;
    else t->progress = layers > 0 ? layer * 100.0f / layers : -1;
    float ct = 0, tt = 0;
    pj_num(s, "PrintInfo.CurrentTicks", &ct);
    pj_num(s, "PrintInfo.TotalTicks", &tt);
    if (tt > 20 * 24 * 3600) {
        /* the resin machines count milliseconds */
        ct /= 1000;
        tt /= 1000;
    }
    t->elapsed_s = active ? (int32_t)ct : -1;
    t->remaining_s = active && tt > ct ? (int32_t)(tt - ct) : (t->state == P3D_ST_DONE ? 0 : -1);
    if (!active && t->state != P3D_ST_DONE) t->progress = -1;
    int err = 0;
    pj_int(s, "PrintInfo.ErrorNumber", &err);
    if (err) snprintf(t->msg, sizeof t->msg, _("Error %d"), err);
    else t->msg[0] = '\0';
    p3d_publish(m);
    p3d_unlock();
}

static void attributes(p3d_machine_t *m, const char *js)
{
    const char *a = pj_get(js, "Attributes");
    char name[48] = "", fw[40] = "";
    if (!pj_str(a, "MachineName", name, sizeof name)) pj_str(a, "Name", name, sizeof name);
    pj_str(a, "FirmwareVersion", fw, sizeof fw);
    p3d_lock();
    if (name[0]) p3d_copy(m->st.model, sizeof m->st.model, name);
    if (fw[0]) snprintf(m->st.fw, sizeof m->st.fw, "Elegoo %.38s", fw);
    p3d_unlock();
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
    char name[192] = "";
    int type = 1;
    if (!pj_str(v, "name", name, sizeof name)) return true;
    pj_int(v, "type", &type);
    if (type != 1 || fl->n >= P3D_FILES_MAX) return true;
    p3d_file_t *f = &fl->f[fl->n++];
    memset(f, 0, sizeof *f);
    p3d_copy(f->path, sizeof f->path, name);
    const char *b = strrchr(name, '/');
    p3d_copy(f->name, sizeof f->name, b ? b + 1 : name);
    float x;
    f->size = pj_num(v, "usedSize", &x) ? (uint32_t)x : 0;
    f->est_s = -1;
    return true;
}

static void response(p3d_machine_t *m, elegoo_t *e, const char *js)
{
    int cmd = -1;
    pj_int(js, "Data.Cmd", &cmd);
    const char *d = pj_get(js, "Data.Data");
    if (cmd == 258) {
        flist_t fl = { p3d_psram(sizeof(p3d_file_t) * P3D_FILES_MAX), 0 };
        if (fl.f) pj_each(pj_get(d, "FileList"), file_cb, &fl);
        p3d_files_set(m, fl.f, fl.n, NULL);
    } else if (cmd == 386) {
        char url[160] = "";
        if (pj_str(d, "VideoUrl", url, sizeof url) && url[0]) {
            p3d_lock();
            if (!strncmp(url, "http", 4)) p3d_copy(m->st.cam_url, sizeof m->st.cam_url, url);
            else if (!strncmp(url, "rtsp", 4)) m->st.cam_url[0] = '\0';
            else snprintf(m->st.cam_url, sizeof m->st.cam_url, "http://%.170s", url);
            p3d_unlock();
        }
    } else {
        int ack = 0;
        if (pj_int(d, "Ack", &ack) && ack != 0) p3d_con_add(m, "! ", _("La impresora no aceptó la orden"));
    }
    (void)e;
}

static void command(p3d_machine_t *m, elegoo_t *e, const p3d_cmd_t *c)
{
    char d[300], esc[220];
    switch (c->op) {
    case P3D_C_PAUSE: send_cmd(e, 129, NULL); break;
    case P3D_C_RESUME: send_cmd(e, 131, NULL); break;
    case P3D_C_CANCEL: send_cmd(e, 130, NULL); break;
    case P3D_C_START:
        p3d_json_esc(esc, sizeof esc, c->text);
        snprintf(d, sizeof d, "{\"Filename\":\"%s\",\"StartLayer\":0}", esc);
        send_cmd(e, 128, d);
        break;
    case P3D_C_DELETE:
        p3d_json_esc(esc, sizeof esc, c->text);
        snprintf(d, sizeof d, "{\"FileList\":[\"%s\"],\"FolderList\":[]}", esc);
        send_cmd(e, 259, d);
        send_cmd(e, 258, "{\"Url\":\"/local\"}");
        break;
    case P3D_C_FILES:
        p3d_files_begin(m);
        send_cmd(e, 258, "{\"Url\":\"/local\"}");
        break;
    case P3D_C_TEMP:
        snprintf(d, sizeof d, "{\"%s\":%d}", c->a == 2 ? "TempTargetHotbed" : c->a == 3 ? "TempTargetBox" : "TempTargetNozzle",
                 (int)roundf(c->v[0]));
        send_cmd(e, 403, d);
        break;
    case P3D_C_FAN:
        snprintf(d, sizeof d, "{\"TargetFanSpeed\":{\"ModelFan\":%d}}", (int)roundf(c->v[0]));
        send_cmd(e, 403, d);
        break;
    case P3D_C_SPEED:
        snprintf(d, sizeof d, "{\"PrintSpeedPct\":%d}", (int)roundf(c->v[0]));
        send_cmd(e, 403, d);
        break;
    case P3D_C_LIGHT:
        snprintf(d, sizeof d, "{\"LightStatus\":{\"SecondLight\":%d}}", c->a ? 1 : 0);
        send_cmd(e, 403, d);
        break;
    case P3D_C_HOME: {
        int a = c->a ? c->a : 7;
        snprintf(d, sizeof d, "{\"Axis\":\"%s%s%s\"}", a & 1 ? "X" : "", a & 2 ? "Y" : "", a & 4 ? "Z" : "");
        send_cmd(e, 402, d);
        break;
    }
    case P3D_C_JOG:
        for (int i = 0; i < 3; i++) {
            if (c->v[i] == 0) continue;
            snprintf(d, sizeof d, "{\"Axis\":\"%c\",\"Step\":%.2f}", "XYZ"[i], (double)c->v[i]);
            send_cmd(e, 401, d);
        }
        break;
    default:
        p3d_con_add(m, "! ", _("Elegoo no acepta G-code por la red"));
        break;
    }
}

static void run(p3d_machine_t *m)
{
    elegoo_t *e = p3d_psram(sizeof *e);
    if (!e) return;
    int fails = 0;
    char url[160];
    snprintf(url, sizeof url, "ws://%.96s:%d/websocket", m->cfg.host, p3d_cfg_port(&m->cfg));
    while (!m->stop) {
        memset(e, 0, sizeof *e);
        p3d_copy(e->board, sizeof e->board, m->cfg.serial);
        e->ws = aos_hal_ws_open(url, NULL, CONNECT_MS);
        if (!e->ws) {
            fails++;
            p3d_offline(m, _("No responde"));
            if (!p3d_wait(m, p3d_backoff(m, fails))) break;
            continue;
        }
        fails = 0;
        e->last_rx = aos_hal_uptime_ms();
        e->last_ping = e->last_rx;
        if (e->board[0]) {
            send_cmd(e, 1, NULL);
            send_cmd(e, 0, NULL);
        }
        e->last_ask = e->last_rx;
        while (!m->stop && aos_hal_ws_is_open(e->ws)) {
            p3d_cmd_t c;
            while (e->board[0] && p3d_take(m, &c)) command(m, e, &c);
            const char *msg;
            int n = aos_hal_ws_recv_text(e->ws, &msg, 200);
            if (n < 0) break;
            uint64_t now = aos_hal_uptime_ms();
            if (n > 0 && msg[0] == '{') {
                e->last_rx = now;
                if (!e->board[0]) {
                    if (!pj_str(msg, "MainboardID", e->board, sizeof e->board)) {
                        pj_str(msg, "Data.MainboardID", e->board, sizeof e->board);
                    }
                    if (e->board[0]) {
                        send_cmd(e, 1, NULL);
                        send_cmd(e, 0, NULL);
                    }
                }
                char topic[96] = "";
                pj_str(msg, "Topic", topic, sizeof topic);
                if (strstr(topic, "/status/")) status(m, msg);
                else if (strstr(topic, "/attributes/")) attributes(m, msg);
                else if (strstr(topic, "/response/")) response(m, e, msg);
                else if (pj_get(msg, "Status")) status(m, msg);
                if (!e->video && e->board[0] && m->cfg.camera[0] == '\0') {
                    e->video = true;
                    send_cmd(e, 386, "{\"Enable\":1}");
                }
            } else if (n > 0) {
                e->last_rx = now;                    /* "pong" */
            }
            if (!e->board[0] && now - e->last_ask > STATUS_MS) {
                p3d_offline(m, _("Falta el número de la placa (MainboardID): escribilo en Serie"));
                e->last_ask = now;
            }
            if (e->board[0] && now - e->last_ask >= (uint64_t)(m->poll_ms > STATUS_MS ? m->poll_ms : STATUS_MS)) {
                e->last_ask = now;
                send_cmd(e, 0, NULL);
            }
            if (now - e->last_ping > PING_MS) {
                e->last_ping = now;
                aos_hal_ws_send_text(e->ws, "ping", 4);
            }
            if (now - e->last_rx > SILENT_MS) break;
        }
        aos_hal_ws_close(e->ws);
        e->ws = NULL;
        if (!m->stop) {
            p3d_offline(m, _("Se cortó la conexión"));
            p3d_wait(m, 2000);
        }
    }
    free(e);
}

const p3d_driver_t p3d_drv_elegoo = {
    .word      = "elegoo",
    .label     = "Elegoo (SDCP)",
    .makers    = N_("Centauri Carbon y las de resina Saturn y Mars con Wi-Fi"),
    .key_label = NULL,
    .key_hint  = NULL,
    .port      = 3030,
    .tls_port  = 3030,
    .fields    = P3D_F_SERIAL,
    .run       = run,
};
