# The Game Boy Advance in the simulator (included by apps/emulador/sim.cmake,
# in sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set).
#
# On the Mac there is no executable memory to hand out, so gpSP is built
# as its upstream libretro core builds without HAVE_DYNAREC: the portable
# interpreter, cpu_threaded.c and riscv/ left out. Same adapter (gba_core.c)
# and same files as the board otherwise.
#
# Symbols: the simulator links every app and every core into one
# executable, and gpSP's globals have generic names (reg, dma, timer,
# retro_run...). The core is compiled to objects, linked with -r into a
# single object that keeps only emu_core_gba global, and that object is the
# library the simulator links. Nothing else of gpSP can meet another core's
# or another app's names.

# gpSP has C++ (cpu.cc, video.cc) and its built-in BIOS comes in by .incbin
# in bios_data.S; the simulator's project only enables C.
enable_language(CXX)
enable_language(ASM)

set(_gba_dir ${CMAKE_CURRENT_LIST_DIR})
set(_gba_gp  ${_gba_dir}/gpsp)
set(_gba_lrc ${_gba_gp}/libretro/libretro-common)

set(_gba_vendored
    ${_gba_gp}/bios_data.S
    ${_gba_gp}/cheats.c
    ${_gba_gp}/cpu.cc
    ${_gba_gp}/gba_cc_lut.c
    ${_gba_gp}/gba_memory.c
    ${_gba_gp}/gba_rom_scan.c
    ${_gba_gp}/gbp.c
    ${_gba_gp}/input.c
    ${_gba_gp}/libretro/libretro.c
    ${_gba_gp}/main.c
    ${_gba_gp}/memmap.c
    ${_gba_gp}/rfu.c
    ${_gba_gp}/savestate.c
    ${_gba_gp}/serial.c
    ${_gba_gp}/serial_proto.c
    ${_gba_gp}/sound.c
    ${_gba_gp}/video.cc
    ${_gba_lrc}/compat/compat_posix_string.c
    ${_gba_lrc}/compat/compat_strl.c
    ${_gba_lrc}/compat/fopen_utf8.c
    ${_gba_lrc}/encodings/encoding_utf.c
    ${_gba_lrc}/file/file_path.c
    ${_gba_lrc}/file/file_path_io.c
    ${_gba_lrc}/streams/file_stream.c
    ${_gba_lrc}/string/stdstring.c
    ${_gba_lrc}/time/rtime.c
    ${_gba_lrc}/vfs/vfs_implementation.c
)

add_library(emu_core_gba_sim_objs OBJECT ${_gba_dir}/gba_core.c ${_gba_vendored})
target_include_directories(emu_core_gba_sim_objs PRIVATE
    ${EMU_MAIN_DIR}
    ${_gba_gp}
    ${_gba_gp}/include
    ${_gba_gp}/libretro
    ${_gba_lrc}/include)
target_compile_definitions(emu_core_gba_sim_objs PRIVATE
    AOS_SIM
    FRONTEND_SUPPORTS_RGB565
    HAVE_INTTYPES_H
    HAVE_STDINT_H
    HAVE_STRINGS_H
    INLINE=inline
    __LIBRETRO__)
# -fno-common: a tentative definition would stay a common symbol, which
# the -r link below cannot make local.
target_compile_options(emu_core_gba_sim_objs PRIVATE
    -O2
    -fno-common
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
# Third-party code: its warnings are upstream's business.
set_source_files_properties(${_gba_vendored} PROPERTIES COMPILE_OPTIONS -w)

set(_gba_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_gba_sim.o)
if(APPLE)
    # ld64 turns every global not exported into a local symbol with -r.
    add_custom_command(OUTPUT ${_gba_one}
        COMMAND ${CMAKE_C_COMPILER} -r -nostdlib -Wl,-exported_symbol,_emu_core_gba
                -o ${_gba_one} $<TARGET_OBJECTS:emu_core_gba_sim_objs>
        DEPENDS emu_core_gba_sim_objs $<TARGET_OBJECTS:emu_core_gba_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the GBA core into one object")
else()
    add_custom_command(OUTPUT ${_gba_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_gba_one}.all $<TARGET_OBJECTS:emu_core_gba_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_gba ${_gba_one}.all ${_gba_one}
        DEPENDS emu_core_gba_sim_objs $<TARGET_OBJECTS:emu_core_gba_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the GBA core into one object")
endif()
set_source_files_properties(${_gba_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_gba_sim STATIC ${_gba_one})
set_target_properties(emu_core_gba_sim PROPERTIES LINKER_LANGUAGE C)

list(APPEND AOS_SIM_APP_LIBS emu_core_gba_sim)
