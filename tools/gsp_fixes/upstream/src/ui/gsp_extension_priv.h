/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT */
#pragma once
#include "esp_gsp_extension.h"

bool gsp_extension_observed(esp_gsp_handle_t gsp, esp_gsp_extension_kind_t kind);
bool gsp_extension_canvas_observed(esp_gsp_handle_t gsp, esp_gsp_canvas_operation_t operation);
void gsp_extension_emit(esp_gsp_handle_t gsp, const esp_gsp_extension_event_t *event);
void gsp_extension_render_error(esp_gsp_handle_t gsp, gsp_err_t error);

/* Private default identities allow the unused notification path to avoid
 * constructing payloads. Keep definitions separate from dispatch/call sites. */
void gsp_default_hook_app_created(esp_gsp_handle_t gsp);
void gsp_default_hook_app_destroying(esp_gsp_handle_t gsp);
void gsp_default_hook_scene_changed(esp_gsp_handle_t gsp, uint16_t previous, uint16_t current);
void gsp_default_hook_action(esp_gsp_handle_t gsp, const esp_gsp_event_t *event);
void gsp_default_hook_canvas_result(esp_gsp_handle_t gsp, const esp_gsp_canvas_result_t *result);
void gsp_default_hook_render_error(esp_gsp_handle_t gsp, gsp_err_t error);
