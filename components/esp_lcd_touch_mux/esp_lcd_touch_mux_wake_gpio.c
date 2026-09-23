/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_lcd_touch_mux_wake.h"

#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"

esp_err_t esp_lcd_touch_mux_gpio_trigger(gpio_num_t gpio_num, void *user_ctx)
{
    (void)user_ctx;
    if (!GPIO_IS_VALID_GPIO(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
#if defined(GPIO_STATUS_W1TS_REG)
    if (gpio_num < 32) {
        REG_WRITE(GPIO_STATUS_W1TS_REG, 1U << gpio_num);
        return ESP_OK;
    }
#if defined(GPIO_STATUS1_W1TS_REG)
    if (gpio_num < 64) {
        REG_WRITE(GPIO_STATUS1_W1TS_REG, 1U << (gpio_num - 32));
        return ESP_OK;
    }
#endif
#endif
    return ESP_ERR_NOT_SUPPORTED;
}
