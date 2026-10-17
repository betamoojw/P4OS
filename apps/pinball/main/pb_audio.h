/*
 * PINBALL - the sound: the table's own effects and music, from its pack
 *
 * Every table brings its sounds (tools/sounds.py makes them for its theme:
 * the zombies' groans, the casino's coins): "s_<name>" for each of
 * PB_SND_NAMES and "music", a loop, all mu-law at 16 kHz. A small mixer
 * plays them on the streaming speaker, topped up from the frame timer.
 */
#pragma once

#include "pb_pak.h"

#include <stdbool.h>

bool pb_audio_open(pb_pak_t *pak);
void pb_audio_close(void);
/* vol 0..256 */
void pb_audio_play(int snd, int vol);
/* keeps ~100 ms queued; call every frame */
void pb_audio_pump(void);
void pb_audio_set(bool sfx, bool music);
void pb_audio_pause(bool paused);
