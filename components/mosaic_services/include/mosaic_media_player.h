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
 * Creates the local track controller and registers the media.player
 * capability.
 *
 * The decoder holds a mixer track while registered, so this pairs with
 * mosaic_media_player_stop() when the App closes.
 */
esp_err_t mosaic_media_player_start(void);

/** Unregisters the capability and releases the decoder. Safe to call twice. */
void mosaic_media_player_stop(void);

#ifdef __cplusplus
}
#endif
