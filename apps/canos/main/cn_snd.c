/*
 * CAÑOS - the sound (see cn_snd.h)
 */
#include "cn_snd.h"
#include "cn_world.h"

#include "aos_hal.h"

#include <math.h>
#include <string.h>

#define RATE        16000
#define TARGET      1600            /* samples to keep queued: 100 ms */
#define VOICES      12

enum { V_OFF = 0, V_NOISE, V_TONE, V_SQUARE };

typedef struct {
    uint8_t  kind;
    int32_t  t, dur;                /* samples */
    int32_t  amp, attack;
    uint32_t phase, step, step_end; /* 16.16 over a 256-entry sine */
    int32_t  lp, lp_k, hp;
    uint8_t  hp_on;
    uint32_t seed;
} voice_t;

static voice_t  s_v[VOICES];
static bool     s_open, s_on = true;
static uint32_t s_rng = 0x2545F491u;
static int16_t  s_sin[256];

/* the liquid */
static int      s_world;
static int32_t  s_level, s_level_t;     /* 0..4096, now and wanted */
static int32_t  s_nlp, s_nlp2, s_nhp;
static int32_t  s_gurgle;               /* samples to the next gurgle */

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t hz_step(int hz)
{
    return (uint32_t)hz * 1049u;        /* 2^24 / 16000: 256 << 16 per cycle */
}

static void tables(void)
{
    for (int i = 0; i < 256; i++) {
        s_sin[i] = (int16_t)(sinf((float)i * (6.2831853f / 256.0f)) * 32767.0f);
    }
}

static voice_t *voice_get(void)
{
    for (int i = 0; i < VOICES; i++) {
        if (s_v[i].kind == V_OFF) return &s_v[i];
    }
    int best = 0, left = 0x7FFFFFFF;
    for (int i = 0; i < VOICES; i++) {
        int l = s_v[i].dur - s_v[i].t;
        if (l < left) {
            left = l;
            best = i;
        }
    }
    return &s_v[best];
}

static void noise(int ms, int amp, int attack_ms, int lp_k, bool hp, int delay_ms)
{
    voice_t *v = voice_get();
    memset(v, 0, sizeof(*v));
    v->kind = V_NOISE;
    v->dur = ms * RATE / 1000;
    v->amp = amp;
    v->attack = attack_ms * RATE / 1000;
    v->lp_k = lp_k;
    v->hp_on = hp;
    v->seed = rnd() | 1;
    v->t = -(delay_ms * RATE / 1000);
}

static void tone(int hz, int hz_end, int ms, int amp, int attack_ms, int delay_ms, uint8_t kind)
{
    voice_t *v = voice_get();
    memset(v, 0, sizeof(*v));
    v->kind = kind;
    v->dur = ms * RATE / 1000;
    v->amp = amp;
    v->attack = attack_ms * RATE / 1000;
    v->step = hz_step(hz);
    v->step_end = hz_step(hz_end);
    v->t = -(delay_ms * RATE / 1000);
}

static inline int32_t env(const voice_t *v)
{
    if (v->t < v->attack) return v->amp * v->t / (v->attack ? v->attack : 1);
    int32_t left = v->dur - v->t, span = v->dur - v->attack;
    if (span <= 0) return 0;
    int32_t f = (left << 10) / span;
    return ((v->amp * f) >> 10) * f >> 10;
}

static int32_t voice_sample(voice_t *v)
{
    int32_t e = env(v), s = 0;
    switch (v->kind) {
    case V_NOISE: {
        v->seed ^= v->seed << 13;
        v->seed ^= v->seed >> 17;
        v->seed ^= v->seed << 5;
        int32_t n = (int32_t)(v->seed & 0xFFFF) - 32768;
        v->lp += ((n - v->lp) * v->lp_k) >> 8;
        s = v->lp;
        if (v->hp_on) {
            int32_t hp = s - v->hp;
            v->hp += (s - v->hp) >> 4;
            s = hp;
        }
        break;
    }
    case V_TONE:
    case V_SQUARE: {
        int32_t k = v->dur ? (v->t << 8) / v->dur : 0;
        uint32_t st = v->step + (uint32_t)((((int32_t)v->step_end - (int32_t)v->step) >> 8) * k);
        v->phase += st;
        s = s_sin[(v->phase >> 16) & 255];
        if (v->kind == V_SQUARE) s = s > 0 ? 14000 + (s >> 2) : -14000 + (s >> 2);
        break;
    }
    default:
        break;
    }
    v->t++;
    if (v->t >= v->dur) v->kind = V_OFF;
    return (s * e) >> 15;
}

