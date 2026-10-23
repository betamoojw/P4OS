/*
 * P4OS - a draw unit for LVGL that hands the P4's DMA engines what they do
 * better than the CPU (2026-10-22).
 *
 * LVGL draws into two buffers in PSRAM, and the CPU copies PSRAM to PSRAM
 * at 67 MB/s (bench/results/20260928-resumen.md): a photo behind the home
 * screen cost 29 ms a whole frame, 72.9 -> 101.7 ms. Only the display's own
 * layer is taken, whose buffers aos_hal_p4.c aligns to the cache line, and
 * only from MIN_PX pixels up. Pref "draw_hw" (/api/tune) picks the way at
 * boot and aos_hal_draw_hw() at run time, for the display bench's A/B:
 *
 *   0  off: the CPU draws everything
 *   1  the PPA: opaque fills (ppa_do_fill) and RGB565 images drawn as they
 *      are (ppa_do_scale_rotate_mirror at 1:1). Measured: images slower
 *      than the CPU, 102.7 -> 134.7 ms at home; fills even.
 *   2  the PPA, images through its blender instead
 *   3  the AXI-GDMA (esp_async_memcpy) for images that span the whole
 *      width of the buffer, a photo behind the screen: source and
 *      destination are one contiguous run of rows each, a plain memcpy
 *
 * The PPA ways wait until nothing else of the layer is being drawn: the
 * PPA driver invalidates the whole rows of its output window before
 * writing, and those rows may share cache lines with pixels the other draw
 * unit, on the other core, is writing. Dispatching is LVGL's own thread,
 * so nothing starts while one works. The GDMA way writes whole rows only,
 * which no other task of the same moment can touch, and does not wait.
 * Either way the rows are written back from the cache first.
 *
 * LVGL's own PPA unit (LV_USE_PPA) is not used: it wants every draw buffer
 * in the system aligned to the cache line, the card's apps' canvases
 * included, and it does not guard against the invalidation above.
 */
#include "aos_hal.h"

#include "lvgl.h"
#include "lvgl_private.h"

#include "driver/ppa.h"
#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG          "draw_hw"
#define UNIT_ID      90             /* any id no other unit uses: SW is 1, LVGL's PPA 80-ish */
#define PREF_SCORE   20             /* lower wins; the software renderer gives 100 */
#define MIN_PX       (64 * 64)

enum { M_OFF, M_PPA_SRM, M_PPA_BLEND, M_GDMA, M_COUNT };

typedef struct {
    lv_draw_unit_t base;
    ppa_client_handle_t srm, fill, blend;
    async_memcpy_handle_t mcp;
    SemaphoreHandle_t done;
} unit_t;

static unit_t *s_unit;
static volatile int s_mode;
static uint32_t s_fills, s_images, s_skipped;

static bool full_rows(const lv_draw_task_t *t);

static bool layer_ok(const lv_layer_t *layer)
{
    return layer && !layer->parent && layer->color_format == LV_COLOR_FORMAT_RGB565;
}

static bool big_enough(const lv_draw_task_t *t)
{
    lv_area_t a;
    return lv_area_intersect(&a, &t->area, &t->clip_area) && lv_area_get_size(&a) >= MIN_PX;
}

