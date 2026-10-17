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

/* genrator68k interface */



#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef USE_GENERATOR68K
#include <stdlib.h>

#include "generator68k/generator.h"
#include "generator68k/cpu68k.h"
#include "generator68k/reg68k.h"
#include "generator68k/mem68k.h"
#include "memory.h"
#include "emu.h"
#include "state.h"
#include "gnutil.h"
#include "ng_core.h" /* P4OS: ng_big_alloc, the save state */

extern unsigned int cpu68k_clocks;
extern Uint8 *cpu68k_rom;
extern unsigned int cpu68k_romlen;
extern Uint8 *cpu68k_ram;

extern Uint32 reg68k_pc;
extern t_sr reg68k_sr;

Uint8 *mem68k_memptr_bad(Uint32 addr);
Uint8 *mem68k_memptr_cpu(Uint32 addr);
Uint8 *mem68k_memptr_bios(Uint32 addr);
Uint8 *mem68k_memptr_cpu_bk(Uint32 addr);
Uint8 *mem68k_memptr_ram(Uint32 addr);

int diss68k_getdumpline(uint32 addr68k, uint8 *addr, char *dumpline);

t_mem68k_def mem68k_def[] = {
    /* P4OS: 0xfff (was 0x1000: one entry past the tables, which
     * mem68k_init() fills up to 'end' inclusive) */
    {0x000, 0xfff, mem68k_memptr_bad,
     mem68k_fetch_invalid_byte, mem68k_fetch_invalid_word,
     mem68k_fetch_invalid_long,
     mem68k_store_invalid_byte, mem68k_store_invalid_word,
     mem68k_store_invalid_long},

    /* RAM */
    {0x100, 0x1FF, mem68k_memptr_ram,
     mem68k_fetch_ram_byte, mem68k_fetch_ram_word, mem68k_fetch_ram_long,
     mem68k_store_ram_byte, mem68k_store_ram_word, mem68k_store_ram_long},

    /* BANKED CPU */
    {0x200, 0x2ff, mem68k_memptr_cpu_bk,
     NULL, NULL, NULL, NULL, NULL, NULL},

    /* CPU BANK 0 */
    {0x000, 0x0ff, mem68k_memptr_cpu,
     mem68k_fetch_cpu_byte, mem68k_fetch_cpu_word, mem68k_fetch_cpu_long,
     mem68k_store_invalid_byte, mem68k_store_invalid_word,
     mem68k_store_invalid_long},

    /* BIOS */
    {0xc00, 0xcFf, mem68k_memptr_bios,
     mem68k_fetch_bios_byte, mem68k_fetch_bios_word,
     mem68k_fetch_bios_long,
     mem68k_store_invalid_byte, mem68k_store_invalid_word,
     mem68k_store_invalid_long},

    /* SRAM */
    {0xd00, 0xdff, mem68k_memptr_bad,
     mem68k_fetch_sram_byte, mem68k_fetch_sram_word,
     mem68k_fetch_sram_long,
     mem68k_store_sram_byte, mem68k_store_sram_word,
     mem68k_store_sram_long},

    /* PAL */
    {0x400, 0x401, mem68k_memptr_bad,
     mem68k_fetch_pal_byte, mem68k_fetch_pal_word, mem68k_fetch_pal_long,
     mem68k_store_pal_byte, mem68k_store_pal_word, mem68k_store_pal_long},

    /* VIDEO */
    {0x3c0, 0x3c0, mem68k_memptr_bad,
     mem68k_fetch_video_byte, mem68k_fetch_video_word,
     mem68k_fetch_video_long,
     mem68k_store_video_byte, mem68k_store_video_word,
     mem68k_store_video_long},

    /* CONTRLOER 1 */
    {0x300, 0x300, mem68k_memptr_bad,
     mem68k_fetch_ctl1_byte, mem68k_fetch_ctl1_word,
     mem68k_fetch_ctl1_long,
     mem68k_store_invalid_byte, mem68k_store_invalid_word,
     mem68k_store_invalid_long},

    /* CONTRLOER 2 */
    {0x340, 0x340, mem68k_memptr_bad,
     mem68k_fetch_ctl2_byte, mem68k_fetch_ctl2_word,
     mem68k_fetch_ctl2_long,
     mem68k_store_invalid_byte, mem68k_store_invalid_word,
     mem68k_store_invalid_long},

    /* CONTROLER 3 + PD4990 */
    {0x380, 0x380, mem68k_memptr_bad,
     mem68k_fetch_ctl3_byte, mem68k_fetch_ctl3_word,
     mem68k_fetch_ctl3_long,
     mem68k_store_pd4990_byte, mem68k_store_pd4990_word,
     mem68k_store_pd4990_long},

    /* COIN + Z80 */
    {0x320, 0x320, mem68k_memptr_bad,
     mem68k_fetch_coin_byte, mem68k_fetch_coin_word,
     mem68k_fetch_coin_long,
     mem68k_store_z80_byte, mem68k_store_z80_word, mem68k_store_z80_long},

    /* MEMCARD */
    {0x800, 0x800, mem68k_memptr_bad,
     mem68k_fetch_memcrd_byte, mem68k_fetch_memcrd_word,
     mem68k_fetch_memcrd_long,
     mem68k_store_memcrd_byte, mem68k_store_memcrd_word,
     mem68k_store_memcrd_long},

    /* SETTING DIVER */
    {0x3A0, 0x3a0, mem68k_memptr_bad,
     mem68k_fetch_invalid_byte, mem68k_fetch_invalid_word,
     mem68k_fetch_invalid_long,
     mem68k_store_setting_byte, mem68k_store_setting_word,
     mem68k_store_setting_long},

    {0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL}

};

