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

/** Registers media.bluetooth. The A2DP sink starts on first use. */
esp_err_t mosaic_media_bluetooth_init(void);

/** Powers the sink down and unregisters media.bluetooth. Safe to call twice. */
void mosaic_media_bluetooth_deinit(void);

#ifdef __cplusplus
}
#endif
