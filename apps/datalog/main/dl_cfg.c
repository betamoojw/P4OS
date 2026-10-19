/*
 * P4OS - Datalog: the lines to read, in <data>/datalog.txt.
 *
 * A text file so that the portal's page reads it as it is, and so that a
 * person can too:
 *
 *   # Datalog: one line per reading (apps/datalog/README.md)
 *   running=1
 *   csv=comma
 *   line id=horno name="Horno" every=10 kind=spi sensor=max6675 key=temp hi=80 hyst=1 sound=1 out=46 out_level=high
 *   line id=puerta name="Puerta" at=08:00,20:00 kind=gpio pin=33 pull=up invert=1
 *   line id=tanque name="Tanque" every=60 kind=gpio analog=1 sensor=ads1115 key=a0 mode=scale x1=0.5 y1=0 x2=4.5 y2=10 unit="bar"
 *
 * Numbers are written with a point whatever the language; a value with
 * spaces goes between double quotes (a quote inside one becomes an
 * apostrophe). An unknown key is skipped, so a newer app's file still loads.
 */
#include "dl.h"

#include <stdlib.h>

static const char *const KIND[DL_K_COUNT] = { "gpio", "i2c", "spi", "ow", "pin", "uart", "sys" };
static const char *const PULL[3] = { "none", "up", "down" };
static const char *const MODE[3] = { "volt", "raw", "scale" };
static const char *const SYS[DL_SYS_COUNT] = { "rssi", "psram", "ram", "uptime" };

static int pick(const char *v, const char *const *names, int n, int def)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(v, names[i])) return i;
    return def;
}

void dl_cfg_path(char *out, size_t n)
{
    snprintf(out, n, "%.200s/datalog.txt", aos_hal_path_data());
}

void dl_cfg_defaults(dl_ch_t *c)
{
    memset(c, 0, sizeof *c);
    c->every_s = 60;
    c->kind = DL_K_I2C;
    c->pin = -1;
    c->pull = DL_PULL_UP;
    c->out_pin = -1;
    c->out_high = true;
    c->sound = true;
    c->x1 = 0.0f;
    c->y1 = 0.0f;
    c->x2 = 3.3f;
    c->y2 = 100.0f;
    dl_scpy(c->unit, sizeof c->unit, "%");
}

/* A name the card takes: a-z 0-9 and _, the accents of Spanish taken off */
void dl_cfg_slug(const char *name, char *out, size_t n)
{
    size_t o = 0;
    bool sep = false;
    for (const unsigned char *s = (const unsigned char *)name; *s && o + 1 < n; s++) {
        unsigned char c = *s;
        if (c == 0xC3 && s[1]) {
            /* á é í ó ú ñ ü and their capitals, in UTF-8 */
            static const char MAP[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
            unsigned char d = s[1];
            s++;
            if (d >= 0x80 && d < 0xA0) d += 0x20;
            c = d >= 0xA0 && d < 0xC0 ? (unsigned char)MAP[d - 0xA0] : '_';
            if (c == '/') c = '_';
        } else if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        } else if (c >= 0x80) {
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (sep && o) out[o++] = '_';
            sep = false;
            if (o + 1 < n) out[o++] = (char)c;
        } else {
            sep = true;
        }
    }
    out[o] = 0;
    if (!o) dl_scpy(out, n, "lectura");
}

void dl_sched_text(const dl_ch_t *c, char *out, size_t n)
{
    if (c->sched_at) {
        size_t o = (size_t)snprintf(out, n, "%s", _("a las"));
        for (int i = 0; i < c->n_at && o < n; i++)
            o += (size_t)snprintf(out + o, n - o, "%s%02u:%02u", i ? " · " : " ", c->at[i] / 60u, c->at[i] % 60u);
        if (!c->n_at) dl_scpy(out, n, _("sin horario"));
        return;
    }
    uint32_t s = c->every_s;
    if (s % 3600 == 0) snprintf(out, n, _("cada %u h"), (unsigned)(s / 3600));
    else if (s % 60 == 0) snprintf(out, n, _("cada %u min"), (unsigned)(s / 60));
    else snprintf(out, n, _("cada %u s"), (unsigned)s);
}

/* ---- reading ---- */

