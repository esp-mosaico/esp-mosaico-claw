/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mosaic_ai_create_runtime.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ai_create_session_history.h"
#include "ai_create_voice_session.h"
#include "mosaic_app_catalog.h"
#include "mosaic_loader.h"
#include "sdkconfig.h"

#define AI_CREATE_APP_NAME "ai_create"
#define AI_CREATE_SESSION_QUEUE_DEPTH 4U
#define AI_CREATE_SESSION_TASK_STACK 6144U
#define AI_CREATE_SESSION_TASK_PRIORITY 3U

typedef enum {
    AI_CREATE_SESSION_REFRESH = 0,
    AI_CREATE_SESSION_SELECT,
    AI_CREATE_SESSION_DELETE,
} ai_create_session_command_type_t;

typedef struct {
    ai_create_session_command_type_t type;
    uint32_t generation;
    char alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
} ai_create_session_command_t;

static SemaphoreHandle_t s_lock;
static asr_service_handle_t s_asr;
static ai_create_voice_session_handle_t s_voice_session;
static EXT_RAM_BSS_ATTR ai_create_voice_status_t s_voice_fallback_status;
static bool s_voice_swap_in_progress;
static ai_create_controller_handle_t s_controller;
static QueueHandle_t s_session_queue;
static TaskHandle_t s_session_worker;
static EXT_RAM_BSS_ATTR claw_session_mgr_alias_map_t s_sessions;
static uint32_t s_sessions_revision;
static uint32_t s_invalidation_revision;
static TaskHandle_t s_ui_update_task;
static uint32_t s_ui_update_depth;
static bool s_refresh_queued;

#if CONFIG_MOSAIC_UI_AI_CREATE_DEVICE_MOCK

#define AI_CREATE_MOCK_ACCEPT_DELAY_US  20000LL
#define AI_CREATE_MOCK_PARTIAL_DELAY_US 120000LL
#define AI_CREATE_MOCK_FINAL_DELAY_US   180000LL

static const char *TAG = "ai_create_mock";

typedef enum {
    AI_CREATE_MOCK_IDLE = 0,
    AI_CREATE_MOCK_ACCEPTED,
    AI_CREATE_MOCK_PARTIAL,
    AI_CREATE_MOCK_FINAL,
    AI_CREATE_MOCK_CANCELLED,
} ai_create_mock_phase_t;

typedef struct {
    ai_create_mock_phase_t phase;
    uint32_t request_id;
    uint32_t sequence;
    int64_t due_us;
} ai_create_mock_request_t;

static cap_im_ai_create_event_cb_t s_mock_callback;
static void *s_mock_callback_ctx;
static ai_create_mock_request_t s_mock_request;

#endif

static void invalidate_app(void)
{
    const mosaic_app_descriptor_t *app =
        mosaic_app_descriptor_for_name(AI_CREATE_APP_NAME);
    if (!app) return;
    uint32_t revision = __atomic_add_fetch(
        &s_invalidation_revision, 1U, __ATOMIC_RELAXED);
    if (revision == 0) {
        revision = __atomic_add_fetch(
            &s_invalidation_revision, 1U, __ATOMIC_RELAXED);
    }
    (void)mosaic_loader_invalidate_app(app->id, revision);
}

static void controller_changed(ai_create_controller_handle_t controller,
    void *user_ctx)
{
    (void)controller;
    (void)user_ctx;
    bool rendered_by_current_event = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        rendered_by_current_event = s_ui_update_depth > 0 &&
            s_ui_update_task == xTaskGetCurrentTaskHandle();
        xSemaphoreGive(s_lock);
    }
    if (!rendered_by_current_event) invalidate_app();
}

static esp_err_t ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static void publish_sessions(const claw_session_mgr_alias_map_t *sessions)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (sessions) s_sessions = *sessions;
    else memset(&s_sessions, 0, sizeof(s_sessions));
    s_sessions_revision++;
    if (s_sessions_revision == 0) s_sessions_revision = 1;
    xSemaphoreGive(s_lock);
    invalidate_app();
}

static void refresh_sessions_now(void)
{
    claw_session_mgr_alias_map_t *sessions = calloc(1, sizeof(*sessions));
    if (!sessions) return;
    if (ai_create_controller_list_sessions(s_controller, sessions) != ESP_OK) {
        memset(sessions, 0, sizeof(*sessions));
    }
    publish_sessions(sessions);
    free(sessions);
}

