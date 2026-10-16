/*
 * P4OS - Duke Nukem 3D: the music, as a Sound Blaster played it.
 *
 * On DOS, MUSIC.C picked the driver for the card in the setup, and with a
 * Sound Blaster that was Apogee's AdLib driver: MIDI.C read the song a MIDI
 * tick at a time, called by TaskMan from the PC's timer interrupt at the
 * song's tick rate, and AL_MIDI.C turned the notes into register writes on
 * the card's FM chip with the instruments of the game's timbre bank. Those
 * two files are here unchanged in what they do (game/audiolib/, every
 * change marked "P4OS:"); this file is MUSIC.C for the one card the port
 * has, and it is TaskMan and the timer too.
 *
 * There is one thread. The mixer (port/dk_sound.c) calls dk_music_mix() from
 * the game's task for the samples it is about to queue, and this file
 * generates them with the software chip (port/dk_opl.c), stopping at each
 * MIDI tick to run MIDI_ServiceRoutine(), whose register writes then sound
 * from the next sample on. The song's clock is the sample count, which is
 * what the speaker plays: the tempo holds whatever the frame rate does, and
 * the game's own MUSIC_*() calls come from the same task, so nothing needs
 * a lock.
 *
 * The timing is TaskMan's own arithmetic, not an idealised one: the tempo
 * is whole beats per minute (MIDI_SetTempo), the tick rate whole ticks per
 * second, and the period whole counts of the PIT, which TaskMan computed
 * as 1192030 / rate while the chip really counts at 1193182 Hz. A song
 * therefore runs about 0.1 % fast, as it did on a PC.
 */
#pragma GCC optimize("O2")

#include "dk_compat.h" /* P4OS: the C library as the board has it */
#include "../duke_port.h"
#include "dk_music.h"

#include "music.h"
#include "midi.h"
#include "al_midi.h"
#include "filesystem.h"

#define PIT_HZ      1193182u    /* the PC timer's real clock */
#define FADE_PERIOD (1192030L / 40)   /* MUSIC_FadeRoutine's 40 Hz task */
#define CHUNK       256

/* The chip's level to the mixer's, Q8 (256 is DBOPL's own). Measured on the
 * shareware's seven songs at full music volume: the title (GRABBAG) peaks
 * at 9261 with an RMS of 1477, the others peak between 1900 and 5500. */
#define GAIN_Q8     320

int MUSIC_SoundDevice = -1;     /* midi.c reads it for EMIDI's card type */
int MUSIC_ErrorCode   = MUSIC_Ok;

static midifuncs s_funcs;
static int       s_on;          /* MUSIC_Init done and not shut down */
static int       s_volume = 255;
static int32_t   s_mul;         /* GAIN_Q8 scaled by s_volume, Q8 */
static char     *s_song;        /* PlayMusic's copy of the file */

/* The two tasks TaskMan ran, as accumulators: each sample adds PIT_HZ, and
 * a task is due when its accumulator reaches its period times DK_RATE. */
static uint64_t  s_midi_clock;
static uint64_t  s_fade_clock;

static int       s_fade_length; /* MUSIC.C's fade state, as it was */
static int       s_fade_rate;
static unsigned  s_fade_current;
static unsigned  s_fade_last;
static int       s_fade_end;
static int       s_fading;

static int32_t   s_dc_x, s_dc_y;    /* the DC blocker's memory */
static int       s_quiet;           /* samples of silent chip since a song */
static int32_t   s_buf[CHUNK * 2];

/* --------------------------------------------------------------------------
 * The volume: the Sound Blaster's mixer, which MIDI.C reached through
 * SetVolume/GetVolume. Linear here (the SB16 mixer went in 2 dB steps);
 * the 16 steps of Duke's slider then cover about 24 dB.
 * -------------------------------------------------------------------------- */

static void set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    s_volume = volume;
    s_mul = volume * GAIN_Q8 / 255;
}

