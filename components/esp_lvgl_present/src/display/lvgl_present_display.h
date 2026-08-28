/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lv_adapter_display.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_display_t *lvgl_present_display_register(
    const esp_lv_adapter_display_config_t *cfg,
    const esp_lv_adapter_presenter_config_t *presenter_cfg);

esp_err_t lvgl_present_display_unregister(lv_display_t *disp);
void lvgl_present_display_clear(void);
esp_err_t lvgl_present_display_wait_flush_done(lv_display_t *disp, int32_t timeout_ms);

#ifdef __cplusplus
}
#endif
