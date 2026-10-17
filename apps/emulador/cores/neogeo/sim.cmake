# The Neo Geo in the simulator (included by apps/emulador/sim.cmake, in
# sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set): the same sources and
# flags as board.cmake, built by the Mac's compiler.
#
# Symbols: the simulator links every app and every core into one
# executable, and gngeo's globals have generic names (memory, conf, regs,
# timers...). The core is compiled to objects, linked with -r into a single
# object that keeps only emu_core_neogeo global, and that object is the
# library the simulator links (as the GBA core does).

set(EMU_NEOGEO_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_NEOGEO_DIR}/sources.cmake)

add_library(emu_core_neogeo_sim_objs OBJECT ${EMU_NEOGEO_SRCS})
# BEFORE: gngeo's own SDL.h shim (gngeo/include) must win over the real SDL
# the simulator puts in every target's path.
target_include_directories(emu_core_neogeo_sim_objs BEFORE PRIVATE ${EMU_NEOGEO_INCLUDES})
target_include_directories(emu_core_neogeo_sim_objs PRIVATE ${EMU_MAIN_DIR})
target_compile_definitions(emu_core_neogeo_sim_objs PRIVATE AOS_SIM ${EMU_NEOGEO_DEFINES})
# -fno-common: a tentative definition would stay a common symbol, which
# the -r link below cannot make local.
target_compile_options(emu_core_neogeo_sim_objs PRIVATE -O2 -fno-common)
set_source_files_properties(${EMU_NEOGEO_VENDOR_SRCS} PROPERTIES
    COMPILE_OPTIONS "-w;-include;${EMU_NEOGEO_FORCED}")
set_source_files_properties(${EMU_NEOGEO_68K_SRCS} PROPERTIES
    COMPILE_OPTIONS "-w;-O3;-include;${EMU_NEOGEO_FORCED}")
set_source_files_properties(${EMU_NEOGEO_OWN_SRCS} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

set(_ng_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_neogeo_sim.o)
if(APPLE)
    # ld64 turns every global not exported into a local symbol with -r.
    add_custom_command(OUTPUT ${_ng_one}
        COMMAND ${CMAKE_C_COMPILER} -r -nostdlib -Wl,-exported_symbol,_emu_core_neogeo
                -o ${_ng_one} $<TARGET_OBJECTS:emu_core_neogeo_sim_objs>
        DEPENDS emu_core_neogeo_sim_objs $<TARGET_OBJECTS:emu_core_neogeo_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Neo Geo core into one object")
else()
    add_custom_command(OUTPUT ${_ng_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_ng_one}.all $<TARGET_OBJECTS:emu_core_neogeo_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_neogeo ${_ng_one}.all ${_ng_one}
        DEPENDS emu_core_neogeo_sim_objs $<TARGET_OBJECTS:emu_core_neogeo_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Neo Geo core into one object")
endif()
set_source_files_properties(${_ng_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_neogeo_sim STATIC ${_ng_one})
set_target_properties(emu_core_neogeo_sim PROPERTIES LINKER_LANGUAGE C)

list(APPEND AOS_SIM_APP_LIBS emu_core_neogeo_sim)
