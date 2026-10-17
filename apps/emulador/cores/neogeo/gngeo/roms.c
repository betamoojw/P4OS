#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
/* P4OS: only what the kept part uses */
#include "roms.h"
#include "emu.h"
#include "memory.h"
#include "video.h"
#include "transpack.h"
#include "gnutil.h"

/* Prototype */
void kof98_decrypt_68k(GAME_ROMS *r);
void kof99_decrypt_68k(GAME_ROMS *r);
void garou_decrypt_68k(GAME_ROMS *r);
void garouo_decrypt_68k(GAME_ROMS *r);
void mslug3_decrypt_68k(GAME_ROMS *r);
void kof2000_decrypt_68k(GAME_ROMS *r);
void kof2002_decrypt_68k(GAME_ROMS *r);
void matrim_decrypt_68k(GAME_ROMS *r);
void samsho5_decrypt_68k(GAME_ROMS *r);
void samsh5p_decrypt_68k(GAME_ROMS *r);
void mslug5_decrypt_68k(GAME_ROMS *r);
void kf2k3pcb_decrypt_s1data(GAME_ROMS *r);
void kf2k3pcb_decrypt_68k(GAME_ROMS *r);
void kof2003_decrypt_68k(GAME_ROMS *r);
void kof99_neogeo_gfx_decrypt(GAME_ROMS *r, int extra_xor);
void kof2000_neogeo_gfx_decrypt(GAME_ROMS *r, int extra_xor);
void cmc50_neogeo_gfx_decrypt(GAME_ROMS *r, int extra_xor);
void cmc42_neogeo_gfx_decrypt(GAME_ROMS *r, int extra_xor);
void neogeo_bootleg_cx_decrypt(GAME_ROMS *r);
void neogeo_bootleg_sx_decrypt(GAME_ROMS *r, int extra_xor);
void svcpcb_gfx_decrypt(GAME_ROMS *r);
void svcpcb_s1data_decrypt(GAME_ROMS *r);
void neo_pcm2_swap(GAME_ROMS *r, int value);
void neo_pcm2_snk_1999(GAME_ROMS *r, int value);
void neogeo_cmc50_m1_decrypt(GAME_ROMS *r);

static int need_decrypt = 1;

int neogeo_fix_bank_type = 0;
int neogeo_pvc_protection = 0;  /* set by init_mslug5/svc/kof2003, installed after mem68k_init */

int bankoffset_kof99[64] = {
	0x000000, 0x100000, 0x200000, 0x300000, 0x3cc000,
	0x4cc000, 0x3f2000, 0x4f2000, 0x407800, 0x507800, 0x40d000, 0x50d000,
	0x417800, 0x517800, 0x420800, 0x520800, 0x424800, 0x524800, 0x429000,
	0x529000, 0x42e800, 0x52e800, 0x431800, 0x531800, 0x54d000, 0x551000,
	0x567000, 0x592800, 0x588800, 0x581800, 0x599800, 0x594800, 0x598000,
	/* rest not used? */
};
/* addr,uncramblecode,.... */
Uint8 scramblecode_kof99[7] = {0xF0, 14, 6, 8, 10, 12, 5,};

int bankoffset_garou[64] = {
	0x000000, 0x100000, 0x200000, 0x300000, // 00
	0x280000, 0x380000, 0x2d0000, 0x3d0000, // 04
	0x2f0000, 0x3f0000, 0x400000, 0x500000, // 08
	0x420000, 0x520000, 0x440000, 0x540000, // 12
	0x498000, 0x598000, 0x4a0000, 0x5a0000, // 16
	0x4a8000, 0x5a8000, 0x4b0000, 0x5b0000, // 20
	0x4b8000, 0x5b8000, 0x4c0000, 0x5c0000, // 24
	0x4c8000, 0x5c8000, 0x4d0000, 0x5d0000, // 28
	0x458000, 0x558000, 0x460000, 0x560000, // 32
	0x468000, 0x568000, 0x470000, 0x570000, // 36
	0x478000, 0x578000, 0x480000, 0x580000, // 40
	0x488000, 0x588000, 0x490000, 0x590000, // 44
	0x5d0000, 0x5d8000, 0x5e0000, 0x5e8000, // 48
	0x5f0000, 0x5f8000, 0x600000, /* rest not used? */
};
Uint8 scramblecode_garou[7] = {0xC0, 5, 9, 7, 6, 14, 12,};
int bankoffset_garouo[64] = {
	0x000000, 0x100000, 0x200000, 0x300000, // 00
	0x280000, 0x380000, 0x2d0000, 0x3d0000, // 04
	0x2c8000, 0x3c8000, 0x400000, 0x500000, // 08
	0x420000, 0x520000, 0x440000, 0x540000, // 12
	0x598000, 0x698000, 0x5a0000, 0x6a0000, // 16
	0x5a8000, 0x6a8000, 0x5b0000, 0x6b0000, // 20
	0x5b8000, 0x6b8000, 0x5c0000, 0x6c0000, // 24
	0x5c8000, 0x6c8000, 0x5d0000, 0x6d0000, // 28
	0x458000, 0x558000, 0x460000, 0x560000, // 32
	0x468000, 0x568000, 0x470000, 0x570000, // 36
	0x478000, 0x578000, 0x480000, 0x580000, // 40
	0x488000, 0x588000, 0x490000, 0x590000, // 44
	0x5d8000, 0x6d8000, 0x5e0000, 0x6e0000, // 48
	0x5e8000, 0x6e8000, 0x6e8000, 0x000000, // 52
	0x000000, 0x000000, 0x000000, 0x000000, // 56
	0x000000, 0x000000, 0x000000, 0x000000, // 60
};
Uint8 scramblecode_garouo[7] = {0xC0, 4, 8, 14, 2, 11, 13,};