/* The next "key=value" of a line; value may be "quoted". false at the end. */
static bool next_kv(char **p, char **k, char **v)
{
    char *s = *p;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') return false;
        *k = s;
        while (*s && *s != '=' && *s != ' ' && *s != '\t') s++;
        if (*s == '=') break;
        /* a word without a value: skipped */
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    *s++ = 0;
    if (*s == '"') {
        *v = ++s;
        while (*s && *s != '"') s++;
    } else {
        *v = s;
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    if (*s) *s++ = 0;
    *p = s;
    return true;
}

static void parse_line(dl_ch_t *c, char *p)
{
    char *k, *v;
    while (next_kv(&p, &k, &v)) {
        if (!strcmp(k, "id")) dl_scpy(c->id, sizeof c->id, v);
        else if (!strcmp(k, "name")) dl_scpy(c->name, sizeof c->name, v);
        else if (!strcmp(k, "every")) { c->sched_at = false; c->every_s = (uint32_t)strtol(v, NULL, 10); }
        else if (!strcmp(k, "at")) {
            c->sched_at = true;
            c->n_at = 0;
            for (char *t = v; *t && c->n_at < DL_MAX_AT;) {
                int h = (int)strtol(t, &t, 10), m = 0;
                if (*t == ':') m = (int)strtol(t + 1, &t, 10);
                if (h >= 0 && h < 24 && m >= 0 && m < 60) c->at[c->n_at++] = (uint16_t)(h * 60 + m);
                while (*t && *t != ',') t++;
                if (*t == ',') t++;
            }
        }
        else if (!strcmp(k, "kind")) c->kind = (uint8_t)pick(v, KIND, DL_K_COUNT, DL_K_I2C);
        else if (!strcmp(k, "analog")) c->analog = atoi(v) != 0;
        else if (!strcmp(k, "pin")) c->pin = (int8_t)atoi(v);
        else if (!strcmp(k, "pull")) c->pull = (uint8_t)pick(v, PULL, 3, DL_PULL_NONE);
        else if (!strcmp(k, "invert")) c->invert = atoi(v) != 0;
        else if (!strcmp(k, "sensor")) dl_scpy(c->sensor, sizeof c->sensor, v);
        else if (!strcmp(k, "key")) dl_scpy(c->key, sizeof c->key, v);
        else if (!strcmp(k, "mode")) c->an_mode = (uint8_t)pick(v, MODE, 3, DL_AN_VOLT);
        else if (!strcmp(k, "x1")) c->x1 = strtof(v, NULL);
        else if (!strcmp(k, "y1")) c->y1 = strtof(v, NULL);
        else if (!strcmp(k, "x2")) c->x2 = strtof(v, NULL);
        else if (!strcmp(k, "y2")) c->y2 = strtof(v, NULL);
        else if (!strcmp(k, "unit")) dl_scpy(c->unit, sizeof c->unit, v);
        else if (!strcmp(k, "sys")) c->sys = (uint8_t)pick(v, SYS, DL_SYS_COUNT, DL_SYS_RSSI);
        else if (!strcmp(k, "lo")) { c->lo_on = true; c->lo = strtof(v, NULL); }
        else if (!strcmp(k, "hi")) { c->hi_on = true; c->hi = strtof(v, NULL); }
        else if (!strcmp(k, "hyst")) c->hyst = strtof(v, NULL);
        else if (!strcmp(k, "sound")) c->sound = atoi(v) != 0;
        else if (!strcmp(k, "out")) c->out_pin = (int8_t)atoi(v);
        else if (!strcmp(k, "out_level")) c->out_high = strcmp(v, "low") != 0;
    }
}

bool dl_cfg_load(dl_cfg_t *cfg)
{
    char path[256];
    dl_cfg_path(path, sizeof path);
    memset(cfg, 0, sizeof *cfg);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "running=", 8)) cfg->running = atoi(p + 8) != 0;
        else if (!strncmp(p, "csv=", 4)) cfg->semicolon = !strcmp(p + 4, "semicolon");
        else if (!strncmp(p, "line ", 5) && cfg->n < DL_MAX_CH) {
            dl_ch_t *c = &cfg->ch[cfg->n];
            dl_cfg_defaults(c);
            c->unit[0] = 0;
            c->sound = false;
            parse_line(c, p + 5);
            if (!c->id[0]) dl_cfg_slug(c->name, c->id, sizeof c->id);
            if (!c->name[0]) dl_scpy(c->name, sizeof c->name, c->id);
            if (!c->every_s) c->every_s = 1;
            /* the same id twice would write one file from two lines */
            bool dup = false;
            for (int i = 0; i < cfg->n; i++) dup |= !strcmp(cfg->ch[i].id, c->id);
            if (!dup) cfg->n++;
        }
    }
    fclose(f);
    return true;
}

