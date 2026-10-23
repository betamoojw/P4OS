/*
 * CAÑOS - the sound: the clicks of the pieces, the valve, and the liquid
 * itself, synthesised on the streaming speaker (16 kHz, mono).
 *
 * The running liquid is a bed of filtered noise with gurgles on top, both
 * shaped by the world: water burbles in the middle of the range, crude and
 * lava plop low and slow, the acid fizzes, the nitrogen hisses. Without the
 * speaker (another app holds it) the events fall back to beeps and the
 * liquid is silent.
 */
#pragma once

#include <stdbool.h>

enum {
    CN_SND_CLICK = 0,   /* a button        */
    CN_SND_TURN,        /* a pipe turns    */
    CN_SND_PLACE,       /* one is put down */
    CN_SND_TAKE,
    CN_SND_DENY,        /* can't touch that */
    CN_SND_VALVE,       /* the wheel turns */
    CN_SND_GUSH,        /* out of the tank */
    CN_SND_TICK,        /* the last seconds of the countdown */
    CN_SND_CROSS,       /* a cross taken the second time */
    CN_SND_LEAK,
    CN_SND_DRAIN,
    CN_SND_WIN,
    CN_SND_LOSE,
};

void cn_snd_open(void);
void cn_snd_close(void);
void cn_snd_enable(bool on);
bool cn_snd_enabled(void);
void cn_snd(int id);
/* The liquid running: which world's, and how loud (0 = quiet). */
void cn_snd_flow(int world, float level);
/* Every frame: tops the speaker up. */
void cn_snd_tick(void);
