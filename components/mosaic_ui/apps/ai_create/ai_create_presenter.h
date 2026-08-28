/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "ai_create_controller.h"
#include "esp_gsp.h"

typedef struct ai_create_presenter_t *ai_create_presenter_handle_t;

#define AI_CREATE_SESSION_PAGE_SIZE 3U
#define AI_CREATE_SESSION_MAX_PAGES \
    ((CLAW_SESSION_MGR_MAX_SESSIONS + AI_CREATE_SESSION_PAGE_SIZE - 1U) / \
     AI_CREATE_SESSION_PAGE_SIZE)

esp_err_t ai_create_presenter_create(
    ai_create_presenter_handle_t *ret_presenter);
void ai_create_presenter_delete(ai_create_presenter_handle_t presenter);
void ai_create_presenter_invalidate(ai_create_presenter_handle_t presenter);

esp_err_t ai_create_presenter_render(
    ai_create_presenter_handle_t presenter,
    esp_gsp_handle_t ui,
    const ai_create_controller_snapshot_t *model,
    const claw_session_mgr_alias_map_t *sessions,
    size_t session_page,
    int64_t now_us,
    int64_t voice_started_us,
    uint8_t voice_loudness);
