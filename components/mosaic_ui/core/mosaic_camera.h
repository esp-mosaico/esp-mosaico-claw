/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gsp.h"

/** Scene binds the preview driver writes to.
 *
 * The camera service owns the sensor, the JPEG encoder and the preview
 * pipeline, but not the scene. The App that hosts the preview passes its own
 * generated bind keys so this service never includes App scene headers.
 */
typedef struct {
    uint16_t canvas;
    uint16_t missing_hint_visible;
} mosaic_camera_binds_t;

esp_err_t mosaic_camera_start(
    esp_gsp_handle_t ui, const mosaic_camera_binds_t *binds);
esp_err_t mosaic_camera_capture_photo(const char *path, bool use_flash);
esp_err_t mosaic_camera_set_flash_enabled(bool enabled);
esp_err_t mosaic_camera_toggle_flip(bool *out_enabled);
void mosaic_camera_tick(esp_gsp_handle_t ui);
void mosaic_camera_stop(esp_gsp_handle_t ui);
