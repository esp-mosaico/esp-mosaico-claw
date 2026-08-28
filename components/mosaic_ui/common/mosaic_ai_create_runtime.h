/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ai_create_controller.h"
#include "asr_service.h"
#include "esp_err.h"

esp_err_t mosaic_ai_create_runtime_set_asr(asr_service_handle_t asr);
esp_err_t mosaic_ai_create_runtime_set_voice_status(
    ai_create_voice_status_t status);
esp_err_t mosaic_ai_create_runtime_init(void);
/* Advances injected Host services. Firmware services are callback-driven, so
 * the firmware implementation only validates initialization. */
esp_err_t mosaic_ai_create_runtime_step(int64_t now_us);
ai_create_controller_handle_t mosaic_ai_create_runtime_controller(void);
/* Session storage is serviced outside Mosaic's loader task. */
esp_err_t mosaic_ai_create_runtime_refresh_sessions(void);
esp_err_t mosaic_ai_create_runtime_get_sessions(
    claw_session_mgr_alias_map_t *out_sessions, uint32_t *out_revision);
esp_err_t mosaic_ai_create_runtime_get_sessions_revision(
    uint32_t *out_revision);
esp_err_t mosaic_ai_create_runtime_select_session(size_t index);
esp_err_t mosaic_ai_create_runtime_confirm_session_delete(void);
/* Controller notifications raised by the current loader event are rendered
 * before that event returns and therefore do not need a second queued model
 * invalidation. Notifications from other tasks remain asynchronous. */
void mosaic_ai_create_runtime_begin_ui_update(void);
void mosaic_ai_create_runtime_end_ui_update(void);
