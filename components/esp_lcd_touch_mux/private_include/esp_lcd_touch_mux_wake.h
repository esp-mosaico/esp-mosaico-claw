/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

esp_err_t esp_lcd_touch_mux_gpio_trigger(gpio_num_t gpio_num, void *user_ctx);
