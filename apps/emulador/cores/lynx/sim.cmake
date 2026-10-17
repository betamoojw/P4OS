# The Atari Lynx in the simulator (included by apps/emulador/sim.cmake, in
# sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set): the same sources as
# board.cmake, built by the Mac's compiler.
#
# Symbols: the simulator links every app and every core into one
# executable, and Handy's globals have generic names. The core is compiled
# to objects, linked with -r into a single object that keeps only
# emu_core_lynx global (ld64 makes every other global local), and that
# object is the library the simulator links. On the Mac operator new comes
# from libc++, which the simulator then links (lynx_rt.cpp is for the board
# only).

enable_language(CXX)

set(EMU_LYNX_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_LYNX_DIR}/sources.cmake)

add_library(emu_core_lynx_sim_objs OBJECT ${EMU_LYNX_SRCS})
target_include_directories(emu_core_lynx_sim_objs PRIVATE ${EMU_MAIN_DIR} ${EMU_LYNX_INCLUDES})
target_compile_definitions(emu_core_lynx_sim_objs PRIVATE AOS_SIM ${EMU_LYNX_DEFINES})
# -fno-common: a tentative definition would stay a common symbol, which the
# -r link below cannot make local.
target_compile_options(emu_core_lynx_sim_objs PRIVATE
    -O2
    -fno-common
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
set_source_files_properties(${EMU_LYNX_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_LYNX_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

set(_lynx_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_lynx_sim.o)
if(APPLE)
    add_custom_command(OUTPUT ${_lynx_one}
        COMMAND ${CMAKE_CXX_COMPILER} -r -nostdlib -Wl,-exported_symbol,_emu_core_lynx
                -o ${_lynx_one} $<TARGET_OBJECTS:emu_core_lynx_sim_objs>
        DEPENDS emu_core_lynx_sim_objs $<TARGET_OBJECTS:emu_core_lynx_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Atari Lynx core into one object")
else()
    add_custom_command(OUTPUT ${_lynx_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_lynx_one}.all $<TARGET_OBJECTS:emu_core_lynx_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_lynx ${_lynx_one}.all ${_lynx_one}
        DEPENDS emu_core_lynx_sim_objs $<TARGET_OBJECTS:emu_core_lynx_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Atari Lynx core into one object")
endif()
set_source_files_properties(${_lynx_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_lynx_sim STATIC ${_lynx_one})
set_target_properties(emu_core_lynx_sim PROPERTIES LINKER_LANGUAGE C)
# operator new and delete: the C++ library, which the simulator (a C
# project) does not link by itself.
if(APPLE)
    target_link_libraries(emu_core_lynx_sim INTERFACE c++)
else()
    target_link_libraries(emu_core_lynx_sim INTERFACE stdc++)
endif()

list(APPEND AOS_SIM_APP_LIBS emu_core_lynx_sim)
