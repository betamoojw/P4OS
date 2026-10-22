/*
 * P4OS - video frames converted and scaled by the PPA (2026-10-22).
 *
 * A camera's H.264 is decoded in software (tinyh264, aos_h264.c): what is
 * left of the CPU after decoding is what the frame rate gets. Until now the
 * CPU also turned each picture from I420 to RGB565 and scaled it to the
 * screen, nearest neighbour, a table look-up per output pixel
 * (apps/camaras/main/cam_conv.c). The PPA does both in hardware, with its
 * filter, BT.601 limited range as cameras send.
 *
 * The PPA reads YUV420 in Espressif's packed layout, not in three planes:
 * line by line, the first of each pair "U Y Y" and the second "V Y Y" for
 * every two pixels (esp_h264's ESP_H264_RAW_FMT_O_UYY_E_VYY, the JPEG
 * engine's output). So the CPU still touches every source byte once, to
 * interleave the planes into a scratch buffer - a shuffle, no arithmetic -
 * and the PPA does the colour and the scaling.
 *
 * The scaler steps in sixteenths: the picture that comes out may be a
 * pixel or two smaller than asked, and *out_w / *out_h say what it is.
 *
 * Measured on the board (2026-10-22, the doorbell, 704x576 to 720x590 in
 * the viewer): 40-47 ms a frame, interleaving included, and 6.1 fps on the
 * panel, against up to 8.9 with the CPU's own conversion. Off by default
 * (pref "video_hw"). The work is bound by PSRAM, not by arithmetic: the
 * CPU's conversion reads about 1.5 bytes a pixel and writes 2; this way
 * reads and writes the interleaved copy (3 bytes a pixel) and then the PPA
 * reads it again and writes the 2, while the decoder on the other core
 * wants the same PSRAM. It would pay with a decoder that wrote the PPA's
 * layout itself.
 */
#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG     "video"
#define LINE    128
#define FRAC    16                  /* the scaler's step, PPA_LL_SRM_SCALING_FRAG_MAX */

static ppa_client_handle_t s_srm;
static SemaphoreHandle_t   s_mx;            /* one frame at a time, from any worker */
static uint8_t            *s_scratch;
static size_t              s_cap;
static int                 s_on = -1;       /* -1: pref not read yet */
static uint32_t            s_frames, s_us;

static bool setup(void)
{
    if (s_srm) return true;
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    ppa_client_config_t c = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1,
                              .data_burst_length = PPA_DATA_BURST_LENGTH_128 };
    if (ppa_register_client(&c, &s_srm) != ESP_OK) {
        s_srm = NULL;
        return false;
    }
    return true;
}

int aos_hal_video_hw(int on)
{
    if (s_on < 0) {
        int32_t v = 0;                  /* off: slower than the CPU (above) */
        aos_hal_pref_get_i32("video_hw", &v);
        s_on = v != 0;
    }
    if (on >= 0) s_on = on != 0;
    return s_on;
}

void aos_hal_video_hw_stats(uint32_t *frames, uint32_t *us)
{
    if (frames) *frames = s_frames;
    if (us) *us = s_us;
}

/* The largest scale, in sixteenths, that keeps 'in' scaled within 'max' */
static int steps_for(int in, int max)
{
    int s = max * FRAC / in;
    while (s > 1 && in * s / FRAC > max) s--;
    return s;
}

bool aos_hal_video_i420(const aos_h264_pic_t *pic, int sx, int sy, int sw, int sh,
                        uint16_t *out, size_t out_bytes, int max_w, int max_h, int *out_w, int *out_h)
{
    if (!pic || !out || aos_hal_video_hw(-1) == 0) return false;
    sx &= ~1; sy &= ~1; sw &= ~1; sh &= ~1;
    if (sw < 2 || sh < 2 || max_w < 2 || max_h < 2 || sx + sw > pic->width || sy + sh > pic->height) return false;
    if (((uintptr_t)out & (LINE - 1)) || out_bytes < (size_t)max_w * max_h * 2) return false;
    int kx = steps_for(sw, max_w), ky = steps_for(sh, max_h);
    if (kx < 1 || ky < 1 || kx >= 16 * FRAC || ky >= 16 * FRAC) return false;
    int ow = sw * kx / FRAC, oh = sh * ky / FRAC;
    if (ow < 2 || oh < 2) return false;
    if (!setup()) return false;

    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint64_t t0 = aos_hal_uptime_us();
    /* interleave: 3 bytes for every 2 pixels of a line */
    size_t row = (size_t)sw * 3 / 2, need = (row * sh + LINE - 1) & ~(size_t)(LINE - 1);
    if (need > s_cap) {
        heap_caps_free(s_scratch);
        s_scratch = heap_caps_aligned_alloc(LINE, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_cap = s_scratch ? need : 0;
    }
    bool ok = s_scratch != NULL;
    if (ok) {
        for (int y = 0; y < sh; y++) {
            const uint8_t *py = pic->y + (size_t)(sy + y) * pic->stride_y + sx;
            const uint8_t *pc = ((y & 1) ? pic->v : pic->u) + (size_t)((sy + y) >> 1) * pic->stride_uv + (sx >> 1);
            uint8_t *o = s_scratch + (size_t)y * row;
            for (int x = 0; x < sw; x += 2) {
                o[0] = pc[x >> 1];
                o[1] = py[x];
                o[2] = py[x + 1];
                o += 3;
            }
        }
        ppa_srm_oper_config_t c = {
            .in = {
                .buffer = s_scratch,
                .pic_w = (uint32_t)sw,
                .pic_h = (uint32_t)sh,
                .block_w = (uint32_t)sw,
                .block_h = (uint32_t)sh,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
            .out = {
                .buffer = out,
                .buffer_size = (uint32_t)(out_bytes & ~(size_t)(LINE - 1)),
                .pic_w = (uint32_t)ow,
                .pic_h = (uint32_t)oh,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = (float)kx / FRAC,
            .scale_y = (float)ky / FRAC,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t e = ppa_do_scale_rotate_mirror(s_srm, &c);
        if (e != ESP_OK) {
            static bool told;
            if (!told) ESP_LOGW(TAG, "PPA refused %dx%d -> %dx%d: %s", sw, sh, ow, oh, esp_err_to_name(e));
            told = true;
            ok = false;
        }
    }
    if (ok) {
        s_frames++;
        s_us += (uint32_t)(aos_hal_uptime_us() - t0);
    }
    xSemaphoreGive(s_mx);
    if (!ok) return false;
    if (out_w) *out_w = ow;
    if (out_h) *out_h = oh;
    return true;
}
