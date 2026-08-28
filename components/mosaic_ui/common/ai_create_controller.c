/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ai_create_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_paths.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#ifdef ESP_PLATFORM
#include "freertos/task.h"
#endif

#define CONTROLLER_JOURNAL_PATH_MAX 256U
#define CONTROLLER_JOURNAL_NAME "ai_create/controller.json"
#define CONTROLLER_MAX_INFLIGHT_MESSAGES 8U
#define CONTROLLER_JOURNAL_TASK_STACK 4096U
#define CONTROLLER_JOURNAL_TASK_PRIORITY 3U

static const char *TAG = "ai_create_ctrl";

typedef struct {
    bool used;
    uint32_t request_id;
    uint32_t run_id;
    uint32_t last_event_sequence;
} controller_message_track_t;

struct ai_create_controller_t {
    SemaphoreHandle_t lock;
    const ai_create_gateway_ops_t *gateway;
    const ai_create_history_ops_t *history;
    void *history_ctx;
    ai_create_voice_port_handle_t voice_port;
    ai_create_controller_changed_cb_t on_changed;
    void *on_changed_ctx;
    bool started;
    bool subscribed;
    bool cancel_on_stop;
    bool persist_journal;
    bool start_with_new_session;
    bool session_creation_in_progress;
    bool session_switch_pending;
    uint32_t session_switch_generation;
    bool session_delete_pending;
    uint32_t session_delete_generation;
#ifdef ESP_PLATFORM
    TaskHandle_t journal_worker;
    SemaphoreHandle_t journal_stopped;
    bool journal_stopping;
#endif
    uint32_t next_request_id;
    uint32_t next_voice_operation_id;
    uint32_t voice_operation_id;
    int64_t voice_deadline_us;
    char pending_session_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    char journal_path[CONTROLLER_JOURNAL_PATH_MAX];
    controller_message_track_t messages[CONTROLLER_MAX_INFLIGHT_MESSAGES];
    ai_create_controller_snapshot_t snapshot;
};

static const char *const s_state_names[] = {
    "idle", "drafting", "recording", "transcribing", "ready",
    "submitting", "running", "completed", "error", "cancelling",
    "cancelled",
};

const char *ai_create_controller_state_name(ai_create_controller_state_t state)
{
    return state <= AI_CREATE_STATE_CANCELLED ? s_state_names[state] : "unknown";
}

static esp_err_t gateway_subscribe(cap_im_ai_create_event_cb_t cb, void *ctx)
{
    return cap_im_ai_create_subscribe(cb, ctx);
}

static esp_err_t gateway_unsubscribe(cap_im_ai_create_event_cb_t cb, void *ctx)
{
    return cap_im_ai_create_unsubscribe(cb, ctx);
}

static const ai_create_gateway_ops_t s_default_gateway = {
    .subscribe = gateway_subscribe,
    .unsubscribe = gateway_unsubscribe,
    .post_message = cap_im_ai_create_post_message,
    .cancel = cap_im_ai_create_cancel,
};

const ai_create_gateway_ops_t *ai_create_controller_default_gateway(void)
{
    return &s_default_gateway;
}

static void notify_changed(ai_create_controller_handle_t controller)
{
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    controller->snapshot.revision++;
    if (controller->snapshot.revision == 0) {
        controller->snapshot.revision = 1;
    }
    xSemaphoreGive(controller->lock);
    if (controller->on_changed) {
        controller->on_changed(controller, controller->on_changed_ctx);
    }
}

static void clear_notice_locked(ai_create_controller_handle_t controller)
{
    controller->snapshot.notice_visible = false;
    controller->snapshot.notice_code = ESP_OK;
    controller->snapshot.notice[0] = '\0';
}

static void transcript_clear_locked(ai_create_controller_handle_t controller)
{
    memset(&controller->snapshot.transcript, 0,
           sizeof(controller->snapshot.transcript));
}

static void transcript_drop_oldest_turn(ai_create_transcript_t *transcript)
{
    if (!transcript || transcript->count == 0) return;

    size_t remove_count = 1U;
    if (transcript->count >= AI_CREATE_CONTROLLER_MESSAGES_PER_TURN &&
        transcript->messages[0].role == AI_CREATE_MESSAGE_USER &&
        transcript->messages[1].role == AI_CREATE_MESSAGE_ASSISTANT) {
        remove_count = AI_CREATE_CONTROLLER_MESSAGES_PER_TURN;
    }
    memmove(&transcript->messages[0],
            &transcript->messages[remove_count],
            (transcript->count - remove_count) *
                sizeof(transcript->messages[0]));
    transcript->count -= remove_count;
    memset(&transcript->messages[transcript->count], 0,
           remove_count * sizeof(transcript->messages[0]));
}

static void transcript_append_locked(ai_create_controller_handle_t controller,
    uint32_t request_id, ai_create_message_role_t role, const char *text,
    bool pending, bool error)
{
    ai_create_transcript_t *transcript = &controller->snapshot.transcript;
    if (transcript->count == AI_CREATE_CONTROLLER_HISTORY_MAX) {
        transcript_drop_oldest_turn(transcript);
    }
    ai_create_message_t *message =
        &transcript->messages[transcript->count++];
    memset(message, 0, sizeof(*message));
    message->request_id = request_id;
    message->role = role;
    message->pending = pending;
    message->error = error;
    strlcpy(message->text, text ? text : "", sizeof(message->text));
}

static void transcript_update_assistant_locked(
    ai_create_controller_handle_t controller, uint32_t request_id,
    const char *text,
    bool pending, bool error)
{
    ai_create_transcript_t *transcript = &controller->snapshot.transcript;
    ai_create_message_t *message = NULL;
    for (size_t i = transcript->count; i > 0; --i) {
        ai_create_message_t *candidate = &transcript->messages[i - 1U];
        if (candidate->role == AI_CREATE_MESSAGE_ASSISTANT &&
            candidate->request_id == request_id) {
            message = candidate;
            break;
        }
    }
    if (!message) {
        transcript_append_locked(controller, request_id,
                                 AI_CREATE_MESSAGE_ASSISTANT, text,
                                 pending, error);
        return;
    }
    message->pending = pending;
    message->error = error;
    strlcpy(message->text, text ? text : "", sizeof(message->text));
}

static controller_message_track_t *find_message_locked(
    ai_create_controller_handle_t controller, uint32_t request_id)
{
    for (size_t i = 0; i < CONTROLLER_MAX_INFLIGHT_MESSAGES; ++i) {
        if (controller->messages[i].used &&
            controller->messages[i].request_id == request_id) {
            return &controller->messages[i];
        }
    }
    return NULL;
}

static controller_message_track_t *reserve_message_locked(
    ai_create_controller_handle_t controller, uint32_t request_id)
{
    if (find_message_locked(controller, request_id)) return NULL;
    for (size_t i = 0; i < CONTROLLER_MAX_INFLIGHT_MESSAGES; ++i) {
        if (!controller->messages[i].used) {
            controller->messages[i] = (controller_message_track_t) {
                .used = true,
                .request_id = request_id,
            };
            return &controller->messages[i];
        }
    }
    return NULL;
}

static controller_message_track_t *find_message_by_run_locked(
    ai_create_controller_handle_t controller, uint32_t run_id,
    const controller_message_track_t *exclude)
{
    if (run_id == 0) return NULL;
    for (size_t i = 0; i < CONTROLLER_MAX_INFLIGHT_MESSAGES; ++i) {
        controller_message_track_t *track = &controller->messages[i];
        if (track != exclude && track->used &&
            track->run_id == run_id) {
            return track;
        }
    }
    return NULL;
}

static void refresh_active_locked(ai_create_controller_handle_t controller)
{
    controller->snapshot.active = false;
    for (size_t i = 0; i < CONTROLLER_MAX_INFLIGHT_MESSAGES; ++i) {
        if (controller->messages[i].used) {
            controller->snapshot.active = true;
            controller->snapshot.request_id =
                controller->messages[i].request_id;
            return;
        }
    }
    controller->snapshot.request_id = 0;
}

static void rebind_transcript_request_locked(
    ai_create_controller_handle_t controller, uint32_t from_request_id,
    uint32_t to_request_id)
{
    for (size_t i = 0; i < controller->snapshot.transcript.count; ++i) {
        if (controller->snapshot.transcript.messages[i].request_id ==
            from_request_id) {
            controller->snapshot.transcript.messages[i].request_id =
                to_request_id;
        }
    }
}

static bool transcript_valid(const ai_create_transcript_t *transcript)
{
    if (!transcript || transcript->count > AI_CREATE_CONTROLLER_HISTORY_MAX) {
        return false;
    }
    if ((transcript->count % 2U) != 0U) return false;
    for (size_t i = 0; i < transcript->count; ++i) {
        const ai_create_message_role_t expected =
            (i % 2U) == 0U ? AI_CREATE_MESSAGE_USER
                           : AI_CREATE_MESSAGE_ASSISTANT;
        if (transcript->messages[i].role != expected) {
            return false;
        }
    }
    return true;
}

