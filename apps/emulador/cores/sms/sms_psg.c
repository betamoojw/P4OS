/*
 * P4OS - Emulador, Sega 8-bit core: the SN76489 sound chip.
 *
 * Copyright (C) 2026 P4OS. This program is free software; you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Written for P4OS to replace the SN76489 SMS Plus carries, whose licence
 * is not stated anywhere in its sources. Three square-wave tone channels
 * and a noise channel, each with 16 volume steps of 2 dB, as in Sega's
 * VDP (Master System, Game Gear: a 16-bit noise register tapped at bits 0
 * and 3) and in the SN76489A on its own (ColecoVision, SG-1000: 15 bits,
 * tapped at bits 0 and 1). The Game Gear's stereo register routes each
 * channel to the left and right outputs.
 *
 * The chip counts at its clock / 16 (224 kHz for the consoles' 3.58 MHz).
 * Each output sample is the average of the chip's levels over the counts
 * it spans (a box filter), which keeps the high tones from folding back as
 * noise at 44.1 kHz. Periods of 6 counts or fewer (37 kHz and up) are held
 * high instead of toggling, as the consoles' DACs blur them: games play
 * samples by writing the volume of such a channel.
 *
 * The consoles' sound goes out through a capacitor, which takes away the
 * DC a channel held high (or a periodic noise) leaves: a high-pass at
 * about 35 Hz does the same here, so volume changes do not click and the
 * level stays centred.
 */
#include <string.h>

#include "sms_psg.h"

/* The 2 dB volume steps, 15 = silent: the loudest is a quarter of the
 * 16-bit range, so four channels together never clip. */
static const int16_t s_vol[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031,  819,  650,  517,  410,  326,    0,
};

void sms_psg_init(sms_psg_t *p, bool sega, uint32_t frame_cycles, int samples_per_frame)
{
    memset(p, 0, sizeof(*p));
    p->sega = sega;
    /* 16.16 counts per output sample, from the CPU cycles a frame takes (a
     * count is 16 of them) and the samples a frame gives, so the chip
     * keeps time with the CPU. 32-bit: 71364 * 4096 still fits. */
    p->step = frame_cycles * 4096u / (uint32_t)samples_per_frame;
    sms_psg_reset(p);
}

void sms_psg_reset(sms_psg_t *p)
{
    for (int i = 0; i < 3; i++) p->period[i] = 0;
    for (int i = 0; i < 4; i++) {
        p->count[i] = 1;
        p->out[i] = 1;
        p->vol[i] = 15;
    }
    p->noise_ctrl = 0;
    p->lfsr = p->sega ? 0x8000 : 0x4000;
    p->latch = 0;
    p->stereo = 0xFF;
    p->phase = 0;
    p->dc_x[0] = p->dc_x[1] = 0;
    p->dc_y[0] = p->dc_y[1] = 0;
}

void sms_psg_write(sms_psg_t *p, uint8_t data)
{
    if (data & 0x80) {
        p->latch = (data >> 4) & 7;
        int ch = p->latch >> 1;
        if (p->latch & 1) {
            p->vol[ch] = data & 0x0F;
        } else if (ch < 3) {
            p->period[ch] = (uint16_t)((p->period[ch] & 0x3F0) | (data & 0x0F));
        } else {
            p->noise_ctrl = data & 7;
            p->lfsr = p->sega ? 0x8000 : 0x4000;
        }
    } else {
        int ch = p->latch >> 1;
        if (p->latch & 1) {
            p->vol[ch] = data & 0x0F;
        } else if (ch < 3) {
            p->period[ch] = (uint16_t)((p->period[ch] & 0x00F) | ((data & 0x3F) << 4));
        } else {
            p->noise_ctrl = data & 7;
            p->lfsr = p->sega ? 0x8000 : 0x4000;
        }
    }
}

void sms_psg_stereo(sms_psg_t *p, uint8_t data)
{
    p->stereo = data;
}

/* One count of the chip; returns nothing, updates the channels' levels. */
static inline void tick(sms_psg_t *p)
{
    for (int i = 0; i < 3; i++) {
        if (--p->count[i] <= 0) {
            int per = p->period[i];
            p->count[i] = per ? per : 1;
            p->out[i] = (per <= 6) ? 1 : (uint8_t)(p->out[i] ^ 1);
        }
    }
    if (--p->count[3] <= 0) {
        int rate = p->noise_ctrl & 3;
        int per = rate == 3 ? (p->period[2] ? p->period[2] : 1) : (0x10 << rate);
        p->count[3] = per;
        p->out[3] ^= 1;
        if (p->out[3]) {
            /* the register shifts on the rising edge */
            uint16_t r = p->lfsr;
            unsigned fb;
            if (p->noise_ctrl & 4) {
                fb = p->sega ? ((r ^ (r >> 3)) & 1) : ((r ^ (r >> 1)) & 1);
            } else {
                fb = r & 1;
            }
            r >>= 1;
            r |= (uint16_t)(fb << (p->sega ? 15 : 14));
            p->lfsr = r;
        }
    }
}

void sms_psg_render(sms_psg_t *p, int16_t *lr, int frames)
{
    for (int s = 0; s < frames; s++) {
        p->phase += p->step;
        int n = (int)(p->phase >> 16);
        p->phase &= 0xFFFF;
        int32_t acc[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < n; k++) {
            tick(p);
            for (int i = 0; i < 3; i++) acc[i] += p->out[i] ? 1 : -1;
            acc[3] += (p->lfsr & 1) ? 1 : -1;
        }
        int32_t l = 0, r = 0;
        if (n > 0) {
            uint8_t st = p->stereo;
            for (int i = 0; i < 4; i++) {
                int32_t v = acc[i] * s_vol[p->vol[i]] / n;
                if (st & (0x10 << i)) l += v;
                if (st & (0x01 << i)) r += v;
            }
        }
        /* y = x - x' + 0.995 y' */
        int32_t yl = l - p->dc_x[0] + ((p->dc_y[0] * 32604) >> 15);
        int32_t yr = r - p->dc_x[1] + ((p->dc_y[1] * 32604) >> 15);
        p->dc_x[0] = l;
        p->dc_x[1] = r;
        p->dc_y[0] = yl;
        p->dc_y[1] = yr;
        if (yl > 32767) yl = 32767;
        if (yl < -32768) yl = -32768;
        if (yr > 32767) yr = 32767;
        if (yr < -32768) yr = -32768;
        lr[2 * s] = (int16_t)yl;
        lr[2 * s + 1] = (int16_t)yr;
    }
}