int bankoffset_mslug3[64] = {
	0x000000, 0x020000, 0x040000, 0x060000, // 00
	0x070000, 0x090000, 0x0b0000, 0x0d0000, // 04
	0x0e0000, 0x0f0000, 0x120000, 0x130000, // 08
	0x140000, 0x150000, 0x180000, 0x190000, // 12
	0x1a0000, 0x1b0000, 0x1e0000, 0x1f0000, // 16
	0x200000, 0x210000, 0x240000, 0x250000, // 20
	0x260000, 0x270000, 0x2a0000, 0x2b0000, // 24
	0x2c0000, 0x2d0000, 0x300000, 0x310000, // 28
	0x320000, 0x330000, 0x360000, 0x370000, // 32
	0x380000, 0x390000, 0x3c0000, 0x3d0000, // 36
	0x400000, 0x410000, 0x440000, 0x450000, // 40
	0x460000, 0x470000, 0x4a0000, 0x4b0000, // 44
	0x4c0000, /* rest not used? */
};
Uint8 scramblecode_mslug3[7] = {0xE4, 14, 12, 15, 6, 3, 9,};
int bankoffset_kof2000[64] = {
	0x000000, 0x100000, 0x200000, 0x300000, // 00
	0x3f7800, 0x4f7800, 0x3ff800, 0x4ff800, // 04
	0x407800, 0x507800, 0x40f800, 0x50f800, // 08
	0x416800, 0x516800, 0x41d800, 0x51d800, // 12
	0x424000, 0x524000, 0x523800, 0x623800, // 16
	0x526000, 0x626000, 0x528000, 0x628000, // 20
	0x52a000, 0x62a000, 0x52b800, 0x62b800, // 24
	0x52d000, 0x62d000, 0x52e800, 0x62e800, // 28
	0x618000, 0x619000, 0x61a000, 0x61a800, // 32
};
Uint8 scramblecode_kof2000[7] = {0xEC, 15, 14, 7, 3, 10, 5,};

/* P4OS: the loader's buffer went with the loader. */

//char romerror[1024];

/* Actuall Code */

/*
 TODO
 static DRIVER_INIT( fatfury2 )
 {
 DRIVER_INIT_CALL(neogeo);
 fatfury2_install_protection(machine);
 }

 static DRIVER_INIT( mslugx )
 {
 DRIVER_INIT_CALL(neogeo);
 mslugx_install_protection(machine);
 }

 */

int init_mslugx(GAME_ROMS *r) {
	unsigned int i;
	Uint8 *RAM = r->cpu_m68k.p;
	if (need_decrypt) {
		for (i = 0; i < r->cpu_m68k.size; i += 2) {
			if ((READ_WORD_ROM(&RAM[i + 0]) == 0x0243)
					&& (READ_WORD_ROM(&RAM[i + 2]) == 0x0001) && /* andi.w  #$1, D3 */
					(READ_WORD_ROM(&RAM[i + 4]) == 0x6600)) { /* bne xxxx */

				WRITE_WORD_ROM(&RAM[i + 4], 0x4e71);
				WRITE_WORD_ROM(&RAM[i + 6], 0x4e71);
			}
		}

		WRITE_WORD_ROM(&RAM[0x3bdc], 0x4e71);
		WRITE_WORD_ROM(&RAM[0x3bde], 0x4e71);
		WRITE_WORD_ROM(&RAM[0x3be0], 0x4e71);
		WRITE_WORD_ROM(&RAM[0x3c0c], 0x4e71);
		WRITE_WORD_ROM(&RAM[0x3c0e], 0x4e71);
		WRITE_WORD_ROM(&RAM[0x3c10], 0x4e71);

		WRITE_WORD_ROM(&RAM[0x3c36], 0x4e71);
		WRITE_WORD_ROM(&RAM[0x3c38], 0x4e71);
	}
	return 0;
}

int init_kof99(GAME_ROMS *r) {
	if (need_decrypt) {
		kof99_decrypt_68k(r);
		kof99_neogeo_gfx_decrypt(r, 0x00);
	}
	neogeo_fix_bank_type = 0;
	memory.bksw_offset = bankoffset_kof99;
	memory.bksw_unscramble = scramblecode_kof99;
	memory.sma_rng_addr = 0xF8FA;
	//kof99_install_protection(machine);
	return 0;
}

int init_kof99n(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0x00);
	return 0;
}

