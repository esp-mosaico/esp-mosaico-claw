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

typedef enum {
    MOSAIC_SETUP_MODE_ONBOARDING = 0,
    MOSAIC_SETUP_MODE_MANAGE,
} mosaic_setup_mode_t;

typedef enum {
    MOSAIC_SETUP_ROUTE_OVERVIEW = 0,
    MOSAIC_SETUP_ROUTE_NETWORK,
    MOSAIC_SETUP_ROUTE_WECHAT,
    MOSAIC_SETUP_ROUTE_LLM,
    MOSAIC_SETUP_ROUTE_INTEGRATIONS,
} mosaic_setup_route_t;

#define MOSAIC_SETUP_WECHAT_QR_LEN      256U
#define MOSAIC_SETUP_WECHAT_ACCOUNT_LEN 64U
#define MOSAIC_SETUP_WECHAT_MESSAGE_LEN 96U

typedef enum {
    MOSAIC_SETUP_WECHAT_IDLE = 0,
    MOSAIC_SETUP_WECHAT_WAITING_SCAN,
    MOSAIC_SETUP_WECHAT_SCANNED,
    MOSAIC_SETUP_WECHAT_SAVING,
    MOSAIC_SETUP_WECHAT_AWAITING_SAVE,
    MOSAIC_SETUP_WECHAT_COMPLETE,
    MOSAIC_SETUP_WECHAT_CANCELLED,
    MOSAIC_SETUP_WECHAT_EXPIRED,
    MOSAIC_SETUP_WECHAT_ERROR,
} mosaic_setup_wechat_state_t;

typedef struct {
    bool active;
    bool configured;
    bool persisted;
    mosaic_setup_wechat_state_t state;
    char message[MOSAIC_SETUP_WECHAT_MESSAGE_LEN];
    char qr_payload[MOSAIC_SETUP_WECHAT_QR_LEN];
    char account_id[MOSAIC_SETUP_WECHAT_ACCOUNT_LEN];
} mosaic_setup_wechat_status_t;

typedef struct {
    esp_err_t (*start)(void *user_ctx, const char *account_id, bool force);
    esp_err_t (*cancel)(void *user_ctx);
    void *user_ctx;
} mosaic_setup_wechat_ops_t;

typedef esp_err_t (*mosaic_setup_app_request_cb_t)(
    void *user_ctx, const char *app_name);

/** Provide App navigation for hosts that do not use the ESP loader.
 *
 * Passing NULL clears the override. Firmware normally uses the built-in
 * loader fallback; host and Web simulators should bind their active runtime.
 */
esp_err_t mosaic_setup_configure_app_request(
    mosaic_setup_app_request_cb_t callback, void *user_ctx);

/** Open Setup Center at one logical route. */
esp_err_t mosaic_setup_open(mosaic_setup_mode_t mode,
                            mosaic_setup_route_t route);

/** Return to this App when a managed Setup route closes; NULL clears it. */
esp_err_t mosaic_setup_set_return_app(const char *app_name);

/** Connect Setup Center to the platform's WeChat binding service. */
esp_err_t mosaic_setup_configure_wechat(
    const mosaic_setup_wechat_ops_t *ops);

/** Thread-safe status publication; rendering remains on the UI task. */
esp_err_t mosaic_setup_set_wechat_status(
    const mosaic_setup_wechat_status_t *status);

/** Snapshot the shared platform binding port without exposing Setup UI state.
 * Settings owns its own page/state machine and may consume this service. */
esp_err_t mosaic_setup_get_wechat_service(
    mosaic_setup_wechat_ops_t *ret_ops,
    mosaic_setup_wechat_status_t *ret_status,
    uint32_t *ret_revision);

#ifdef __cplusplus
}
#endif
