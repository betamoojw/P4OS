/*
 * P4OS - a photo as the wallpaper, behind the home screen and behind the
 * lock screen, each its own (2026-10-22).
 *
 * The photo is decoded on a thread of its own with aos_hal_image_decode(),
 * cropped to cover the screen (fill), and kept in PSRAM as RGB565: 1.8 MB a
 * screen, 3.7 MB with both. It is darkened while decoding, as much as its
 * own brightness asks (more behind the lock screen's big clock), so the
 * white names stay readable at no cost per frame: a translucent layer over
 * it would be blended on every redraw. Internal RAM: nothing but this file's
 * few words, the state is in PSRAM too.
 *
 * Under the photo the gradient is still there: the photo missing or not
 * readable, it shows as before. The paths are prefs ("wall_img",
 * "lock_img"); turning the screen decodes again at the new size.
 *
 * Only while they can be seen: with an app over the home screen both are
 * let go (3.7 MB back, which was what the doorbell's 1920x1080 stream was
 * short of, 2026-10-22), and decoded again - under half a second - on the
 * way home, or for the lock screen as the screen goes dark and locks.
 */
#include "aos_internal.h"
#include "aos_lock.h"
#include "lvgl_private.h"           /* lv_image_cache_drop */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_MAX_   192
/* The photo is darkened so its mean brightness is at most this (of 255):
 * a dark photo is left nearly as it is, a bright one comes down until the
 * white names on it read. Never below DIM_MIN of 256. */
#define LUMA_HOME   95
#define LUMA_LOCK   80
#define DIM_MIN     90
#define DIM_MAX     236

static const char *const PREF[2] = { "wall_img", "lock_img" };

typedef struct {
    char           path[PATH_MAX_];     /* "" none */
    uint16_t      *px;                  /* on show */
    int            w, h;
    lv_image_dsc_t dsc;
    uint32_t       gen;                 /* the request wanted */
    bool           want;                /* a request not yet on show */
    int            inflight;            /* decodes running: their results still to free */
    /* the worker's result, under s_mx */
    bool           ready;
    uint16_t      *new_px;
    int            new_w, new_h;
    uint32_t       new_gen;
    bool           new_thread;          /* from a decode (else from request(), with none) */
    uint32_t       tint, new_tint;      /* the photo's colour, lightened: the lock clock's */
} wall_t;

AOS_BSS_PSRAM static wall_t W[2];
static void       *s_mx;
static lv_timer_t *s_poll;
static lv_obj_t   *s_home_img;

typedef struct {
    int      which;
    uint32_t gen;
    int      w, h;
    char     path[PATH_MAX_];
} job_t;

static void dim(uint16_t *px, size_t n, unsigned target)
{
    uint64_t sum = 0;                   /* mean luma, from a pixel in 16 */
    size_t m = 0;
    for (size_t i = 0; i < n; i += 16, m++) {
        uint16_t p = px[i];
        unsigned r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
        sum += (r * 77 + g * 150 + b * 29) >> 8;
    }
    unsigned mean = m ? (unsigned)(sum / m) : 0;
    unsigned k = mean ? target * 256 / mean : DIM_MAX;
    if (k > DIM_MAX) k = DIM_MAX;
    if (k < DIM_MIN) k = DIM_MIN;
    for (size_t i = 0; i < n; i++) {
        uint16_t p = px[i];
        unsigned r = ((p >> 11) & 0x1F) * k >> 8, g = ((p >> 5) & 0x3F) * k >> 8, b = (p & 0x1F) * k >> 8;
        px[i] = (uint16_t)(r << 11 | g << 5 | b);
    }
}

/* The photo's mean colour, then three fifths of the way to white: a colour
 * of its own that a big clock can be drawn in and still read on it */
static uint32_t tint_of(const uint16_t *px, size_t n)
{
    uint32_t r = 0, g = 0, b = 0, m = 0;
    for (size_t i = 0; i < n; i += 37, m++) {
        uint16_t p = px[i];
        r += (p >> 11) << 3;
        g += ((p >> 5) & 0x3F) << 2;
        b += (p & 0x1F) << 3;
    }
    if (!m) return 0;
    r /= m; g /= m; b /= m;
    r += (255 - r) * 3 / 5; g += (255 - g) * 3 / 5; b += (255 - b) * 3 / 5;
    return r << 16 | g << 8 | b;
}

static void worker(void *arg)
{
    job_t *j = arg;
    int w = 0, h = 0;
    uint16_t *px = aos_hal_image_decode(j->path, 0, 0, j->w, j->h, true, &w, &h, NULL, NULL);
    if (px && (w != j->w || h != j->h)) { aos_hal_image_free(px); px = NULL; }
    uint32_t tint = px ? tint_of(px, (size_t)w * h) : 0;
    if (px) dim(px, (size_t)w * h, j->which == AOS_WALL_HOME ? LUMA_HOME : LUMA_LOCK);
    else aos_hal_log("ui", "wallpaper: %s could not be read", j->path);
    wall_t *v = &W[j->which];
    aos_hal_mutex_lock(s_mx);
    if (v->ready && v->new_px) aos_hal_image_free(v->new_px);   /* an older one nobody took */
    v->new_px = px;
    v->new_w = w;
    v->new_h = h;
    v->new_gen = j->gen;
    v->new_tint = tint;
    v->new_thread = true;
    v->ready = true;
    aos_hal_mutex_unlock(s_mx);
    free(j);
}