int init_garou(GAME_ROMS *r) {
	if (need_decrypt) {
		garou_decrypt_68k(r);
		kof99_neogeo_gfx_decrypt(r, 0x06);
	}
	neogeo_fix_bank_type = 1;
	memory.bksw_offset = bankoffset_garou;
	memory.bksw_unscramble = scramblecode_garou;
	memory.sma_rng_addr = 0xCCF0;
	//garou_install_protection(machine);
	DEBUG_LOG("I HAS INITIALIZD GAROU\n");
	return 0;
}

int init_garouo(GAME_ROMS *r) {
	if (need_decrypt) {
		garouo_decrypt_68k(r);
		kof99_neogeo_gfx_decrypt(r, 0x06);
	}
	neogeo_fix_bank_type = 1;
	memory.bksw_offset = bankoffset_garouo;
	memory.bksw_unscramble = scramblecode_garouo;
	memory.sma_rng_addr = 0xCCF0;

	//garouo_install_protection(machine);
	return 0;
}

/*
 int init_garoup(GAME_ROMS *r) {
 garou_decrypt_68k(r);
 kof99_neogeo_gfx_decrypt(r, 0x06);

 return 0;
 }
 */
int init_garoubl(GAME_ROMS *r) {
	/* TODO: Bootleg support */
	if (need_decrypt) {
		neogeo_bootleg_sx_decrypt(r, 2);
		neogeo_bootleg_cx_decrypt(r);
	}
	return 0;
}

int init_mslug3(GAME_ROMS *r) {
	printf("INIT MSLUG3\n");
	if (need_decrypt) {
		mslug3_decrypt_68k(r);
		kof99_neogeo_gfx_decrypt(r, 0xad);
	}
	neogeo_fix_bank_type = 1;
	memory.bksw_offset = bankoffset_mslug3;
	memory.bksw_unscramble = scramblecode_mslug3;
	//memory.sma_rng_addr=0xF8FA;
	memory.sma_rng_addr = 0;

	//mslug3_install_protection(r);
	return 0;
}

int init_mslug3h(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0xad);
	return 0;
}

int init_mslug3b6(GAME_ROMS *r) {
	/* TODO: Bootleg support */
	if (need_decrypt) {
		neogeo_bootleg_sx_decrypt(r, 2);
		cmc42_neogeo_gfx_decrypt(r, 0xad);
	}
	return 0;
}

int init_kof2000(GAME_ROMS *r) {
	if (need_decrypt) {
		kof2000_decrypt_68k(r);
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0x00);
	}
	neogeo_fix_bank_type = 2;
	memory.bksw_offset = bankoffset_kof2000;
	memory.bksw_unscramble = scramblecode_kof2000;
	memory.sma_rng_addr = 0xD8DA;
	//kof2000_install_protection(r);
	return 0;

}

int init_kof2000n(GAME_ROMS *r) {
	neogeo_fix_bank_type = 2;
	if (need_decrypt) {
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0x00);
	}
	return 0;
}

int init_kof2001(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) {
		kof2000_neogeo_gfx_decrypt(r, 0x1e);
		neogeo_cmc50_m1_decrypt(r);
	}
	return 0;

}

/*

 TODO:
 static DRIVER_INIT( cthd2003 )
 {
 decrypt_cthd2003(machine);
 DRIVER_INIT_CALL(neogeo);
 patch_cthd2003(machine);
 }

 static DRIVER_INIT ( ct2k3sp )
 {
 decrypt_ct2k3sp(machine);
 DRIVER_INIT_CALL(neogeo);
 patch_cthd2003(machine);
 }

 static DRIVER_INIT ( ct2k3sa )
 {
 decrypt_ct2k3sa(machine);
 DRIVER_INIT_CALL(neogeo);
 patch_ct2k3sa(machine);
 }

 */

int init_mslug4(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1; /* USA violent content screen is wrong --
							 * not a bug, confirmed on real hardware! */
	if (need_decrypt) {
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0x31);

		neo_pcm2_snk_1999(r, 8);
	}
	return 0;

}

int init_ms4plus(GAME_ROMS *r) {
	if (need_decrypt) {
		cmc50_neogeo_gfx_decrypt(r, 0x31);
		neo_pcm2_snk_1999(r, 8);
		neogeo_cmc50_m1_decrypt(r);
	}
	return 0;
}

int init_ganryu(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0x07);
	return 0;
}

int init_s1945p(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0x05);
	return 0;
}

int init_preisle2(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0x9f);
	return 0;
}

int init_bangbead(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0xf8);
	return 0;
}

int init_nitd(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0xff);
	return 0;
}

int init_zupapa(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0xbd);
	return 0;
}

int init_sengoku3(GAME_ROMS *r) {
	neogeo_fix_bank_type = 1;
	if (need_decrypt) kof99_neogeo_gfx_decrypt(r, 0xfe);
	return 0;
}

int init_kof98(GAME_ROMS *r) {
	if (need_decrypt) kof98_decrypt_68k(r);

	/* Enable kof98 protection, start at state 0 (return original ROM values) */
	memory.kof98_prot = 1;
	memory.kof98_prot_state = 0;

	return 0;
}

