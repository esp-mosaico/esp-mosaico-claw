/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_gsp.h"

typedef enum {
    MOSAIC_DEMO_IMU,
    MOSAIC_DEMO_ENV,
    MOSAIC_DEMO_TOF,
} mosaic_demo_kind_t;

void mosaic_demo_tick(esp_gsp_handle_t ui, mosaic_demo_kind_t kind);
