# The Game Boy and the Game Boy Color in the simulator (included by
# apps/emulador/sim.cmake, in sim/CMakeLists.txt's scope, with EMU_MAIN_DIR
# set): the same sources as board.cmake, built by the Mac's compiler.
#
# Symbols: the simulator links every app and every core into one
# executable, and gnuboy's globals have generic names (GB, cart...). The
# core is compiled to objects, linked with -r into a single object that
# keeps only emu_core_gb and emu_core_gbc global, and that object is the
# library the simulator links (as cores/gba does).

set(EMU_CORE_IDS gb gbc)

set(EMU_GB_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_GB_DIR}/sources.cmake)

add_library(emu_core_gb_sim_objs OBJECT ${EMU_GB_SRCS})
target_include_directories(emu_core_gb_sim_objs PRIVATE
    ${EMU_MAIN_DIR} ${EMU_GB_DIR}/gnuboy)
target_compile_definitions(emu_core_gb_sim_objs PRIVATE AOS_SIM P4OS)
# -fno-common: a tentative definition would stay a common symbol, which
# the -r link below cannot make local.
target_compile_options(emu_core_gb_sim_objs PRIVATE -O2 -fno-common)
set_source_files_properties(${EMU_GB_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_GB_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

set(_gb_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_gb_sim.o)
if(APPLE)
    # ld64 turns every global not exported into a local symbol with -r.
    add_custom_command(OUTPUT ${_gb_one}
        COMMAND ${CMAKE_C_COMPILER} -r -nostdlib
                -Wl,-exported_symbol,_emu_core_gb -Wl,-exported_symbol,_emu_core_gbc
                -o ${_gb_one} $<TARGET_OBJECTS:emu_core_gb_sim_objs>
        DEPENDS emu_core_gb_sim_objs $<TARGET_OBJECTS:emu_core_gb_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Game Boy core into one object")
else()
    add_custom_command(OUTPUT ${_gb_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_gb_one}.all $<TARGET_OBJECTS:emu_core_gb_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_gb --keep-global-symbol=emu_core_gbc
                ${_gb_one}.all ${_gb_one}
        DEPENDS emu_core_gb_sim_objs $<TARGET_OBJECTS:emu_core_gb_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Game Boy core into one object")
endif()
set_source_files_properties(${_gb_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_gb_sim STATIC ${_gb_one})
set_target_properties(emu_core_gb_sim PROPERTIES LINKER_LANGUAGE C)

list(APPEND AOS_SIM_APP_LIBS emu_core_gb_sim)
