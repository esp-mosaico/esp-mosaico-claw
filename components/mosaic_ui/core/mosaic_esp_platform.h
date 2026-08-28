/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_gsp_esp_lcd.h"
#include "mosaic_runtime.h"

typedef struct mosaic_esp_platform_t* mosaic_esp_platform_handle_t;

typedef bool (*mosaic_esp_platform_post_event_cb_t)(
    void* user_ctx, uint32_t generation, const esp_gsp_event_t* event);
typedef bool (*mosaic_esp_platform_post_pointer_cb_t)(void* user_ctx,
    uint32_t generation, int32_t x, int32_t y, bool pressed);
typedef void (*mosaic_esp_platform_request_app_cb_t)(
    void* user_ctx, const mosaic_app_descriptor_t* app);

typedef struct {
    struct esp_display_presenter* presenter;
    esp_display_present_render_alignment_t render_alignment;
    esp_lcd_touch_handle_t touch;
    mosaic_esp_platform_post_event_cb_t post_event;
    void* post_event_ctx;
    mosaic_esp_platform_post_pointer_cb_t post_pointer;
    void* post_pointer_ctx;
    mosaic_esp_platform_request_app_cb_t request_app;
    void* request_app_ctx;
} mosaic_esp_platform_config_t;

esp_err_t mosaic_esp_platform_create(const mosaic_esp_platform_config_t* config,
    mosaic_esp_platform_handle_t* ret_platform);
void mosaic_esp_platform_delete(mosaic_esp_platform_handle_t platform);

const mosaic_platform_ops_t* mosaic_esp_platform_ops(void);

/** Delivers one render-task event on the Mosaic runtime owner task. */
bool mosaic_esp_platform_deliver_event(mosaic_esp_platform_handle_t platform,
    uint32_t generation, const esp_gsp_event_t* event);
/** Delivers one render-task pointer sample on the runtime owner task. */
bool mosaic_esp_platform_deliver_pointer(
    mosaic_esp_platform_handle_t platform, mosaic_runtime_handle_t runtime,
    uint32_t generation, int32_t x, int32_t y, bool pressed);

esp_gsp_handle_t mosaic_esp_platform_ui(mosaic_esp_platform_handle_t platform);
esp_err_t mosaic_esp_platform_quiesce(
    mosaic_esp_platform_handle_t platform, uint32_t timeout_ms);
esp_err_t mosaic_esp_platform_activate(mosaic_esp_platform_handle_t platform,
    struct esp_display_presenter* presenter);
/** Stop the active GSP render task before Display Off. Safe if already paused. */
esp_err_t mosaic_esp_platform_pause_screen(
    mosaic_esp_platform_handle_t platform, uint32_t timeout_ms);
/** Resume the GSP render task after Display On. */
esp_err_t mosaic_esp_platform_resume_screen(
    mosaic_esp_platform_handle_t platform);
