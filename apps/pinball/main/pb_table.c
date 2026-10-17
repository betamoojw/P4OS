/*
 * PINBALL - the table's definition (see pb_table.h)
 */
#include "pb_table.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOK 140
#define PB_PI   3.14159265f

static const char *const TYPES[OT_N] = {
    "wall", "post", "bumper", "sling", "drop", "target", "rollover", "spinner", "saucer", "ramp", "gate", "kickback",
    "flipper",
};
static const char *const ROLES[RL_N] = {
    "none", "lane", "inlane", "outlane", "bumper", "sling", "drop", "target", "spinner", "saucer", "ramp", "orbit",
    "kickback", "post",
};
static const char *const GOALS[GL_N] = {
    "bumper", "sling", "drop", "target", "spinner", "ramp", "orbit", "lane", "saucer", "any",
};
static const char *const LANGS[PB_LANGS] = { "es", "en", "de" };

int pb_lang(void)
{
    const char *c = aos_i18n_current();
    if (!c || !c[0] || !strncmp(c, "es", 2)) return 0;
    if (!strncmp(c, "de", 2)) return 2;
    return 1;
}

static int lookup(const char *const *tab, int n, const char *w)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(tab[i], w)) return i;
    }
    return -1;
}

/* splits a line in place: words, and "strings" without their quotes */
static int split(char *s, char **tok, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (!*s) break;
        if (*s == '"') {
            s++;
            tok[n++] = s;
            while (*s && *s != '"') s++;
        } else {
            tok[n++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '\r') s++;
        }
        if (*s) *s++ = 0;
    }
    return n;
}