static void session_worker(void *arg)
{
    (void)arg;
    ai_create_session_command_t command;
    for (;;) {
        if (xQueueReceive(s_session_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command.type == AI_CREATE_SESSION_REFRESH) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_refresh_queued = false;
            xSemaphoreGive(s_lock);
            refresh_sessions_now();
            continue;
        }
        if (command.type == AI_CREATE_SESSION_SELECT) {
            (void)ai_create_controller_complete_session_switch(
                s_controller, command.alias, command.generation);
            refresh_sessions_now();
            continue;
        }
        if (command.type == AI_CREATE_SESSION_DELETE) {
            (void)ai_create_controller_complete_session_delete(
                s_controller, command.alias, command.generation);
            refresh_sessions_now();
        }
    }
}

#if CONFIG_MOSAIC_UI_AI_CREATE_DEVICE_MOCK

static esp_err_t mock_gateway_subscribe(
    cap_im_ai_create_event_cb_t callback, void *ctx)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mock_callback &&
        (s_mock_callback != callback || s_mock_callback_ctx != ctx)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_mock_callback = callback;
    s_mock_callback_ctx = ctx;
    memset(&s_mock_request, 0, sizeof(s_mock_request));
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "local response backend enabled; Agent gateway is bypassed");
    return ESP_OK;
}

static esp_err_t mock_gateway_unsubscribe(
    cap_im_ai_create_event_cb_t callback, void *ctx)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mock_callback != callback || s_mock_callback_ctx != ctx) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_mock_callback = NULL;
    s_mock_callback_ctx = NULL;
    memset(&s_mock_request, 0, sizeof(s_mock_request));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t mock_gateway_post_message(
    const cap_im_ai_create_request_t *request)
{
    if (!request || request->request_id == 0 || !request->text ||
        !request->text[0] || request->mode >= CAP_IM_AI_CREATE_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_mock_callback) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mock_request.phase != AI_CREATE_MOCK_IDLE) {
        cap_im_ai_create_event_cb_t callback = s_mock_callback;
        void *callback_ctx = s_mock_callback_ctx;
        const cap_im_ai_create_event_t event = {
            .request_id = request->request_id,
            .run_id = s_mock_request.request_id,
            .sequence = 1,
            .kind = CAP_IM_AI_CREATE_EVENT_MESSAGE_ACCEPTED,
            .delivery = CAP_IM_AI_CREATE_DELIVERY_ACTIVE_RUN,
            .status = CAP_IM_AI_CREATE_STATUS_OK,
        };
        xSemaphoreGive(s_lock);
        callback(&event, callback_ctx);
        return ESP_OK;
    }
    s_mock_request = (ai_create_mock_request_t) {
        .phase = AI_CREATE_MOCK_ACCEPTED,
        .request_id = request->request_id,
        .due_us = esp_timer_get_time() + AI_CREATE_MOCK_ACCEPT_DELAY_US,
    };
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "request=%" PRIu32 " mode=%d ASR text: %s",
             request->request_id, (int)request->mode, request->text);
    return ESP_OK;
}

