/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_ai_create.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "claw_agent_mgr.h"
#include "claw_cap.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CAP_IM_AI_CREATE_MAX_REQUESTS 8U
#define CAP_IM_AI_CREATE_LOCK_TIMEOUT_MS 1000U
#define CAP_IM_AI_CREATE_UNSUBSCRIBE_TIMEOUT_MS 1000U
#define CAP_IM_AI_CREATE_REQUEST_TIMEOUT_MS 120000U
#define CAP_IM_AI_CREATE_SUBMIT_TIMEOUT_MS 5000U
#define CAP_IM_AI_CREATE_CHAT_ID_MAX 95U
#define CAP_IM_AI_CREATE_MESSAGE_ID_MAX 95U
#define CAP_IM_AI_CREATE_WORKER_STACK 6144U
#define CAP_IM_AI_CREATE_WORKER_PRIORITY 5U

typedef struct {
    bool used;
    uint32_t frontend_request_id;
    uint32_t run_id;
    uint32_t event_sequence;
    char chat_id[CAP_IM_AI_CREATE_CHAT_ID_MAX + 1U];
    char message_id[CAP_IM_AI_CREATE_MESSAGE_ID_MAX + 1U];
} cap_im_ai_create_pending_t;

typedef struct {
    SemaphoreHandle_t lock;
    bool started;
    cap_im_ai_create_event_cb_t callback;
    void *callback_ctx;
    uint32_t callbacks_in_flight;
    uint32_t message_sequence;
    cap_im_ai_create_pending_t pending[CAP_IM_AI_CREATE_MAX_REQUESTS];
} cap_im_ai_create_state_t;

typedef struct {
    uint32_t frontend_request_id;
    uint32_t run_id;
} cap_im_ai_create_worker_args_t;

static EXT_RAM_BSS_ATTR cap_im_ai_create_state_t s_channel;
static DRAM_ATTR StaticSemaphore_t s_channel_lock_storage;
static portMUX_TYPE s_channel_init_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *cap_im_ai_create_mode_instruction(cap_im_ai_create_mode_t mode)
{
    static const char *const instructions[CAP_IM_AI_CREATE_MODE_COUNT] = {
        [CAP_IM_AI_CREATE_MODE_CREATE] = "Trusted AI Create mode instruction: Before responding, call activate_skill with skill_id \"skill_creator\" and follow the activated Skill.\n\nUser request:\n",
        [CAP_IM_AI_CREATE_MODE_SKILL] = "Trusted AI Create mode instruction: Before responding, call activate_skill with skill_id \"skills_lab_search\" and follow the activated Skill.\n\nUser request:\n",
        [CAP_IM_AI_CREATE_MODE_MEMORY] = "Trusted AI Create mode instruction: Before responding, call activate_skill with skill_id \"memory_ops\" and follow the activated Skill.\n\nUser request:\n",
        [CAP_IM_AI_CREATE_MODE_PLAN] = "Trusted AI Create mode instruction: Before responding, call activate_skill with skill_id \"plan_mode\" and follow the activated Skill.\n\nUser request:\n",
        [CAP_IM_AI_CREATE_MODE_QUICK] = "Trusted AI Create mode instruction: Before responding, call activate_skill with skill_id \"ai_create_quick\" and follow the activated Skill.\n\nUser request:\n",
        [CAP_IM_AI_CREATE_MODE_SCHEDULE] = "Trusted AI Create mode instruction: Before responding, call activate_skill with skill_id \"cap_scheduler\" and follow the activated Skill.\n\nUser request:\n",
    };

    return mode < CAP_IM_AI_CREATE_MODE_COUNT ? instructions[mode] : NULL;
}

