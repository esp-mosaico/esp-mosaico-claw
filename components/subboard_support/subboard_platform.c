/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "subboard_platform.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_log.h"
#include "subboard_support/subboard.h"

#define SUBBOARD_I2C_NAME          "i2c_subboard"
#define SUBBOARD_VCC_ENABLE_GPIO   GPIO_NUM_60
#define SUBBOARD_VCC_ON_LEVEL      0

static const char *TAG = "subboard_platform";
static i2c_master_bus_handle_t s_i2c_bus;
static bool s_power_initialized;

esp_err_t subboard_platform_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_board_periph_get_handle(SUBBOARD_I2C_NAME, (void **)&s_i2c_bus), TAG,
                        "get Board Manager subboard I2C failed");
    ESP_RETURN_ON_FALSE(s_i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "Board Manager subboard I2C handle is NULL");
    ESP_LOGI(TAG, "Using Board Manager peripheral '%s'", SUBBOARD_I2C_NAME);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_subboard_get_i2c_bus(void)
{
    return s_i2c_bus;
}

static esp_err_t subboard_platform_power_init(void)
{
    if (s_power_initialized) {
        return ESP_OK;
    }

    const int current_level = gpio_get_level(SUBBOARD_VCC_ENABLE_GPIO);
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(SUBBOARD_VCC_ENABLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(SUBBOARD_VCC_ENABLE_GPIO,
                                      current_level),
                        TAG, "preserve subboard VCC level failed");
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG,
                        "configure subboard VCC control failed");
    s_power_initialized = true;
    return ESP_OK;
}

esp_err_t subboard_platform_set_power(bool on)
{
    ESP_RETURN_ON_ERROR(subboard_platform_power_init(), TAG,
                        "initialize subboard power failed");
    const int level = on ? SUBBOARD_VCC_ON_LEVEL
                         : !SUBBOARD_VCC_ON_LEVEL;
    ESP_RETURN_ON_ERROR(gpio_set_level(SUBBOARD_VCC_ENABLE_GPIO, level), TAG,
                        "set subboard VCC failed");
    ESP_LOGI(TAG, "Subboard VCC_3V3 %s", on ? "enabled" : "disabled");
    return ESP_OK;
}
