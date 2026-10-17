/*
 * PINBALL - the rules (see pb_game.h)
 */
#include "pb_game.h"

#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const PB_SND_NAMES[SND_N] = {
    "flipper", "bumper", "sling", "drop", "bank", "target", "lane", "lanes", "spinner", "saucer", "eject",
    "ramp", "rampfail", "orbit", "launch", "drain", "save", "kickback", "mission", "missiondone", "jackpot",
    "multiball", "lock", "extra", "tiltwarn", "tilt", "skill", "bonus", "gameover", "start", "wall", "post",
    "toyhit", "toydown", "video", "cannon", "magnet", "disc", "letter", "videohit",
};

#define BALL_SAVE_S     10.0f
#define MB_SAVE_S       15.0f
#define SKILL_S         6.0f
#define COMBO_S         3.0f
#define ORBIT_S         2.5f

static void feat_setup(pb_game_t *g);
static void feat_reset(pb_game_t *g);
static void feat_update(pb_game_t *g, float dt);
static int  feat_lamp(const pb_game_t *g, const char *n);
static void start_multiball(pb_game_t *g, bool wizard);
static void jackpot(pb_game_t *g);
static void mission_goal(pb_game_t *g, int goal);

static const char *TX(const pb_game_t *g, const char *key, const char *fallback)
{
    const char *s = pb_table_text(g->t, key, g->lang);
    return s && s[0] ? s : fallback;
}

void pb_game_sound(pb_game_t *g, int snd)
{
    if (g->nsnd < PB_SND_Q) g->snd[g->nsnd++] = (uint8_t)snd;
}

void pb_game_say(pb_game_t *g, const char *a, const char *b, float secs, bool big)
{
    /* a message of the same kind replaces the one waiting */
    if (g->nmsg >= PB_MSG_MAX) {
        memmove(&g->msg[0], &g->msg[1], sizeof(pb_msg_t) * (PB_MSG_MAX - 1));
        g->nmsg--;
    }
    pb_msg_t *m = &g->msg[g->nmsg++];
    memset(m, 0, sizeof *m);
    snprintf(m->a, sizeof m->a, "%s", a ? a : "");
    snprintf(m->b, sizeof m->b, "%s", b ? b : "");
    m->secs = secs;
    m->big = big;
    if (g->nmsg == 1) g->msg_t = 0;
}

static void points(pb_game_t *g, uint32_t p)
{
    if (g->tilted || g->state != GS_PLAY) return;
    if (g->wizard) p *= 5;
    g->score += p;
}

static void flash_lamp(pb_game_t *g, const char *name, float secs)
{
    int l = pb_table_find_lamp(g->t, name);
    if (l >= 0) g->lamp_flash[l] = secs;
}

static void flash_obj(pb_game_t *g, int obj, float secs)
{
    int l = g->t->obj[obj].lamp;
    if (l >= 0) g->lamp_flash[l] = secs;
}

static void big_flash(pb_game_t *g)
{
    flash_lamp(g, "flash1", 0.5f);
    flash_lamp(g, "flash2", 0.5f);
    flash_lamp(g, "flash3", 0.5f);
    g->flash_req = true;
}

void pb_game_init(pb_game_t *g, const pb_table_t *t, pb_phys_t *ph, uint32_t best)
{
    memset(g, 0, sizeof *g);
    g->t = t;
    g->ph = ph;
    g->best = best;
    g->lang = pb_lang();
    g->state = GS_ATTRACT;
    g->mission = g->mission_lit = -1;
    g->skill_lane = -1;
    feat_setup(g);
}

static void new_ball(pb_game_t *g)
{
    g->bonus = 0;
    g->bonus_x = 1;
    g->tilt = 0;
    g->tilt_warns = 0;
    g->tilted = false;
    g->ph->tilted = false;
    g->launched = false;
    g->ball_saved_once = false;
    g->skill_lane = (int)((g->clock * 1000.0f)) % 3;
    g->skill_t = 0;
    g->need_serve = true;
    g->ph->saucer_hold = 1.2f;
}

void pb_game_start(pb_game_t *g)
{
    pb_phys_reset(g->ph);
    uint32_t best = g->best;
    const pb_table_t *t = g->t;
    pb_phys_t *ph = g->ph;
    pb_game_init(g, t, ph, best);
    g->state = GS_PLAY;
    g->ball = 1;
    g->lanes = 0;
    g->spin_x = 1;
    g->jackpot = 100000;
    g->mission_next = 0;
    g->ph->kickback_lit = true;
    feat_reset(g);
    new_ball(g);
    pb_game_say(g, TX(g, "start", _("¡A JUGAR!")), NULL, 2.0f, false);
    pb_game_sound(g, SND_START);
}

bool pb_game_wants_launch(const pb_game_t *g)
{
    if (g->state != GS_PLAY) return false;
    return (pb_phys_on_plunger(g->ph) >= 0 && !g->auto_on_plunger) || pb_phys_cannon_loaded(g->ph) >= 0;
}

void pb_game_flipper(pb_game_t *g, int side)
{
    if (g->state != GS_PLAY || g->tilted) return;
    /* the lit top lanes move round with the flippers */
    uint8_t l = g->lanes & 7;
    if (side == 0) l = (uint8_t)(((l >> 1) | (l << 2)) & 7);
    else l = (uint8_t)(((l << 1) | (l >> 2)) & 7);
    g->lanes = l;
}

bool pb_game_nudge(pb_game_t *g)
{
    if (g->state != GS_PLAY || g->tilted) return false;
    g->tilt += 1.0f;
    if (g->tilt > 3.2f) {
        g->tilted = true;
        g->ph->tilted = true;
        g->ph->flip[0].held = g->ph->flip[1].held = false;
        g->ball_save = 0;
        pb_game_say(g, _("TILT"), NULL, 3.0f, true);
        pb_game_sound(g, SND_TILT);
    } else if (g->tilt > 2.0f) {
        g->tilt_warns++;
        pb_game_say(g, _("¡CUIDADO!"), NULL, 1.0f, true);
        pb_game_sound(g, SND_TILT_WARN);
    }
    return true;
}

/* ---- the rules ---------------------------------------------------------- */

static void mission_start(pb_game_t *g, int m)
{
    const pb_mission_t *mi = &g->t->mission[m];
    g->mission = m;
    g->mission_left = mi->count;
    g->mission_t = (float)mi->secs;
    g->mission_lit = -1;
    g->ph->saucer_hold = 2.4f;
    pb_game_say(g, _("MISIÓN"), mi->name[g->lang][0] ? mi->name[g->lang] : mi->name[0], 2.4f, false);
    pb_game_sound(g, SND_MISSION);
    big_flash(g);
}

