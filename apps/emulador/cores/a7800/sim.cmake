# The Atari 7800 in the simulator (included by apps/emulador/sim.cmake, in
# sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set): the same sources as
# board.cmake, built by the Mac's compiler.
#
# Symbols: the simulator links every app and every core into one
# executable, and ProSystem's globals have generic names. The core is compiled
# to objects, linked with -r into a single object that keeps only
# emu_core_a7800 global (ld64 makes every other global local), and that
# object is the library the simulator links.

set(EMU_A7800_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_A7800_DIR}/sources.cmake)

add_library(emu_core_a7800_sim_objs OBJECT ${EMU_A7800_SRCS})
target_include_directories(emu_core_a7800_sim_objs PRIVATE ${EMU_MAIN_DIR} ${EMU_A7800_INCLUDES})
target_compile_definitions(emu_core_a7800_sim_objs PRIVATE AOS_SIM)
# -fno-common: a tentative definition would stay a common symbol, which the
# -r link below cannot make local.
target_compile_options(emu_core_a7800_sim_objs PRIVATE -O2 -fno-common)
set_source_files_properties(${EMU_A7800_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_A7800_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

set(_a78_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_a7800_sim.o)
if(APPLE)
    add_custom_command(OUTPUT ${_a78_one}
        COMMAND ${CMAKE_C_COMPILER} -r -nostdlib -Wl,-exported_symbol,_emu_core_a7800
                -o ${_a78_one} $<TARGET_OBJECTS:emu_core_a7800_sim_objs>
        DEPENDS emu_core_a7800_sim_objs $<TARGET_OBJECTS:emu_core_a7800_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Atari 7800 core into one object")
else()
    add_custom_command(OUTPUT ${_a78_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_a78_one}.all $<TARGET_OBJECTS:emu_core_a7800_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_a7800 ${_a78_one}.all ${_a78_one}
        DEPENDS emu_core_a7800_sim_objs $<TARGET_OBJECTS:emu_core_a7800_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Atari 7800 core into one object")
endif()
set_source_files_properties(${_a78_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_a7800_sim STATIC ${_a78_one})
set_target_properties(emu_core_a7800_sim PROPERTIES LINKER_LANGUAGE C)

list(APPEND AOS_SIM_APP_LIBS emu_core_a7800_sim)
