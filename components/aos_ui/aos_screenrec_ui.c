/*
 * P4OS - the screen recording's controls (2026-10-22): the control centre's
 * button starts and stops it (aos_hal_screenrec_*, the PPA and the H.264
 * engine), a red dot in the status bar says it is on, and a toast where the
 * file went when it ends.
 *
 * It starts a moment after the button, so the control centre is gone from
 * the screen by the first frame.
 */
#include "aos_internal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

#define START_MS    700
#define POLL_MS     500
#define FPS         15

static lv_timer_t *s_poll;
static bool        s_on, s_pending;

static void poll_cb(lv_timer_t *t)
{
    if (aos_hal_screenrec_state(NULL, NULL, NULL)) return;
    lv_timer_delete(t);
    s_poll = NULL;
    s_on = false;
    aos_ui_sound(AOS_SND_REC_OFF);
    aos_ui_status_icon_set("sys.screenrec", NULL, 0);
    aos_ui_toast(aos_hal_screenrec_done_ok() ? _("Grabación guardada en la carpeta videos de la tarjeta")
                                             : _("La grabación no se pudo terminar bien"), 3000);
}

static void start_cb(lv_timer_t *t)
{
    (void)t;
    s_pending = false;
    if (!aos_hal_screenrec_start(NULL, FPS)) {
        aos_ui_toast(_("No se pudo empezar a grabar (¿hay tarjeta?)"), 2500);
        return;
    }
    s_on = true;
    aos_ui_sound(AOS_SND_REC_ON);
    aos_ui_status_icon_set("sys.screenrec", AOS_SYM_RECORD_REC, 0xFF3B30);
    if (!s_poll) s_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}

bool aos_screenrec_on(void)
{
    return s_on || s_pending || aos_hal_screenrec_state(NULL, NULL, NULL);
}

void aos_screenrec_toggle(void)
{
    if (aos_hal_screenrec_state(NULL, NULL, NULL)) {
        aos_hal_screenrec_stop();      /* the poll says when the file is whole */
        return;
    }
    if (s_on || s_pending) return;     /* already about to start */
    s_pending = true;
    lv_timer_t *t = lv_timer_create(start_cb, START_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void aos_ui_screenrec(bool on)
{
    if (on != aos_screenrec_on()) aos_screenrec_toggle();
}
