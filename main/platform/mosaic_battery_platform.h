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

/** Return the last valid sample, or an unavailable snapshot before first read. */
esp_err_t mosaic_battery_platform_get(
    mosaic_settings_battery_t *ret_battery);

#ifdef __cplusplus
}
#endif
