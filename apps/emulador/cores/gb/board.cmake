# The Game Boy and the Game Boy Color on the board: gnuboy (cores/gb/gnuboy,
# as Retro-Go carries it) and the adapter (gb_core.c), as the static library
# emu_core_gb that apps/emulador/CMakeLists.txt links into emulador.so. One
# library, two consoles: the tables emu_core_gb and emu_core_gbc.
# Included from that file's scope; emu_core_board_lib() is in
# cores/emu_cores.cmake.

set(EMU_CORE_IDS gb gbc)

set(EMU_GB_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_GB_DIR}/sources.cmake)

emu_core_board_lib(gb ${EMU_GB_SRCS})

target_include_directories(emu_core_gb PRIVATE ${EMU_GB_DIR}/gnuboy)
# P4OS: gnuboy.h sends gnuboy's messages to the adapter (gnuboy_p4os_log).
target_compile_definitions(emu_core_gb PRIVATE P4OS)
# -O2 rather than the .so's -Os: the CPU's switch and the line renderer are
# the whole frame. Not upstream's -O3: on a 16 KB instruction cache the
# bigger code of -O3 is not obviously faster (the CPU loop alone grows).
target_compile_options(emu_core_gb PRIVATE -O2)
# gnuboy is third-party code with warnings we are not going to fix; the
# adapter is held to -Wall -Wextra.
set_source_files_properties(${EMU_GB_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_GB_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# Two globals: everything ends up in emulador.so next to the other cores,
# and gnuboy's names are generic (GB, cart, gnuboy_run...). After the
# archive is made, its objects are linked with -r into one, every global
# but the two tables is made local, and that single object replaces the
# archive's contents (as cores/gba does).
set(_gb_one ${CMAKE_BINARY_DIR}/cores/emu_core_gb_one.o)
set(_gb_objcopy ${CMAKE_OBJCOPY})
if(NOT _gb_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _gb_objcopy ${CMAKE_C_COMPILER})
endif()
add_custom_command(TARGET emu_core_gb POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_gb_one}.all "$<TARGET_OBJECTS:emu_core_gb>"
    COMMAND ${_gb_objcopy} --keep-global-symbol=emu_core_gb --keep-global-symbol=emu_core_gbc
            ${_gb_one}.all ${_gb_one}
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_gb>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_gb> ${_gb_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_gb>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_gb and emu_core_gbc the only globals of the Game Boy core")
