# The Atari 7800 core's sources, for board.cmake and sim.cmake alike:
# ProSystem as RetroESP32-P4 carries it (prosystem/, without its runner
# prosystem_run.c) and the adapter.

set(_p ${EMU_A7800_DIR}/prosystem)
set(EMU_A7800_VENDOR_SRCS
    ${_p}/Bios.c
    ${_p}/Cartridge.c
    ${_p}/Database.c
    ${_p}/Hash.c
    ${_p}/Maria.c
    ${_p}/Memory.c
    ${_p}/Palette.c
    ${_p}/Pokey.c
    ${_p}/ProSystem.c
    ${_p}/Region.c
    ${_p}/Riot.c
    ${_p}/Sally.c
    ${_p}/Tia.c)

set(EMU_A7800_OWN_SRCS ${EMU_A7800_DIR}/a7800_core.c)

set(EMU_A7800_SRCS ${EMU_A7800_VENDOR_SRCS} ${EMU_A7800_OWN_SRCS})

# compat/ first: its esp_attr.h and esp_heap_caps.h stand in for ESP-IDF's.
set(EMU_A7800_INCLUDES ${EMU_A7800_DIR}/compat ${_p}/include)
