/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "app_config.h"
#include "app_settings_service.h"
#include "esp_err.h"
#include "network_provisioning_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*mosaic_device_ops_save_config_cb_t)(
    const app_config_t *config);

typedef struct {
    app_settings_service_handle_t settings;
    network_provisioning_service_handle_t network_provisioning;
    mosaic_device_ops_save_config_cb_t save_config;
} mosaic_device_ops_config_t;

/** Bind the device model to Edge Agent services (settings, Wi-Fi, battery). */
esp_err_t mosaic_device_ops_init(
    const mosaic_device_ops_config_t *config);

/** Start battery sampling after Mosaic UI is ready to surface power notices. */
esp_err_t mosaic_device_ops_start_battery_monitor(void);

/** Notify the device model after network provisioning state changes. */
void mosaic_device_ops_notify_network_changed(void);

#ifdef __cplusplus
}
#endif