static void start_multiball(pb_game_t *g, bool wizard)
{
    g->multiball = true;
    g->wizard = wizard;
    g->locks = 0;
    g->lock_lit = false;
    g->to_serve += 2;
    g->mb_serve_t = 0.8f;
    g->ball_save = MB_SAVE_S;
    g->jackpot = wizard ? 500000 : 100000 + (uint32_t)g->ramps * 10000;
    pb_game_say(g, wizard ? TX(g, "wizard", _("MODO FINAL")) : TX(g, "multiball", _("MULTIBOLA")),
                wizard ? _("TODO VALE x5") : _("JACKPOT EN LA RAMPA"), 3.0f, false);
    pb_game_sound(g, SND_MULTIBALL);
    big_flash(g);
}

static void jackpot(pb_game_t *g)
{
    char num[24], line[64];
    dmd_number(num, sizeof num, g->jackpot);
    snprintf(line, sizeof line, "%s", num);
    points(g, g->jackpot);
    pb_game_say(g, TX(g, "jackpot", _("JACKPOT")), line, 2.2f, false);
    pb_game_sound(g, SND_JACKPOT);
    big_flash(g);
    g->jackpot += 50000;
}

static void mission_goal(pb_game_t *g, int goal)
{
    if (g->mission < 0) return;
    const pb_mission_t *mi = &g->t->mission[g->mission];
    if (mi->goal != goal && mi->goal != GL_ANY) return;
    if (--g->mission_left > 0) {
        points(g, 5000);
        return;
    }
    char num[24];
    dmd_number(num, sizeof num, mi->points);
    points(g, mi->points);
    g->missions_done |= 1u << g->mission;
    pb_game_say(g, _("¡MISIÓN CUMPLIDA!"), num, 2.5f, false);
    pb_game_sound(g, SND_MISSION_DONE);
    big_flash(g);
    g->mission = -1;
    g->bonus += 25000;
    int n = g->t->nmission;
    if (n && g->missions_done == (n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1) && !g->multiball) {
        start_multiball(g, true);
        g->missions_done = 0;
    }
    /* three missions light the extra ball too */
    int done = 0;
    for (int i = 0; i < n; i++) done += (g->missions_done >> i) & 1;
    if (done == 3 && !g->extra_given) g->extra_lit = true;
}

static void light_mission(pb_game_t *g)
{
    int n = g->t->nmission;
    if (!n || g->mission >= 0 || g->mission_lit >= 0) return;
    for (int k = 0; k < n; k++) {
        int m = (g->mission_next + k) % n;
        if (!(g->missions_done & (1u << m))) {
            g->mission_lit = m;
            g->mission_next = (m + 1) % n;
            pb_game_say(g, _("MISIÓN LISTA"), _("TIRÁ AL EMBUDO"), 2.0f, false);
            return;
        }
    }
}


/* ---- the table's own attractions ---------------------------------------------
 *
 * A table lists them in its definition ("feat <kind> key=value ..."):
 *
 *   bash     toy= hits= points= text= lights=video|lock|multiball
 *            a toy to beat: hit it 'hits' times and it sinks (the zombie
 *            back in its grave, the ship under the waves); it comes back
 *            harder after a while
 *   lockpen  saucer= toy=   a lock you can see: while the lock is lit and
 *            the toy is down, the saucer keeps the ball, and a new one is
 *            served; the second lock starts multiball and lets them go
 *   letters  objs=a,b,c word=   spell the word with those targets or
 *            rollovers: it lights the video mode
 *   video    game= saucer= light=letters|bash|always   a DMD game played
 *            while the ball waits in that saucer
 *   roulette toy=   the ball leaving the spinning disc lands on a number:
 *            the lit colour pays, the zero pays a lot
 *   cannon   toy= targets=   the cannon swings with the ball in it; the
 *            launch button fires; targets hit soon after pay more
 *   magnet   toy= every= on= text=   on now and then; it grabs the ball,
 *            pays, and throws it
 *   disc     toy=   a turning platform that pays for every ride
 */

static const char *const FEATS[FK_N] = { "bash", "lockpen", "letters", "video", "roulette", "cannon", "magnet",
                                         "disc" };

static int obj_named(const pb_table_t *t, const char *name)
{
    return name ? pb_table_find_obj(t, name) : -1;
}

static void feat_setup(pb_game_t *g)
{
    const pb_table_t *t = g->t;
    g->nfeat = 0;
    g->video_feat = -1;
    g->letters_feat = -1;
    for (int i = 0; i < t->nfeat && g->nfeat < PB_MAX_FEAT; i++) {
        const pb_feat_t *f = &t->feat[i];
        int k = -1;
        for (int j = 0; j < FK_N; j++) {
            if (!strcmp(f->kind, FEATS[j])) k = j;
        }
        if (k < 0) continue;
        pb_fstate_t *s = &g->feat[g->nfeat++];
        memset(s, 0, sizeof *s);
        s->kind = (uint8_t)k;
        s->toy = (int16_t)pb_table_find_toy(t, pb_feat_get(f, "toy", ""));
        s->obj = (int16_t)obj_named(t, pb_feat_get(f, "saucer", NULL));
        s->need = s->need0 = pb_feat_int(f, "hits", 3);
        s->need_lit = (uint8_t)pb_feat_int(f, "lit", 1);
        s->points = (uint32_t)pb_feat_int(f, "points", 150000);
        s->every = (float)pb_feat_int(f, "every", 25);
        s->on_s = (float)pb_feat_int(f, "on", 8);
        snprintf(s->text, sizeof s->text, "%s", pb_feat_get(f, "text", ""));
        snprintf(s->text_hit, sizeof s->text_hit, "%s", pb_feat_get(f, "hit", "toyhit"));
        snprintf(s->text_done, sizeof s->text_done, "%s", pb_feat_get(f, "done", "toydown"));
        snprintf(s->text_back, sizeof s->text_back, "%s", pb_feat_get(f, "back", "toyback"));
        const char *l = pb_feat_get(f, "lights", pb_feat_get(f, "light", ""));
        s->lights = !strcmp(l, "video") || !strcmp(l, "letters") ? 1 : !strcmp(l, "lock") ? 2
                  : !strcmp(l, "multiball") ? 3 : !strcmp(l, "always") ? 4 : !strcmp(l, "bash") ? 5 : 0;
        snprintf(s->word, sizeof s->word, "%s", pb_feat_get(f, "word", ""));
        if (k == FK_VIDEO) s->round = pb_video_kind(pb_feat_get(f, "game", "shoot"));
        if (k == FK_LETTERS) g->letters_feat = g->nfeat - 1;
        /* a list of objects: letters' targets, the cannon's */
        char buf[24];
        snprintf(buf, sizeof buf, "%s", pb_feat_get(f, k == FK_CANNON ? "targets" : "objs", ""));
        for (char *a = buf; *a && s->nobjs < 8;) {
            char *c = strchr(a, ',');
            if (c) *c = 0;
            int o = obj_named(t, a);
            if (o >= 0) s->objs[s->nobjs++] = (int16_t)o;
            if (!c) break;
            a = c + 1;
        }
    }
}