static void copy(char *dst, const char *src, int n)
{
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static float F(const char *s) { return strtof(s, NULL); }
static int I(const char *s) { return (int)strtol(s, NULL, 0); }
static uint32_t HEX(const char *s) { return (uint32_t)strtol(s, NULL, 16); }

static void seg_finish(pb_seg_t *s)
{
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    s->len = sqrtf(dx * dx + dy * dy);
    if (s->len < 1e-3f) s->len = 1e-3f;
    s->dx = dx / s->len;
    s->dy = dy / s->len;
}

bool pb_table_parse(pb_table_t *t, const char *text, bool only_head)
{
    memset(t, 0, sizeof *t);
    t->grav = 1500.0f;
    t->ball_r = 13.5f;
    size_t len = strlen(text);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return false;
    memcpy(buf, text, len + 1);
    char *tok[MAX_TOK];
    char banks[8][16];
    int nbanks = 0;
    bool ok = true;
    int line = 0;
    for (char *p = buf; p && *p; line++) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        int n = split(p, tok, MAX_TOK);
        p = nl ? nl + 1 : NULL;
        if (n == 0 || tok[0][0] == '#') continue;
        const char *k = tok[0];
        if (!strcmp(k, "pinball")) {
            if (n < 2 || I(tok[1]) != 1) {
                ok = false;
                break;
            }
        } else if (!strcmp(k, "id") && n >= 2) {
            copy(t->id, tok[1], sizeof t->id);
        } else if (!strcmp(k, "order") && n >= 2) {
            t->order = I(tok[1]);
        } else if ((!strcmp(k, "name") || !strcmp(k, "desc")) && n >= 3) {
            int l = lookup(LANGS, PB_LANGS, tok[1]);
            if (l >= 0) {
                if (k[0] == 'n') copy(t->name[l], tok[2], PB_TXT);
                else copy(t->desc[l], tok[2], sizeof t->desc[l]);
            }
        } else if (!strcmp(k, "theme") && n >= 6) {
            t->c_dmd = HEX(tok[1]);
            t->c_bg = HEX(tok[2]);
            t->c_accent = HEX(tok[3]);
            t->c_panel = HEX(tok[4]);
            t->c_text = HEX(tok[5]);
        } else if (only_head) {
            continue;
        } else if (!strcmp(k, "size") && n >= 3) {
            t->w = F(tok[1]);
            t->h = F(tok[2]);
        } else if (!strcmp(k, "grav") && n >= 2) {
            t->grav = F(tok[1]);
        } else if (!strcmp(k, "ball") && n >= 2) {
            t->ball_r = F(tok[1]);
        } else if (!strcmp(k, "cam") && n >= 8) {
            pb_cam_t *c = &t->cam;
            c->f = F(tok[1]);
            c->cx = F(tok[2]);
            c->cy = F(tok[3]);
            c->px = F(tok[4]);
            c->py = F(tok[5]);
            c->pz = F(tok[6]);
            c->pitch = F(tok[7]);
            c->sp = sinf(c->pitch);
            c->cp = cosf(c->pitch);
        } else if (!strcmp(k, "plunger") && n >= 3) {
            t->plunger_x = F(tok[1]);
            t->plunger_y = F(tok[2]);
        } else if (!strcmp(k, "obj") && n >= 5) {
            int i = I(tok[1]);
            if (i != t->nobj || i >= PB_MAX_OBJ) {
                ok = false;
                break;
            }
            pb_obj_t *o = &t->obj[t->nobj++];
            int ty = lookup(TYPES, OT_N, tok[2]);
            int rl = lookup(ROLES, RL_N, tok[4]);
            o->type = (uint8_t)(ty < 0 ? OT_WALL : ty);
            o->role = (uint8_t)(rl < 0 ? RL_NONE : rl);
            copy(o->name, tok[3], sizeof o->name);
            o->lamp = -1;
            o->bank = 0;
            if (n >= 6) o->a = F(tok[5]);
            if (n >= 7) o->b = F(tok[6]);
            if (o->type == OT_DROP && n >= 6) {
                /* drop targets: banks by name */
                int b = 0;
                while (b < nbanks && strcmp(banks[b], tok[5])) b++;
                if (b == nbanks && nbanks < 8) copy(banks[nbanks++], tok[5], 16);
                o->bank = (int8_t)(b < 8 ? b : 7);
                o->a = 0;
            }
            o->index = (int8_t)t->role_n[o->role]++;
        } else if (!strcmp(k, "seg") && n >= 7) {
            if (t->nseg >= PB_MAX_SEG) continue;
            pb_seg_t *s = &t->seg[t->nseg++];
            s->x0 = F(tok[1]);
            s->y0 = F(tok[2]);
            s->x1 = F(tok[3]);
            s->y1 = F(tok[4]);
            s->mat = (uint8_t)I(tok[5]);
            s->obj = (int16_t)I(tok[6]);
            seg_finish(s);
        } else if (!strcmp(k, "circ") && n >= 6) {
            if (t->ncirc >= PB_MAX_CIRC) continue;
            pb_circ_t *c = &t->circ[t->ncirc++];
            c->x = F(tok[1]);
            c->y = F(tok[2]);
            c->r = F(tok[3]);
            c->mat = (uint8_t)I(tok[4]);
            c->obj = (int16_t)I(tok[5]);
        } else if (!strcmp(k, "trig") && n >= 6) {
            if (t->ntrig >= PB_MAX_TRIG) continue;
            pb_trig_t *g = &t->trig[t->ntrig++];
            g->x0 = F(tok[1]);
            g->y0 = F(tok[2]);
            g->x1 = F(tok[3]);
            g->y1 = F(tok[4]);
            g->obj = (int16_t)I(tok[5]);
            float dx = g->x1 - g->x0, dy = g->y1 - g->y0;
            g->len = sqrtf(dx * dx + dy * dy);
            g->dx = dx / g->len;
            g->dy = dy / g->len;
        } else if (!strcmp(k, "roll") && n >= 5) {
            if (t->nroll >= PB_MAX_ROLL) continue;
            pb_roll_t *r = &t->roll[t->nroll++];
            r->x = F(tok[1]);
            r->y = F(tok[2]);
            r->r = F(tok[3]);
            r->obj = (int16_t)I(tok[4]);
        } else if (!strcmp(k, "flipper") && n >= 9) {
            if (t->nflip >= 2) continue;
            pb_flipdef_t *f = &t->flip[t->nflip++];
            f->side = tok[1][0] == 'R';
            f->x = F(tok[2]);
            f->y = F(tok[3]);
            f->len = F(tok[4]);
            f->r0 = F(tok[5]);
            f->r1 = F(tok[6]);
            float rest = F(tok[7]) * PB_PI / 180.0f, up = F(tok[8]) * PB_PI / 180.0f;
            /* the right flipper is the left one mirrored */
            f->rest = f->side ? PB_PI - rest : rest;
            f->up = f->side ? PB_PI - up : up;
        } else if (!strcmp(k, "ramp") && n >= 4) {
            if (t->nramp >= PB_MAX_RAMP) continue;
            pb_ramp_t *r = &t->ramp[t->nramp++];
            r->obj = (int16_t)I(tok[1]);
            r->exit_speed = F(tok[2]);
            int np = I(tok[3]);
            if (np > PB_MAX_RAMP_PTS) np = PB_MAX_RAMP_PTS;
            if (n < 4 + np * 3) {
                ok = false;
                break;
            }
            r->n = np;
            float s = 0;
            for (int i = 0; i < np; i++) {
                r->x[i] = F(tok[4 + i * 3]);
                r->y[i] = F(tok[5 + i * 3]);
                r->z[i] = F(tok[6 + i * 3]);
                if (i) {
                    float dx = r->x[i] - r->x[i - 1], dy = r->y[i] - r->y[i - 1], dz = r->z[i] - r->z[i - 1];
                    s += sqrtf(dx * dx + dy * dy + dz * dz);
                }
                r->s[i] = s;
            }
            r->total = s;
        } else if (!strcmp(k, "lamp") && n >= 6) {
            if (t->nlamp >= PB_MAX_LAMP) continue;
            pb_lampdef_t *l = &t->lamp[t->nlamp++];
            copy(l->name, tok[1], sizeof l->name);
            l->kind = !strcmp(tok[2], "bumper") ? LK_BUMPER : !strcmp(tok[2], "sling") ? LK_SLING
                    : !strcmp(tok[2], "flasher") ? LK_FLASHER : LK_INSERT;
            l->sx = (int16_t)I(tok[3]);
            l->sy = (int16_t)I(tok[4]);
            l->color = HEX(tok[5]);
        } else if (!strcmp(k, "mission") && n >= 8) {
            if (t->nmission >= PB_MAX_MISSION) continue;
            pb_mission_t *m = &t->mission[t->nmission++];
            int g = lookup(GOALS, GL_N, tok[1]);
            m->goal = (uint8_t)(g < 0 ? GL_ANY : g);
            m->count = (int16_t)I(tok[2]);
            m->secs = (int16_t)I(tok[3]);
            m->points = (uint32_t)strtol(tok[4], NULL, 10);
            for (int l = 0; l < PB_LANGS; l++) copy(m->name[l], tok[5 + l], PB_TXT);
        } else if (!strcmp(k, "text") && n >= 5) {
            if (t->ntext >= PB_MAX_TEXT) continue;
            pb_text_t *x = &t->text[t->ntext++];
            copy(x->key, tok[1], sizeof x->key);
            for (int l = 0; l < PB_LANGS; l++) copy(x->s[l], tok[2 + l], PB_TXT);
        }
    }
    free(buf);
    if (!ok) {
        aos_hal_log("pinball", "the table's definition is broken near line %d", line);
        return false;
    }
    if (only_head) return t->id[0] != 0;
    for (int i = 0; i < t->nobj; i++) {
        t->obj[i].lamp = (int16_t)pb_table_find_lamp(t, t->obj[i].name);
    }
    if (t->nflip != 2 || t->cam.f <= 0 || t->w <= 0) {
        aos_hal_log("pinball", "table %s lacks flippers, camera or size", t->id);
        return false;
    }
    return true;
}

