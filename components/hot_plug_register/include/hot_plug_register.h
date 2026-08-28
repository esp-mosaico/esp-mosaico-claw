/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOT_PLUG_SUBBOARD_CAMERA_NAME "CameraBoard"

typedef void (*hot_plug_insert_notice_callback_t)(
    const char *subboard_name, char slot, bool present, void *user_ctx);

/**
 * @brief Start the Mosaico subboard hot-plug registry
 *
 * The registry scans both subboard EEPROM addresses in a background task.
 * Repeated calls are idempotent.
 */
esp_err_t hot_plug_register_init(void);

/**
 * @brief Stop discovery and release every registered subboard
 *
 * The call waits for handles borrowed with hot_plug_register_get_handle() to
 * be returned before invoking each subboard's removal callback.
 */
esp_err_t hot_plug_register_deinit(void);

/**
 * @brief Borrow a registered subboard handle by its EEPROM board name
 *
 * CameraBoard insertion already registers its V4L2 device; callers that need
 * the native Mosaico pipeline must call mosaico_camera_open() on the borrowed
 * handle. Pair every success with hot_plug_register_put_handle().
 * ESP_ERR_NOT_FOUND means that the name is not present in the static registry;
 * ESP_ERR_INVALID_STATE means that the registered board is not currently
 * available.
 */
esp_err_t hot_plug_register_get_handle(const char *subboard_name,
                                       void **out_handle);

/**
 * @brief Return a handle borrowed with hot_plug_register_get_handle()
 */
esp_err_t hot_plug_register_put_handle(const char *subboard_name,
                                       void *handle);

/**
 * @brief True when the named subboard is inserted, even if the camera is closed
 */
esp_err_t hot_plug_register_is_present(const char *subboard_name,
                                       bool *out_present);

/**
 * @brief Close CameraBoard capture and free mmap buffers without unregistering
 *
 * The V4L2 path and insert presence stay available until physical removal.
 */
esp_err_t hot_plug_register_release_device(const char *subboard_name);

/**
 * @brief Register the UI callback invoked when a subboard is inserted or removed
 *
 * slot is 'L' or 'R'. present is true after a successful insert callback and
 * false after the board is physically removed. Passing NULL unregisters the
 * callback. If an insertion occurred before the callback was registered, the
 * pending notice is delivered immediately.
 */
void hot_plug_register_set_insert_notice_callback(
    hot_plug_insert_notice_callback_t callback, void *user_ctx);

#ifdef __cplusplus
}
#endif