static esp_err_t reload_history_internal(
    ai_create_controller_handle_t controller, bool notify)
{
    if (!controller || !controller->history || !controller->history->load) {
        return ESP_OK;
    }
    char chat_id[AI_CREATE_CONTROLLER_CHAT_ID_MAX + 1U];
    char alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->snapshot.new_session_pending) {
        transcript_clear_locked(controller);
        xSemaphoreGive(controller->lock);
        if (notify) notify_changed(controller);
        return ESP_OK;
    }
    strlcpy(chat_id, controller->snapshot.chat_id, sizeof(chat_id));
    strlcpy(alias, controller->snapshot.session_alias, sizeof(alias));
    xSemaphoreGive(controller->lock);

    ai_create_transcript_t *loaded = calloc(1, sizeof(*loaded));
    if (!loaded) return ESP_ERR_NO_MEM;
    esp_err_t err = controller->history->load(
        controller->history_ctx, chat_id, alias, loaded);
    if (err == ESP_ERR_NOT_FOUND) {
        free(loaded);
        return ESP_OK;
    }
    if (err == ESP_OK && !transcript_valid(loaded)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        for (size_t i = 0; i < loaded->count; ++i) {
            loaded->messages[i].text[AI_CREATE_CONTROLLER_MESSAGE_MAX] = '\0';
        }
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        if (!controller->snapshot.active &&
            strcmp(controller->snapshot.chat_id, chat_id) == 0 &&
            strcmp(controller->snapshot.session_alias, alias) == 0) {
            controller->snapshot.transcript = *loaded;
        }
        xSemaphoreGive(controller->lock);
    }
    free(loaded);
    if (err == ESP_OK && notify) notify_changed(controller);
    return err;
}

static void set_notice(ai_create_controller_handle_t controller,
                       esp_err_t error, const char *message)
{
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    controller->snapshot.notice_visible = true;
    controller->snapshot.notice_code = error;
    strlcpy(controller->snapshot.notice,
            message && message[0] ? message : esp_err_to_name(error),
            sizeof(controller->snapshot.notice));
    xSemaphoreGive(controller->lock);
    notify_changed(controller);
}

static void sync_current_session_alias(
    ai_create_controller_handle_t controller)
{
    claw_session_mgr_alias_map_t *sessions = malloc(sizeof(*sessions));
    if (!sessions) return;
    char chat_id[AI_CREATE_CONTROLLER_CHAT_ID_MAX + 1U];
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->snapshot.new_session_pending) {
        xSemaphoreGive(controller->lock);
        free(sessions);
        return;
    }
    strlcpy(chat_id, controller->snapshot.chat_id, sizeof(chat_id));
    xSemaphoreGive(controller->lock);
    if (claw_session_mgr_list_chat_sessions(
            0, CAP_IM_AI_CREATE_CHANNEL, chat_id, sessions) == ESP_OK &&
        sessions->current_alias[0]) {
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        if (!controller->snapshot.active &&
            !controller->snapshot.new_session_pending &&
            strcmp(controller->snapshot.chat_id, chat_id) == 0) {
            strlcpy(controller->snapshot.session_alias,
                    sessions->current_alias,
                    sizeof(controller->snapshot.session_alias));
        }
        xSemaphoreGive(controller->lock);
    }
    free(sessions);
}

static void write_journal_now(ai_create_controller_handle_t controller)
{
    if (!controller->persist_journal || !controller->journal_path[0]) return;
    ai_create_controller_snapshot_t *copy = malloc(sizeof(*copy));
    if (!copy) return;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    *copy = controller->snapshot;
    xSemaphoreGive(controller->lock);
    cJSON *root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddNumberToObject(root, "schema_version", 1) ||
        !cJSON_AddStringToObject(root, "state", ai_create_controller_state_name(copy->state)) ||
        !cJSON_AddNumberToObject(root, "mode", copy->mode) ||
        !cJSON_AddNumberToObject(root, "request_id", copy->request_id) ||
        !cJSON_AddStringToObject(root, "chat_id", copy->chat_id) ||
        !cJSON_AddStringToObject(root, "session_alias", copy->session_alias) ||
        !cJSON_AddBoolToObject(root, "new_session_pending",
                              copy->new_session_pending) ||
        !cJSON_AddStringToObject(root, "input", copy->input) ||
        !cJSON_AddStringToObject(root, "response", copy->response)) {
        cJSON_Delete(root);
        free(copy);
        return;
    }
    free(copy);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    char *temp = malloc(CONTROLLER_JOURNAL_PATH_MAX);
    if (!temp || snprintf(temp, CONTROLLER_JOURNAL_PATH_MAX, "%s.tmp",
                          controller->journal_path) >=
                     (int)CONTROLLER_JOURNAL_PATH_MAX) {
        free(temp);
        free(json);
        return;
    }
    FILE *file = fopen(temp, "wb");
    if (file) {
        size_t len = strlen(json);
        bool ok = fwrite(json, 1, len, file) == len && fflush(file) == 0;
        ok = fclose(file) == 0 && ok;
        if (ok) (void)rename(temp, controller->journal_path);
        else (void)remove(temp);
    }
    free(temp);
    free(json);
}

#ifdef ESP_PLATFORM
static void journal_worker(void *arg)
{
    ai_create_controller_handle_t controller = arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        const bool stopping = controller->journal_stopping;
        xSemaphoreGive(controller->lock);
        write_journal_now(controller);
        if (stopping) break;
    }
    controller->journal_worker = NULL;
    xSemaphoreGive(controller->journal_stopped);
    vTaskDelete(NULL);
}
#endif

static void schedule_journal(ai_create_controller_handle_t controller)
{
    if (!controller || !controller->persist_journal) return;
#ifdef ESP_PLATFORM
    if (controller->journal_worker) {
        xTaskNotifyGive(controller->journal_worker);
        return;
    }
#endif
    /* Host tests do not provide a scheduler; their journal is disabled in
     * normal use and retaining the synchronous fallback keeps the controller
     * portable. */
    write_journal_now(controller);
}

static ai_create_controller_state_t parse_journal_state(const char *value)
{
    for (size_t i = 0; i < sizeof(s_state_names) / sizeof(s_state_names[0]); ++i) {
        if (value && strcmp(value, s_state_names[i]) == 0) {
            return (ai_create_controller_state_t)i;
        }
    }
    return AI_CREATE_STATE_IDLE;
}

static void load_journal(ai_create_controller_handle_t controller)
{
    if (!controller->persist_journal || !controller->journal_path[0]) return;
    FILE *file = fopen(controller->journal_path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return;
    }
    long length = ftell(file);
    if (length <= 0 || length > 16 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return;
    }
    char *data = malloc((size_t)length + 1U);
    if (!data) {
        fclose(file);
        return;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return;
    }
    data[length] = '\0';
    fclose(file);
    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *chat_id = cJSON_GetObjectItemCaseSensitive(root, "chat_id");
    const cJSON *alias = cJSON_GetObjectItemCaseSensitive(root, "session_alias");
    const cJSON *new_session_pending =
        cJSON_GetObjectItemCaseSensitive(root, "new_session_pending");
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
    const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
    ai_create_controller_state_t restored = cJSON_IsString(state)
        ? parse_journal_state(state->valuestring) : AI_CREATE_STATE_IDLE;
    if (cJSON_IsNumber(mode) && mode->valueint >= 0 &&
        mode->valueint < CAP_IM_AI_CREATE_MODE_COUNT) {
        controller->snapshot.mode = (cap_im_ai_create_mode_t)mode->valueint;
    }
    if (cJSON_IsNumber(request_id) && request_id->valuedouble > 0 &&
        request_id->valuedouble <= UINT32_MAX) {
        controller->snapshot.request_id = (uint32_t)request_id->valuedouble;
        controller->next_request_id = controller->snapshot.request_id + 1U;
        if (controller->next_request_id == 0) controller->next_request_id = 1;
    }
    if (cJSON_IsString(chat_id) && chat_id->valuestring[0]) {
        strlcpy(controller->snapshot.chat_id, chat_id->valuestring,
                sizeof(controller->snapshot.chat_id));
    }
    if (cJSON_IsString(alias) && alias->valuestring[0]) {
        strlcpy(controller->snapshot.session_alias, alias->valuestring,
                sizeof(controller->snapshot.session_alias));
    }
    controller->snapshot.new_session_pending =
        cJSON_IsTrue(new_session_pending);
    if (cJSON_IsString(input)) {
        strlcpy(controller->snapshot.input, input->valuestring,
                sizeof(controller->snapshot.input));
    }
    if (cJSON_IsString(response)) {
        strlcpy(controller->snapshot.response, response->valuestring,
                sizeof(controller->snapshot.response));
    }
    const bool legacy_approval = cJSON_IsString(state) &&
        strcmp(state->valuestring, "awaiting_approval") == 0;
    bool interrupted = restored == AI_CREATE_STATE_SUBMITTING ||
        restored == AI_CREATE_STATE_RUNNING ||
        restored == AI_CREATE_STATE_CANCELLING || legacy_approval;
    controller->snapshot.state = interrupted ? AI_CREATE_STATE_ERROR : restored;
    controller->snapshot.active = false;
    controller->snapshot.terminal = controller->snapshot.request_id != 0;
    controller->snapshot.retryable = interrupted || restored == AI_CREATE_STATE_ERROR;
    if (interrupted) {
        controller->snapshot.status = CAP_IM_AI_CREATE_STATUS_UNAVAILABLE;
        strlcpy(controller->snapshot.error,
                "The previous request was interrupted. You can retry it.",
                sizeof(controller->snapshot.error));
    }
    transcript_clear_locked(controller);
    if (controller->snapshot.input[0]) {
        transcript_append_locked(controller, controller->snapshot.request_id,
                                 AI_CREATE_MESSAGE_USER,
                                 controller->snapshot.input, false, false);
    }
    if (controller->snapshot.response[0] || controller->snapshot.error[0]) {
        transcript_append_locked(
            controller, controller->snapshot.request_id,
            AI_CREATE_MESSAGE_ASSISTANT,
            controller->snapshot.response[0] ? controller->snapshot.response
                                             : controller->snapshot.error,
            false, controller->snapshot.error[0] != '\0');
    }
    cJSON_Delete(root);
}

