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

/* P4OS: rewritten around sms_psg.c (see sound.h). Kept from SMS Plus: the
 * entry points the rest of the core calls and the FM unit's detection
 * port. */

#include "shared.h"

snd_t snd;

/* Room for the largest frame: 48 kHz at PAL's 49.7 frames a second. */
#define SND_MAX_FRAME 1024

int sound_init(void)
{
  int lpf = (sms.display == DISPLAY_NTSC) ? 262 : 313;
  uint32 clock = (sms.display == DISPLAY_NTSC) ? CLOCK_NTSC : CLOCK_PAL;
  uint32 frame_cycles = (uint32)CYCLES_PER_LINE * lpf;

  snd.sample_rate = option.sndrate;
  if (snd.sample_rate < 8000 || snd.sample_rate > 48000)
    snd.sample_rate = 44100;

  /* A whole number of samples a frame, the nearest to the console's own
   * rate: the frame rate the app is told comes from it (sms_core.c). */
  snd.sample_count = (int)(((uint32)snd.sample_rate * frame_cycles + clock / 2) / clock);
  if (snd.sample_count > SND_MAX_FRAME) snd.sample_count = SND_MAX_FRAME;
  snd.lines_per_frame = lpf;
  snd.done_so_far = 0;

  if (!snd.buffer)
  {
    snd.buffer = calloc(SND_MAX_FRAME * 2, sizeof(int16));
    if (!snd.buffer) return 0;
  }

  /* Re-initialised (a state load, a change of region): the chip keeps its
   * registers, only its timing changes. */
  if (snd.enabled)
  {
    sms_psg_t keep = snd.psg;
    sms_psg_init(&snd.psg, sms.console >= CONSOLE_SMS, frame_cycles, snd.sample_count);
    uint32 step = snd.psg.step;
    snd.psg = keep;
    snd.psg.step = step;
  }
  else
  {
    sms_psg_init(&snd.psg, sms.console >= CONSOLE_SMS, frame_cycles, snd.sample_count);
  }

  snd.enabled = 1;
  return 1;
}


void sound_shutdown(void)
{
  free(snd.buffer);
  snd.buffer = NULL;
  snd.enabled = 0;
}


void sound_reset(void)
{
  if(!snd.enabled)
    return;

  sms_psg_reset(&snd.psg);
  snd.done_so_far = 0;
}


void sound_update(int line)
{
  if(!snd.enabled)
    return;

  int target = (line + 1) * snd.sample_count / snd.lines_per_frame;
  if (line >= snd.lines_per_frame - 1) target = snd.sample_count;
  if (target > snd.done_so_far)
  {
    sms_psg_render(&snd.psg, snd.buffer + 2 * snd.done_so_far, target - snd.done_so_far);
    snd.done_so_far = target;
  }
  if (line >= snd.lines_per_frame - 1)
    snd.done_so_far = 0;
}


/*--------------------------------------------------------------------------*/
/* Sound chip access handlers                                               */
/*--------------------------------------------------------------------------*/

void psg_stereo_w(int data)
{
  if(!snd.enabled) return;
  sms_psg_stereo(&snd.psg, (uint8)data);
}

void psg_write(int data)
{
  if(!snd.enabled) return;
  sms_psg_write(&snd.psg, (uint8)data);
}

/*--------------------------------------------------------------------------*/
/* Mark III FM Unit / Master System (J) built-in FM handlers                */
/*--------------------------------------------------------------------------*/

/* No YM2413: with sms.use_fm clear the unit is never found, and Japanese
 * games play their PSG music. */
int fmunit_detect_r(void)
{
  return sms.fm_detect;
}

void fmunit_detect_w(int data)
{
  if(!snd.enabled || !sms.use_fm) return;
  sms.fm_detect = data;
}

void fmunit_write(int offset, int data)
{
  (void)offset;
  (void)data;
}
