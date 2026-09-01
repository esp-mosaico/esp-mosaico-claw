/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_imu.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "mosaic_capability.h"

static mosaic_imu_ops_t s_provider;
static bool s_configured;
static portMUX_TYPE s_provider_lock = portMUX_INITIALIZER_UNLOCKED;

static void provider_lock(void)
{
    portENTER_CRITICAL(&s_provider_lock);
}

static void provider_unlock(void)
{
    portEXIT_CRITICAL(&s_provider_lock);
}

static esp_err_t read_capability(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    if (user_ctx != &s_provider || out_payload == NULL ||
            payload_size != sizeof(mosaic_cap_orientation_t)) {
        return ESP_ERR_INVALID_STATE;
    }
    provider_lock();
    const bool configured = s_configured;
    const mosaic_imu_ops_t provider = s_provider;
    provider_unlock();
    if (!configured || provider.read == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    mosaic_imu_sample_t sample = {0};
    esp_err_t err = provider.read(&sample, provider.user_ctx);
    if (err != ESP_OK) {
        return err;
    }
    mosaic_cap_orientation_t *out = out_payload;
    out->pitch_deg = sample.pitch_deg;
    out->roll_deg = sample.roll_deg;
    out->yaw_deg = sample.yaw_deg;
    return ESP_OK;
}

static const mosaic_capability_ops_t s_imu_ops = {
    .read = read_capability,
};

esp_err_t mosaic_imu_configure(const mosaic_imu_ops_t *ops)
{
    if (ops != NULL && ops->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ops == NULL) {
        esp_err_t err = mosaic_capability_unregister("sensor.imu", &s_provider);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            provider_lock();
            memset(&s_provider, 0, sizeof(s_provider));
            s_configured = false;
            provider_unlock();
        }
        return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
    }

    provider_lock();
    const bool was_configured = s_configured;
    if (!was_configured) {
        s_provider = *ops;
        s_configured = true;
    }
    provider_unlock();
    esp_err_t err = mosaic_capability_register(&(mosaic_capability_provider_t) {
        .name = "sensor.imu",
        .ops = &s_imu_ops,
        .user_ctx = &s_provider,
    });
    if (err != ESP_OK && !was_configured) {
        provider_lock();
        memset(&s_provider, 0, sizeof(s_provider));
        s_configured = false;
        provider_unlock();
        return err;
    }
    if (err == ESP_OK && was_configured) {
        provider_lock();
        s_provider = *ops;
        provider_unlock();
    }
    return err;
}
