/*  gngeo a neogeo emulator
 *  Copyright (C) 2001 Peponas Mathieu
 * 
 *  This program is free software; you can redistribute it and/or modify  
 *  it under the terms of the GNU General Public License as published by   
 *  the Free Software Foundation; either version 2 of the License, or    
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA. 
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
/* P4OS: zlib, FATFS's diskio, the screen, frame skip and message headers
 * are not vendored; the two things this file took from screen.h are
 * declared here (ng_core.c defines them). */
#include "video.h"
#include "memory.h"
#include "emu.h"
#include "transpack.h"
#include "gnutil.h"

extern SDL_Surface *buffer;
void *ng_big_alloc(size_t n);
void ng_big_free(void *p);
extern SDL_Rect visible_area;
/* ng_load.c: reads 'count' banks of the sprite cache file, starting at
 * bank 'bank', into 'dest'. 0 when it worked. */
int ng_spr_read_banks(Uint32 bank, Uint32 count, Uint8 *dest);

extern int neogeo_fix_bank_type;
unsigned int neogeo_frame_counter;

#ifdef PROCESSOR_ARM
/* global declaration for video_arm.S */
Uint8 *mem_gfx = NULL; /*=memory.rom.tiles.p;*/
Uint8 *mem_video = NULL; //memory.vid.ram;
//#define TOTAL_GFX_BANK 4096
Uint32 *mem_bank_usage;

//GFX_CACHE gcache;

void draw_one_char_arm(int byte1, int byte2, unsigned short *br);
int draw_tile_arm_norm(unsigned int tileno, int color, unsigned char *bmp, int zy);
#endif

#ifdef I386_ASM
/* global declaration for video_i386.asm */
Uint8 **mem_gfx; //=&memory.rom.tiles.p;
Uint8 *mem_video; //=memory.vid.ram;

/* prototype */
void draw_tile_i386_norm(unsigned int tileno, int sx, int sy, int zx, int zy,
		int color, int xflip, int yflip, unsigned char *bmp);
void draw_tile_i386_50(unsigned int tileno, int sx, int sy, int zx, int zy,
		int color, int xflip, int yflip, unsigned char *bmp);
void draw_one_char_i386(int byte1, int byte2, unsigned short *br);

void draw_scanline_tile_i386_norm(unsigned int tileno, int yoffs, int sx, int line, int zx,
		int color, int xflip, unsigned char *bmp);

void draw_scanline_tile_i386_50(unsigned int tileno, int yoffs, int sx, int line, int zx,
		int color, int xflip, unsigned char *bmp);
#endif

//Uint8 strip_usage[0x300];
#define PEN_USAGE(tileno) ((((Uint32*) memory.rom.spr_usage.p)[tileno>>4]>>((tileno&0xF)*2))&0x3)