static pb_fstate_t *feat_of(pb_game_t *g, int kind)
{
    for (int i = 0; i < g->nfeat; i++) {
        if (g->feat[i].kind == kind) return &g->feat[i];
    }
    return NULL;
}

static void feat_reset(pb_game_t *g)
{
    for (int i = 0; i < g->nfeat; i++) {
        pb_fstate_t *s = &g->feat[i];
        s->count = 0;
        s->round = s->kind == FK_VIDEO ? s->round : 0;
        s->need = s->need0;
        s->mask = 0;
        s->t = 0;
        if (s->toy >= 0) pb_phys_toy_up(g->ph, s->toy, true);
        if (s->kind == FK_LOCKPEN && s->obj >= 0) pb_phys_saucer_keep(g->ph, s->obj, false);
    }
    g->video_lit = false;
    g->video.on = false;
    g->video_feat = -1;
}

static void feat_light(pb_game_t *g, int what)
{
    if (what == 1 || what == 5) {
        if (!g->video_lit && feat_of(g, FK_VIDEO)) {
            g->video_lit = true;
            pb_game_say(g, _("MINIJUEGO LISTO"), _("TIRÁ AL EMBUDO"), 2.0f, false);
        }
    } else if (what == 2) {
        if (!g->lock_lit && !g->multiball) {
            g->lock_lit = true;
            pb_game_say(g, _("TRABA LISTA"), NULL, 1.6f, false);
        }
    } else if (what == 3) {
        if (!g->multiball) start_multiball(g, false);
    }
}

static void feat_toy_hit(pb_game_t *g, int obj)
{
    const pb_table_t *t = g->t;
    int toy = -1;
    for (int i = 0; i < t->ntoy; i++) {
        if (t->toy[i].obj == obj) toy = i;
    }
    pb_game_sound(g, SND_TOY_HIT);
    mission_goal(g, GL_TOY);
    points(g, 10000);
    g->bonus += 1000;
    for (int i = 0; i < g->nfeat; i++) {
        pb_fstate_t *s = &g->feat[i];
        if (s->kind != FK_BASH || s->toy != toy || !g->ph->toy[toy].up) continue;
        if (++s->count < s->need) {
            char a[24];
            snprintf(a, sizeof a, "%d / %d", s->count, s->need);
            pb_game_say(g, TX(g, s->text_hit, _("¡GOLPE!")), a, 0.9f, false);
            continue;
        }
        /* beaten */
        s->count = 0;
        s->round++;
        s->t = 12.0f;
        pb_phys_toy_up(g->ph, toy, false);
        char num[24];
        uint32_t p = s->points * (uint32_t)s->round;
        dmd_number(num, sizeof num, p);
        points(g, p);
        pb_game_say(g, TX(g, s->text_done, _("¡DERROTADO!")), num, 2.2f, false);
        pb_game_sound(g, SND_TOY_DOWN);
        big_flash(g);
        if (s->lights) feat_light(g, s->lights);
        if (g->multiball) jackpot(g);
    }
}

/* the ball in a saucer: an attraction takes it, or false */
static bool feat_saucer(pb_game_t *g, int obj, int ball)
{
    pb_ball_t *b = ball >= 0 ? &g->ph->ball[ball] : NULL;
    for (int i = 0; i < g->nfeat; i++) {
        pb_fstate_t *s = &g->feat[i];
        if (s->obj != obj) continue;
        if (s->kind == FK_LOCKPEN && g->ph->keep[obj]) {
            /* locked: it sits there a moment, then the toy swallows it and a
             * new ball comes; multiball brings them back */
            pb_phys_saucer_keep(g->ph, obj, false);
            g->lock_lit = false;
            g->locks++;
            g->targets = 0;
            pb_game_sound(g, SND_LOCK);
            if (b) b->hold = 1.6f;
            if (g->locks >= 2) {
                start_multiball(g, false);
            } else {
                char a[32];
                snprintf(a, sizeof a, _("BOLA %d TRABADA"), g->locks);
                pb_game_say(g, TX(g, "lock", _("BOLA TRABADA")), a, 2.0f, false);
                if (b) b->vanish = true;
                g->need_serve = true;
                g->launched = false;
            }
            return true;
        }
        bool other = g->mission_lit >= 0 || g->extra_lit;     /* the saucer has something lit of its own */
        if (s->kind == FK_VIDEO && s->round >= 0 && !g->multiball &&
            (g->video_lit || (s->lights == 4 && !other))) {
            g->video_lit = false;
            g->video_feat = i;
            g->video_ball = ball;
            if (b) b->hold = 1e9f;
            pb_video_start(&g->video, s->round, (uint32_t)(g->clock * 1000) + 77);
            pb_game_sound(g, SND_VIDEO);
            return true;
        }
    }
    return false;
}

static void feat_obj(pb_game_t *g, int obj)
{
    /* letters: a target or rollover of the word */
    if (g->letters_feat < 0) return;
    pb_fstate_t *s = &g->feat[g->letters_feat];
    for (int k = 0; k < s->nobjs; k++) {
        if (s->objs[k] != obj || (s->mask >> k) & 1) continue;
        s->mask |= 1u << k;
        points(g, 15000);
        pb_game_sound(g, SND_LETTER);
        g->letters_show = 1.5f;
        if (s->mask == (1u << s->nobjs) - 1) {
            s->mask = 0;
            points(g, 100000);
            pb_game_say(g, s->word, _("¡COMPLETO!"), 2.0f, true);
            big_flash(g);
            feat_light(g, s->lights ? s->lights : 1);
        }
    }
    /* the cannon's targets, soon after a shot */
    pb_fstate_t *c = feat_of(g, FK_CANNON);
    if (c && g->cannon_t > 0) {
        for (int k = 0; k < c->nobjs; k++) {
            if (c->objs[k] != obj) continue;
            g->cannon_t = 0;
            c->count++;
            uint32_t p = 100000u * (uint32_t)c->count;
            char num[24];
            dmd_number(num, sizeof num, p);
            points(g, p);
            pb_game_say(g, TX(g, "cannon", _("¡BLANCO!")), num, 2.0f, false);
            pb_game_sound(g, SND_JACKPOT);
            big_flash(g);
        }
    }
}

