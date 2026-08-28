/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_board_variant.h"

#include "dev_display_lcd.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"

#define MOSAICO_VARIANT_I2C_PORT I2C_NUM_0
#define MOSAICO_V1_0_I2C_SDA GPIO_NUM_0
#define MOSAICO_V1_0_I2C_SCL GPIO_NUM_1
#define MOSAICO_V1_1_I2C_SDA GPIO_NUM_56
#define MOSAICO_V1_1_I2C_SCL GPIO_NUM_3
#define MOSAICO_V1_0_CODEC_POWER_GPIO GPIO_NUM_56
#define MOSAICO_HW_VERSION(major, minor) ((uint16_t)(((uint16_t)(major) << 8) | ((uint16_t)(minor) & 0xFFU)))
#define MOSAICO_HW_VERSION_MAJOR(version) ((uint8_t)((version) >> 8))
#define MOSAICO_HW_VERSION_MINOR(version) ((uint8_t)((version) & 0xFFU))
#define MOSAICO_HW_VERSION_UNPROGRAMMED MOSAICO_HW_VERSION(0, 0)
#define ESP_EFUSE_USER_DATA_HW_VERSION ESP_EFUSE_USER_DATA

static const char *TAG = "MOSAICO_VARIANT";
static mosaico_board_variant_t s_variant = MOSAICO_BOARD_VARIANT_V1_0;
static bool s_prepared;

static esp_err_t read_hardware_version(uint16_t *version)
{
    ESP_RETURN_ON_FALSE(version != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid hardware version output");
    *version = MOSAICO_HW_VERSION_UNPROGRAMMED;
    return esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA_HW_VERSION, version, sizeof(*version) * 8U);
}

static esp_err_t mosaico_i2c_init(void *cfg, int cfg_size, void **periph_handle)
{
    (void)cfg;
    (void)cfg_size;
    ESP_RETURN_ON_FALSE(periph_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid I2C output handle");
    const gpio_num_t sda = s_variant == MOSAICO_BOARD_VARIANT_V1_1 ? MOSAICO_V1_1_I2C_SDA : MOSAICO_V1_0_I2C_SDA;
    const gpio_num_t scl = s_variant == MOSAICO_BOARD_VARIANT_V1_1 ? MOSAICO_V1_1_I2C_SCL : MOSAICO_V1_0_I2C_SCL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = MOSAICO_VARIANT_I2C_PORT,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 1,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "create I2C bus on SDA=%d SCL=%d failed", sda, scl);
    *periph_handle = bus;
    ESP_LOGI(TAG, "I2C bus configured for v%d.%d: SDA=%d SCL=%d", s_variant == MOSAICO_BOARD_VARIANT_V1_1 ? 1 : 1,
             s_variant == MOSAICO_BOARD_VARIANT_V1_1 ? 1 : 0, sda, scl);
    return ESP_OK;
}

static esp_err_t mosaico_i2c_deinit(void *periph_handle)
{
    ESP_RETURN_ON_FALSE(periph_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid I2C handle");
    return i2c_del_master_bus((i2c_master_bus_handle_t)periph_handle);
}

static esp_err_t configure_v1_0_codec_power(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << MOSAICO_V1_0_CODEC_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure codec power GPIO failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(MOSAICO_V1_0_CODEC_POWER_GPIO, 1), TAG, "enable codec power failed");
    return ESP_OK;
}

static esp_err_t override_lcd_pins(void)
{
    periph_spi_config_t *spi_cfg = NULL;
    void *display_cfg_raw = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_periph_config("spi_display", (void **)&spi_cfg), TAG, "get SPI config failed");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config("display_lcd", &display_cfg_raw), TAG, "get LCD config failed");
    ESP_RETURN_ON_FALSE(spi_cfg != NULL && display_cfg_raw != NULL, ESP_ERR_INVALID_STATE, TAG, "board display config unavailable");

    spi_cfg->spi_bus_config.sclk_io_num = s_variant == MOSAICO_BOARD_VARIANT_V1_1 ? GPIO_NUM_42 : GPIO_NUM_44;
    dev_display_lcd_config_t display_cfg = *(const dev_display_lcd_config_t *)display_cfg_raw;
    display_cfg.sub_cfg.spi.panel_config.reset_gpio_num = s_variant == MOSAICO_BOARD_VARIANT_V1_1 ? GPIO_NUM_44 : GPIO_NUM_42;
    ESP_RETURN_ON_ERROR(esp_board_device_override_config("display_lcd", &display_cfg, sizeof(display_cfg)), TAG,
                        "override LCD config failed");
    return ESP_OK;
}

esp_err_t mosaico_board_variant_prepare(void)
{
    if (s_prepared) return ESP_OK;
    uint16_t version = MOSAICO_HW_VERSION_UNPROGRAMMED;
    ESP_RETURN_ON_ERROR(read_hardware_version(&version), TAG, "read hardware version from eFuse failed");
    switch (version) {
        case MOSAICO_HW_VERSION(1, 0):
            s_variant = MOSAICO_BOARD_VARIANT_V1_0;
            break;
        case MOSAICO_HW_VERSION(1, 1):
            s_variant = MOSAICO_BOARD_VARIANT_V1_1;
            break;
        default:
            ESP_LOGE(TAG, "unsupported hardware version: v%u.%u (raw=0x%04X)", (unsigned)(version >> 8),
                     (unsigned)(version & 0xFFU), (unsigned)version);
            vTaskDelay(pdMS_TO_TICKS(1000));
            return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "hardware version: v%u.%u (raw=0x%04X)", MOSAICO_HW_VERSION_MAJOR(version), MOSAICO_HW_VERSION_MINOR(version), (unsigned)version);
    ESP_RETURN_ON_ERROR(override_lcd_pins(), TAG, "apply LCD pins failed");
    ESP_RETURN_ON_ERROR(esp_board_periph_init_custom("i2c_master", mosaico_i2c_init, mosaico_i2c_deinit), TAG, "install board I2C failed");
    if (s_variant == MOSAICO_BOARD_VARIANT_V1_0) {
        ESP_RETURN_ON_ERROR(configure_v1_0_codec_power(), TAG, "enable v1.0 codec power failed");
    }
    s_prepared = true;
    return ESP_OK;
}

mosaico_board_variant_t mosaico_board_variant_get(void)
{
    return s_variant;
}
