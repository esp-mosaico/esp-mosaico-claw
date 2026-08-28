/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ai_create_voice_session.h"

#include <inttypes.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define VOICE_COMMAND_QUEUE_DEPTH 8U
#define VOICE_WORKER_STACK_SIZE   5120U
#define VOICE_WORKER_PRIORITY     8U

static const char *TAG = "ai_create_voice";

typedef enum {
    VOICE_COMMAND_BEGIN = 0,
    VOICE_COMMAND_FINISH,
    VOICE_COMMAND_CANCEL,
    VOICE_COMMAND_SHUTDOWN,
} voice_command_type_t;

typedef struct {
    voice_command_type_t type;
    uint32_t operation_id;
    bool send;
} voice_command_t;

struct ai_create_voice_session_t {
    asr_service_handle_t asr;
    ai_create_voice_port_handle_t port;
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t worker_stopped;
    TaskHandle_t worker;
    ai_create_voice_event_cb_t on_event;
    void *on_event_ctx;
    ai_create_voice_status_t status;
    char *result;
    size_t result_size;
    uint32_t current_operation_id;
    uint32_t cancelled_operation_id;
    bool active;
    bool error_forwarded;
};

static ai_create_voice_status_t status_from_error(esp_err_t error)
{
    ai_create_voice_status_t status = {
        .readiness = AI_CREATE_VOICE_READINESS_PROVIDER_ERROR,
        .error = error,
        .retryable = true,
    };
    switch (error) {
    case ESP_OK:
        return ai_create_voice_status_ready();
    case ESP_ERR_TIMEOUT:
        status.readiness = AI_CREATE_VOICE_READINESS_OFFLINE;
        break;
    case ESP_ERR_INVALID_STATE:
        /* The Controller already serializes operations. An invalid provider
         * state here usually means a dropped/not-yet-ready connection. */
        status.readiness = AI_CREATE_VOICE_READINESS_PROVIDER_ERROR;
        break;
    case ESP_ERR_INVALID_SIZE:
        status.readiness = AI_CREATE_VOICE_READINESS_AUDIO_ERROR;
        break;
    case ESP_ERR_NOT_FOUND:
        /* No recognized text is an operation result, not degraded service. */
        status.readiness = AI_CREATE_VOICE_READINESS_READY;
        break;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_NOT_SUPPORTED:
        status.retryable = false;
        break;
    default:
        break;
    }
    return status;
}

static void emit_event(ai_create_voice_session_handle_t session,
    ai_create_voice_event_type_t type, uint32_t operation_id,
    esp_err_t error, bool send, const char *event_text)
{
    xSemaphoreTake(session->lock, portMAX_DELAY);
    session->status = status_from_error(error);
    ai_create_voice_event_cb_t callback = session->on_event;
    if (callback) {
        const ai_create_voice_event_t event = {
            .type = type,
            .operation_id = operation_id,
            .status = session->status,
            .send = send,
            .text = event_text,
        };
        callback(session->port, &event, session->on_event_ctx);
    }
    xSemaphoreGive(session->lock);
}

static void asr_event(asr_service_handle_t asr,
    const asr_service_event_data_t *event, void *user_ctx)
{
    (void)asr;
    ai_create_voice_session_handle_t session = user_ctx;
    if (!session || !event) return;

    xSemaphoreTake(session->lock, portMAX_DELAY);
    const uint32_t operation_id = session->current_operation_id;
    if (event->event == ASR_SERVICE_EVENT_ERROR) {
        session->error_forwarded = true;
    }
    ai_create_voice_event_cb_t callback = session->on_event;
    if (callback && operation_id != 0) {
        ai_create_voice_event_type_t type;
        bool forward = true;
        switch (event->event) {
        case ASR_SERVICE_EVENT_STARTED:
            ESP_LOGI(TAG, "op=%" PRIu32 " ASR started", operation_id);
            type = AI_CREATE_VOICE_EVENT_STARTED;
            break;
        case ASR_SERVICE_EVENT_PARTIAL:
            ESP_LOGI(TAG, "op=%" PRIu32 " ASR partial: %s",
                     operation_id, event->text ? event->text : "");
            type = AI_CREATE_VOICE_EVENT_TRANSCRIPT;
            break;
        case ASR_SERVICE_EVENT_FINAL:
            ESP_LOGI(TAG, "op=%" PRIu32 " ASR final: %s",
                     operation_id, event->text ? event->text : "");
            type = AI_CREATE_VOICE_EVENT_TRANSCRIPT;
            break;
        case ASR_SERVICE_EVENT_ERROR:
            ESP_LOGE(TAG, "op=%" PRIu32 " ASR error: %s",
                     operation_id, esp_err_to_name(event->error));
            type = AI_CREATE_VOICE_EVENT_ERROR;
            break;
        default:
            forward = false;
            type = AI_CREATE_VOICE_EVENT_ERROR;
            break;
        }
        if (forward) {
            session->status = status_from_error(event->error);
            const ai_create_voice_event_t voice_event = {
                .type = type,
                .operation_id = operation_id,
                .status = session->status,
                .text = event->text,
            };
            callback(session->port, &voice_event, session->on_event_ctx);
        }
    }
    xSemaphoreGive(session->lock);

    if (event->event == ASR_SERVICE_EVENT_ERROR && operation_id != 0) {
        const voice_command_t cleanup = {
            .type = VOICE_COMMAND_FINISH,
            .operation_id = operation_id,
            .send = false,
        };
        if (xQueueSendToFront(session->queue, &cleanup, 0) != pdTRUE) {
            ESP_LOGE(TAG, "voice cleanup queue is full");
        }
    }
}

