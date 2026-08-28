/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_gsp.h"

typedef void (*mosaic_app_shell_exit_fn)(void *user_ctx);

typedef enum {
    MOSAIC_SYSTEM_NOTICE_BATTERY_LOW = 0,
    MOSAIC_SYSTEM_NOTICE_BATTERY_CRITICAL,
} mosaic_system_notice_t;

void mosaic_app_shell_set_exit_handler(
    mosaic_app_shell_exit_fn fn, void *user_ctx);
void mosaic_app_shell_attach(esp_gsp_handle_t ui, uint32_t root_stack_key,
                             const char *title, bool root_header_enabled);
/** Synchronize root-only chrome immediately after in-app navigation. */
void mosaic_app_shell_sync(esp_gsp_handle_t ui);
/** Re-register input/chrome after the platform pointer observer is live. */
void mosaic_app_shell_rearm(esp_gsp_handle_t ui);
/** Transfer top-left chrome ownership before an asynchronous stack command. */
void mosaic_app_shell_set_root_visible(esp_gsp_handle_t ui, bool visible);
/** Show or hide the bottom indicator and its upward-exit input interceptor. */
void mosaic_app_shell_set_bottom_enabled(esp_gsp_handle_t ui, bool enabled);
/** Show a system notice above the active App. Zero duration keeps it visible. */
esp_gsp_err_t mosaic_app_shell_show_system_notice(
    esp_gsp_handle_t ui, mosaic_system_notice_t notice, uint32_t duration_ms);
void mosaic_app_shell_detach(esp_gsp_handle_t ui);
