/*
 * P4OS - Duke Nukem 3D: the music, as a Sound Blaster played it.
 *
 * Duke's music is General MIDI files in the GRP. On a Sound Blaster the
 * Apogee Sound System played them through its AdLib driver
 * (game/audiolib/al_midi.c) on the card's FM chip, with the instruments of
 * the game's own timbre bank (D3DTIMBR.TMB, or gmtimbre.c without it).
 * Here the chip is DOSBox's DBOPL in software (port/dbopl.c, the same one
 * Doom's port runs), and port/dk_music.c is what TaskMan and the PC's timer
 * were: the song advances as the samples are generated, in the game's own
 * task, with no threads and no timers.
 *
 * This header does not include the game's headers, so the mixer can use it
 * from anywhere.
 */
#ifndef DK_MUSIC_H
#define DK_MUSIC_H

#include <stdint.h>

/* The chip of the Sound Blaster being played: 1 is a SB16's OPL3, used
 * mono with eighteen voices, which is what Apogee's driver did with one;
 * 0 is the original Blaster's OPL2 with nine voices, which drops notes on
 * the busier songs. */
#ifndef DK_MUSIC_OPL3
#define DK_MUSIC_OPL3 1
#endif

/* For the mixer (port/dk_sound.c): adds n mono samples at DK_RATE to acc.
 * At full music volume the loud passages peak around +-9000. Called from
 * the game's task, the same that calls MUSIC_*(). */
void dk_music_mix(int32_t *acc, int n);

/* Forgets everything: the song, its memory, the chip. For when the game
 * leaves, before the heap goes back; harmless when nothing ever played. */
void dk_music_reset(void);

/* --- between the port's music files ------------------------------------ */

/* port/dk_opl.c: the chip. Registers 0x000-0x0FF are the first bank,
 * 0x100-0x1FF the OPL3's second. */
void dk_opl_init(int rate);
void dk_opl_write(unsigned reg, unsigned val);
/* Generates n samples into buf, which holds 2 * n: returns the stride of
 * the mono signal in it (1 for the OPL2, 2 for the OPL3's left output). */
int  dk_opl_generate(int32_t *buf, int n);

#endif
