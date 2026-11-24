"""Check the game's real RGB565 turn and glyph routines on RV32."""
import unittest

from rv32_harness import ROOT, run_c


class AsteroidesDisplay(unittest.TestCase):
    def test_partial_tiles_rotations_and_mask_clipping(self):
        run_c(self, r'''
#define AST_HARNESS
#include "ast_gfx.c"
void *memset(void *dest, int value, size_t n) {
    volatile unsigned char *p=dest; while(n--) *p++=(unsigned char)value; return dest;
}
static ast_gfx_t g;
static uint16_t panel[19 * 35 + 2], strip[19 * AST_TILE];
static ast_glyph_mask_t mask;
static ast_item_t item;
int test_main(void) {
    ast_gfx_size(&g, 19, 35);
    if(g.TW != 2 || g.TH != 3) return 1;
    g.fb = panel + 1;
    panel[0] = panel[19*35+1] = 0xcafe;
    for(int rot=90; rot<=270; rot+=180) {
        g.out = rot;
        for(int y0=0; y0<35; y0+=AST_TILE) {
            for(int j=0; j<AST_TILE; j++) for(int x=0; x<19; x++)
                strip[j*19+x] = (uint16_t)(1 + (y0+j)*19 + x);
            turn_out(&g, strip, 0, 19, y0);
        }
        for(int y=0; y<35; y++) for(int x=0; x<19; x++) {
            int at = rot==90 ? x*35+34-y : (18-x)*35+y;
            if(g.fb[at] != 1+y*19+x) return 2;
        }
        if(panel[0]!=0xcafe || panel[19*35+1]!=0xcafe) return 3;
    }
    memset(g.fb, 0, 19*35*2);
    memset(mask.pixels, 255, sizeof mask.pixels);
    mask.key=0x6771;
    g.item = &item;
    ast_glyph(&g, &mask, -2, 31, 8, 0xffffff, 255);
    if (g.nitem != 1 || item.x0 != 0 || item.y1 != 35) return 4;
    ast_rect_t clip = {1, 32, 4, 34};
    draw_glyph(&g, g.fb, &item, &clip);
    for(int y=0; y<35; y++) for(int x=0; x<19; x++) {
        uint16_t expected = (x>=1 && x<4 && y>=32 && y<34) ? 0xffff : 0;
        if(g.fb[y*19+x] != expected) return 5;
    }
    uint32_t first_hash=item.hash;
    g.nitem=0; mask.key|=1u<<24;
    ast_glyph(&g,&mask,-2,31,8,0xffffff,255);
    if(first_hash == item.hash) return 6;
    return panel[0]!=0xcafe || panel[19*35+1]!=0xcafe;
}
''', '-I' + str(ROOT / 'apps/asteroides/main'), '-ffunction-sections',
              '-fdata-sections', '-Wl,--gc-sections', '-lgcc')
