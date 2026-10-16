# The app's own files only. The consoles' cores are libraries of their own
# (cores/<id>/board.cmake, linked into the .so by ../CMakeLists.txt): the
# .so's compile step globs every .c under main/, so nothing of theirs lives
# here.
idf_component_register(
    SRCS "emulador.c" "emu_cores.c" "em_files.c" "em_run.c" "em_game.c" "em_menu.c" "em_lib.c" "em_strings.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
