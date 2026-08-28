/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mosaic_setup.h"

typedef enum {
    WECHAT_BINDING_FLOW_BINDING = 0,
    WECHAT_BINDING_FLOW_PROGRESS,
    WECHAT_BINDING_FLOW_SUCCESS,
} wechat_binding_flow_phase_t;

typedef struct {
    wechat_binding_flow_phase_t phase;
    bool configured;
    bool simulation;
    uint8_t progress;
    int64_t started_us;
    int64_t deadline_us;
    char bind_status[MOSAIC_SETUP_WECHAT_MESSAGE_LEN];
    char stage[MOSAIC_SETUP_WECHAT_MESSAGE_LEN];
    char qr_payload[MOSAIC_SETUP_WECHAT_QR_LEN];
} wechat_binding_flow_t;

void wechat_binding_flow_init(
    wechat_binding_flow_t *flow, bool configured);
void wechat_binding_flow_begin_simulation(
    wechat_binding_flow_t *flow, int64_t now_us);
bool wechat_binding_flow_step(
    wechat_binding_flow_t *flow, int64_t now_us);
bool wechat_binding_flow_apply_status(
    wechat_binding_flow_t *flow,
    const mosaic_setup_wechat_status_t *status,
    bool operation_active);
bool wechat_binding_flow_has_backend(
    const mosaic_setup_wechat_ops_t *ops);
esp_err_t wechat_binding_flow_start_backend(
    const mosaic_setup_wechat_ops_t *ops,
    const mosaic_setup_wechat_status_t *status,
    bool force);
esp_err_t wechat_binding_flow_cancel_backend(
    const mosaic_setup_wechat_ops_t *ops);
