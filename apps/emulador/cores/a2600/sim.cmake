# The Atari 2600 in the simulator (included by apps/emulador/sim.cmake, in
# sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set): the same sources as
# board.cmake, built by the Mac's compiler.
#
# Symbols: the simulator links every app and every core into one
# executable, and Stella's globals have generic names. The core is compiled
# to objects, linked with -r into a single object that keeps only
# emu_core_a2600 global (ld64 makes every other global local), and that
# object is the library the simulator links. On the Mac std::string and
# operator new come from libc++, which the simulator then links
# (a2600_rt.cpp and a2600_libc.c are for the board only).

enable_language(CXX)

set(EMU_A2600_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_A2600_DIR}/sources.cmake)

add_library(emu_core_a2600_sim_objs OBJECT ${EMU_A2600_SRCS})
target_include_directories(emu_core_a2600_sim_objs PRIVATE ${EMU_MAIN_DIR} ${EMU_A2600_INCLUDES})
target_compile_definitions(emu_core_a2600_sim_objs PRIVATE AOS_SIM ${EMU_A2600_DEFINES})
# -fno-common: a tentative definition would stay a common symbol, which the
# -r link below cannot make local.
target_compile_options(emu_core_a2600_sim_objs PRIVATE
    -O2
    -fno-common
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
set_source_files_properties(${EMU_A2600_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_A2600_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

set(_a26_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_a2600_sim.o)
if(APPLE)
    add_custom_command(OUTPUT ${_a26_one}
        COMMAND ${CMAKE_CXX_COMPILER} -r -nostdlib -Wl,-exported_symbol,_emu_core_a2600
                -o ${_a26_one} $<TARGET_OBJECTS:emu_core_a2600_sim_objs>
        DEPENDS emu_core_a2600_sim_objs $<TARGET_OBJECTS:emu_core_a2600_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Atari 2600 core into one object")
else()
    add_custom_command(OUTPUT ${_a26_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_a26_one}.all $<TARGET_OBJECTS:emu_core_a2600_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_a2600 ${_a26_one}.all ${_a26_one}
        DEPENDS emu_core_a2600_sim_objs $<TARGET_OBJECTS:emu_core_a2600_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Atari 2600 core into one object")
endif()
set_source_files_properties(${_a26_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_a2600_sim STATIC ${_a26_one})
set_target_properties(emu_core_a2600_sim PROPERTIES LINKER_LANGUAGE C)
# std::string and operator new: the C++ library, which the simulator (a C
# project) does not link by itself.
if(APPLE)
    target_link_libraries(emu_core_a2600_sim INTERFACE c++)
else()
    target_link_libraries(emu_core_a2600_sim INTERFACE stdc++)
endif()

list(APPEND AOS_SIM_APP_LIBS emu_core_a2600_sim)