static void reset_session_view_locked(
    ai_create_controller_handle_t controller)
{
    controller->snapshot.state = AI_CREATE_STATE_DRAFTING;
    controller->snapshot.screen = AI_CREATE_SCREEN_WELCOME;
    controller->snapshot.composer = AI_CREATE_COMPOSER_DEFAULT;
    controller->snapshot.mode = CAP_IM_AI_CREATE_MODE_QUICK;
    controller->snapshot.status = CAP_IM_AI_CREATE_STATUS_OK;
    controller->snapshot.request_id = 0;
    controller->snapshot.active = false;
    controller->snapshot.retryable = false;
    controller->snapshot.terminal = false;
    controller->snapshot.input[0] = '\0';
    controller->snapshot.response[0] = '\0';
    controller->snapshot.progress[0] = '\0';
    controller->snapshot.error[0] = '\0';
    memset(controller->messages, 0, sizeof(controller->messages));
    transcript_clear_locked(controller);
    clear_notice_locked(controller);
    controller->snapshot.session_delete_confirm_visible = false;
    controller->snapshot.session_deleting = false;
    controller->snapshot.delete_session_alias[0] = '\0';
}

static void cancel_session_switch_locked(
    ai_create_controller_handle_t controller)
{
    controller->session_switch_generation++;
    if (controller->session_switch_generation == 0) {
        controller->session_switch_generation = 1;
    }
    controller->session_switch_pending = false;
    controller->snapshot.session_loading = false;
}

static void clear_session_delete_prompt_locked(
    ai_create_controller_handle_t controller)
{
    controller->snapshot.session_delete_confirm_visible = false;
    controller->snapshot.session_deleting = false;
    controller->snapshot.delete_session_alias[0] = '\0';
}

static esp_err_t prepare_new_session(
    ai_create_controller_handle_t controller, const char *requested_alias,
    bool notify)
{
    if (!controller ||
        (requested_alias && requested_alias[0] &&
         !claw_session_mgr_alias_is_valid(requested_alias))) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->snapshot.active || controller->voice_operation_id != 0 ||
        controller->session_creation_in_progress ||
        controller->session_delete_pending) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    cancel_session_switch_locked(controller);
    reset_session_view_locked(controller);
    controller->snapshot.state = AI_CREATE_STATE_IDLE;
    controller->snapshot.session_alias[0] = '\0';
    controller->snapshot.new_session_pending = true;
    strlcpy(controller->pending_session_alias,
            requested_alias && requested_alias[0] ? requested_alias : "",
            sizeof(controller->pending_session_alias));
    xSemaphoreGive(controller->lock);

    schedule_journal(controller);
    if (notify) notify_changed(controller);
    return ESP_OK;
}

static esp_err_t bind_session_for_submit(
    ai_create_controller_handle_t controller)
{
    char chat_id[AI_CREATE_CONTROLLER_CHAT_ID_MAX + 1U];
    char requested_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    char current_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    bool create_new = false;

    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (!controller->started || controller->snapshot.active ||
        controller->session_creation_in_progress) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    create_new = controller->snapshot.new_session_pending;
    strlcpy(chat_id, controller->snapshot.chat_id, sizeof(chat_id));
    strlcpy(requested_alias, controller->pending_session_alias,
            sizeof(requested_alias));
    strlcpy(current_alias, controller->snapshot.session_alias,
            sizeof(current_alias));
    controller->session_creation_in_progress = true;
    xSemaphoreGive(controller->lock);

    esp_err_t err;
    if (create_new) {
        const bool has_requested_alias = requested_alias[0] != '\0';
        err = claw_session_mgr_new_chat_session(
            0, CAP_IM_AI_CREATE_CHANNEL, chat_id,
            has_requested_alias ? requested_alias : NULL,
            has_requested_alias, current_alias, sizeof(current_alias));
    } else if (current_alias[0]) {
        char resolved[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
        err = claw_session_mgr_switch_chat_session(
            0, CAP_IM_AI_CREATE_CHANNEL, chat_id, current_alias,
            resolved, sizeof(resolved));
        if (err == ESP_OK) {
            strlcpy(current_alias, resolved, sizeof(current_alias));
        }
    } else {
        err = ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (err == ESP_OK && !controller->snapshot.active &&
        strcmp(controller->snapshot.chat_id, chat_id) == 0 &&
        controller->snapshot.new_session_pending == create_new) {
        strlcpy(controller->snapshot.session_alias, current_alias,
                sizeof(controller->snapshot.session_alias));
        controller->snapshot.new_session_pending = false;
        controller->pending_session_alias[0] = '\0';
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        controller->session_creation_in_progress = false;
    }
    xSemaphoreGive(controller->lock);

    if (err != ESP_OK) {
        set_notice(controller, err,
                   "Could not create or restore the session. Try again later.");
    }
    return err;
}

static ai_create_controller_state_t state_for_event(
    cap_im_ai_create_event_kind_t kind)
{
    switch (kind) {
    case CAP_IM_AI_CREATE_EVENT_MESSAGE_ACCEPTED:
        return AI_CREATE_STATE_RUNNING;
    case CAP_IM_AI_CREATE_EVENT_AGENT_PROGRESS:
    case CAP_IM_AI_CREATE_EVENT_RESPONSE_PARTIAL: return AI_CREATE_STATE_RUNNING;
    case CAP_IM_AI_CREATE_EVENT_RESPONSE_FINAL: return AI_CREATE_STATE_COMPLETED;
    case CAP_IM_AI_CREATE_EVENT_ERROR: return AI_CREATE_STATE_ERROR;
    case CAP_IM_AI_CREATE_EVENT_CANCELLED: return AI_CREATE_STATE_CANCELLED;
    default: return AI_CREATE_STATE_ERROR;
    }
}

static void gateway_event(const cap_im_ai_create_event_t *event, void *user_ctx)
{
    ai_create_controller_handle_t controller = user_ctx;
    if (!controller || !event) return;
    bool accepted = false;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    controller_message_track_t *track = find_message_locked(
        controller, event->request_id);
    if (controller->started && track &&
        event->sequence > track->last_event_sequence) {
        track->last_event_sequence = event->sequence;
        controller->snapshot.state = state_for_event(event->kind);
        controller->snapshot.status = event->status;
        controller->snapshot.retryable = event->retryable;
        if (event->kind == CAP_IM_AI_CREATE_EVENT_MESSAGE_ACCEPTED) {
            track->run_id = event->run_id;
            if (event->delivery == CAP_IM_AI_CREATE_DELIVERY_ACTIVE_RUN) {
                controller_message_track_t *run_owner =
                    find_message_by_run_locked(controller,
                                               event->run_id, track);
                if (!run_owner) {
                    xSemaphoreGive(controller->lock);
                    return;
                }
                rebind_transcript_request_locked(
                    controller, event->request_id, run_owner->request_id);
                memset(track, 0, sizeof(*track));
            }
        } else if (event->terminal) {
            memset(track, 0, sizeof(*track));
        }
        refresh_active_locked(controller);
        controller->snapshot.terminal = !controller->snapshot.active;
        if (event->terminal && controller->snapshot.active) {
            controller->snapshot.state = AI_CREATE_STATE_RUNNING;
        }
        if (event->kind == CAP_IM_AI_CREATE_EVENT_RESPONSE_PARTIAL ||
            event->kind == CAP_IM_AI_CREATE_EVENT_RESPONSE_FINAL) {
            strlcpy(controller->snapshot.response, event->text ? event->text : "",
                    sizeof(controller->snapshot.response));
            transcript_update_assistant_locked(
                controller, event->request_id,
                event->text ? event->text : "",
                event->kind == CAP_IM_AI_CREATE_EVENT_RESPONSE_PARTIAL,
                false);
        } else if (event->kind == CAP_IM_AI_CREATE_EVENT_ERROR) {
            strlcpy(controller->snapshot.error,
                    event->text ? event->text : "Request failed",
                    sizeof(controller->snapshot.error));
            transcript_update_assistant_locked(
                controller, event->request_id,
                event->text ? event->text : "Request failed",
                false, true);
        } else if (event->kind !=
                       CAP_IM_AI_CREATE_EVENT_MESSAGE_ACCEPTED &&
                   event->text) {
            strlcpy(controller->snapshot.progress, event->text,
                    sizeof(controller->snapshot.progress));
            transcript_update_assistant_locked(controller,
                                                event->request_id,
                                                event->text,
                                                !event->terminal, false);
        }
        accepted = true;
    }
    xSemaphoreGive(controller->lock);
    if (!accepted) return;
    if (event->terminal) schedule_journal(controller);
    notify_changed(controller);
}

static void voice_failure_locked(ai_create_controller_handle_t controller,
                                 esp_err_t error, const char *message)
{
    controller->voice_operation_id = 0;
    controller->voice_deadline_us = 0;
    controller->snapshot.voice_operation_id = 0;
    /* Voice failures are terminal for the overlay. Keep the error as a
     * retryable notice on the underlying page instead of trapping the user
     * in a modal voice state. */
    controller->snapshot.voice_state = AI_CREATE_VOICE_STATE_IDLE;
    controller->snapshot.status = CAP_IM_AI_CREATE_STATUS_UNAVAILABLE;
    controller->snapshot.retryable = true;
    strlcpy(controller->snapshot.error,
            message && message[0] ? message :
                "Speech recognition failed. Please retry.",
            sizeof(controller->snapshot.error));
    controller->snapshot.notice_visible = true;
    controller->snapshot.notice_code = error;
    strlcpy(controller->snapshot.notice, controller->snapshot.error,
            sizeof(controller->snapshot.notice));
}

static int64_t voice_timeout_for_state(ai_create_voice_state_t state)
{
    switch (state) {
    case AI_CREATE_VOICE_STATE_PREPARING:
        return AI_CREATE_CONTROLLER_VOICE_PREPARING_TIMEOUT_US;
    case AI_CREATE_VOICE_STATE_LISTENING:
        return AI_CREATE_CONTROLLER_VOICE_LISTENING_TIMEOUT_US;
    case AI_CREATE_VOICE_STATE_FINALIZING:
        return AI_CREATE_CONTROLLER_VOICE_FINALIZING_TIMEOUT_US;
    default:
        return 0;
    }
}

static void set_voice_deadline_locked(
    ai_create_controller_handle_t controller, ai_create_voice_state_t state,
    int64_t now_us)
{
    const int64_t timeout_us = voice_timeout_for_state(state);
    controller->voice_deadline_us = timeout_us > 0 && now_us > 0
        ? now_us + timeout_us : 0;
}

static esp_err_t cancel_voice_operation(
    ai_create_controller_handle_t controller, uint32_t expected_operation_id,
    esp_err_t reason, const char *message)
{
    uint32_t operation_id = 0;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    operation_id = controller->voice_operation_id;
    if (operation_id == 0) {
        xSemaphoreGive(controller->lock);
        return ESP_OK;
    }
    if (expected_operation_id != 0 &&
        operation_id != expected_operation_id) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    controller->voice_operation_id = 0;
    controller->voice_deadline_us = 0;
    controller->snapshot.voice_operation_id = 0;
    controller->snapshot.voice_state = AI_CREATE_VOICE_STATE_IDLE;
    if (reason != ESP_OK) {
        controller->snapshot.status = reason == ESP_ERR_TIMEOUT
            ? CAP_IM_AI_CREATE_STATUS_TIMEOUT
            : CAP_IM_AI_CREATE_STATUS_UNAVAILABLE;
        controller->snapshot.retryable = true;
        strlcpy(controller->snapshot.error,
                message && message[0] ? message :
                    "Speech recognition was cancelled.",
                sizeof(controller->snapshot.error));
        controller->snapshot.notice_visible = true;
        controller->snapshot.notice_code = reason;
        strlcpy(controller->snapshot.notice, controller->snapshot.error,
                sizeof(controller->snapshot.notice));
    }
    xSemaphoreGive(controller->lock);
    notify_changed(controller);

    esp_err_t err = controller->voice_port
        ? ai_create_voice_port_cancel(controller->voice_port, operation_id)
        : ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "voice cancel enqueue failed op=%lu: %s",
                 (unsigned long)operation_id, esp_err_to_name(err));
    }
    return err;
}

