/* UTF-8 fallback for the small software-rendered game canvases.
 * Include after defining AOS_PIXEL_BUF and AOS_PIXEL_PX. Latin art is unchanged.
 * Calls only existing ABI 101 exports, so no dynamic-app ABI change is needed. */
#pragma once
#include "aos_fonts.h"
#include <string.h>
#include "aos_utf8.h"
#define pixel_codepoint aos_utf8_next
static int pixel_text_width(const char *s, int advance)
{
    int w=0;
    while (*s) w += pixel_codepoint(&s)>=0x2e80 ? AOS_PIXEL_CJK_ADV : advance;
    return w ? w-1 : 0;
}
static void pixel_unicode(AOS_PIXEL_BUF *b, int x, int y, uint32_t cp, uint16_t color, int scale)
{
    lv_font_glyph_dsc_t g={0};
    if (!lv_font_get_glyph_dsc(&aos_inter_14,&g,cp,0) || !g.box_w || !g.box_h) return;
    lv_draw_buf_t *buf=lv_draw_buf_create(g.box_w,g.box_h,LV_COLOR_FORMAT_A8,0);
    if (!buf) return;
    const void *image=g.resolved_font->get_glyph_bitmap(&g,buf);
    int w=cp>=0x2e80 ? AOS_PIXEL_CJK_ADV : 6, h=cp>=0x2e80 ? AOS_PIXEL_CJK_ADV : 8;
    if (image) for (int yy=0;yy<h;yy++) for (int xx=0;xx<w;xx++) {
        if (buf->data[(yy*g.box_h/h)*buf->header.stride+xx*g.box_w/w] < 48) continue;
        for (int sy=0;sy<scale;sy++) for (int sx=0;sx<scale;sx++)
            AOS_PIXEL_PX(b,x+xx*scale+sx,y+yy*scale+sy,color);
    }
    if (g.resolved_font->release_glyph) g.resolved_font->release_glyph(g.resolved_font,&g);
    lv_draw_buf_destroy(buf);
}
