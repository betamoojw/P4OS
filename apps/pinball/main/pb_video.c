/*
 * PINBALL - the video modes (see pb_video.h)
 */
#include "pb_video.h"

#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

static const char *const NAMES[VM_N] = { "shoot", "invaders", "runner", "darts", "slots", "duel" };

int pb_video_kind(const char *name)
{
    for (int i = 0; i < VM_N; i++) {
        if (name && !strcmp(name, NAMES[i])) return i;
    }
    return -1;
}

static uint32_t rnd(pb_video_t *v)
{
    v->rng ^= v->rng << 13;
    v->rng ^= v->rng >> 17;
    v->rng ^= v->rng << 5;
    return v->rng;
}

static float frnd(pb_video_t *v)
{
    return (float)(rnd(v) & 0xFFFF) / 65535.0f;
}

/* ---- little pictures: a row a uint16, bit (w-1) the left dot ------------- */

static const uint16_t ZOMBIE[12] = { 0x0E0, 0x1F0, 0x150, 0x1F0, 0x0E0, 0x7FC, 0xCE6, 0x0E0, 0x0E0, 0x1B0,
                                     0x318, 0x318 };   /* 12 wide */
static const uint16_t SHIP[4] = { 0x08, 0x1C, 0x7F, 0x63 };            /* 7 */
static const uint16_t ALIEN[5] = { 0x22, 0x3E, 0x6B, 0x7F, 0x55 };     /* 7 */
static const uint16_t RUNNER[8] = { 0x0C, 0x0C, 0x1E, 0x2D, 0x0C, 0x12, 0x21, 0x21 };   /* 6 */
static const uint16_t ROCK[6] = { 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x3C };                 /* 8 */
static const uint16_t IDOL[7] = { 0x0E, 0x1F, 0x15, 0x1F, 0x0E, 0x1F, 0x1F };           /* 5 */
static const uint16_t BALLOON[8] = { 0x1E, 0x3F, 0x3F, 0x3F, 0x1E, 0x0C, 0x04, 0x08 };  /* 6 */
static const uint16_t PIRATE[16] = { 0x03C0, 0x0FF0, 0x1FF8, 0x0FF0, 0x0DB0, 0x0FF0, 0x07E0, 0x03C0,
                                     0x0FF0, 0x3FFC, 0x7FFE, 0x6FF6, 0x0FF0, 0x0E70, 0x0C30, 0x1C38 };  /* 16 */
static const uint16_t SYMS[6][11] = {
    { 0x7FE, 0x7FE, 0x00E, 0x01C, 0x038, 0x070, 0x0E0, 0x1C0, 0x1C0, 0x1C0, 0x1C0 },   /* 7 */
    { 0x000, 0x7FF, 0x7FF, 0x401, 0x5B5, 0x5B5, 0x5B5, 0x401, 0x7FF, 0x7FF, 0x000 },   /* BAR */
    { 0x00C, 0x030, 0x0C0, 0x140, 0x220, 0x410, 0x6D8, 0xFFC, 0xFFC, 0x6D8, 0x000 },   /* cherries */
    { 0x020, 0x070, 0x0F8, 0x1FC, 0x1FC, 0x1FC, 0x1FC, 0x3FE, 0x7FF, 0x020, 0x070 },   /* bell */
    { 0x020, 0x070, 0x070, 0x7FF, 0x3FE, 0x1FC, 0x0F8, 0x1FC, 0x18C, 0x306, 0x000 },   /* star */
    { 0x0F0, 0x1F8, 0x3FC, 0x7FE, 0x7FE, 0x7FE, 0x7FE, 0x3FC, 0x1F8, 0x0F0, 0x000 },   /* lemon */
};

static void pic(pb_dmd_t *d, int x, int y, const uint16_t *rows, int w, int h, int level)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (!(rows[j] & (1u << (w - 1 - i)))) continue;
            int px = x + i, py = y + j;
            if (px >= 0 && py >= 0 && px < DMD_W && py < DMD_H) d->px[py][px] = (uint8_t)level;
        }
    }
}

static pb_vitem_t *spawn(pb_video_t *v, int kind, int x, int y)
{
    for (int i = 0; i < VM_ITEMS; i++) {
        if (!v->it[i].life) {
            pb_vitem_t *p = &v->it[i];
            memset(p, 0, sizeof *p);
            p->kind = (int8_t)kind;
            p->x = (int16_t)x;
            p->y = (int16_t)y;
            p->life = 1;
            return p;
        }
    }
    return NULL;
}

