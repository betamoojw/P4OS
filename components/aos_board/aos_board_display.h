#pragma once
#include "aos_board.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_touch.h"

esp_lcd_dpi_panel_config_t aos_board_dpi_config(void);
esp_err_t aos_board_panel_new(esp_lcd_panel_io_handle_t io,
    esp_lcd_dsi_bus_handle_t bus, esp_lcd_dpi_panel_config_t *dpi,
    esp_lcd_panel_handle_t *panel);
esp_err_t aos_board_touch_new(esp_lcd_touch_handle_t *touch);
