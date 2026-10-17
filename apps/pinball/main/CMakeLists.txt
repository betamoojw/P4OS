idf_component_register(
    SRCS "pinball.c" "pb_pak.c" "pb_table.c" "pb_phys.c" "pb_game.c" "pb_dmd.c" "pb_render.c" "pb_audio.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
# -O2 for the physics and the pixel loops is a #pragma in those files: the
# .so is compiled by elf_loader.cmake with -Os, and per-file options do not
# reach it.
