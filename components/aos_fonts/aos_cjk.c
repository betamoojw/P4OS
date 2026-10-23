/* Noto Sans SC/TC (OFL): regional glyphs from the offline Flash resource. */
#include "aos_cjk.h"
#include "aos_cjk_resource.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef AOS_CJK_DEFAULT_PATH
#define AOS_CJK_DEFAULT_PATH "assets/cjk/cjk.bin"
#endif
#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#else
#include <pthread.h>
#endif

enum { CELL = 48, RECORD = 4 + CELL * CELL / 4, LIMIT = 12 * 1024 * 1024, CACHE_SLOTS = 128 };
typedef struct { uint32_t index, age; unsigned char pixels[CELL*CELL/4]; } glyph_cache_t;
static uint32_t *s_keys;
static glyph_cache_t *s_cache;
static uint32_t s_count, s_tick;
static unsigned s_variant;
#ifdef ESP_PLATFORM
static const esp_partition_t *s_part;
static SemaphoreHandle_t s_mutex;
#define FONT_LOCK() xSemaphoreTake(s_mutex, portMAX_DELAY)
#define FONT_UNLOCK() xSemaphoreGive(s_mutex)
#else
static FILE *s_file;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
#define FONT_LOCK() pthread_mutex_lock(&s_mutex)
#define FONT_UNLOCK() pthread_mutex_unlock(&s_mutex)
#endif
static bool resource_read(size_t offset, void *dst, size_t length)
{
#ifdef ESP_PLATFORM
    return esp_partition_read(s_part, offset, dst, length)==ESP_OK;
#else
    return fseek(s_file, (long)offset, SEEK_SET)==0 && fread(dst,1,length,s_file)==length;
#endif
}
static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n)
{
    while (n--) { crc ^= *p++; for (int i=0;i<8;i++) crc = (crc>>1) ^ (0xedb88320u & (0u-(crc&1))); }
    return crc;
}
bool aos_cjk_init(void)
{
    if (s_keys) return true;
    unsigned char h[32];
#ifdef ESP_PLATFORM
    s_part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!s_part || s_part->size<32) return false;
#else
    const char *path=getenv("AOS_CJK_PATH");
    s_file=fopen(path ? path : AOS_CJK_DEFAULT_PATH,"rb");
    if (!s_file) return false;
#endif
    bool valid=resource_read(0,h,sizeof(h));
    uint32_t count=valid ? rd32(h+16) : 0, bytes=valid ? rd32(h+20) : 0;
    valid=valid && !memcmp(h,"P4CJK001",8) && rd32(h+8)==1 && rd32(h+12)==CELL &&
        count && count<=(LIMIT-32)/RECORD && bytes==count*RECORD && rd32(h+24)==AOS_CJK_RESOURCE_CRC;
#ifdef ESP_PLATFORM
    valid=valid && bytes<=s_part->size-32;
    uint32_t *keys=valid ? heap_caps_malloc(count*sizeof(*keys),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) : NULL;
    glyph_cache_t *cache=valid ? heap_caps_calloc(CACHE_SLOTS,sizeof(*cache),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) : NULL;
    unsigned char *chunk=valid ? heap_caps_malloc(16384,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT) : NULL;
#else
    uint32_t *keys=valid ? malloc(count*sizeof(*keys)) : NULL;
    glyph_cache_t *cache=valid ? calloc(CACHE_SLOTS,sizeof(*cache)) : NULL;
    unsigned char *chunk=valid ? malloc(16384) : NULL;
#endif
    valid=valid && keys && cache && chunk;
    uint32_t crc=~0u;
    /* Validate the complete image once, keeping only its index in PSRAM. */
    for (uint32_t offset=0;valid && offset<bytes;) {
        uint32_t take=bytes-offset;
        if (take>16384) take=16384;
        valid=resource_read(32+offset,chunk,take);
        if (!valid) break;
        crc=crc_update(crc,chunk,take);
        for (uint32_t at=(RECORD-offset%RECORD)%RECORD;at+4<=take;at+=RECORD)
            keys[(offset+at)/RECORD]=rd32(chunk+at);
        offset+=take;
#ifdef ESP_PLATFORM
        if ((offset & 0x3ffff)==0) vTaskDelay(1);
#endif
    }
    free(chunk);
    valid=valid && ~crc==rd32(h+24);
    for (uint32_t i=1;valid && i<count;i++) valid=keys[i-1]<keys[i];
