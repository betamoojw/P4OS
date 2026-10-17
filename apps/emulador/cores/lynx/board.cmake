# The Atari Lynx core for the board: Handy (cores/lynx/handy, as Retro-Go
# carries it) and the adapter, as the static library emu_core_lynx that
# apps/emulador/CMakeLists.txt makes the module lynx.so of. Included from
# that file's scope; emu_core_board_lib() is in cores/emu_cores.cmake.

set(EMU_LYNX_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_LYNX_DIR}/sources.cmake)

emu_core_board_lib(lynx ${EMU_LYNX_SRCS})

target_include_directories(emu_core_lynx PRIVATE ${EMU_LYNX_INCLUDES})
target_compile_definitions(emu_core_lynx PRIVATE ${EMU_LYNX_DEFINES})
# -O2, as Retro-Go builds Handy: the 65C02, Mikie's timers and Susie's
# sprites are the whole frame. Handy needs no C++ library (lynx_rt.cpp).
target_compile_options(emu_core_lynx PRIVATE
    -O2
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
# Handy is third-party code with warnings we are not going to fix; the
# adapter and the runtime pieces are held to -Wall -Wextra.
set_source_files_properties(${EMU_LYNX_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_LYNX_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

# One global: Handy's names are generic (gSystemCycleCount, CSystem,
# lss_read...), and in the simulator every core and app shares one
# executable; on the board the module keeps the same rule. After
# the archive is made its objects are linked with -r into one, every global
# but emu_core_lynx is made local and every COMDAT group gets a name of
# this core's own (one_object.cmake says why), and that single object
# replaces the archive's contents.
set(_lynx_one ${CMAKE_BINARY_DIR}/cores/emu_core_lynx_one.o)
set(_lynx_objcopy ${CMAKE_OBJCOPY})
if(NOT _lynx_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _lynx_objcopy ${CMAKE_C_COMPILER})
endif()
string(REGEX REPLACE "objcopy$" "readelf" _lynx_readelf ${_lynx_objcopy})
add_custom_command(TARGET emu_core_lynx POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_lynx_one}.all "$<TARGET_OBJECTS:emu_core_lynx>"
    COMMAND ${CMAKE_COMMAND} -DIN=${_lynx_one}.all -DOUT=${_lynx_one} -DKEEP=emu_core_lynx
            -DTAG=lynx -DREADELF=${_lynx_readelf} -DOBJCOPY=${_lynx_objcopy}
            -P ${EMU_LYNX_DIR}/one_object.cmake
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_lynx>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_lynx> ${_lynx_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_lynx>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_lynx the only global of the Lynx core")
