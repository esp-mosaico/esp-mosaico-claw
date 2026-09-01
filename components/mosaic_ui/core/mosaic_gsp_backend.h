/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_gsp_esp_lcd.h"
#include "mosaic_runtime.h"

/* Device GSP/LCD implementation of mosaic_platform_ops_t.
 * This is the runtime display backend, not a product platform binding.
 */

typedef struct mosaic_gsp_backend_t* mosaic_gsp_backend_handle_t;

typedef bool (*mosaic_gsp_backend_post_event_cb_t)(
    void* user_ctx, uint32_t generation, const esp_gsp_event_t* event);
typedef bool (*mosaic_gsp_backend_post_pointer_cb_t)(void* user_ctx,
    uint32_t generation, int32_t x, int32_t y, bool pressed);
typedef void (*mosaic_gsp_backend_request_app_cb_t)(
    void* user_ctx, const mosaic_app_descriptor_t* app);

typedef struct {
    struct esp_display_presenter* presenter;
    esp_display_present_render_alignment_t render_alignment;
    esp_lcd_touch_handle_t touch;
    mosaic_gsp_backend_post_event_cb_t post_event;
    void* post_event_ctx;
    mosaic_gsp_backend_post_pointer_cb_t post_pointer;
    void* post_pointer_ctx;
    mosaic_gsp_backend_request_app_cb_t request_app;
    void* request_app_ctx;
} mosaic_gsp_backend_config_t;

esp_err_t mosaic_gsp_backend_create(const mosaic_gsp_backend_config_t* config,
    mosaic_gsp_backend_handle_t* ret_backend);
void mosaic_gsp_backend_delete(mosaic_gsp_backend_handle_t backend);

const mosaic_platform_ops_t* mosaic_gsp_backend_ops(void);

/** Delivers one render-task event on the Mosaic runtime owner task. */
bool mosaic_gsp_backend_deliver_event(mosaic_gsp_backend_handle_t backend,
    uint32_t generation, const esp_gsp_event_t* event);
/** Delivers one render-task pointer sample on the runtime owner task. */
bool mosaic_gsp_backend_deliver_pointer(
    mosaic_gsp_backend_handle_t backend, mosaic_runtime_handle_t runtime,
    uint32_t generation, int32_t x, int32_t y, bool pressed);

esp_gsp_handle_t mosaic_gsp_backend_ui(mosaic_gsp_backend_handle_t backend);
esp_err_t mosaic_gsp_backend_quiesce(
    mosaic_gsp_backend_handle_t backend, uint32_t timeout_ms);
esp_err_t mosaic_gsp_backend_activate(mosaic_gsp_backend_handle_t backend,
    struct esp_display_presenter* presenter);
/** Stop the active GSP render task before Display Off. Safe if already paused. */
esp_err_t mosaic_gsp_backend_pause_screen(
    mosaic_gsp_backend_handle_t backend, uint32_t timeout_ms);
/** Resume the GSP render task after Display On. */
esp_err_t mosaic_gsp_backend_resume_screen(
    mosaic_gsp_backend_handle_t backend);