static char *cap_im_ai_create_build_agent_user_text(const cap_im_ai_create_request_t *request)
{
    const char *instruction;
    size_t instruction_len;
    size_t text_len;
    char *agent_user_text;

    if (!request || !request->text) {
        return NULL;
    }
    if (!request->inject_mode_instruction) return strdup(request->text);
    instruction = cap_im_ai_create_mode_instruction(request->mode);
    if (!instruction) return NULL;
    instruction_len = strlen(instruction);
    text_len = strlen(request->text);
    if (text_len > SIZE_MAX - instruction_len - 1U) {
        return NULL;
    }
    agent_user_text = malloc(instruction_len + text_len + 1U);
    if (!agent_user_text) {
        return NULL;
    }
    memcpy(agent_user_text, instruction, instruction_len);
    memcpy(agent_user_text + instruction_len, request->text, text_len + 1U);
    return agent_user_text;
}

static esp_err_t channel_lock(void)
{
    SemaphoreHandle_t lock;

    portENTER_CRITICAL(&s_channel_init_mux);
    if (!s_channel.lock) {
        s_channel.lock = xSemaphoreCreateMutexStatic(&s_channel_lock_storage);
    }
    lock = s_channel.lock;
    portEXIT_CRITICAL(&s_channel_init_mux);
    if (!lock) {
        return ESP_ERR_NO_MEM;
    }
    return xSemaphoreTake(lock,
                          pdMS_TO_TICKS(CAP_IM_AI_CREATE_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void channel_unlock(void)
{
    xSemaphoreGive(s_channel.lock);
}

static cap_im_ai_create_pending_t *find_pending_locked(uint32_t frontend_request_id)
{
    for (size_t i = 0; i < CAP_IM_AI_CREATE_MAX_REQUESTS; ++i) {
        if (s_channel.pending[i].used &&
            s_channel.pending[i].frontend_request_id == frontend_request_id) {
            return &s_channel.pending[i];
        }
    }
    return NULL;
}

static cap_im_ai_create_pending_t *reserve_pending_locked(uint32_t frontend_request_id)
{
    if (find_pending_locked(frontend_request_id)) {
        return NULL;
    }
    for (size_t i = 0; i < CAP_IM_AI_CREATE_MAX_REQUESTS; ++i) {
        if (!s_channel.pending[i].used) {
            memset(&s_channel.pending[i], 0, sizeof(s_channel.pending[i]));
            s_channel.pending[i].used = true;
            s_channel.pending[i].frontend_request_id = frontend_request_id;
            return &s_channel.pending[i];
        }
    }
    return NULL;
}

static void clear_pending_locked(cap_im_ai_create_pending_t *pending)
{
    if (pending) {
        memset(pending, 0, sizeof(*pending));
    }
}

static uint32_t next_event_sequence_locked(cap_im_ai_create_pending_t *pending)
{
    pending->event_sequence++;
    if (pending->event_sequence == 0) {
        pending->event_sequence = 1;
    }
    return pending->event_sequence;
}

static void emit_to_ui(const cap_im_ai_create_event_t *event)
{
    cap_im_ai_create_event_cb_t callback = NULL;
    void *callback_ctx = NULL;

    if (channel_lock() != ESP_OK) {
        return;
    }
    callback = s_channel.callback;
    callback_ctx = s_channel.callback_ctx;
    if (callback) {
        s_channel.callbacks_in_flight++;
    }
    channel_unlock();

    if (callback) {
        callback(event, callback_ctx);
        if (channel_lock() == ESP_OK) {
            s_channel.callbacks_in_flight--;
            channel_unlock();
        }
    }
}

static cap_im_ai_create_status_t response_error_status(const char *message,
                                                       bool *retryable)
{
    *retryable = false;
    if (!message) {
        return CAP_IM_AI_CREATE_STATUS_INTERNAL;
    }
    if (strstr(message, "auth") || strstr(message, "credential") ||
        strstr(message, "API key")) {
        return CAP_IM_AI_CREATE_STATUS_AUTH_REQUIRED;
    }
    if (strstr(message, "rate") || strstr(message, "429")) {
        *retryable = true;
        return CAP_IM_AI_CREATE_STATUS_RATE_LIMITED;
    }
    if (strstr(message, "timeout") || strstr(message, "temporar")) {
        *retryable = true;
        return CAP_IM_AI_CREATE_STATUS_TIMEOUT;
    }
    return CAP_IM_AI_CREATE_STATUS_INTERNAL;
}

static const char *agent_phase_text(claw_core_agent_loop_phase_t phase)
{
    switch (phase) {
    case CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_BUILD_ITERATION_CONTEXT:
    case CLAW_CORE_AGENT_LOOP_PHASE_BUILDING_ITERATION_CONTEXT:
        return "Preparing context…";
    case CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_LLM_HTTP:
    case CLAW_CORE_AGENT_LOOP_PHASE_IN_LLM_HTTP:
        return "Thinking…";
    case CLAW_CORE_AGENT_LOOP_PHASE_AFTER_LLM_BEFORE_TOOL:
    case CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL:
        return "Running tools…";
    case CLAW_CORE_AGENT_LOOP_PHASE_FINALIZING:
        return "Preparing results…";
    default:
        return NULL;
    }
}

static void response_worker(void *arg)
{
    cap_im_ai_create_worker_args_t args =
        *(cap_im_ai_create_worker_args_t *)arg;
    claw_core_response_t response = {0};
    cap_im_ai_create_event_t event = {
        .request_id = args.frontend_request_id,
        .run_id = args.run_id,
        .delivery = CAP_IM_AI_CREATE_DELIVERY_NEW_RUN,
    };
    free(arg);

    if (channel_lock() == ESP_OK) {
        cap_im_ai_create_pending_t *pending =
            find_pending_locked(args.frontend_request_id);
        if (pending && pending->run_id == args.run_id) {
            event.sequence = next_event_sequence_locked(pending);
            event.kind = CAP_IM_AI_CREATE_EVENT_AGENT_PROGRESS;
            event.status = CAP_IM_AI_CREATE_STATUS_OK;
            event.text = "Processing…";
        }
        channel_unlock();
    }
    if (event.sequence != 0) {
        emit_to_ui(&event);
    }

    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)CAP_IM_AI_CREATE_REQUEST_TIMEOUT_MS * 1000;
    claw_core_agent_loop_phase_t last_phase = CLAW_CORE_AGENT_LOOP_PHASE_IDLE;
    esp_err_t err;
    do {
        err = claw_agent_mgr_receive_root_for(args.run_id, &response, 1000);
        if (err != ESP_ERR_TIMEOUT || esp_timer_get_time() >= deadline_us) {
            break;
        }
        claw_core_handle_t core = claw_agent_mgr_get_root_core();
        claw_core_agent_loop_phase_t phase = core
            ? claw_core_get_agent_loop_phase(core)
            : CLAW_CORE_AGENT_LOOP_PHASE_IDLE;
        const char *phase_text = phase != last_phase
            ? agent_phase_text(phase) : NULL;
        last_phase = phase;
        if (!phase_text) {
            continue;
        }
        cap_im_ai_create_event_t progress = {
            .request_id = args.frontend_request_id,
            .run_id = args.run_id,
            .kind = CAP_IM_AI_CREATE_EVENT_AGENT_PROGRESS,
            .delivery = CAP_IM_AI_CREATE_DELIVERY_NEW_RUN,
            .status = CAP_IM_AI_CREATE_STATUS_OK,
            .text = phase_text,
        };
        if (channel_lock() == ESP_OK) {
            cap_im_ai_create_pending_t *pending =
                find_pending_locked(args.frontend_request_id);
            if (pending && pending->run_id == args.run_id) {
                progress.sequence = next_event_sequence_locked(pending);
            }
            channel_unlock();
        }
        if (progress.sequence) {
            emit_to_ui(&progress);
        }
    } while (true);

    memset(&event, 0, sizeof(event));
    event.request_id = args.frontend_request_id;
    event.run_id = args.run_id;
    event.delivery = CAP_IM_AI_CREATE_DELIVERY_NEW_RUN;
    event.terminal = true;

    if (channel_lock() != ESP_OK) {
        claw_core_response_free(&response);
        vTaskDelete(NULL);
        return;
    }
    cap_im_ai_create_pending_t *pending =
        find_pending_locked(args.frontend_request_id);
    if (!pending || pending->run_id != args.run_id) {
        channel_unlock();
        claw_core_response_free(&response);
        vTaskDelete(NULL);
        return;
    }
    event.sequence = next_event_sequence_locked(pending);
    clear_pending_locked(pending);
    channel_unlock();

    if (err == ESP_ERR_TIMEOUT) {
        (void)claw_agent_mgr_cancel_root_request(args.run_id);
        event.kind = CAP_IM_AI_CREATE_EVENT_ERROR;
        event.status = CAP_IM_AI_CREATE_STATUS_TIMEOUT;
        event.retryable = true;
        event.text = "AI Create request timed out. Please retry.";
    } else if (err != ESP_OK) {
        event.kind = CAP_IM_AI_CREATE_EVENT_ERROR;
        event.status = err == ESP_ERR_INVALID_STATE
            ? CAP_IM_AI_CREATE_STATUS_UNAVAILABLE
            : CAP_IM_AI_CREATE_STATUS_INTERNAL;
        event.retryable = event.status == CAP_IM_AI_CREATE_STATUS_UNAVAILABLE;
        event.text = "AI Create backend is unavailable";
    } else if (response.status == CLAW_CORE_RESPONSE_STATUS_OK) {
        event.kind = CAP_IM_AI_CREATE_EVENT_RESPONSE_FINAL;
        event.status = CAP_IM_AI_CREATE_STATUS_OK;
        event.text = response.text ? response.text : "";
    } else {
        event.kind = CAP_IM_AI_CREATE_EVENT_ERROR;
        event.status = response_error_status(response.error_message,
                                             &event.retryable);
        event.text = response.error_message
            ? response.error_message : "AI Create request failed";
    }

    emit_to_ui(&event);
    claw_core_response_free(&response);
    vTaskDelete(NULL);
}

static esp_err_t channel_gateway_init(void)
{
    return channel_lock() == ESP_OK ? (channel_unlock(), ESP_OK)
                                    : ESP_ERR_NO_MEM;
}

static esp_err_t channel_gateway_start(void)
{
    esp_err_t err = channel_lock();
    if (err != ESP_OK) {
        return err;
    }
    s_channel.started = true;
    channel_unlock();
    return ESP_OK;
}

static esp_err_t channel_gateway_stop(void)
{
    uint32_t run_ids[CAP_IM_AI_CREATE_MAX_REQUESTS] = {0};
    size_t run_count = 0;
    esp_err_t err = channel_lock();
    if (err != ESP_OK) {
        return err;
    }
    s_channel.started = false;
    for (size_t i = 0; i < CAP_IM_AI_CREATE_MAX_REQUESTS; ++i) {
        if (s_channel.pending[i].used && s_channel.pending[i].run_id) {
            run_ids[run_count++] = s_channel.pending[i].run_id;
        }
        clear_pending_locked(&s_channel.pending[i]);
    }
    channel_unlock();
    for (size_t i = 0; i < run_count; ++i) {
        (void)claw_agent_mgr_cancel_root_request(run_ids[i]);
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_descriptors[] = {
    {
        .id = "ai_create_gateway",
        .name = "ai_create_gateway",
        .family = "im",
        .description = "AI Create local UI event source.",
        .kind = CLAW_CAP_KIND_EVENT_SOURCE,
        .cap_flags = CLAW_CAP_FLAG_EMITS_EVENTS |
                     CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .init = channel_gateway_init,
        .start = channel_gateway_start,
        .stop = channel_gateway_stop,
    },
};

static const claw_cap_group_t s_group = {
    .group_id = "cap_im_ai_create",
    .descriptors = s_descriptors,
    .descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]),
};

esp_err_t cap_im_ai_create_register_group(void)
{
    return claw_cap_group_exists(s_group.group_id) ? ESP_OK
                                                   : claw_cap_register_group(&s_group);
}

esp_err_t cap_im_ai_create_subscribe(cap_im_ai_create_event_cb_t callback,
                                     void *user_ctx)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = channel_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (s_channel.callback &&
        (s_channel.callback != callback || s_channel.callback_ctx != user_ctx)) {
        channel_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_channel.callback = callback;
    s_channel.callback_ctx = user_ctx;
    channel_unlock();
    return ESP_OK;
}

esp_err_t cap_im_ai_create_unsubscribe(cap_im_ai_create_event_cb_t callback,
                                       void *user_ctx)
{
    esp_err_t err = channel_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (s_channel.callback != callback || s_channel.callback_ctx != user_ctx) {
        channel_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    s_channel.callback = NULL;
    s_channel.callback_ctx = NULL;
    channel_unlock();

    int64_t deadline_us = esp_timer_get_time() +
        (int64_t)CAP_IM_AI_CREATE_UNSUBSCRIBE_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline_us) {
        uint32_t in_flight = 0;
        if (channel_lock() == ESP_OK) {
            in_flight = s_channel.callbacks_in_flight;
            channel_unlock();
        }
        if (in_flight == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t cap_im_ai_create_post_message(
    const cap_im_ai_create_request_t *request)
{
    const char *chat_id = request ? request->chat_id : NULL;
    char *agent_user_text;
    if (!request ||
        request->protocol_version != CAP_IM_AI_CREATE_PROTOCOL_VERSION ||
        request->request_id == 0 || !chat_id || !chat_id[0] ||
        !request->text || !request->text[0] ||
        strlen(request->text) > CAP_IM_AI_CREATE_TEXT_MAX_BYTES ||
        request->mode >= CAP_IM_AI_CREATE_MODE_COUNT ||
        request->input > CAP_IM_AI_CREATE_INPUT_VOICE) {
        return ESP_ERR_INVALID_ARG;
    }

    agent_user_text = cap_im_ai_create_build_agent_user_text(request);
    if (!agent_user_text) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = channel_lock();
    if (err != ESP_OK) {
        free(agent_user_text);
        return err;
    }
    if (!s_channel.started) {
        channel_unlock();
        free(agent_user_text);
        return ESP_ERR_INVALID_STATE;
    }
    cap_im_ai_create_pending_t *pending =
        reserve_pending_locked(request->request_id);
    if (!pending) {
        channel_unlock();
        free(agent_user_text);
        return ESP_ERR_NO_MEM;
    }
    uint32_t message_sequence = ++s_channel.message_sequence;
    strlcpy(pending->chat_id, chat_id, sizeof(pending->chat_id));
    snprintf(pending->message_id, sizeof(pending->message_id),
             "ai-create-%" PRIu32 "-%" PRIu32,
             request->request_id, message_sequence);
    char message_id[CAP_IM_AI_CREATE_MESSAGE_ID_MAX + 1U];
    strlcpy(message_id, pending->message_id, sizeof(message_id));
    channel_unlock();

    claw_agent_mgr_root_input_t input = {
        .session_policy = CLAW_SESSION_POLICY_CHAT,
        .user_text = agent_user_text,
        .source_cap = "ai_create_gateway",
        .event_type = "ai_create_message",
        .source_channel = CAP_IM_AI_CREATE_CHANNEL,
        .source_chat_id = chat_id,
        .source_sender_id = "mosaic_user",
        .source_message_id = message_id,
        .event_id = message_id,
        .target_channel = CAP_IM_AI_CREATE_CHANNEL,
        .target_chat_id = chat_id,
    };
    claw_core_message_receipt_t receipt = {0};
    err = claw_agent_mgr_post_root_message(
        &input, CAP_IM_AI_CREATE_SUBMIT_TIMEOUT_MS, &receipt);
    free(agent_user_text);
    if (err != ESP_OK) {
        if (channel_lock() == ESP_OK) {
            pending = find_pending_locked(request->request_id);
            clear_pending_locked(pending);
            channel_unlock();
        }
        emit_to_ui(&(cap_im_ai_create_event_t) {
            .request_id = request->request_id,
            .sequence = 1,
            .kind = CAP_IM_AI_CREATE_EVENT_ERROR,
            .status = CAP_IM_AI_CREATE_STATUS_UNAVAILABLE,
            .terminal = true,
            .retryable = true,
            .text = "AI Create backend is unavailable",
        });
        return err;
    }

    cap_im_ai_create_delivery_t delivery =
        receipt.disposition == CLAW_CORE_MESSAGE_APPENDED_TO_RUN
            ? CAP_IM_AI_CREATE_DELIVERY_ACTIVE_RUN
            : CAP_IM_AI_CREATE_DELIVERY_NEW_RUN;
    uint32_t accepted_sequence = 0;
    if (channel_lock() == ESP_OK) {
        pending = find_pending_locked(request->request_id);
        if (pending) {
            pending->run_id = receipt.run_id;
            accepted_sequence = next_event_sequence_locked(pending);
        }
        channel_unlock();
    }
    if (accepted_sequence == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    emit_to_ui(&(cap_im_ai_create_event_t) {
        .request_id = request->request_id,
        .run_id = receipt.run_id,
        .sequence = accepted_sequence,
        .kind = CAP_IM_AI_CREATE_EVENT_MESSAGE_ACCEPTED,
        .delivery = delivery,
        .status = CAP_IM_AI_CREATE_STATUS_OK,
    });

    if (delivery == CAP_IM_AI_CREATE_DELIVERY_ACTIVE_RUN) {
        if (channel_lock() == ESP_OK) {
            pending = find_pending_locked(request->request_id);
            clear_pending_locked(pending);
            channel_unlock();
        }
        return ESP_OK;
    }

    cap_im_ai_create_worker_args_t *worker_args =
        malloc(sizeof(*worker_args));
    if (!worker_args) {
        err = ESP_ERR_NO_MEM;
    } else {
        *worker_args = (cap_im_ai_create_worker_args_t) {
            .frontend_request_id = request->request_id,
            .run_id = receipt.run_id,
        };
        BaseType_t task_ok = xTaskCreate(
            response_worker, "ai_create_rsp", CAP_IM_AI_CREATE_WORKER_STACK,
            worker_args, CAP_IM_AI_CREATE_WORKER_PRIORITY, NULL);
        if (task_ok != pdPASS) {
            free(worker_args);
            err = ESP_ERR_NO_MEM;
        } else {
            return ESP_OK;
        }
    }

    (void)claw_agent_mgr_cancel_root_request(receipt.run_id);
    uint32_t error_sequence = accepted_sequence + 1U;
    if (channel_lock() == ESP_OK) {
        pending = find_pending_locked(request->request_id);
        if (pending) {
            error_sequence = next_event_sequence_locked(pending);
            clear_pending_locked(pending);
        }
        channel_unlock();
    }
    emit_to_ui(&(cap_im_ai_create_event_t) {
        .request_id = request->request_id,
        .run_id = receipt.run_id,
        .sequence = error_sequence,
        .kind = CAP_IM_AI_CREATE_EVENT_ERROR,
        .status = CAP_IM_AI_CREATE_STATUS_INTERNAL,
        .terminal = true,
        .retryable = true,
        .text = "AI Create could not start the response task",
    });
    return err;
}

esp_err_t cap_im_ai_create_cancel(uint32_t request_id)
{
    if (request_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = channel_lock();
    if (err != ESP_OK) {
        return err;
    }
    cap_im_ai_create_pending_t *pending = find_pending_locked(request_id);
    if (!pending) {
        channel_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t run_id = pending->run_id;
    uint32_t sequence = next_event_sequence_locked(pending);
    clear_pending_locked(pending);
    channel_unlock();

    err = run_id ? claw_agent_mgr_cancel_root_request(run_id) : ESP_OK;
    if (err == ESP_ERR_NOT_FOUND) {
        err = ESP_OK;
    }
    emit_to_ui(&(cap_im_ai_create_event_t) {
        .request_id = request_id,
        .run_id = run_id,
        .sequence = sequence,
        .kind = CAP_IM_AI_CREATE_EVENT_CANCELLED,
        .status = CAP_IM_AI_CREATE_STATUS_CANCELLED,
        .terminal = true,
    });
    return err;
}
