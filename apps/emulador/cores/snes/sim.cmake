# The Super Nintendo in the simulator (included by apps/emulador/sim.cmake,
# in sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set): the same sources
# and switches as board.cmake, built by the Mac's compiler.
#
# Symbols: the simulator links every app and every core into one
# executable, and Snes9x's globals have generic names (CPU, PPU, Memory,
# Settings, GFX...). The core is compiled to objects, linked with -r into a
# single object that keeps only emu_core_snes global, and that object is
# the library the simulator links (as the GBA core does).

set(EMU_SNES_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_SNES_DIR}/sources.cmake)

add_library(emu_core_snes_sim_objs OBJECT ${EMU_SNES_SRCS})
target_include_directories(emu_core_snes_sim_objs PRIVATE
    ${EMU_MAIN_DIR} ${EMU_SNES_DIR} ${EMU_SNES_DIR}/snes9x)
# The Mac reads a halfword from any address: FAST_LSB_WORD_ACCESS is on.
target_compile_definitions(emu_core_snes_sim_objs PRIVATE
    AOS_SIM ${EMU_SNES_DEFS} FAST_LSB_WORD_ACCESS)
# -fno-common: a tentative definition would stay a common symbol, which
# the -r link below cannot make local.
target_compile_options(emu_core_snes_sim_objs PRIVATE -O2 -fno-common)
set_source_files_properties(${EMU_SNES_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_SNES_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

set(_snes_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_snes_sim.o)
if(APPLE)
    # ld64 turns every global not exported into a local symbol with -r.
    add_custom_command(OUTPUT ${_snes_one}
        COMMAND ${CMAKE_C_COMPILER} -r -nostdlib -Wl,-exported_symbol,_emu_core_snes
                -o ${_snes_one} $<TARGET_OBJECTS:emu_core_snes_sim_objs>
        DEPENDS emu_core_snes_sim_objs $<TARGET_OBJECTS:emu_core_snes_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the SNES core into one object")
else()
    add_custom_command(OUTPUT ${_snes_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_snes_one}.all $<TARGET_OBJECTS:emu_core_snes_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_snes ${_snes_one}.all ${_snes_one}
        DEPENDS emu_core_snes_sim_objs $<TARGET_OBJECTS:emu_core_snes_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the SNES core into one object")
endif()
set_source_files_properties(${_snes_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_snes_sim STATIC ${_snes_one})
set_target_properties(emu_core_snes_sim PROPERTIES LINKER_LANGUAGE C)

list(APPEND AOS_SIM_APP_LIBS emu_core_snes_sim)
