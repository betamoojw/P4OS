# The simulator's side of the cores (included by sim/CMakeLists.txt when the
# app is in P4OS_SIM_APPS). Each cores/<id>/sim.cmake builds its core for the
# Mac as a static library and appends the target to AOS_SIM_APP_LIBS, which
# the simulator links. In the simulator there is no executable memory to
# hand out: a core with a recompiler builds its interpreter there.
set(EMU_APP_DIR ${CMAKE_CURRENT_LIST_DIR})
set(EMU_MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/main)
file(GLOB _emu_core_files ${CMAKE_CURRENT_LIST_DIR}/cores/*/sim.cmake)
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
    foreach(_id ${EMU_CORE_IDS})
        string(TOUPPER ${_id} _up)
        add_compile_definitions(EMU_HAVE_${_up})
    endforeach()
endforeach()
