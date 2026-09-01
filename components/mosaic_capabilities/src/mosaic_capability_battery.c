/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_capability_adapters.h"

#include "mosaic_capability.h"

static esp_err_t read_battery(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    const mosaic_system_capability_ops_t *ops = user_ctx;
    if (ops == NULL || out_payload == NULL ||
            payload_size != sizeof(mosaic_cap_battery_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ops->get_battery(ops->user_ctx, out_payload);
}

static const mosaic_capability_ops_t s_battery_ops = {
    .read = read_battery,
};

esp_err_t mosaic_capability_battery_register(
    const mosaic_system_capability_ops_t *ops)
{
    return mosaic_capability_register(&(mosaic_capability_provider_t) {
        .name = "system.battery",
        .ops = &s_battery_ops,
        .user_ctx = (void *)ops,
    });
}
