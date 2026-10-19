idf_component_register(
    SRCS "datalog.c" "dl_cfg.c" "dl_engine.c" "dl_src.c" "dl_csv.c" "dl_ui.c" "dl_list.c" "dl_view.c" "dl_edit.c" "dl_i18n.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
