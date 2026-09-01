/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registers the net.weather capability over the weather service.
 *
 * Call after weather_service_init(). Snapshots reach subscribers through the
 * capability publish stream from then on.
 */
esp_err_t mosaic_net_weather_init(void);

#ifdef __cplusplus
}
#endif
