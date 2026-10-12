idf_component_register(
    SRCS "cas_app.c" "cas_rt.c" "cas_rt_sim.c" "tlsf.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