static esp_err_t mock_gateway_cancel(uint32_t request_id)
{
    if (request_id == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mock_request.phase == AI_CREATE_MOCK_IDLE ||
        s_mock_request.request_id != request_id) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_mock_request.phase = AI_CREATE_MOCK_CANCELLED;
    s_mock_request.due_us = esp_timer_get_time() +
                            AI_CREATE_MOCK_ACCEPT_DELAY_US;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static const ai_create_gateway_ops_t s_mock_gateway = {
    .subscribe = mock_gateway_subscribe,
    .unsubscribe = mock_gateway_unsubscribe,
    .post_message = mock_gateway_post_message,
    .cancel = mock_gateway_cancel,
};

static esp_err_t mock_gateway_step(int64_t now_us)
{
    cap_im_ai_create_event_cb_t callback = NULL;
    void *callback_ctx = NULL;
    cap_im_ai_create_event_t event = {0};
    bool emit = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mock_request.phase != AI_CREATE_MOCK_IDLE &&
        now_us >= s_mock_request.due_us && s_mock_callback) {
        callback = s_mock_callback;
        callback_ctx = s_mock_callback_ctx;
        event.request_id = s_mock_request.request_id;
        event.run_id = s_mock_request.request_id;
        event.sequence = ++s_mock_request.sequence;
        event.status = CAP_IM_AI_CREATE_STATUS_OK;
        emit = true;
        switch (s_mock_request.phase) {
        case AI_CREATE_MOCK_ACCEPTED:
            event.kind = CAP_IM_AI_CREATE_EVENT_MESSAGE_ACCEPTED;
            event.delivery = CAP_IM_AI_CREATE_DELIVERY_NEW_RUN;
            event.text = "Device simulation: voice input received.";
            s_mock_request.phase = AI_CREATE_MOCK_PARTIAL;
            s_mock_request.due_us = now_us +
                                    AI_CREATE_MOCK_PARTIAL_DELAY_US;
            break;
        case AI_CREATE_MOCK_PARTIAL:
            event.kind = CAP_IM_AI_CREATE_EVENT_RESPONSE_PARTIAL;
            event.text = "Device simulation: testing page transitions…";
            s_mock_request.phase = AI_CREATE_MOCK_FINAL;
            s_mock_request.due_us = now_us + AI_CREATE_MOCK_FINAL_DELAY_US;
            break;
        case AI_CREATE_MOCK_FINAL:
            event.kind = CAP_IM_AI_CREATE_EVENT_RESPONSE_FINAL;
            event.terminal = true;
            event.text = "Simulated response: voice input and page transitions "
                         "succeeded; the backend Agent was not called.";
            memset(&s_mock_request, 0, sizeof(s_mock_request));
            break;
        case AI_CREATE_MOCK_CANCELLED:
            event.kind = CAP_IM_AI_CREATE_EVENT_CANCELLED;
            event.status = CAP_IM_AI_CREATE_STATUS_CANCELLED;
            event.terminal = true;
            event.text = "Device simulation request cancelled.";
            memset(&s_mock_request, 0, sizeof(s_mock_request));
            break;
        case AI_CREATE_MOCK_IDLE:
        default:
            emit = false;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (emit) {
        ESP_LOGI(TAG, "request=%" PRIu32 " event=%d sequence=%" PRIu32,
                 event.request_id, (int)event.kind, event.sequence);
        callback(&event, callback_ctx);
    }
    return ESP_OK;
}

#endif

static esp_err_t create_voice_adapter(asr_service_handle_t asr,
    ai_create_voice_session_handle_t *ret_session,
    ai_create_voice_port_handle_t *ret_port)
{
    if (!ret_session || !ret_port) return ESP_ERR_INVALID_ARG;
    *ret_session = NULL;
    *ret_port = NULL;
    if (!asr) return ESP_OK;
    esp_err_t err = ai_create_voice_session_create(
        &(ai_create_voice_session_config_t) {
            .asr = asr,
            .result_size = AI_CREATE_CONTROLLER_TEXT_MAX + 1U,
        }, ret_session);
    if (err == ESP_OK) {
        *ret_port = ai_create_voice_session_get_port(*ret_session);
        if (!*ret_port) {
            ai_create_voice_session_delete(*ret_session);
            *ret_session = NULL;
            err = ESP_ERR_INVALID_STATE;
        }
    }
    return err;
}

static esp_err_t set_asr_with_status(asr_service_handle_t asr,
    ai_create_voice_status_t fallback_status)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_controller) {
        s_asr = asr;
        s_voice_fallback_status = fallback_status;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_voice_swap_in_progress) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_asr == asr && asr != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_voice_swap_in_progress = true;
    ai_create_controller_handle_t controller = s_controller;
    ai_create_voice_session_handle_t previous_session = s_voice_session;
    xSemaphoreGive(s_lock);

    ai_create_voice_session_handle_t replacement_session = NULL;
    ai_create_voice_port_handle_t replacement_port = NULL;
    err = create_voice_adapter(asr, &replacement_session, &replacement_port);
    if (err == ESP_OK) {
        err = ai_create_controller_set_voice_port(controller,
            replacement_port, fallback_status);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK && s_controller == controller &&
        s_voice_session == previous_session) {
        s_asr = asr;
        s_voice_fallback_status = fallback_status;
        s_voice_session = replacement_session;
        replacement_session = NULL;
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    s_voice_swap_in_progress = false;
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ai_create_voice_session_delete(previous_session);
    } else {
        ai_create_voice_session_delete(replacement_session);
    }
    return err;
}

esp_err_t mosaic_ai_create_runtime_set_asr(asr_service_handle_t asr)
{
    return set_asr_with_status(asr, asr ? ai_create_voice_status_ready()
                                        : ai_create_voice_status_disabled());
}

esp_err_t mosaic_ai_create_runtime_set_voice_status(
    ai_create_voice_status_t status)
{
    return set_asr_with_status(NULL, status);
}

