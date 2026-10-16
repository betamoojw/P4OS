cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(duke3d)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The Build engine and the game define their globals in headers ("int32_t
# numtiles;" in a .h that twenty files include), which C has always merged
# into one variable and gcc 10 stopped doing by default. -fcommon brings it
# back. It goes through CMAKE_C_FLAGS because elf_loader.cmake's project_so
# builds the .so's command lines by hand and only adds the toolchain's flags
# (that variable) to its own; per-file options would not reach it.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fcommon")

# And the C the code was written for (Watcom, 1996): signed arithmetic that
# wraps (the fixed-point maths counts on it), pointers walked past zero (the
# column loops in engine/draw.c run a pointer up from -n until it wraps), and
# the same memory read as bytes and as longs. An optimising gcc may assume
# none of that happens; these three tell it that it does.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fwrapv -fwrapv-pointer -fno-strict-aliasing")

# Genera duke3d.so en lugar de un firmware completo.
include(elf_loader)
project_so(duke3d)
