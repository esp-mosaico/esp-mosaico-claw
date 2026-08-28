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
    MOSAIC_SYSTEM_BOOT_SETUP = 0,
    MOSAIC_SYSTEM_BOOT_WELCOME,
    MOSAIC_SYSTEM_BOOT_HOME,
} mosaic_system_boot_stage_t;

typedef struct {
    esp_err_t (*get_boot_stage)(void *user_ctx,
                                mosaic_system_boot_stage_t *ret_stage);
    esp_err_t (*set_boot_stage)(void *user_ctx,
                                mosaic_system_boot_stage_t stage);
    void *user_ctx;
} mosaic_system_ops_t;

/** Configure persistent system lifecycle before mosaic_ui_start(). */
esp_err_t mosaic_system_configure(const mosaic_system_ops_t *ops);

#ifdef __cplusplus
}
#endif