/* P4OS: allocated by cpu_68k_alloc() (7 x 0x1000 pointers, 112 KB on the
 * board) instead of .bss. */
Uint8 *(**mem68k_memptr) (Uint32 addr);
Uint8(**mem68k_fetch_byte) (Uint32 addr);
Uint16(**mem68k_fetch_word) (Uint32 addr);
Uint32(**mem68k_fetch_long) (Uint32 addr);
void (**mem68k_store_byte) (Uint32 addr, Uint8 data);
void (**mem68k_store_word) (Uint32 addr, Uint16 data);
void (**mem68k_store_long) (Uint32 addr, Uint32 data);
static void *s_mem68k_tables;

bool cpu_68k_alloc(void)
{
    const size_t n = 0x1000 * sizeof(void *);
    s_mem68k_tables = ng_big_alloc(7 * n);
    if (!s_mem68k_tables) return false;
    uint8_t *t = s_mem68k_tables;
    mem68k_memptr = (void *)(t + 0 * n);
    mem68k_fetch_byte = (void *)(t + 1 * n);
    mem68k_fetch_word = (void *)(t + 2 * n);
    mem68k_fetch_long = (void *)(t + 3 * n);
    mem68k_store_byte = (void *)(t + 4 * n);
    mem68k_store_word = (void *)(t + 5 * n);
    mem68k_store_long = (void *)(t + 6 * n);
    if (!cpu68k_alloc()) {
        cpu_68k_free();
        return false;
    }
    return true;
}

void cpu_68k_free(void)
{
    cpu68k_free();
    ng_big_free(s_mem68k_tables);
    s_mem68k_tables = NULL;
    mem68k_memptr = NULL;
    mem68k_fetch_byte = NULL;
    mem68k_fetch_word = NULL;
    mem68k_fetch_long = NULL;
    mem68k_store_byte = NULL;
    mem68k_store_word = NULL;
    mem68k_store_long = NULL;
}

static void swap_memory(Uint8 * mem, Uint32 length)
{
    int i, j;

    /* swap bytes in each word */
    for (i = 0; i < length; i += 2) {
	j = mem[i];
	mem[i] = mem[i + 1];
	mem[i + 1] = j;
    }
}

/*** initialise memory tables ***/

void bankswitcher_init() {
    mem68k_def[2].fetch_byte=mem68k_fetch_bk_normal_byte;
    mem68k_def[2].fetch_word=mem68k_fetch_bk_normal_word;
    mem68k_def[2].fetch_long=mem68k_fetch_bk_normal_long;
    mem68k_def[2].store_byte=mem68k_store_bk_normal_byte;
    mem68k_def[2].store_word=mem68k_store_bk_normal_word;
    mem68k_def[2].store_long=mem68k_store_bk_normal_long;
}

