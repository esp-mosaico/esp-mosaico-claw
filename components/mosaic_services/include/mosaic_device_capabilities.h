/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "mosaic_capability_contracts.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Haptics owner hook.
 *
 * The vibration motor is driven by the shell, which also owns the quick
 * setting that gates it. The device domain only forwards the request so
 * Apps have one way in.
 */
typedef esp_err_t (*mosaic_haptic_pulse_fn)(
    void *user_ctx, uint32_t duration_ms);

/** Board IMU sample hook. Unset leaves sensor.imu unbound. */
typedef esp_err_t (*mosaic_imu_read_fn)(
    void *user_ctx, mosaic_cap_orientation_t *out);

/** Publish the device-facing capability domains.
 *
 * Registers system.time / display / audio / haptic / power / update /
 * lifecycle, sensor.imu when an IMU hook is bound, net.wifi /
 * net.wifi.scan / net.provisioning, config.agent, media.player and
 * media.bluetooth. Call after mosaic_settings_configure(). Bind IMU with
 * mosaic_device_capabilities_set_imu() first when the board has a sensor.
 */
esp_err_t mosaic_device_capabilities_init(void);

/** Unregister providers and drop model subscriptions. Safe to call twice. */
void mosaic_device_capabilities_deinit(void);

/** Hand the haptics implementation to the system.haptic pulse command. */
void mosaic_device_capabilities_set_haptic(
    mosaic_haptic_pulse_fn pulse, void *user_ctx);

/** Hand the IMU implementation to sensor.imu. */
void mosaic_device_capabilities_set_imu(
    mosaic_imu_read_fn read, void *user_ctx);

#ifdef __cplusplus
}
#endif
