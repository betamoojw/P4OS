cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emulador)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The consoles. Each core is a static library of its own, compiled here with
# its own flags (its optimisation, its defines, C++ and assembly where it has
# them) and linked into emulador.so through ELF_LIBS. project_so() only
# compiles the .c files under main/, with -Os and the same flags for all of
# them, which suits the app's screens and not an emulator's inner loop.
# cores/emu_cores.cmake has the helper every cores/<id>/board.cmake calls.
# A console whose cores/<id>/board.cmake is not there yet is left out, and
# the app is told by EMU_HAVE_<ID> (main/emu_cores.c).
set(EMU_CORES "")
set(EMU_CORE_LIBS "")
include(${CMAKE_CURRENT_LIST_DIR}/cores/emu_cores.cmake)
foreach(_core nes gba)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/cores/${_core}/board.cmake)
        include(${CMAKE_CURRENT_LIST_DIR}/cores/${_core}/board.cmake)
        list(APPEND EMU_CORES ${_core})
        string(TOUPPER ${_core} _up)
        idf_build_set_property(COMPILE_DEFINITIONS "EMU_HAVE_${_up}" APPEND)
    endif()
endforeach()
set(ELF_LIBS ${EMU_CORE_LIBS})

include(elf_loader)
project_so(emulador)

# project_so() links the libraries by path: build them first.
foreach(_core ${EMU_CORES})
    add_dependencies(so emu_core_${_core})
endforeach()