int mem68k_init(void)
{
    int i = 0;
    int j;
    bankswitcher_init();
    do {
	for (j = mem68k_def[i].start; j <= mem68k_def[i].end; j++) {
	    mem68k_memptr[j] = mem68k_def[i].memptr;
	    mem68k_fetch_byte[j] = mem68k_def[i].fetch_byte;
	    mem68k_fetch_word[j] = mem68k_def[i].fetch_word;
	    mem68k_fetch_long[j] = mem68k_def[i].fetch_long;
	    mem68k_store_byte[j] = mem68k_def[i].store_byte;
	    mem68k_store_word[j] = mem68k_def[i].store_word;
	    mem68k_store_long[j] = mem68k_def[i].store_long;
	}
	i++;
    }
    while ((mem68k_def[i].start != 0) || (mem68k_def[i].end != 0));
    return 0;
}

/*** memptr routines - called for IPC generation so speed is not vital ***/

Uint8 *mem68k_memptr_bad(Uint32 addr)
{
    return memory.rom.cpu_m68k.p;
}

Uint8 *mem68k_memptr_cpu(Uint32 addr)
{
    if (addr < cpu68k_romlen) {
	return (memory.rom.cpu_m68k.p + addr);
    }
    /* We should never reach this point */
    return memory.rom.cpu_m68k.p;
}

Uint8 *mem68k_memptr_bios(Uint32 addr)
{
    addr &= 0x1FFFF;
    return (memory.rom.bios_m68k.p + addr);
}

Uint8 *mem68k_memptr_cpu_bk(Uint32 addr)
{
    addr &= 0xFFFFF;
    //printf("mem68k_memptr_cpu_bk %x %x %d\n",addr,bankaddress,current_cpu_bank);
    return (memory.rom.cpu_m68k.p + addr + bankaddress);
}

Uint8 *mem68k_memptr_ram(Uint32 addr)
{
    addr &= 0xffff;
    return (neogeo_fast_ram + addr);
}


void cpu_68k_bankswitch(Uint32 address)
{

    bankaddress = address;
//    current_cpu_bank = data + 1;
};

void cpu_68k_reset(void)
{
    cpu68k_reset();
}

static Uint32 pc;


/* P4OS: the registers field by field (gngeo wrote the struct as it is in
 * memory), the clocks of the frame so far, the bank, and the IPC cache
 * dropped after a load (it is rebuilt as the code runs). */
void ng_68k_state(ng_st_t *st)
{
    uint32_t v[22];
    if (st->writing) {
        v[0] = regs.pc;
        v[1] = regs.sp;
        v[2] = regs.sr.sr_int;
        v[3] = regs.stop;
        for (int i = 0; i < 16; i++) v[4 + i] = regs.regs[i];
        v[20] = regs.pending;
        v[21] = cpu68k_clocks;
    }
    ng_st_data(st, v, sizeof(v));
    ng_st_data(st, &bankaddress, sizeof(bankaddress));
    if (!st->writing && !st->failed) {
        regs.pc = v[0];
        regs.sp = v[1];
        regs.sr.sr_int = (uint16)v[2];
        regs.stop = (uint16)v[3];
        for (int i = 0; i < 16; i++) regs.regs[i] = v[4 + i];
        regs.pending = (uint16)v[20];
        cpu68k_clocks = v[21];
        cpu68k_clearcache();
    }
}

/* P4OS: a BIOS read after cpu_68k_init() (the system option changed)
 * gets the same byte order as the first. */
void cpu_68k_init_bios(void)
{
    if (memory.rom.bios_m68k.p && memory.rom.bios_m68k.p[0] == 0x10)
        swap_memory(memory.rom.bios_m68k.p, memory.rom.bios_m68k.size);
    cpu68k_clearcache();
}

