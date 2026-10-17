# The Neo Geo on the board: gngeo as RetroESP32-P4 carries it (gngeo/),
# with superzazu's Z80 (z80/), tinfl and the core's own files, as the
# static library emu_core_neogeo that apps/emulador/CMakeLists.txt turns
# into the module neogeo.so. emu_core_board_lib() is in cores/emu_cores.cmake.

set(EMU_NEOGEO_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_NEOGEO_DIR}/sources.cmake)

emu_core_board_lib(neogeo ${EMU_NEOGEO_SRCS})

target_include_directories(emu_core_neogeo PRIVATE ${EMU_NEOGEO_INCLUDES})
target_compile_definitions(emu_core_neogeo PRIVATE ${EMU_NEOGEO_DEFINES})
# -O2 for all (the .so's -Os would cost the 68000 and the sprite drawing
# dearly); the 68000's handlers -O3, as RetroESP32 builds them.
target_compile_options(emu_core_neogeo PRIVATE -O2)
# gngeo is third-party code with warnings we are not going to fix; it gets
# ng_p4os.h forced in (printf to the log, no exit, strdup). The core's own
# files are held to -Wall -Wextra.
# GCC 14 makes some of gngeo's old C (a Uint32 * where an unsigned int * is
# meant: the same type on the Mac, not on riscv32) errors that -w does not
# silence: back to warnings, silenced.
set(_ng_old_c -w -Wno-error=incompatible-pointer-types -Wno-error=int-conversion
    -Wno-error=implicit-function-declaration)
set_source_files_properties(${EMU_NEOGEO_VENDOR_SRCS} PROPERTIES
    COMPILE_OPTIONS "${_ng_old_c};-include;${EMU_NEOGEO_FORCED}")
set_source_files_properties(${EMU_NEOGEO_68K_SRCS} PROPERTIES
    COMPILE_OPTIONS "${_ng_old_c};-O3;-include;${EMU_NEOGEO_FORCED}")
set_source_files_properties(${EMU_NEOGEO_OWN_SRCS} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")

# One global: gngeo's names are generic (memory, conf, regs, timers...).
# After the archive is made, its objects are linked with -r into one,
# every global but emu_core_neogeo is made local, and that single object
# replaces the archive's contents (as the GBA core does).
set(_ng_one ${CMAKE_BINARY_DIR}/cores/emu_core_neogeo_one.o)
set(_ng_objcopy ${CMAKE_OBJCOPY})
if(NOT _ng_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _ng_objcopy ${CMAKE_C_COMPILER})
endif()
add_custom_command(TARGET emu_core_neogeo POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_ng_one}.all "$<TARGET_OBJECTS:emu_core_neogeo>"
    COMMAND ${_ng_objcopy} --keep-global-symbol=emu_core_neogeo ${_ng_one}.all ${_ng_one}
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_neogeo>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_neogeo> ${_ng_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_neogeo>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_neogeo the only global of the Neo Geo core")
