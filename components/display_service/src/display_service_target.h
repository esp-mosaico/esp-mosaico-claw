/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "devices/dev_display_lcd/dev_display_lcd.h"
#include "esp_display_present.h"
#include "esp_lcd_touch.h"

esp_err_t display_service_target_build(
    const dev_display_lcd_config_t *lcd_cfg,
    const dev_display_lcd_handles_t *lcd_handles,
    esp_display_present_target_config_t *out_target);

esp_err_t display_service_target_load_display(
    dev_display_lcd_config_t **out_lcd_cfg,
    dev_display_lcd_handles_t **out_lcd_handles);

esp_lcd_touch_handle_t display_service_target_load_touch(void);