/* ---- the liquid ---- */

/* One gurgle, the world's kind, at the current level. */
static void gurgle(void)
{
    int lv = s_level >> 4;              /* 0..256 */
    uint32_t r = rnd();
    switch (cn_world(s_world)->ground) {
    case CN_GROUND_TILES: {             /* water: a bubble's rising blip */
        int f = 380 + (int)(r % 520);
        tone(f, f * 17 / 10, 30 + (int)((r >> 10) % 50), 9 * lv, 3, 0, V_TONE);
        s_gurgle = RATE / 1000 * (25 + (int)((r >> 16) % 110));
        break;
    }
    case CN_GROUND_TREAD: {             /* fuel: a lower, rounder glug */
        int f = 240 + (int)(r % 300);
        tone(f, f * 14 / 10, 45 + (int)((r >> 10) % 60), 8 * lv, 4, 0, V_TONE);
        s_gurgle = RATE / 1000 * (40 + (int)((r >> 16) % 160));
        break;
    }
    case CN_GROUND_LAB:                 /* acid: crackling fizz */
        noise(3 + (int)(r % 5), 14 * lv, 0, 230, true, 0);
        if ((r >> 20) % 5 == 0) tone(1800 + (int)(r % 1400), 2600, 18, 4 * lv, 1, 0, V_TONE);
        s_gurgle = RATE / 1000 * (6 + (int)((r >> 16) % 30));
        break;
    case CN_GROUND_SAND: {              /* crude: slow heavy plops */
        int f = 90 + (int)(r % 90);
        tone(f + 60, f, 90 + (int)((r >> 10) % 80), 12 * lv, 6, 0, V_TONE);
        s_gurgle = RATE / 1000 * (160 + (int)((r >> 16) % 380));
        break;
    }
    case CN_GROUND_BASALT: {            /* lava: a deep blurp, and crackles */
        int f = 60 + (int)(r % 70);
        if ((r >> 24) & 1) tone(f + 40, f, 140 + (int)((r >> 10) % 120), 13 * lv, 10, 0, V_TONE);
        else noise(4, 12 * lv, 0, 200, true, 0);
        s_gurgle = RATE / 1000 * (90 + (int)((r >> 16) % 260));
        break;
    }
    default:                            /* nitrogen: boiling ticks and pops */
        noise(2 + (int)(r % 4), 12 * lv, 0, 240, true, 0);
        if ((r >> 20) % 4 == 0) tone(1200 + (int)(r % 1600), 3000, 14, 4 * lv, 1, 0, V_TONE);
        s_gurgle = RATE / 1000 * (5 + (int)((r >> 16) % 25));
        break;
    }
}

static int32_t flow_sample(void)
{
    s_level += (s_level_t - s_level) >> 10;
    if (s_level < 8) return 0;
    if (--s_gurgle <= 0) gurgle();
    int32_t n = (int32_t)(rnd() & 0xFFFF) - 32768;
    const cn_world_t *w = cn_world(s_world);
    int32_t s;
    switch (w->ground) {
    case CN_GROUND_SAND:
    case CN_GROUND_BASALT:              /* a low rumble */
        s_nlp += ((n - s_nlp) * 6) >> 8;
        s_nlp2 += ((s_nlp - s_nlp2) * 10) >> 8;
        s = s_nlp2 * 3;
        break;
    case CN_GROUND_LAB:
    case CN_GROUND_ICE:                 /* a hiss */
        s_nlp += ((n - s_nlp) * 200) >> 8;
        s = s_nlp - s_nhp;
        s_nhp += (s_nlp - s_nhp) >> 3;
        s >>= 1;
        break;
    default:                            /* a rush */
        s_nlp += ((n - s_nlp) * 40) >> 8;
        s_nlp2 += ((s_nlp - s_nlp2) * 90) >> 8;
        s = s_nlp2;
        break;
    }
    return (s * (s_level >> 2)) >> 12;
}

/* ---- effects ---- */