/* ---- writing ---- */

/* "value" with the quotes a name with spaces needs, and none inside */
static void put_str(FILE *f, const char *k, const char *v)
{
    char t[64];
    size_t o = 0;
    for (; *v && o + 1 < sizeof t; v++) t[o++] = *v == '"' ? '\'' : *v;
    t[o] = 0;
    fputs(" ", f);
    fputs(k, f);
    fputs(strchr(t, ' ') || !t[0] ? "=\"" : "=", f);
    fputs(t, f);
    if (strchr(t, ' ') || !t[0]) fputs("\"", f);
}

static void put_num(FILE *f, const char *k, float v)
{
    char t[40];
    snprintf(t, sizeof t, " %s=%g", k, (double)v);
    fputs(t, f);
}

static void put_int(FILE *f, const char *k, int v)
{
    char t[40];
    snprintf(t, sizeof t, " %s=%d", k, v);
    fputs(t, f);
}

bool dl_cfg_save(const dl_cfg_t *cfg)
{
    char path[256], tmp[264];
    dl_cfg_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs("# Datalog: one line per reading (apps/datalog/README.md)\n", f);
    fputs(cfg->running ? "running=1\n" : "running=0\n", f);
    fputs(cfg->semicolon ? "csv=semicolon\n" : "csv=comma\n", f);
    for (int i = 0; i < cfg->n; i++) {
        const dl_ch_t *c = &cfg->ch[i];
        fputs("line", f);
        put_str(f, "id", c->id);
        put_str(f, "name", c->name);
        if (c->sched_at) {
            char t[8 * DL_MAX_AT + 8];
            size_t o = 0;
            for (int k = 0; k < c->n_at; k++)
                o += (size_t)snprintf(t + o, sizeof t - o, "%s%02u:%02u", k ? "," : "", c->at[k] / 60u, c->at[k] % 60u);
            t[o] = 0;
            fputs(" at=", f);
            fputs(t, f);
        } else {
            put_int(f, "every", (int)c->every_s);
        }
        fputs(" kind=", f);
        fputs(KIND[c->kind < DL_K_COUNT ? c->kind : DL_K_I2C], f);
        if (c->kind == DL_K_GPIO && !c->analog) {
            put_int(f, "pin", c->pin);
            fputs(" pull=", f);
            fputs(PULL[c->pull < 3 ? c->pull : 0], f);
            if (c->invert) fputs(" invert=1", f);
        } else if (c->kind == DL_K_SYS) {
            fputs(" sys=", f);
            fputs(SYS[c->sys < DL_SYS_COUNT ? c->sys : 0], f);
        } else {
            if (c->kind == DL_K_GPIO) fputs(" analog=1", f);
            put_str(f, "sensor", c->sensor);
            put_str(f, "key", c->key);
            if (c->kind == DL_K_GPIO) {
                fputs(" mode=", f);
                fputs(MODE[c->an_mode < 3 ? c->an_mode : 0], f);
                if (c->an_mode == DL_AN_SCALE) {
                    put_num(f, "x1", c->x1);
                    put_num(f, "y1", c->y1);
                    put_num(f, "x2", c->x2);
                    put_num(f, "y2", c->y2);
                    put_str(f, "unit", c->unit);
                }
            }
        }
        if (c->lo_on) put_num(f, "lo", c->lo);
        if (c->hi_on) put_num(f, "hi", c->hi);
        if ((c->lo_on || c->hi_on) && c->hyst > 0) put_num(f, "hyst", c->hyst);
        if (c->lo_on || c->hi_on) {
            put_int(f, "sound", c->sound ? 1 : 0);
            if (c->out_pin >= 0) {
                put_int(f, "out", c->out_pin);
                fputs(c->out_high ? " out_level=high" : " out_level=low", f);
            }
        }
        fputs("\n", f);
    }
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    return ok;
}
