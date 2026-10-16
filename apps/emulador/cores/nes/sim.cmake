# The NES core for the simulator: the same sources as cores/nes/board.cmake,
# built by the Mac's compiler into a static library the simulator links.
# Included from sim/CMakeLists.txt's scope (through apps/emulador/sim.cmake),
# so the list(APPEND) at the end reaches the simulator's AOS_SIM_APP_LIBS.

set(EMU_NES_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_NES_DIR}/sources.cmake)

add_library(emu_core_nes_sim STATIC ${EMU_NES_SRCS})
target_include_directories(emu_core_nes_sim PRIVATE
    ${EMU_MAIN_DIR} ${EMU_NES_DIR} ${EMU_NES_DIR}/nofrendo)
target_compile_definitions(emu_core_nes_sim PRIVATE AOS_SIM RETRO_GO)
# nes_rename.h gives nofrendo's globals a prefix of their own (see there),
# for every file of the core, the adapter included.
target_compile_options(emu_core_nes_sim PRIVATE
    -O2 -include ${EMU_NES_DIR}/nes_rename.h)
# nofrendo is third-party code with warnings we are not going to fix; the
# adapter and the shim keep the simulator's own.
set_source_files_properties(${EMU_NES_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_NES_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

list(APPEND AOS_SIM_APP_LIBS emu_core_nes_sim)
