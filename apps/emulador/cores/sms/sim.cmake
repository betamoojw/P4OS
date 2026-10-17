# The Sega 8-bit core in the simulator (included by apps/emulador/sim.cmake,
# in sim/CMakeLists.txt's scope, with EMU_MAIN_DIR set): the same sources as
# board.cmake, linked with -r into a single object that keeps only the
# three tables global, since the simulator links every app and every core
# into one executable and SMS Plus's names are generic.

set(EMU_SMS_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${EMU_SMS_DIR}/sources.cmake)
set(EMU_CORE_IDS sms gg col)

add_library(emu_core_sms_sim_objs OBJECT ${EMU_SMS_SRCS})
target_include_directories(emu_core_sms_sim_objs PRIVATE ${EMU_MAIN_DIR} ${EMU_SMS_INCS})
target_compile_definitions(emu_core_sms_sim_objs PRIVATE AOS_SIM)
# -fno-common: a tentative definition would stay a common symbol, which
# the -r link below cannot make local.
target_compile_options(emu_core_sms_sim_objs PRIVATE -O2 -fno-common)
set_source_files_properties(${EMU_SMS_VENDOR_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${EMU_SMS_OWN_SRCS} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

set(_sms_one ${CMAKE_CURRENT_BINARY_DIR}/emu_core_sms_sim.o)
if(APPLE)
    # ld64 turns every global not exported into a local symbol with -r.
    add_custom_command(OUTPUT ${_sms_one}
        COMMAND ${CMAKE_C_COMPILER} -r -nostdlib
                -Wl,-exported_symbol,_emu_core_sms -Wl,-exported_symbol,_emu_core_gg
                -Wl,-exported_symbol,_emu_core_col
                -o ${_sms_one} $<TARGET_OBJECTS:emu_core_sms_sim_objs>
        DEPENDS emu_core_sms_sim_objs $<TARGET_OBJECTS:emu_core_sms_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Sega 8-bit core into one object")
else()
    add_custom_command(OUTPUT ${_sms_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_sms_one}.all $<TARGET_OBJECTS:emu_core_sms_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=emu_core_sms --keep-global-symbol=emu_core_gg
                --keep-global-symbol=emu_core_col ${_sms_one}.all ${_sms_one}
        DEPENDS emu_core_sms_sim_objs $<TARGET_OBJECTS:emu_core_sms_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Sega 8-bit core into one object")
endif()
set_source_files_properties(${_sms_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(emu_core_sms_sim STATIC ${_sms_one})
set_target_properties(emu_core_sms_sim PROPERTIES LINKER_LANGUAGE C)

list(APPEND AOS_SIM_APP_LIBS emu_core_sms_sim)
