/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_gsp.h"

esp_err_t mosaic_camera_start(esp_gsp_handle_t ui);
esp_err_t mosaic_camera_capture_photo(const char *path, bool use_flash);
esp_err_t mosaic_camera_set_flash_enabled(bool enabled);
esp_err_t mosaic_camera_toggle_flip(bool *out_enabled);
void mosaic_camera_tick(esp_gsp_handle_t ui);
void mosaic_camera_stop(esp_gsp_handle_t ui);
