/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ai_create_voice_port.h"

#include <stdlib.h>

struct ai_create_voice_port_t {
    const ai_create_voice_port_ops_t *ops;
    void *ctx;
};

esp_err_t ai_create_voice_port_create(
    const ai_create_voice_port_config_t *config,
    ai_create_voice_port_handle_t *ret_port)
{
    if (!config || !ret_port || !config->ops ||
        !config->ops->register_cb || !config->ops->get_status ||
        !config->ops->begin || !config->ops->finish ||
        !config->ops->cancel || !config->ops->get_loudness) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_port = calloc(1, sizeof(**ret_port));
    if (!*ret_port) return ESP_ERR_NO_MEM;
    (*ret_port)->ops = config->ops;
    (*ret_port)->ctx = config->ctx;
    return ESP_OK;
}

void ai_create_voice_port_delete(ai_create_voice_port_handle_t port)
{
    free(port);
}

esp_err_t ai_create_voice_port_register_cb(ai_create_voice_port_handle_t port,
    ai_create_voice_event_cb_t callback, void *callback_ctx)
{
    if (!port) return ESP_ERR_INVALID_ARG;
    return port->ops->register_cb(port->ctx, callback, callback_ctx);
}

esp_err_t ai_create_voice_port_get_status(ai_create_voice_port_handle_t port,
    ai_create_voice_status_t *out_status)
{
    if (!port || !out_status) return ESP_ERR_INVALID_ARG;
    return port->ops->get_status(port->ctx, out_status);
}

esp_err_t ai_create_voice_port_begin(ai_create_voice_port_handle_t port,
    uint32_t operation_id)
{
    if (!port) return ESP_ERR_INVALID_ARG;
    return port->ops->begin(port->ctx, operation_id);
}

esp_err_t ai_create_voice_port_finish(ai_create_voice_port_handle_t port,
    uint32_t operation_id, bool send)
{
    if (!port) return ESP_ERR_INVALID_ARG;
    return port->ops->finish(port->ctx, operation_id, send);
}

esp_err_t ai_create_voice_port_cancel(ai_create_voice_port_handle_t port,
    uint32_t operation_id)
{
    if (!port) return ESP_ERR_INVALID_ARG;
    return port->ops->cancel(port->ctx, operation_id);
}

esp_err_t ai_create_voice_port_get_loudness(ai_create_voice_port_handle_t port,
    uint8_t *out_level)
{
    if (!port || !out_level) return ESP_ERR_INVALID_ARG;
    return port->ops->get_loudness(port->ctx, out_level);
}

ai_create_voice_status_t ai_create_voice_status_disabled(void)
{
    return (ai_create_voice_status_t) {
        .readiness = AI_CREATE_VOICE_READINESS_DISABLED,
        .error = ESP_ERR_NOT_SUPPORTED,
        .retryable = false,
    };
}

ai_create_voice_status_t ai_create_voice_status_ready(void)
{
    return (ai_create_voice_status_t) {
        .readiness = AI_CREATE_VOICE_READINESS_READY,
        .error = ESP_OK,
        .retryable = true,
    };
}