static void voice_event(ai_create_voice_port_handle_t port,
                        const ai_create_voice_event_t *event, void *user_ctx)
{
    (void)port;
    ai_create_controller_handle_t controller = user_ctx;
    if (!controller || !event) return;
    bool accepted = false;
    bool submit = false;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->started && event->operation_id != 0 &&
        event->operation_id == controller->voice_operation_id) {
        accepted = true;
        controller->snapshot.voice_status = event->status;
        if (event->type == AI_CREATE_VOICE_EVENT_STARTED) {
            if (controller->snapshot.voice_state ==
                AI_CREATE_VOICE_STATE_PREPARING) {
                controller->snapshot.voice_state =
                    AI_CREATE_VOICE_STATE_LISTENING;
                set_voice_deadline_locked(
                    controller, AI_CREATE_VOICE_STATE_LISTENING,
                    esp_timer_get_time());
            }
        } else if (event->type == AI_CREATE_VOICE_EVENT_TRANSCRIPT) {
            if (event->text) {
                strlcpy(controller->snapshot.input, event->text,
                        sizeof(controller->snapshot.input));
            }
        } else if (event->type == AI_CREATE_VOICE_EVENT_ERROR) {
            voice_failure_locked(controller, event->status.error, NULL);
            controller->snapshot.retryable = event->status.retryable;
            /* Presenter maps the structured readiness to recovery copy. */
            controller->snapshot.notice[0] = '\0';
        } else if (event->type == AI_CREATE_VOICE_EVENT_COMPLETED) {
            controller->voice_operation_id = 0;
            controller->voice_deadline_us = 0;
            controller->snapshot.voice_operation_id = 0;
            if (!event->send) {
                if (event->status.error == ESP_OK) {
                    controller->snapshot.voice_state =
                        AI_CREATE_VOICE_STATE_IDLE;
                }
            } else if (event->status.error == ESP_OK && event->text &&
                       event->text[0]) {
                strlcpy(controller->snapshot.input, event->text,
                        sizeof(controller->snapshot.input));
                controller->snapshot.voice_state =
                    AI_CREATE_VOICE_STATE_IDLE;
                submit = true;
            } else {
                voice_failure_locked(
                    controller, event->status.error,
                    event->status.error == ESP_ERR_NOT_FOUND ||
                            !event->text || !event->text[0]
                        ? "No speech was detected. Please retry."
                        : "Speech recognition could not finish. Please retry.");
            }
        }
    }
    xSemaphoreGive(controller->lock);
    if (!accepted) return;
    notify_changed(controller);
    if (submit) {
        (void)ai_create_controller_post_text(controller, event->text);
    }
}

static ai_create_voice_status_t normalized_voice_status(
    ai_create_voice_port_handle_t port, ai_create_voice_status_t fallback)
{
    if (port) {
        ai_create_voice_status_t status;
        if (ai_create_voice_port_get_status(port, &status) == ESP_OK) {
            return status;
        }
    }
    if (fallback.readiness > AI_CREATE_VOICE_READINESS_PROVIDER_ERROR) {
        return ai_create_voice_status_disabled();
    }
    if (!port && fallback.readiness == AI_CREATE_VOICE_READINESS_DISABLED &&
        fallback.error == ESP_OK) {
        return ai_create_voice_status_disabled();
    }
    return fallback;
}

esp_err_t ai_create_controller_create(const ai_create_controller_config_t *config,
    ai_create_controller_handle_t *ret_controller)
{
    if (!config || !ret_controller || !config->chat_id || !config->chat_id[0])
        return ESP_ERR_INVALID_ARG;
    *ret_controller = NULL;
    ai_create_controller_handle_t controller = calloc(1, sizeof(*controller));
    if (!controller) return ESP_ERR_NO_MEM;
    controller->lock = xSemaphoreCreateMutex();
    if (!controller->lock) {
        free(controller);
        return ESP_ERR_NO_MEM;
    }
    controller->gateway = config->gateway ? config->gateway : &s_default_gateway;
    if (!controller->gateway->subscribe || !controller->gateway->unsubscribe ||
        !controller->gateway->post_message || !controller->gateway->cancel) {
        ai_create_controller_delete(controller);
        return ESP_ERR_INVALID_ARG;
    }
    if (config->history && !config->history->load) {
        ai_create_controller_delete(controller);
        return ESP_ERR_INVALID_ARG;
    }
    controller->history = config->history;
    controller->history_ctx = config->history_ctx;
    controller->voice_port = config->voice_port;
    controller->cancel_on_stop = config->cancel_on_stop;
    controller->persist_journal = config->persist_journal;
    controller->start_with_new_session =
        !config->session_alias || !config->session_alias[0];
    controller->on_changed = config->on_changed;
    controller->on_changed_ctx = config->on_changed_ctx;
    controller->next_request_id = 1;
    controller->next_voice_operation_id = 1;
    controller->session_switch_generation = 1;
    controller->session_delete_generation = 1;
    controller->snapshot.state = AI_CREATE_STATE_IDLE;
    controller->snapshot.voice_state = AI_CREATE_VOICE_STATE_IDLE;
    controller->snapshot.mode = CAP_IM_AI_CREATE_MODE_QUICK;
    controller->snapshot.screen = AI_CREATE_SCREEN_WELCOME;
    controller->snapshot.composer = AI_CREATE_COMPOSER_DEFAULT;
    strlcpy(controller->snapshot.chat_id, config->chat_id,
            sizeof(controller->snapshot.chat_id));
    if (controller->start_with_new_session) {
        controller->snapshot.new_session_pending = true;
    } else {
        strlcpy(controller->snapshot.session_alias, config->session_alias,
                sizeof(controller->snapshot.session_alias));
    }
    if (config->persist_journal) {
        (void)claw_paths_join(CLAW_PATH_DATA, CONTROLLER_JOURNAL_NAME,
                              controller->journal_path,
                              sizeof(controller->journal_path));
        load_journal(controller);
    }
    if (controller->start_with_new_session) {
        reset_session_view_locked(controller);
        controller->snapshot.session_alias[0] = '\0';
        controller->snapshot.new_session_pending = true;
        controller->pending_session_alias[0] = '\0';
    }
    controller->snapshot.voice_status = normalized_voice_status(
        config->voice_port, config->voice_status);
    esp_err_t err = config->voice_port
        ? ai_create_voice_port_register_cb(config->voice_port, voice_event,
                                           controller)
        : ESP_OK;
    if (err != ESP_OK) {
        vSemaphoreDelete(controller->lock);
        free(controller);
        return err;
    }
#ifdef ESP_PLATFORM
    if (controller->persist_journal) {
        controller->journal_stopped = xSemaphoreCreateBinary();
        if (!controller->journal_stopped ||
            xTaskCreate(journal_worker, "ai_journal",
                        CONTROLLER_JOURNAL_TASK_STACK, controller,
                        CONTROLLER_JOURNAL_TASK_PRIORITY,
                        &controller->journal_worker) != pdPASS) {
            if (controller->journal_stopped) {
                vSemaphoreDelete(controller->journal_stopped);
            }
            if (controller->voice_port) {
                (void)ai_create_voice_port_register_cb(
                    controller->voice_port, NULL, NULL);
            }
            vSemaphoreDelete(controller->lock);
            free(controller);
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    if (!controller->snapshot.new_session_pending) {
        (void)reload_history_internal(controller, false);
    }
    *ret_controller = controller;
    return ESP_OK;
}

void ai_create_controller_delete(ai_create_controller_handle_t controller)
{
    if (!controller) return;
    (void)ai_create_controller_stop(controller);
#ifdef ESP_PLATFORM
    if (controller->journal_worker) {
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        controller->journal_stopping = true;
        TaskHandle_t worker = controller->journal_worker;
        xSemaphoreGive(controller->lock);
        xTaskNotifyGive(worker);
        xSemaphoreTake(controller->journal_stopped, portMAX_DELAY);
    }
    if (controller->journal_stopped) {
        vSemaphoreDelete(controller->journal_stopped);
    }
#endif
    if (controller->voice_port) {
        (void)ai_create_voice_port_register_cb(
            controller->voice_port, NULL, NULL);
    }
    vSemaphoreDelete(controller->lock);
    free(controller);
}

esp_err_t ai_create_controller_start(ai_create_controller_handle_t controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->started) {
        xSemaphoreGive(controller->lock);
        return ESP_OK;
    }
    xSemaphoreGive(controller->lock);
    esp_err_t err = controller->gateway->subscribe(gateway_event, controller);
    if (err != ESP_OK) return err;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    controller->started = true;
    controller->subscribed = true;
    xSemaphoreGive(controller->lock);
    if (controller->start_with_new_session) {
        (void)prepare_new_session(controller, NULL, false);
    } else {
        sync_current_session_alias(controller);
        (void)reload_history_internal(controller, false);
    }
    notify_changed(controller);
    return ESP_OK;
}

esp_err_t ai_create_controller_stop(ai_create_controller_handle_t controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (!controller->started) {
        xSemaphoreGive(controller->lock);
        return ESP_OK;
    }
    const bool cancel = controller->cancel_on_stop;
    controller->started = false;
    cancel_session_switch_locked(controller);
    xSemaphoreGive(controller->lock);

    if (cancel) {
        (void)ai_create_controller_abort_current(controller);
    } else {
        (void)ai_create_controller_voice_cancel(controller);
    }

    esp_err_t err = controller->gateway->unsubscribe(gateway_event, controller);
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    controller->subscribed = false;
    xSemaphoreGive(controller->lock);
    schedule_journal(controller);
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

esp_err_t ai_create_controller_set_voice_port(
    ai_create_controller_handle_t controller,
    ai_create_voice_port_handle_t voice_port,
    ai_create_voice_status_t fallback_status)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    ai_create_voice_status_t status = normalized_voice_status(
        voice_port, fallback_status);
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->voice_operation_id != 0) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    ai_create_voice_port_handle_t previous = controller->voice_port;
    if (previous == voice_port) {
        controller->snapshot.voice_status = status;
        xSemaphoreGive(controller->lock);
        notify_changed(controller);
        return ESP_OK;
    }
    xSemaphoreGive(controller->lock);

    esp_err_t err = voice_port
        ? ai_create_voice_port_register_cb(voice_port, voice_event, controller)
        : ESP_OK;
    if (err != ESP_OK) return err;

    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->voice_operation_id != 0 ||
        controller->voice_port != previous) {
        xSemaphoreGive(controller->lock);
        if (voice_port) {
            (void)ai_create_voice_port_register_cb(voice_port, NULL, NULL);
        }
        return ESP_ERR_INVALID_STATE;
    }
    controller->voice_port = voice_port;
    controller->snapshot.voice_status = status;
    if (voice_port &&
        controller->snapshot.notice_code == ESP_ERR_NOT_SUPPORTED) {
        clear_notice_locked(controller);
    }
    xSemaphoreGive(controller->lock);
    if (previous) {
        (void)ai_create_voice_port_register_cb(previous, NULL, NULL);
    }
    notify_changed(controller);
    return ESP_OK;
}

