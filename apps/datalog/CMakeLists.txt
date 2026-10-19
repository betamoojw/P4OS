cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(datalog)

# common.cmake brings the header's API (aos_io.h, aos_sensors.h) too.
include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# Genera datalog.so en lugar de un firmware completo.
include(elf_loader)
project_so(datalog)
