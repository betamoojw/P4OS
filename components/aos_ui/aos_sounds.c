/*
 * P4OS - the system's sounds (2026-10-22): locking and unlocking, a
 * notification, a screenshot, a screen recording starting and stopping,
 * and the keyboard's click.
 *
 * Each is two or three notes of the HAL's bell (aos_hal_beep: a short
 * attack and a decay, queued and played by the output task, so nothing here
 * waits), no files and no PSRAM. Off with one tap: the control centre's
 * bell, or Settings, Sound (pref "sys_snd", on by default); the keyboard's
 * click apart (pref "key_snd", off by default).
 */
#include "aos_internal.h"

typedef struct { uint16_t hz, ms; } note_t;

static const note_t LOCK[]   = { { 880, 45 }, { 660, 70 } };
static const note_t UNLOCK[] = { { 660, 40 }, { 990, 70 } };
static const note_t NOTIF[]  = { { 1320, 70 }, { 1760, 110 } };
static const note_t SHOT[]   = { { 2600, 25 }, { 2100, 35 } };
static const note_t REC_ON[] = { { 880, 60 }, { 1320, 90 } };
static const note_t REC_OFF[]= { { 1320, 60 }, { 880, 90 } };
static const note_t CALL[]   = { { 1320, 140 } };
static const note_t KEY[]    = { { 2800, 8 } };

static const struct { const note_t *n; int len; } SOUND[] = {
    [AOS_SND_LOCK]    = { LOCK, 2 },
    [AOS_SND_UNLOCK]  = { UNLOCK, 2 },
    [AOS_SND_NOTIF]   = { NOTIF, 2 },
    [AOS_SND_SHOT]    = { SHOT, 2 },
    [AOS_SND_REC_ON]  = { REC_ON, 2 },
    [AOS_SND_REC_OFF] = { REC_OFF, 2 },
    [AOS_SND_CALL]    = { CALL, 1 },
    [AOS_SND_KEY]     = { KEY, 1 },
};

static int s_sys = -1, s_key = -1;             /* -1: not read yet */

static bool pref(const char *key, int *cache, int def)
{
    if (*cache < 0) {
        int32_t v = def;
        aos_hal_pref_get_i32(key, &v);
        *cache = v != 0;
    }
    return *cache != 0;
}

bool aos_ui_sounds_on(void) { return pref("sys_snd", &s_sys, 1); }
bool aos_ui_keyclick_on(void) { return pref("key_snd", &s_key, 0); }

void aos_ui_sounds_set(bool on)
{
    s_sys = on;
    aos_hal_pref_set_i32("sys_snd", on);
}

void aos_ui_keyclick_set(bool on)
{
    s_key = on;
    aos_hal_pref_set_i32("key_snd", on);
}

void aos_ui_sound(aos_sound_t s)
{
    if ((int)s < 0 || (size_t)s >= sizeof SOUND / sizeof SOUND[0] || !SOUND[s].n) return;
    if (!aos_ui_sounds_on()) return;
    if (s == AOS_SND_KEY && !aos_ui_keyclick_on()) return;
    for (int i = 0; i < SOUND[s].len; i++) aos_hal_beep(SOUND[s].n[i].hz, SOUND[s].n[i].ms);
}
