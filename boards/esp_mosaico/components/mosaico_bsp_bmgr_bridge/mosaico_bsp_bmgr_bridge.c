/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_bsp_bmgr_bridge.h"

#include "bsp/esp_mosaico.h"
#include "dev_display_lcd.h"
#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"

static const char *TAG = "mosaico_bmgr";

static esp_err_t bsp_i2c_provider_init(void *cfg, int cfg_size, void **periph_handle)
{
    ESP_RETURN_ON_FALSE(cfg && cfg_size >= sizeof(i2c_master_bus_config_t) && periph_handle, ESP_ERR_INVALID_ARG, TAG,
                        "invalid Board Manager I2C configuration");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "initialize BSP I2C failed");
    *periph_handle = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(*periph_handle, ESP_ERR_INVALID_STATE, TAG, "BSP I2C handle is null");
    return ESP_OK;
}

static esp_err_t bsp_i2c_provider_deinit(void *periph_handle)
{
    ESP_RETURN_ON_FALSE(periph_handle == bsp_i2c_get_handle(), ESP_ERR_INVALID_ARG, TAG, "unknown BSP I2C handle");
    return ESP_OK;
}

static esp_err_t override_lcd_pins(bsp_board_variant_t variant)
{
    periph_spi_config_t *spi_cfg = NULL;
    void *display_cfg_raw = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_periph_config("spi_display", (void **)&spi_cfg), TAG, "get SPI config failed");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config("display_lcd", &display_cfg_raw), TAG, "get LCD config failed");
    ESP_RETURN_ON_FALSE(spi_cfg && display_cfg_raw, ESP_ERR_INVALID_STATE, TAG, "board display config unavailable");

    spi_cfg->spi_bus_config.sclk_io_num = variant == BSP_BOARD_VARIANT_V1_2 ? BSP_LCD_SCL_V1_2 : BSP_LCD_SCL_V1_0;
    dev_display_lcd_config_t display_cfg = *(const dev_display_lcd_config_t *)display_cfg_raw;
    display_cfg.sub_cfg.spi.panel_config.reset_gpio_num = variant == BSP_BOARD_VARIANT_V1_2 ? BSP_LCD_RST_V1_2 : BSP_LCD_RST_V1_0;
    return esp_board_device_override_config("display_lcd", &display_cfg, sizeof(display_cfg));
}

esp_err_t mosaico_bsp_bmgr_init(void)
{
    bsp_board_variant_t variant;
    ESP_RETURN_ON_ERROR(bsp_board_variant_get(&variant), TAG, "get BSP board variant failed");
    ESP_RETURN_ON_ERROR(override_lcd_pins(variant), TAG, "apply LCD pins failed");
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 failed");
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG, "enable codec power failed");
    ESP_RETURN_ON_ERROR(esp_board_periph_init_custom("i2c_master", bsp_i2c_provider_init, bsp_i2c_provider_deinit), TAG,
                        "install BSP I2C provider failed");
    return esp_board_periph_unref_handle("i2c_master");
}