static int32_t evaluate_cb(lv_draw_unit_t *u, lv_draw_task_t *t)
{
    (void)u;
    int mode = s_mode;
    if (mode == M_OFF) return 0;
    const lv_draw_dsc_base_t *base = t->draw_dsc;
    if (!layer_ok(base->layer) || !big_enough(t)) return 0;
    if (t->type == LV_DRAW_TASK_TYPE_FILL) {
        if (mode == M_GDMA) return 0;   /* fills: the PPA was only even with the CPU */
        const lv_draw_fill_dsc_t *d = t->draw_dsc;
        if (d->radius || d->grad.dir != LV_GRAD_DIR_NONE || d->opa < LV_OPA_MAX) return 0;
    } else if (t->type == LV_DRAW_TASK_TYPE_IMAGE) {
        const lv_draw_image_dsc_t *d = t->draw_dsc;
        if (lv_image_src_get_type(d->src) != LV_IMAGE_SRC_VARIABLE) return 0;
        const lv_image_dsc_t *img = d->src;
        if (img->header.cf != LV_COLOR_FORMAT_RGB565 || img->header.stride != img->header.w * 2) return 0;
        if (d->rotation || d->scale_x != LV_SCALE_NONE || d->scale_y != LV_SCALE_NONE || d->skew_x || d->skew_y) return 0;
        if (d->recolor_opa > LV_OPA_MIN || d->opa < LV_OPA_MAX || d->clip_radius || d->bitmap_mask_src || d->tile) return 0;
        if (d->blend_mode != LV_BLEND_MODE_NORMAL || d->colorkey) return 0;
        /* the image's own area, as LVGL places it: drawn 1:1 only */
        if (lv_area_get_width(&t->area) != (int32_t)img->header.w || lv_area_get_height(&t->area) != (int32_t)img->header.h) return 0;
        if (mode == M_GDMA && !full_rows(t)) return 0;
    } else {
        return 0;
    }
    if (t->preference_score > PREF_SCORE) {
        t->preference_score = PREF_SCORE;
        t->preferred_draw_unit_id = UNIT_ID;
    }
    return 1;
}

/* An image whose drawn part is whole rows of both the image and the
 * layer's buffer: one contiguous run of bytes in each */
static bool full_rows(const lv_draw_task_t *t)
{
    const lv_layer_t *layer = t->target_layer;
    const lv_image_dsc_t *img = ((const lv_draw_image_dsc_t *)t->draw_dsc)->src;
    lv_area_t a;
    if (!layer || !lv_area_intersect(&a, &t->area, &t->clip_area)) return false;
    int32_t bw = lv_area_get_width(&layer->buf_area);
    return a.x1 == t->area.x1 && a.x1 == layer->buf_area.x1 && lv_area_get_width(&a) == bw &&
           (int32_t)img->header.w == bw;
}

/* Nothing else of this layer being drawn right now: the rows we write may
 * share cache lines with pixels another unit is writing */
static bool layer_quiet(const lv_layer_t *layer)
{
    for (const lv_draw_task_t *t = layer->draw_task_head; t; t = t->next)
        if (t->state == LV_DRAW_TASK_STATE_IN_PROGRESS) return false;
    return true;
}

/* Writes back the whole rows of the output window: the driver invalidates
 * them before the transfer, and whatever the CPU left dirty there would be
 * lost */
