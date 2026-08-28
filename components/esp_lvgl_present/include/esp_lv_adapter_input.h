/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"
#include "esp_lv_adapter_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*custom_touch_read)(esp_lcd_touch_handle_t tp,
                                   esp_lcd_touch_point_data_t *points, uint8_t *count,
                                   uint8_t max_count, void *user_ctx);
    void *user_ctx;
} esp_lv_adapter_touch_callbacks_t;

typedef struct {
    lv_display_t *disp;
    esp_lcd_touch_handle_t handle;
    struct {
        float x;
        float y;
    } scale;
    esp_lv_adapter_touch_callbacks_t callbacks;
} esp_lv_adapter_touch_config_t;

#define ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch_handle) { \
    .disp = display,                                            \
    .handle = touch_handle,                                     \
    .scale = {                                                  \
        .x = 1.0f,                                              \
        .y = 1.0f,                                              \
    },                                                          \
}

lv_indev_t *esp_lv_adapter_register_touch(const esp_lv_adapter_touch_config_t *config);
esp_err_t esp_lv_adapter_unregister_touch(lv_indev_t *touch);

#ifdef __cplusplus
}
#endif