static void request(int which)
{
    wall_t *v = &W[which];
    v->gen++;
    v->want = true;
    if (!v->path[0]) {                  /* none: back to the gradient at once */
        aos_hal_mutex_lock(s_mx);
        if (v->ready && v->new_px) aos_hal_image_free(v->new_px);
        v->new_px = NULL;
        v->new_w = v->new_h = 0;
        v->new_gen = v->gen;
        v->new_thread = false;
        v->ready = true;
        aos_hal_mutex_unlock(s_mx);
    } else {
        job_t *j = malloc(sizeof *j);
        if (!j) return;
        const aos_geo_t *g = aos_ui_geo();
        j->which = which;
        j->gen = v->gen;
        j->w = g->w;
        j->h = g->h;
        snprintf(j->path, sizeof j->path, "%s", v->path);
        if (!aos_hal_thread_start("wallpaper", worker, j, 8192, 3)) { free(j); v->want = false; return; }
        v->inflight++;
    }
    lv_timer_resume(s_poll);
}

static void apply_home(void)
{
    wall_t *v = &W[AOS_WALL_HOME];
    if (!v->px) {
        if (s_home_img) lv_obj_delete(s_home_img);
        s_home_img = NULL;
        return;
    }
    if (!s_home_img) {
        s_home_img = lv_image_create(aos_ui_wall());
        lv_obj_remove_flag(s_home_img, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_image_set_src(s_home_img, NULL);     /* the same dsc with new pixels: read it again */
    lv_image_set_src(s_home_img, &v->dsc);
    lv_obj_center(s_home_img);
}

/* LVGL's task: takes what the worker finished */
static void poll_cb(lv_timer_t *t)
{
    for (int i = 0; i < 2; i++) {
        wall_t *v = &W[i];
        aos_hal_mutex_lock(s_mx);
        bool ready = v->ready;
        uint16_t *px = v->new_px;
        int w = v->new_w, h = v->new_h;
        uint32_t gen = v->new_gen;
        bool from_thread = v->new_thread;
        uint32_t tint = v->new_tint;
        v->ready = false;
        v->new_px = NULL;
        aos_hal_mutex_unlock(s_mx);
        if (!ready) continue;
        if (from_thread && v->inflight > 0) v->inflight--;
        if (gen != v->gen) { if (px) aos_hal_image_free(px); continue; }   /* asked for again since */
        v->want = false;
        uint16_t *old = v->px;
        lv_image_cache_drop(&v->dsc);
        v->px = px;
        v->w = w;
        v->h = h;
        if (px) v->tint = tint;
        memset(&v->dsc, 0, sizeof v->dsc);
        if (px) {
            v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            v->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            v->dsc.header.w = (uint32_t)w;
            v->dsc.header.h = (uint32_t)h;
            v->dsc.header.stride = (uint32_t)w * 2;
            v->dsc.data = (const uint8_t *)px;
            v->dsc.data_size = (uint32_t)w * h * 2;
        }
        if (i == AOS_WALL_HOME) apply_home();
        else aos_lock_layout();
        if (old) aos_hal_image_free(old);
    }
    if (!W[0].want && !W[1].want && !W[0].inflight && !W[1].inflight) lv_timer_pause(t);
}

/* Lets the photo go (want false) or has it decoded again (true) */
void aos_wall_photo_want(int which, bool want)
{
    if (which < 0 || which > 1 || !s_mx) return;
    wall_t *v = &W[which];
    if (!v->path[0]) return;
    if (want) {
        if (!v->px && !v->want) request(which);
        return;
    }
    v->gen++;                           /* whatever is being decoded is dropped when it comes */
    v->want = false;
    if (which == AOS_WALL_HOME && s_home_img) {
        lv_obj_delete(s_home_img);
        s_home_img = NULL;
    }
    if (v->px) {
        lv_image_cache_drop(&v->dsc);
        aos_hal_image_free(v->px);
        v->px = NULL;
        v->w = v->h = 0;
        memset(&v->dsc, 0, sizeof v->dsc);
    }
}

uint32_t aos_wall_photo_tint(int which)
{
    return which >= 0 && which <= 1 && W[which].path[0] ? W[which].tint : 0;
}

const lv_image_dsc_t *aos_wall_photo_dsc(int which)
{
    if (which < 0 || which > 1 || !W[which].px) return NULL;
    return &W[which].dsc;
}

void aos_ui_wall_photo_set(int which, const char *path)
{
    if (which < 0 || which > 1 || !s_mx) return;
    snprintf(W[which].path, sizeof W[which].path, "%s", path ? path : "");
    aos_hal_pref_set_str(PREF[which], W[which].path);
    request(which);
}

bool aos_ui_wall_photo(int which, char *out, size_t len)
{
    if (which < 0 || which > 1 || !W[which].path[0]) return false;
    if (out && len) snprintf(out, len, "%s", W[which].path);
    return true;
}

void aos_wall_photo_init(void)
{
    if (s_mx) return;
    s_mx = aos_hal_mutex_create();
    s_poll = lv_timer_create(poll_cb, 100, NULL);
    lv_timer_pause(s_poll);
    for (int i = 0; i < 2; i++) {
        if (!aos_hal_pref_get_str(PREF[i], W[i].path, sizeof W[i].path)) W[i].path[0] = 0;
        if (W[i].path[0]) request(i);
    }
}

/* The screen turned: each photo again, cropped to the new shape */
void aos_wall_photo_relayout(void)
{
    if (!s_mx) return;
    const aos_geo_t *g = aos_ui_geo();
    for (int i = 0; i < 2; i++)
        if (W[i].path[0] && (W[i].w != g->w || W[i].h != g->h)) request(i);
    if (s_home_img) lv_obj_center(s_home_img);
}
