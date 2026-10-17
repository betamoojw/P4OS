/*
 * P4OS - Emulador, Sega 8-bit core: the SN76489 sound chip (sms_psg.c).
 * GPL-2.0-or-later, written for P4OS.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t period[3];     /* tone registers, 10 bits                   */
    uint8_t  vol[4];        /* attenuation, 0 loudest .. 15 off          */
    uint8_t  noise_ctrl;    /* bits 0-1 rate, bit 2 white noise          */
    uint8_t  latch;         /* register the next data byte goes to       */
    uint8_t  stereo;        /* Game Gear: bit 4+n left, bit n right      */
    uint8_t  sega;          /* the VDP's PSG (16-bit noise) or a 76489A  */
    int16_t  count[4];      /* counts left to the next edge              */
    uint8_t  out[4];        /* each channel's flip-flop                  */
    uint16_t lfsr;          /* the noise shift register                  */
    uint16_t _pad;
    uint32_t phase;         /* 16.16 counts owed to the next sample      */
    uint32_t step;          /* 16.16 counts per sample                   */
    int32_t  dc_x[2], dc_y[2];  /* the output's DC blocker, L and R      */
} sms_psg_t;

/* frame_cycles: CPU cycles in a frame; samples_per_frame: what a frame
 * renders. Clears and resets. */
void sms_psg_init(sms_psg_t *p, bool sega, uint32_t frame_cycles, int samples_per_frame);
void sms_psg_reset(sms_psg_t *p);
void sms_psg_write(sms_psg_t *p, uint8_t data);
void sms_psg_stereo(sms_psg_t *p, uint8_t data);
/* 'frames' stereo frames, L R L R... */
void sms_psg_render(sms_psg_t *p, int16_t *lr, int frames);