/* where a roulette's ball lands */
static const uint8_t WHEEL[37] = { 0, 32, 15, 19, 4, 21, 2, 25, 17, 34, 6, 27, 13, 36, 11, 30, 8, 23, 10, 5, 24, 16,
                                   33, 1, 20, 14, 31, 9, 22, 18, 29, 7, 28, 12, 35, 3, 26 };

static bool red_number(int n)
{
    static const uint8_t RED[18] = { 1, 3, 5, 7, 9, 12, 14, 16, 18, 19, 21, 23, 25, 27, 30, 32, 34, 36 };
    for (int i = 0; i < 18; i++) {
        if (RED[i] == n) return true;
    }
    return false;
}

static void feat_disc_out(pb_game_t *g, int obj, float ang)
{
    for (int i = 0; i < g->nfeat; i++) {
        pb_fstate_t *s = &g->feat[i];
        if (s->toy < 0 || g->t->toy[s->toy].obj != obj) continue;
        if (s->kind == FK_DISC) {
            s->count++;
            points(g, 5000u * (uint32_t)s->count);
            pb_game_sound(g, SND_DISC);
            mission_goal(g, GL_TOY);
        } else if (s->kind == FK_ROULETTE) {
            int pocket = (int)(ang / (2 * 3.14159265f) * 37.0f) % 37;
            int n = WHEEL[pocket];
            char a[32], num[24];
            bool red = red_number(n);
            if (n == 0) {
                points(g, 500000);
                dmd_number(num, sizeof num, 500000);
                pb_game_say(g, _("¡CERO!"), num, 2.4f, false);
                pb_game_sound(g, SND_JACKPOT);
                big_flash(g);
            } else {
                bool win = (red ? 0 : 1) == g->roulette_bet;
                snprintf(a, sizeof a, "%s %d", red ? _("ROJO") : _("NEGRO"), n);
                uint32_t p = win ? 60000 : 5000;
                dmd_number(num, sizeof num, p);
                points(g, p);
                pb_game_say(g, a, win ? num : _("LA CASA GANA"), 1.6f, false);
                pb_game_sound(g, win ? SND_LANES : SND_DISC);
                if (win) flash_lamp(g, "flash1", 0.4f);
            }
            g->roulette_bet ^= 1;
            mission_goal(g, GL_TOY);
        }
    }
}

static void feat_update(pb_game_t *g, float dt)
{
    if (g->letters_show > 0) g->letters_show -= dt;
    if (g->cannon_t > 0) g->cannon_t -= dt;
    for (int i = 0; i < g->nfeat; i++) {
        pb_fstate_t *s = &g->feat[i];
        if (s->kind == FK_BASH && s->toy >= 0 && !g->ph->toy[s->toy].up) {
            s->t -= dt;
            /* not over a ball locked inside it */
            for (int k = 0; k < g->nfeat; k++) {
                const pb_fstate_t *l = &g->feat[k];
                if (l->kind != FK_LOCKPEN || l->toy != s->toy || l->obj < 0) continue;
                for (int b = 0; b < PB_MAX_BALLS; b++) {
                    if (g->ph->ball[b].state == BS_SAUCER && g->ph->ball[b].saucer == l->obj && s->t < 1) s->t = 1;
                }
            }
            if (s->t <= 0) {
                /* it is back, and takes one more */
                pb_phys_toy_up(g->ph, s->toy, true);
                if (s->need < 6) s->need++;
                pb_game_say(g, TX(g, s->text_back, _("¡VOLVIÓ!")), NULL, 1.4f, true);
            }
        } else if (s->kind == FK_LOCKPEN && s->obj >= 0) {
            /* the pen keeps a ball while the lock is lit and its door is open */
            bool open = s->toy < 0 || !g->ph->toy[s->toy].up;
            pb_phys_saucer_keep(g->ph, s->obj, (g->lock_lit || !s->need_lit) && open && !g->multiball);
        } else if (s->kind == FK_MAGNET && s->toy >= 0) {
            s->t += dt;
            float cyc = s->every > 1 ? s->every : 25;
            bool on = fmodf(s->t, cyc) > cyc - (s->on_s > 0 ? s->on_s : 8) || g->multiball;
            pb_phys_magnet(g->ph, s->toy, on && !g->tilted);
        }
    }
    if (g->video.on) {
        if (!pb_video_step(&g->video, dt, g->in_lp, g->in_rp, g->in_lh, g->in_rh)) {
            uint32_t p = g->video.score;
            points(g, p);
            char num[24];
            dmd_number(num, sizeof num, p);
            pb_game_say(g, _("MINIJUEGO"), num, 2.0f, false);
            if (g->video_ball >= 0 && g->ph->ball[g->video_ball].state == BS_SAUCER) g->ph->ball[g->video_ball].hold = 0.4f;
            g->video_feat = -1;
        }
    }
    g->in_lp = g->in_rp = false;
}

static int feat_lamp(const pb_game_t *g, const char *n)
{
    if (!strcmp(n, "video")) return g->video_lit ? LM_FAST : g->video.on ? LM_ON : LM_OFF;
    if (!strncmp(n, "letter", 6) && g->letters_feat >= 0) {
        int k = n[6] - '1';
        return (g->feat[g->letters_feat].mask >> k) & 1 ? LM_ON : LM_SLOW;
    }
    if (!strcmp(n, "lockpen")) return g->lock_lit ? LM_FAST : LM_OFF;
    if (!strcmp(n, "cannon")) return pb_phys_cannon_loaded(g->ph) >= 0 ? LM_FAST : LM_OFF;
    if (!strcmp(n, "magnet")) {
        for (int i = 0; i < g->t->ntoy; i++) {
            if (g->t->toy[i].kind == TK_MAGNET) return g->ph->toy[i].on ? LM_FAST : LM_OFF;
        }
    }
    if (!strcmp(n, "red")) return g->roulette_bet == 0 ? LM_SLOW : LM_OFF;
    if (!strcmp(n, "black")) return g->roulette_bet == 1 ? LM_SLOW : LM_OFF;
    if (!strcmp(n, "toy")) {
        for (int i = 0; i < g->nfeat; i++) {
            const pb_fstate_t *s = &g->feat[i];
            if (s->kind == FK_BASH && s->toy >= 0) return g->ph->toy[s->toy].up ? LM_SLOW : LM_OFF;
        }
    }
    return -1;
}

