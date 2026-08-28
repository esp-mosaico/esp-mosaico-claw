/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASR_PROVIDER_SAMPLE_RATE 16000
#define ASR_PROVIDER_CHANNELS    1
#define ASR_PROVIDER_BITS        16

typedef void *asr_provider_handle_t;

typedef enum {
    ASR_PROVIDER_RESULT_PARTIAL = 0,
    ASR_PROVIDER_RESULT_FINAL,
} asr_provider_result_type_t;

typedef void (*asr_provider_result_cb_t)(asr_provider_handle_t handle, asr_provider_result_type_t type, const char *text, void *user_ctx);

typedef struct {
    const char *api_key;
    const char *workspace_id;
    const char *endpoint;
    const char *model;
    const char *language_hint;
    bool trial_auth;
    uint32_t connect_timeout_ms;
    uint32_t send_timeout_ms;
    asr_provider_result_cb_t result_cb;
    void *result_user_ctx;
} asr_provider_config_t;

typedef struct {
    esp_err_t (*create)(const asr_provider_config_t *config, asr_provider_handle_t *ret_handle);
    esp_err_t (*connect)(asr_provider_handle_t handle);
    esp_err_t (*start_stream)(asr_provider_handle_t handle);
    esp_err_t (*send_audio)(asr_provider_handle_t handle, const uint8_t *data, size_t len);
    bool (*has_final_result)(asr_provider_handle_t handle);
    esp_err_t (*finish_stream)(asr_provider_handle_t handle);
    esp_err_t (*get_final_text)(asr_provider_handle_t handle, char *text, size_t text_size);
    void (*disconnect)(asr_provider_handle_t handle);
    void (*delete)(asr_provider_handle_t handle);
} asr_provider_ops_t;

const asr_provider_ops_t *asr_provider_qwen_ops(void);
const asr_provider_ops_t *asr_provider_trial_ops(void);

#ifdef __cplusplus
}
#endif