esp_err_t ai_create_controller_set_mode(ai_create_controller_handle_t controller,
    cap_im_ai_create_mode_t mode)
{
    if (!controller || mode >= CAP_IM_AI_CREATE_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->snapshot.active || controller->snapshot.session_loading ||
        controller->session_delete_pending) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    controller->snapshot.mode = mode;
    controller->snapshot.state = AI_CREATE_STATE_DRAFTING;
    controller->snapshot.composer = AI_CREATE_COMPOSER_MODE;
    clear_notice_locked(controller);
    xSemaphoreGive(controller->lock);
    notify_changed(controller);
    return ESP_OK;
}

esp_err_t ai_create_controller_set_session(ai_create_controller_handle_t controller,
    const char *chat_id, const char *session_alias)
{
    if (!controller || !chat_id || !chat_id[0] ||
        strlen(chat_id) > AI_CREATE_CONTROLLER_CHAT_ID_MAX ||
        (session_alias && session_alias[0] &&
         !claw_session_mgr_alias_is_valid(session_alias))) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->snapshot.active ||
        controller->session_creation_in_progress ||
        controller->snapshot.session_loading ||
        controller->session_delete_pending) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    cancel_session_switch_locked(controller);
    reset_session_view_locked(controller);
    strlcpy(controller->snapshot.chat_id, chat_id,
            sizeof(controller->snapshot.chat_id));
    strlcpy(controller->snapshot.session_alias,
            session_alias && session_alias[0] ? session_alias : "default",
            sizeof(controller->snapshot.session_alias));
    controller->snapshot.new_session_pending = false;
    controller->pending_session_alias[0] = '\0';
    xSemaphoreGive(controller->lock);
    (void)reload_history_internal(controller, false);
    schedule_journal(controller);
    notify_changed(controller);
    return ESP_OK;
}

