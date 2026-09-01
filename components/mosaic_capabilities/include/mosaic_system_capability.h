/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "mosaic_capability_contracts.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Product hooks behind the built-in system capabilities.
 *
 * The platform owns the actual gauge and network state; this layer only
 * republishes it as declared capabilities.
 */
typedef struct {
    esp_err_t (*get_battery)(void *user_ctx, mosaic_cap_battery_t *out_battery);
    esp_err_t (*get_status)(void *user_ctx, mosaic_cap_status_t *out_status);
    void *user_ctx;
} mosaic_system_capability_ops_t;

/** Register the product battery, wall-clock and system-status providers. */
esp_err_t mosaic_system_capabilities_init(
    const mosaic_system_capability_ops_t *ops);

/** Publish a fresh battery sample to system.battery subscribers. */
void mosaic_system_capabilities_publish_battery(
    const mosaic_cap_battery_t *battery);

/** Publish a fresh aggregate status sample to system.status subscribers. */
void mosaic_system_capabilities_publish_status(
    const mosaic_cap_status_t *status);

#ifdef __cplusplus
}
#endif
