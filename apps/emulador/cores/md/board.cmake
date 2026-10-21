# The Mega Drive on the board: Gwenesis (gwenesis/) and the adapter
# (md_core.c), as the static library emu_core_md that
# apps/emulador/CMakeLists.txt links into the module md.so. Included from
# that file's scope; emu_core_board_lib() is in cores/emu_cores.cmake.

set(EMU_MD_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_MD_DIR}/sources.cmake)

emu_core_board_lib(md ${EMU_MD_SRCS})

target_include_directories(emu_core_md PRIVATE ${EMU_MD_INCLUDES})
target_compile_definitions(emu_core_md PRIVATE ${EMU_MD_DEFINES})
# -O2 rather than the .so's -Os: the 68000, the VDP's lines and the YM2612
# are the whole frame. (Gwenesis's files also carry RetroESP32-P4's
# "#pragma GCC optimize Ofast", left as they were.) md_glue.h sends
# Gwenesis's printf to the host's log.
target_compile_options(emu_core_md PRIVATE
    -O2 -include ${EMU_MD_DIR}/md_glue.h)
# Gwenesis is third-party code with warnings we are not going to fix; the
# adapter is held to -Wall -Wextra.
set_source_files_properties(${EMU_MD_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_MD_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# One global: Gwenesis's names are generic (VRAM, CRAM, m68k, zclk,
# scan_line, load_cartridge...). After the archive is made its objects are
# linked with -r into one, every global but emu_core_md is made local, and
# that object replaces the archive's contents (as the GBA core does).
set(_md_one ${CMAKE_BINARY_DIR}/cores/emu_core_md_one.o)
set(_md_objcopy ${CMAKE_OBJCOPY})
if(NOT _md_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _md_objcopy ${CMAKE_C_COMPILER})
    string(REGEX REPLACE "gcc\\.exe$" "objcopy.exe" _md_objcopy ${_md_objcopy})
endif()
add_custom_command(TARGET emu_core_md POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_md_one}.all "$<TARGET_OBJECTS:emu_core_md>"
    COMMAND ${_md_objcopy} --keep-global-symbol=emu_core_md ${_md_one}.all ${_md_one}
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_md>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_md> ${_md_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_md>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_md the only global of the Mega Drive core")