esp_err_t ai_create_controller_post_text(ai_create_controller_handle_t controller,
    const char *text)
{
    if (!controller || !text || !text[0] ||
        strlen(text) > AI_CREATE_CONTROLLER_TEXT_MAX) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    const bool had_active_run = controller->snapshot.active;
    const bool session_loading = controller->snapshot.session_loading;
    xSemaphoreGive(controller->lock);
    if (session_loading) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    if (!had_active_run) {
        err = bind_session_for_submit(controller);
        if (err != ESP_OK) return err;
    }
    cap_im_ai_create_request_t request = {0};
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (!controller->started ||
        (!had_active_run && !controller->session_creation_in_progress)) {
        controller->session_creation_in_progress = false;
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    request.protocol_version = CAP_IM_AI_CREATE_PROTOCOL_VERSION;
    request.request_id = controller->next_request_id++;
    if (controller->next_request_id == 0) controller->next_request_id = 1;
    request.chat_id = controller->snapshot.chat_id;
    request.session_alias = controller->snapshot.session_alias;
    request.mode = controller->snapshot.mode;
    request.input = CAP_IM_AI_CREATE_INPUT_TEXT;
    request.inject_mode_instruction = controller->snapshot.transcript.count == 0;
    request.text = text;
    controller_message_track_t *track = reserve_message_locked(
        controller, request.request_id);
    if (!track) {
        controller->session_creation_in_progress = false;
        xSemaphoreGive(controller->lock);
        return ESP_ERR_NO_MEM;
    }
    if (!had_active_run) {
        controller->snapshot.request_id = request.request_id;
        controller->snapshot.response[0] = '\0';
        controller->snapshot.progress[0] = '\0';
        controller->snapshot.error[0] = '\0';
        strlcpy(controller->snapshot.input, text,
                sizeof(controller->snapshot.input));
    }
    controller->snapshot.screen = AI_CREATE_SCREEN_CHAT;
    controller->snapshot.composer = AI_CREATE_COMPOSER_DEFAULT;
    controller->snapshot.state = had_active_run ? AI_CREATE_STATE_RUNNING
                                                : AI_CREATE_STATE_SUBMITTING;
    controller->snapshot.status = CAP_IM_AI_CREATE_STATUS_OK;
    refresh_active_locked(controller);
    controller->snapshot.terminal = false;
    controller->snapshot.retryable = false;
    transcript_append_locked(controller, request.request_id,
                             AI_CREATE_MESSAGE_USER,
                             text, false, false);
    transcript_append_locked(controller, request.request_id,
                             AI_CREATE_MESSAGE_ASSISTANT,
                             had_active_run ? "Added to the current task…" :
                                              "Processing…",
                             true, false);
    controller->session_creation_in_progress = false;
    xSemaphoreGive(controller->lock);
    schedule_journal(controller);
    err = controller->gateway->post_message(&request);
    if (err != ESP_OK) {
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        controller_message_track_t *track = find_message_locked(
            controller, request.request_id);
        if (track) {
            memset(track, 0, sizeof(*track));
            refresh_active_locked(controller);
            controller->snapshot.state = AI_CREATE_STATE_ERROR;
            controller->snapshot.status = err == ESP_ERR_INVALID_STATE
                                              ? CAP_IM_AI_CREATE_STATUS_BUSY
                                              : CAP_IM_AI_CREATE_STATUS_UNAVAILABLE;
            controller->snapshot.terminal = !controller->snapshot.active;
            controller->snapshot.retryable = true;
            snprintf(controller->snapshot.error, sizeof(controller->snapshot.error),
                     "Submission failed: %s", esp_err_to_name(err));
            transcript_update_assistant_locked(
                controller, request.request_id,
                controller->snapshot.error, false, true);
        }
        xSemaphoreGive(controller->lock);
    }
    notify_changed(controller);
    return err;
}

esp_err_t ai_create_controller_cancel(ai_create_controller_handle_t controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (!controller->snapshot.active || controller->snapshot.request_id == 0) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t id = controller->snapshot.request_id;
    controller->snapshot.state = AI_CREATE_STATE_CANCELLING;
    xSemaphoreGive(controller->lock);
    notify_changed(controller);
    return controller->gateway->cancel(id);
}

esp_err_t ai_create_controller_abort_current(
    ai_create_controller_handle_t controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;

    uint32_t voice_operation_id = 0;
    uint32_t request_ids[CONTROLLER_MAX_INFLIGHT_MESSAGES] = {0};
    size_t request_count = 0;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    voice_operation_id = controller->voice_operation_id;
    for (size_t i = 0; i < CONTROLLER_MAX_INFLIGHT_MESSAGES; ++i) {
        if (controller->messages[i].used) {
            request_ids[request_count++] = controller->messages[i].request_id;
        }
    }

    controller->voice_operation_id = 0;
    controller->voice_deadline_us = 0;
    controller->snapshot.voice_operation_id = 0;
    controller->snapshot.voice_state = AI_CREATE_VOICE_STATE_IDLE;
    if (request_count > 0) {
        memset(controller->messages, 0, sizeof(controller->messages));
        refresh_active_locked(controller);
        controller->snapshot.terminal = true;
        controller->snapshot.retryable = false;
        controller->snapshot.state = AI_CREATE_STATE_CANCELLED;
        controller->snapshot.status = CAP_IM_AI_CREATE_STATUS_CANCELLED;
        strlcpy(controller->snapshot.progress, "Current task ended.",
                sizeof(controller->snapshot.progress));
        transcript_update_assistant_locked(
            controller, request_ids[0], "Current task ended.", false, false);
    }
    controller->snapshot.screen = AI_CREATE_SCREEN_WELCOME;
    controller->snapshot.composer = AI_CREATE_COMPOSER_DEFAULT;
    cancel_session_switch_locked(controller);
    clear_notice_locked(controller);
    xSemaphoreGive(controller->lock);

    /* Publish the local cancellation before invoking ports. Gateway and ASR
     * callbacks may be synchronous; terminal/operation guards must already
     * reject those stale completions. */
    notify_changed(controller);
    schedule_journal(controller);

    esp_err_t first_err = ESP_OK;
    if (voice_operation_id && controller->voice_port) {
        esp_err_t err = ai_create_voice_port_cancel(
            controller->voice_port, voice_operation_id);
        if (err != ESP_OK) first_err = err;
    }
    for (size_t i = 0; i < request_count; ++i) {
        esp_err_t err = controller->gateway->cancel(request_ids[i]);
        if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
        if (first_err == ESP_OK && err != ESP_OK) first_err = err;
    }
    return first_err;
}

esp_err_t ai_create_controller_retry(ai_create_controller_handle_t controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    char *text = malloc(AI_CREATE_CONTROLLER_TEXT_MAX + 1U);
    if (!text) return ESP_ERR_NO_MEM;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    bool allowed = controller->snapshot.terminal && controller->snapshot.retryable;
    strlcpy(text, controller->snapshot.input, AI_CREATE_CONTROLLER_TEXT_MAX + 1U);
    xSemaphoreGive(controller->lock);
    esp_err_t err = allowed ? ai_create_controller_post_text(controller, text)
                            : ESP_ERR_INVALID_STATE;
    free(text);
    return err;
}

esp_err_t ai_create_controller_voice_start(ai_create_controller_handle_t controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    ai_create_voice_port_handle_t voice_port = controller->voice_port;
    ai_create_voice_status_t cached_status =
        controller->snapshot.voice_status;
    xSemaphoreGive(controller->lock);
    ai_create_voice_status_t current_status = normalized_voice_status(
        voice_port, cached_status);

    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (voice_port != controller->voice_port) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (voice_port) {
        controller->snapshot.voice_status = current_status;
    }
    const ai_create_voice_status_t voice_status =
        controller->snapshot.voice_status;
    const bool service_ready = voice_port &&
        (voice_status.readiness == AI_CREATE_VOICE_READINESS_READY ||
         ((voice_status.readiness == AI_CREATE_VOICE_READINESS_OFFLINE ||
           voice_status.readiness ==
               AI_CREATE_VOICE_READINESS_PROVIDER_ERROR ||
           voice_status.readiness == AI_CREATE_VOICE_READINESS_AUDIO_ERROR) &&
          voice_status.retryable));
    if (!service_ready) {
        controller->snapshot.status = CAP_IM_AI_CREATE_STATUS_UNAVAILABLE;
        controller->snapshot.retryable = voice_status.retryable;
        controller->snapshot.notice_visible = true;
        controller->snapshot.notice_code = voice_status.error != ESP_OK
            ? voice_status.error : ESP_ERR_NOT_SUPPORTED;
        const esp_err_t unavailable_error =
            controller->snapshot.notice_code;
        controller->snapshot.notice[0] = '\0';
        xSemaphoreGive(controller->lock);
        notify_changed(controller);
        return unavailable_error;
    }
    bool ready = controller->started && controller->voice_operation_id == 0 &&
                 !controller->snapshot.session_loading &&
                 controller->snapshot.screen !=
                     AI_CREATE_SCREEN_SESSION_PICKER &&
                 (controller->snapshot.composer ==
                      AI_CREATE_COMPOSER_DEFAULT ||
                  controller->snapshot.composer ==
                      AI_CREATE_COMPOSER_MODE);
    uint32_t operation_id = 0;
    if (ready) {
        operation_id = controller->next_voice_operation_id++;
        if (operation_id == 0) {
            operation_id = controller->next_voice_operation_id++;
        }
        controller->voice_operation_id = operation_id;
        controller->snapshot.voice_operation_id = operation_id;
        controller->snapshot.voice_state =
            AI_CREATE_VOICE_STATE_PREPARING;
        set_voice_deadline_locked(
            controller, AI_CREATE_VOICE_STATE_PREPARING,
            esp_timer_get_time());
        controller->snapshot.input[0] = '\0';
        controller->snapshot.error[0] = '\0';
        controller->snapshot.retryable = false;
        clear_notice_locked(controller);
    }
    xSemaphoreGive(controller->lock);
    if (!ready) return ESP_ERR_INVALID_STATE;
    notify_changed(controller);
    esp_err_t err = ai_create_voice_port_begin(voice_port, operation_id);
    if (err != ESP_OK) {
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        if (controller->voice_operation_id == operation_id) {
            voice_failure_locked(controller, err,
                                 "Could not start recording. Please retry.");
        }
        xSemaphoreGive(controller->lock);
        notify_changed(controller);
    }
    return err;
}

esp_err_t ai_create_controller_voice_stop(ai_create_controller_handle_t controller,
    bool send)
{
    if (!controller || !controller->voice_port) return ESP_ERR_NOT_SUPPORTED;
    if (!send) return ai_create_controller_voice_cancel(controller);
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    const uint32_t operation_id = controller->voice_operation_id;
    if (operation_id == 0 ||
        (controller->snapshot.voice_state !=
             AI_CREATE_VOICE_STATE_PREPARING &&
         controller->snapshot.voice_state !=
             AI_CREATE_VOICE_STATE_LISTENING)) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    controller->snapshot.voice_state = AI_CREATE_VOICE_STATE_FINALIZING;
    set_voice_deadline_locked(
        controller, AI_CREATE_VOICE_STATE_FINALIZING,
        esp_timer_get_time());
    xSemaphoreGive(controller->lock);
    notify_changed(controller);
    esp_err_t err = ai_create_voice_port_finish(controller->voice_port,
                                                operation_id, send);
    if (err != ESP_OK) {
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        if (controller->voice_operation_id == operation_id || send) {
            voice_failure_locked(controller, err,
                                 "Could not stop recording. Please retry.");
        }
        xSemaphoreGive(controller->lock);
        notify_changed(controller);
    }
    return err;
}

esp_err_t ai_create_controller_voice_cancel(
    ai_create_controller_handle_t controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    return cancel_voice_operation(controller, 0, ESP_OK, NULL);
}

esp_err_t ai_create_controller_get_voice_loudness(
    ai_create_controller_handle_t controller, uint8_t *out_level)
{
    if (!controller || !out_level) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    ai_create_voice_port_handle_t voice_port = controller->voice_port;
    xSemaphoreGive(controller->lock);
    return voice_port
        ? ai_create_voice_port_get_loudness(voice_port, out_level)
        : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ai_create_controller_list_sessions(ai_create_controller_handle_t controller,
    claw_session_mgr_alias_map_t *out_sessions)
{
    if (!controller || !out_sessions) return ESP_ERR_INVALID_ARG;
    char chat_id[AI_CREATE_CONTROLLER_CHAT_ID_MAX + 1U];
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    strlcpy(chat_id, controller->snapshot.chat_id, sizeof(chat_id));
    xSemaphoreGive(controller->lock);
    esp_err_t err = claw_session_mgr_list_chat_sessions(
        0, CAP_IM_AI_CREATE_CHANNEL, chat_id, out_sessions);
    if (err == ESP_OK && out_sessions->current_alias[0]) {
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        if (!controller->snapshot.active &&
            !controller->snapshot.new_session_pending &&
            strcmp(controller->snapshot.chat_id, chat_id) == 0) {
            strlcpy(controller->snapshot.session_alias,
                    out_sessions->current_alias,
                    sizeof(controller->snapshot.session_alias));
        }
        xSemaphoreGive(controller->lock);
    }
    return err;
}

esp_err_t ai_create_controller_new_session(ai_create_controller_handle_t controller,
    const char *requested_alias)
{
    return prepare_new_session(controller, requested_alias, true);
}

esp_err_t ai_create_controller_switch_session(ai_create_controller_handle_t controller,
    const char *alias)
{
    if (!controller || !alias) return ESP_ERR_INVALID_ARG;
    char chat_id[AI_CREATE_CONTROLLER_CHAT_ID_MAX + 1U];
    char resolved[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->snapshot.active ||
        controller->session_creation_in_progress) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(chat_id, controller->snapshot.chat_id, sizeof(chat_id));
    xSemaphoreGive(controller->lock);
    esp_err_t err = claw_session_mgr_switch_chat_session(0,
        CAP_IM_AI_CREATE_CHANNEL, chat_id, alias, resolved, sizeof(resolved));
    return err == ESP_OK
               ? ai_create_controller_set_session(controller, chat_id, resolved)
               : err;
}

esp_err_t ai_create_controller_begin_session_switch(
    ai_create_controller_handle_t controller, uint32_t *out_generation)
{
    if (!controller || !out_generation) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (!controller->started || controller->snapshot.active ||
        controller->voice_operation_id != 0 ||
        controller->session_creation_in_progress ||
        controller->session_delete_pending ||
        controller->snapshot.session_delete_confirm_visible) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    cancel_session_switch_locked(controller);
    reset_session_view_locked(controller);
    controller->session_switch_pending = true;
    controller->snapshot.session_loading = true;
    controller->snapshot.screen = AI_CREATE_SCREEN_CHAT;
    controller->snapshot.composer = AI_CREATE_COMPOSER_DEFAULT;
    controller->snapshot.new_session_pending = false;
    controller->snapshot.session_alias[0] = '\0';
    controller->pending_session_alias[0] = '\0';
    *out_generation = controller->session_switch_generation;
    xSemaphoreGive(controller->lock);
    notify_changed(controller);
    return ESP_OK;
}

esp_err_t ai_create_controller_complete_session_switch(
    ai_create_controller_handle_t controller, const char *alias,
    uint32_t generation)
{
    if (!controller || !alias || !claw_session_mgr_alias_is_valid(alias) ||
        generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char chat_id[AI_CREATE_CONTROLLER_CHAT_ID_MAX + 1U];
    char resolved[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (!controller->session_switch_pending ||
        controller->session_switch_generation != generation) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(chat_id, controller->snapshot.chat_id, sizeof(chat_id));
    xSemaphoreGive(controller->lock);

    ai_create_transcript_t *loaded = calloc(1, sizeof(*loaded));
    esp_err_t err = loaded ? ESP_OK : ESP_ERR_NO_MEM;
    strlcpy(resolved, alias, sizeof(resolved));
    if (err == ESP_OK && controller->history && controller->history->load) {
        err = controller->history->load(
            controller->history_ctx, chat_id, resolved, loaded);
        if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK && !transcript_valid(loaded)) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) {
            for (size_t i = 0; i < loaded->count; ++i) {
                loaded->messages[i]
                    .text[AI_CREATE_CONTROLLER_MESSAGE_MAX] = '\0';
            }
        }
    }

    bool current = false;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    current = controller->session_switch_pending &&
        controller->session_switch_generation == generation;
    if (current && err == ESP_OK) {
        /* Commit the persistent binding only after the slow history read and
         * while cancellation is serialized by the controller lock. */
        err = claw_session_mgr_switch_chat_session(
            0, CAP_IM_AI_CREATE_CHANNEL, chat_id, alias,
            resolved, sizeof(resolved));
    }
    if (current && err == ESP_OK) {
        reset_session_view_locked(controller);
        controller->session_switch_pending = false;
        controller->snapshot.session_loading = false;
        controller->snapshot.screen = AI_CREATE_SCREEN_CHAT;
        strlcpy(controller->snapshot.chat_id, chat_id,
                sizeof(controller->snapshot.chat_id));
        strlcpy(controller->snapshot.session_alias, resolved,
                sizeof(controller->snapshot.session_alias));
        controller->snapshot.new_session_pending = false;
        controller->snapshot.transcript = *loaded;
    } else if (current) {
        controller->session_switch_pending = false;
        controller->snapshot.session_loading = false;
        controller->snapshot.screen = AI_CREATE_SCREEN_SESSION_PICKER;
        controller->snapshot.notice_visible = true;
        controller->snapshot.notice_code = err;
        strlcpy(controller->snapshot.notice,
                "Could not switch to the selected session.",
                sizeof(controller->snapshot.notice));
    }
    xSemaphoreGive(controller->lock);
    free(loaded);

    if (!current) return ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) schedule_journal(controller);
    notify_changed(controller);
    return err;
}

void ai_create_controller_cancel_session_switch(
    ai_create_controller_handle_t controller)
{
    if (!controller) return;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    const bool changed = controller->session_switch_pending ||
        controller->snapshot.session_loading;
    cancel_session_switch_locked(controller);
    xSemaphoreGive(controller->lock);
    if (changed) notify_changed(controller);
}

static esp_err_t load_session_for_delete(
    ai_create_controller_handle_t controller, const char *chat_id,
    const char *alias, ai_create_transcript_t *out_transcript)
{
    if (!controller || !chat_id || !alias || !out_transcript) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_transcript, 0, sizeof(*out_transcript));
    if (!controller->history || !controller->history->load) return ESP_OK;
    esp_err_t err = controller->history->load(
        controller->history_ctx, chat_id, alias, out_transcript);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (!transcript_valid(out_transcript)) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < out_transcript->count; ++i) {
        out_transcript->messages[i]
            .text[AI_CREATE_CONTROLLER_MESSAGE_MAX] = '\0';
    }
    return ESP_OK;
}

