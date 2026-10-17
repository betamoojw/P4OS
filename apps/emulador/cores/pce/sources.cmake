# The PC Engine core's sources, for board.cmake and sim.cmake alike:
# pce-go (pce-go/, as Retro-Go carries it) and the adapter.

set(EMU_PCE_VENDOR_SRCS
    ${EMU_PCE_DIR}/pce-go/gfx.c
    ${EMU_PCE_DIR}/pce-go/h6280.c
    ${EMU_PCE_DIR}/pce-go/pce-go.c
    ${EMU_PCE_DIR}/pce-go/pce.c
    ${EMU_PCE_DIR}/pce-go/psg.c)

set(EMU_PCE_OWN_SRCS
    ${EMU_PCE_DIR}/pce_core.c)

set(EMU_PCE_SRCS ${EMU_PCE_VENDOR_SRCS} ${EMU_PCE_OWN_SRCS})
set(EMU_PCE_INCS ${EMU_PCE_DIR}/pce-go)
