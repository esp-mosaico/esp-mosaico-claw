/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_IM_AI_CREATE_CHANNEL "ai_create"
#define CAP_IM_AI_CREATE_PROTOCOL_VERSION 4U
#define CAP_IM_AI_CREATE_TEXT_MAX_BYTES 4096U

typedef enum {
    CAP_IM_AI_CREATE_MODE_CREATE = 0,
    CAP_IM_AI_CREATE_MODE_SKILL,
    CAP_IM_AI_CREATE_MODE_MEMORY,
    CAP_IM_AI_CREATE_MODE_PLAN,
    CAP_IM_AI_CREATE_MODE_QUICK,
    CAP_IM_AI_CREATE_MODE_SCHEDULE,
    CAP_IM_AI_CREATE_MODE_COUNT,
} cap_im_ai_create_mode_t;

typedef enum {
    CAP_IM_AI_CREATE_INPUT_TEXT = 0,
    CAP_IM_AI_CREATE_INPUT_VOICE,
} cap_im_ai_create_input_t;

typedef struct {
    uint16_t protocol_version;
    uint32_t request_id;
    /** Stable transport conversation id. Required for new callers. */
    const char *chat_id;
    /** Optional user-facing session alias resolved by the session manager. */
    const char *session_alias;
    cap_im_ai_create_mode_t mode;
    cap_im_ai_create_input_t input;
    /** True only for the first user message in this session. */
    bool inject_mode_instruction;
    const char *text;
} cap_im_ai_create_request_t;

typedef enum {
    CAP_IM_AI_CREATE_EVENT_MESSAGE_ACCEPTED = 0,
    CAP_IM_AI_CREATE_EVENT_AGENT_PROGRESS = 2,
    CAP_IM_AI_CREATE_EVENT_RESPONSE_PARTIAL = 3,
    CAP_IM_AI_CREATE_EVENT_RESPONSE_FINAL = 4,
    CAP_IM_AI_CREATE_EVENT_ERROR = 5,
    CAP_IM_AI_CREATE_EVENT_CANCELLED = 6,
} cap_im_ai_create_event_kind_t;

typedef enum {
    CAP_IM_AI_CREATE_DELIVERY_NEW_RUN = 0,
    CAP_IM_AI_CREATE_DELIVERY_ACTIVE_RUN,
} cap_im_ai_create_delivery_t;

typedef enum {
    CAP_IM_AI_CREATE_STATUS_OK = 0,
    CAP_IM_AI_CREATE_STATUS_INVALID_REQUEST,
    CAP_IM_AI_CREATE_STATUS_BUSY,
    CAP_IM_AI_CREATE_STATUS_UNAVAILABLE,
    CAP_IM_AI_CREATE_STATUS_TIMEOUT,
    CAP_IM_AI_CREATE_STATUS_CANCELLED,
    CAP_IM_AI_CREATE_STATUS_AUTH_REQUIRED,
    CAP_IM_AI_CREATE_STATUS_RATE_LIMITED,
    CAP_IM_AI_CREATE_STATUS_INTERNAL,
} cap_im_ai_create_status_t;

/** Strings are borrowed and remain valid only for the duration of the callback. */
typedef struct {
    uint32_t request_id;
    /** Agent run that owns this message. */
    uint32_t run_id;
    uint32_t sequence;
    cap_im_ai_create_event_kind_t kind;
    cap_im_ai_create_delivery_t delivery;
    cap_im_ai_create_status_t status;
    bool terminal;
    bool retryable;
    const char *text;
} cap_im_ai_create_event_t;

typedef void (*cap_im_ai_create_event_cb_t)(const cap_im_ai_create_event_t *event,
        void *user_ctx);

esp_err_t cap_im_ai_create_register_group(void);

/** One UI consumer owns the local channel subscription at a time. */
esp_err_t cap_im_ai_create_subscribe(cap_im_ai_create_event_cb_t callback,
                                     void *user_ctx);
esp_err_t cap_im_ai_create_unsubscribe(cap_im_ai_create_event_cb_t callback,
                                       void *user_ctx);

/** Post a message; the Agent runtime decides whether it joins an active run. */
esp_err_t cap_im_ai_create_post_message(
    const cap_im_ai_create_request_t *request);
esp_err_t cap_im_ai_create_cancel(uint32_t request_id);

#ifdef __cplusplus
}
#endif