int init_rotd(GAME_ROMS *r) {
	if (need_decrypt) {
		neo_pcm2_snk_1999(r, 16);
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0x3f);
	}
	neogeo_fix_bank_type = 1;
	return 0;
}

int init_kof2002(GAME_ROMS *r) {
	if (need_decrypt) {
		kof2002_decrypt_68k(r);
		neo_pcm2_swap(r, 0);
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0xec);
	}
	return 0;
}

int init_kof2002b(GAME_ROMS *r) {
	/* TODO: Bootleg */
	if (need_decrypt) {
		kof2002_decrypt_68k(r);
		neo_pcm2_swap(r, 0);
		neogeo_cmc50_m1_decrypt(r);
		//kof2002b_gfx_decrypt(r, r->tiles.p,0x4000000);
		//kof2002b_gfx_decrypt(r, r->game_sfix.p,0x20000);
	}
	return 0;
}

int init_kf2k2pls(GAME_ROMS *r) {
	if (need_decrypt) {
		kof2002_decrypt_68k(r);
		neo_pcm2_swap(r, 0);
		neogeo_cmc50_m1_decrypt(r);
		cmc50_neogeo_gfx_decrypt(r, 0xec);
	}
	return 0;
}

int init_kf2k2mp(GAME_ROMS *r) {
	/* TODO: Bootleg */
	if (need_decrypt) {
		//kf2k2mp_decrypt(r);
		neo_pcm2_swap(r, 0);
		//neogeo_bootleg_sx_decrypt(r, 2);
		cmc50_neogeo_gfx_decrypt(r, 0xec);
	}
	return 0;
}

int init_kof2km2(GAME_ROMS *r) {
	/* TODO: Bootleg */
	if (need_decrypt) {
		//kof2km2_px_decrypt(r);
		neo_pcm2_swap(r, 0);
		//neogeo_bootleg_sx_decrypt(r, 1);
		cmc50_neogeo_gfx_decrypt(r, 0xec);
	}
	return 0;
}

/*

 TODO
 static DRIVER_INIT( kof10th )
 {
 decrypt_kof10th(machine);
 DRIVER_INIT_CALL(neogeo);
 install_kof10th_protection(machine);
 }

 static DRIVER_INIT( kf10thep )
 {
 decrypt_kf10thep(machine);
 DRIVER_INIT_CALL(neogeo);
 }

 static DRIVER_INIT( kf2k5uni )
 {
 decrypt_kf2k5uni(machine);
 DRIVER_INIT_CALL(neogeo);
 }

 static DRIVER_INIT( kof2k4se )
 {
 decrypt_kof2k4se_68k(machine);
 DRIVER_INIT_CALL(neogeo);
 }

 static DRIVER_INIT( matrimbl )
 {
 matrim_decrypt_68k(machine);
 neogeo_fixed_layer_bank_type = 2;
 matrimbl_decrypt(machine);
 neogeo_sfix_decrypt(machine);
 DRIVER_INIT_CALL(neogeo);
 }

 */

int init_matrim(GAME_ROMS *r) {
	if (need_decrypt) {
		matrim_decrypt_68k(r);
		neo_pcm2_swap(r, 1);
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0x6a);
	}
	neogeo_fix_bank_type = 2;
	return 0;
}

int init_pnyaa(GAME_ROMS *r) {
	if (need_decrypt) {
		neo_pcm2_snk_1999(r, 4);
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0x2e);
	}
	neogeo_fix_bank_type = 1;
	return 0;
}

