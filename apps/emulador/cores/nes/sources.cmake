# The NES core's sources, for board.cmake and sim.cmake alike. Left out:
# nofrendo's nes/dis.c (a disassembler for debugging) and the mapper files
# that are only a comment pointing at map004.c, where those MMC3 variants
# live (an empty object makes ranlib complain on the Mac).

set(EMU_NES_VENDOR_SRCS
    ${EMU_NES_DIR}/nofrendo/nofrendo.c
    ${EMU_NES_DIR}/nofrendo/nes/apu.c
    ${EMU_NES_DIR}/nofrendo/nes/cpu.c
    ${EMU_NES_DIR}/nofrendo/nes/input.c
    ${EMU_NES_DIR}/nofrendo/nes/mem.c
    ${EMU_NES_DIR}/nofrendo/nes/mmc.c
    ${EMU_NES_DIR}/nofrendo/nes/nes.c
    ${EMU_NES_DIR}/nofrendo/nes/ppu.c
    ${EMU_NES_DIR}/nofrendo/nes/rom.c
    ${EMU_NES_DIR}/nofrendo/nes/state.c)
file(GLOB _emu_nes_mappers ${EMU_NES_DIR}/nofrendo/mappers/map*.c)
foreach(_m 074 119 176 191 192 194 195)
    list(REMOVE_ITEM _emu_nes_mappers ${EMU_NES_DIR}/nofrendo/mappers/map${_m}.c)
endforeach()
list(SORT _emu_nes_mappers)
list(APPEND EMU_NES_VENDOR_SRCS ${_emu_nes_mappers})

set(EMU_NES_OWN_SRCS
    ${EMU_NES_DIR}/nes_core.c
    ${EMU_NES_DIR}/nes_rg_shim.c)

set(EMU_NES_SRCS ${EMU_NES_VENDOR_SRCS} ${EMU_NES_OWN_SRCS})
