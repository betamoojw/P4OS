# The Atari 7800 core for the board: ProSystem (cores/a7800/prosystem, as
# RetroESP32-P4 carries it) and the adapter, as the static library
# emu_core_a7800 that apps/emulador/CMakeLists.txt makes the module
# a7800.so of. Included from that file's scope; emu_core_board_lib() is in
# cores/emu_cores.cmake.

set(EMU_A7800_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_A7800_DIR}/sources.cmake)

emu_core_board_lib(a7800 ${EMU_A7800_SRCS})

target_include_directories(emu_core_a7800 PRIVATE ${EMU_A7800_INCLUDES})
# -O2: the CPU and MARIA loops are the whole frame.
target_compile_options(emu_core_a7800 PRIVATE -O2)
# ProSystem is third-party code with warnings we are not going to fix; the
# adapter is held to -Wall -Wextra.
set_source_files_properties(${EMU_A7800_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_A7800_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

# One global: ProSystem's names are generic (memory_Read, fastmap, cartRAM,
# palette_data, region_type...), and in the simulator every core and app
# shares one executable; on the board the module keeps the same rule. After
# the archive is made its objects are linked with -r into one, every global
# but emu_core_a7800 is made local and every COMDAT group gets a name of
# this core's own (one_object.cmake says why), and that single object
# replaces the archive's contents.
set(_a78_one ${CMAKE_BINARY_DIR}/cores/emu_core_a7800_one.o)
set(_a78_objcopy ${CMAKE_OBJCOPY})
if(NOT _a78_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _a78_objcopy ${CMAKE_C_COMPILER})
    string(REGEX REPLACE "gcc\\.exe$" "objcopy.exe" _a78_objcopy ${_a78_objcopy})
endif()
string(REGEX REPLACE "objcopy$" "readelf" _a78_readelf ${_a78_objcopy})
string(REGEX REPLACE "objcopy\\.exe$" "readelf.exe" _a78_readelf ${_a78_readelf})
add_custom_command(TARGET emu_core_a7800 POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_a78_one}.all "$<TARGET_OBJECTS:emu_core_a7800>"
    COMMAND ${CMAKE_COMMAND} -DIN=${_a78_one}.all -DOUT=${_a78_one} -DKEEP=emu_core_a7800
            -DTAG=a7800 -DREADELF=${_a78_readelf} -DOBJCOPY=${_a78_objcopy}
            -P ${EMU_A7800_DIR}/one_object.cmake
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_a7800>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_a7800> ${_a78_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_a7800>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_a7800 the only global of the Atari 7800 core")
