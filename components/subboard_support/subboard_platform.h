/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t subboard_platform_i2c_init(void);
esp_err_t subboard_platform_set_power(bool on);

#ifdef __cplusplus
}
#endif
