"""Check flight quality, panel coverage and touch alignment on both geometries."""
import unittest

from rv32_harness import ROOT, run_c


class VueloDisplay(unittest.TestCase):
    def test_all_qualities_fill_the_two_landscape_panels(self):
        source = (ROOT / 'apps/vuelo/main/vuelo.c').read_text(encoding='utf-8')
        dimension = source[source.index('static int render_dimension'):source.index('uint32_t vr_now_us')]
        present = source[source.index('static bool ppa_show'):source.index('static void present')]
        run_c(self, '''
#include <stdint.h>
#include <stdbool.h>
#define VA_QUALITIES 3
#define AOS_PANEL_H 1280
typedef struct {int SW, SH, fw[1], fh[1]; uint16_t *fr[1];} app_t;
static uint16_t back;
static int sh, rect[5][4], count;
static int aos_hal_screen_w(void){return 1280;}
static int aos_hal_screen_h(void){return sh;}
static uint16_t *aos_hal_display_back(void){return &back;}
static bool add_rect(int x,int y,int w,int h){
    if(count>=5 || x<0 || y<0 || x+w>1280 || y+h>sh) return false;
    int *r=rect[count++]; r[0]=x; r[1]=y; r[2]=x+w; r[3]=y+h; return true;
}
static bool aos_hal_display_blit_into_fit(uint16_t *b,int x,int y,int w,int h,
        uint16_t *src,int sw,int sh,int stride,bool be) { return add_rect(x,y,w,h); }
static bool aos_hal_display_blit_into(uint16_t *b,int x,int y,int w,int h,const uint16_t *src) {
    if(src[0] || src[w*h-1]) return false;
    return add_rect(x,y,w,h);
}
static bool aos_hal_display_flip(uint16_t *b){return b==&back;}
''' + dimension + present + '''
int test_main(void) {
    static const int heights[2][3]={{180,240,288},{200,266,320}};
    static const int widths[3]={320,426,512};
    for(int b=0; b<2; b++) for(int q=0; q<3; q++) {
        sh=b?800:720;
        app_t a={.SW=1280,.SH=sh};
        a.fw[0]=render_dimension(a.SW,q); a.fh[0]=render_dimension(a.SH,q);
        if(a.fw[0]!=widths[q] || a.fh[0]!=heights[b][q]) return 1;
        count=0;
        if(!ppa_show(&a,0) || rect[0][0]>1 || rect[0][1]>1) return 2;
        /* Every corner and edge midpoint is repainted, including the
         * one-pixel border where the PPA's integral scale cannot reach. */
        for(int iy=0; iy<3; iy++) for(int ix=0; ix<3; ix++) {
            int y=(sh-1)*iy/2, x=1279*ix/2, filled=0;
            for(int i=0; i<count; i++) filled |= x>=rect[i][0] && x<rect[i][2] && y>=rect[i][1] && y<rect[i][3];
            if(!filled) return 3;
        }
    }
    return 0;
}
''')
