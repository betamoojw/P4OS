# The PC Engine core for the board: pce-go and the adapter, as the static
# library emu_core_pce that apps/emulador/CMakeLists.txt links into
# emulador.so. Included from that file's scope; emu_core_board_lib() is in
# cores/emu_cores.cmake.

set(EMU_PCE_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_PCE_DIR}/sources.cmake)

emu_core_board_lib(pce ${EMU_PCE_SRCS})

target_include_directories(emu_core_pce PRIVATE ${EMU_PCE_INCS})
# -O2 rather than the .so's -Os: the HuC6280's switch and the tile and
# sprite loops are the frame (Retro-Go builds pce-go at -O3).
target_compile_options(emu_core_pce PRIVATE -O2)
# pce-go is third-party code with warnings we are not going to fix; the
# adapter is held to -Wall -Wextra.
set_source_files_properties(${EMU_PCE_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_PCE_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# One global: pce-go's names are generic (PCE, CPU, gfx_run, psg_update,
# LoadState...). The archive's objects are linked with -r into one, every
# global but emu_core_pce is made local, and that object replaces the
# archive's contents (as the GBA core does).
set(_pce_one ${CMAKE_BINARY_DIR}/cores/emu_core_pce_one.o)
set(_pce_objcopy ${CMAKE_OBJCOPY})
if(NOT _pce_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _pce_objcopy ${CMAKE_C_COMPILER})
endif()
add_custom_command(TARGET emu_core_pce POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_pce_one}.all "$<TARGET_OBJECTS:emu_core_pce>"
    COMMAND ${_pce_objcopy} --keep-global-symbol=emu_core_pce ${_pce_one}.all ${_pce_one}
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_pce>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_pce> ${_pce_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_pce>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_pce the only global of the PC Engine core")
