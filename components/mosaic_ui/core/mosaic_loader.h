/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "esp_gsp_esp_lcd.h"
#include "mosaic_app_catalog.h"
#include "mosaic_app_shell.h"

typedef void (*mosaic_loader_event_cb_t)(esp_gsp_handle_t ui,
    const mosaic_app_descriptor_t* app, const esp_gsp_event_t* event,
    void* user_ctx);
typedef void (*mosaic_loader_deferred_cb_t)(void* user_ctx);

typedef struct {
    struct esp_display_presenter* presenter;
    esp_display_present_render_alignment_t render_alignment;
    esp_lcd_touch_handle_t touch;
    uint32_t producer_generation;
    mosaic_loader_event_cb_t on_event;
    void* on_event_ctx;
} mosaic_loader_config_t;

esp_err_t mosaic_loader_init(const mosaic_loader_config_t* config);
esp_err_t mosaic_loader_start_hub(void);
esp_err_t mosaic_loader_request(const mosaic_app_descriptor_t* app);
/** Thread-safe physical/UI Back request. Child StackViews pop before App exit. */
esp_err_t mosaic_loader_request_back(void);
esp_err_t mosaic_loader_invalidate_app(uint16_t app_id, uint32_t revision);
/** Thread-safe request for a transient notice above the active App. */
esp_err_t mosaic_loader_show_system_notice(
    mosaic_system_notice_t notice, uint32_t duration_ms);
/** Run work on the loader task after the current runtime lock is released. */
esp_err_t mosaic_loader_defer(
    mosaic_loader_deferred_cb_t callback, void* user_ctx);
bool mosaic_loader_poll(void);
const mosaic_app_descriptor_t* mosaic_loader_app(void);
esp_gsp_handle_t mosaic_loader_ui(void);
esp_err_t mosaic_loader_quiesce(uint32_t timeout_ms);
esp_err_t mosaic_loader_activate(
    struct esp_display_presenter* presenter, uint32_t producer_generation);
/** Render and flush Hub Lock Screen, then pause that exact frame. */
esp_err_t mosaic_loader_lock_and_pause_hub(uint32_t timeout_ms);
esp_err_t mosaic_loader_resume_screen(void);
uint16_t mosaic_loader_hub_scene(void);