void cn_snd(int id)
{
    if (!s_on) return;
    if (!s_open) {
        switch (id) {
        case CN_SND_TURN:  aos_hal_beep(1400, 15); break;
        case CN_SND_PLACE: aos_hal_beep(700, 30); break;
        case CN_SND_TICK:  aos_hal_beep(1760, 40); break;
        case CN_SND_WIN:   aos_hal_beep(1047, 90); aos_hal_beep(1568, 200); break;
        case CN_SND_LOSE:  aos_hal_beep(300, 300); break;
        default: break;
        }
        return;
    }
    switch (id) {
    case CN_SND_CLICK:
        noise(14, 2600, 0, 180, true, 0);
        break;
    case CN_SND_TURN:
        noise(26, 4200, 0, 150, true, 0);
        tone(900, 700, 40, 2600, 2, 0, V_TONE);
        noise(20, 3000, 0, 120, true, 60);
        break;
    case CN_SND_PLACE:
        noise(60, 5200, 0, 40, false, 0);
        tone(180, 120, 90, 4200, 3, 0, V_TONE);
        break;
    case CN_SND_TAKE:
        noise(30, 3000, 0, 120, true, 0);
        tone(500, 900, 70, 2400, 3, 0, V_TONE);
        break;
    case CN_SND_DENY:
        tone(220, 200, 90, 3800, 2, 0, V_SQUARE);
        break;
    case CN_SND_VALVE:
        for (int i = 0; i < 4; i++) {
            tone(520 + i * 40, 600 + i * 40, 110, 2200, 30, i * 170, V_TONE);
            noise(40, 2400, 5, 160, true, i * 170 + 60);
        }
        break;
    case CN_SND_GUSH:
        noise(600, 7000, 60, 60, false, 0);
        tone(140, 90, 500, 3000, 40, 0, V_TONE);
        break;
    case CN_SND_TICK:
        tone(1760, 1760, 45, 3600, 1, 0, V_TONE);
        break;
    case CN_SND_CROSS:
        tone(1319, 1319, 80, 3800, 2, 0, V_TONE);
        tone(1760, 1760, 160, 3600, 2, 70, V_TONE);
        break;
    case CN_SND_LEAK:
        noise(900, 9000, 10, 70, false, 0);
        noise(300, 6000, 0, 200, true, 0);
        tone(400, 140, 400, 4000, 5, 0, V_TONE);
        break;
    case CN_SND_DRAIN:
        for (int i = 0; i < 6; i++) tone(700 - i * 60, 300 - i * 30, 120, 3600, 8, i * 120, V_TONE);
        break;
    case CN_SND_WIN: {
        static const int NOTES[] = { 523, 659, 784, 1047, 1319 };
        for (int i = 0; i < 5; i++) tone(NOTES[i], NOTES[i], i == 4 ? 420 : 140, 5200, 4, i * 110, V_TONE);
        break;
    }
    case CN_SND_LOSE:
        tone(440, 415, 220, 4400, 6, 0, V_SQUARE);
        tone(392, 370, 220, 4200, 6, 240, V_SQUARE);
        tone(330, 220, 520, 4400, 6, 480, V_SQUARE);
        break;
    default:
        break;
    }
}

void cn_snd_flow(int world, float level)
{
    s_world = world;
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    s_level_t = s_on ? (int32_t)(level * 4096.0f) : 0;
}

/* ---- the machine ---- */

void cn_snd_open(void)
{
    if (s_open) return;
    tables();
    memset(s_v, 0, sizeof(s_v));
    s_level = s_level_t = 0;
    s_open = aos_hal_spk_open(RATE);
    if (!s_open) aos_hal_log("canos", "no speaker: beeps");
}

void cn_snd_close(void)
{
    if (s_open) aos_hal_spk_close();
    s_open = false;
}

void cn_snd_enable(bool on)
{
    s_on = on;
    if (!on) s_level_t = 0;
}

bool cn_snd_enabled(void)
{
    return s_on;
}

void cn_snd_tick(void)
{
    if (!s_open) return;
    int queued = aos_hal_spk_queued();
    int need = TARGET - queued;
    if (need <= 0) return;
    static int16_t buf[TARGET];
    if (need > TARGET) need = TARGET;
    for (int i = 0; i < need; i++) {
        int32_t mix = flow_sample();
        for (int k = 0; k < VOICES; k++) {
            voice_t *v = &s_v[k];
            if (v->kind == V_OFF) continue;
            if (v->t < 0) {
                v->t++;
                continue;
            }
            mix += voice_sample(v);
        }
        if (mix > 32767) mix = 32767;
        if (mix < -32767) mix = -32767;
        buf[i] = (int16_t)mix;
    }
    aos_hal_spk_write(buf, need);
}
