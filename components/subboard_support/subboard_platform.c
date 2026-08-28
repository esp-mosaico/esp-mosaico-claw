/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "subboard_platform.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "subboard_support/subboard.h"

#define SUBBOARD_I2C_PORT          I2C_NUM_0
#define SUBBOARD_I2C_SDA           GPIO_NUM_0
#define SUBBOARD_I2C_SCL           GPIO_NUM_1
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

    if (i2c_master_get_bus_handle(SUBBOARD_I2C_PORT, &s_i2c_bus) == ESP_OK &&
            s_i2c_bus != NULL) {
        ESP_LOGI(TAG, "Reusing board-manager I2C bus on port %d",
                 SUBBOARD_I2C_PORT);
        return ESP_OK;
    }

    const i2c_master_bus_config_t config = {
        .i2c_port = SUBBOARD_I2C_PORT,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = SUBBOARD_I2C_SDA,
        .scl_io_num = SUBBOARD_I2C_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_i2c_bus), TAG,
                        "create subboard I2C bus failed");
    ESP_LOGI(TAG, "Created subboard I2C bus on port %d",
             SUBBOARD_I2C_PORT);
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
