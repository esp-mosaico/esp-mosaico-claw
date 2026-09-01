/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_capabilities_platform.h"

#include "mosaic_settings.h"
#include "mosaic_system_capability.h"

static esp_err_t read_battery(
    void *user_ctx, mosaic_cap_battery_t *out_battery)
{
    (void)user_ctx;
    if (out_battery == NULL) return ESP_ERR_INVALID_ARG;

    mosaic_settings_battery_t battery = {0};
    esp_err_t err = mosaic_settings_get_battery(&battery);
    if (err != ESP_OK) return err;
    *out_battery = (mosaic_cap_battery_t) {
        .available = battery.available,
        .charging = battery.charging,
        .percent = battery.state_of_charge,
        .voltage_mv = battery.voltage_mv,
        .current_ma = battery.current_ma,
    };
    return ESP_OK;
}

static esp_err_t read_status(
    void *user_ctx, mosaic_cap_status_t *out_status)
{
    (void)user_ctx;
    if (out_status == NULL) return ESP_ERR_INVALID_ARG;

    mosaic_settings_battery_t battery = {0};
    mosaic_settings_network_t network = {0};
    const esp_err_t battery_err = mosaic_settings_get_battery(&battery);
    const esp_err_t network_err = mosaic_settings_get_wifi(&network);
    if (battery_err != ESP_OK && network_err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_status = (mosaic_cap_status_t) {
        .battery_available = battery_err == ESP_OK && battery.available,
        .charging = battery_err == ESP_OK && battery.charging,
        .network_enabled = network_err == ESP_OK && network.enabled,
        .network_connected = network_err == ESP_OK && network.connected,
        .network_rssi = network_err == ESP_OK ? network.rssi : 0,
    };
    return ESP_OK;
}

esp_err_t mosaic_capabilities_platform_init(void)
{
    return mosaic_system_capabilities_init(
        &(mosaic_system_capability_ops_t) {
            .get_battery = read_battery,
            .get_status = read_status,
        });
}
