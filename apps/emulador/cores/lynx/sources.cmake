# The Lynx core's sources, for board.cmake and sim.cmake alike: Handy as
# Retro-Go carries it (handy/) and the adapter. lynx_rt.cpp is for the board
# only (it is empty in the simulator).

set(_h ${EMU_LYNX_DIR}/handy)
set(EMU_LYNX_VENDOR_SRCS
    ${_h}/cart.cpp
    ${_h}/eeprom.cpp
    ${_h}/lynxdec.cpp
    ${_h}/mikie.cpp
    ${_h}/susie.cpp
    ${_h}/system.cpp)

set(EMU_LYNX_OWN_SRCS
    ${EMU_LYNX_DIR}/lynx_core.cpp
    ${EMU_LYNX_DIR}/lynx_rt.cpp
    ${EMU_LYNX_DIR}/lynx_shim.c)

set(EMU_LYNX_SRCS ${EMU_LYNX_VENDOR_SRCS} ${EMU_LYNX_OWN_SRCS})

# compat/ has rg_system.h, which Handy includes when built with RETRO_GO.
set(EMU_LYNX_INCLUDES ${EMU_LYNX_DIR}/compat ${_h})
set(EMU_LYNX_DEFINES RETRO_GO)
