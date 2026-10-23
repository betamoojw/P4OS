#pragma once
#include <stdint.h>
#include <stddef.h>
#define AOS_PIXEL_CJK_ADV 12

/* All helpers advance by complete code points, including on malformed input. */
static inline uint32_t aos_utf8_next(const char **s)
{
    const unsigned char *p=(const unsigned char *)*s;
    if (!*p) return 0;
    uint32_t cp=*p++; unsigned n=0; uint32_t minimum=0;
    if (cp>=0xf0 && cp<=0xf4) { cp&=7; n=3; minimum=0x10000; }
    else if (cp>=0xe0 && cp<=0xef) { cp&=15; n=2; minimum=0x800; }
    else if (cp>=0xc2 && cp<=0xdf) { cp&=31; n=1; minimum=0x80; }
    else if (cp>=128) cp=0xfffd;
    while (n--) {
        if ((*p&0xc0)!=0x80) { cp=0xfffd; break; }
        cp=(cp<<6)|(*p++&63);
    }
    if (cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) cp=0xfffd;
    *s=(const char *)p; return cp;
}
static inline size_t aos_utf8_length(const char *s)
{
    size_t n=0; while (*s) { aos_utf8_next(&s); n++; } return n;
}
static inline size_t aos_utf8_prefix_bytes(const char *s, size_t count)
{
    const char *start=s;
    while (*s && count--) aos_utf8_next(&s);
    return (size_t)(s-start);
}
/* width is in logical pixels; stride includes the trailing NUL byte. */
static inline int aos_utf8_wrap(const char *s, int width, int advance,
                                char *dst, size_t stride, int max)
{
    if (!s || !dst || stride<2 || width<1 || max<1) return 0;
    int lines=0;
    while (*s && lines<max) {
        char *out=dst+(size_t)lines*stride;
        size_t used=0, space_used=0;
        const char *last_space=NULL;
        int pixels=0;
        while (*s && *s!='\n') {
            const char *next=s;
            uint32_t cp=aos_utf8_next(&next);
            size_t bytes=(size_t)(next-s);
            int step=cp>=0x2e80 ? AOS_PIXEL_CJK_ADV : advance;
            if (used+bytes>=stride || pixels+step>width) {
                if (!used) {
                    if (bytes<stride) while (s<next) out[used++]=*s++;
                    else s=next;
                } else if (last_space) {
                    s=last_space+1; used=space_used;
                }
                break;
            }
            if (cp==' ') { last_space=s; space_used=used; }
            while (s<next) out[used++]=*s++;
            pixels+=step;
        }
        out[used]=0; lines++;
        if (*s=='\n') s++;
        else while (*s==' ') s++;
    }
    return lines;
}
