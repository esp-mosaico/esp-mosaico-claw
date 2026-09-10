/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mosaico_module_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mosaico_camera_availability_callback_t)(char slot, bool available, void *user_ctx);

esp_err_t mosaico_camera_service_init(void);
esp_err_t mosaico_camera_service_deinit(void);
esp_err_t mosaico_camera_service_get_default(mosaico_camera_handle_t *out_camera);
bool mosaico_camera_service_is_available(void);
void mosaico_camera_service_set_callback(mosaico_camera_availability_callback_t callback, void *user_ctx);

#ifdef __cplusplus
}
#endif
