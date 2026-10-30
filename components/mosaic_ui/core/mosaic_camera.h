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
#include "camera_vision.h"

typedef struct {
    uint32_t sequence;
    bool pending;
    bool ready;
    esp_err_t result;
    /* Inactive sessions report INVALID_STATE unless a startup error is retained. */
    esp_err_t startup_result;
} mosaic_camera_capture_status_t;

/* Read the latest request result. ESP_OK from capture_photo only queues a
 * request; pending remains true through encoding and saving. */
esp_err_t mosaic_camera_get_capture_status(mosaic_camera_capture_status_t *status);

esp_err_t mosaic_camera_start(esp_gsp_handle_t ui);
esp_err_t mosaic_camera_capture_photo(const char *path, bool use_flash);
esp_err_t mosaic_camera_set_flash_enabled(bool enabled);
esp_err_t mosaic_camera_toggle_flip(bool *out_enabled);
esp_err_t mosaic_camera_set_recognition_mode(camera_vision_mode_t mode);
void mosaic_camera_tick(esp_gsp_handle_t ui);
/* On failure the session and borrowed pixels remain live; retry before
 * destroying the UI or starting another Camera session. */
esp_err_t mosaic_camera_stop(esp_gsp_handle_t ui);