bool pb_game_video(const pb_game_t *g)
{
    return g->video.on;
}

void pb_game_buttons(pb_game_t *g, bool lp, bool rp, bool lh, bool rh)
{
    g->in_lp |= lp;
    g->in_rp |= rp;
    g->in_lh = lh;
    g->in_rh = rh;
}

static void on_ramp(pb_game_t *g, int obj)
{
    (void)obj;
    g->ramps++;
    g->bonus += 2000;
    points(g, 20000);
    pb_game_sound(g, SND_RAMP);
    g->combo_t = COMBO_S;
    mission_goal(g, GL_RAMP);
    if (g->multiball) {
        jackpot(g);
    } else if (g->lock_lit && !feat_of(g, FK_LOCKPEN)) {
        g->locks++;
        g->lock_lit = false;
        g->targets = 0;
        if (g->locks >= 2) {
            start_multiball(g, false);
        } else {
            char b[32];
            snprintf(b, sizeof b, _("BOLA %d TRABADA"), g->locks);
            pb_game_say(g, TX(g, "lock", _("BOLA TRABADA")), b, 2.0f, false);
            pb_game_sound(g, SND_LOCK);
        }
    }
    if (g->ramps == 5 && !g->extra_given) {
        g->extra_lit = true;
        pb_game_say(g, _("BOLA EXTRA"), _("TIRÁ AL EMBUDO"), 2.0f, false);
    }
}

static void on_saucer(pb_game_t *g, int obj, int ball)
{
    points(g, 5000);
    g->bonus += 1000;
    pb_game_sound(g, SND_SAUCER);
    mission_goal(g, GL_SAUCER);
    g->ph->saucer_hold = 1.2f;
    if (feat_saucer(g, obj, ball)) return;
    pb_ball_t *b = ball >= 0 ? &g->ph->ball[ball] : NULL;
    if (b && b->hold < 100.0f) b->hold = 1.2f;
    if (g->t->obj[obj].role != RL_SAUCER) return;
    if (g->extra_lit) {
        g->extra_lit = false;
        g->extra_given = true;
        g->extra_balls++;
        pb_game_say(g, _("¡BOLA EXTRA!"), NULL, 2.0f, true);
        pb_game_sound(g, SND_EXTRA);
        if (b) b->hold = 2.0f;
    }
    if (g->multiball) {
        jackpot(g);
    } else if (g->mission_lit >= 0) {
        mission_start(g, g->mission_lit);
        if (b) b->hold = 2.4f;
    }
}

static void on_roll(pb_game_t *g, int obj, int ball)
{
    const pb_obj_t *o = &g->t->obj[obj];
    (void)ball;
    switch (o->role) {
    case RL_LANE: {
        int i = o->index;
        pb_game_sound(g, SND_LANE);
        points(g, 1000);
        g->bonus += 500;
        if (g->skill_lane >= 0) {
            if (i == g->skill_lane && g->skill_t < SKILL_S) {
                points(g, 50000);
                pb_game_say(g, _("¡TIRO PERFECTO!"), "50.000", 2.0f, false);
                pb_game_sound(g, SND_SKILL);
                big_flash(g);
            }
            g->skill_lane = -1;
        }
        mission_goal(g, GL_LANE);
        if (i < 3) g->lanes |= (uint8_t)(1 << i);
        if ((g->lanes & 7) == 7) {
            g->lanes = 0;
            points(g, 25000);
            if (g->bonus_x < 5) {
                g->bonus_x++;
                char b[24];
                snprintf(b, sizeof b, _("BONUS x%d"), g->bonus_x);
                pb_game_say(g, b, NULL, 1.6f, true);
            }
            pb_game_sound(g, SND_LANES);
        }
        break;
    }
    case RL_INLANE:
        points(g, 1000);
        pb_game_sound(g, SND_LANE);
        g->inlanes |= (uint8_t)(1 << (o->index & 1));
        if ((g->inlanes & 3) == 3) {
            g->inlanes = 0;
            if (!g->ph->kickback_lit) {
                g->ph->kickback_lit = true;
                pb_game_say(g, _("KICKBACK LISTO"), NULL, 1.4f, false);
            }
        }
        break;
    case RL_OUTLANE:
        points(g, 5000);
        pb_game_sound(g, SND_LANE);
        break;
    case RL_ORBIT: {
        int s = o->index & 1;
        g->orbit_t[s] = g->clock;
        float other = g->orbit_t[s ^ 1];
        if (other > 0 && g->clock - other < ORBIT_S) {
            g->orbit_t[0] = g->orbit_t[1] = 0;
            uint32_t p = 15000;
            if (g->combo_t > 0) {
                p *= 3;
                pb_game_say(g, _("¡COMBO!"), NULL, 1.2f, true);
            }
            points(g, p);
            g->bonus += 1500;
            if (g->spin_x < 4) g->spin_x++;
            pb_game_sound(g, SND_ORBIT);
            mission_goal(g, GL_ORBIT);
            g->combo_t = 0;
        }
        break;
    }
    default:
        break;
    }
}

