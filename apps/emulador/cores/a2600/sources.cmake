# The Atari 2600 core's sources, for board.cmake and sim.cmake alike: Stella
# 3.9.3 as RetroESP32-P4 carries it (stella/), with the bank-switching
# schemes this port adds back (see README.md), and the adapter. Left out of
# Stella's tree: the GUI, the debugger, the cheats, the SDL sound and video,
# and the schemes that need exceptions or files of their own (FA2, MC, CM,
# CTY); their sources are not vendored.

set(_s ${EMU_A2600_DIR}/stella)
set(EMU_A2600_VENDOR_SRCS
    ${_s}/common/Base.cpp
    ${_s}/common/SoundSDL.cpp
    ${_s}/emucore/Booster.cpp
    ${_s}/emucore/Cart.cpp
    ${_s}/emucore/Cart0840.cpp
    ${_s}/emucore/Cart2K.cpp
    ${_s}/emucore/Cart3E.cpp
    ${_s}/emucore/Cart3F.cpp
    ${_s}/emucore/Cart4A50.cpp
    ${_s}/emucore/Cart4K.cpp
    ${_s}/emucore/Cart4KSC.cpp
    ${_s}/emucore/CartAR.cpp
    ${_s}/emucore/CartBF.cpp
    ${_s}/emucore/CartBFSC.cpp
    ${_s}/emucore/CartCV.cpp
    ${_s}/emucore/CartDF.cpp
    ${_s}/emucore/CartDFSC.cpp
    ${_s}/emucore/CartDPC.cpp
    ${_s}/emucore/CartDPCPlus.cpp
    ${_s}/emucore/CartE0.cpp
    ${_s}/emucore/CartE7.cpp
    ${_s}/emucore/CartEF.cpp
    ${_s}/emucore/CartEFSC.cpp
    ${_s}/emucore/CartF0.cpp
    ${_s}/emucore/CartF4.cpp
    ${_s}/emucore/CartF4SC.cpp
    ${_s}/emucore/CartF6.cpp
    ${_s}/emucore/CartF6SC.cpp
    ${_s}/emucore/CartF8.cpp
    ${_s}/emucore/CartF8SC.cpp
    ${_s}/emucore/CartFA.cpp
    ${_s}/emucore/CartFE.cpp
    ${_s}/emucore/CartSB.cpp
    ${_s}/emucore/CartUA.cpp
    ${_s}/emucore/CartX07.cpp
    ${_s}/emucore/Console.cpp
    ${_s}/emucore/Control.cpp
    ${_s}/emucore/Driving.cpp
    ${_s}/emucore/Genesis.cpp
    ${_s}/emucore/Joystick.cpp
    ${_s}/emucore/Keyboard.cpp
    ${_s}/emucore/M6502.cpp
    ${_s}/emucore/M6532.cpp
    ${_s}/emucore/MD5.cpp
    ${_s}/emucore/NullDev.cpp
    ${_s}/emucore/Paddles.cpp
    ${_s}/emucore/Props.cpp
    ${_s}/emucore/PropsSet.cpp
    ${_s}/emucore/Random.cpp
    ${_s}/emucore/Serializer.cpp
    ${_s}/emucore/Settings.cpp
    ${_s}/emucore/Stubs.cpp
    ${_s}/emucore/Switches.cpp
    ${_s}/emucore/System.cpp
    ${_s}/emucore/Thumbulator.cpp
    ${_s}/emucore/TIA.cpp
    ${_s}/emucore/TIASnd.cpp
    ${_s}/emucore/TIATables.cpp
    ${_s}/emucore/TrackBall.cpp)

set(EMU_A2600_OWN_SRCS
    ${EMU_A2600_DIR}/a2600_core.cpp
    ${EMU_A2600_DIR}/a2600_rt.cpp
    ${EMU_A2600_DIR}/a2600_libc.c)

set(EMU_A2600_SRCS ${EMU_A2600_VENDOR_SRCS} ${EMU_A2600_OWN_SRCS})

# compat/ first: its esp_attr.h stands in for ESP-IDF's.
set(EMU_A2600_INCLUDES
    ${EMU_A2600_DIR}/compat
    ${_s}/common
    ${_s}/emucore
    ${_s}/gui)

# THUMB_SUPPORT: the ARM emulation DPC+ cartridges run their code on.
# NDEBUG: Stella's asserts out (the firmware has no __assert_func).
set(EMU_A2600_DEFINES SOUND_SUPPORT=1 THUMB_SUPPORT NDEBUG)
