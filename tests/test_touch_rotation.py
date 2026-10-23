"""Execute the real input wrapper and panel transform, including released points."""
import unittest
from rv32_harness import ROOT, run_c

PRELUDE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LV_MAX(a,b) ((a) > (b) ? (a) : (b))
#define LV_CLAMP(lo,x,hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
'''


class TouchRotation(unittest.TestCase):
    def test_panel_bounds_all_boards_and_silicon_profiles(self):
        source = (ROOT / 'components/aos_hal/aos_hal_p4.c').read_text(encoding='utf-8')
        transform = source[source.index('static void touch_rotate('):source.index('/* Finger frames')]
        cases = r'''
int test_main(void) {
    int pxs[] = {-1, 0, AOS_PANEL_W-1, AOS_PANEL_W, 65535};
    int pys[] = {-1, 0, AOS_PANEL_H-1, AOS_PANEL_H, 65535};
    for (s_rot=0; s_rot<360; s_rot+=90) {
        int w = s_rot % 180 ? AOS_PANEL_H : AOS_PANEL_W;
        int h = s_rot % 180 ? AOS_PANEL_W : AOS_PANEL_H;
        for (int a=0; a<5; a++) for (int b=0; b<5; b++) {
            int16_t x,y; touch_rotate(pxs[a],pys[b],&x,&y);
            if (x<0 || x>=w || y<0 || y>=h) return 1;
            /* Reverse the display rotation: every edge must map correctly. */
            int px,py;
            switch(s_rot) {
            case 90: px=AOS_PANEL_W-1-y; py=x; break;
            case 180: px=AOS_PANEL_W-1-x; py=AOS_PANEL_H-1-y; break;
            case 270: px=y; py=AOS_PANEL_H-1-x; break;
            default: px=x; py=y;
            }
            if(px!=LV_CLAMP(0,pxs[a],AOS_PANEL_W-1) || py!=LV_CLAMP(0,pys[b],AOS_PANEL_H-1)) return 2;
        }
    }
    return 0;
}
'''
        for board in (5, 7, 8, 101):
            for rev in (100, 300):
                with self.subTest(board=board, silicon=rev):
                    run_c(self, PRELUDE + '#include "aos_hal.h"\nstatic int s_rot;\n' + transform + cases,
                          '-DAOS_SIM', '-I' + str(ROOT / 'components/aos_hal/include'), f'-DAOS_BOARD_ID={board}',
                          f'-DCONFIG_ESP32P4_REV_MIN_FULL={rev}')

    def test_released_cache_rotation_and_injected_gestures(self):
        source = (ROOT / 'components/aos_ui/aos_ui.c').read_text(encoding='utf-8')
        state = source[source.index('static struct {\n    bool     down;'):source.index('#define EDGE_BOTTOM')]
        wrapper = source[source.index('static void read_wrap('):source.index('void aos_ui_block_gestures')]
        inject = source[source.index('void aos_ui_inject_tap('):source.index('void aos_ui_touch_stats(')]
        stubs = r'''
typedef int gkind_t;
typedef struct {int unused;} lv_indev_t;
typedef struct {struct {int32_t x,y;} point; int state;} lv_indev_data_t;
typedef void (*lv_indev_read_cb_t)(lv_indev_t*,lv_indev_data_t*);
enum {LV_INDEV_STATE_RELEASED, LV_INDEV_STATE_PRESSED, LV_DIR_NONE=0};
static int width=1280,height=800,s_last_swipe,begins,moves,ends,resets,rotate_on_end;
static uint32_t tick;
static int aos_hal_screen_w(void) {return width;}
static int aos_hal_screen_h(void) {return height;}
static uint32_t lv_tick_get(void) {return tick;}
static uint32_t lv_tick_elaps(uint32_t start) {return tick-start;}
static bool aos_hal_display_is_on(void) {return true;}
static void aos_hal_activity(void) {}
static void lv_indev_reset(lv_indev_t *in,void *obj) {resets++;}
void *memset(void *p,int v,size_t n) {unsigned char *b=p;while(n--)*b++=v;return p;}
'''
        gestures = r'''
static void gesture_begin(int x,int y) {begins++;g.down=true;}
static void gesture_move(int x,int y) {moves++;}
static void gesture_end(void) {ends++;g.down=false;if(rotate_on_end){input_rotation_reset();width=800;height=1280;}}
static int physical_down;
/* Like the board with no finger, leave LVGL's previous raw point intact. */
static void orig(lv_indev_t *in,lv_indev_data_t *d) {d->state=physical_down;}
'''
        cases = r'''
int test_main(void) {
    lv_indev_t dev={0};s_indev=&dev;s_orig_read=orig;
    lv_indev_data_t d={.point={803,799}};
    input_rotation_reset();width=800;height=1280;
    for(int i=0;i<100;i++){read_wrap(&dev,&d);if(d.point.x!=799 || d.state) return 1;}
    /* Physical finger held across a turn: no press, click or swipe. */
    physical_down=1;g.down=true;input_rotation_reset();
    for(int i=0;i<3;i++)read_wrap(&dev,&d);
    if(begins || ends || g.down || d.state) return 2;
    physical_down=0;read_wrap(&dev,&d);physical_down=1;read_wrap(&dev,&d);
    if(begins!=1 || !d.state) return 3;
    physical_down=0;read_wrap(&dev,&d);
    /* Pending and ongoing remote gestures are cancelled on rotation. */
    aos_ui_inject_drag(1200,700,1000,600,300);input_rotation_reset();read_wrap(&dev,&d);
    if(s_inj.active || d.state || begins!=1) return 4;
    aos_ui_inject_tap(780,1100,80);read_wrap(&dev,&d);
    if(!d.state || d.point.x!=780 || d.point.y!=1100) return 5;
    input_rotation_reset();width=1280;height=800;tick+=100;read_wrap(&dev,&d);
    if(d.state || d.point.y!=799) return 6;
    /* Remote out-of-range sample, and rotation caused by gesture_end itself. */
    aos_ui_inject_tap(1300,-1,80);read_wrap(&dev,&d);
    if(d.point.x!=1279 || d.point.y!=0) return 7;
    tick+=100;rotate_on_end=1;read_wrap(&dev,&d);
    if(d.state || d.point.x>=800 || d.point.y<0 || s_last_swipe!=LV_DIR_NONE) return 8;
    if(resets<5) return 9;
    return 0;
}
'''
        run_c(self, PRELUDE + stubs + state + gestures + wrapper + inject + cases)


if __name__ == '__main__':
    unittest.main()
