/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

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

/** Publish the device-facing capability domains.
 *
 * Registers system.display / audio / haptic / power / update / lifecycle,
 * net.wifi / net.wifi.scan / net.provisioning and config.agent on top of
 * the configured device model, and bridges the model's battery and Wi-Fi
 * notifications onto the capability publish stream.
 *
 * Call after mosaic_settings_configure().
 */
esp_err_t mosaic_device_capabilities_init(void);

/** Hand the haptics implementation to the system.haptic pulse command. */
void mosaic_device_capabilities_set_haptic(
    mosaic_haptic_pulse_fn pulse, void *user_ctx);

#ifdef __cplusplus
}
#endif
