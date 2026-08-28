/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wechat_binding_service_t *
    wechat_binding_service_handle_t;

typedef enum {
    WECHAT_BINDING_STATE_IDLE,
    WECHAT_BINDING_STATE_WAITING_SCAN,
    WECHAT_BINDING_STATE_SCANNED,
    WECHAT_BINDING_STATE_SAVING,
    WECHAT_BINDING_STATE_AWAITING_SAVE,
    WECHAT_BINDING_STATE_COMPLETE,
    WECHAT_BINDING_STATE_CANCELLED,
    WECHAT_BINDING_STATE_EXPIRED,
    WECHAT_BINDING_STATE_ERROR,
} wechat_binding_state_t;

typedef enum {
    /** Return completed credentials to the caller without saving them. */
    WECHAT_BINDING_PERSIST_MANUAL,
    /** Save completed credentials through the configured persist callback. */
    WECHAT_BINDING_PERSIST_AUTOMATIC,
} wechat_binding_persist_mode_t;

typedef struct {
    char token[256];
    char base_url[160];
    char account_id[64];
} wechat_binding_credentials_t;

typedef struct {
    bool active;
    bool configured;
    bool persisted;
    wechat_binding_state_t state;
    char message[96];
    char qr_payload[256];
    char account_id[64];
    char user_id[96];
} wechat_binding_status_t;

/**
 * Full session snapshot for trusted transports such as the local setup
 * server. UI observers receive wechat_binding_status_t and never credentials.
 */
typedef struct {
    bool active;
    bool configured;
    bool completed;
    bool persisted;
    char session_key[64];
    char status[32];
    char message[160];
    char qr_data_url[256];
    char account_id[64];
    char user_id[96];
    char token[256];
    char base_url[160];
} wechat_binding_session_t;

typedef esp_err_t (*wechat_binding_persist_fn_t)(
    const wechat_binding_credentials_t *credentials,
    void *user_ctx);
typedef void (*wechat_binding_event_cb_t)(
    wechat_binding_service_handle_t handle,
    const wechat_binding_status_t *status,
    void *user_ctx);

typedef struct {
    wechat_binding_persist_fn_t persist;
    void *persist_ctx;
    bool initially_configured;
    const char *initial_account_id;
} wechat_binding_service_config_t;

esp_err_t wechat_binding_service_create(
    const wechat_binding_service_config_t *config,
    wechat_binding_service_handle_t *ret_handle);
void wechat_binding_service_delete(
    wechat_binding_service_handle_t handle);

esp_err_t wechat_binding_service_start(
    wechat_binding_service_handle_t handle,
    const char *account_id,
    bool force,
    wechat_binding_persist_mode_t persist_mode);
esp_err_t wechat_binding_service_cancel(
    wechat_binding_service_handle_t handle);
esp_err_t wechat_binding_service_get_status(
    wechat_binding_service_handle_t handle,
    wechat_binding_status_t *ret_status);
esp_err_t wechat_binding_service_get_session(
    wechat_binding_service_handle_t handle,
    wechat_binding_session_t *ret_session);
esp_err_t wechat_binding_service_register_cb(
    wechat_binding_service_handle_t handle,
    wechat_binding_event_cb_t callback,
    void *user_ctx);
esp_err_t wechat_binding_service_unregister_cb(
    wechat_binding_service_handle_t handle,
    wechat_binding_event_cb_t callback,
    void *user_ctx);

#ifdef __cplusplus
}
#endif
