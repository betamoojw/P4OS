#include "dk_compat.h" /* P4OS: the C library as the board has it */
/*
 * P4OS - Duke Nukem 3D: the sound card. Replaces audiolib/dsl.c, which
 * played the Apogee Sound System's mix through SDL_mixer.
 *
 * The Apogee mixer (multivoc.c) works as it did on a Sound Blaster: a ring
 * of small buffers (16 x 256 samples), and a service routine that mixes
 * the next one whenever the card has played one. On DOS the card's
 * interrupt called it, under SDL its audio thread did; here the game's own
 * task does, from dk_sound_update(), which the frame and every wait call:
 * it keeps the board's streaming speaker (aos_hal_spk_*) about 100 ms
 * ahead, mixing buffer after buffer and pushing each one. No thread and no
 * lock, so the reverb's mutex had nothing left to guard (port/SDL.h).
 *
 * Mono, 16-bit, at Duke's own 22050 Hz (DK_RATE): one speaker, so the
 * mixer pans nothing, and the HAL resamples to its 48 kHz bus. The music
 * (port/dk_music.c, MIDI on the OPL emulator) is added on the same clock,
 * sample for sample, before the push.
 */
#pragma GCC optimize("O2")

#include "dsl.h"
#include "../duke_port.h"
#include "dk_port.h"
#include "aos_hal.h"

void dk_music_mix(int32_t *acc, int n);

#define AHEAD       (DK_RATE / 10)      /* samples kept queued: 100 ms */
#define CHUNK       512

static int  DSL_ErrorCode = DSL_Ok;
static bool s_enabled;                  /* the app opened the speaker */
static bool s_playing;

static void          (*s_callback)(void);
static volatile char  *s_buffer_start;
static int             s_buffer_size;   /* one division, bytes */
static int             s_divisions;
static unsigned        s_rate;
static int             s_mixmode;
static int             s_left;          /* samples of the current division not pushed yet */

extern volatile int MV_MixPage;

/* What the mixer costs, in CPU cycles: the core's own counter, which the
 * app turns into a share of core 0 for the log. */
static volatile uint32_t s_cyc_music, s_cyc_total;

static inline uint32_t cycles(void)
{
#if defined(__riscv)
    uint32_t c;
    __asm__ volatile("rdcycle %0" : "=r"(c));
    return c;
#else
    return 0;
#endif
}

void dk_audio_cycles(uint32_t *music, uint32_t *total)
{
    *music = s_cyc_music;
    *total = s_cyc_total;
}

void dk_sound_enable(bool on)
{
    s_enabled = on;
}

char *DSL_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber) {
    case DSL_Ok:                return "Sound ok.";
    case DSL_SDLInitFailure:    return "No speaker.";
    case DSL_MixerActive:       return "Mixer already running.";
    case DSL_MixerInitFailure:  return "Mixer failed.";
    default:                    return "Sound error.";
    }
}

int DSL_Init(void)
{
    DSL_ErrorCode = s_enabled ? DSL_Ok : DSL_SDLInitFailure;
    return s_enabled ? DSL_Ok : DSL_Error;
}

void DSL_Shutdown(void)
{
    DSL_StopPlayback();
}

int DSL_BeginBufferedPlayback(char *BufferStart, int BufferSize, int NumDivisions,
                              unsigned SampleRate, int MixMode, void (*CallBackFunc)(void))
{
    if (s_playing) {
        DSL_ErrorCode = DSL_MixerActive;
        return DSL_Error;
    }
    if (!s_enabled) {
        DSL_ErrorCode = DSL_SDLInitFailure;
        return DSL_Error;
    }
    s_callback = CallBackFunc;
    s_buffer_start = BufferStart;
    s_buffer_size = BufferSize / NumDivisions;
    s_divisions = NumDivisions;
    s_rate = SampleRate;
    s_mixmode = MixMode;
    s_left = 0;
    s_playing = true;
    printf("sound: %u Hz, %s, %s, %d x %d bytes\n", SampleRate,
           (MixMode & SIXTEEN_BIT) ? "16-bit" : "8-bit", (MixMode & STEREO) ? "stereo" : "mono",
           NumDivisions, s_buffer_size);
    return DSL_Ok;
}

void DSL_StopPlayback(void)
{
    s_playing = false;
    s_callback = NULL;
}

unsigned DSL_GetPlaybackRate(void)
{
    return s_rate;
}

uint32_t DisableInterrupts(void)
{
    return 0;
}

void RestoreInterrupts(uint32_t flags)
{
    (void)flags;
}

void dk_sound_shutdown(void)
{
    s_playing = false;
    s_callback = NULL;
}

/* One sample of the division the mixer last finished, as a signed 16-bit
 * value whatever its format (the game's config asks for 16-bit mono, but a
 * DUKE3D.CFG from a DOS install may say 8-bit or stereo). */
static inline int sample_at(const uint8_t *div, int i)
{
    int bytes = ((s_mixmode & SIXTEEN_BIT) ? 2 : 1) * ((s_mixmode & STEREO) ? 2 : 1);
    const uint8_t *p = div + i * bytes;
    int v;
    if (s_mixmode & SIXTEEN_BIT) {
        v = (int16_t)(p[0] | (p[1] << 8));
        if (s_mixmode & STEREO) v = (v + (int16_t)(p[2] | (p[3] << 8))) / 2;
    } else {
        v = ((int)p[0] - 128) << 8;
        if (s_mixmode & STEREO) v = (v + (((int)p[1] - 128) << 8)) / 2;
    }
    return v;
}

void dk_sound_update(void)
{
    if (!s_playing || !s_callback || !s_enabled) return;
    uint32_t t0 = cycles();
    int need = AHEAD - aos_hal_spk_queued();
    if (need <= 0) return;
    int bytes = ((s_mixmode & SIXTEEN_BIT) ? 2 : 1) * ((s_mixmode & STEREO) ? 2 : 1);
    int per_div = s_buffer_size / bytes;
    static int16_t out[CHUNK];
    static int32_t acc[CHUNK];
    while (need > 0) {
        int n = 0;
        while (n < CHUNK && n < need) {
            if (s_left == 0) {
                s_callback();               /* mixes the next division into MV_MixPage */
                s_left = per_div;
            }
            const uint8_t *div = (const uint8_t *)&s_buffer_start[MV_MixPage * s_buffer_size];
            int take = s_left;
            if (take > CHUNK - n) take = CHUNK - n;
            if (take > need - n) take = need - n;
            int from = per_div - s_left;
            for (int i = 0; i < take; i++) acc[n + i] = sample_at(div, from + i);
            n += take;
            s_left -= take;
        }
        uint32_t m0 = cycles();
        dk_music_mix(acc, n);               /* the music, on the same clock */
        s_cyc_music += cycles() - m0;
        for (int i = 0; i < n; i++) {
            int32_t v = acc[i];
            /* a soft knee over 3/4 of full scale: effects and music
             * together bend instead of clipping flat (Doom's) */
            int32_t a = v < 0 ? -v : v;
            if (a > 24576) {
                a = 24576 + (a - 24576) / 4;
                if (a > 32767) a = 32767;
                v = v < 0 ? -a : a;
            }
            out[i] = (int16_t)v;
        }
        aos_hal_spk_write(out, n);
        need -= n;
    }
    s_cyc_total += cycles() - t0;
}
