# The Super Nintendo core's sources, for board.cmake and sim.cmake alike:
# every file of the vendored Snes9x (cores/snes/snes9x) and the adapter.

set(EMU_SNES_VENDOR_SRCS
    ${EMU_SNES_DIR}/snes9x/apu.c
    ${EMU_SNES_DIR}/snes9x/c4.c
    ${EMU_SNES_DIR}/snes9x/c4emu.c
    ${EMU_SNES_DIR}/snes9x/clip.c
    ${EMU_SNES_DIR}/snes9x/cpu.c
    ${EMU_SNES_DIR}/snes9x/cpuexec.c
    ${EMU_SNES_DIR}/snes9x/cpuops.c
    ${EMU_SNES_DIR}/snes9x/dma.c
    ${EMU_SNES_DIR}/snes9x/dsp.c
    ${EMU_SNES_DIR}/snes9x/getset.c
    ${EMU_SNES_DIR}/snes9x/gfx.c
    ${EMU_SNES_DIR}/snes9x/globals.c
    ${EMU_SNES_DIR}/snes9x/memmap.c
    ${EMU_SNES_DIR}/snes9x/obc1.c
    ${EMU_SNES_DIR}/snes9x/ppu.c
    ${EMU_SNES_DIR}/snes9x/soundux.c
    ${EMU_SNES_DIR}/snes9x/spc700.c
    ${EMU_SNES_DIR}/snes9x/srtc.c
    ${EMU_SNES_DIR}/snes9x/tile.c)

set(EMU_SNES_OWN_SRCS
    ${EMU_SNES_DIR}/snes_core.c)

set(EMU_SNES_SRCS ${EMU_SNES_VENDOR_SRCS} ${EMU_SNES_OWN_SRCS})

# What Retro-Go builds Snes9x with: >> on signed values is arithmetic, and
# no 128 KB colour-math table (GFX.ZERO), worked out inline instead.
set(EMU_SNES_DEFS RIGHTSHIFT_IS_SAR NO_ZERO_LUT)
