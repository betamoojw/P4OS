idf_component_register(
    SRCS "cartero.c" "ct_ui.c" "ct_ui_home.c" "ct_ui_req.c" "ct_ui_resp.c" "ct_ui_env.c" "ct_ui_run.c"
         "ct_store.c" "ct_job.c" "ct_http.c" "ct_val.c" "ct_json.c" "ct_js.c" "ct_pm.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
