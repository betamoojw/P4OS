/*
 * P4OS - 3D printers: impresoras.txt, the saved machines.
 *
 * A text file in the app's data folder (/data/impresoras.txt for the
 * portal), which the app and its page both read and write. One block per
 * machine:
 *
 *     [Prusa 1]
 *     type   = octoprint      octoprint prusalink bambu moonraker duet
 *                             elegoo flashforge creality
 *     host   = 192.168.0.50
 *     port   = 80             optional: each type has its own
 *     tls    = 0              1: https / wss
 *     key    = ...            API key, access code, password, check code
 *     user   = maker          PrusaLink's user (Digest)
 *     serial = ...            Bambu and FlashForge serial, Elegoo mainboard
 *     camera = off            "": what the machine says; off; or a URL
 *
 * Only type and host are needed. The keys are kept in the clear: the app
 * and the page both say so where they are typed.
 */
#include "p3d.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define FILE_NAME "impresoras.txt"
#define MAX_LINE  320

static char s_path[160];

const char *p3d_cfg_path(void)
{
    const char *env = getenv("P3D_CONFIG");     /* the simulator's tests */
    if (env && env[0]) snprintf(s_path, sizeof s_path, "%s", env);
    else snprintf(s_path, sizeof s_path, "%s/%s", aos_hal_path_data(), FILE_NAME);
    return s_path;
}

uint32_t p3d_cfg_stamp(void)
{
    struct stat st;
    if (stat(p3d_cfg_path(), &st) != 0) return 0;
    return (uint32_t)st.st_size * 2654435761u ^ (uint32_t)st.st_mtime;
}

void p3d_cfg_defaults(p3d_cfg_t *m, int kind)
{
    memset(m, 0, sizeof *m);
    m->kind = kind >= 0 && kind < P3D_KIND_N ? kind : P3D_OCTOPRINT;
    if (m->kind == P3D_PRUSALINK) p3d_copy(m->user, sizeof m->user, "maker");
}

int p3d_cfg_port(const p3d_cfg_t *m)
{
    if (m->port > 0) return m->port;
    const p3d_driver_t *d = p3d_driver(m->kind);
    return m->tls ? d->tls_port : d->port;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* "host:port" written in the host field */
static void split_host(p3d_cfg_t *m)
{
    char *h = m->host;
    if (!strncmp(h, "http://", 7)) memmove(h, h + 7, strlen(h + 7) + 1);
    else if (!strncmp(h, "https://", 8)) {
        memmove(h, h + 8, strlen(h + 8) + 1);
        m->tls = true;
    }
    char *slash = strchr(h, '/');
    if (slash) *slash = '\0';
    char *c = strchr(h, ':');
    if (c && !strchr(c + 1, ':')) {
        int p = atoi(c + 1);
        if (p > 0 && p < 65536) m->port = p;
        *c = '\0';
    }
}

void p3d_cfg_load(p3d_cfgs_t *c)
{
    memset(c, 0, sizeof *c);
    FILE *f = fopen(p3d_cfg_path(), "r");
    if (!f) return;
    char line[MAX_LINE];
    p3d_cfg_t *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!s[0] || s[0] == '#' || s[0] == ';') continue;
        if (s[0] == '[') {
            char *e = strchr(s, ']');
            if (e) *e = '\0';
            cur = NULL;
            if (c->count < P3D_MAX) {
                cur = &c->m[c->count++];
                p3d_cfg_defaults(cur, P3D_OCTOPRINT);
                cur->user[0] = '\0';
                p3d_copy(cur->name, sizeof cur->name, trim(s + 1));
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !cur) continue;
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        if (!strcmp(key, "type")) {
            int k = p3d_kind_from_id(val);
            if (k >= 0) cur->kind = k;
        } else if (!strcmp(key, "host")) {
            p3d_copy(cur->host, sizeof cur->host, val);
        } else if (!strcmp(key, "port")) {
            int p = atoi(val);
            if (p > 0 && p < 65536) cur->port = p;
        } else if (!strcmp(key, "tls")) {
            cur->tls = atoi(val) != 0;
        } else if (!strcmp(key, "key")) {
            p3d_copy(cur->key, sizeof cur->key, val);
        } else if (!strcmp(key, "user")) {
            p3d_copy(cur->user, sizeof cur->user, val);
        } else if (!strcmp(key, "serial")) {
            p3d_copy(cur->serial, sizeof cur->serial, val);
        } else if (!strcmp(key, "camera")) {
            p3d_copy(cur->camera, sizeof cur->camera, val);
        }
    }
    fclose(f);
    int n = 0;
    for (int i = 0; i < c->count; i++) {
        p3d_cfg_t *m = &c->m[i];
        if (!m->host[0]) continue;
        split_host(m);
        if (m->kind == P3D_PRUSALINK && !m->user[0]) p3d_copy(m->user, sizeof m->user, "maker");
        if (!m->name[0]) p3d_copy(m->name, sizeof m->name, m->host);
        if (n != i) c->m[n] = *m;
        n++;
    }
    c->count = n;
}

bool p3d_cfg_save(const p3d_cfgs_t *c)
{
    const char *path = p3d_cfg_path();
    mkdir(aos_hal_path_data(), 0777);
    char tmp[176];
    snprintf(tmp, sizeof tmp, "%.160s.part", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs("# 3D printers for P4OS (apps/impresoras). One block each:\n"
          "# [Name]  type (octoprint prusalink bambu moonraker duet elegoo flashforge creality),\n"
          "# host, port, tls (1: https), key (API key, access code, password or check code),\n"
          "# user (PrusaLink), serial (Bambu, FlashForge, Elegoo), camera (off or a URL).\n"
          "# The keys are kept here in the clear.\n", f);
    char b[400];
    for (int i = 0; i < c->count; i++) {
        const p3d_cfg_t *m = &c->m[i];
        snprintf(b, sizeof b, "\n[%s]\ntype = %s\nhost = %s\n", m->name, p3d_driver(m->kind)->word, m->host);
        fputs(b, f);
        if (m->port > 0) {
            snprintf(b, sizeof b, "port = %d\n", m->port);
            fputs(b, f);
        }
        if (m->tls) fputs("tls = 1\n", f);
        if (m->key[0]) {
            snprintf(b, sizeof b, "key = %s\n", m->key);
            fputs(b, f);
        }
        if (m->user[0] && !(m->kind == P3D_PRUSALINK && !strcmp(m->user, "maker"))) {
            snprintf(b, sizeof b, "user = %s\n", m->user);
            fputs(b, f);
        }
        if (m->serial[0]) {
            snprintf(b, sizeof b, "serial = %s\n", m->serial);
            fputs(b, f);
        }
        if (m->camera[0]) {
            snprintf(b, sizeof b, "camera = %s\n", m->camera);
            fputs(b, f);
        }
    }
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    return ok;
}
