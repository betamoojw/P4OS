# The simulator's side of the cores (included by sim/CMakeLists.txt when the
# app is in P4OS_SIM_APPS). Each cores/<id>/sim.cmake builds its core for the
# Mac as a static library and appends the target to AOS_SIM_APP_LIBS, which
# the simulator links. In the simulator there is no executable memory to
# hand out: a core with a recompiler builds its interpreter there.
set(EMU_APP_DIR ${CMAKE_CURRENT_LIST_DIR})
set(EMU_MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/main)
foreach(_core nes gba)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/cores/${_core}/sim.cmake)
        include(${CMAKE_CURRENT_LIST_DIR}/cores/${_core}/sim.cmake)
        string(TOUPPER ${_core} _up)
        add_compile_definitions(EMU_HAVE_${_up})
    endif()
endforeach()
