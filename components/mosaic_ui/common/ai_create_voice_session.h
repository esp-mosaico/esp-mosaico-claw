/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ai_create_voice_port.h"
#include "asr_service.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ai_create_voice_session_t *ai_create_voice_session_handle_t;

typedef struct {
    asr_service_handle_t asr;
    size_t result_size;
} ai_create_voice_session_config_t;

esp_err_t ai_create_voice_session_create(
    const ai_create_voice_session_config_t *config,
    ai_create_voice_session_handle_t *ret_session);
void ai_create_voice_session_delete(ai_create_voice_session_handle_t session);
ai_create_voice_port_handle_t ai_create_voice_session_get_port(
    ai_create_voice_session_handle_t session);

#ifdef __cplusplus
}
#endif
