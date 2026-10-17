# The PC Engine in the simulator (included by apps/emulador/sim.cmake, in
# sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set): the same sources as
# board.cmake, linked with -r into a single object that keeps only
# emu_core_pce global, since the simulator links every app and every core
# into one executable and pce-go's names are generic.

set(EMU_PCE_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_PCE_DIR}/sources.cmake)

add_library(emu_core_pce_sim_objs OBJECT ${EMU_PCE_SRCS})
target_include_directories(emu_core_pce_sim_objs PRIVATE ${EMU_MAIN_DIR} ${EMU_PCE_INCS})
target_compile_definitions(emu_core_pce_sim_objs PRIVATE AOS_SIM)
# -fno-common: a tentative definition would stay a common symbol, which
# the -r link below cannot make local.
target_compile_options(emu_core_pce_sim_objs PRIVATE -O2 -fno-common)
set_source_files_properties(${EMU_PCE_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_PCE_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

set(_pce_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_pce_sim.o)
if(APPLE)
    # ld64 turns every global not exported into a local symbol with -r.
    add_custom_command(OUTPUT ${_pce_one}
        COMMAND ${CMAKE_C_COMPILER} -r -nostdlib -Wl,-exported_symbol,_emu_core_pce
                -o ${_pce_one} $<TARGET_OBJECTS:emu_core_pce_sim_objs>
        DEPENDS emu_core_pce_sim_objs $<TARGET_OBJECTS:emu_core_pce_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the PC Engine core into one object")
else()
    add_custom_command(OUTPUT ${_pce_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_pce_one}.all $<TARGET_OBJECTS:emu_core_pce_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_pce ${_pce_one}.all ${_pce_one}
        DEPENDS emu_core_pce_sim_objs $<TARGET_OBJECTS:emu_core_pce_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the PC Engine core into one object")
endif()
set_source_files_properties(${_pce_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_pce_sim STATIC ${_pce_one})
set_target_properties(emu_core_pce_sim PROPERTIES LINKER_LANGUAGE C)

list(APPEND AOS_SIM_APP_LIBS emu_core_pce_sim)
