/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ai_create_voice_port_t *ai_create_voice_port_handle_t;

typedef enum {
    AI_CREATE_VOICE_READINESS_DISABLED = 0,
    AI_CREATE_VOICE_READINESS_INITIALIZING,
    AI_CREATE_VOICE_READINESS_READY,
    AI_CREATE_VOICE_READINESS_OFFLINE,
    AI_CREATE_VOICE_READINESS_AUTH_FAILED,
    AI_CREATE_VOICE_READINESS_AUDIO_ERROR,
    AI_CREATE_VOICE_READINESS_BUSY,
    AI_CREATE_VOICE_READINESS_PROVIDER_ERROR,
} ai_create_voice_readiness_t;

typedef struct {
    ai_create_voice_readiness_t readiness;
    esp_err_t error;
    bool retryable;
} ai_create_voice_status_t;

typedef enum {
    AI_CREATE_VOICE_EVENT_STARTED = 0,
    AI_CREATE_VOICE_EVENT_TRANSCRIPT,
    AI_CREATE_VOICE_EVENT_ERROR,
    AI_CREATE_VOICE_EVENT_COMPLETED,
} ai_create_voice_event_type_t;

typedef struct {
    ai_create_voice_event_type_t type;
    uint32_t operation_id;
    ai_create_voice_status_t status;
    bool send;
    /* Text is valid only during the callback. */
    const char *text;
} ai_create_voice_event_t;

typedef void (*ai_create_voice_event_cb_t)(
    ai_create_voice_port_handle_t port,
    const ai_create_voice_event_t *event, void *user_ctx);

typedef struct {
    esp_err_t (*register_cb)(void *ctx, ai_create_voice_event_cb_t callback,
                             void *callback_ctx);
    esp_err_t (*get_status)(void *ctx, ai_create_voice_status_t *out_status);
    esp_err_t (*begin)(void *ctx, uint32_t operation_id);
    esp_err_t (*finish)(void *ctx, uint32_t operation_id, bool send);
    esp_err_t (*cancel)(void *ctx, uint32_t operation_id);
    esp_err_t (*get_loudness)(void *ctx, uint8_t *out_level);
} ai_create_voice_port_ops_t;

typedef struct {
    const ai_create_voice_port_ops_t *ops;
    void *ctx;
} ai_create_voice_port_config_t;

esp_err_t ai_create_voice_port_create(
    const ai_create_voice_port_config_t *config,
    ai_create_voice_port_handle_t *ret_port);
void ai_create_voice_port_delete(ai_create_voice_port_handle_t port);

esp_err_t ai_create_voice_port_register_cb(ai_create_voice_port_handle_t port,
    ai_create_voice_event_cb_t callback, void *callback_ctx);
esp_err_t ai_create_voice_port_get_status(ai_create_voice_port_handle_t port,
    ai_create_voice_status_t *out_status);
esp_err_t ai_create_voice_port_begin(ai_create_voice_port_handle_t port,
    uint32_t operation_id);
esp_err_t ai_create_voice_port_finish(ai_create_voice_port_handle_t port,
    uint32_t operation_id, bool send);
esp_err_t ai_create_voice_port_cancel(ai_create_voice_port_handle_t port,
    uint32_t operation_id);
esp_err_t ai_create_voice_port_get_loudness(ai_create_voice_port_handle_t port,
    uint8_t *out_level);

ai_create_voice_status_t ai_create_voice_status_disabled(void);
ai_create_voice_status_t ai_create_voice_status_ready(void);

#ifdef __cplusplus
}
#endif
