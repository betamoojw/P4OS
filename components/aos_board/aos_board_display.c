#include "aos_board_display.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"

#if AOS_BOARD_ID == 5
#include "esp_lcd_hx8394.h"
#elif AOS_BOARD_ID == 7
#include "esp_lcd_ili9881c.h"
#include "panel_commands.h"
#else
#include "esp_lcd_jd9365.h"
#include "panel_commands.h"
#endif

esp_lcd_dpi_panel_config_t aos_board_dpi_config(void)
{
#if AOS_BOARD_ID == 5
    return (esp_lcd_dpi_panel_config_t)HX8394_720_1280_PANEL_30HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
#elif AOS_BOARD_ID == 7
    /* Waveshare LCD-X BSP 2.0.3's ILI9881C timing. */
    return (esp_lcd_dpi_panel_config_t) {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 80, .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .video_timing = {
            .h_size = 720, .v_size = 1280,
            .hsync_back_porch = 239, .hsync_pulse_width = 50, .hsync_front_porch = 33,
            .vsync_back_porch = 20, .vsync_pulse_width = 30, .vsync_front_porch = 2,
        },
    };
#else
    return (esp_lcd_dpi_panel_config_t)JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
#endif
}

esp_err_t aos_board_panel_new(esp_lcd_panel_io_handle_t io,
    esp_lcd_dsi_bus_handle_t bus, esp_lcd_dpi_panel_config_t *dpi,
    esp_lcd_panel_handle_t *panel)
{
#if AOS_BOARD_ID == 5
    hx8394_vendor_config_t vendor = {
#elif AOS_BOARD_ID == 7
    ili9881c_vendor_config_t vendor = {
        .init_cmds = ili9881c_vendor_specific_init_default,
        .init_cmds_size = sizeof(ili9881c_vendor_specific_init_default) / sizeof(ili9881c_vendor_specific_init_default[0]),
#else
    jd9365_vendor_config_t vendor = {
        .init_cmds = jd9365_vendor_specific_init_default,
        .init_cmds_size = sizeof(jd9365_vendor_specific_init_default) / sizeof(jd9365_vendor_specific_init_default[0]),
#endif
        .mipi_config = { .dsi_bus = bus, .dpi_config = dpi, .lane_num = 2 },
    };
    esp_lcd_panel_dev_config_t dev = {
        .bits_per_pixel = 16, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = BSP_LCD_RST, .vendor_config = &vendor,
        .flags.reset_active_high = AOS_BOARD_ID == 5,
    };
#if AOS_BOARD_ID == 5
    return esp_lcd_new_panel_hx8394(io, &dev, panel);
#elif AOS_BOARD_ID == 7
    return esp_lcd_new_panel_ili9881c(io, &dev, panel);
#else
    return esp_lcd_new_panel_jd9365(io, &dev, panel);
#endif
}

esp_err_t aos_board_touch_new(esp_lcd_touch_handle_t *touch)
{
#if AOS_BOARD_ID == 5
    bsp_display_cfg_t cfg = {0};
    return bsp_touch_new(&cfg, touch);
#else
    /* GT9271 uses the GT911 protocol in Waveshare's BSP. LCD-X has no
     * connected touch reset/interrupt GPIOs. Never drive LCD-5's pins here. */
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) return err;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    if (i2c_master_probe(bus, io_cfg.dev_addr, 50) != ESP_OK) {
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        if (i2c_master_probe(bus, io_cfg.dev_addr, 50) != ESP_OK) return ESP_ERR_NOT_FOUND;
    }
    io_cfg.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    esp_lcd_panel_io_handle_t io;
    err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err != ESP_OK) return err;
    const esp_lcd_touch_config_t cfg = {
        .x_max = AOS_NATIVE_W, .y_max = AOS_NATIVE_H,
        .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = GPIO_NUM_NC,
    };
    err = esp_lcd_touch_new_i2c_gt911(io, &cfg, touch);
    if (err != ESP_OK) esp_lcd_panel_io_del(io);
    return err;
#endif
}
