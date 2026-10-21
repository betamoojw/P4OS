idf_component_register(SRCS "aos_board_display.c"
    INCLUDE_DIRS "." "../aos_hal/include"
    REQUIRES esp_lcd espressif__esp_lcd_touch
    PRIV_REQUIRES waveshare__esp32_p4_wifi6_touch_lcd_5
        waveshare__esp_lcd_hx8394 waveshare__esp_lcd_ili9881c
        espressif__esp_lcd_jd9365 espressif__esp_lcd_touch_gt911)
