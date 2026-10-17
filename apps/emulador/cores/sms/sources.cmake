# The Sega 8-bit core's sources, for board.cmake and sim.cmake alike:
# SMS Plus (smsplus/, as Retro-Go carries it, without its Z80, its SN76489,
# its FM chips, its state code and its built-in ColecoVision BIOS), the
# core's own Z80 (z80/) and SN76489 (sms_psg.c), and the adapter.

set(EMU_SMS_VENDOR_SRCS
    ${EMU_SMS_DIR}/smsplus/loadrom.c
    ${EMU_SMS_DIR}/smsplus/memz80.c
    ${EMU_SMS_DIR}/smsplus/pio.c
    ${EMU_SMS_DIR}/smsplus/render.c
    ${EMU_SMS_DIR}/smsplus/sms.c
    ${EMU_SMS_DIR}/smsplus/system.c
    ${EMU_SMS_DIR}/smsplus/tms.c
    ${EMU_SMS_DIR}/smsplus/vdp.c
    ${EMU_SMS_DIR}/smsplus/sound/sound.c)

set(EMU_SMS_OWN_SRCS
    ${EMU_SMS_DIR}/sms_core.c
    ${EMU_SMS_DIR}/sms_psg.c
    ${EMU_SMS_DIR}/z80/z80.c)

set(EMU_SMS_SRCS ${EMU_SMS_VENDOR_SRCS} ${EMU_SMS_OWN_SRCS})
set(EMU_SMS_INCS ${EMU_SMS_DIR} ${EMU_SMS_DIR}/smsplus ${EMU_SMS_DIR}/z80)
