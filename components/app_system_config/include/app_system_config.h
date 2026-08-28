/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_SYSTEM_BOOT_SETUP = 0,
    APP_SYSTEM_BOOT_WELCOME,
    APP_SYSTEM_BOOT_HOME,
} app_system_boot_stage_t;

typedef struct {
    app_system_boot_stage_t boot_stage;
    uint16_t rotation;
    uint8_t brightness;
    uint8_t volume;
    uint32_t screen_timeout_ms;
    bool vibration_enabled;
    bool wifi_enabled;
} app_system_config_t;

void app_system_config_defaults(app_system_config_t *config);
esp_err_t app_system_config_load(app_system_config_t *config);
esp_err_t app_system_config_save(const app_system_config_t *config);
esp_err_t app_system_config_reset(void);

#ifdef __cplusplus
}
#endif
