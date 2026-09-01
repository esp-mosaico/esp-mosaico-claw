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

typedef struct {
    float pitch_deg;
    float roll_deg;
    float yaw_deg;
} mosaic_imu_sample_t;

typedef esp_err_t (*mosaic_imu_read_cb_t)(mosaic_imu_sample_t *sample,
                                         void *user_ctx);

typedef struct {
    mosaic_imu_read_cb_t read;
    void *user_ctx;
} mosaic_imu_ops_t;

/** Configure the device IMU provider. A NULL provider unregisters it. */
esp_err_t mosaic_imu_configure(const mosaic_imu_ops_t *ops);

#ifdef __cplusplus
}
#endif