void cpu_68k_init(void)
{
    printf("GEN68k CPU INIT\n");
    //#ifdef WORDS_BIGENDIAN
    
	cpu68k_clearcache();

    { /* P4OS: no "dump" setting */
		swap_memory(memory.rom.cpu_m68k.p, memory.rom.cpu_m68k.size);

		if (memory.rom.bios_m68k.p[0]==0x10) {
			printf("BIOS BYTE1=%08x\n",memory.rom.bios_m68k.p[0]);
			swap_memory(memory.rom.bios_m68k.p, memory.rom.bios_m68k.size);
		}
		swap_memory(memory.game_vector, 0x80);
    }
    //#endif

    /* neogeo_fast_ram points to same buffer as memory.ram (PSRAM).
       Internal SRAM allocation not feasible — largest contiguous block is ~31KB,
       too small for 64KB work RAM. */
    neogeo_fast_ram = memory.ram;
    memset(neogeo_fast_ram, 0, 0x10000);

    cpu68k_ram = neogeo_fast_ram;
    cpu68k_rom = memory.rom.cpu_m68k.p;
    if (memory.rom.cpu_m68k.size < 0x100000)
	cpu68k_romlen = memory.rom.cpu_m68k.size;
    else
	cpu68k_romlen = 0x100000;
    mem68k_init();

    cpu68k_init();
    if (memory.rom.cpu_m68k.size > 0x100000) {
	cpu_68k_bankswitch(0x100000);
    }
    //cpu_68k_init_save_state();
}

/* P4OS: the clocks count from the start of the frame (cpu_68k_frame_start)
 * and not from the start of each call: the frame runs in slices, between
 * which the Z80 runs, and the 68000 reads the beam's line from them. */
int cpu_68k_run(Uint32 nb_cycle)
{
    return reg68k_external_execute(nb_cycle);
}

void cpu_68k_frame_start(void)
{
    cpu68k_endfield();
}

#if 0 /* P4OS: the console debugger needs stdin and the disassembler */
/* Debuger interface */
static Uint32 gen68k_disassemble(int pc, int nb_instr)
{
    int i;
    char buf[512];
    Uint8 *addr;
    //printf("%x\n",pc);
    for (i = 0; i < nb_instr; i++) {
	addr = mem68k_memptr[pc >> 12] (pc);
	pc += diss68k_getdumpline(pc, addr, buf)*2;
	printf("%s", buf);
    }
    return pc;
}

static void gen68k_dumpreg(void)
{
    int i;
    printf("d0=%08x   d4=%08x   a0=%08x   a4=%08x   %c%c%c%c%c %04x\n",
	   regs.regs[0],regs.regs[4],regs.regs[8],regs.regs[12],
	   ((regs.sr.sr_int >> 4) & 1 ? 'X' : '-'),
	   ((regs.sr.sr_int >> 3) & 1 ? 'N' : '-'),
	   ((regs.sr.sr_int >> 2) & 1 ? 'Z' : '-'),
	   ((regs.sr.sr_int >> 1) & 1 ? 'V' : '-'),
	   ((regs.sr.sr_int     ) & 1 ? 'C' : '-'),regs.sr.sr_int);
    printf("d1=%08x   d5=%08x   a1=%08x   a5=%08x\n",
	   regs.regs[1],regs.regs[5],regs.regs[9],regs.regs[13]);
    printf("d2=%08x   d6=%08x   a2=%08x   a6=%08x\n",
	   regs.regs[2],regs.regs[6],regs.regs[10],regs.regs[14]);
    printf("d3=%08x   d7=%08x   a3=%08x   a7=%08x   usp=%08x\n",
	   regs.regs[3],regs.regs[7],regs.regs[11],regs.regs[15],regs.sp);
    
}

static void hexdump(Uint32 addr) {
    Uint8 c, tmpchar[16];
    Uint32 tmpaddr;
    int i, j, k;
    tmpaddr = addr & 0xFFFFFFF0;
    for(i = 0; i < 8; i++) {
	printf("%08X: %c", tmpaddr,(addr == tmpaddr) ? '>' : ' ' );
	for(j = 0; j < 16; j += 2) {
	    k = fetchword(tmpaddr) & 0xFFFF;
#ifdef WORDS_BIGENDIAN
	    tmpchar[j + 1] = k >> 8;
	    tmpchar[j    ] = k & 0xFF;
#else
	    tmpchar[j    ] = k >> 8;
	    tmpchar[j + 1] = k & 0xFF;
#endif
	    tmpaddr += 2;
	    printf("%02X%02X%c",
		   tmpchar[j], tmpchar[j + 1],
		   (( addr            == tmpaddr   )&&(j!=14))?'>':
		   (((addr&0xFFFFFFFE)==(tmpaddr-2))?'<':' ')
		);
	}
	printf("  ");
	for(j = 0; j < 16; j++) {
	    c = tmpchar[j];
	    if((c<32)||(c>126))c='.';
	    printf("%c", c);
	}
	printf("\n");
    }
    //addr += 0x80;
}

#endif