esp_err_t mosaic_ai_create_runtime_init(void)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_controller) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    ai_create_voice_port_handle_t voice_port = NULL;
    err = create_voice_adapter(s_asr, &s_voice_session, &voice_port);
    if (err == ESP_OK) {
        err = ai_create_controller_create(&(ai_create_controller_config_t) {
#if CONFIG_MOSAIC_UI_AI_CREATE_DEVICE_MOCK
            .gateway = &s_mock_gateway,
#endif
            .history = ai_create_session_history_ops(),
            .voice_port = voice_port,
            .voice_status = s_voice_fallback_status,
            .chat_id = "device-local",
            .session_alias = NULL,
            .cancel_on_stop = true,
            .persist_journal = true,
            .on_changed = controller_changed,
        }, &s_controller);
    }
    if (err == ESP_OK) {
        memset(&s_sessions, 0, sizeof(s_sessions));
        s_sessions_revision = 1;
        s_session_queue = xQueueCreate(
            AI_CREATE_SESSION_QUEUE_DEPTH,
            sizeof(ai_create_session_command_t));
        if (!s_session_queue ||
            xTaskCreate(session_worker, "ai_sessions",
                        AI_CREATE_SESSION_TASK_STACK, NULL,
                        AI_CREATE_SESSION_TASK_PRIORITY,
                        &s_session_worker) != pdPASS) {
            if (s_session_queue) {
                vQueueDelete(s_session_queue);
                s_session_queue = NULL;
            }
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        if (s_controller) {
            ai_create_controller_delete(s_controller);
            s_controller = NULL;
        }
        ai_create_voice_session_delete(s_voice_session);
        s_voice_session = NULL;
    }
    xSemaphoreGive(s_lock);
    return err;
}

ai_create_controller_handle_t mosaic_ai_create_runtime_controller(void)
{
    return s_controller;
}

esp_err_t mosaic_ai_create_runtime_step(int64_t now_us)
{
    if (!s_controller) return ESP_ERR_INVALID_STATE;
    if (now_us <= 0) now_us = esp_timer_get_time();
    esp_err_t err = ai_create_controller_step(s_controller, now_us);
    if (err != ESP_OK) return err;
#if CONFIG_MOSAIC_UI_AI_CREATE_DEVICE_MOCK
    return mock_gateway_step(now_us);
#else
    return ESP_OK;
#endif
}

esp_err_t mosaic_ai_create_runtime_refresh_sessions(void)
{
    if (!s_controller || !s_session_queue) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_refresh_queued) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_refresh_queued = true;
    xSemaphoreGive(s_lock);
    const ai_create_session_command_t command = {
        .type = AI_CREATE_SESSION_REFRESH,
    };
    if (xQueueSend(s_session_queue, &command, 0) == pdTRUE) return ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_refresh_queued = false;
    xSemaphoreGive(s_lock);
    return ESP_ERR_TIMEOUT;
}

esp_err_t mosaic_ai_create_runtime_get_sessions(
    claw_session_mgr_alias_map_t *out_sessions, uint32_t *out_revision)
{
    if (!out_sessions || !out_revision) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_sessions = s_sessions;
    *out_revision = s_sessions_revision;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t mosaic_ai_create_runtime_get_sessions_revision(
    uint32_t *out_revision)
{
    if (!out_revision) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_revision = s_sessions_revision;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t mosaic_ai_create_runtime_select_session(size_t index)
{
    if (!s_controller || !s_session_queue) return ESP_ERR_INVALID_STATE;
    ai_create_session_command_t command = {
        .type = AI_CREATE_SESSION_SELECT,
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index >= s_sessions.session_count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(command.alias, s_sessions.sessions[index],
            sizeof(command.alias));
    xSemaphoreGive(s_lock);

    esp_err_t err = ai_create_controller_begin_session_switch(
        s_controller, &command.generation);
    if (err != ESP_OK) return err;
    if (xQueueSend(s_session_queue, &command, 0) == pdTRUE) return ESP_OK;

    ai_create_controller_cancel_session_switch(s_controller);
    (void)ai_create_controller_dispatch(s_controller,
        &(ai_create_command_t) {
            .type = AI_CREATE_COMMAND_OPEN_SESSION_PICKER,
        });
    return ESP_ERR_TIMEOUT;
}

esp_err_t mosaic_ai_create_runtime_confirm_session_delete(void)
{
    if (!s_controller || !s_session_queue) return ESP_ERR_INVALID_STATE;
    ai_create_session_command_t command = {
        .type = AI_CREATE_SESSION_DELETE,
    };
    esp_err_t err = ai_create_controller_begin_session_delete(
        s_controller, command.alias, sizeof(command.alias),
        &command.generation);
    if (err != ESP_OK) return err;
    if (xQueueSend(s_session_queue, &command, 0) == pdTRUE) return ESP_OK;
    ai_create_controller_fail_session_delete(
        s_controller, command.generation, ESP_ERR_TIMEOUT);
    return ESP_ERR_TIMEOUT;
}

void mosaic_ai_create_runtime_begin_ui_update(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (s_ui_update_depth == 0) s_ui_update_task = current;
    if (s_ui_update_task == current) s_ui_update_depth++;
    xSemaphoreGive(s_lock);
}

void mosaic_ai_create_runtime_end_ui_update(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ui_update_task == xTaskGetCurrentTaskHandle() &&
        s_ui_update_depth > 0) {
        s_ui_update_depth--;
        if (s_ui_update_depth == 0) s_ui_update_task = NULL;
    }
    xSemaphoreGive(s_lock);
}
