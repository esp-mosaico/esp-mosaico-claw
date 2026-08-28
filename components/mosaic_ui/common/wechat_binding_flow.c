/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wechat_binding_flow.h"

#include <stdio.h>
#include <string.h>

#define WECHAT_FLOW_PROGRESS_COUNT 12U
#define WECHAT_FLOW_STEP_US 3000000LL
#define WECHAT_FLOW_SCAN_US WECHAT_FLOW_STEP_US
#define WECHAT_FLOW_BIND_US (WECHAT_FLOW_STEP_US * 3LL)
#define WECHAT_FLOW_MOCK_QR "https://mosaico.local/setup/wechat/mock"

static void flow_copy(char *dst, size_t size, const char *src)
{
    if (size == 0U) return;
    if (src == NULL) src = "";
    (void)snprintf(dst, size, "%s", src);
}

void wechat_binding_flow_init(
    wechat_binding_flow_t *flow, bool configured)
{
    if (flow == NULL) return;
    memset(flow, 0, sizeof(*flow));
    flow->configured = configured;
    flow->phase = configured
        ? WECHAT_BINDING_FLOW_SUCCESS : WECHAT_BINDING_FLOW_BINDING;
    flow_copy(flow->bind_status, sizeof(flow->bind_status),
        configured ? "Account linked" : "Preparing QR code");
}

void wechat_binding_flow_begin_simulation(
    wechat_binding_flow_t *flow, int64_t now_us)
{
    if (flow == NULL) return;
    wechat_binding_flow_init(flow, false);
    flow->simulation = true;
    flow->started_us = now_us;
    flow->deadline_us = now_us + WECHAT_FLOW_SCAN_US;
    flow_copy(flow->bind_status, sizeof(flow->bind_status),
        "Scan QR code with WeChat");
    flow_copy(flow->qr_payload, sizeof(flow->qr_payload),
        WECHAT_FLOW_MOCK_QR);
}

bool wechat_binding_flow_step(
    wechat_binding_flow_t *flow, int64_t now_us)
{
    if (flow == NULL || !flow->simulation) return false;
    if (flow->phase == WECHAT_BINDING_FLOW_BINDING) {
        if (now_us < flow->deadline_us) return false;
        flow->phase = WECHAT_BINDING_FLOW_PROGRESS;
        flow->started_us = now_us;
        flow->deadline_us = now_us + WECHAT_FLOW_BIND_US;
        flow->progress = 0;
        flow_copy(flow->stage, sizeof(flow->stage),
            "Confirm on your phone");
        return true;
    }
    if (flow->phase != WECHAT_BINDING_FLOW_PROGRESS) return false;

    const int64_t elapsed = now_us - flow->started_us;
    uint8_t progress = elapsed <= 0 ? 0 : (uint8_t)(
        (elapsed * WECHAT_FLOW_PROGRESS_COUNT) / WECHAT_FLOW_BIND_US);
    if (progress > WECHAT_FLOW_PROGRESS_COUNT) {
        progress = WECHAT_FLOW_PROGRESS_COUNT;
    }
    const char *stage = elapsed < WECHAT_FLOW_STEP_US
        ? "Confirm on your phone"
        : (elapsed < WECHAT_FLOW_STEP_US * 2LL
            ? "Saving binding..." : "Ready to save");
    const bool changed = progress != flow->progress ||
        strcmp(stage, flow->stage) != 0;
    flow->progress = progress;
    flow_copy(flow->stage, sizeof(flow->stage), stage);
    if (now_us >= flow->deadline_us) {
        flow->phase = WECHAT_BINDING_FLOW_SUCCESS;
        flow->configured = true;
        flow->progress = WECHAT_FLOW_PROGRESS_COUNT;
        return true;
    }
    return changed;
}

