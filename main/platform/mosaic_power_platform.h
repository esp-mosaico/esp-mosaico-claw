/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"

/** Pulse the board shutdown signal through its revision-aware open-drain API. */
esp_err_t mosaic_power_platform_request_shutdown(void);
