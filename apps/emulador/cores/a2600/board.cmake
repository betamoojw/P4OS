# The Atari 2600 core for the board: Stella (cores/a2600/stella, as
# RetroESP32-P4 carries it) and the adapter, as the static library
# emu_core_a2600 that apps/emulador/CMakeLists.txt makes the module
# a2600.so of.
# Included from that file's scope; emu_core_board_lib() is in
# cores/emu_cores.cmake.

set(EMU_A2600_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_A2600_DIR}/sources.cmake)

emu_core_board_lib(a2600 ${EMU_A2600_SRCS})

target_include_directories(emu_core_a2600 PRIVATE ${EMU_A2600_INCLUDES})
target_compile_definitions(emu_core_a2600 PRIVATE ${EMU_A2600_DEFINES})
# C++20: libstdc++ then instantiates std::string in the core's own code
# instead of expecting it from a library the firmware does not have (see
# a2600_rt.cpp). -O2: the CPU and TIA loops are the whole frame.
target_compile_options(emu_core_a2600 PRIVATE
    -O2
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>)
# Stella is third-party code with warnings we are not going to fix; the
# adapter and the runtime pieces are held to -Wall -Wextra.
set_source_files_properties(${EMU_A2600_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_A2600_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

# One global: Stella's names are generic (System, Console, Settings,
# Random...), and in the simulator every core and app shares one
# executable; on the board the module keeps the same rule. After
# the archive is made its objects are linked with -r into one, every global
# but emu_core_a2600 is made local and every COMDAT group gets a name of
# this core's own (one_object.cmake says why), and that single object
# replaces the archive's contents.
set(_a26_one ${CMAKE_BINARY_DIR}/cores/emu_core_a2600_one.o)
set(_a26_objcopy ${CMAKE_OBJCOPY})
if(NOT _a26_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _a26_objcopy ${CMAKE_C_COMPILER})
endif()
string(REGEX REPLACE "objcopy$" "readelf" _a26_readelf ${_a26_objcopy})
add_custom_command(TARGET emu_core_a2600 POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_a26_one}.all "$<TARGET_OBJECTS:emu_core_a2600>"
    COMMAND ${CMAKE_COMMAND} -DIN=${_a26_one}.all -DOUT=${_a26_one} -DKEEP=emu_core_a2600
            -DTAG=a2600 -DREADELF=${_a26_readelf} -DOBJCOPY=${_a26_objcopy}
            -P ${EMU_A2600_DIR}/one_object.cmake
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_a2600>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_a2600> ${_a26_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_a2600>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_a2600 the only global of the Atari 2600 core")
