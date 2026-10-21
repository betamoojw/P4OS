/*
 * P4OS - one compressed glyph at a time.
 *
 * The fonts from 24 px up are RLE-compressed (tools/gen_fonts.py). LVGL
 * 9.5's decoder keeps its state in one global (LV_GLOBAL_DEFAULT()->
 * font_fmt_rle), and the board draws with two software draw units, one on
 * each core: two glyphs decompressed at once trampled each other, and big
 * text came out as noise on the board while the simulator, with one unit,
 * drew it right (2026-10-21). Every compressed font points its
 * get_glyph_bitmap here instead of at lv_font_get_bitmap_fmt_txt; the
 * generators write that in. Uncompressed fonts never take the lock.
 */
#include "lvgl.h"

#include <pthread.h>

const void *aos_font_bitmap_locked(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf);

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

const void *aos_font_bitmap_locked(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
    pthread_mutex_lock(&s_mx);
    const void *r = lv_font_get_bitmap_fmt_txt(g, buf);
    pthread_mutex_unlock(&s_mx);
    return r;
}
