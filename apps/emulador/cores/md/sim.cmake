# The Mega Drive in the simulator (included by apps/emulador/sim.cmake, in
# sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set): the same sources as
# board.cmake, built by the Mac's compiler.
#
# Symbols: the simulator links every app and every core into one
# executable, and Gwenesis's globals have generic names. The core is
# compiled to objects, linked with -r into a single object that keeps only
# emu_core_md global, and that object is the library the simulator links.

set(EMU_MD_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_MD_DIR}/sources.cmake)

add_library(emu_core_md_sim_objs OBJECT ${EMU_MD_SRCS})
target_include_directories(emu_core_md_sim_objs PRIVATE ${EMU_MAIN_DIR} ${EMU_MD_INCLUDES})
target_compile_definitions(emu_core_md_sim_objs PRIVATE AOS_SIM ${EMU_MD_DEFINES})
# -fno-common: a tentative definition would stay a common symbol, which
# the -r link below cannot make local.
target_compile_options(emu_core_md_sim_objs PRIVATE
    -O2 -fno-common -include ${EMU_MD_DIR}/md_glue.h)
set_source_files_properties(${EMU_MD_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_MD_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

set(_md_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_md_sim.o)
if(APPLE)
    # ld64 turns every global not exported into a local symbol with -r.
    add_custom_command(OUTPUT ${_md_one}
        COMMAND ${CMAKE_C_COMPILER} -r -nostdlib -Wl,-exported_symbol,_emu_core_md
                -o ${_md_one} $<TARGET_OBJECTS:emu_core_md_sim_objs>
        DEPENDS emu_core_md_sim_objs $<TARGET_OBJECTS:emu_core_md_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Mega Drive core into one object")
else()
    add_custom_command(OUTPUT ${_md_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_md_one}.all $<TARGET_OBJECTS:emu_core_md_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_md ${_md_one}.all ${_md_one}
        DEPENDS emu_core_md_sim_objs $<TARGET_OBJECTS:emu_core_md_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Mega Drive core into one object")
endif()
set_source_files_properties(${_md_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_md_sim STATIC ${_md_one})
set_target_properties(emu_core_md_sim PROPERTIES LINKER_LANGUAGE C)

list(APPEND AOS_SIM_APP_LIBS emu_core_md_sim)
