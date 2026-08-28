/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_capture.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASR_SERVICE_DEFAULT_ENDPOINT      "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
#define ASR_SERVICE_DEFAULT_MODEL         "fun-asr-realtime"
#define ASR_SERVICE_DEFAULT_LANGUAGE_HINT "zh"

typedef struct asr_service_t *asr_service_handle_t;

typedef enum {
    ASR_SERVICE_PROVIDER_QWEN = 0,
    ASR_SERVICE_PROVIDER_TRIAL,
} asr_service_provider_t;

typedef enum {
    ASR_SERVICE_EVENT_STARTED = 0,
    ASR_SERVICE_EVENT_PARTIAL,
    ASR_SERVICE_EVENT_FINAL,
    ASR_SERVICE_EVENT_ERROR,
    ASR_SERVICE_EVENT_STOPPED,
} asr_service_event_t;

typedef struct {
    asr_service_event_t event;
    esp_err_t error;
    const char *text;
} asr_service_event_data_t;

typedef void (*asr_service_event_cb_t)(asr_service_handle_t handle, const asr_service_event_data_t *event, void *user_ctx);

typedef struct {
    audio_capture_handle_t capture;
    asr_service_provider_t provider;
    const char *api_key;
    const char *workspace_id;
    const char *endpoint;
    const char *model;
    const char *language_hint;
    uint32_t connect_timeout_ms;
    uint32_t send_timeout_ms;
    /* Lossless PCM queue used while connecting and during network stalls.
     * 0 uses the default. Queue exhaustion stops ASR with an explicit error
     * instead of silently overwriting audio. */
    uint32_t prebuffer_ms;
} asr_service_config_t;

esp_err_t asr_service_create(const asr_service_config_t *config, asr_service_handle_t *ret_handle);

/* Callback text is valid only during the callback. Do not call start/stop/delete from the callback. */
esp_err_t asr_service_register_cb(asr_service_handle_t handle, asr_service_event_cb_t cb, void *user_ctx);

esp_err_t asr_service_start(asr_service_handle_t handle);

/* Closes local capture at the next PCM frame without waiting for provider I/O.
 * A normal stop drains the captured tail and queued PCM before finish-task.
 * With discard=true, an in-progress provider startup is cancelled after its
 * current blocking connect call returns. */
esp_err_t asr_service_end_capture(asr_service_handle_t handle, bool discard);

/* Stops streaming and optionally copies the final text into `text`.
 * Pass NULL/0 when only stopping is needed. */
esp_err_t asr_service_stop(asr_service_handle_t handle, char *text, size_t text_size);

/* Copies the latest final text cached by the service.
 * Returns ESP_ERR_NOT_FOUND when no final text is available.
 * Returns ESP_ERR_INVALID_SIZE when the output was truncated. */
esp_err_t asr_service_get_result(asr_service_handle_t handle, char *text, size_t text_size);

/* Returns the current microphone loudness envelope in [0, 255]. The value is
 * derived from the existing 20 ms ASR PCM frames and can be polled without
 * taking a service mutex or advancing ASR state. */
esp_err_t asr_service_get_loudness(asr_service_handle_t handle,
                                   uint8_t *out_level);

void asr_service_delete(asr_service_handle_t handle);

#ifdef __cplusplus
}
#endif
