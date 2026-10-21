cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emulador)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The consoles. Each core is a static library of its own, compiled here with
# its own flags (its optimisation, its defines, C++ and assembly where it has
# them): cores/emu_cores.cmake has the helper every cores/<dir>/board.cmake
# calls. Each library then becomes a MODULE in build/<profile>/modules/<dir>.so,
# which tools/install_apps.sh puts in the card's apps/emulador/ and the app
# loads with aos_hal_module_open() only while one of its consoles plays: the
# app itself (emulador.so, the screens) carries none of them. So a console
# costs memory only when it plays, the cores' licences stay apart (gwenesis
# is AGPL), and a console can be added to a card without the app.
#
# A cores/<dir>/board.cmake may set EMU_CORE_IDS to the consoles (emu_core_t
# tables) it brings, when they are more than one or not named as the folder
# (the Sega 8-bit core brings sms, gg and col).
set(EMU_CORES "")
include(${CMAKE_CURRENT_LIST_DIR}/cores/emu_cores.cmake)
file(GLOB _emu_core_files ${CMAKE_CURRENT_LIST_DIR}/cores/*/board.cmake)
foreach(_f ${_emu_core_files})
    get_filename_component(_dir ${_f} DIRECTORY)
    get_filename_component(_core ${_dir} NAME)
    # -DEMU_ONLY="nes;gba" (or the environment's EMU_ONLY="nes gba") builds
    # just those cores: one being written elsewhere does not break the build
    set(_only "${EMU_ONLY}")
    if(NOT _only AND DEFINED ENV{EMU_ONLY})
        string(REPLACE " " ";" _only "$ENV{EMU_ONLY}")
    endif()
    if(_only AND NOT _core IN_LIST _only)
        continue()
    endif()
    set(EMU_CORE_IDS "")
    include(${_f})
    if(NOT EMU_CORE_IDS)
        set(EMU_CORE_IDS ${_core})
    endif()
    list(APPEND EMU_CORES ${_core})
    set(EMU_IDS_${_core} ${EMU_CORE_IDS})
endforeach()

include(elf_loader)
project_so(emulador)

# Each core's module: the generated entry points (cores/emu_module.c.in)
# and the core's library, linked and stripped exactly as project_so() links
# and strips an app (its so_* lists are the macro's, so in scope here).
set(_mod_dir ${CMAKE_BINARY_DIR}/modules)
file(MAKE_DIRECTORY ${_mod_dir})
set(_mod_targets "")
foreach(_core ${EMU_CORES})
    set(EMU_DIR ${_core})
    set(EMU_EXTERNS "")
    set(EMU_TABLES "")
    foreach(_id ${EMU_IDS_${_core}})
        string(APPEND EMU_EXTERNS "extern const emu_core_t emu_core_${_id};\n")
        string(APPEND EMU_TABLES "&emu_core_${_id}, ")
    endforeach()
    string(REPLACE "\n" "
" EMU_EXTERNS "${EMU_EXTERNS}")
    configure_file(${CMAKE_CURRENT_LIST_DIR}/cores/emu_module.c.in ${_mod_dir}/${_core}_module.c @ONLY)
    set(_entry_o ${_mod_dir}/${_core}_module.o)
    set(_mod_so ${_mod_dir}/${_core}.so)
    add_custom_command(OUTPUT ${_mod_so}
        COMMAND ${CMAKE_C_COMPILER} ${so_compile_flags} -I${CMAKE_CURRENT_LIST_DIR}/main
                -MMD -MF ${_entry_o}.d ${_mod_dir}/${_core}_module.c -o ${_entry_o}
        COMMAND ${CMAKE_C_COMPILER} ${so_link_flags} -o ${_mod_so} ${_entry_o}
                ${CMAKE_BINARY_DIR}/cores/libemu_core_${_core}.a -Wl,--allow-shlib-undefined
        COMMAND ${CMAKE_STRIP_SO} ${so_strip_flags} ${_mod_so}
        DEPENDS emu_core_${_core} ${CMAKE_BINARY_DIR}/cores/libemu_core_${_core}.a
                ${_mod_dir}/${_core}_module.c ${so_obj_dir}/compile.rsp
        DEPFILE ${_entry_o}.d
        COMMENT "Build the module ${_core}.so"
        VERBATIM)
    list(APPEND _mod_targets ${_mod_so})
endforeach()
add_custom_target(emu_modules ALL DEPENDS ${_mod_targets})
add_dependencies(so emu_modules)
