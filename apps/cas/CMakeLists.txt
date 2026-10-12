cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cas)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The engine (Giac and its runtime) is C++ and comes prebuilt as PIC by
# engine/build_engine.sh: project_so() only compiles the C in main/. The end
# marker of .init_array goes last on purpose (engine/cas_end.c).
set(_cas_lib ${CMAKE_CURRENT_LIST_DIR}/engine/lib)
if(NOT EXISTS ${_cas_lib}/libcasengine.a)
    message(FATAL_ERROR "CAS: the engine is not built. Run apps/cas/engine/build_engine.sh first.")
endif()
# cas_exports.o is engine/cas_exports.ld: a linker script under a name
# project_so() lets through.
set(ELF_LIBS ${_cas_lib}/cas_exports.o ${_cas_lib}/libcasengine.a ${_cas_lib}/libcasend.a)

# Genera cas.so en lugar de un firmware completo.
include(elf_loader)
project_so(cas)