bool wechat_binding_flow_apply_status(
    wechat_binding_flow_t *flow,
    const mosaic_setup_wechat_status_t *status,
    bool operation_active)
{
    if (flow == NULL || status == NULL) return false;
    flow->simulation = false;
    flow->configured = status->configured ||
        status->state == MOSAIC_SETUP_WECHAT_COMPLETE;
    flow->qr_payload[0] = '\0';
    flow->stage[0] = '\0';
    flow->progress = 0;
    flow_copy(flow->bind_status, sizeof(flow->bind_status),
        status->message[0] ? status->message : "Preparing QR code");

    switch (status->state) {
    case MOSAIC_SETUP_WECHAT_WAITING_SCAN:
        flow->phase = operation_active
            ? WECHAT_BINDING_FLOW_BINDING
            : (flow->configured ? WECHAT_BINDING_FLOW_SUCCESS
                                : WECHAT_BINDING_FLOW_BINDING);
        if (operation_active) {
            flow_copy(flow->bind_status, sizeof(flow->bind_status),
                "Scan QR code with WeChat");
            flow_copy(flow->qr_payload, sizeof(flow->qr_payload),
                status->qr_payload);
        }
        break;
    case MOSAIC_SETUP_WECHAT_SCANNED:
    case MOSAIC_SETUP_WECHAT_SAVING:
    case MOSAIC_SETUP_WECHAT_AWAITING_SAVE:
        if (operation_active) {
            flow->phase = WECHAT_BINDING_FLOW_PROGRESS;
            flow->progress = status->state == MOSAIC_SETUP_WECHAT_SCANNED
                ? 4 : (status->state == MOSAIC_SETUP_WECHAT_SAVING ? 8 : 10);
            const char *fallback = status->state == MOSAIC_SETUP_WECHAT_SCANNED
                ? "Confirm on your phone"
                : (status->state == MOSAIC_SETUP_WECHAT_SAVING
                    ? "Saving binding..." : "Ready to save");
            flow_copy(flow->stage, sizeof(flow->stage),
                status->message[0] ? status->message : fallback);
        }
        break;
    case MOSAIC_SETUP_WECHAT_COMPLETE:
        flow->phase = WECHAT_BINDING_FLOW_SUCCESS;
        flow->progress = WECHAT_FLOW_PROGRESS_COUNT;
        break;
    case MOSAIC_SETUP_WECHAT_CANCELLED:
        flow->phase = flow->configured
            ? WECHAT_BINDING_FLOW_SUCCESS : WECHAT_BINDING_FLOW_BINDING;
        flow_copy(flow->bind_status, sizeof(flow->bind_status),
            "Binding cancelled");
        break;
    case MOSAIC_SETUP_WECHAT_EXPIRED:
        flow->phase = flow->configured
            ? WECHAT_BINDING_FLOW_SUCCESS : WECHAT_BINDING_FLOW_BINDING;
        flow_copy(flow->bind_status, sizeof(flow->bind_status),
            "QR code expired");
        break;
    case MOSAIC_SETUP_WECHAT_ERROR:
        flow->phase = flow->configured
            ? WECHAT_BINDING_FLOW_SUCCESS : WECHAT_BINDING_FLOW_BINDING;
        flow_copy(flow->bind_status, sizeof(flow->bind_status),
            status->message[0] ? status->message : "Unable to bind WeChat");
        break;
    case MOSAIC_SETUP_WECHAT_IDLE:
    default:
        flow->phase = operation_active
            ? WECHAT_BINDING_FLOW_BINDING
            : (flow->configured ? WECHAT_BINDING_FLOW_SUCCESS
                                : WECHAT_BINDING_FLOW_BINDING);
        break;
    }
    return true;
}

bool wechat_binding_flow_has_backend(
    const mosaic_setup_wechat_ops_t *ops)
{
    return ops != NULL && ops->start != NULL && ops->cancel != NULL;
}

esp_err_t wechat_binding_flow_start_backend(
    const mosaic_setup_wechat_ops_t *ops,
    const mosaic_setup_wechat_status_t *status,
    bool force)
{
    if (!wechat_binding_flow_has_backend(ops) || status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *account_id = force && status->account_id[0] != '\0'
        ? status->account_id : NULL;
    return ops->start(ops->user_ctx, account_id, force);
}

esp_err_t wechat_binding_flow_cancel_backend(
    const mosaic_setup_wechat_ops_t *ops)
{
    if (!wechat_binding_flow_has_backend(ops)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ops->cancel(ops->user_ctx);
}