int pb_table_find_obj(const pb_table_t *t, const char *name)
{
    for (int i = 0; i < t->nobj; i++) {
        if (!strcmp(t->obj[i].name, name)) return i;
    }
    return -1;
}

int pb_table_find_lamp(const pb_table_t *t, const char *name)
{
    for (int i = 0; i < t->nlamp; i++) {
        if (!strcmp(t->lamp[i].name, name)) return i;
    }
    return -1;
}

const char *pb_table_text(const pb_table_t *t, const char *key, int lang)
{
    for (int i = 0; i < t->ntext; i++) {
        if (!strcmp(t->text[i].key, key)) return t->text[i].s[lang][0] ? t->text[i].s[lang] : t->text[i].s[0];
    }
    return NULL;
}

void pb_project(const pb_cam_t *c, float x, float y, float z, float *sx, float *sy, float *depth)
{
    float qx = x - c->px, qy = y - c->py, qz = z - c->pz;
    float yc = -qy * c->sp + qz * c->cp;
    float zc = -qy * c->cp - qz * c->sp;
    if (zc < 1.0f) zc = 1.0f;
    *sx = c->cx + c->f * qx / zc;
    *sy = c->cy - c->f * yc / zc;
    if (depth) *depth = zc;
}

float pb_plane_depth(const pb_cam_t *c, float sy, float h)
{
    float b = -(sy - c->cy) / c->f;
    float den = b * c->cp - c->sp;
    if (den > -1e-4f) return 1e6f;
    return (h - c->pz) / den;
}