void pb_video_start(pb_video_t *v, int kind, uint32_t seed)
{
    memset(v, 0, sizeof *v);
    v->kind = kind;
    v->on = true;
    v->rng = seed | 1;
    v->lives = 3;
    v->px = 64 * 16;
    v->lane = 1;
    v->next = 1.0f;
    switch (kind) {
    case VM_SHOOT: v->dur = 20; break;
    case VM_INVADERS:
        v->dur = 25;
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 5; c++) spawn(v, 2, (16 + c * 16) * 16, (9 + r * 7) * 16);
        v->next = 1;      /* the swarm's direction */
        break;
    case VM_RUNNER: v->dur = 25; v->lives = 2; break;
    case VM_DARTS: v->dur = 20; break;
    case VM_SLOTS:
        v->dur = 30;
        for (int i = 0; i < 3; i++) v->spin[i] = (float)(rnd(v) % 6);
        break;
    case VM_DUEL: v->dur = 40; v->enemy_hp = 5; break;
    default: v->dur = 15; break;
    }
}

static void finish(pb_video_t *v)
{
    if (v->end_t <= 0) v->end_t = 2.0f;
}

bool pb_video_step(pb_video_t *v, float dt, bool lp, bool rp, bool lh, bool rh)
{
    if (!v->on) return false;
    v->t += dt;
    if (v->end_t > 0) {
        v->end_t -= dt;
        if (v->end_t <= 0) v->on = false;
        return v->on;
    }
    if (v->t >= v->dur || v->lives <= 0) {
        finish(v);
        return true;
    }
    switch (v->kind) {
    case VM_SHOOT: {
        /* zombies rise; y counts dots shown (0..12) */
        v->next -= dt;
        if (v->next <= 0) {
            int g = (int)(rnd(v) % 3);
            bool busy = false;
            for (int i = 0; i < VM_ITEMS; i++) busy |= v->it[i].life && v->it[i].x == g;
            if (!busy) {
                pb_vitem_t *z = spawn(v, 1, g, 0);
                if (z) z->t = 0;
            }
            v->next = 0.9f - v->t * 0.02f + frnd(v) * 0.5f;
        }
        int shot = -1;
        if (lp) shot = rh ? 1 : 0;
        else if (rp) shot = lh ? 1 : 2;
        int aimed = shot;
        for (int i = 0; i < VM_ITEMS; i++) {
            pb_vitem_t *z = &v->it[i];
            if (!z->life) continue;
            z->t += dt;
            if (z->kind == 1) {
                z->y = (int16_t)(z->t < 0.4f ? z->t / 0.4f * 12 : 12);
                if (shot == z->x && z->y >= 5) {
                    z->kind = 2;
                    z->t = 0;
                    v->hits++;
                    v->score += 25000;
                    shot = -2;
                } else if (z->t > 1.9f - v->t * 0.03f) {
                    v->lives--;
                    z->life = 0;
                }
            } else if (z->t > 0.3f) {
                z->life = 0;
            }
        }
        if (shot >= 0) v->misses++;
        if (aimed >= 0) {
            v->fire = 0.12f;
            v->lane = aimed;
        } else {
            v->fire -= dt;
        }
        break;
    }
    case VM_INVADERS: {
        v->px += (int)((rh - lh) * 45 * 16 * dt);
        if (v->px < 4 * 16) v->px = 4 * 16;
        if (v->px > 123 * 16) v->px = 123 * 16;
        v->fire -= dt;
        if (v->fire <= 0) {
            spawn(v, 1, v->px, 27 * 16);
            v->fire = 0.33f;
        }
        float dir = v->next;
        int minx = 999, maxx = -1, alive = 0, low = 0;
        for (int i = 0; i < VM_ITEMS; i++) {
            pb_vitem_t *a = &v->it[i];
            if (!a->life || a->kind != 2) continue;
            alive++;
            int ax = a->x / 16;
            if (ax < minx) minx = ax;
            if (ax > maxx) maxx = ax;
            if (a->y / 16 > low) low = a->y / 16;
        }
        float spd = (8 + v->t * 0.8f) * 16 * dt;
        bool turn = (dir > 0 && maxx > 118) || (dir < 0 && minx < 2);
        for (int i = 0; i < VM_ITEMS; i++) {
            pb_vitem_t *a = &v->it[i];
            if (!a->life) continue;
            if (a->kind == 2) {
                if (turn) a->y += 3 * 16;
                else a->x += (int16_t)(dir * spd);
            } else {
                a->y -= (int16_t)(70 * 16 * dt);
                if (a->y < 0) a->life = 0;
                for (int k = 0; k < VM_ITEMS; k++) {
                    pb_vitem_t *b = &v->it[k];
                    if (!b->life || b->kind != 2) continue;
                    if (a->x / 16 >= b->x / 16 && a->x / 16 < b->x / 16 + 7 && a->y / 16 >= b->y / 16 &&
                        a->y / 16 < b->y / 16 + 5) {
                        b->life = 0;
                        a->life = 0;
                        v->hits++;
                        v->score += 20000;
                    }
                }
            }
        }
        if (turn) v->next = -dir;
        if (!alive) {
            v->score += 150000;
            for (int r = 0; r < 2; r++)
                for (int c = 0; c < 5; c++) spawn(v, 2, (16 + c * 16) * 16, (9 + r * 7) * 16);
        }
        if (low >= 23) v->lives = 0;
        break;
    }
    case VM_RUNNER: {
        if (lp && v->lane > 0) v->lane--;
        if (rp && v->lane < 2) v->lane++;
        v->next -= dt;
        if (v->next <= 0) {
            spawn(v, frnd(v) < 0.7f ? 1 : 2, (int)(rnd(v) % 3), -8 * 16);
            v->next = 0.75f - v->t * 0.015f;
        }
        float spd = (22 + v->t * 1.2f) * 16;
        for (int i = 0; i < VM_ITEMS; i++) {
            pb_vitem_t *o = &v->it[i];
            if (!o->life) continue;
            o->y += (int16_t)(spd * dt);
            int oy = o->y / 16;
            if (oy >= 20 && oy <= 28 && o->x == v->lane) {
                if (o->kind == 1) {
                    v->lives--;
                    v->misses++;
                } else {
                    v->hits++;
                    v->score += 30000;
                }
                o->life = 0;
            } else if (oy > 32) {
                o->life = 0;
                if (o->kind == 1) v->score += 2000;
            }
        }
        break;
    }
    case VM_DARTS: {
        v->px += (int)((rh - lh) * 50 * 16 * dt);
        if (v->px < 3 * 16) v->px = 3 * 16;
        if (v->px > 124 * 16) v->px = 124 * 16;
        if ((lp && rh) || (rp && lh)) spawn(v, 2, v->px, 26 * 16);
        v->next -= dt;
        if (v->next <= 0) {
            pb_vitem_t *b = spawn(v, 1, (int)(8 + rnd(v) % 112) * 16, 34 * 16);
            if (b) b->t = 8 + frnd(v) * 10;
            v->next = 0.7f;
        }
        for (int i = 0; i < VM_ITEMS; i++) {
            pb_vitem_t *o = &v->it[i];
            if (!o->life) continue;
            if (o->kind == 1) {
                o->y -= (int16_t)(o->t * 16 * dt);
                if (o->y < -8 * 16) o->life = 0;
            } else {
                o->y -= (int16_t)(80 * 16 * dt);
                if (o->y < 0) o->life = 0;
                for (int k = 0; k < VM_ITEMS; k++) {
                    pb_vitem_t *b = &v->it[k];
                    if (!b->life || b->kind != 1) continue;
                    int dx = o->x / 16 - b->x / 16, dy = o->y / 16 - b->y / 16;
                    if (dx >= 0 && dx < 6 && dy >= 0 && dy < 6) {
                        b->life = 0;
                        o->life = 0;
                        v->hits++;
                        v->score += 20000;
                    }
                }
            }
        }
        break;
    }
    case VM_SLOTS: {
        for (int i = v->stopped; i < 3; i++) v->spin[i] += dt * (9.0f + i * 1.7f);
        bool press = lp || rp;
        if (v->t > 4.0f + v->stopped * 1.5f) press = true;       /* it stops by itself */
        if (press && v->stopped < 3) {
            v->reel[v->stopped] = ((int)(v->spin[v->stopped] + 0.5f)) % 6;
            v->spin[v->stopped] = (float)v->reel[v->stopped];
            v->stopped++;
            if (v->stopped == 3) {
                int a = v->reel[0], b = v->reel[1], c = v->reel[2];
                if (a == 0 && b == 0 && c == 0) {
                    v->result = 3;
                    v->score = 1000000;
                } else if (a == b && b == c) {
                    v->result = 2;
                    v->score = 300000;
                } else if (a == b || b == c || a == c) {
                    v->result = 1;
                    v->score = 75000;
                } else if (a == 2 || b == 2 || c == 2) {
                    v->result = 1;
                    v->score = 25000;
                }
                finish(v);
            }
        }
        break;
    }
    case VM_DUEL: {
        if (v->threat == 0) {
            v->next -= dt;
            if (v->next <= 0) {
                v->threat = 1 + (int)(rnd(v) % 2);
                v->threat_t = 0.8f - (5 - v->enemy_hp) * 0.08f;
            }
            if (lp || rp) v->misses++;
        } else {
            v->threat_t -= dt;
            bool right = (v->threat == 1 && lp) || (v->threat == 2 && rp);
            bool wrong = (v->threat == 1 && rp) || (v->threat == 2 && lp);
            if (right) {
                v->enemy_hp--;
                v->hits++;
                v->score += 50000;
                v->threat = 0;
                v->next = 0.8f + frnd(v) * 0.7f;
                v->fire = 0.25f;
                if (v->enemy_hp <= 0) {
                    v->score += 250000;
                    finish(v);
                }
            } else if (wrong || v->threat_t <= 0) {
                v->lives--;
                v->threat = 0;
                v->next = 0.9f + frnd(v) * 0.6f;
                v->misses++;
            }
        }
        if (v->fire > 0) v->fire -= dt;
        break;
    }
    }
    return true;
}