static int get_volume(void)
{
    return s_volume;
}

/* --------------------------------------------------------------------------
 * MUSIC.C
 * -------------------------------------------------------------------------- */

char *MUSIC_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber) {
    case MUSIC_Warning:
    case MUSIC_Error:       return MUSIC_ErrorString(MUSIC_ErrorCode);
    case MUSIC_Ok:          return "Music ok.";
    case MUSIC_ASSVersion:  return "Apogee Sound System Version " ASS_VERSION_STRING
                                   "  Programmed by Jim Dose\n"
                                   "(c) Copyright 1996 James R. Dose.  All Rights Reserved.\n";
    case MUSIC_SoundCardError: return "Sound Blaster error.";
    case MUSIC_MPU401Error: return "Could not detect MPU-401.";
    case MUSIC_InvalidCard: return "Invalid Music device.";
    case MUSIC_MidiError:   return "Error playing MIDI file.";
    case MUSIC_TaskManError: return "TaskMan error.";
    case MUSIC_FMNotDetected: return "Could not detect FM chip.";
    case MUSIC_DPMI_Error:  return "DPMI Error in MUSIC.";
    default:                return "Unknown Music error code.";
    }
}

/* Whatever card the config names, the music is FM: an AdLib is the OPL2,
 * a Pro AudioSpectrum the OPL3, and every other card (a General MIDI one,
 * an AWE32, the -1 of a fresh config) is played as a Sound Blaster. */
int MUSIC_Init(int SoundCard, int Address)
{
    int i;

    (void)Address;
    if (s_on) MUSIC_Shutdown();

    switch (SoundCard) {
    case Adlib:
    case ProAudioSpectrum:
    case SoundMan16:
        break;
    default:
        SoundCard = SoundBlaster;
        break;
    }
    MUSIC_SoundDevice = SoundCard;

    for (i = 0; i < 128; i++) MIDI_PatchMap[i] = (char)i;

    dk_opl_init(DK_RATE);
    AL_Init(SoundCard);

    memset(&s_funcs, 0, sizeof s_funcs);
    s_funcs.NoteOff       = AL_NoteOff;
    s_funcs.NoteOn        = AL_NoteOn;
    s_funcs.ControlChange = AL_ControlChange;
    s_funcs.ProgramChange = AL_ProgramChange;
    s_funcs.PitchBend     = AL_SetPitchBend;
    s_funcs.SetVolume     = set_volume;
    s_funcs.GetVolume     = get_volume;
    MIDI_SetMidiFuncs(&s_funcs);

    s_midi_clock = s_fade_clock = 0;
    s_dc_x = s_dc_y = 0;
    s_quiet = 0;
    s_fading = 0;
    set_volume(s_volume);
    s_on = 1;
    MUSIC_ErrorCode = MUSIC_Ok;
    return MUSIC_Ok;
}

int MUSIC_Shutdown(void)
{
    if (!s_on) return MUSIC_Ok;
    MIDI_StopSong();
    MUSIC_StopFade();
    AL_Shutdown();
    s_on = 0;
    return MUSIC_Ok;
}

void MUSIC_SetMaxFMMidiChannel(int channel)
{
    AL_SetMaxMidiChannel(channel);
}

void MUSIC_SetVolume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    if (s_on) MIDI_SetVolume(volume);
    else set_volume(volume);    /* remembered for when there is music */
}

void MUSIC_SetMidiChannelVolume(int channel, int volume)
{
    MIDI_SetUserChannelVolume(channel, volume);
}

void MUSIC_ResetMidiChannelVolumes(void)
{
    MIDI_ResetUserChannelVolume();
}

int MUSIC_GetVolume(void)
{
    return s_on ? MIDI_GetVolume() : 0;
}

void MUSIC_SetLoopFlag(int loopflag)
{
    MIDI_SetLoopFlag(loopflag);
}

