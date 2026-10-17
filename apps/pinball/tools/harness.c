/*
 * PINBALL - the physics and the rules without a screen, for tuning a
 * table's geometry: a bot plays thousands of balls and the counts say
 * whether ramps can be made, orbits go round, the saucer catches, and
 * whether a ball ever sticks or leaves the table.
 *
 *     python3 apps/pinball/tools/pbtable_def.py zombis > /tmp/z.txt
 *     cc -O2 -DAOS_SIM -I... tools/harness.c main/pb_*.c -o harness   (tools/harness.sh)
 *     ./harness /tmp/z.txt [balls] [heat.pgm]
 */
#include "pb_game.h"
#include "pb_phys.h"
#include "pb_table.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aos_hal_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
const char *aos_i18n_current(void) { return "es"; }
const char *aos_tr(const char *s) { return s; }

static uint32_t rng = 12345;
static float frand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (float)(rng & 0xFFFF) / 65535.0f;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    static char txt[1 << 20];
    size_t n = fread(txt, 1, sizeof txt - 1, f);
    txt[n] = 0;
    fclose(f);
    int balls = argc > 2 ? atoi(argv[2]) : 300;
    float skill = getenv("SKILL") ? (float)atof(getenv("SKILL")) : 0.35f;
    static pb_table_t t;
    if (!pb_table_parse(&t, txt, false)) return 2;
    static pb_phys_t ph;
    static pb_game_t g;
    pb_phys_init(&ph, &t);
    pb_game_init(&g, &t, &ph, 0);
    static uint32_t heat[260][130];
    int ev[16] = { 0 };
    int roles[RL_N] = { 0 };
    int out = 0, drains = 0, games = 0;
    int objc[PB_MAX_OBJ] = { 0 };
    int last_role[PB_MAX_BALLS] = { 0 };
    int drain_by[RL_N] = { 0 };
    static float hist[PB_MAX_BALLS][64][2];
    int hk = 0;
    static int from[60][30];
    FILE *traj = getenv("TRAJ") ? fopen(getenv("TRAJ"), "w") : NULL;
    int ntraj = 0;
    double ball_time = 0, t_now = 0;
    float hold[2] = { 0, 0 };
    const float dt = 1.0f / 60.0f;
    double longest = 0, cur = 0;
    static float stuck[4096][2];
    static int snd[SND_N];
    bool counted = false;
    double score_sum = 0;
    uint32_t score_max = 0;
    int nstuck = 0;
    while (drains < balls) {
        if (g.state == GS_ATTRACT || g.state == GS_OVER) {
            pb_game_start(&g);
            games++;
        }
        /* the bot: a flipper goes up when a ball comes near it */
        for (int s = 0; s < 2; s++) {
            const pb_flipdef_t *fd = &t.flip[s];
            bool want = false;
            for (int i = 0; i < PB_MAX_BALLS; i++) {
                const pb_ball_t *b = &ph.ball[i];
                if (b->state != BS_FREE) continue;
                /* a fair player: the ball over the flipper's outer half,
                 * coming down, and a reaction that is not always perfect */
                float tx = fd->x + cosf(fd->rest) * fd->len * 0.65f, ty = fd->y + sinf(fd->rest) * fd->len * 0.65f;
                float dx = b->x - tx, dy = b->y - ty;
                if (dx * dx + dy * dy < 42 * 42 && b->vy > 0 && frand() < skill) want = true;
            }
            if (want && hold[s] <= 0) hold[s] = 0.12f + frand() * 0.25f;
            hold[s] -= dt;
            pb_phys_flipper(&ph, s, hold[s] > 0);
        }
        if (pb_game_wants_launch(&g)) pb_phys_launch(&ph, 0.5f + frand() * 0.5f);
        uint32_t k0 = ph.kicks;
        pb_phys_step(&ph, dt);
        if (ph.kicks != k0 && nstuck < 4096) {
            stuck[nstuck][0] = ph.kick_x;
            stuck[nstuck][1] = ph.kick_y;
            nstuck++;
        }
        for (int i = 0; i < ph.nev; i++) {
            pb_event_t *e = &ph.ev[i];
            if (e->kind < 16) ev[e->kind]++;
            if (e->obj >= 0 && (e->kind == EV_HIT || e->kind == EV_ROLL)) {
                roles[t.obj[e->obj].role]++;
                objc[e->obj]++;
                if (e->ball >= 0) last_role[e->ball] = t.obj[e->obj].role;
                if (e->ball >= 0 && t.obj[e->obj].role == RL_OUTLANE && traj && ntraj < 60) {
                    /* the last second of it, for trajview.py */
                    for (int k = 1; k <= 64; k++) {
                        float *q = hist[e->ball][(hk + k) % 64];
                        fprintf(traj, "%.1f %.1f ", q[0], q[1]);
                    }
                    fputc('\n', traj);
                    ntraj++;
                }
                if (e->ball >= 0 && t.obj[e->obj].role == RL_OUTLANE) {
                    /* where it was 0.6 s before */
                    float *q = hist[e->ball][(hk + 64 - 36) % 64];
                    int cx = (int)(q[0] / 20), cy = (int)(q[1] / 20);
                    if (cx >= 0 && cy >= 0 && cx < 30 && cy < 60) from[cy][cx]++;
                }
            }
            if (e->kind == EV_DRAIN) {
                drain_by[e->ball >= 0 ? last_role[e->ball] : 0]++;
                drains++;
                if (cur > longest) longest = cur;
                ball_time += cur;
                cur = 0;
            }
        }
        pb_game_update(&g, dt);
        for (int i = 0; i < g.nsnd; i++) snd[g.snd[i]]++;
        g.nsnd = 0;
        if (g.state == GS_OVER && !counted) {
            counted = true;
            score_sum += g.score;
            if (g.score > score_max) score_max = g.score;
        } else if (g.state != GS_OVER) {
            counted = false;
        }
        t_now += dt;
        cur += dt;
        hk = (hk + 1) % 64;
        for (int i = 0; i < PB_MAX_BALLS; i++) {
            hist[i][hk][0] = ph.ball[i].x;
            hist[i][hk][1] = ph.ball[i].y;
        }
        for (int i = 0; i < PB_MAX_BALLS; i++) {
            const pb_ball_t *b = &ph.ball[i];
            if (b->state == BS_OFF) continue;
            if (b->x < -5 || b->x > t.w + 5 || b->y < -5) out++;
            int hx = (int)(b->x / 4), hy = (int)(b->y / 4);
            if (hx >= 0 && hy >= 0 && hx < 130 && hy < 260) heat[hy][hx]++;
        }
    }
    printf("%d balls in %d games, %.1f s a ball (longest %.0f s), %.0f s played\n", drains, games, ball_time / drains,
           longest, t_now);
    printf("ramps %d made / %d failed (%d in), saucer %d, kickbacks %d, spins %d, stuck kicks %u, outside %d\n",
           ev[EV_RAMP_DONE], ev[EV_RAMP_FAIL], ev[EV_RAMP_IN], ev[EV_SAUCER], ev[EV_KICKBACK], ev[EV_SPIN], ph.kicks,
           out);
    static const char *RN[RL_N] = { "none", "lane", "inlane", "outlane", "bumper", "sling", "drop", "target",
                                    "spinner", "saucer", "ramp", "orbit", "kickback", "post" };
    for (int i = 1; i < RL_N; i++) {
        if (roles[i]) printf("  %-8s %d\n", RN[i], roles[i]);
    }
    /* where balls stuck, in 20 mm cells, the worst first */
    static int cells[60][30];
    for (int i = 0; i < nstuck; i++) {
        int cx = (int)(stuck[i][0] / 20), cy = (int)(stuck[i][1] / 20);
        if (cx >= 0 && cy >= 0 && cx < 30 && cy < 60) cells[cy][cx]++;
    }
    for (int k = 0; k < 6; k++) {
        int bx = -1, by = -1, bv = 0;
        for (int y = 0; y < 60; y++)
            for (int x = 0; x < 30; x++)
                if (cells[y][x] > bv) {
                    bv = cells[y][x];
                    bx = x;
                    by = y;
                }
        if (bv < 2) break;
        printf("  stuck %3d times near (%d, %d)\n", bv, bx * 20 + 10, by * 20 + 10);
        cells[by][bx] = 0;
    }
    printf("drains after: outlane %d, inlane %d, sling %d, post %d, other %d\n", drain_by[RL_OUTLANE],
           drain_by[RL_INLANE], drain_by[RL_SLING], drain_by[RL_POST],
           drains - drain_by[RL_OUTLANE] - drain_by[RL_INLANE] - drain_by[RL_SLING] - drain_by[RL_POST]);
    for (int i = 0; i < t.nobj; i++)
        if (t.obj[i].role == RL_POST || t.obj[i].role == RL_OUTLANE) printf("  %-12s %d\n", t.obj[i].name, objc[i]);
    for (int k = 0; k < 8; k++) {
        int bx = -1, by = -1, bv = 0;
        for (int y = 0; y < 60; y++)
            for (int x = 0; x < 30; x++)
                if (from[y][x] > bv) {
                    bv = from[y][x];
                    bx = x;
                    by = y;
                }
        if (bv < 3) break;
        printf("  to the outlanes %3d times from (%d, %d)\n", bv, bx * 20 + 10, by * 20 + 10);
        from[by][bx] = 0;
    }
    {
        static const char *GN[GL_N] = { "bumper", "sling", "drop", "target", "spinner", "ramp", "orbit", "lane",
                                        "saucer", "any" };
        printf("a minute of play gives:");
        static const int RG[GL_N] = { RL_BUMPER, RL_SLING, RL_DROP, RL_TARGET, RL_SPINNER, RL_RAMP, RL_ORBIT,
                                      RL_LANE, RL_SAUCER, 0 };
        for (int i = 0; i < GL_N - 1; i++) {
            double v = i == GL_RAMP ? ev[EV_RAMP_DONE] : i == GL_SAUCER ? ev[EV_SAUCER] : i == GL_SPINNER ? ev[EV_SPIN]
                     : i == GL_ORBIT ? snd[SND_ORBIT] : roles[RG[i]];
            printf(" %s %.1f", GN[i], v * 60.0 / t_now);
        }
        printf("\n");
    }
    printf("missions %d started / %d done, locks %d, multiballs %d, jackpots %d, extra balls %d, skill shots %d, "
           "tilts %d, saves %d\n", snd[SND_MISSION], snd[SND_MISSION_DONE], snd[SND_LOCK], snd[SND_MULTIBALL],
           snd[SND_JACKPOT], snd[SND_EXTRA], snd[SND_SKILL], snd[SND_TILT], snd[SND_SAVE]);
    printf("score: %.0f a game, best %u\n", score_sum / (games > 1 ? games - 1 : 1), score_max);
    if (argc > 3) {
        FILE *o = fopen(argv[3], "wb");
        fprintf(o, "P5 130 260 255\n");
        uint32_t mx = 1;
        for (int y = 0; y < 260; y++)
            for (int x = 0; x < 130; x++)
                if (heat[y][x] > mx) mx = heat[y][x];
        for (int y = 0; y < 260; y++)
            for (int x = 0; x < 130; x++) fputc((int)(255 * sqrt((double)heat[y][x] / mx)), o);
        fclose(o);
    }
    return 0;
}
