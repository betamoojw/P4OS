# The NES core for the board: nofrendo (cores/nes/nofrendo, as Retro-Go
# carries it) and the adapter, as the static library emu_core_nes that
# apps/emulador/CMakeLists.txt links into emulador.so. Included from that
# file's scope; emu_core_board_lib() is in cores/emu_cores.cmake.

set(EMU_NES_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_NES_DIR}/sources.cmake)

emu_core_board_lib(nes ${EMU_NES_SRCS})

target_include_directories(emu_core_nes PRIVATE ${EMU_NES_DIR} ${EMU_NES_DIR}/nofrendo)
# RETRO_GO: nofrendo takes its log and its CRC from nes_rg_shim.h.
target_compile_definitions(emu_core_nes PRIVATE RETRO_GO)
# -O2 rather than the .so's -Os: the CPU and PPU loops are the whole frame.
# nes_rename.h gives nofrendo's globals a prefix of their own (see there).
target_compile_options(emu_core_nes PRIVATE
    -O2 -include ${EMU_NES_DIR}/nes_rename.h)
# nofrendo is third-party code with warnings we are not going to fix; the
# adapter and the shim are held to -Wall -Wextra.
set_source_files_properties(${EMU_NES_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_NES_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
