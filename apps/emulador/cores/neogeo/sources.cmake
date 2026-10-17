# The Neo Geo core's sources, for board.cmake and sim.cmake alike. Set
# EMU_NEOGEO_DIR before including.

set(_ng ${EMU_NEOGEO_DIR})
set(_gn ${EMU_NEOGEO_DIR}/gngeo)

# gngeo as RetroESP32-P4 carries it (GPL-2), with the changes marked P4OS.
# Left out of RetroESP32's list: emu.c (its frame loop is ng_core.c),
# esp32_platform.c (ng_platform.c), unzip.c / stb_zlib.c / stb_image.c /
# resfile.c (ng_zip.c, tinfl.c and the compiled-in drivers), state.c,
# frame_skip.c, transpack.c, list.c, gnutil.c, the MAME Z80 and its
# interface (ng_z80.c + z80/), and Musashi (not used: Generator is).
set(EMU_NEOGEO_VENDOR_SRCS
    ${_gn}/memory.c
    ${_gn}/video.c
    ${_gn}/timer.c
    ${_gn}/pd4990a.c
    ${_gn}/neoboot.c
    ${_gn}/roms.c
    ${_gn}/neocrypt.c
    ${_gn}/mame_layer.c
    ${_gn}/adpcm_cache.c
    ${_gn}/generator68k_interf.c
    ${_gn}/generator68k/cpu68k.c
    ${_gn}/generator68k/reg68k.c
    ${_gn}/ym2610/ym2610.c
    ${_gn}/ym2610/2610intf.c
    ${_ng}/z80/z80.c
    ${_ng}/tinfl.c)

# Generator's instruction handlers (the hot part, as RetroESP32: -O3).
set(EMU_NEOGEO_68K_SRCS
    ${_gn}/generator68k/cpu68k-0.c
    ${_gn}/generator68k/cpu68k-1.c
    ${_gn}/generator68k/cpu68k-2.c
    ${_gn}/generator68k/cpu68k-3.c
    ${_gn}/generator68k/cpu68k-4.c
    ${_gn}/generator68k/cpu68k-5.c
    ${_gn}/generator68k/cpu68k-6.c
    ${_gn}/generator68k/cpu68k-7.c
    ${_gn}/generator68k/cpu68k-8.c
    ${_gn}/generator68k/cpu68k-9.c
    ${_gn}/generator68k/cpu68k-a.c
    ${_gn}/generator68k/cpu68k-b.c
    ${_gn}/generator68k/cpu68k-c.c
    ${_gn}/generator68k/cpu68k-d.c
    ${_gn}/generator68k/cpu68k-e.c
    ${_gn}/generator68k/cpu68k-f.c)

set(EMU_NEOGEO_OWN_SRCS
    ${_ng}/ng_core.c
    ${_ng}/ng_load.c
    ${_ng}/ng_platform.c
    ${_ng}/ng_z80.c
    ${_ng}/ng_zip.c
    ${_ng}/ng_drivers.c)

set(EMU_NEOGEO_SRCS ${EMU_NEOGEO_OWN_SRCS} ${EMU_NEOGEO_VENDOR_SRCS} ${EMU_NEOGEO_68K_SRCS})

set(EMU_NEOGEO_INCLUDES
    ${_ng}
    ${_gn}
    ${_gn}/include
    ${_gn}/generator68k
    ${_gn}/ym2610)

# USE_GENERATOR68K: memory.h's byte order for the 68000's ROMs.
# Z80_FLAT_MEM: superzazu's Z80 on a flat 64 KB array (ng_z80.c).
set(EMU_NEOGEO_DEFINES USE_GENERATOR68K=1 Z80_FLAT_MEM)
set(EMU_NEOGEO_FORCED ${_ng}/ng_p4os.h)
