/*
 * P4OS - 3D printers: Bambu Lab in LAN mode (X1, X1E, P1P, P1S, A1, A1 mini,
 * H2D).
 *
 * The printer is an MQTT broker of its own on port 8883 (TLS): user "bblp",
 * the password is the access code the printer shows (Settings, Network /
 * WLAN, "LAN only" or "Developer mode"), and the topics are named after the
 * serial number:
 *
 *   device/<serial>/report    what the printer says; an X1 sends the whole
 *                             status, a P1 or A1 only what changed, so every
 *                             field is taken only when it comes
 *   device/<serial>/request   what it is told: {"pushing":{"command":
 *                             "pushall"}} for everything at once, and
 *                             {"print":{"command":"pause"|"resume"|"stop"|
 *                             "print_speed"|"gcode_line", ...}}
 *
 * Since the firmware of early 2025 a printer only takes commands on the LAN
 * when "Developer mode" is on; without it, it still reports.
 *
 * Its certificate is signed by Bambu's own authority and names the serial,
 * not the address: the connection is aos_tls_open_trust() against Bambu's
 * authorities (p3d_trust.c) and the serial, still refusing anything else.
 */
#include "p3d.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CONNECT_MS  8000
#define PUSHALL_MS  60000           /* everything again now and then: a P1 only sends changes */
#define SILENT_MS   30000           /* nothing in this long: the link is dead */

typedef struct {
    p3d_mqtt_t *q;
    char        req[80];
    uint32_t    seq;
    uint64_t    last_rx, last_all;
} bambu_t;

static int state_of(const char *s, int err)
{
    if (!strcmp(s, "RUNNING")) return P3D_ST_PRINTING;
    if (!strcmp(s, "PAUSE")) return P3D_ST_PAUSED;
    if (!strcmp(s, "FINISH")) return P3D_ST_DONE;
    if (!strcmp(s, "FAILED")) return err == 0x0300400C ? P3D_ST_CANCELLED : P3D_ST_ERROR;
    if (!strcmp(s, "PREPARE") || !strcmp(s, "SLICING") || !strcmp(s, "INIT")) return P3D_ST_BUSY;
    return P3D_ST_IDLE;
}

static void temp(const char *pr, const char *cur, const char *tgt, p3d_temp_t *t)
{
    float v;
    if (pj_num(pr, cur, &v)) {
        t->on = true;
        t->cur = v;
    }
    if (pj_num(pr, tgt, &v)) t->target = v;
}

static uint32_t hex_rgb(const char *s)
{
    uint32_t v = (uint32_t)strtoul(s, NULL, 16);
    return strlen(s) >= 8 ? v >> 8 : v;
}

