# The interpreter's sources are not listed: the .so takes them by
# project_so()'s GLOB_RECURSE. This list is the ordinary component build's,
# which only needs to compile the app.
idf_component_register(
    SRCS "py_app.c" "p4py_rt.c" "p4py_term.c" "p4py_kbd.c" "mod_p4.c" "mod_card.c" "mod_net.c"
    INCLUDE_DIRS "." "upy"
    REQUIRES lvgl
)
