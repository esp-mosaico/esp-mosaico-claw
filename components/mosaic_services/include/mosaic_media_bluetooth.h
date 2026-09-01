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
 * Brings up the A2DP sink and registers the media.bluetooth capability.
 *
 * The provider claims its own mixer track, so callers never handle the audio
 * pipeline. The Bluetooth stack stays powered only while the capability is
 * registered, so this pairs with mosaic_media_bluetooth_stop() when the App
 * closes. Returns ESP_ERR_NOT_SUPPORTED without a Bluetooth radio, and
 * ESP_ERR_INVALID_STATE when the mixer is not up yet.
 */
esp_err_t mosaic_media_bluetooth_start(void);

/** Unregisters the capability and releases the radio. Safe to call twice. */
void mosaic_media_bluetooth_stop(void);

#ifdef __cplusplus
}
#endif
