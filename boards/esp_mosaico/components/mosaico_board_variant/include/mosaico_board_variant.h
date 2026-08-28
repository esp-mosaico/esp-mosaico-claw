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

typedef enum {
    MOSAICO_BOARD_VARIANT_V1_0 = 0,
    MOSAICO_BOARD_VARIANT_V1_1,
} mosaico_board_variant_t;

esp_err_t mosaico_board_variant_prepare(void);
mosaico_board_variant_t mosaico_board_variant_get(void);

#ifdef __cplusplus
}
#endif
