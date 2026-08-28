/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_gsp.h"

typedef esp_err_t (*gsp_analog_clock_set_deg_fn)(esp_gsp_handle_t ui,
                                                 int32_t deg);

typedef struct {
    gsp_analog_clock_set_deg_fn set_hour_deg;
    gsp_analog_clock_set_deg_fn set_minute_deg;
    gsp_analog_clock_set_deg_fn set_second_deg;
} gsp_analog_clock_ops_t;

static inline int32_t gsp_analog_clock_hour_deg(int hour, int minute)
{
    int normalized_hour = hour % 12;
    if (normalized_hour < 0) {
        normalized_hour += 12;
    }
    return normalized_hour * 30 + minute / 2;
}

static inline int32_t gsp_analog_clock_minute_deg(int minute)
{
    return (minute % 60) * 6;
}

static inline int32_t gsp_analog_clock_second_deg(int second)
{
    return (second % 60) * 6;
}

static inline esp_err_t gsp_analog_clock_set_time(
    esp_gsp_handle_t ui, const gsp_analog_clock_ops_t *ops,
    int hour, int minute, int second)
{
    if (ui == NULL || ops == NULL || ops->set_hour_deg == NULL ||
            ops->set_minute_deg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ops->set_hour_deg(
        ui, gsp_analog_clock_hour_deg(hour, minute));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ops->set_minute_deg(ui, gsp_analog_clock_minute_deg(minute));
    if (ret != ESP_OK) {
        return ret;
    }
    if (ops->set_second_deg != NULL) {
        ret = ops->set_second_deg(ui, gsp_analog_clock_second_deg(second));
    }
    return ret;
}