static void report(p3d_machine_t *m, const char *js)
{
    const char *pr = pj_get(js, "print");
    if (!pr) return;
    p3d_lock();
    p3d_status_t *s = &m->st;
    s->caps = P3D_CAP_PAUSE | P3D_CAP_CANCEL | P3D_CAP_TEMP | P3D_CAP_JOG | P3D_CAP_HOME | P3D_CAP_EXTRUDE |
              P3D_CAP_FAN | P3D_CAP_SPEED_LVL | P3D_CAP_GCODE | P3D_CAP_LIGHT | P3D_CAP_MOTORS_OFF;
    temp(pr, "nozzle_temper", "nozzle_target_temper", &s->tool[0]);
    temp(pr, "bed_temper", "bed_target_temper", &s->bed);
    float v;
    if (pj_num(pr, "chamber_temper", &v) && v > 0) {
        s->chamber.on = true;
        s->chamber.cur = v;
    }
    int n, err = 0;
    pj_int(pr, "print_error", &err);
    char gs[16];
    if (pj_str(pr, "gcode_state", gs, sizeof gs)) {
        p3d_copy(s->raw, sizeof s->raw, gs);
        s->state = state_of(gs, err);
    } else if (s->state <= P3D_ST_CONNECTING) {
        s->state = P3D_ST_IDLE;
    }
    if (pj_get(pr, "print_error")) {
        if (err && s->state != P3D_ST_CANCELLED) snprintf(s->msg, sizeof s->msg, _("Error %04X-%04X"), (unsigned)(err >> 16), (unsigned)(err & 0xFFFF));
        else s->msg[0] = '\0';
    }
    if (pj_int(pr, "mc_percent", &n)) s->progress = (float)n;
    if (pj_int(pr, "mc_remaining_time", &n)) s->remaining_s = n * 60;
    if (pj_int(pr, "layer_num", &n)) s->layer = n;
    if (pj_int(pr, "total_layer_num", &n)) s->layers = n;
    char name[128];
    if (pj_str(pr, "subtask_name", name, sizeof name)) p3d_copy(s->file, sizeof s->file, name);
    if (pj_int(pr, "spd_lvl", &n)) s->speed_lvl = n;
    if (pj_int(pr, "spd_mag", &n)) s->speed = n;
    if (pj_num(pr, "cooling_fan_speed", &v)) s->fan = (int)roundf(v * 100 / 15);
    const char *lr = pj_get(pr, "lights_report");
    for (int i = 0; lr && i < 4; i++) {
        char p[24], node[32] = "", mode[12] = "";
        snprintf(p, sizeof p, "%d.node", i);
        if (!pj_str(lr, p, node, sizeof node)) break;
        snprintf(p, sizeof p, "%d.mode", i);
        pj_str(lr, p, mode, sizeof mode);
        if (!strcmp(node, "chamber_light")) s->light = strcmp(mode, "off") ? 1 : 0;
    }
    /* the AMS: up to two units of four here */
    const char *ams = pj_get(pr, "ams.ams");
    if (ams) {
        int k = 0;
        for (int u = 0; u < 2; u++) {
            char p[32];
            snprintf(p, sizeof p, "%d.tray", u);
            const char *tr = pj_get(ams, p);
            if (!tr) break;
            for (int i = 0; i < 4 && k < P3D_TRAYS; i++, k++) {
                p3d_tray_t *t = &s->tray[k];
                char col[12] = "";
                snprintf(p, sizeof p, "%d.tray_type", i);
                t->type[0] = '\0';
                pj_str(tr, p, t->type, sizeof t->type);
                t->on = t->type[0] != '\0';
                snprintf(p, sizeof p, "%d.tray_color", i);
                t->rgb = pj_str(tr, p, col, sizeof col) ? hex_rgb(col) : 0x808080;
                snprintf(p, sizeof p, "%d.remain", i);
                t->remain = pj_int(tr, p, &n) && n >= 0 && n <= 100 ? (int8_t)n : -1;
            }
        }
        s->ntrays = k;
    }
    char now[8];
    if (pj_str(pr, "ams.tray_now", now, sizeof now)) {
        n = atoi(now);
        s->tray_now = n < P3D_TRAYS ? n : -1;
    }
    if (s->state != P3D_ST_PRINTING && s->state != P3D_ST_PAUSED && s->state != P3D_ST_BUSY) {
        if (s->state != P3D_ST_DONE) s->remaining_s = -1;
    }
    p3d_copy(s->cam_url, sizeof s->cam_url, "bambu:");
    p3d_publish(m);
    p3d_unlock();
}

static void version(p3d_machine_t *m, const char *js)
{
    const char *mods = pj_get(js, "info.module");
    for (int i = 0; mods && i < 12; i++) {
        char p[24], name[24] = "", ver[24] = "", prod[40] = "";
        snprintf(p, sizeof p, "%d.name", i);
        if (!pj_str(mods, p, name, sizeof name)) break;
        if (strcmp(name, "ota")) continue;
        snprintf(p, sizeof p, "%d.sw_ver", i);
        pj_str(mods, p, ver, sizeof ver);
        snprintf(p, sizeof p, "%d.product_name", i);
        pj_str(mods, p, prod, sizeof prod);
        p3d_lock();
        snprintf(m->st.fw, sizeof m->st.fw, "Bambu %.30s", ver);
        if (prod[0]) p3d_copy(m->st.model, sizeof m->st.model, prod);
        p3d_unlock();
    }
}

static bool request(bambu_t *b, const char *json)
{
    return p3d_mqtt_pub(b->q, b->req, json, -1);
}

static void print_cmd(bambu_t *b, const char *cmd, const char *param)
{
    char j[512], esc[400];
    p3d_json_esc(esc, sizeof esc, param);
    snprintf(j, sizeof j, "{\"print\":{\"sequence_id\":\"%u\",\"command\":\"%s\",\"param\":\"%s\"}}", (unsigned)++b->seq,
             cmd, esc);
    request(b, j);
}

