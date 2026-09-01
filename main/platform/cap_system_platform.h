/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "app_settings_service.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_cap_system_platform_init(
    app_settings_service_handle_t settings);

/** Pulse the board vibration motor for the requested duration. */
esp_err_t app_cap_system_platform_vibrate(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
