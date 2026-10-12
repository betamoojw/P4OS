idf_component_register(
    SRCS "cas_app.c" "cas_calc.c" "cas_graph.c" "cas_keypad.c" "cas_plot.c" "cas_num.c" "cas_form.c" "cas_stats.c" "cas_fin.c" "cas_geo.c" "cas_geom.c" "cas_save.c" "cas_grid.c"
         "mx_box.c" "mx_edit.c" "mx_layout.c" "mx_parse.c" "mx_util.c" "mx_view.c"
         "cas_rt.c" "cas_rt_sim.c" "tlsf.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
