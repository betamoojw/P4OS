# The Game Boy Advance on the board: gpSP (gpsp/, from midworld/gpsp-esp32)
# with its RV32 recompiler, as upstream's components/gpsp/CMakeLists.txt
# builds it for the ESP32-P4, plus the adapter (gba_core.c).
#
# The recompiler runs only when the app hands the core executable memory
# (emu_host_t's exec_*). Without it gpSP's block cache fails to allocate at
# load and the same library interprets: nothing here has to change for that.

# ---------------------------------------------------------------------------
# The ISA extensions, all in one place.
#
# The firmware is built for plain rv32imafc (+ the PIE). gpSP on the P4 also
# uses Zba/Zbb/Zbs/Zcb, the hardware loops (Xesploop) and the PIE vector unit
# (XespV, affine backgrounds), and nobody has run them on this rev 1.3 chip
# yet. Each one is a switch: 1 lets both the compiler (gpSP's own C and
# assembly) and the recompiler (the code it writes at run time) use it, 0
# keeps it out of both. gpsp-esp32's tests/isa-probe is a stand-alone
# firmware that executes one instruction of each and reports which trap; run
# it (or an equivalent probe) before trusting these on a new chip.
#
#   Zcb needs C and Zbb for some of its forms; the C switch is only for the
#   recompiler (the compiler always has C, the firmware does too).
set(EMU_GBA_ZBA       1)
set(EMU_GBA_ZBB       1)
set(EMU_GBA_ZBS       1)
set(EMU_GBA_ZCB       1)
set(EMU_GBA_RVC       1)
set(EMU_GBA_XESPLOOP  1)
set(EMU_GBA_AFFINE_PIE 1)

# The recompiler's caches (KiB of executable PSRAM; chunks are allocated as
# they fill, so a small game takes one chunk of each). Upstream's defaults.
set(EMU_GBA_ROM_CACHE_KIB  2048)
set(EMU_GBA_RAM_CACHE_KIB  2048)
set(EMU_GBA_CHUNK_KIB       256)
# ---------------------------------------------------------------------------

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
    ${_gba_gp}/cpu_threaded.c
    ${_gba_gp}/riscv/rv32_arm_alu.c
    ${_gba_gp}/riscv/rv32_arm_branch.c
    ${_gba_gp}/riscv/rv32_arm_flow_alu.c
    ${_gba_gp}/riscv/rv32_arm_flow_full.c
    ${_gba_gp}/riscv/rv32_arm_io.S
    ${_gba_gp}/riscv/rv32_arm_memory.c
    ${_gba_gp}/riscv/rv32_arm_mul.c
    ${_gba_gp}/riscv/rv32_arm_palette.S
    ${_gba_gp}/riscv/rv32_arm_system.c
    ${_gba_gp}/riscv/rv32_block_emit.c
    ${_gba_gp}/riscv/rv32_block_link.c
    ${_gba_gp}/riscv/rv32_block_runtime.cc
    ${_gba_gp}/riscv/rv32_codegen.c
    ${_gba_gp}/riscv/rv32_fallback.c
    ${_gba_gp}/riscv/rv32_flow_timing.c
    ${_gba_gp}/riscv/rv32_interpreter_stub.S
    ${_gba_gp}/riscv/rv32_platform.c
    ${_gba_gp}/riscv/rv32_ram_units.c
    ${_gba_gp}/riscv/rv32_rom_load.S
    ${_gba_gp}/riscv/rv32_stub.S
    ${_gba_gp}/riscv/rv32_thumb_alu.c
    ${_gba_gp}/riscv/rv32_thumb_exit_shared.S
    ${_gba_gp}/riscv/rv32_thumb_flow_full.c
    ${_gba_gp}/riscv/rv32_thumb_memory.c
    ${_gba_gp}/riscv/rv32_thumb_multi_ram.S
    ${_gba_gp}/riscv/rv32_thumb_ram_helper.S
    ${_gba_gp}/riscv/rv32_thumb_ram_store.S
)
if(EMU_GBA_AFFINE_PIE)
    list(APPEND _gba_vendored ${_gba_gp}/riscv/rv32_affine_pie.S)
endif()

emu_core_board_lib(gba ${_gba_dir}/gba_core.c ${_gba_vendored})

set(_gba_isa rv32imafc_zicsr_zifencei)
foreach(_e ZBA ZBB ZBS ZCB XESPLOOP)
    if(EMU_GBA_${_e})
        string(TOLOWER ${_e} _l)
        string(APPEND _gba_isa _${_l})
    endif()
endforeach()
if(EMU_GBA_AFFINE_PIE)
    string(APPEND _gba_isa _xespv)
endif()

target_include_directories(emu_core_gba PRIVATE
    ${_gba_gp}
    ${_gba_gp}/include
    ${_gba_gp}/libretro
    ${_gba_lrc}/include)
target_compile_definitions(emu_core_gba PRIVATE
    FRONTEND_SUPPORTS_RGB565
    HAVE_INTTYPES_H
    HAVE_STDINT_H
    HAVE_STRINGS_H
    INLINE=inline
    __LIBRETRO__
    P4OS_NO_FSEEKO
    GPSP_RV32_JIT=1
    GPSP_RV32_ROM_CACHE_KIB=${EMU_GBA_ROM_CACHE_KIB}
    GPSP_RV32_RAM_CACHE_KIB=${EMU_GBA_RAM_CACHE_KIB}
    GPSP_RV32_CODE_CHUNK_KIB=${EMU_GBA_CHUNK_KIB}
    GPSP_RV32_USE_ZBA=${EMU_GBA_ZBA}
    GPSP_RV32_USE_ZBB=${EMU_GBA_ZBB}
    GPSP_RV32_USE_ZBS=${EMU_GBA_ZBS}
    GPSP_RV32_USE_RVC=${EMU_GBA_RVC}
    GPSP_RV32_USE_ZCB=${EMU_GBA_ZCB}
    $<$<BOOL:${EMU_GBA_AFFINE_PIE}>:GPSP_AFFINE_PIE=1>)
target_compile_options(emu_core_gba PRIVATE
    -march=${_gba_isa}
    -O2
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>)
set_source_files_properties(${_gba_vendored} PROPERTIES COMPILE_OPTIONS -w)
# Upstream's choice for the scanline renderer's inner loops.
set_source_files_properties(${_gba_gp}/video.cc PROPERTIES COMPILE_OPTIONS "-w;-O3")

# One global: everything ends up in emulador.so next to the other cores, and
# gpSP's names are generic (reg, dma, timer, retro_run...). After the archive
# is made, its objects are linked with -r into one, every global but
# emu_core_gba is made local, and that single object replaces the archive's
# contents. A local symbol is also never preempted, which the assembly's
# PC-relative references to gpSP's data need in a shared object.
set(_gba_one ${CMAKE_BINARY_DIR}/cores/emu_core_gba_one.o)
set(_gba_objcopy ${CMAKE_OBJCOPY})
if(NOT _gba_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _gba_objcopy ${CMAKE_C_COMPILER})
endif()
add_custom_command(TARGET emu_core_gba POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_gba_one}.all "$<TARGET_OBJECTS:emu_core_gba>"
    COMMAND ${_gba_objcopy} --keep-global-symbol=emu_core_gba ${_gba_one}.all ${_gba_one}
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:emu_core_gba>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:emu_core_gba> ${_gba_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:emu_core_gba>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving emu_core_gba the only global of the GBA core")
