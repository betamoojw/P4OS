# The Sega 8-bit core for the board: one library, emu_core_sms, with three
# consoles (the Master System, the Game Gear, the ColecoVision), linked into
# emulador.so by apps/emulador/CMakeLists.txt. Included from that file's
# scope; emu_core_board_lib() is in cores/emu_cores.cmake.

set(EMU_SMS_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_SMS_DIR}/sources.cmake)
set(EMU_CORE_IDS sms gg col)

emu_core_board_lib(sms ${EMU_SMS_SRCS})

target_include_directories(emu_core_sms PRIVATE ${EMU_SMS_INCS})
# -O2 rather than the .so's -Os: the Z80 and the line renderer are the
# frame.
target_compile_options(emu_core_sms PRIVATE -O2)
# SMS Plus is third-party code with warnings we are not going to fix; our
# own files are held to -Wall -Wextra.
set_source_files_properties(${EMU_SMS_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_SMS_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# Three globals: SMS Plus's names are generic (sms, vdp, cart, option,
# render_line, z80_execute...). After the archive is made its objects are
# linked with -r into one, every global but the three tables is made
# local, and that object replaces the archive's contents (as the GBA core
# does).
set(_sms_one ${CMAKE_BINARY_DIR}/cores/emu_core_sms_one.o)
set(_sms_objcopy ${CMAKE_OBJCOPY})
if(NOT _sms_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _sms_objcopy ${CMAKE_C_COMPILER})
endif()
add_custom_command(TARGET emu_core_sms POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_sms_one}.all "$<TARGET_OBJECTS:emu_core_sms>"
    COMMAND ${_sms_objcopy} --keep-global-symbol=emu_core_sms --keep-global-symbol=emu_core_gg
            --keep-global-symbol=emu_core_col ${_sms_one}.all ${_sms_one}
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_sms>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_sms> ${_sms_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_sms>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving the three tables the only globals of the Sega 8-bit core")
