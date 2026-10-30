/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "app_settings_service.h"
#include "esp_err.h"
#include "cap_system.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_cap_system_platform_init(
    app_settings_service_handle_t settings);
/** Register the board battery adapter before platform init / task startup. */
esp_err_t app_cap_system_platform_set_battery_provider(
    cap_system_platform_get_info_fn get_info, void *user_ctx);

/** Pulse the board vibration motor for the requested duration. */
esp_err_t app_cap_system_platform_vibrate(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
