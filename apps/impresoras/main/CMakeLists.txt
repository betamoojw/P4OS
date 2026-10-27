idf_component_register(
    SRCS "impresoras.c" "p3d_detail.c" "p3d_edit.c" "p3d_ui.c" "p3d_core.c" "p3d_cfg.c" "p3d_net.c"
         "p3d_json.c" "p3d_mqtt.c" "p3d_cam.c" "p3d_trust.c"
         "drv_octoprint.c" "drv_prusalink.c" "drv_bambu.c" "drv_moonraker.c" "drv_duet.c" "drv_elegoo.c"
         "drv_flashforge.c" "drv_creality.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