int init_mslug5(GAME_ROMS *r) {
	Uint8 *p = r->cpu_m68k.p;
	int rom_size = r->cpu_m68k.size;

	/* mslug5 P-ROMs use MAME load32_word_swap: two 4MB files interleaved
	 * at 16-bit word granularity within 32-bit longwords.
	 * p1 at offset 0, p2 at offset 2 within each 4-byte group.
	 * Our loader places p1 at 0x000000 and p2 at 0x400000 sequentially.
	 * Re-interleave: for each 4 bytes of output, take 2 bytes from p1 half
	 * and 2 bytes from p2 half. */
	if (rom_size == 0x800000) {
		int half = rom_size / 2; /* 0x400000 */
		Uint8 *buf = (Uint8 *)malloc(rom_size);
		if (buf) {
			memcpy(buf, p, rom_size);
			printf("MSLUG5: Interleaving P-ROMs (load32_word_swap)\n");
			for (int i = 0; i < half; i += 2) {
				/* Output offset: i*2 (since each 2-byte input pair produces 4 output bytes) */
				int out = i * 2;
				/* Word from p1 (first half) -> bytes 0-1 of each 4-byte group */
				p[out + 0] = buf[i + 0];
				p[out + 1] = buf[i + 1];
				/* Word from p2 (second half) -> bytes 2-3 of each 4-byte group */
				p[out + 2] = buf[half + i + 0];
				p[out + 3] = buf[half + i + 1];
			}
			free(buf);
			printf("MSLUG5: Interleave done. First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
				p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
		} else {
			printf("MSLUG5: WARNING - cannot allocate interleave buffer!\n");
		}
	} else {
		printf("MSLUG5: ROM size 0x%X != 0x800000, skipping interleave\n", rom_size);
	}

	/* Dump raw loaded bytes BEFORE any processing */
	printf("MSLUG5 RAW (before decrypt):\n");
	printf("  @0x100: %02X %02X %02X %02X  %02X %02X %02X %02X\n",
		p[0x100],p[0x101],p[0x102],p[0x103],p[0x104],p[0x105],p[0x106],p[0x107]);

	if (need_decrypt) {
		/* NO pre-swap: gngeo's neocrypt.c operates on raw BE data.
		 * swap_memory in cpu_68k_init handles LE conversion afterwards. */
		mslug5_decrypt_68k(r);
		neo_pcm2_swap(r, 2);
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0x19);
	}

	/* Dump key offsets after decrypt for debugging */
	printf("MSLUG5 after decrypt:\n");
	printf("  @0x000 (SSP/PC): %02X %02X %02X %02X  %02X %02X %02X %02X\n",
		p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
	printf("  @0x100 (header):  %02X %02X %02X %02X  %02X %02X %02X %02X\n",
		p[0x100],p[0x101],p[0x102],p[0x103],p[0x104],p[0x105],p[0x106],p[0x107]);
	printf("  @0x122 (USER):    %02X %02X %02X %02X  %02X %02X %02X %02X\n",
		p[0x122],p[0x123],p[0x124],p[0x125],p[0x126],p[0x127],p[0x128],p[0x129]);

	neogeo_fix_bank_type = 1;
	neogeo_pvc_protection = 1;
	return 0;
}

/*
 TODO:
 static TIMER_CALLBACK( ms5pcb_bios_timer_callback )
 {
 int harddip3 = input_port_read(machine, "HARDDIP") & 1;
 memory_set_bankptr(machine, NEOGEO_BANK_BIOS, memory_region(machine, "mainbios")
 +0x20000+harddip3*0x20000);
 }

 */
int init_ms5pcb(GAME_ROMS *r) {

	/* TODO: start a timer that will check the BIOS select DIP every second */
	//timer_set(machine, attotime_zero, NULL, 0, ms5pcb_bios_timer_callback);
	//timer_pulse(machine, ATTOTIME_IN_MSEC(1000), NULL, 0, ms5pcb_bios_timer_callback);
	if (need_decrypt) {
		mslug5_decrypt_68k(r);
		svcpcb_gfx_decrypt(r);
		neogeo_cmc50_m1_decrypt(r);
		kof2000_neogeo_gfx_decrypt(r, 0x19);
		svcpcb_s1data_decrypt(r);
		neo_pcm2_swap(r, 2);
	}
	neogeo_fix_bank_type = 2;
	neogeo_pvc_protection = 1;
	return 0;
}

int init_ms5plus(GAME_ROMS *r) {
	/* TODO: Bootleg */
	if (need_decrypt) {
		cmc50_neogeo_gfx_decrypt(r, 0x19);
		neo_pcm2_swap(r, 2);
		//neogeo_bootleg_sx_decrypt(r, 1);
	}
	neogeo_fix_bank_type = 1;

	//install_ms5plus_protection(r);
	return 0;
}
#if 0
// TODO:

static TIMER_CALLBACK(svcpcb_bios_timer_callback) {
	int harddip3 = input_port_read(machine, "HARDDIP") & 1;
	memory_set_bankptr(machine, NEOGEO_BANK_BIOS, memory_region(machine, "mainbios")
			+ 0x20000 + harddip3 * 0x20000);
}

static DRIVER_INIT(svcpcb) {
	/* start a timer that will check the BIOS select DIP every second */
	timer_set(machine, attotime_zero, NULL, 0, svcpcb_bios_timer_callback);
	timer_pulse(machine, ATTOTIME_IN_MSEC(1000), NULL, 0, svcpcb_bios_timer_callback);

	svc_px_decrypt(machine);
	svcpcb_gfx_decrypt(machine);
	neogeo_cmc50_m1_decrypt(machine);
	kof2000_neogeo_gfx_decrypt(machine, 0x57);
	svcpcb_s1data_decrypt(machine);
	neo_pcm2_swap(machine, 3);
	neogeo_fixed_layer_bank_type = 2;
	DRIVER_INIT_CALL(neogeo);
	install_pvc_protection(machine);
}

static DRIVER_INIT(svc) {
	svc_px_decrypt(machine);
	neo_pcm2_swap(machine, 3);
	neogeo_fixed_layer_bank_type = 2;
	neogeo_cmc50_m1_decrypt(machine);
	kof2000_neogeo_gfx_decrypt(machine, 0x57);
	DRIVER_INIT_CALL(neogeo);
	install_pvc_protection(machine);
}

static DRIVER_INIT(svcboot) {
	svcboot_px_decrypt(machine);
	svcboot_cx_decrypt(machine);
	DRIVER_INIT_CALL(neogeo);
	install_pvc_protection(machine);
}

static DRIVER_INIT(svcplus) {
	svcplus_px_decrypt(machine);
	svcboot_cx_decrypt(machine);
	neogeo_bootleg_sx_decrypt(machine, 1);
	svcplus_px_hack(machine);
	DRIVER_INIT_CALL(neogeo);
}

static DRIVER_INIT(svcplusa) {
	svcplusa_px_decrypt(machine);
	svcboot_cx_decrypt(machine);
	svcplus_px_hack(machine);
	DRIVER_INIT_CALL(neogeo);
}

static DRIVER_INIT(svcsplus) {
	svcsplus_px_decrypt(machine);
	neogeo_bootleg_sx_decrypt(machine, 2);
	svcboot_cx_decrypt(machine);
	svcsplus_px_hack(machine);
	DRIVER_INIT_CALL(neogeo);
	install_pvc_protection(machine);
}

static DRIVER_INIT(samsho5) {
	samsho5_decrypt_68k(machine);
	neo_pcm2_swap(machine, 4);
	neogeo_fixed_layer_bank_type = 1;
	neogeo_cmc50_m1_decrypt(machine);
	kof2000_neogeo_gfx_decrypt(machine, 0x0f);
	DRIVER_INIT_CALL(neogeo);
}

static DRIVER_INIT(samsho5b) {
	samsho5b_px_decrypt(machine);
	samsho5b_vx_decrypt(machine);
	neogeo_bootleg_sx_decrypt(machine, 1);
	neogeo_bootleg_cx_decrypt(machine);
	DRIVER_INIT_CALL(neogeo);
}

static DRIVER_INIT(kf2k3pcb) {
	kf2k3pcb_decrypt_68k(machine);
	kf2k3pcb_gfx_decrypt(machine);
	kof2003biosdecode(machine);
	neogeo_cmc50_m1_decrypt(machine);

	/* extra little swap on the m1 - this must be performed AFTER the m1 decrypt
	 or the m1 checksum (used to generate the key) for decrypting the m1 is
	 incorrect */
	{
		int i;
		UINT8* rom = memory_region(machine, "audiocpu");
		for (i = 0; i < 0x90000; i++) {
			rom[i] = BITSWAP8(rom[i], 5, 6, 1, 4, 3, 0, 7, 2);
		}

	}

	kof2000_neogeo_gfx_decrypt(machine, 0x9d);
	kf2k3pcb_decrypt_s1data(machine);
	neo_pcm2_swap(machine, 5);
	neogeo_fixed_layer_bank_type = 2;
	DRIVER_INIT_CALL(neogeo);
	install_pvc_protection(machine);
	memory_install_read16_handler(cputag_get_address_space(machine, "maincpu",
			ADDRESS_SPACE_PROGRAM), 0xc00000, 0xc7ffff, 0, 0,
			(read16_space_func) SMH_BANK(6)); // 512k bios
}

static DRIVER_INIT(kof2003) {
	kof2003_decrypt_68k(machine);
	neo_pcm2_swap(machine, 5);
	neogeo_fixed_layer_bank_type = 2;
	neogeo_cmc50_m1_decrypt(machine);
	kof2000_neogeo_gfx_decrypt(machine, 0x9d);
	DRIVER_INIT_CALL(neogeo);
	install_pvc_protection(machine);
}

static DRIVER_INIT(kof2003h) {
	kof2003h_decrypt_68k(machine);
	neo_pcm2_swap(machine, 5);
	neogeo_fixed_layer_bank_type = 2;
	neogeo_cmc50_m1_decrypt(machine);
	kof2000_neogeo_gfx_decrypt(machine, 0x9d);
	DRIVER_INIT_CALL(neogeo);
	install_pvc_protection(machine);
}

static DRIVER_INIT(kf2k3bl) {
	cmc50_neogeo_gfx_decrypt(machine, 0x9d);
	neo_pcm2_swap(machine, 5);
	neogeo_bootleg_sx_decrypt(machine, 1);
	DRIVER_INIT_CALL(neogeo);
	kf2k3bl_install_protection(machine);
}

static DRIVER_INIT(kf2k3pl) {
	cmc50_neogeo_gfx_decrypt(machine, 0x9d);
	neo_pcm2_swap(machine, 5);
	kf2k3pl_px_decrypt(machine);
	neogeo_bootleg_sx_decrypt(machine, 1);
	DRIVER_INIT_CALL(neogeo);
	kf2k3pl_install_protection(machine);
}

static DRIVER_INIT(kf2k3upl) {
	cmc50_neogeo_gfx_decrypt(machine, 0x9d);
	neo_pcm2_swap(machine, 5);
	kf2k3upl_px_decrypt(machine);
	neogeo_bootleg_sx_decrypt(machine, 2);
	DRIVER_INIT_CALL(neogeo);
	kf2k3upl_install_protection(machine);
}

static DRIVER_INIT(samsh5sp) {
	samsh5sp_decrypt_68k(machine);
	neo_pcm2_swap(machine, 6);
	neogeo_fixed_layer_bank_type = 1;
	neogeo_cmc50_m1_decrypt(machine);
	kof2000_neogeo_gfx_decrypt(machine, 0x0d);
	DRIVER_INIT_CALL(neogeo);
}

static DRIVER_INIT(jockeygp) {
	UINT16* extra_ram;

	neogeo_fixed_layer_bank_type = 1;
	neogeo_cmc50_m1_decrypt(machine);
	kof2000_neogeo_gfx_decrypt(machine, 0xac);

	/* install some extra RAM */
	extra_ram = auto_alloc_array(machine, UINT16, 0x2000 / 2);
	state_save_register_global_pointer(machine, extra_ram, 0x2000 / 2);

	memory_install_readwrite16_handler(cputag_get_address_space(machine, "maincpu",
			ADDRESS_SPACE_PROGRAM), 0x200000, 0x201fff, 0, 0,
			(read16_space_func) SMH_BANK(8), (write16_space_func) SMH_BANK(8));
	memory_set_bankptr(machine, NEOGEO_BANK_EXTRA_RAM, extra_ram);

	//  memory_install_read_port_handler(cputag_get_address_space(machine,
	//"maincpu", ADDRESS_SPACE_PROGRAM), 0x280000, 0x280001, 0, 0, "IN5");
	//  memory_install_read_port_handler(cputag_get_address_space(machine,
	//"maincpu", ADDRESS_SPACE_PROGRAM), 0x2c0000, 0x2c0001, 0, 0, "IN6");

	DRIVER_INIT_CALL(neogeo);
}

static DRIVER_INIT(vliner) {
	UINT16* extra_ram;

	/* install some extra RAM */
	extra_ram = auto_alloc_array(machine, UINT16, 0x2000 / 2);
	state_save_register_global_pointer(machine, extra_ram, 0x2000 / 2);

	memory_install_readwrite16_handler(cputag_get_address_space(machine, "maincpu",
			ADDRESS_SPACE_PROGRAM), 0x200000, 0x201fff, 0, 0, (read16_space_func)
			SMH_BANK(8), (write16_space_func) SMH_BANK(8));
	memory_set_bankptr(machine, NEOGEO_BANK_EXTRA_RAM, extra_ram);

	memory_install_read_port_handler(cputag_get_address_space(machine, "maincpu",
			ADDRESS_SPACE_PROGRAM), 0x280000, 0x280001, 0, 0, "IN5");
	memory_install_read_port_handler(cputag_get_address_space(machine, "maincpu",
			ADDRESS_SPACE_PROGRAM), 0x2c0000, 0x2c0001, 0, 0, "IN6");

	DRIVER_INIT_CALL(neogeo);
}

static DRIVER_INIT(kog) {
	/* overlay cartridge ROM */
	memory_install_read_port_handler(cputag_get_address_space(machine, "maincpu",
			ADDRESS_SPACE_PROGRAM), 0x0ffffe, 0x0fffff, 0, 0, "JUMPER");

	kog_px_decrypt(machine);
	neogeo_bootleg_sx_decrypt(machine, 1);
	neogeo_bootleg_cx_decrypt(machine);
	DRIVER_INIT_CALL(neogeo);
}

static DRIVER_INIT(lans2004) {
	lans2004_decrypt_68k(machine);
	lans2004_vx_decrypt(machine);
	neogeo_bootleg_sx_decrypt(machine, 1);
	neogeo_bootleg_cx_decrypt(machine);
	DRIVER_INIT_CALL(neogeo);
}

#endif

struct roms_init_func {
	char *name;
	int (*init)(GAME_ROMS * r);
} init_func_table[] = {
	//	{"mslugx",init_mslugx},
	{ "kof99", init_kof99},
	{ "kof99n", init_kof99n},
	{ "garou", init_garou},
	{ "garouo", init_garouo},
	//	{"garoup",init_garoup},
	{ "garoubl", init_garoubl},
	{ "mslug3", init_mslug3},
	{ "mslug3h", init_mslug3h},
	{ "mslug3n", init_mslug3h},
	{ "mslug3b6", init_mslug3b6},
	{ "kof2000", init_kof2000},
	{ "kof2000n", init_kof2000n},
	{ "kof2001", init_kof2001},
	{ "mslug4", init_mslug4},
	{ "ms4plus", init_ms4plus},
	{ "ganryu", init_ganryu},
	{ "s1945p", init_s1945p},
	{ "preisle2", init_preisle2},
	{ "bangbead", init_bangbead},
	{ "nitd", init_nitd},
	{ "zupapa", init_zupapa},
	{ "sengoku3", init_sengoku3},
	{ "kof98", init_kof98},
	{ "rotd", init_rotd},
	{ "kof2002", init_kof2002},
	{ "kof2002b", init_kof2002b},
	{ "kf2k2pls", init_kf2k2pls},
	{ "kf2k2mp", init_kf2k2mp},
	{ "kof2km2", init_kof2km2},
	{ "matrim", init_matrim},
	{ "pnyaa", init_pnyaa},
	{ "mslug5", init_mslug5},
	{ "ms5pcb", init_ms5pcb},
	{ "ms5plus", init_ms5plus},
	{ NULL, NULL}
};

/* Games that need CMC42/CMC50 GFX decryption — requires tiles-sized temp buffer.
 * If tiles are loaded in PSRAM (not streaming), neogeo_gfx_decrypt() needs to
 * malloc a buffer equal to tiles_size. This must be accounted for in the PSRAM
 * budget to avoid OOM crash during init_roms(). */
int game_needs_gfx_decrypt(const char *name) {
	int i = 0;
	while (init_func_table[i].name) {
		if (strcmp(init_func_table[i].name, name) == 0)
			return 1;
		i++;
	}
	return 0;
}

/* P4OS: what followed here in RetroESP32-P4's roms.c (allocate_region()
 * to the end: the zip loader, the streaming cache set-up through FATFS,
 * the BIOS loader, the .gno files, nvram/memcard files, init_game) is
 * replaced by ng_load.c. Kept: the per-game init functions above and the
 * tile and fix conversions below; init_roms() and
 * game_needs_gfx_decrypt() are no longer static. */

/* P4OS: not static: ng_load.c converts with it */
int convert_roms_tile(Uint8 *g, int tileno) {
	unsigned char swap[128];
	unsigned int *gfxdata;
	int x, y;
	unsigned int pen, usage = 0;
	gfxdata = (Uint32*) & g[tileno << 7];

	memcpy(swap, gfxdata, 128);

	//filed=1;
	for (y = 0; y < 16; y++) {
		unsigned int dw;

		dw = 0;
		for (x = 0; x < 8; x++) {
			pen = ((swap[64 + (y << 2) + 3] >> x) & 1) << 3;
			pen |= ((swap[64 + (y << 2) + 1] >> x) & 1) << 2;
			pen |= ((swap[64 + (y << 2) + 2] >> x) & 1) << 1;
			pen |= (swap[64 + (y << 2)] >> x) & 1;
			//if (!pen) filed=0;
			dw |= pen << ((7 - x) << 2);
			//memory.pen_usage[tileno]  |= (1 << pen);
			usage |= (1 << pen);
		}
		*(gfxdata++) = dw;

		dw = 0;
		for (x = 0; x < 8; x++) {
			pen = ((swap[(y << 2) + 3] >> x) & 1) << 3;
			pen |= ((swap[(y << 2) + 1] >> x) & 1) << 2;
			pen |= ((swap[(y << 2) + 2] >> x) & 1) << 1;
			pen |= (swap[(y << 2)] >> x) & 1;
			//if (!pen) filed=0;
			dw |= pen << ((7 - x) << 2);
			//memory.pen_usage[tileno]  |= (1 << pen);
			usage |= (1 << pen);
		}
		*(gfxdata++) = dw;
	}

	//if ((usage & ~1) == 0) pen_usage|=(TILE_INVISIBLE<<((tileno&0xF)*2));
	/* TODO transpack support */
	if ((usage & ~1) == 0)
		return (TILE_INVISIBLE << ((tileno & 0xF) * 2));
	else
		return 0;

}

/* P4OS: convert_all_tile() went with allocate_region(); ng_load.c
 * converts the tiles itself (in memory or into the cache file). */

void convert_all_char(Uint8 *Ptr, int Taille,
		Uint8 *usage_ptr) {
	int i, j;
	unsigned char usage;

	Uint8 *Src;
	Uint8 *sav_src;

	Src = (Uint8*) malloc(Taille);
	if (!Src) {
		printf("Not enought memory!!\n");
		return;
	}
	sav_src = Src;
	memcpy(Src, Ptr, Taille);
#ifdef WORDS_BIGENDIAN
#define CONVERT_TILE *Ptr++ = *(Src+8);\
	             usage |= *(Src+8);\
                     *Ptr++ = *(Src);\
		     usage |= *(Src);\
		     *Ptr++ = *(Src+24);\
		     usage |= *(Src+24);\
		     *Ptr++ = *(Src+16);\
		     usage |= *(Src+16);\
		     Src++;
#else
#define CONVERT_TILE *Ptr++ = *(Src+16);\
	             usage |= *(Src+16);\
                     *Ptr++ = *(Src+24);\
		     usage |= *(Src+24);\
		     *Ptr++ = *(Src);\
		     usage |= *(Src);\
		     *Ptr++ = *(Src+8);\
		     usage |= *(Src+8);\
		     Src++;
#endif
	for (i = Taille; i > 0; i -= 32) {
		usage = 0;
		for (j = 0; j < 8; j++) {
			CONVERT_TILE
		}
		Src += 24;
		*usage_ptr++ = usage;
	}
	free(sav_src);
#undef CONVERT_TILE
}

int init_roms(GAME_ROMS *r) {
	int i = 0;
	//printf("INIT ROM %s\n",r->info.name);
	neogeo_fix_bank_type = 0;
	neogeo_pvc_protection = 0;
	memory.bksw_handler = 0;
	memory.bksw_unscramble = NULL;
	memory.bksw_offset = NULL;
	memory.sma_rng_addr = 0;
	memory.kof98_prot = 0;

	while (init_func_table[i].name) {
		//printf("INIT ROM ? %s %s\n",init_func_table[i].name,r->info.name);
		if (strcmp(init_func_table[i].name, r->info.name) == 0
				&& init_func_table[i].init != NULL) {
			DEBUG_LOG("Special init func\n");
			return init_func_table[i].init(r);
		}
		i++;
	}
	DEBUG_LOG("Default roms init\n");
	return 0;
}