char *ldda_y_skip;
char *dda_x_skip;
char ddaxskip[16][16] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
	{ 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
	{ 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
	{ 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
	{ 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0},
	{ 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0},
	{ 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0},
	{ 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0},
	{ 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0},
	{ 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0},
	{ 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1},
	{ 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1},
	{ 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1},
	{ 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1},
	{ 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
	{ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}
};
Uint32 ddaxskip_i[16] = {
	0x0080, 0x0880, 0x0888, 0x2888, 0x288a, 0x2a8a, 0x2aaa, 0xaaaa,
	0xaaea, 0xbaea, 0xbaeb, 0xbbeb, 0xbbef, 0xfbef, 0xfbff, 0xffff
};
Uint32 dda_x_skip_i;

static __inline__ Uint16 alpha_blend(Uint16 dest, Uint16 src, Uint8 a) {
	static Uint8 dr, dg, db, sr, sg, sb;

	dr = ((dest & 0xF800) >> 11) << 3;
	dg = ((dest & 0x7E0) >> 5) << 2;
	db = ((dest & 0x1F)) << 3;

	sr = ((src & 0xF800) >> 11) << 3;
	sg = ((src & 0x7E0) >> 5) << 2;
	sb = ((src & 0x1F)) << 3;

	dr = (((sr - dr)*(a)) >> 8) + dr;
	dg = (((sg - dg)*(a)) >> 8) + dg;
	db = (((sb - db)*(a)) >> 8) + db;

	return ((dr >> 3) << 11) | ((dg >> 2) << 5) | (db >> 3);
}
#define BLEND16_50(a,b) ((((a)&0xf7de)>>1)+(((b)&0xf7de)>>1))
#define BLEND16_25(a,b) alpha_blend(a,b,63)


char dda_y_skip[17];
Uint32 dda_y_skip_i;
Uint32 full_y_skip_i = 0xFFFE;
char full_y_skip[16] = {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
unsigned int neogeo_frame_counter_speed = 8;
static Uint16 fix_addr[40][32];
static Uint8 fix_shift[40];


/* P4OS: which slots the frame being drawn needs. The cache used to evict
 * round-robin, so when a frame's banks filled most of it (Metal Slug: ~900
 * banks a frame against 1268 slots) each new bank pushed out one the same
 * frame was about to draw, and the frame read its own banks back from the
 * card over and over: 62 % hits, ~330 reads a frame, 1.8 fps on the board.
 * Now each slot carries the number of the frame that last needed it, and
 * eviction passes over the slots of the frame being drawn: only banks the
 * frame does not use are replaced. */
static Uint32 *spr_stamp;           /* per slot */
static Uint32 spr_stamp_now = 1;
static int spr_slot_shift;          /* log2(slot_size): a pointer's slot */

static int spr_stamp_alloc(int slots) {
	spr_stamp = ng_big_alloc((size_t)slots * sizeof(Uint32));
	if (!spr_stamp) return 0;
	memset(spr_stamp, 0, (size_t)slots * sizeof(Uint32));
	spr_stamp_now = 1;
	spr_slot_shift = 0;
	while ((1u << spr_slot_shift) < memory.vid.spr_cache.slot_size) spr_slot_shift++;
	return 1;
}

static void spr_stamp_free(void) {
	ng_big_free(spr_stamp);
	spr_stamp = NULL;
}

int init_sprite_cache(Uint32 size, Uint32 bsize) {
	GFX_CACHE *gcache = &memory.vid.spr_cache;
	int i;

	if (gcache->data != NULL) { /* We allready have a cache, just reset it */
		memset(gcache->ptr, 0, gcache->total_bank * sizeof (Uint8*));
		for (i = 0; i < gcache->max_slot; i++)
			gcache->usage[i] = -1;
		return 0;
	}

	/* Create our video cache */
	gcache->slot_size = bsize;
	printf("gfx_size=%08x\n", memory.rom.tiles.size);
	gcache->total_bank = memory.rom.tiles.size / gcache->slot_size;
	gcache->ptr = ng_big_alloc(gcache->total_bank * sizeof (Uint8*)); /* P4OS: under the budget */
	if (gcache->ptr == NULL)
		return GN_FALSE;
	memset(gcache->ptr, 0, gcache->total_bank * sizeof (Uint8*));

	gcache->size = size;
	gcache->data = ng_big_alloc(gcache->size); /* P4OS */
	if (gcache->data == NULL) {
		ng_big_free(gcache->ptr);
		gcache->ptr = NULL;
		return GN_FALSE;
	}
	printf("INIT CACHE %p (%u KB)\n", gcache->data, size / 1024);

	gcache->max_slot = size / gcache->slot_size;
	printf("Allocating %08x for gfx cache (%d %d slot)\n", gcache->size, gcache->max_slot, gcache->slot_size);
	gcache->usage = ng_big_alloc(gcache->max_slot * sizeof (Uint32)); /* P4OS */
	for (i = 0; i < gcache->max_slot; i++)
		gcache->usage[i] = -1;

	/* P4OS: RetroESP32's GNO decompression buffer is gone (the cache
	 * file holds the converted tiles as they are). */
	if (gcache->usage == NULL) {
		ng_big_free(gcache->ptr);
		ng_big_free(gcache->data);
		gcache->ptr = NULL;
		gcache->data = NULL;
		return GN_FALSE;
	}
	gcache->hits = gcache->misses = gcache->reads = 0;
	/* P4OS: the frame each slot was last needed in (see evict_slot) */
	if (!spr_stamp_alloc(gcache->max_slot)) {
		ng_big_free(gcache->usage);
		ng_big_free(gcache->ptr);
		ng_big_free(gcache->data);
		gcache->usage = NULL;
		gcache->ptr = NULL;
		gcache->data = NULL;
		return GN_FALSE;
	}
	return GN_TRUE;
}

void free_sprite_cache(void) {
	GFX_CACHE *gcache = &memory.vid.spr_cache;
	spr_stamp_free();
	/* P4OS: ng_big_free (the blocks count in the core's budget) */
	ng_big_free(gcache->data);
	gcache->data = NULL;
	ng_big_free(gcache->ptr);
	gcache->ptr = NULL;
	ng_big_free(gcache->usage);
	gcache->usage = NULL;
	/* P4OS: no decompression buffer nor sector map any more. */
}

/* Shared eviction position for sprite cache (used by get_cached_sprite_ptr and prefetch) */
static int spr_cache_evict_pos = 0;

/* P4OS: the next slot to replace, passing over the ones the frame being
 * drawn needs; when every slot is needed (a frame bigger than the cache),
 * the next one round-robin, as before. */
static int evict_slot(void) {
	GFX_CACHE *gcache = &memory.vid.spr_cache;
	for (int tries = 0; tries < gcache->max_slot; tries++) {
		int a = spr_cache_evict_pos;
		if (++spr_cache_evict_pos >= gcache->max_slot) spr_cache_evict_pos = 0;
		if (!spr_stamp || spr_stamp[a] != spr_stamp_now) return a;
	}
	int a = spr_cache_evict_pos;
	if (++spr_cache_evict_pos >= gcache->max_slot) spr_cache_evict_pos = 0;
	return a;
}

static void spr_mark(Uint8 *p) {
	GFX_CACHE *gcache = &memory.vid.spr_cache;
	if (spr_stamp && p) spr_stamp[(p - gcache->data) >> spr_slot_shift] = spr_stamp_now;
}

Uint8 *get_cached_sprite_ptr(Uint32 tileno) {
	GFX_CACHE *gcache = &memory.vid.spr_cache;
	static int read_errors = 0;
	int tile_sh = ~((gcache->slot_size >> 7) - 1);

	int bank = ((tileno & tile_sh) / (gcache->slot_size >> 7));
	int a;

	if (gcache->ptr[bank]) {
		/* The bank is present in the cache */
		return gcache->ptr[bank];
	}
	/* We have to find a slot for this bank (P4OS: not one this frame needs) */
	a = evict_slot();
	if (spr_stamp) spr_stamp[a] = spr_stamp_now;

	/* P4OS: one bank read with fread from the converted tile file;
	 * a failed read leaves the bank blank rather than stopping the game.
	 * RetroESP32 read it with FATFS's disk_read through a sector map it
	 * built at load, which an app on P4OS cannot reach. */
	{
		Uint8 *dest = gcache->data + a * gcache->slot_size;
		gcache->misses++;
		gcache->reads++;
		if (ng_spr_read_banks(bank, 1, dest) != 0) {
			memset(dest, 0, gcache->slot_size);
			read_errors++;
			if (read_errors <= 10)
				printf("CACHE_ERR: bank %d read failed (tile %u, errors=%d)\n",
					   bank, tileno, read_errors);
		}
	}

	gcache->ptr[bank] = gcache->data + a * gcache->slot_size;

	if (gcache->usage[a] != -1) {
		gcache->ptr[gcache->usage[a]] = 0;
	}
	gcache->usage[a] = bank;
	return gcache->ptr[bank];
}

static void fix_value_init(void) {
	int x, y;
	for (x = 0; x < 40; x++) {
		for (y = 0; y < 32; y++) {
			fix_addr[x][y] = 0xea00 + (y << 1) + 64 * (x / 6);
		}
		fix_shift[x] = (5 - (x % 6));
	}
}

#define fix_add(x, y) ((((READ_WORD(memory.vid.ram + 0xEA00 + (((y-1)&31)*2 + 64 * (x/6))) >> (5-(x%6))*2) & 3) ^ 3))

/* Drawing function generation */
#define RENAME(name) name##_tile
#define PUTPIXEL(dst,src) dst=src
#include "video_template.h"

#define RENAME(name) name##_tile_50
#define PUTPIXEL(dst,src) dst=BLEND16_50(src,dst)
#include "video_template.h"

#define RENAME(name) name##_tile_25
#define PUTPIXEL(dst,src) dst=BLEND16_25(src,dst)
#include "video_template.h"

#ifdef PROCESSOR_ARM

static __inline__ void draw_tile_arm(unsigned int tileno, int sx, int sy, int zx, int zy,
		int color, int xflip, int yflip, unsigned char *bmp) {
	Uint32 pitch = 352/*buffer->pitch>>1*/;
	//static SDL_Rect blit_rect={0,0,16,16};

	if (zy == 16)
		ldda_y_skip = full_y_skip;
	else
		ldda_y_skip = dda_y_skip;

	//if (yskip==16) dda_y_skip_i=0xFFFE;
	if (zx == 16) {
		if (!xflip) {
			if (!yflip) {
				draw_tile_arm_norm(tileno, color, (unsigned short*) bmp + (sy) * pitch + sx, zy);
				//draw_tile_arm_norm(tileno,color,(unsigned short*)sprbuf->pixels,zy);
			} else {
				draw_tile_arm_yflip_norm(tileno, color, (unsigned short*) bmp + ((zy - 1) + sy) * pitch + sx, zy);
				//draw_tile_arm_yflip_norm(tileno,color,(unsigned short*)sprbuf->pixels+(zy-1)*32,zy);
			}
		} else {
			if (!yflip) {
				//draw_tile_arm_xflip_norm(tileno,color,(unsigned short*)sprbuf->pixels,zy);
				draw_tile_arm_xflip_norm(tileno, color, (unsigned short*) bmp + (sy) * pitch + sx, zy);
			} else {
				draw_tile_arm_xyflip_norm(tileno, color, (unsigned short*) bmp + ((zy - 1) + sy) * pitch + sx, zy);
				//draw_tile_arm_xyflip_norm(tileno,color,(unsigned short*)sprbuf->pixels+(zy-1)*32,zy);
			}
		}
	} else {
		dda_x_skip_i = ddaxskip_i[zx];
		/*
		  draw_tile(tileno,sx+16,sy,rzx,yskip,tileatr>>8,
		  tileatr & 0x01,tileatr & 0x02,
		  (unsigned char*)buffer->pixels);
		 */
		if (!xflip) {
			if (!yflip) {
				draw_tile_arm_xzoom(tileno, color,
						(unsigned short*) bmp + (sy) * pitch + sx,
						zy);
			} else {
				draw_tile_arm_yflip_xzoom(tileno, color,
						(unsigned short*) bmp + ((zy - 1) + sy) * pitch + sx,
						zy);
			}
		} else {
			if (!yflip) {
				draw_tile_arm_xflip_xzoom(tileno, color,
						(unsigned short*) bmp + (sy) * pitch + sx,
						zy);
			} else {
				draw_tile_arm_xyflip_xzoom(tileno, color,
						(unsigned short*) bmp + ((zy - 1) + sy) * pitch + sx,
						zy);
			}
		}
	}
}
#endif

static __inline__ void draw_fix_char(unsigned char *buf, int start, int end) {
	unsigned int *gfxdata, myword;
	int x, y, yy;
	unsigned char col;
	unsigned short *br;
	unsigned int *paldata;
	unsigned int byte1, byte2;
	int banked, garouoffsets[32];
	SDL_Rect clip;
	int ystart = 1, yend = 32;

	//banked = (current_fix == memory.rom.game_sfix.p && memory.rom.game_sfix.size > 0x1000) ? 1 : 0;
	banked = (current_fix == memory.rom.game_sfix.p && neogeo_fix_bank_type && memory.rom.game_sfix.size > 0x1000) ? 1 : 0;
	//if (banked && conf.rom_type==MVS_CMC42)
	if (banked && neogeo_fix_bank_type == 1) {
		int garoubank = 0;
		int k = 0;
		y = 0;

		while (y < 32) {
			if (READ_WORD(&memory.vid.ram[0xea00 + (k << 1)]) == 0x0200 &&
					(READ_WORD(&memory.vid.ram[0xeb00 + (k << 1)]) & 0xff00) == 0xff00) {

				garoubank = READ_WORD(&memory.vid.ram[0xeb00 + (k << 1)]) & 3;
				garouoffsets[y++] = garoubank;
			}
			garouoffsets[y++] = garoubank;
			k += 2;
		}
	}

	if (start != 0 && end != 0) {
		ystart = start >> 3;
		yend = (end >> 3) + 1;
		if (ystart < 1) ystart = 1;
		clip.x = 0;
		clip.y = start + 16;
		clip.w = buffer->w;
		clip.h = (end - start) + 16;
		SDL_SetClipRect(buffer, &clip);
	}

	for (y = ystart; y < yend; y++)
		for (x = 1; x < 39; x++) {
			byte1 = (READ_WORD(&memory.vid.ram[0xE000 + ((y + (x << 5)) << 1)]));
			byte2 = byte1 >> 12;
			byte1 = byte1 & 0xfff;

			if (banked) {
				switch (neogeo_fix_bank_type) {
					case 1:
						/* Garou, MSlug 3 */
						byte1 += 0x1000 * (garouoffsets[(y - 2)&31] ^ 3);
						break;
					case 2:
						byte1 += 0x1000 * fix_add(x, y);
						break;
				}
			}

			if ((byte1 >= (memory.rom.game_sfix.size >> 5)) || (fix_usage[byte1] == 0x00)) continue;

			br = (unsigned short*) buf + ((y << 3)) * buffer->w + (x << 3) + 16;
#ifdef PROCESSOR_ARM
			draw_one_char_arm(byte1, byte2, br);
#elif I386_ASM
			draw_one_char_i386(byte1, byte2, br);
#else
			paldata = (unsigned int *) &current_pc_pal[16 * byte2];
			gfxdata = (unsigned int *) &current_fix[ byte1 << 5];

			for (yy = 0; yy < 8; yy++) {
				myword = gfxdata[yy];
				col = (myword >> 28)&0xf;
				if (col) br[7] = paldata[col];
				col = (myword >> 24)&0xf;
				if (col) br[6] = paldata[col];
				col = (myword >> 20)&0xf;
				if (col) br[5] = paldata[col];
				col = (myword >> 16)&0xf;
				if (col) br[4] = paldata[col];
				col = (myword >> 12)&0xf;
				if (col) br[3] = paldata[col];
				col = (myword >> 8)&0xf;
				if (col) br[2] = paldata[col];
				col = (myword >> 4)&0xf;
				if (col) br[1] = paldata[col];
				col = (myword >> 0)&0xf;
				if (col) br[0] = paldata[col];
				br += buffer->w;
			}
#endif

		}
	if (start != 0 && end != 0) SDL_SetClipRect(buffer, NULL);
}

/* ── Sprite bank pre-fetch: batch-load missing SD banks before rendering ──
 * P4OS: RetroESP32's prefetch (walk the sprite list as draw_screen will,
 * collect the banks that are not in the cache, read them in order before
 * drawing), with the reads done by fread: the missing banks are sorted by
 * their place in the file and runs of consecutive banks that fit in
 * consecutive cache slots are read in one call. The list of missing banks
 * and the 'seen' bitmap live in the cache's own allocations
 * (ng_spr_scratch) instead of the stack. */
#define PREFETCH_MAX 512
Uint32 *ng_spr_scratch(Uint32 **seen_words);

static int cmp_u32(const void *a, const void *b) {
	Uint32 x = *(const Uint32 *)a, y = *(const Uint32 *)b;
	return x < y ? -1 : x > y;
}

static void prefetch_sprite_banks(void) {
	GFX_CACHE *gcache = &memory.vid.spr_cache;
	Uint32 *seen;
	Uint32 *missing = ng_spr_scratch(&seen);
	if (!gcache->data || !missing) return;

	Uint8 *vidram = memory.vid.ram;
	int tile_mask = ~((gcache->slot_size >> 7) - 1);
	int tiles_per_bank = gcache->slot_size >> 7;
	int total_banks = gcache->total_bank;
	int nmiss = 0;

	memset(seen, 0, ((total_banks + 31) >> 5) * sizeof(Uint32));
	/* P4OS: a new frame: its banks get this stamp as they are found */
	if (++spr_stamp_now == 0) spr_stamp_now = 1;

	int sx = 0, my = 0, rzx = 15;
	for (int count = 0; count < 0x300; count += 2) {
		unsigned int t1 = READ_WORD(&vidram[0x10400 + count]);
		unsigned int t2 = READ_WORD(&vidram[0x10800 + count]);
		unsigned int t3 = READ_WORD(&vidram[0x10000 + count]);

		if (t1 & 0x40) {
			sx += rzx;
			int zx = (t3 >> 8) & 0x0f;
			rzx = (zx != 15) ? zx + 1 : 16;
		} else {
			int zx = (t3 >> 8) & 0x0f;
			int rzy = t3 & 0xff;
			if (rzy == 0) continue;
			sx = (t2 >> 7);
			my = t1 & 0x3f;
			if (my > 0x20) my = 0x20;
			if (rzy < 0xff && my < 0x10 && my) {
				my = my * 255 / rzy;
				if (my > 0x10) my = 0x10;
			}
			rzx = (zx != 15) ? zx + 1 : 16;
		}

		if (my == 0) continue;
		if (sx >= 0x1F0) sx -= 0x200;
		if (sx >= 320) continue;

		unsigned int offs = count << 6;
		for (unsigned int y2 = 0; y2 < (unsigned)my; y2++) {
			unsigned int tileno = READ_WORD(&vidram[offs]);
			offs += 2;
			unsigned int tileatr = READ_WORD(&vidram[offs]);
			offs += 2;

			if (memory.nb_of_tiles > 0x10000 && (tileatr & 0x10)) tileno += 0x10000;
			if (memory.nb_of_tiles > 0x20000 && (tileatr & 0x20)) tileno += 0x20000;
			if (memory.nb_of_tiles > 0x40000 && (tileatr & 0x40)) tileno += 0x40000;
			if (tileatr & 0x8)
				tileno = (tileno & ~7) + ((tileno + neogeo_frame_counter) & 7);
			else if (tileatr & 0x4)
				tileno = (tileno & ~3) + ((tileno + neogeo_frame_counter) & 3);

			if (tileno > memory.nb_of_tiles) continue;

			Uint8 pen = PEN_USAGE(tileno);
			if (pen == TILE_INVISIBLE) continue;

			int bank = (tileno & tile_mask) / tiles_per_bank;
			if (bank >= total_banks) continue;
			int word = bank >> 5;
			Uint32 bit = 1u << (bank & 31);
			if (seen[word] & bit) continue;
			seen[word] |= bit;
			if (gcache->ptr[bank]) {
				gcache->hits++;
				spr_mark(gcache->ptr[bank]);
				continue;
			}
			if (nmiss < PREFETCH_MAX) missing[nmiss++] = bank;
		}
	}

	if (nmiss == 0) return;
	gcache->misses += nmiss;
	/* More banks than the cache holds would evict each other: keep the
	 * first ones, draw_screen reads the rest as it meets them. */
	if (nmiss > gcache->max_slot / 2) nmiss = gcache->max_slot / 2;

	qsort(missing, nmiss, sizeof(Uint32), cmp_u32);

	int i = 0;
	while (i < nmiss) {
		/* A run of consecutive banks, read in one call when the slots that
		 * take them are consecutive too: the first slot from evict_slot(),
		 * the next ones only while they are free to replace (P4OS). */
		int a = evict_slot();
		int run = 1;
		while (i + run < nmiss && run < 16 &&
		       missing[i + run] == missing[i] + run &&
		       a + run < gcache->max_slot &&
		       (!spr_stamp || spr_stamp[a + run] != spr_stamp_now))
			run++;
		if (a + run > spr_cache_evict_pos) {
			spr_cache_evict_pos = a + run;
			if (spr_cache_evict_pos >= gcache->max_slot) spr_cache_evict_pos = 0;
		}

		Uint8 *dest = gcache->data + a * gcache->slot_size;
		gcache->reads++;
		if (ng_spr_read_banks(missing[i], run, dest) != 0)
			memset(dest, 0, run * gcache->slot_size);

		for (int k = 0; k < run; k++) {
			if (gcache->usage[a + k] != -1)
				gcache->ptr[gcache->usage[a + k]] = 0;
			gcache->usage[a + k] = missing[i + k];
			gcache->ptr[missing[i + k]] = dest + k * gcache->slot_size;
			if (spr_stamp) spr_stamp[a + k] = spr_stamp_now;
		}
		i += run;
	}
}

void draw_screen(void) {
	int sx = 0, sy = 0, oy = 0, my = 0, zx = 1, rzy = 1;
	unsigned int offs, i, count, y;
	unsigned int tileno, tileatr, t1, t2, t3;
	char fullmode = 0;
	int ddax = 0, dday = 0, rzx = 15, yskip = 0;
	Uint8 *vidram = memory.vid.ram;
	Uint8 penusage;

	//    int drawtrans=0;

	/* Pre-fetch all sprite banks from SD card before rendering.
	 * This separates I/O from rendering for better L2 cache behavior
	 * and batches consecutive sector reads. P4OS: only when the tiles are
	 * streamed, and without RetroESP32's timing probes. */
	if (memory.vid.spr_cache.data)
		prefetch_sprite_banks();

	SDL_FillRect(buffer, NULL, current_pc_pal[4095]);
	SDL_LockSurface(buffer);

	/* Draw sprites */
	for (count = 0; count < 0x300; count += 2) {
		t3 = READ_WORD(&vidram[0x10000 + count]);
		t1 = READ_WORD(&vidram[0x10400 + count]);
		t2 = READ_WORD(&vidram[0x10800 + count]);

		//printf("%04x %04x %04x\n",t3,t1,t2);
		/* If this bit is set this new column is placed next to last one */
		if (t1 & 0x40) {
			sx += rzx; /* new x */


			//            if ( sx >= 0x1F0 )    /* x>496 => x-=512 */
			//                sx -= 0x200;

			/* Get new zoom for this column */
			zx = (t3 >> 8) & 0x0f;
			sy = oy; /* y pos = old y pos */
		} else { /* nope it is a new block */
			/* Sprite scaling */
			zx = (t3 >> 8) & 0x0f; /* zomm x */
			rzy = t3 & 0xff; /* zoom y */
			if (rzy == 0) continue;
			sx = (t2 >> 7); /* x pos */
			/*
					drawtrans=0;
					if (t2&0x7f) {
					printf("t2 0-6 set to %x for strip %x\n",t2&0x7f,count>>1);
					drawtrans=1;
					}*/

			/* Number of tiles in this strip */
			my = t1 & 0x3f;



			if (my == 0x20) fullmode = 1;
			else if (my >= 0x21) fullmode = 2; /* most games use 0x21, but */
				/* Alpha Mission II uses 0x3f */
			else fullmode = 0;

			sy = 0x200 - (t1 >> 7); /* sprite bank position */

			if (sy > 0x110) sy -= 0x200;

			if (fullmode == 2 || (fullmode == 1 && rzy == 0xff)) {
				while (sy < 0) sy += ((rzy + 1) << 1);
			}

			oy = sy; /* on se souvient de la position y */


			if (rzy < 0xff && my < 0x10 && my) {
				//my = (my<<8)/(rzy+1);
				my = my * 255 / rzy;
				if (my > 0x10) my = 0x10;
			}

			if (my > 0x20) my = 0x20;

			ddax = 0; /* =16; NS990110 neodrift fix */ /* setup x zoom */
		}

		/* No point doing anything if tile strip is 0 */
		if (my == 0) continue;

		/* Process x zoom */
		if (zx != 15) {
			dda_x_skip = ddaxskip[zx];
			rzx = zx + 1;

		} else rzx = 16;

		if (sx >= 0x1F0) sx -= 0x200;
		if (sx >= 320) continue;
		//if (sx<-16) continue;

		/* Setup y zoom */
		if (rzy == 255)
			yskip = 16;
		else
			dday = 0; /* =256; NS990105 mslug fix */

		offs = count << 6;

		/* my holds the number of tiles in each vertical multisprite block */
		for (y = 0; y < my; y++) {
			tileno = READ_WORD(&vidram[offs]);
			offs += 2;
			tileatr = READ_WORD(&vidram[offs]);
			offs += 2;


			if (memory.nb_of_tiles > 0x10000 && (tileatr & 0x10)) tileno += 0x10000;
			if (memory.nb_of_tiles > 0x20000 && (tileatr & 0x20)) tileno += 0x20000;
			if (memory.nb_of_tiles > 0x40000 && (tileatr & 0x40)) tileno += 0x40000;


			/* animation automatique */
			/*if (tileatr&0x80) printf("PLOP\n");*/
			if (tileatr & 0x8) {
				tileno = (tileno&~7)+((tileno + neogeo_frame_counter)&7);
			} else {
				if (tileatr & 0x4) {
					tileno = (tileno&~3)+((tileno + neogeo_frame_counter)&3);
				}
			}

			if (tileno > memory.nb_of_tiles) {
				//printf("Tno %04x Tat %04x %d\n",tileno,tileatr,memory.nb_of_tiles);
				continue;
			}


			if (fullmode == 2 || (fullmode == 1 && rzy == 0xff)) {

				if (sy >= 248) sy -= ((rzy + 1) << 1);
			} else if (fullmode == 1) {

				if (y == 0x10) sy -= ((rzy + 1) << 1);
			} else if (sy > 0x110) sy -= 0x200; // NS990105 mslug2 fix



			if (rzy != 255) {
				yskip = 0;
				dda_y_skip_i = 0;
				dda_y_skip[0] = 0;
				for (i = 0; i < 16; i++) {
					dda_y_skip[i + 1] = 0;
					dday -= (rzy + 1);
					if (dday <= 0) {
						dday += 256;
						yskip++;
						dda_y_skip[yskip]++;
					} else dda_y_skip[yskip]++;

					//if (dda_y_skip[i])
					//	    dda_y_skip_i=dda_y_skip_i|(1<<i);
				}
				//printf("%04x\n",dda_y_skip_i);

			}



			if (sx >= -16 && sx + 15 < 336 && sy >= 0 && sy + 15 < 256) {
				unsigned int orig_tileno = tileno;

				penusage = PEN_USAGE(tileno);
				/* P4OS: an invisible tile is not drawn: do not read its
				 * bank either. */
				if (memory.vid.spr_cache.data && penusage != TILE_INVISIBLE) {
					memory.rom.tiles.p = get_cached_sprite_ptr(tileno);
					tileno = (tileno & ((memory.vid.spr_cache.slot_size >> 7) - 1));
				}

#ifdef PROCESSOR_ARM
				mem_gfx = memory.rom.tiles.p;
				//if (memory.pen_usage[tileno]!=TILE_INVISIBLE)
				if (penusage != TILE_INVISIBLE)
					draw_tile_arm(tileno, sx + 16, sy, rzx, yskip, tileatr >> 8,
						tileatr & 0x01, tileatr & 0x02,
						(unsigned char*) buffer->pixels);
#elif I386_ASM
				mem_gfx = &memory.rom.tiles.p;
				//switch (memory.pen_usage[tileno]) {
				switch (penusage) {
					case TILE_NORMAL:
						//printf("%d %d %x %x %x %x\n",tileno,sx,count,t1,t2,t3);
						draw_tile_i386_norm(tileno, sx + 16, sy, rzx, yskip, tileatr >> 8,
								tileatr & 0x01, tileatr & 0x02,
								(unsigned char*) buffer->pixels);
						break;
					case TILE_TRANSPARENT50:
						draw_tile_i386_50(tileno, sx + 16, sy, rzx, yskip, tileatr >> 8,
								tileatr & 0x01, tileatr & 0x02,
								(unsigned char*) buffer->pixels);
						break;
						/* TODO: 25% transparency in i386 asm */
					case TILE_TRANSPARENT25:
						draw_tile_25(tileno, sx + 16, sy, rzx, yskip, tileatr >> 8,
								tileatr & 0x01, tileatr & 0x02,
								(unsigned char*) buffer->pixels);
						break;
					case TILE_INVISIBLE:
						//printf("INVISIBLE %08x\n",tileno);
						break;
				}
#else
				switch (penusage) {
					case TILE_NORMAL:
						draw_tile(tileno, sx + 16, sy, rzx, yskip, tileatr >> 8,
								tileatr & 0x01, tileatr & 0x02,
								(unsigned char*) buffer->pixels);
						break;
					case TILE_TRANSPARENT50:
						draw_tile_50(tileno, sx + 16, sy, rzx, yskip, tileatr >> 8,
								tileatr & 0x01, tileatr & 0x02,
								(unsigned char*) buffer->pixels);
						break;
					case TILE_TRANSPARENT25:
						draw_tile_25(tileno, sx + 16, sy, rzx, yskip, tileatr >> 8,
								tileatr & 0x01, tileatr & 0x02,
								(unsigned char*) buffer->pixels);
						break;
						/*
							default:
								{
									SDL_Rect r={sx+16,sy,rzx,yskip};
									SDL_FillRect(buffer,&r,0xFFAA);
								}
								//((Uint16*)(buffer->pixels))[sx+16+sy*356]=0xFFFF;
                    
								break;
						 */
				}
#endif
			}


			sy += yskip;
		} /* for y */
	} /* for count */

	draw_fix_char(buffer->pixels, 0, 0);

	SDL_UnlockSurface(buffer);

	/* P4OS: no on-screen message nor fps text (the app has its own), no
	 * timing probes, and no screen_update(): ng_core.c hands the frame to
	 * the host once the 68000's frame is over. */
}

void draw_screen_scanline(int start_line, int end_line, int refresh) {
	int sx = 0, sy = 0, my = 0, zx = 1, zy = 1;
	int offs, count, y;
	int tileno, tileatr;
	int tctl1, tctl2, tctl3;
	unsigned char *vidram = memory.vid.ram;
	static SDL_Rect clear_rect;
	int yy;
	int otile, tile, yoffs;
	int zoom_line;
	int invert;
	Uint8 *zoomy_rom;
	Uint8 penusage;

	if (start_line > 255) start_line = 255;
	if (end_line > 255) end_line = 255;

	clear_rect.x = visible_area.x;
	clear_rect.w = visible_area.w;
	clear_rect.y = start_line;
	clear_rect.h = end_line - start_line + 1;


	SDL_FillRect(buffer, &clear_rect, current_pc_pal[4095]);

	/* Draw sprites */
	for (count = 0; count < 0x300; count += 2) {


		tctl3 = READ_WORD(&vidram[0x10000 + count]);
		tctl1 = READ_WORD(&vidram[0x10400 + count]);
		tctl2 = READ_WORD(&vidram[0x10800 + count]);

		/* If this bit is set this new column is placed next to last one */
		if (tctl1 & 0x40) {
			sx += zx + 1; /* new x */


			/* Get new zoom for this column */
			zx = (tctl3 >> 8) & 0x0f;
		} else { /* nope it is a new block */
			/* Sprite scaling */
			zx = (tctl3 >> 8) & 0x0f; /* zomm x */
			zy = tctl3 & 0xff; /* zoom y */

			sx = (tctl2 >> 7); /* x pos 0 - 512  */


			/* Number of tiles in this strip */
			my = tctl1 & 0x3f;

			sy = 512 - (tctl1 >> 7); /* y pos 512 - 0 */

			if (my > 0x20) my = 0x20;
		}


		/* No point doing anything if tile strip is 0 */
		if (my == 0) continue;
		if (sx >= 496) { /* after 496, we consider sx negative */
			//printf("SX=%d\n",sx);
			sx -= 512;
			//continue;
		}

		if (sx > 320) {
			continue;
			//sx-=16;
			//printf("SX=%d\n",sx);
		}

		if (sx<-16) continue;
		//sx&=0x1ff;

		/* Process x zoom */
		if (zx != 16) {
			dda_x_skip = ddaxskip[zx];
		} else zx = 16;



		offs = count << 6;
		zoomy_rom = memory.ng_lo + (zy << 8);

		otile = -1;
		for (yy = start_line; yy <= end_line; yy++) {
			y = yy - sy; /* y: 0 -> my*16 */

			if (y < 0) y += 512;

			if (y >= (my << 4)) continue;

			invert = 0;

			zoom_line = y & 0xff;

			if (y & 0x100) {
				zoom_line ^= 0xff; /* zoom_line = 255 - zoom_line */
				invert = 1;
			}

			if (my == 0x20) /* fix for joyjoy, trally... */ {
				if (zy) {
					zoom_line %= (zy << 1);
					if (zoom_line >= zy) {
						zoom_line = (zy << 1) - 1 - zoom_line;
						invert ^= 1;
					}
				}
			}

			yoffs = zoomy_rom[zoom_line] & 0x0f;
			tile = zoomy_rom[zoom_line] >> 4;

			if (invert) {
				tile ^= 0x1f; // tile=31 - tile;
				yoffs ^= 0x0f; // yoffs= 15 - yoffs;
			}

			tileno = READ_WORD(&vidram[offs + (tile << 2)]);
			tileatr = READ_WORD(&vidram[offs + (tile << 2) + 2]);

			if (memory.nb_of_tiles > 0x10000 && (tileatr & 0x10)) tileno += 0x10000;
			if (memory.nb_of_tiles > 0x20000 && (tileatr & 0x20)) tileno += 0x20000;
			if (memory.nb_of_tiles > 0x40000 && (tileatr & 0x40)) tileno += 0x40000;

			/* animation automatique */
			if (tileatr & 0x8) tileno = (tileno&~7)+((tileno + neogeo_frame_counter)&7);
			else if (tileatr & 0x4) tileno = (tileno&~3)+((tileno + neogeo_frame_counter)&3);
			if (tileatr & 0x02) yoffs ^= 0x0f; /* flip y */

			if (tileno > memory.nb_of_tiles) continue; /* P4OS: as draw_screen */
			penusage = PEN_USAGE(tileno);
			if (memory.vid.spr_cache.data && penusage != TILE_INVISIBLE) { /* P4OS */
				memory.rom.tiles.p = get_cached_sprite_ptr(tileno);
				tileno = (tileno & ((memory.vid.spr_cache.slot_size >> 7) - 1));
			}

			switch (penusage) {
#ifdef I386_ASM
					mem_gfx = &memory.rom.tiles.p;
				case TILE_NORMAL:
					draw_scanline_tile_i386_norm(tileno, yoffs, sx + 16, yy, zx, tileatr >> 8,
							tileatr & 0x01, (unsigned char*) buffer->pixels);
					break;
				case TILE_TRANSPARENT50:
					draw_scanline_tile_i386_50(tileno, yoffs, sx + 16, yy, zx, tileatr >> 8,
							tileatr & 0x01, (unsigned char*) buffer->pixels);
					break;
				case TILE_TRANSPARENT25:
					draw_scanline_tile_25(tileno, yoffs, sx + 16, yy, zx, tileatr >> 8,
							tileatr & 0x01, (unsigned char*) buffer->pixels);
					break;
#else
				case TILE_NORMAL:
					draw_scanline_tile(tileno, yoffs, sx + 16, yy, zx, tileatr >> 8,
							tileatr & 0x01, (unsigned char*) buffer->pixels);
					break;
				case TILE_TRANSPARENT50:
					draw_scanline_tile_50(tileno, yoffs, sx + 16, yy, zx, tileatr >> 8,
							tileatr & 0x01, (unsigned char*) buffer->pixels);
					break;
				case TILE_TRANSPARENT25:
					draw_scanline_tile_25(tileno, yoffs, sx + 16, yy, zx, tileatr >> 8,
							tileatr & 0x01, (unsigned char*) buffer->pixels);
					break;
#endif
			}


		}
	} /* for count */

	if (refresh) {
		/* P4OS: as draw_screen() (no message, no screen_update). */
		draw_fix_char(buffer->pixels, 0, 0);
	}
}

void init_video(void) {
#ifdef PROCESSOR_ARM
	if (!mem_gfx) {
		mem_gfx = memory.rom.tiles.p;
	}
	if (!mem_video) {
		mem_video = memory.vid.ram;
	}
#elif I386_ASM
	mem_gfx = &memory.rom.tiles.p;
	mem_video = memory.vid.ram;
#endif
	fix_value_init();
	memory.vid.modulo = 1;
}
