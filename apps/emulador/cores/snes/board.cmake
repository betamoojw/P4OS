# The Super Nintendo on the board: Snes9x (cores/snes/snes9x, the
# snes9x2005 / CATSFC line as Retro-Go carries it) and the adapter, as the
# static library emu_core_snes that apps/emulador/CMakeLists.txt links into
# emulador.so. Included from that file's scope; emu_core_board_lib() is in
# cores/emu_cores.cmake.

# ---------------------------------------------------------------------------
# Switches.
#
# EMU_SNES_FAST_WORD: Snes9x reads 16-bit values (the 65816's operands, its
#   memory) with one halfword load at any address (FAST_LSB_WORD_ACCESS),
#   instead of two byte loads. Off until a misaligned halfword load has
#   been seen to work on this rev 1.3 chip: if the core does not do them in
#   hardware it traps, and ESP-IDF has no handler for that (a panic).
# EMU_SNES_SPLIT_SOUND: the sound of a frame is mixed by the other half of
#   run() (README.md, "The second core"). With the host's parallel() that
#   is the other core; until the host has one, a stub runs both halves in
#   series. 0 mixes the sound right after its frame, as Snes9x does.
set(EMU_SNES_FAST_WORD   0)
set(EMU_SNES_SPLIT_SOUND 1)
# ---------------------------------------------------------------------------

set(EMU_SNES_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_SNES_DIR}/sources.cmake)

emu_core_board_lib(snes ${EMU_SNES_SRCS})

target_include_directories(emu_core_snes PRIVATE ${EMU_SNES_DIR} ${EMU_SNES_DIR}/snes9x)
target_compile_definitions(emu_core_snes PRIVATE
    ${EMU_SNES_DEFS}
    $<$<BOOL:${EMU_SNES_FAST_WORD}>:FAST_LSB_WORD_ACCESS>
    $<$<NOT:$<BOOL:${EMU_SNES_SPLIT_SOUND}>>:SNES_NO_SPLIT>)
# -O2 rather than the .so's -Os: the CPU, sound CPU and renderer loops are
# the whole frame. Not -O3: the code runs from PSRAM through a 16 KB
# instruction cache, and spc700.c asks for O3 itself (a pragma).
target_compile_options(emu_core_snes PRIVATE -O2 -fno-common)
# Snes9x is third-party code with warnings we are not going to fix; the
# adapter is held to -Wall -Wextra.
set_source_files_properties(${EMU_SNES_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_SNES_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# One global: Snes9x's names are generic (CPU, PPU, APU, Memory, Settings,
# GFX, S9x*...) and everything ends up in emulador.so next to the other
# cores. After the archive is made, its objects are linked with -r into
# one, every global but emu_core_snes is made local, and that single object
# replaces the archive's contents (as the GBA core does).
set(_snes_one ${CMAKE_BINARY_DIR}/cores/emu_core_snes_one.o)
set(_snes_objcopy ${CMAKE_OBJCOPY})
if(NOT _snes_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _snes_objcopy ${CMAKE_C_COMPILER})
endif()
add_custom_command(TARGET emu_core_snes POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_snes_one}.all "$<TARGET_OBJECTS:emu_core_snes>"
    COMMAND ${_snes_objcopy} --keep-global-symbol=emu_core_snes ${_snes_one}.all ${_snes_one}
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_snes>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_snes> ${_snes_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_snes>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_snes the only global of the SNES core")