static void on_hit(pb_game_t *g, int obj)
{
    const pb_obj_t *o = &g->t->obj[obj];
    switch (o->type) {
    case OT_BUMPER:
        g->bumper_hits++;
        if (g->bumper_hits % 25 == 0 && g->bumper_level < 4) {
            g->bumper_level++;
            pb_game_say(g, _("BUMPERS MÁS FUERTES"), NULL, 1.2f, false);
        }
        points(g, 1000u * (uint32_t)(1 + g->bumper_level));
        g->bonus += 100;
        flash_obj(g, obj, 0.12f);
        pb_game_sound(g, SND_BUMPER);
        mission_goal(g, GL_BUMPER);
        break;
    case OT_SLING:
        points(g, 110);
        flash_obj(g, obj, 0.1f);
        pb_game_sound(g, SND_SLING);
        mission_goal(g, GL_SLING);
        break;
    case OT_DROP: {
        points(g, 2500);
        g->bonus += 500;
        pb_game_sound(g, SND_DROP);
        mission_goal(g, GL_DROP);
        /* the whole bank? */
        bool all = true;
        for (int i = 0; i < g->t->nobj; i++) {
            const pb_obj_t *q = &g->t->obj[i];
            if (q->type == OT_DROP && q->bank == o->bank && !g->ph->down[i]) all = false;
        }
        if (all) {
            points(g, 25000);
            pb_game_sound(g, SND_BANK);
            light_mission(g);
            /* the bank comes back up after a moment (pb_game_update) */
            snprintf(g->bank_name, sizeof g->bank_name, "%s", o->name);
            size_t n = strlen(g->bank_name);
            while (n && g->bank_name[n - 1] >= '0' && g->bank_name[n - 1] <= '9') g->bank_name[--n] = 0;
            g->bank_t = 1.0f;
        }
        break;
    }
    case OT_TARGET:
        points(g, 5000);
        g->bonus += 500;
        pb_game_sound(g, SND_TARGET);
        flash_obj(g, obj, 0.2f);
        mission_goal(g, GL_TARGET);
        g->targets |= 1u << (o->index & 31);
        if (g->t->role_n[RL_TARGET] > 0 &&
            g->targets == (1u << g->t->role_n[RL_TARGET]) - 1) {
            g->targets = 0;
            if (g->multiball) {
                g->jackpot += 25000;
                pb_game_say(g, _("JACKPOT MÁS GRANDE"), NULL, 1.4f, false);
            } else if (!g->lock_lit) {
                g->lock_lit = true;
                pb_game_say(g, _("TRABA LISTA"), _("TIRÁ A LA RAMPA"), 2.0f, false);
            }
        }
        break;
    case OT_POST:
        pb_game_sound(g, SND_POST);
        break;
    default:
        break;
    }
}

static void drained(pb_game_t *g)
{
    /* balls locked in a toy are not in play */
    int kept = pb_phys_kept(g->ph);
    int left = pb_phys_balls(g->ph) - kept + g->to_serve;
    if (g->ball_save > 0 && !g->tilted) {
        /* saved: the same ball back, launched by itself */
        g->to_serve++;
        g->mb_serve_t = 0.6f;
        if (!g->multiball) {
            pb_game_say(g, _("¡SALVADA!"), NULL, 1.5f, true);
            pb_game_sound(g, SND_SAVE);
        }
        return;
    }
    if (left >= 2) return;
    if (g->multiball && left == 1) {
        g->multiball = false;
        g->wizard = false;
        pb_game_say(g, _("FIN DE LA MULTIBOLA"), NULL, 1.5f, false);
        return;
    }
    if (left > 0) return;
    /* the last ball: the bonus, then the next one or the end */
    g->multiball = g->wizard = false;
    g->mission = -1;
    g->state = GS_BONUS;
    g->end_t = 0;
    g->bonus_shown = 0;
    pb_game_sound(g, SND_DRAIN);
    pb_game_say(g, TX(g, "drain", _("BOLA PERDIDA")), NULL, 1.5f, false);
}

static void lamps(pb_game_t *g, float dt)
{
    const pb_table_t *t = g->t;
    bool slow = ((int)(g->clock * 2.5f)) & 1;
    bool fast = ((int)(g->clock * 7.0f)) & 1;
    for (int i = 0; i < t->nlamp; i++) {
        const char *n = t->lamp[i].name;
        int m = LM_OFF;
        int fm = g->state == GS_PLAY ? feat_lamp(g, n) : -1;
        if (fm >= 0) {
            m = fm;
        } else if (g->state == GS_ATTRACT || g->state == GS_OVER) {
            /* a chase round the table */
            int k = (int)(g->attract_t * 12.0f);
            m = ((i + k) % 6) < 2 ? LM_ON : LM_OFF;
        } else if (!strncmp(n, "lane", 4)) {
            int k = n[4] - '1';
            if (k == g->skill_lane && !g->launched) m = LM_FAST;
            else if (k == g->skill_lane && g->skill_t < SKILL_S) m = LM_FAST;
            else m = (g->lanes >> k) & 1 ? LM_ON : LM_OFF;
        } else if (!strcmp(n, "inL") || !strcmp(n, "inR")) {
            m = (g->inlanes >> (n[2] == 'R')) & 1 ? LM_ON : LM_OFF;
        } else if (!strcmp(n, "kickback") || !strcmp(n, "outL")) {
            m = g->ph->kickback_lit ? LM_ON : LM_OFF;
        } else if (!strcmp(n, "outR")) {
            m = g->ball_save > 0 ? LM_SLOW : LM_OFF;
        } else if (!strncmp(n, "drop", 4)) {
            int o = pb_table_find_obj(t, n);
            m = (o >= 0 && !g->ph->down[o]) ? LM_ON : LM_OFF;
            if (g->mission >= 0 && t->mission[g->mission].goal == GL_DROP) m = LM_FAST;
        } else if (!strncmp(n, "target", 6)) {
            int o = pb_table_find_obj(t, n);
            m = (o >= 0 && (g->targets >> (t->obj[o].index & 31)) & 1) ? LM_ON : LM_SLOW;
            if (g->mission >= 0 && t->mission[g->mission].goal == GL_TARGET) m = LM_FAST;
        } else if (!strcmp(n, "lock")) {
            m = g->lock_lit ? LM_FAST : (g->locks ? LM_ON : LM_OFF);
        } else if (!strncmp(n, "ramp", 4)) {
            m = (g->lock_lit || g->multiball) ? LM_FAST : LM_OFF;
            if (g->mission >= 0 && t->mission[g->mission].goal == GL_RAMP) m = LM_FAST;
            if (m == LM_OFF && g->ramps >= 3 && !g->extra_given) m = LM_SLOW;
        } else if (!strcmp(n, "saucer")) {
            if (g->mission_lit >= 0 || g->extra_lit || g->multiball) m = LM_FAST;
            else if (g->mission >= 0) m = LM_ON;
        } else if (!strncmp(n, "mission", 7)) {
            int k = n[7] - '1';
            if (k == g->mission) m = LM_FAST;
            else if (k == g->mission_lit) m = LM_SLOW;
            else if ((g->missions_done >> k) & 1) m = LM_ON;
        } else if (n[0] == 'x' && n[1] >= '2' && n[1] <= '9') {
            m = g->bonus_x >= n[1] - '0' ? LM_ON : LM_OFF;
        } else if (!strcmp(n, "extraball")) {
            m = g->extra_lit ? LM_FAST : LM_OFF;
        } else if (!strcmp(n, "shootagain")) {
            m = g->ball_save > 0 ? LM_FAST : g->extra_balls ? LM_ON : LM_OFF;
        } else if (!strcmp(n, "jackpot")) {
            m = g->multiball ? LM_FAST : LM_OFF;
        } else if (!strcmp(n, "spinner")) {
            m = g->spin_x > 1 ? (g->spin_x > 2 ? LM_FAST : LM_ON) : LM_OFF;
        } else if (!strncmp(n, "orbit", 5)) {
            m = g->combo_t > 0 ? LM_FAST : LM_OFF;
            if (g->mission >= 0 && t->mission[g->mission].goal == GL_ORBIT) m = LM_FAST;
        } else if (!strncmp(n, "bumper", 6)) {
            m = g->bumper_level > 0 ? LM_ON : LM_OFF;
            if (g->mission >= 0 && t->mission[g->mission].goal == GL_BUMPER) m = LM_SLOW;
        }
        if (g->lamp_flash[i] > 0) {
            g->lamp_flash[i] -= dt;
            m = LM_ON;
        }
        if (g->tilted && g->state == GS_PLAY) m = LM_OFF;
        g->lamp_mode[i] = (uint8_t)m;
        g->lamp_on[i] = m == LM_ON || (m == LM_SLOW && slow) || (m == LM_FAST && fast);
    }
}

