/*
 * PINBALL - the sound (see pb_audio.h)
 */
#include "pb_audio.h"

#include "pb_game.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#define RATE        16000
#define TARGET      1800            /* samples kept queued: ~110 ms          */
#define CHUNK       320
#define VOICES      10

typedef struct {
    const uint8_t *data;
    uint32_t len, pos;
    int      vol;
    int      snd;
} voice_t;

static struct {
    bool     open;
    bool     sfx_on, music_on, paused;
    uint8_t *snd[SND_N];
    uint32_t snd_len[SND_N];
    uint8_t *music;
    uint32_t music_len, music_pos;
    voice_t  v[VOICES];
    int16_t  ulaw[256];
    uint32_t age;
} A;

static void ulaw_table(void)
{
    for (int i = 0; i < 256; i++) {
        int u = ~i & 0xFF;
        int t = ((u & 0x0F) << 3) + 0x84;
        t <<= (u & 0x70) >> 4;
        A.ulaw[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
    }
}

bool pb_audio_open(pb_pak_t *pak)
{
    pb_audio_close();
    ulaw_table();
    char name[PB_NAME_LEN];
    for (int i = 0; i < SND_N; i++) {
        snprintf(name, sizeof name, "s_%s", PB_SND_NAMES[i]);
        const pb_entry_t *e = pb_pak_find(pak, name);
        if (!e || e->type != PK_PCM) continue;
        A.snd[i] = (uint8_t *)pb_pak_load(pak, e);
        A.snd_len[i] = A.snd[i] ? e->rawlen : 0;
    }
    const pb_entry_t *e = pb_pak_find(pak, "music");
    if (e && e->type == PK_PCM) {
        A.music = (uint8_t *)pb_pak_load(pak, e);
        A.music_len = A.music ? e->rawlen : 0;
    }
    A.open = aos_hal_spk_open(RATE);
    if (!A.open) aos_hal_log("pinball", "no speaker");
    return A.open;
}

void pb_audio_close(void)
{
    if (A.open) aos_hal_spk_close();
    for (int i = 0; i < SND_N; i++) pb_free(A.snd[i]);
    pb_free(A.music);
    bool sfx = A.sfx_on, mus = A.music_on;
    memset(&A, 0, sizeof A);
    A.sfx_on = sfx;
    A.music_on = mus;
}

void pb_audio_set(bool sfx, bool music)
{
    A.sfx_on = sfx;
    A.music_on = music;
}

void pb_audio_pause(bool paused)
{
    A.paused = paused;
}

void pb_audio_play(int snd, int vol)
{
    /* a sound the pack lacks plays a cousin it has */
    static const int8_t FALLBACK[SND_N] = {
        [SND_TOY_HIT] = SND_TARGET, [SND_TOY_DOWN] = SND_JACKPOT, [SND_VIDEO] = SND_MISSION, [SND_CANNON] = SND_EJECT,
        [SND_MAGNET] = SND_SAUCER, [SND_DISC] = SND_SPINNER, [SND_LETTER] = SND_LANE, [SND_VIDEO_HIT] = SND_TARGET,
    };
    if (snd >= 0 && snd < SND_N && !A.snd[snd] && FALLBACK[snd]) snd = FALLBACK[snd];
    if (!A.open || !A.sfx_on || snd < 0 || snd >= SND_N || !A.snd[snd]) return;
    /* the same sound again restarts its voice instead of piling up */
    int pick = -1;
    for (int i = 0; i < VOICES; i++) {
        if (A.v[i].data && A.v[i].snd == snd && A.v[i].pos < 1600) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        for (int i = 0; i < VOICES; i++) {
            if (!A.v[i].data) {
                pick = i;
                break;
            }
        }
    }
    if (pick < 0) {
        /* all busy: the one nearest its end */
        uint32_t best = 0;
        for (int i = 0; i < VOICES; i++) {
            uint32_t done = A.v[i].len ? A.v[i].pos * 256 / A.v[i].len : 256;
            if (pick < 0 || done > best) {
                best = done;
                pick = i;
            }
        }
    }
    voice_t *v = &A.v[pick];
    v->data = A.snd[snd];
    v->len = A.snd_len[snd];
    v->pos = 0;
    v->vol = vol;
    v->snd = snd;
}

void pb_audio_pump(void)
{
    if (!A.open) return;
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 16) {
        int32_t mix[CHUNK];
        memset(mix, 0, sizeof mix);
        if (!A.paused) {
            if (A.music && A.music_on && A.music_len) {
                for (int k = 0; k < CHUNK; k++) {
                    mix[k] += (A.ulaw[A.music[A.music_pos]] * 150) >> 8;
                    if (++A.music_pos >= A.music_len) A.music_pos = 0;
                }
            }
            for (int i = 0; i < VOICES; i++) {
                voice_t *v = &A.v[i];
                if (!v->data) continue;
                int n = CHUNK;
                if (v->pos + (uint32_t)n > v->len) n = (int)(v->len - v->pos);
                for (int k = 0; k < n; k++) mix[k] += (A.ulaw[v->data[v->pos + k]] * v->vol) >> 8;
                v->pos += (uint32_t)n;
                if (v->pos >= v->len) v->data = NULL;
            }
        }
        for (int k = 0; k < CHUNK; k++) {
            int32_t s = mix[k];
            /* a soft knee instead of a hard clip when many play at once */
            if (s > 24000) s = 24000 + (s - 24000) / 4;
            if (s < -24000) s = -24000 + (s + 24000) / 4;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[k] = (int16_t)s;
        }
        if (aos_hal_spk_write(buf, CHUNK) <= 0) break;
    }
}