esp_err_t ai_create_controller_begin_session_delete(
    ai_create_controller_handle_t controller,
    char *out_alias, size_t out_alias_size, uint32_t *out_generation)
{
    if (!controller || !out_alias || out_alias_size == 0 ||
        !out_generation) {
        return ESP_ERR_INVALID_ARG;
    }
    out_alias[0] = '\0';
    *out_generation = 0;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (!controller->started || controller->snapshot.active ||
        controller->voice_operation_id != 0 ||
        controller->session_creation_in_progress ||
        controller->session_switch_pending ||
        controller->session_delete_pending ||
        controller->snapshot.screen != AI_CREATE_SCREEN_SESSION_PICKER ||
        !controller->snapshot.session_delete_confirm_visible ||
        !claw_session_mgr_alias_is_valid(
            controller->snapshot.delete_session_alias)) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    controller->session_delete_generation++;
    if (controller->session_delete_generation == 0) {
        controller->session_delete_generation = 1;
    }
    controller->session_delete_pending = true;
    controller->snapshot.session_deleting = true;
    strlcpy(out_alias, controller->snapshot.delete_session_alias,
            out_alias_size);
    *out_generation = controller->session_delete_generation;
    clear_notice_locked(controller);
    xSemaphoreGive(controller->lock);
    notify_changed(controller);
    return ESP_OK;
}

void ai_create_controller_fail_session_delete(
    ai_create_controller_handle_t controller, uint32_t generation,
    esp_err_t error)
{
    if (!controller || generation == 0) return;
    bool changed = false;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->session_delete_pending &&
        controller->session_delete_generation == generation) {
        controller->session_delete_pending = false;
        clear_session_delete_prompt_locked(controller);
        controller->snapshot.notice_visible = true;
        controller->snapshot.notice_code = error;
        strlcpy(controller->snapshot.notice,
                "Could not delete the session. Try again later.",
                sizeof(controller->snapshot.notice));
        changed = true;
    }
    xSemaphoreGive(controller->lock);
    if (changed) notify_changed(controller);
}