void pb_game_update(pb_game_t *g, float dt)
{
    pb_phys_t *ph = g->ph;
    g->clock += dt;
    g->attract_t += dt;
    g->scroll++;
    for (int i = 0; i < ph->nev; i++) {
        const pb_event_t *e = &ph->ev[i];
        switch (e->kind) {
        case EV_HIT:
            on_hit(g, e->obj);
            feat_obj(g, e->obj);
            break;
        case EV_ROLL:
            on_roll(g, e->obj, e->ball);
            feat_obj(g, e->obj);
            break;
        case EV_SPIN:
            points(g, 200u * (uint32_t)g->spin_x);
            pb_game_sound(g, SND_SPINNER);
            mission_goal(g, GL_SPINNER);
            break;
        case EV_SAUCER:
            on_saucer(g, e->obj, e->ball);
            break;
        case EV_TOY_HIT:
            feat_toy_hit(g, e->obj);
            break;
        case EV_DISC_OUT:
            feat_disc_out(g, e->obj, e->speed);
            break;
        case EV_CANNON_IN:
            pb_game_sound(g, SND_CANNON);
            pb_game_say(g, TX(g, "cannonin", _("¡APUNTÁ!")), _("LANZÁ PARA DISPARAR"), 2.0f, false);
            break;
        case EV_CANNON_FIRE:
            pb_game_sound(g, SND_CANNON);
            g->cannon_t = 2.5f;
            points(g, 10000);
            break;
        case EV_MAGNET_GRAB: {
            pb_fstate_t *m = feat_of(g, FK_MAGNET);
            pb_game_sound(g, SND_MAGNET);
            points(g, m ? m->points : 50000);
            pb_game_say(g, TX(g, m && m->text[0] ? m->text : "magnet", _("¡ATRAPADA!")), NULL, 1.4f, true);
            mission_goal(g, GL_TOY);
            break;
        }
        case EV_EJECT:
            pb_game_sound(g, SND_EJECT);
            break;
        case EV_RAMP_DONE:
            on_ramp(g, e->obj);
            break;
        case EV_RAMP_FAIL:
            pb_game_sound(g, SND_RAMP_FAIL);
            break;
        case EV_KICKBACK:
            pb_game_sound(g, SND_KICKBACK);
            pb_game_say(g, _("KICKBACK"), NULL, 1.0f, true);
            break;
        case EV_DRAIN:
            if (g->state == GS_PLAY) drained(g);
            break;
        case EV_FLIP:
            pb_game_sound(g, SND_FLIPPER);
            break;
        case EV_WALL:
            pb_game_sound(g, SND_WALL);
            break;
        case EV_LAUNCH:
            pb_game_sound(g, SND_LAUNCH);
            if (!g->launched) {
                g->launched = true;
                g->ball_save = BALL_SAVE_S;
                g->skill_t = 0;
            }
            break;
        default:
            break;
        }
    }
    ph->nev = 0;

    if (g->state == GS_PLAY) {
        if (g->launched && g->ball_save > 0 && pb_phys_on_plunger(ph) < 0) g->ball_save -= dt;
        if (g->skill_lane >= 0 && g->launched) {
            g->skill_t += dt;
            if (g->skill_t > SKILL_S) g->skill_lane = -1;
        }
        if (g->combo_t > 0) g->combo_t -= dt;
        if (g->tilt > 0) g->tilt -= dt * 0.55f;
        if (g->tilt < 0) g->tilt = 0;
        if (g->mission >= 0) {
            g->mission_t -= dt;
            if (g->mission_t <= 0) {
                pb_game_say(g, _("SE ACABÓ EL TIEMPO"), NULL, 1.6f, false);
                g->mission = -1;
            }
        }
        if (g->need_serve) {
            if (pb_phys_serve(ph) >= 0) g->need_serve = false;
        }
        /* multiball's balls, and saved ones, come out one at a time and go
         * by themselves */
        if (g->to_serve > 0 || g->auto_on_plunger) {
            g->mb_serve_t -= dt;
            if (g->mb_serve_t <= 0) {
                if (g->auto_on_plunger) {
                    pb_phys_launch(ph, 0.93f);
                    g->auto_on_plunger = false;
                    g->mb_serve_t = 0.5f;
                } else if (pb_phys_serve(ph) >= 0) {
                    g->to_serve--;
                    g->auto_on_plunger = true;
                    g->mb_serve_t = 0.35f;
                } else {
                    g->mb_serve_t = 0.2f;
                }
            }
        }
        if (g->bank_t > 0) {
            g->bank_t -= dt;
            if (g->bank_t <= 0) pb_phys_drops_up(ph, g->bank_name);
        }
        feat_update(g, dt);
    } else if (g->state == GS_BONUS) {
        g->end_t += dt;
        uint32_t total = g->tilted ? 0 : g->bonus * (uint32_t)g->bonus_x;
        if (g->end_t > 1.2f && g->bonus_shown < total) {
            uint32_t step = total / 40 + 1;
            g->bonus_shown += step;
            if (g->bonus_shown > total) g->bonus_shown = total;
            if (((int)(g->end_t * 20)) % 2 == 0) pb_game_sound(g, SND_BONUS);
        }
        if (g->end_t > 3.6f) {
            g->score += total;
            if (g->extra_balls > 0) {
                g->extra_balls--;
                new_ball(g);
                g->state = GS_PLAY;
                pb_game_say(g, _("TIRÁ DE NUEVO"), NULL, 1.6f, true);
            } else if (g->ball < PB_BALLS) {
                g->ball++;
                new_ball(g);
                g->state = GS_PLAY;
            } else {
                g->state = GS_OVER;
                g->end_t = 0;
                g->attract_t = 0;
                if (g->score > g->best) {
                    g->best = g->score;
                    g->new_best = true;
                }
                pb_game_sound(g, SND_GAMEOVER);
            }
        }
    } else if (g->state == GS_OVER) {
        g->end_t += dt;
    }
    lamps(g, dt);
    if (g->nmsg) {
        g->msg_t += dt;
        if (g->msg_t >= g->msg[0].secs) {
            memmove(&g->msg[0], &g->msg[1], sizeof(pb_msg_t) * (PB_MSG_MAX - 1));
            g->nmsg--;
            g->msg_t = 0;
        }
    }
}

