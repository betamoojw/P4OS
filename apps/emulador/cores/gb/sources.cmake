# The Game Boy core's sources, for board.cmake and sim.cmake alike: gnuboy
# as Retro-Go carries it (cores/gb/gnuboy) and the adapter.

set(EMU_GB_VENDOR_SRCS
    ${EMU_GB_DIR}/gnuboy/cpu.c
    ${EMU_GB_DIR}/gnuboy/gnuboy.c
    ${EMU_GB_DIR}/gnuboy/hw.c
    ${EMU_GB_DIR}/gnuboy/lcd.c
    ${EMU_GB_DIR}/gnuboy/sound.c)

set(EMU_GB_OWN_SRCS
    ${EMU_GB_DIR}/gb_core.c)

set(EMU_GB_SRCS ${EMU_GB_VENDOR_SRCS} ${EMU_GB_OWN_SRCS})
