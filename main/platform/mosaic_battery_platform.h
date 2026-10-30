/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "mosaic_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the device battery sampler and publish subsequent revisions. */
esp_err_t mosaic_battery_platform_start(void);

/** Return cached telemetry with freshness/error metadata; never performs I2C. */
esp_err_t mosaic_battery_platform_get(
    mosaic_settings_battery_t *ret_battery);
/** cap_system adapter using the same sample as the UI. */
esp_err_t mosaic_battery_platform_get_info(const char *section,
    char *output, size_t output_size, void *user_ctx);

#ifdef __cplusplus
}
#endif