/* ---- the DMD ------------------------------------------------------------ */

void pb_game_dmd(pb_game_t *g, pb_dmd_t *d)
{
    dmd_clear(d);
    char num[24], line[64];
    int sh = g->scroll / 2;
    if (g->video.on) {
        pb_video_draw(&g->video, d, g->lang);
        return;
    }
    if (g->letters_show > 0 && g->letters_feat >= 0 && !g->nmsg) {
        const pb_fstate_t *s = &g->feat[g->letters_feat];
        int n = (int)strlen(s->word);
        int w = n * 18 - 4, x = (DMD_W - w) / 2;
        for (int i = 0; i < n; i++) {
            char c[2] = { s->word[i], 0 };
            bool lit = i < s->nobjs && ((s->mask >> i) & 1);
            dmd_text(d, x + i * 18, 9, c, 2, lit ? 15 : 3);
        }
        return;
    }
    if (g->nmsg) {
        const pb_msg_t *m = &g->msg[0];
        bool vis = !m->blink || (((int)(g->msg_t * 6)) & 1);
        if (vis) {
            if (m->big || !m->b[0]) {
                int w = dmd_text_w(m->a, 2);
                if (m->big && w <= DMD_W) dmd_text_c(d, 9, m->a, 2, 15, sh);
                else dmd_text_c(d, m->b[0] ? 6 : 12, m->a, 1, 15, sh);
                if (m->b[0]) dmd_text_c(d, 19, m->b, 1, 12, sh);
            } else {
                dmd_text_c(d, 5, m->a, 1, 15, sh);
                int w2 = dmd_text_w(m->b, 2);
                if (w2 <= DMD_W) dmd_text_c(d, 15, m->b, 2, 12, sh);
                else dmd_text_c(d, 19, m->b, 1, 12, sh);
            }
        }
        dmd_frame(d, 0, 0, DMD_W, DMD_H, 3);
        return;
    }
    switch (g->state) {
    case GS_ATTRACT:
        dmd_text_c(d, 4, g->t->name[g->lang][0] ? g->t->name[g->lang] : g->t->name[0], 1, 15, sh);
        dmd_number(num, sizeof num, g->best);
        snprintf(line, sizeof line, _("RÉCORD %s"), num);
        dmd_text_c(d, 14, line, 1, 9, sh);
        dmd_text_c(d, 23, _("LANZÁ PARA JUGAR"), 1, ((int)(g->attract_t * 2) & 1) ? 15 : 6, sh);
        break;
    case GS_PLAY: {
        dmd_number(num, sizeof num, g->score);
        int w = dmd_text_w(num, 2);
        dmd_text(d, w <= DMD_W ? (DMD_W - w) / 2 : 0, 2, num, w <= DMD_W ? 2 : 1, 15);
        if (g->mission >= 0) {
            const pb_mission_t *mi = &g->t->mission[g->mission];
            const char *nm = mi->name[g->lang][0] ? mi->name[g->lang] : mi->name[0];
            snprintf(line, sizeof line, "%s  %d  0:%02d", nm, g->mission_left, (int)g->mission_t);
            dmd_text_c(d, 21, line, 1, 11, sh);
        } else if (pb_game_wants_launch(g) && !g->launched) {
            snprintf(line, sizeof line, _("BOLA %d"), g->ball);
            dmd_text_c(d, 21, line, 1, ((int)(g->clock * 2) & 1) ? 13 : 7, 0);
        } else {
            snprintf(line, sizeof line, _("BOLA %d"), g->ball);
            dmd_text(d, 1, 25, line, 1, 8);
            if (g->bonus_x > 1) {
                snprintf(line, sizeof line, "x%d", g->bonus_x);
                dmd_text(d, DMD_W - dmd_text_w(line, 1) - 1, 25, line, 1, 8);
            }
            if (g->multiball) dmd_text_c(d, 25, _("MULTIBOLA"), 1, ((int)(g->clock * 3) & 1) ? 12 : 6, 0);
            else if (g->ph->kickback_lit) dmd_text_c(d, 25, "·", 1, 5, 0);
        }
        break;
    }
    case GS_BONUS: {
        uint32_t total = g->tilted ? 0 : g->bonus * (uint32_t)g->bonus_x;
        dmd_number(num, sizeof num, g->bonus);
        snprintf(line, sizeof line, _("BONUS %s x%d"), num, g->bonus_x);
        dmd_text_c(d, 4, line, 1, 12, sh);
        dmd_number(num, sizeof num, g->end_t > 1.2f ? g->bonus_shown : total);
        dmd_text_c(d, 15, num, 2, 15, 0);
        break;
    }
    case GS_OVER:
        dmd_text_c(d, 3, _("FIN DEL JUEGO"), 1, 15, sh);
        dmd_number(num, sizeof num, g->score);
        dmd_text_c(d, 12, num, 2, 15, 0);
        if (g->new_best && (((int)(g->end_t * 3)) & 1)) dmd_frame(d, 0, 0, DMD_W, DMD_H, 15);
        break;
    }
}