#ifdef ESP_PLATFORM
    if (valid) { s_mutex=xSemaphoreCreateMutex(); valid=s_mutex!=NULL; }
#endif
    if (!valid) {
        free(keys);free(cache);
#ifndef ESP_PLATFORM
        fclose(s_file);s_file=NULL;
#endif
        return false;
    }
    for (int i=0;i<CACHE_SLOTS;i++) cache[i].index=UINT32_MAX;
    s_cache=cache;s_count=count;s_keys=keys;
#ifdef ESP_PLATFORM
    ESP_LOGI("cjk","SC/TC fonts ready: %lu glyphs, %lu Flash bytes, %u PSRAM bytes",
        (unsigned long)count,(unsigned long)bytes,(unsigned)(count*sizeof(*keys)+CACHE_SLOTS*sizeof(*cache)));
#endif
    return true;
}
void aos_cjk_language(const char *code) { s_variant = code && !strcmp(code,"zh-Hant"); }

static bool glyph(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t cp, uint32_t next)
{
    (void)next;
    if (!s_keys || !((cp>=0x2e80 && cp<0xe000) || (cp>=0xff00 && cp<=0xffef))) return false;
    uint32_t key=(s_variant<<24)|cp, lo=0, hi=s_count;
    while (lo<hi) { uint32_t mid=lo+(hi-lo)/2; if (s_keys[mid]<key) lo=mid+1; else hi=mid; }
    if (lo==s_count || s_keys[lo]!=key) return false;
    unsigned size=(unsigned)(uintptr_t)font->dsc;
    g->adv_w=size; g->box_w=size; g->box_h=size; g->ofs_x=0; g->ofs_y=-(int)(size/8);
    g->stride=0;
    g->format=LV_FONT_GLYPH_FORMAT_A8; g->gid.index=lo; g->is_placeholder=0;
    return true;
}
static const void *bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
    if (!buf || !s_keys || g->gid.index>=s_count || !g->box_w || g->box_w>144 ||
        g->box_h!=g->box_w || buf->header.w<g->box_w || buf->header.h<g->box_h) return NULL;
    FONT_LOCK();
    glyph_cache_t *slot=NULL, *oldest=&s_cache[0];
    for (int i=0;i<CACHE_SLOTS;i++) {
        if (s_cache[i].index==g->gid.index) { slot=&s_cache[i]; break; }
        if (s_cache[i].age<oldest->age) oldest=&s_cache[i];
    }
    if (!slot) {
        slot=oldest;
        if (!resource_read(32+g->gid.index*RECORD+4,slot->pixels,sizeof(slot->pixels))) {
            slot->index=UINT32_MAX; FONT_UNLOCK(); return NULL;
        }
        slot->index=g->gid.index;
    }
    slot->age=++s_tick;
    const unsigned char *src=slot->pixels;
    unsigned size=g->box_w;
    unsigned stride=lv_draw_buf_width_to_stride(size, LV_COLOR_FORMAT_A8);
    for (unsigned y=0;y<size;y++) for (unsigned x=0;x<size;x++) {
        unsigned x0=x*CELL/size, y0=y*CELL/size;
        unsigned x1=(x+1)*CELL/size, y1=(y+1)*CELL/size;
        if (x1<=x0) x1=x0+1;
        if (y1<=y0) y1=y0+1;
        unsigned coverage=0;
        for (unsigned sy=y0;sy<y1;sy++) for (unsigned sx=x0;sx<x1;sx++) {
            unsigned p=sy*CELL+sx;
            coverage+=((src[p/4]>>(6-2*(p%4)))&3)*85;
        }
        buf->data[y*stride+x]=coverage/((x1-x0)*(y1-y0));
    }
    FONT_UNLOCK();
    return buf;
}

#define CJK_FONT(n) const lv_font_t aos_cjk_##n = { .get_glyph_dsc=glyph, .get_glyph_bitmap=bitmap, .line_height=(n), .base_line=(n)/8, .dsc=(const void *)(uintptr_t)(n) }
/* All sizes referenced by generated Latin fonts, including dynamic apps. */
CJK_FONT(12); CJK_FONT(14); CJK_FONT(16); CJK_FONT(18); CJK_FONT(20);
CJK_FONT(22); CJK_FONT(24); CJK_FONT(26); CJK_FONT(28); CJK_FONT(30);
CJK_FONT(32); CJK_FONT(36); CJK_FONT(40); CJK_FONT(48); CJK_FONT(64);
CJK_FONT(72); CJK_FONT(96); CJK_FONT(144);
