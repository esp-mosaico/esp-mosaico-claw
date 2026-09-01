/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_capability.h"
#include "mosaic_capability_adapters.h"

static mosaic_system_capability_ops_t s_ops;

esp_err_t mosaic_system_capabilities_init(
    const mosaic_system_capability_ops_t *ops)
{
    if (ops == NULL || ops->get_battery == NULL || ops->get_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ops = *ops;
    esp_err_t err = mosaic_capability_battery_register(&s_ops);
    if (err == ESP_OK) err = mosaic_capability_time_register();
    if (err == ESP_OK) err = mosaic_capability_status_register(&s_ops);
    return err;
}

void mosaic_system_capabilities_publish_battery(
    const mosaic_cap_battery_t *battery)
{
    if (battery == NULL) {
        return;
    }
    mosaic_capability_publish("system.battery", battery, sizeof(*battery));
}

void mosaic_system_capabilities_publish_status(
    const mosaic_cap_status_t *status)
{
    if (status == NULL) {
        return;
    }
    mosaic_capability_publish("system.status", status, sizeof(*status));
}