static void hud(const pb_video_t *v, pb_dmd_t *d, const char *title)
{
    char s[32];
    dmd_number(s, sizeof s, v->score);
    dmd_text(d, DMD_W - dmd_text_w(s, 1) - 1, 0, s, 1, 12);
    /* the game's name while it starts, then the seconds left */
    if (v->t < 2.0f) {
        dmd_text(d, 1, 0, title, 1, ((int)(v->t * 6)) & 1 ? 15 : 8);
        return;
    }
    int left = (int)(v->dur - v->t + 0.99f);
    if (left < 0) left = 0;
    snprintf(s, sizeof s, "%d", left);
    dmd_text(d, 1, 0, s, 1, 9);
}

void pb_video_draw(const pb_video_t *v, pb_dmd_t *d, int lang)
{
    (void)lang;
    dmd_clear(d);
    if (v->end_t > 0) {
        char s[32];
        dmd_number(s, sizeof s, v->score);
        const char *msg = v->kind == VM_SLOTS ? (v->result == 3 ? _("¡JACKPOT 777!") : v->result == 2 ? _("¡TRIPLE!")
                                                 : v->result == 1 ? _("¡PREMIO!") : _("SUERTE LA PRÓXIMA"))
                        : v->kind == VM_DUEL && v->enemy_hp <= 0 ? _("¡GANASTE EL DUELO!")
                        : _("FIN DEL JUEGO");
        if (v->kind == VM_SLOTS) {
            for (int i = 0; i < 3; i++) pic(d, 18 + i * 40, 2, SYMS[v->reel[i]], 11, 11, 15);
            dmd_text_c(d, 15, msg, 1, 15, 0);
            dmd_text_c(d, 24, s, 1, 12, 0);
        } else {
            dmd_text_c(d, 5, msg, 1, 15, 0);
            dmd_text_c(d, 15, s, 2, 15, 0);
        }
        return;
    }
    switch (v->kind) {
    case VM_SHOOT:
        hud(v, d, _("ZOMBIS"));
        for (int g = 0; g < 3; g++) {
            int cx = 20 + g * 44;
            dmd_rect(d, cx - 8, 30, 17, 2, 5);           /* the grave's earth */
        }
        for (int i = 0; i < VM_ITEMS; i++) {
            const pb_vitem_t *z = &v->it[i];
            if (!z->life) continue;
            int cx = 20 + z->x * 44;
            if (z->kind == 1) {
                pic(d, cx - 6, 30 - z->y, ZOMBIE, 12, z->y, 13);
            } else {
                for (int k = 0; k < 8; k++) d->px[22 + (k * 5) % 8][cx - 4 + k] = 15;
            }
        }
        if (v->fire > 0) {
            int cx = 20 + v->lane * 44;
            dmd_frame(d, cx - 4, 18, 9, 9, 15);
        }
        for (int i = 0; i < v->lives; i++) dmd_rect(d, 1 + i * 4, 9, 3, 3, 10);
        break;
    case VM_INVADERS:
        hud(v, d, _("INVASORES"));
        pic(d, v->px / 16 - 3, 28, SHIP, 7, 4, 15);
        for (int i = 0; i < VM_ITEMS; i++) {
            const pb_vitem_t *a = &v->it[i];
            if (!a->life) continue;
            if (a->kind == 2) pic(d, a->x / 16, a->y / 16, ALIEN, 7, 5, 11);
            else dmd_rect(d, a->x / 16, a->y / 16, 1, 2, 15);
        }
        break;
    case VM_RUNNER:
        hud(v, d, _("CORRÉ"));
        for (int l = 0; l < 2; l++) dmd_rect(d, 48 + l * 32, 8, 1, 24, 3);
        pic(d, 32 + v->lane * 32 - 3, 24, RUNNER, 6, 8, 15);
        for (int i = 0; i < VM_ITEMS; i++) {
            const pb_vitem_t *o = &v->it[i];
            if (!o->life) continue;
            int x = 32 + o->x * 32;
            if (o->kind == 1) pic(d, x - 4, o->y / 16, ROCK, 8, 6, 9);
            else pic(d, x - 2, o->y / 16, IDOL, 5, 7, 15);
        }
        for (int i = 0; i < v->lives; i++) dmd_rect(d, 1 + i * 4, 9, 3, 3, 10);
        break;
    case VM_DARTS:
        hud(v, d, _("GLOBOS"));
        dmd_rect(d, v->px / 16 - 2, 30, 5, 2, 15);
        dmd_rect(d, v->px / 16, 27, 1, 3, 15);
        for (int i = 0; i < VM_ITEMS; i++) {
            const pb_vitem_t *o = &v->it[i];
            if (!o->life) continue;
            if (o->kind == 1) pic(d, o->x / 16, o->y / 16, BALLOON, 6, 8, 12);
            else dmd_rect(d, o->x / 16, o->y / 16, 1, 3, 15);
        }
        break;
    case VM_SLOTS:
        hud(v, d, _("TRAGAMONEDAS"));
        for (int i = 0; i < 3; i++) {
            int x = 18 + i * 40;
            float sp = v->spin[i];
            int a = ((int)sp) % 6, b = (a + 1) % 6;
            int off = i < v->stopped ? 0 : (int)((sp - (int)sp) * 12);
            dmd_frame(d, x - 3, 8, 17, 24, 6);
            /* the reel rolls down: two symbols through the window */
            for (int j = 0; j < 11; j++) {
                int y1 = 14 + j + off, y2 = 14 + j + off - 12;
                for (int k = 0; k < 11; k++) {
                    if (y1 >= 9 && y1 < 31 && (SYMS[a][j] & (1u << (10 - k)))) d->px[y1][x + k] = 15;
                    if (y2 >= 9 && y2 < 31 && (SYMS[b][j] & (1u << (10 - k)))) d->px[y2][x + k] = 15;
                }
            }
        }
        break;
    case VM_DUEL: {
        hud(v, d, _("DUELO"));
        pic(d, 56, 14, PIRATE, 16, 16, 12);
        for (int i = 0; i < v->enemy_hp; i++) dmd_rect(d, 100 + i * 5, 9, 3, 3, 12);
        for (int i = 0; i < v->lives; i++) dmd_rect(d, 1 + i * 5, 9, 3, 3, 10);
        if (v->threat) {
            bool blink = ((int)(v->t * 10)) & 1;
            int y = v->threat == 1 ? 12 : 26;
            dmd_text(d, 22, y - 3, v->threat == 1 ? "<<" : "<<", 1, blink ? 15 : 8);
            dmd_text(d, 92, y - 3, ">>", 1, blink ? 15 : 8);
            dmd_rect(d, 40, y, 14, 1, 15);
        }
        if (v->fire > 0) dmd_frame(d, 54, 12, 20, 20, 15);
        break;
    }
    }
}
