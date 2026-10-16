# The helper for the cores' board builds (cores/<id>/board.cmake).
#
#   emu_core_board_lib(<id> <sources...>)
#
# makes the static library emu_core_<id> out of C, C++ and assembly sources,
# position-independent (it ends up inside a shared object), with sections per
# function so the .so's --gc-sections drops what nothing calls, hidden (the
# .so exports only the app's entry points, not a core's thousand globals), and adds it
# to EMU_CORE_LIBS. The toolchain's -march/-mabi come from CMAKE_C_FLAGS, as
# for any target of the project. The core's own options (its -O, defines,
# include directories, extra -march extensions) are added by its board.cmake
# on the target afterwards.
#
# Two things the .so's own compile step does, done here too: no absolute
# paths from this machine inside the binary (__FILE__ in an assert would
# carry /Users/... into the release, and build_apps.sh refuses that), and
# emu_core.h reachable from every core.

get_filename_component(EMU_REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(EMU_MAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../main")

function(emu_core_board_lib id)
    add_library(emu_core_${id} STATIC ${ARGN})
    idf_build_get_property(_idf_path IDF_PATH)
    target_compile_options(emu_core_${id} PRIVATE
        -fPIC
        -ffunction-sections
        -fdata-sections
        -fvisibility=hidden
        -ffile-prefix-map=${EMU_REPO}/=
        -ffile-prefix-map=${_idf_path}/=esp-idf/
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
    target_include_directories(emu_core_${id} PRIVATE ${EMU_MAIN_DIR})
    set_target_properties(emu_core_${id} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/cores)
    set(EMU_CORE_LIBS ${EMU_CORE_LIBS} ${CMAKE_BINARY_DIR}/cores/libemu_core_${id}.a PARENT_SCOPE)
endfunction()
