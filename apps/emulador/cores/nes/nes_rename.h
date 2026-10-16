/*
 * P4OS - Emulador, NES core: nofrendo's global names, renamed.
 *
 * The whole app is one shared object on the board, and in the simulator
 * one executable with every other app and the other consoles' cores, so a
 * global named apu_init, input_update or mem_reset could meet another of
 * the same name (gpSP has cpu_*, video_*, input_* of its own). This header
 * is forced into every file of the core (-include, from board.cmake and
 * sim.cmake) and puts nes_ in front of each of nofrendo's globals that did
 * not already start with nes_, nes6502_ or nofrendo_. The sources are left
 * as they are.
 *
 * Made from the core's own objects: nm -g over them, every defined name.
 * A new vendored file with a new global needs its line here.
 */
#pragma once

#define apu_emulate            nes_apu_emulate
#define apu_fc_advance         nes_apu_fc_advance
#define apu_getcontext         nes_apu_getcontext
#define apu_getopt             nes_apu_getopt
#define apu_init               nes_apu_init
#define apu_process            nes_apu_process
#define apu_read               nes_apu_read
#define apu_reset              nes_apu_reset
#define apu_setcontext         nes_apu_setcontext
#define apu_setext             nes_apu_setext
#define apu_setopt             nes_apu_setopt
#define apu_shutdown           nes_apu_shutdown
#define apu_write              nes_apu_write
#define games_database         nes_games_database
#define input_connect          nes_input_connect
#define input_init             nes_input_init
#define input_read             nes_input_read
#define input_reset            nes_input_reset
#define input_update           nes_input_update
#define input_write            nes_input_write
#define map0_intf              nes_map0_intf
#define map10_intf             nes_map10_intf
#define map119_intf            nes_map119_intf
#define map11_intf             nes_map11_intf
#define map15_intf             nes_map15_intf
#define map160_intf            nes_map160_intf
#define map162_intf            nes_map162_intf
#define map163_intf            nes_map163_intf
#define map16_intf             nes_map16_intf
#define map176_intf            nes_map176_intf
#define map185_intf            nes_map185_intf
#define map18_intf             nes_map18_intf
#define map191_intf            nes_map191_intf
#define map192_intf            nes_map192_intf
#define map193_intf            nes_map193_intf
#define map194_intf            nes_map194_intf
#define map195_intf            nes_map195_intf
#define map19_intf             nes_map19_intf
#define map1_intf              nes_map1_intf
#define map206_intf            nes_map206_intf
#define map20_intf             nes_map20_intf
#define map21_intf             nes_map21_intf
#define map228_intf            nes_map228_intf
#define map229_intf            nes_map229_intf
#define map22_intf             nes_map22_intf
#define map231_intf            nes_map231_intf
#define map23_intf             nes_map23_intf
#define map24_intf             nes_map24_intf
#define map25_intf             nes_map25_intf
#define map2_intf              nes_map2_intf
#define map30_intf             nes_map30_intf
#define map31_intf             nes_map31_intf
#define map32_intf             nes_map32_intf
#define map33_intf             nes_map33_intf
#define map34_intf             nes_map34_intf
#define map3_intf              nes_map3_intf
#define map40_intf             nes_map40_intf
#define map41_intf             nes_map41_intf
#define map42_intf             nes_map42_intf
#define map46_intf             nes_map46_intf
#define map4_intf              nes_map4_intf
#define map50_intf             nes_map50_intf
#define map5_intf              nes_map5_intf
#define map64_intf             nes_map64_intf
#define map65_intf             nes_map65_intf
#define map66_intf             nes_map66_intf
#define map70_intf             nes_map70_intf
#define map71_intf             nes_map71_intf
#define map73_intf             nes_map73_intf
#define map74_intf             nes_map74_intf
#define map75_intf             nes_map75_intf
#define map76_intf             nes_map76_intf
#define map78_intf             nes_map78_intf
#define map79_intf             nes_map79_intf
#define map7_intf              nes_map7_intf
#define map85_intf             nes_map85_intf
#define map87_intf             nes_map87_intf
#define map93_intf             nes_map93_intf
#define map94_intf             nes_map94_intf
#define map9_intf              nes_map9_intf
#define mem_getbyte            nes_mem_getbyte
#define mem_getpage            nes_mem_getpage
#define mem_getword            nes_mem_getword
#define mem_init_              nes_mem_init_
#define mem_putbyte            nes_mem_putbyte
#define mem_reset              nes_mem_reset
#define mem_setpage            nes_mem_setpage
#define mem_shutdown           nes_mem_shutdown
#define mmc_bankchr            nes_mmc_bankchr
#define mmc_bankprg            nes_mmc_bankprg
#define mmc_init               nes_mmc_init
#define mmc_reset              nes_mmc_reset
#define mmc_shutdown           nes_mmc_shutdown
#define nsf_current_song       nes_nsf_current_song
#define ppu_dumpbg             nes_ppu_dumpbg
#define ppu_dumpoam            nes_ppu_dumpoam
#define ppu_enabled            nes_ppu_enabled
#define ppu_endline            nes_ppu_endline
#define ppu_getcontext         nes_ppu_getcontext
#define ppu_getnametable       nes_ppu_getnametable
#define ppu_getopt             nes_ppu_getopt
#define ppu_getpage            nes_ppu_getpage
#define ppu_init               nes_ppu_init
#define ppu_read               nes_ppu_read
#define ppu_renderline         nes_ppu_renderline
#define ppu_reset              nes_ppu_reset
#define ppu_setcontext         nes_ppu_setcontext
#define ppu_setlatchfunc       nes_ppu_setlatchfunc
#define ppu_setmirroring       nes_ppu_setmirroring
#define ppu_setnametable       nes_ppu_setnametable
#define ppu_setopt             nes_ppu_setopt
#define ppu_setpage            nes_ppu_setpage
#define ppu_setvreadfunc       nes_ppu_setvreadfunc
#define ppu_shutdown           nes_ppu_shutdown
#define ppu_write              nes_ppu_write
#define rom_free               nes_rom_free
#define rom_loadfile           nes_rom_loadfile
#define rom_loadmem            nes_rom_loadmem
#define rom_loadsram           nes_rom_loadsram
#define rom_savesram           nes_rom_savesram
#define state_load             nes_state_load
#define state_save             nes_state_save

/* Not in the firmware's symbol table (nes_rg_shim.c). */
#define strdup                 nes_rg_strdup