Uint32 cpu_68k_getpc(void)
{
    return regs.pc;
}

#if 0 /* P4OS: debugger */
int cpu_68k_run_step(void)
{
    return reg68k_external_step();
}

int cpu_68k_debuger(void (*execstep)(void),void (*dump)(void)) {
    char buf[200];
    char *res;
    char *args,*argsend;
    Uint8 debug_end=0;
    Uint32 hex=0;
    Uint32 asmpc=0;
    int i;
    
    do{
	printf("cpu1> ");fflush(stdout);
	res=fgets(buf, 200, stdin);
	
	args = buf + 1;
	while((*args) && ((*args) < 32)) args++;
	
	switch (buf[0]) {
	case '?':
	    printf("B [address]           Add a breakpoint at [address]\n"
		   "T                     Show backtrace\n"
		   "N [address]           Del breakpoint at [address]\n"
		   "R                     Run until breakpoint\n"
		   "b [address]           Run continuously, break at PC=[address]\n"
		   "d [address]           Dump memory, starting at [address]\n"
		   "h                     Hardware dump\n"
		   "i [number]            Generate hardware interrupt [number]\n"
		   "j [address]           Jump directly to [address]\n"
		   "q                     Quit\n"
		   "r                     Show register dump and next instruction\n"
		   "t [hex number]        Trace through [hex number] instructions\n"
		   "u [address]           Unassemble code, starting at [address]\n");
	    break;
	case 'T':
	    show_bt();
	    break;
	case 'B':
	    if (args) {
		pc=strtoul(args,&argsend,0);
		if (args != argsend)
		    add_bp(pc);
		else
		    printf("Invalid input\n");
	    }
	    break;
	case 'N':
	    if (args) {
		pc=strtoul(args,&argsend,0);
		if (args != argsend)
		    del_bp(pc);
		else
		    printf("Invalid input\n");
	    }
	    break;
	case 'R':
	    while(check_bp(cpu_68k_getpc())!=GN_TRUE && dbg_step==0) {
		cpu_68k_dpg_step();
	    }
	    if (dbg_step) dbg_step=0;
	    gen68k_dumpreg();
	    gen68k_disassemble(regs.pc,1);
	    break;
	case 'b':
	    if (args) {
		pc=strtoul(args,&argsend,0);
		if (args != argsend) {
		    cpu_68k_dpg_step(); /* trace 1 */
		    while(cpu_68k_getpc()!=pc && dbg_step==0) {
			cpu_68k_dpg_step();
			//if (regs.regs[8]==0xf0f0f0f0) dbg_step=1;
		    }
		    if (dbg_step) dbg_step=0;
		    gen68k_dumpreg();
		    gen68k_disassemble(regs.pc,1);
		} else
		    printf("Invalid input\n");
	    }
	    break;
	case 'j':
	    if (args) {
		pc=strtoul(args,&argsend,0);
		if (args != argsend)
		    regs.pc=pc;
		else
		    printf("Invalid input\n");
	    }
	    break;
	case 'r':
	    gen68k_dumpreg();
	    gen68k_disassemble(regs.pc,1);
	    break;
	case 't':
	    if (args) {
		pc=strtoul(args,&argsend,0);
		if (args != argsend) {
		    for(i=0;i<pc && dbg_step==0;i++)
			cpu_68k_dpg_step();
		    if (dbg_step) dbg_step=0;
		    gen68k_dumpreg();
		    gen68k_disassemble(regs.pc,1);
		} else
		    printf("Invalid input\n");
	    }
	    break;
	case 'd':
	    if (args) {
		pc=strtoul(args,&argsend,0);
		if (args != argsend) {
		    hex=pc;
		}
		hexdump(hex);
		hex+=0x80;
	    }
	    
	    break;
	case 'u':
	    if (args) {
		pc=strtoul(args,&argsend,0);
		if (args != argsend) {
		    asmpc=pc;
		}
		asmpc=gen68k_disassemble(asmpc,16);
	    }
	    break;
	case 'h':
	  //dump_hardware_reg();
	  break;
	case 'q':
	    debug_end=1;
	    break;
	}
    }while(debug_end==0);

    return -1;
}




#endif

void cpu_68k_interrupt(int a)
{
    reg68k_external_autovector(a);
}

int cpu_68k_getcycle(void)
{
    return cpu68k_clocks;
}

#endif