static void command(p3d_machine_t *m, bambu_t *b, const p3d_cmd_t *c)
{
    char g[260], j[300];
    switch (c->op) {
    case P3D_C_PAUSE: print_cmd(b, "pause", ""); break;
    case P3D_C_RESUME: print_cmd(b, "resume", ""); break;
    case P3D_C_CANCEL: print_cmd(b, "stop", ""); break;
    case P3D_C_SPEED_LVL: {
        char lv[4];
        snprintf(lv, sizeof lv, "%d", c->a < 1 ? 1 : c->a > 4 ? 4 : c->a);
        print_cmd(b, "print_speed", lv);
        break;
    }
    case P3D_C_LIGHT:
        snprintf(j, sizeof j,
                 "{\"system\":{\"sequence_id\":\"%u\",\"command\":\"ledctrl\",\"led_node\":\"chamber_light\","
                 "\"led_mode\":\"%s\",\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0}}",
                 (unsigned)++b->seq, c->a ? "on" : "off");
        request(b, j);
        break;
    case P3D_C_FAN: {
        /* P1 is the part fan */
        int s = (int)roundf(c->v[0] * 255 / 100);
        snprintf(g, sizeof g, "M106 P1 S%d\n", s < 0 ? 0 : s > 255 ? 255 : s);
        print_cmd(b, "gcode_line", g);
        break;
    }
    default:
        if (p3d_gcode_of(c, g, sizeof g - 2) > 0) {
            strcat(g, "\n");
            print_cmd(b, "gcode_line", g);
            if (c->op == P3D_C_GCODE) p3d_con_add(m, "> ", c->text);
        } else {
            p3d_con_add(m, "! ", _("Bambu no acepta eso por la red"));
        }
        return;
    }
}

static void run(p3d_machine_t *m)
{
    bambu_t *b = p3d_psram(sizeof *b);
    if (!b) return;
    memset(b, 0, sizeof *b);
    snprintf(b->req, sizeof b->req, "device/%.48s/request", m->cfg.serial);
    char rep[80], client[32];
    snprintf(rep, sizeof rep, "device/%.48s/report", m->cfg.serial);
    snprintf(client, sizeof client, "p4os_%08x", (unsigned)(aos_hal_uptime_ms() * 2654435761u));
    int fails = 0;
    while (!m->stop) {
        if (!m->cfg.serial[0] || !m->cfg.key[0]) {
            p3d_offline(m, _("Faltan el número de serie y el código de acceso"));
            if (!p3d_wait(m, 30000)) break;
            continue;
        }
        char why[128] = "";
        p3d_trust_t trust;
        b->q = p3d_bambu_trust(m->cfg.serial, &trust)
                   ? p3d_mqtt_open(m->cfg.host, p3d_cfg_port(&m->cfg), true, &trust, client, "bblp", m->cfg.key,
                                   CONNECT_MS, why, sizeof why)
                   : NULL;
        if (!b->q) {
            fails++;
            if (strstr(why, "is not for")) {
                p3d_offline(m, _("El certificado de la impresora no es de ese número de serie"));
            } else if (strstr(why, "authority")) {
                p3d_offline(m, _("La impresora no tiene un certificado de Bambu"));
            } else if (strstr(why, "no valid time")) {
                p3d_offline(m, _("Esperando la hora de la red para verificar el certificado"));
            } else if (strstr(why, "authorized")) {
                p3d_offline(m, _("Código de acceso incorrecto"));
            } else {
                p3d_offline(m, _("No responde"));
            }
            aos_hal_log(P3D_TAG, "%s: %s", m->cfg.name, why);
            if (!p3d_wait(m, p3d_backoff(m, fails))) break;
            continue;
        }
        fails = 0;
        p3d_mqtt_sub(b->q, rep);
        request(b, "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}");
        request(b, "{\"info\":{\"sequence_id\":\"0\",\"command\":\"get_version\"}}");
        b->last_rx = b->last_all = aos_hal_uptime_ms();
        while (!m->stop) {
            p3d_cmd_t c;
            while (p3d_take(m, &c)) command(m, b, &c);
            const char *topic, *payload;
            int n = p3d_mqtt_poll(b->q, &topic, &payload, 200);
            if (n < 0) break;
            uint64_t now = aos_hal_uptime_ms();
            if (n > 0) {
                b->last_rx = now;
                if (pj_get(payload, "print")) report(m, payload);
                else if (pj_get(payload, "info")) version(m, payload);
            } else if (now - b->last_rx > SILENT_MS) {
                break;
            }
            if (now - b->last_all > PUSHALL_MS) {
                b->last_all = now;
                request(b, "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}");
            }
        }
        p3d_mqtt_close(b->q);
        b->q = NULL;
        if (!m->stop) {
            p3d_offline(m, _("Se cortó la conexión"));
            p3d_wait(m, 2000);
        }
    }
    free(b);
}

const p3d_driver_t p3d_drv_bambu = {
    .word      = "bambu",
    .label     = "Bambu Lab",
    .makers    = N_("X1, X1E, P1P, P1S, A1, A1 mini y H2D, en modo LAN (con el modo desarrollador para manejarlas)"),
    .key_label = N_("Código de acceso"),
    .key_hint  = N_("En la impresora: Ajustes, Red (WLAN): el código de acceso de 8 caracteres"),
    .port      = 8883,
    .tls_port  = 8883,
    .fields    = P3D_F_KEY | P3D_F_SERIAL | P3D_F_KEY_NEEDED | P3D_F_SERIAL_NEEDED,
    .run       = run,
};