int MUSIC_SongPlaying(void)
{
    return MIDI_SongPlaying();
}

void MUSIC_Continue(void)
{
    MIDI_ContinueSong();
}

void MUSIC_Pause(void)
{
    MIDI_PauseSong();
}

int MUSIC_StopSong(void)
{
    MUSIC_StopFade();
    MIDI_StopSong();
    MUSIC_ErrorCode = MUSIC_Ok;
    return MUSIC_Ok;
}

int MUSIC_PlaySong(char *songData, int loopflag)
{
    if (!s_on) {
        MUSIC_ErrorCode = MUSIC_InvalidCard;
        return MUSIC_Warning;
    }
    MIDI_StopSong();
    if (MIDI_PlaySong((unsigned char *)songData, loopflag) != MIDI_Ok) {
        MUSIC_ErrorCode = MUSIC_MidiError;
        return MUSIC_Warning;
    }
    s_midi_clock = 0;
    s_quiet = 0;
    return MUSIC_Ok;
}

void MUSIC_SetContext(int context)          { MIDI_SetContext(context); }
int  MUSIC_GetContext(void)                 { return MIDI_GetContext(); }
void MUSIC_SetSongTick(uint32_t t)          { MIDI_SetSongTick(t); }
void MUSIC_SetSongTime(uint32_t ms)         { MIDI_SetSongTime(ms); }
void MUSIC_SetSongPosition(int m, int b, int t) { MIDI_SetSongPosition(m, b, t); }
void MUSIC_GetSongPosition(songposition *pos)   { MIDI_GetSongPosition(pos); }
void MUSIC_GetSongLength(songposition *pos)     { MIDI_GetSongLength(pos); }

/* MUSIC_FadeRoutine, run by the sample clock at TaskMan's 40 Hz */
static void fade_routine(void)
{
    int volume;

    s_fade_current += s_fade_rate;
    if (s_fade_length == 0) {
        MIDI_SetVolume(s_fade_end);
        s_fading = 0;
    } else {
        s_fade_length--;
        volume = (int)(s_fade_current >> 7);
        if ((int)s_fade_last != volume) {
            s_fade_last = volume;
            MIDI_SetVolume(volume);
        }
    }
}

int MUSIC_FadeVolume(int tovolume, int milliseconds)
{
    int fromvolume;

    if (s_fading) MUSIC_StopFade();

    if (tovolume < 0) tovolume = 0;
    if (tovolume > 255) tovolume = 255;
    fromvolume = MUSIC_GetVolume();

    s_fade_length = milliseconds / 25;
    if (s_fade_length <= 0) {
        /* the original divided by it */
        MIDI_SetVolume(tovolume);
        return MUSIC_Ok;
    }
    s_fade_rate    = ((tovolume - fromvolume) << 7) / s_fade_length;
    s_fade_last    = fromvolume;
    s_fade_current = fromvolume << 7;
    s_fade_end     = tovolume;
    s_fade_clock   = 0;
    s_fading       = 1;
    return MUSIC_Ok;
}

int MUSIC_FadeActive(void)
{
    return s_fading;
}

void MUSIC_StopFade(void)
{
    s_fading = 0;
}

void MUSIC_RerouteMidiChannel(int channel, int cdecl (*function)(int event, int c1, int c2))
{
    MIDI_RerouteMidiChannel(channel, function);
}

void MUSIC_RegisterTimbreBank(unsigned char *timbres)
{
    AL_RegisterTimbreBank(timbres);
}

/* The game's playmusic() calls this with a name in the GRP ("GRABBAG.MID").
 * DOS Duke read the file into a buffer of 72000 bytes and played it from
 * there, looping; the buffer here is the file's size, kept until the next
 * song, since MIDI.C plays from it. */
