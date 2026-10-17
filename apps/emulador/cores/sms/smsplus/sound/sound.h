/******************************************************************************
 *  Sega Master System / GameGear Emulator
 *  Copyright (C) 1998-2007  Charles MacDonald
 *
 *  additionnal code by Eke-Eke (SMS Plus GX)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *   Sound emulation.
 *
 ******************************************************************************/

/* P4OS: rewritten around the core's own SN76489 (../../sms_psg.c), which
 * replaces SMS Plus's sn76489.c (no stated licence), and without the
 * YM2413 FM files (disabled in Retro-Go's copy). The sound of a frame is
 * one interleaved stereo buffer of a whole number of samples, rendered a
 * line at a time so register writes land where they happened. */

#ifndef _SOUND_H_
#define _SOUND_H_

#include "../../sms_psg.h"

#define SND_NONE 0

/* Sound emulation structure */
typedef struct
{
  int16 *buffer;        /* sample_count stereo frames, L R L R...       */
  int enabled;
  int sample_count;     /* samples a frame makes                        */
  int sample_rate;
  int done_so_far;
  int lines_per_frame;
  sms_psg_t psg;
} snd_t;


/* Global data */
extern snd_t snd;

/* Function prototypes */
void psg_write(int data);
void psg_stereo_w(int data);
int fmunit_detect_r(void);
void fmunit_detect_w(int data);
void fmunit_write(int offset, int data);
int sound_init(void);
void sound_shutdown(void);
void sound_reset(void);
void sound_update(int line);

#endif /* _SOUND_H_ */
