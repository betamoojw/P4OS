# The Mega Drive core's sources, for board.cmake and sim.cmake alike.
# Gwenesis as RetroESP32-P4 carries it (gwenesis/, changes in README.md),
# and the adapter. Left out of the vendored copy: the Z80's debugger
# (Debug.c, ConDebug.c) and Musashi's static jump and cycle tables, which
# m68k_dispatch.h / m68k_handlers.h replace (tools/gen_m68k_dispatch.py).

set(EMU_MD_G ${EMU_MD_DIR}/gwenesis/src)

set(EMU_MD_VENDOR_SRCS
    ${EMU_MD_G}/bus/gwenesis_bus.c
    ${EMU_MD_G}/cpus/M68K/m68kcpu.c
    ${EMU_MD_G}/cpus/Z80/Z80.c
    ${EMU_MD_G}/io/gwenesis_io.c
    ${EMU_MD_G}/savestate/gwenesis_savestate.c
    ${EMU_MD_G}/sound/gwenesis_sn76489.c
    ${EMU_MD_G}/sound/ym2612.c
    ${EMU_MD_G}/sound/z80inst.c
    ${EMU_MD_G}/vdp/gwenesis_vdp_gfx.c
    ${EMU_MD_G}/vdp/gwenesis_vdp_mem.c)

set(EMU_MD_OWN_SRCS
    ${EMU_MD_DIR}/md_core.c)

set(EMU_MD_SRCS ${EMU_MD_VENDOR_SRCS} ${EMU_MD_OWN_SRCS})

set(EMU_MD_INCLUDES
    ${EMU_MD_DIR}
    ${EMU_MD_G}/bus
    ${EMU_MD_G}/cpus/M68K
    ${EMU_MD_G}/cpus/Z80
    ${EMU_MD_G}/io
    ${EMU_MD_G}/savestate
    ${EMU_MD_G}/sound
    ${EMU_MD_G}/vdp)

# GWENESIS_AUDIO_ACCURATE: the sound chips are brought up to the CPU's time
# on every write to them (the DAC's samples land where they were written).
set(EMU_MD_DEFINES GWENESIS_AUDIO_ACCURATE=1 NDEBUG)