void PlayMusic(char *fileName)
{
    int32_t fil, len;

    if (!s_on || !fileName) return;
    fil = kopen4load(fileName, 0);
    if (fil == -1) return;
    len = kfilelength(fil);
    if (len < 22 || len > 512 * 1024) {
        kclose(fil);
        return;
    }

    MUSIC_StopSong();           /* before the data it plays from goes */
    if (s_song) free(s_song);
    s_song = malloc((size_t)len);
    if (!s_song) {
        kclose(fil);
        return;
    }
    if (kread(fil, s_song, len) != len) {
        kclose(fil);
        free(s_song);
        s_song = NULL;
        return;
    }
    kclose(fil);
    MUSIC_PlaySong(s_song, MUSIC_LoopSong);
}

/* --------------------------------------------------------------------------
 * The mixer's side
 * -------------------------------------------------------------------------- */

/* Samples until a task of this period (PIT counts) is due, 0 if it is now */
static inline uint32_t samples_to(uint64_t clock, long period)
{
    uint64_t due = (uint64_t)period * DK_RATE;
    if (clock >= due) return 0;
    return (uint32_t)((due - clock + PIT_HZ - 1) / PIT_HZ);
}

void dk_music_mix(int32_t *acc, int n)
{
    if (!s_on) return;

    while (n > 0) {
        long period = MIDI_TaskPeriod();    /* 0: no song */
        uint32_t todo = n < CHUNK ? (uint32_t)n : CHUNK;
        uint32_t k;
        int stride, i, silent;

        /* Nothing loaded and the chip quiet for half a second (the last
         * notes' release has ended): there is nothing to generate. */
        if (period == 0 && !s_fading && s_quiet >= DK_RATE / 2) {
            s_dc_x = s_dc_y = 0;
            return;
        }

        if (period) {
            k = samples_to(s_midi_clock, period);
            if (k == 0) {
                s_midi_clock -= (uint64_t)period * DK_RATE;
                MIDI_ServiceRoutine();
                continue;
            }
            if (k < todo) todo = k;
        }
        if (s_fading) {
            k = samples_to(s_fade_clock, FADE_PERIOD);
            if (k == 0) {
                s_fade_clock -= (uint64_t)FADE_PERIOD * DK_RATE;
                fade_routine();
                continue;
            }
            if (k < todo) todo = k;
        }

        stride = dk_opl_generate(s_buf, (int)todo);

        /* DBOPL's output is not centred (its log-sine table leans), and
         * the offset eats headroom and clicks when notes stop: a one-pole
         * high-pass at ~17 Hz, y = x - x1 + 0.995 y1. "/ 1024" and not
         * ">> 10": the shift rounds negatives down and the filter would
         * stick at any value a couple of hundred below zero (Doom's
         * README, "A DC blocker that stuck"). */
        silent = 1;
        for (i = 0; i < (int)todo; i++) {
            int32_t x = s_buf[i * stride];
            silent &= (x == 0);
            s_dc_y = x - s_dc_x + s_dc_y * 1019 / 1024;
            s_dc_x = x;
            acc[i] += s_dc_y * s_mul / 256;
        }
        if (silent) {
            if (s_quiet < DK_RATE) s_quiet += (int)todo;
        } else {
            s_quiet = 0;
        }

        acc += todo;
        n -= (int)todo;
        s_midi_clock += (uint64_t)todo * PIT_HZ;
        if (s_fading) s_fade_clock += (uint64_t)todo * PIT_HZ;
    }
}

void dk_music_reset(void)
{
    if (s_on) {
        MIDI_StopSong();        /* gives the tracks' memory back */
        AL_Shutdown();
    }
    MIDI_PortReset();
    if (s_song) free(s_song);
    s_song = NULL;
    s_on = 0;
    s_fading = 0;
    s_volume = 255;
    s_mul = GAIN_Q8;
    s_midi_clock = s_fade_clock = 0;
    s_dc_x = s_dc_y = 0;
    s_quiet = 0;
    MUSIC_SoundDevice = -1;
    MUSIC_ErrorCode = MUSIC_Ok;
}
