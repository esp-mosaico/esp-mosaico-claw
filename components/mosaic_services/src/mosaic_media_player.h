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

/** Registers media.player. The decoder is created on first read or invoke. */
esp_err_t mosaic_media_player_init(void);

/** Releases the decoder and unregisters media.player. Safe to call twice. */
void mosaic_media_player_deinit(void);

#ifdef __cplusplus
}
#endif