static void process_begin(ai_create_voice_session_handle_t session,
    const voice_command_t *command)
{
    xSemaphoreTake(session->lock, portMAX_DELAY);
    if (session->cancelled_operation_id == command->operation_id) {
        session->cancelled_operation_id = 0;
        xSemaphoreGive(session->lock);
        emit_event(session, AI_CREATE_VOICE_EVENT_COMPLETED,
                   command->operation_id, ESP_OK, false, NULL);
        return;
    }
    if (session->current_operation_id != 0) {
        xSemaphoreGive(session->lock);
        emit_event(session, AI_CREATE_VOICE_EVENT_ERROR,
            command->operation_id, ESP_ERR_INVALID_STATE, false, NULL);
        return;
    }
    session->current_operation_id = command->operation_id;
    session->error_forwarded = false;
    xSemaphoreGive(session->lock);

    esp_err_t err = asr_service_start(session->asr);

    xSemaphoreTake(session->lock, portMAX_DELAY);
    const bool cancelled =
        session->cancelled_operation_id == command->operation_id;
    if (err == ESP_OK) session->active = true;
    const bool report_error = err != ESP_OK && !session->error_forwarded &&
        !cancelled;
    if (err != ESP_OK) {
        session->current_operation_id = 0;
        session->active = false;
        if (cancelled) session->cancelled_operation_id = 0;
    }
    xSemaphoreGive(session->lock);
    if (report_error) {
        emit_event(session, AI_CREATE_VOICE_EVENT_ERROR,
            command->operation_id, err, false, NULL);
    }
}

static void process_finish(ai_create_voice_session_handle_t session,
    const voice_command_t *command)
{
    xSemaphoreTake(session->lock, portMAX_DELAY);
    const bool matches = session->active &&
        session->current_operation_id == command->operation_id;
    bool cancelled =
        session->cancelled_operation_id == command->operation_id;
    xSemaphoreGive(session->lock);
    if (!matches) return;

    bool send = command->send && !cancelled;
    char *result = send ? session->result : NULL;
    size_t result_size = send ? session->result_size : 0;
    if (result) result[0] = '\0';
    esp_err_t err = asr_service_stop(session->asr, result, result_size);

    xSemaphoreTake(session->lock, portMAX_DELAY);
    cancelled = cancelled ||
        session->cancelled_operation_id == command->operation_id;
    send = send && !cancelled;
    if (session->current_operation_id == command->operation_id) {
        session->current_operation_id = 0;
        session->active = false;
    }
    if (session->cancelled_operation_id == command->operation_id) {
        session->cancelled_operation_id = 0;
    }
    xSemaphoreGive(session->lock);

    if (send && err == ESP_OK && result && result[0]) {
        ESP_LOGI(TAG, "op=%" PRIu32 " ASR completed: %s",
                 command->operation_id, result);
    } else if (send) {
        ESP_LOGW(TAG, "op=%" PRIu32 " ASR completed without text: %s",
                 command->operation_id, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "op=%" PRIu32 " ASR cancelled",
                 command->operation_id);
    }

    emit_event(session, AI_CREATE_VOICE_EVENT_COMPLETED,
        command->operation_id, err, send, send ? result : NULL);
}