static void rows_writeback(const lv_draw_buf_t *buf, int32_t y, int32_t h)
{
    uint8_t *p = buf->data + (size_t)y * buf->header.stride;
    esp_cache_msync(p, (size_t)h * buf->header.stride, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static bool do_fill(lv_draw_task_t *t, const lv_area_t *clip)
{
    const lv_draw_fill_dsc_t *d = t->draw_dsc;
    lv_layer_t *layer = t->target_layer;
    lv_draw_buf_t *buf = layer->draw_buf;
    lv_area_t a = *clip;
    lv_area_move(&a, -layer->buf_area.x1, -layer->buf_area.y1);
    rows_writeback(buf, a.y1, lv_area_get_height(&a));
    ppa_fill_oper_config_t c = {
        .out = {
            .buffer = buf->data,
            .buffer_size = buf->data_size,
            .pic_w = buf->header.w,
            .pic_h = buf->header.h,
            .block_offset_x = a.x1,
            .block_offset_y = a.y1,
            .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = lv_area_get_width(&a),
        .fill_block_h = lv_area_get_height(&a),
        .fill_argb_color = { .val = lv_color_to_u32(d->color) },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(s_unit->fill, &c) == ESP_OK;
}

static bool do_image(lv_draw_task_t *t, const lv_area_t *clip)
{
    const lv_draw_image_dsc_t *d = t->draw_dsc;
    const lv_image_dsc_t *img = d->src;
    lv_layer_t *layer = t->target_layer;
    lv_draw_buf_t *buf = layer->draw_buf;
    lv_area_t dst = *clip;
    lv_area_move(&dst, -layer->buf_area.x1, -layer->buf_area.y1);
    rows_writeback(buf, dst.y1, lv_area_get_height(&dst));
    ppa_srm_oper_config_t c = {
        .in = {
            .buffer = img->data,
            .pic_w = img->header.w,
            .pic_h = img->header.h,
            .block_w = lv_area_get_width(clip),
            .block_h = lv_area_get_height(clip),
            .block_offset_x = clip->x1 - t->area.x1,
            .block_offset_y = clip->y1 - t->area.y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = buf->data,
            .buffer_size = buf->data_size,
            .pic_w = buf->header.w,
            .pic_h = buf->header.h,
            .block_offset_x = dst.x1,
            .block_offset_y = dst.y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_unit->srm, &c) == ESP_OK;
}

/* the blender with its foreground switched off: what comes out is the
 * background, the image */
static bool do_blend(lv_draw_task_t *t, const lv_area_t *clip)
{
    const lv_draw_image_dsc_t *d = t->draw_dsc;
    const lv_image_dsc_t *img = d->src;
    lv_layer_t *layer = t->target_layer;
    lv_draw_buf_t *buf = layer->draw_buf;
    lv_area_t dst = *clip;
    lv_area_move(&dst, -layer->buf_area.x1, -layer->buf_area.y1);
    rows_writeback(buf, dst.y1, lv_area_get_height(&dst));
    ppa_in_pic_blk_config_t in = {
        .buffer = img->data,
        .pic_w = img->header.w,
        .pic_h = img->header.h,
        .block_w = lv_area_get_width(clip),
        .block_h = lv_area_get_height(clip),
        .block_offset_x = clip->x1 - t->area.x1,
        .block_offset_y = clip->y1 - t->area.y1,
        .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
    };
    ppa_blend_oper_config_t c = {
        .in_bg = in,
        .in_fg = in,
        .out = {
            .buffer = buf->data,
            .buffer_size = buf->data_size,
            .pic_w = buf->header.w,
            .pic_h = buf->header.h,
            .block_offset_x = dst.x1,
            .block_offset_y = dst.y1,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 0xFF,
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .fg_alpha_fix_val = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_blend(s_unit->blend, &c) == ESP_OK;
}

static bool IRAM_ATTR mcp_done(async_memcpy_handle_t h, async_memcpy_event_t *ev, void *arg)
{
    (void)h;
    (void)ev;
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &woke);
    return woke == pdTRUE;
}

/* whole rows: one copy of h * stride bytes */
static bool do_gdma(lv_draw_task_t *t, const lv_area_t *clip)
{
    const lv_image_dsc_t *img = ((const lv_draw_image_dsc_t *)t->draw_dsc)->src;
    lv_layer_t *layer = t->target_layer;
    lv_draw_buf_t *buf = layer->draw_buf;
    uint32_t stride = buf->header.stride;
    int32_t h = lv_area_get_height(clip);
    uint8_t *dst = buf->data + (size_t)(clip->y1 - layer->buf_area.y1) * stride;
    const uint8_t *src = img->data + (size_t)(clip->y1 - t->area.y1) * img->header.stride;
    size_t n = (size_t)h * stride;
    rows_writeback(buf, clip->y1 - layer->buf_area.y1, h);
    if (esp_async_memcpy(s_unit->mcp, dst, (void *)src, n, mcp_done, s_unit->done) != ESP_OK) return false;
    return xSemaphoreTake(s_unit->done, pdMS_TO_TICKS(100)) == pdTRUE;
}

static int32_t dispatch_cb(lv_draw_unit_t *u, lv_layer_t *layer)
{
    lv_draw_task_t *t = lv_draw_get_available_task(layer, NULL, UNIT_ID);
    if (!t || t->preferred_draw_unit_id != UNIT_ID) return LV_DRAW_UNIT_IDLE;
    int mode = s_mode;
    bool gdma = mode == M_GDMA && t->type == LV_DRAW_TASK_TYPE_IMAGE;
    if (!gdma && !layer_quiet(layer)) return LV_DRAW_UNIT_IDLE;     /* asked again when the other unit ends */
    if (!lv_draw_layer_alloc_buf(layer)) return LV_DRAW_UNIT_IDLE;
    lv_draw_buf_t *buf = layer->draw_buf;
    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    t->draw_unit = u;
    lv_area_t clip;
    bool ok = true;
    if (((uintptr_t)buf->data & 127) || (buf->data_size & 127) || buf->header.stride != buf->header.w * 2) {
        ok = false;                     /* not the buffers aos_hal_p4.c made: leave it */
    } else if (lv_area_intersect(&clip, &t->area, &t->clip_area)) {
        ok = t->type == LV_DRAW_TASK_TYPE_FILL ? do_fill(t, &clip)
           : gdma ? do_gdma(t, &clip)
           : mode == M_PPA_BLEND ? do_blend(t, &clip) : do_image(t, &clip);
    }
    if (ok) {
        if (t->type == LV_DRAW_TASK_TYPE_FILL) s_fills++;
        else s_images++;
    } else {
        /* the driver refused it: the CPU draws it after all */
        s_skipped++;
        t->preferred_draw_unit_id = 1;
        t->preference_score = 100;
        t->state = LV_DRAW_TASK_STATE_QUEUED;
        t->draw_unit = NULL;
        lv_draw_dispatch_request();
        return LV_DRAW_UNIT_IDLE;
    }
    t->state = LV_DRAW_TASK_STATE_FINISHED;
    lv_draw_dispatch_request();
    return 1;
}

static int32_t delete_cb(lv_draw_unit_t *u)
{
    unit_t *d = (unit_t *)u;
    if (d->srm) ppa_unregister_client(d->srm);
    if (d->fill) ppa_unregister_client(d->fill);
    if (d->blend) ppa_unregister_client(d->blend);
    if (d->mcp) esp_async_memcpy_uninstall(d->mcp);
    return 0;
}

void aos_draw_accel_init(void)
{
    int32_t mode = M_OFF;                   /* off until one of the ways wins on the board */
    aos_hal_pref_get_i32("draw_hw", &mode);
    unit_t *u = lv_draw_create_unit(sizeof(unit_t));
    u->base.name = "P4OS_PPA";
    u->base.evaluate_cb = evaluate_cb;
    u->base.dispatch_cb = dispatch_cb;
    u->base.delete_cb = delete_cb;
    ppa_client_config_t c = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1,
                              .data_burst_length = PPA_DATA_BURST_LENGTH_128 };
    bool ok = ppa_register_client(&c, &u->srm) == ESP_OK;
    c.oper_type = PPA_OPERATION_FILL;
    ok = ok && ppa_register_client(&c, &u->fill) == ESP_OK;
    c.oper_type = PPA_OPERATION_BLEND;
    ok = ok && ppa_register_client(&c, &u->blend) == ESP_OK;
    async_memcpy_config_t mc = ASYNC_MEMCPY_DEFAULT_CONFIG();
    mc.backlog = 2;
    mc.dma_burst_size = 64;
    u->done = xSemaphoreCreateBinary();
    ok = ok && u->done && esp_async_memcpy_install_gdma_axi(&mc, &u->mcp) == ESP_OK;
    s_unit = ok ? u : NULL;
    s_mode = ok && mode > M_OFF && mode < M_COUNT ? (int)mode : M_OFF;
    ESP_LOGI(TAG, "draw unit %s, way %d", ok ? "ready" : "unavailable", s_mode);
}

int aos_hal_draw_hw(int on)
{
    if (on >= 0 && on < M_COUNT && s_unit) s_mode = on;
    return s_mode;
}

void aos_hal_draw_hw_stats(uint32_t *fills, uint32_t *images, uint32_t *skipped)
{
    if (fills) *fills = s_fills;
    if (images) *images = s_images;
    if (skipped) *skipped = s_skipped;
}
