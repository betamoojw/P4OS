# P4OS - Emulador, Atari 7800 core: the last step of the board build, run
# by board.cmake with cmake -P after the core's objects are linked with -r
# into one (IN).
#
#   cmake -DIN=all.o -DOUT=one.o -DKEEP=emu_core_a7800 -DTAG=a7800
#         -DREADELF=... -DOBJCOPY=... -P one_object.cmake
#
# Makes KEEP the object's only global symbol, and renames every COMDAT
# group's signature (C++'s inline functions and templates;
# ProSystem is C and has none today) with a suffix of this core's own.
# A group's signature is matched by name across a whole link even when its
# symbol is local: two cores in one link both carrying, say,
# std::string::_M_create would have one copy thrown away and the other
# core's local references left pointing into it (ld refuses: "defined in
# discarded section"). Each core is a module of its own today
# (apps/emulador/CMakeLists.txt), so this is a precaution for any link that
# puts cores together again.

execute_process(COMMAND ${READELF} -g -W ${IN}
                OUTPUT_VARIABLE _groups RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "readelf -g ${IN} failed")
endif()
string(REGEX MATCHALL "COMDAT group section \\[ *[0-9]+\\] `\\.group' \\[[^]]+\\]" _lines "${_groups}")
set(_map "")
set(_seen "")
foreach(_l ${_lines})
    string(REGEX REPLACE ".*\\[([^]]+)\\]$" "\\1" _sig "${_l}")
    list(FIND _seen "${_sig}" _i)
    if(_i EQUAL -1)
        list(APPEND _seen "${_sig}")
        string(APPEND _map "${_sig} ${_sig}.${TAG}\n")
    endif()
endforeach()
file(WRITE ${OUT}.syms "${_map}")
execute_process(COMMAND ${OBJCOPY} --redefine-syms=${OUT}.syms
                        --keep-global-symbol=${KEEP} ${IN} ${OUT}
                RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "objcopy on ${IN} failed")
endif()