static void voice_worker(void *arg)
{
    ai_create_voice_session_handle_t session = arg;
    bool shutdown = false;
    while (!shutdown) {
        voice_command_t command;
        if (xQueueReceive(session->queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (command.type) {
        case VOICE_COMMAND_BEGIN:
            process_begin(session, &command);
            break;
        case VOICE_COMMAND_FINISH:
            process_finish(session, &command);
            break;
        case VOICE_COMMAND_CANCEL:
            command.send = false;
            process_finish(session, &command);
            break;
        case VOICE_COMMAND_SHUTDOWN:
            shutdown = true;
            break;
        }
    }

    xSemaphoreTake(session->lock, portMAX_DELAY);
    const uint32_t operation_id = session->current_operation_id;
    const bool active = session->active;
    xSemaphoreGive(session->lock);
    if (active) {
        const voice_command_t finish = {
            .type = VOICE_COMMAND_FINISH,
            .operation_id = operation_id,
            .send = false,
        };
        process_finish(session, &finish);
    }
    xSemaphoreGive(session->worker_stopped);
    vTaskDelete(NULL);
}

static esp_err_t session_begin(ai_create_voice_session_handle_t session,
    uint32_t operation_id);
static esp_err_t session_finish(ai_create_voice_session_handle_t session,
    uint32_t operation_id, bool send);
static esp_err_t session_cancel(ai_create_voice_session_handle_t session,
    uint32_t operation_id);

static esp_err_t port_register_cb(void *ctx,
    ai_create_voice_event_cb_t callback, void *callback_ctx)
{
    ai_create_voice_session_handle_t session = ctx;
    if (!session || !session->lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(session->lock, portMAX_DELAY);
    session->on_event = callback;
    session->on_event_ctx = callback_ctx;
    xSemaphoreGive(session->lock);
    return ESP_OK;
}

static esp_err_t port_get_status(void *ctx,
    ai_create_voice_status_t *out_status)
{
    ai_create_voice_session_handle_t session = ctx;
    if (!session || !out_status) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(session->lock, portMAX_DELAY);
    *out_status = session->status;
    xSemaphoreGive(session->lock);
    return ESP_OK;
}

static esp_err_t port_begin(void *ctx, uint32_t operation_id)
{
    return session_begin(ctx, operation_id);
}

static esp_err_t port_finish(void *ctx, uint32_t operation_id, bool send)
{
    return session_finish(ctx, operation_id, send);
}

static esp_err_t port_cancel(void *ctx, uint32_t operation_id)
{
    return session_cancel(ctx, operation_id);
}

static esp_err_t port_get_loudness(void *ctx, uint8_t *out_level)
{
    ai_create_voice_session_handle_t session = ctx;
    if (!session || !out_level) return ESP_ERR_INVALID_ARG;
    return asr_service_get_loudness(session->asr, out_level);
}

static const ai_create_voice_port_ops_t s_port_ops = {
    .register_cb = port_register_cb,
    .get_status = port_get_status,
    .begin = port_begin,
    .finish = port_finish,
    .cancel = port_cancel,
    .get_loudness = port_get_loudness,
};

esp_err_t ai_create_voice_session_create(
    const ai_create_voice_session_config_t *config,
    ai_create_voice_session_handle_t *ret_session)
{
    if (!config || !ret_session || !config->asr ||
        config->result_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_session = NULL;
    ai_create_voice_session_handle_t session = calloc(1, sizeof(*session));
    if (!session) return ESP_ERR_NO_MEM;
    session->result = malloc(config->result_size);
    session->queue = xQueueCreate(VOICE_COMMAND_QUEUE_DEPTH,
                                  sizeof(voice_command_t));
    session->lock = xSemaphoreCreateMutex();
    session->worker_stopped = xSemaphoreCreateBinary();
    if (!session->result || !session->queue || !session->lock ||
        !session->worker_stopped) {
        ai_create_voice_session_delete(session);
        return ESP_ERR_NO_MEM;
    }
    session->asr = config->asr;
    session->result_size = config->result_size;
    session->status = ai_create_voice_status_ready();
    esp_err_t err = ai_create_voice_port_create(
        &(ai_create_voice_port_config_t) {
            .ops = &s_port_ops,
            .ctx = session,
        }, &session->port);
    if (err != ESP_OK) {
        ai_create_voice_session_delete(session);
        return err;
    }
    err = asr_service_register_cb(config->asr, asr_event, session);
    if (err != ESP_OK) {
        ai_create_voice_session_delete(session);
        return err;
    }
    BaseType_t created = xTaskCreate(voice_worker, "ai_voice",
        VOICE_WORKER_STACK_SIZE, session, VOICE_WORKER_PRIORITY,
        &session->worker);
    if (created != pdPASS) {
        (void)asr_service_register_cb(config->asr, NULL, NULL);
        ai_create_voice_session_delete(session);
        return ESP_ERR_NO_MEM;
    }
    *ret_session = session;
    return ESP_OK;
}

ai_create_voice_port_handle_t ai_create_voice_session_get_port(
    ai_create_voice_session_handle_t session)
{
    return session ? session->port : NULL;
}

void ai_create_voice_session_delete(ai_create_voice_session_handle_t session)
{
    if (!session) return;
    if (session->lock) {
        xSemaphoreTake(session->lock, portMAX_DELAY);
        session->on_event = NULL;
        session->on_event_ctx = NULL;
        xSemaphoreGive(session->lock);
    }
    if (session->asr) {
        (void)asr_service_register_cb(session->asr, NULL, NULL);
    }
    if (session->worker && session->queue && session->worker_stopped) {
        const voice_command_t shutdown = {.type = VOICE_COMMAND_SHUTDOWN};
        (void)xQueueSend(session->queue, &shutdown, portMAX_DELAY);
        (void)xSemaphoreTake(session->worker_stopped, portMAX_DELAY);
    }
    if (session->worker_stopped) vSemaphoreDelete(session->worker_stopped);
    if (session->lock) vSemaphoreDelete(session->lock);
    if (session->queue) vQueueDelete(session->queue);
    ai_create_voice_port_delete(session->port);
    free(session->result);
    free(session);
}

static esp_err_t enqueue_command(ai_create_voice_session_handle_t session,
    voice_command_type_t type, uint32_t operation_id, bool send)
{
    if (!session || operation_id == 0) return ESP_ERR_INVALID_ARG;
    const voice_command_t command = {
        .type = type,
        .operation_id = operation_id,
        .send = send,
    };
    return xQueueSend(session->queue, &command, 0) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t session_begin(
    ai_create_voice_session_handle_t session, uint32_t operation_id)
{
    ESP_LOGI(TAG, "op=%" PRIu32 " voice begin queued", operation_id);
    return enqueue_command(session, VOICE_COMMAND_BEGIN, operation_id, false);
}

static esp_err_t session_finish(
    ai_create_voice_session_handle_t session, uint32_t operation_id,
    bool send)
{
    esp_err_t err = enqueue_command(
        session, VOICE_COMMAND_FINISH, operation_id, send);
    if (err == ESP_OK) {
        /* End microphone capture now. The worker may still be blocked in
         * provider connect/start; buffered speech is preserved until ready. */
        (void)asr_service_end_capture(session->asr, !send);
        ESP_LOGI(TAG, "op=%" PRIu32 " voice %s queued",
                 operation_id, send ? "finish" : "cancel");
    }
    return err;
}

static esp_err_t session_cancel(
    ai_create_voice_session_handle_t session, uint32_t operation_id)
{
    if (!session || operation_id == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(session->lock, portMAX_DELAY);
    if (session->cancelled_operation_id == operation_id) {
        xSemaphoreGive(session->lock);
        return ESP_OK;
    }
    session->cancelled_operation_id = operation_id;
    xSemaphoreGive(session->lock);

    /* Wake capture/provider startup immediately. The high-priority command
     * converts an already queued normal finish into a discarded result. */
    (void)asr_service_end_capture(session->asr, true);
    const voice_command_t command = {
        .type = VOICE_COMMAND_CANCEL,
        .operation_id = operation_id,
        .send = false,
    };
    esp_err_t err = xQueueSendToFront(session->queue, &command, 0) == pdTRUE
                        ? ESP_OK : ESP_ERR_TIMEOUT;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "op=%" PRIu32 " voice cancel queued", operation_id);
    } else {
        ESP_LOGE(TAG, "op=%" PRIu32 " voice cancel queue is full",
                 operation_id);
    }
    return err;
}