esp_err_t ai_create_controller_complete_session_delete(
    ai_create_controller_handle_t controller, const char *alias,
    uint32_t generation)
{
    if (!controller || !alias ||
        !claw_session_mgr_alias_is_valid(alias) || generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char chat_id[AI_CREATE_CONTROLLER_CHAT_ID_MAX + 1U];
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    const bool current_operation = controller->session_delete_pending &&
        controller->session_delete_generation == generation &&
        strcmp(controller->snapshot.delete_session_alias, alias) == 0;
    if (current_operation) {
        strlcpy(chat_id, controller->snapshot.chat_id, sizeof(chat_id));
    }
    xSemaphoreGive(controller->lock);
    if (!current_operation) return ESP_ERR_INVALID_STATE;

    claw_session_mgr_alias_map_t *sessions = calloc(1, sizeof(*sessions));
    ai_create_transcript_t *replacement_transcript =
        calloc(1, sizeof(*replacement_transcript));
    if (!sessions || !replacement_transcript) {
        free(sessions);
        free(replacement_transcript);
        ai_create_controller_fail_session_delete(
            controller, generation, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = claw_session_mgr_list_chat_sessions(
        0, CAP_IM_AI_CREATE_CHANNEL, chat_id, sessions);
    size_t target_index = sessions->session_count;
    if (err == ESP_OK) {
        for (size_t i = 0; i < sessions->session_count; ++i) {
            if (strcmp(sessions->sessions[i], alias) == 0) {
                target_index = i;
                break;
            }
        }
        if (target_index == sessions->session_count) {
            err = ESP_ERR_NOT_FOUND;
        }
    }

    const bool deleting_current = err == ESP_OK &&
        strcmp(sessions->current_alias, alias) == 0;
    bool replacement_created = false;
    bool replacement_selected = false;
    char replacement_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U] = "";
    if (err == ESP_OK && deleting_current && sessions->session_count > 1U) {
        const size_t replacement_index =
            target_index + 1U < sessions->session_count
                ? target_index + 1U : target_index - 1U;
        strlcpy(replacement_alias, sessions->sessions[replacement_index],
                sizeof(replacement_alias));
        err = load_session_for_delete(controller, chat_id,
                                      replacement_alias,
                                      replacement_transcript);
        if (err == ESP_OK) {
            char resolved[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
            err = claw_session_mgr_switch_chat_session(
                0, CAP_IM_AI_CREATE_CHANNEL, chat_id, replacement_alias,
                resolved, sizeof(resolved));
            replacement_selected = err == ESP_OK;
            if (err == ESP_OK) {
                strlcpy(replacement_alias, resolved,
                        sizeof(replacement_alias));
            }
        }
    } else if (err == ESP_OK && deleting_current) {
        err = claw_session_mgr_new_chat_session(
            0, CAP_IM_AI_CREATE_CHANNEL, chat_id, NULL, false,
            replacement_alias, sizeof(replacement_alias));
        replacement_created = err == ESP_OK;
        replacement_selected = replacement_created;
    }

    if (err == ESP_OK) {
        char deleted_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
        err = claw_session_mgr_delete_chat_session(
            0, CAP_IM_AI_CREATE_CHANNEL, chat_id, alias,
            deleted_alias, sizeof(deleted_alias));
    }

    if (err != ESP_OK && deleting_current && replacement_selected) {
        char restored[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
        if (claw_session_mgr_switch_chat_session(
                0, CAP_IM_AI_CREATE_CHANNEL, chat_id, alias,
                restored, sizeof(restored)) == ESP_OK &&
            replacement_created) {
            char removed[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
            (void)claw_session_mgr_delete_chat_session(
                0, CAP_IM_AI_CREATE_CHANNEL, chat_id, replacement_alias,
                removed, sizeof(removed));
        }
    }

    if (err == ESP_OK) {
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        const bool still_current = controller->session_delete_pending &&
            controller->session_delete_generation == generation;
        if (still_current) {
            controller->session_delete_pending = false;
            if (deleting_current) {
                reset_session_view_locked(controller);
                controller->snapshot.screen =
                    AI_CREATE_SCREEN_SESSION_PICKER;
                strlcpy(controller->snapshot.chat_id, chat_id,
                        sizeof(controller->snapshot.chat_id));
                strlcpy(controller->snapshot.session_alias,
                        replacement_alias,
                        sizeof(controller->snapshot.session_alias));
                controller->snapshot.new_session_pending = false;
                controller->snapshot.transcript = *replacement_transcript;
                controller->pending_session_alias[0] = '\0';
            } else {
                clear_session_delete_prompt_locked(controller);
                clear_notice_locked(controller);
            }
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(controller->lock);
    }

    free(replacement_transcript);
    free(sessions);
    if (err != ESP_OK) {
        ai_create_controller_fail_session_delete(controller, generation, err);
        return err;
    }
    schedule_journal(controller);
    notify_changed(controller);
    return ESP_OK;
}

esp_err_t ai_create_controller_get_snapshot(ai_create_controller_handle_t controller,
    ai_create_controller_snapshot_t *out_snapshot)
{
    if (!controller || !out_snapshot) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    *out_snapshot = controller->snapshot;
    xSemaphoreGive(controller->lock);
    return ESP_OK;
}

esp_err_t ai_create_controller_get_revision(
    ai_create_controller_handle_t controller, uint32_t *out_revision)
{
    if (!controller || !out_revision) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    *out_revision = controller->snapshot.revision;
    xSemaphoreGive(controller->lock);
    return ESP_OK;
}

esp_err_t ai_create_controller_reload_history(
    ai_create_controller_handle_t controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    return reload_history_internal(controller, true);
}

esp_err_t ai_create_controller_step(ai_create_controller_handle_t controller,
    int64_t now_us)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    if (now_us <= 0) now_us = esp_timer_get_time();

    uint32_t operation_id = 0;
    ai_create_voice_state_t state = AI_CREATE_VOICE_STATE_IDLE;
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->voice_operation_id != 0 &&
        controller->voice_deadline_us > 0 &&
        now_us >= controller->voice_deadline_us) {
        operation_id = controller->voice_operation_id;
        state = controller->snapshot.voice_state;
    }
    xSemaphoreGive(controller->lock);
    if (operation_id == 0) return ESP_OK;

    const char *message = state == AI_CREATE_VOICE_STATE_PREPARING
        ? "Voice service connection timed out. Check the network and retry."
        : (state == AI_CREATE_VOICE_STATE_LISTENING
            ? "Recording took too long. Please retry."
            : "Speech recognition timed out. Please retry.");
    ESP_LOGW(TAG, "voice timeout op=%lu state=%d",
             (unsigned long)operation_id, (int)state);
    return cancel_voice_operation(controller, operation_id,
                                  ESP_ERR_TIMEOUT, message);
}

static esp_err_t set_ui_state(ai_create_controller_handle_t controller,
    ai_create_screen_t screen, ai_create_composer_t composer)
{
    xSemaphoreTake(controller->lock, portMAX_DELAY);
    if (controller->snapshot.active || controller->session_delete_pending) {
        xSemaphoreGive(controller->lock);
        return ESP_ERR_INVALID_STATE;
    }
    cancel_session_switch_locked(controller);
    clear_session_delete_prompt_locked(controller);
    controller->snapshot.screen = screen;
    controller->snapshot.composer = composer;
    clear_notice_locked(controller);
    xSemaphoreGive(controller->lock);
    notify_changed(controller);
    return ESP_OK;
}

static const char *command_error_message(ai_create_command_type_t type,
                                         esp_err_t error)
{
    (void)error;
    switch (type) {
    case AI_CREATE_COMMAND_NEW_SESSION:
        return "Could not create a new session. Try again later.";
    case AI_CREATE_COMMAND_SELECT_SESSION:
        return "Could not switch to the selected session.";
    case AI_CREATE_COMMAND_REQUEST_DELETE_SESSION:
    case AI_CREATE_COMMAND_CANCEL_DELETE_SESSION:
        return "The selected session cannot be deleted right now.";
    case AI_CREATE_COMMAND_SELECT_MODE:
    case AI_CREATE_COMMAND_CLEAR_MODE:
        return "Wait for the current request to finish before changing modes.";
    case AI_CREATE_COMMAND_RETRY:
        return "The current request cannot be retried.";
    default:
        return "The current operation could not be completed.";
    }
}

esp_err_t ai_create_controller_dispatch(ai_create_controller_handle_t controller,
    const ai_create_command_t *command)
{
    if (!controller || !command) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    switch (command->type) {
    case AI_CREATE_COMMAND_OPEN_SESSION_PICKER:
        err = set_ui_state(controller, AI_CREATE_SCREEN_SESSION_PICKER,
                           AI_CREATE_COMPOSER_DEFAULT);
        break;
    case AI_CREATE_COMMAND_CLOSE_SESSION_PICKER:
        err = set_ui_state(controller, AI_CREATE_SCREEN_WELCOME,
                           AI_CREATE_COMPOSER_DEFAULT);
        break;
    case AI_CREATE_COMMAND_NEW_SESSION:
        err = ai_create_controller_new_session(controller, NULL);
        break;
    case AI_CREATE_COMMAND_SELECT_SESSION: {
        claw_session_mgr_alias_map_t *sessions = malloc(sizeof(*sessions));
        if (!sessions) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        err = ai_create_controller_list_sessions(controller, sessions);
        if (err == ESP_OK) {
            if (command->data.session_index >= sessions->session_count) {
                err = ESP_ERR_NOT_FOUND;
            } else {
                err = ai_create_controller_switch_session(
                    controller,
                    sessions->sessions[command->data.session_index]);
                if (err == ESP_OK) {
                    xSemaphoreTake(controller->lock, portMAX_DELAY);
                    controller->snapshot.screen = AI_CREATE_SCREEN_CHAT;
                    xSemaphoreGive(controller->lock);
                    notify_changed(controller);
                }
            }
        }
        free(sessions);
        break;
    }
    case AI_CREATE_COMMAND_REQUEST_DELETE_SESSION:
        if (!claw_session_mgr_alias_is_valid(
                command->data.session_alias)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        if (controller->snapshot.screen !=
                AI_CREATE_SCREEN_SESSION_PICKER ||
            controller->snapshot.active ||
            controller->snapshot.session_loading ||
            controller->session_delete_pending) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            controller->snapshot.session_delete_confirm_visible = true;
            controller->snapshot.session_deleting = false;
            strlcpy(controller->snapshot.delete_session_alias,
                    command->data.session_alias,
                    sizeof(controller->snapshot.delete_session_alias));
            clear_notice_locked(controller);
            err = ESP_OK;
        }
        xSemaphoreGive(controller->lock);
        if (err == ESP_OK) notify_changed(controller);
        break;
    case AI_CREATE_COMMAND_CANCEL_DELETE_SESSION:
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        if (controller->session_delete_pending) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            clear_session_delete_prompt_locked(controller);
            err = ESP_OK;
        }
        xSemaphoreGive(controller->lock);
        if (err == ESP_OK) notify_changed(controller);
        break;
    case AI_CREATE_COMMAND_SELECT_MODE:
        err = ai_create_controller_set_mode(controller, command->data.mode);
        break;
    case AI_CREATE_COMMAND_CLEAR_MODE:
        err = ai_create_controller_set_mode(
            controller, CAP_IM_AI_CREATE_MODE_QUICK);
        if (err == ESP_OK) {
            xSemaphoreTake(controller->lock, portMAX_DELAY);
            controller->snapshot.composer = AI_CREATE_COMPOSER_DEFAULT;
            xSemaphoreGive(controller->lock);
            notify_changed(controller);
        }
        break;
    case AI_CREATE_COMMAND_LEAVE_CHAT: {
        err = ai_create_controller_abort_current(controller);
        if (err == ESP_OK) {
            err = prepare_new_session(controller, NULL, true);
        }
        break;
    }
    case AI_CREATE_COMMAND_DISMISS_NOTICE:
        xSemaphoreTake(controller->lock, portMAX_DELAY);
        clear_notice_locked(controller);
        xSemaphoreGive(controller->lock);
        notify_changed(controller);
        err = ESP_OK;
        break;
    case AI_CREATE_COMMAND_RETRY:
        err = ai_create_controller_retry(controller);
        break;
    default:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    if (err != ESP_OK) {
        set_notice(controller, err,
                   command_error_message(command->type, err));
    }
    return err;
}
