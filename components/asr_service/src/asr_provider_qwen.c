/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "asr_provider.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "trial_auth.h"

static const char *TAG = "asr_qwen";

#define ASR_QWEN_WS_TEXT_OPCODE        0x1
#define ASR_QWEN_CONNECTED_BIT         BIT0
#define ASR_QWEN_DISCONNECTED_BIT      BIT1
#define ASR_QWEN_TASK_STARTED_BIT      BIT2
#define ASR_QWEN_TASK_FINISHED_BIT     BIT3
#define ASR_QWEN_TASK_FAILED_BIT       BIT4
#define ASR_QWEN_WS_BUFFER_SIZE        8192
#define ASR_QWEN_NETWORK_TIMEOUT_MS    5000
#define ASR_QWEN_RECONNECT_TIMEOUT_MS  1000
#define ASR_QWEN_WS_TASK_PRIO          12
#define ASR_QWEN_RX_MAX_BYTES          16384
#define ASR_QWEN_STR_LEN               320
#define ASR_QWEN_MODEL_LEN             64
#define ASR_QWEN_SHORT_STR_LEN         32
#define ASR_QWEN_TASK_ID_LEN           37
#define ASR_QWEN_USER_AGENT            "esp-claw-asr-service"

typedef struct {
    char api_key[ASR_QWEN_STR_LEN];
    char workspace_id[ASR_QWEN_STR_LEN];
    char endpoint[ASR_QWEN_STR_LEN];
    char model[ASR_QWEN_MODEL_LEN];
    char language_hint[ASR_QWEN_SHORT_STR_LEN];
    uint32_t connect_timeout_ms;
    uint32_t send_timeout_ms;
    esp_websocket_client_handle_t ws;
    EventGroupHandle_t events;
    SemaphoreHandle_t send_lock;
    SemaphoreHandle_t result_lock;
    asr_provider_result_cb_t result_cb;
    void *result_user_ctx;
    char *auth_header;
    char task_id[ASR_QWEN_TASK_ID_LEN];
    char *rx_text_buf;
    size_t rx_text_cap;
    char *final_text;
    int64_t last_final_sentence_id;
    volatile bool connected;
    volatile bool task_started;
    volatile bool task_running;
    volatile bool finish_sent;
    volatile bool have_final;
    volatile esp_err_t remote_error;
    bool trial_auth;
} asr_qwen_t;

static void qwen_disconnect(asr_provider_handle_t handle);
static void qwen_delete(asr_provider_handle_t handle);

static bool has_bearer_prefix(const char *token)
{
    static const char *prefix = "Bearer ";
    return token && strncasecmp(token, prefix, strlen(prefix)) == 0;
}

static char *dup_auth_header(const char *api_key)
{
    static const char *prefix = "Bearer ";
    if (!api_key) {
        return NULL;
    }
    if (has_bearer_prefix(api_key)) {
        size_t len = strlen(api_key) + 1;
        char *copy = malloc(len);
        if (copy) {
            memcpy(copy, api_key, len);
        }
        return copy;
    }

    size_t len = strlen(prefix) + strlen(api_key) + 1;
    char *header = malloc(len);
    if (!header) {
        return NULL;
    }
    snprintf(header, len, "%s%s", prefix, api_key);
    return header;
}

static esp_err_t refresh_trial_auth(asr_qwen_t *qwen, bool force_refresh)
{
    if (!qwen || !qwen->trial_auth) {
        return ESP_OK;
    }
    char *token = NULL;
    esp_err_t err = trial_auth_get_asr_token(force_refresh, &token);
    if (err != ESP_OK) {
        return err;
    }
    char *header = dup_auth_header(token);
    free(token);
    if (!header) {
        return ESP_ERR_NO_MEM;
    }
    free(qwen->auth_header);
    qwen->auth_header = header;
    return ESP_OK;
}

static esp_err_t copy_required_string(char *dst, size_t dst_size, const char *src, const char *name)
{
    if (!src || !src[0]) {
        ESP_LOGE(TAG, "%s is empty", name);
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(src) >= dst_size) {
        ESP_LOGE(TAG, "%s is too long", name);
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(dst, src, dst_size);
    return ESP_OK;
}

static esp_err_t copy_optional_string(char *dst, size_t dst_size, const char *src, const char *name)
{
    if (!src || !src[0]) {
        dst[0] = '\0';
        return ESP_OK;
    }
    if (strlen(src) >= dst_size) {
        ESP_LOGE(TAG, "%s is too long", name);
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(dst, src, dst_size);
    return ESP_OK;
}

static void reset_result(asr_qwen_t *qwen)
{
    if (qwen->result_lock) {
        xSemaphoreTake(qwen->result_lock, portMAX_DELAY);
    }
    free(qwen->final_text);
    qwen->final_text = NULL;
    qwen->have_final = false;
    qwen->last_final_sentence_id = -1;
    qwen->remote_error = ESP_OK;
    if (qwen->result_lock) {
        xSemaphoreGive(qwen->result_lock);
    }
}

/*
 * Fun-ASR emits sentence.text for the current sentence, not for the whole
 * utterance.  Keep committed sentences here and publish a composed snapshot
 * (committed + current partial) so upper layers can remain replace-only.
 */
static esp_err_t compose_result_text(asr_qwen_t *qwen, const char *text,
                                     int64_t sentence_id, bool sentence_end,
                                     char **out_text)
{
    if (!qwen || !text || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    xSemaphoreTake(qwen->result_lock, portMAX_DELAY);

    if (sentence_id < 0) {
        sentence_id = qwen->last_final_sentence_id + 1;
    }

    if (sentence_end && sentence_id > qwen->last_final_sentence_id) {
        const size_t committed_len = qwen->final_text ? strlen(qwen->final_text) : 0;
        const size_t sentence_len = strlen(text);
        char *joined = malloc(committed_len + sentence_len + 1);
        if (!joined) {
            xSemaphoreGive(qwen->result_lock);
            ESP_LOGE(TAG, "final text append alloc failed");
            return ESP_ERR_NO_MEM;
        }
        if (committed_len > 0) {
            memcpy(joined, qwen->final_text, committed_len);
        }
        memcpy(joined + committed_len, text, sentence_len + 1);
        free(qwen->final_text);
        qwen->final_text = joined;
        qwen->last_final_sentence_id = sentence_id;
        qwen->have_final = true;
    }

    const bool include_partial = !sentence_end &&
                                 sentence_id > qwen->last_final_sentence_id;
    const size_t committed_len = qwen->final_text ? strlen(qwen->final_text) : 0;
    const size_t partial_len = include_partial ? strlen(text) : 0;
    char *snapshot = malloc(committed_len + partial_len + 1);
    if (!snapshot) {
        xSemaphoreGive(qwen->result_lock);
        ESP_LOGE(TAG, "result snapshot alloc failed");
        return ESP_ERR_NO_MEM;
    }
    if (committed_len > 0) {
        memcpy(snapshot, qwen->final_text, committed_len);
    }
    if (partial_len > 0) {
        memcpy(snapshot + committed_len, text, partial_len);
    }
    snapshot[committed_len + partial_len] = '\0';
    xSemaphoreGive(qwen->result_lock);

    *out_text = snapshot;
    return ESP_OK;
}

static void notify_result(asr_qwen_t *qwen, asr_provider_result_type_t type, const char *text)
{
    if (qwen->result_cb) {
        qwen->result_cb(qwen, type, text, qwen->result_user_ctx);
    }
}

static void generate_task_id(asr_qwen_t *qwen)
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    uint32_t c = esp_random();
    uint32_t d = esp_random();
    snprintf(qwen->task_id, sizeof(qwen->task_id), "%08x-%04x-%04x-%04x-%04x%08x",
             (unsigned int)a, (unsigned int)(b & 0xFFFF), (unsigned int)((b >> 16) & 0xFFFF),
             (unsigned int)(c & 0xFFFF), (unsigned int)((c >> 16) & 0xFFFF), (unsigned int)d);
}

static esp_err_t send_text_locked(asr_qwen_t *qwen, const char *json)
{
    if (!qwen || !json) {
        ESP_LOGE(TAG, "text send args are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (!qwen->connected || !qwen->ws) {
        ESP_LOGE(TAG, "websocket is not connected");
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t timeout = pdMS_TO_TICKS(qwen->send_timeout_ms);
    if (xSemaphoreTake(qwen->send_lock, timeout) != pdTRUE) {
        ESP_LOGE(TAG, "websocket text send lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    int sent = esp_websocket_client_send_text(qwen->ws, json, (int)strlen(json), timeout);
    xSemaphoreGive(qwen->send_lock);
    if (sent <= 0) {
        ESP_LOGE(TAG, "websocket text send failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_json(asr_qwen_t *qwen, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        ESP_LOGE(TAG, "json render failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = send_text_locked(qwen, json);
    cJSON_free(json);
    return err;
}

static esp_err_t add_language_hints(asr_qwen_t *qwen, cJSON *parameters)
{
    if (!qwen->language_hint[0]) {
        return ESP_OK;
    }
    cJSON *hints = cJSON_AddArrayToObject(parameters, "language_hints");
    cJSON *hint = hints ? cJSON_CreateString(qwen->language_hint) : NULL;
    if (!hints || !hint || !cJSON_AddItemToArray(hints, hint)) {
        cJSON_Delete(hint);
        ESP_LOGE(TAG, "language hints json build failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t send_run_task(asr_qwen_t *qwen)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *header = root ? cJSON_AddObjectToObject(root, "header") : NULL;
    cJSON *payload = root ? cJSON_AddObjectToObject(root, "payload") : NULL;
    cJSON *parameters = payload ? cJSON_AddObjectToObject(payload, "parameters") : NULL;
    cJSON *input = payload ? cJSON_AddObjectToObject(payload, "input") : NULL;
    if (!root || !header || !payload || !parameters || !input) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "run-task json alloc failed");
        return ESP_ERR_NO_MEM;
    }

    bool ok = cJSON_AddStringToObject(header, "action", "run-task") &&
              cJSON_AddStringToObject(header, "task_id", qwen->task_id) &&
              cJSON_AddStringToObject(header, "streaming", "duplex") &&
              cJSON_AddStringToObject(payload, "task_group", "audio") &&
              cJSON_AddStringToObject(payload, "task", "asr") &&
              cJSON_AddStringToObject(payload, "function", "recognition") &&
              cJSON_AddStringToObject(payload, "model", qwen->model) &&
              cJSON_AddStringToObject(parameters, "format", "pcm") &&
              cJSON_AddNumberToObject(parameters, "sample_rate", ASR_PROVIDER_SAMPLE_RATE);
    if (!ok) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "run-task json build failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = add_language_hints(qwen, parameters);
    if (err == ESP_OK) {
        err = send_json(qwen, root);
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_finish_task(asr_qwen_t *qwen)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *header = root ? cJSON_AddObjectToObject(root, "header") : NULL;
    cJSON *payload = root ? cJSON_AddObjectToObject(root, "payload") : NULL;
    cJSON *input = payload ? cJSON_AddObjectToObject(payload, "input") : NULL;
    if (!root || !header || !payload || !input) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "finish-task json alloc failed");
        return ESP_ERR_NO_MEM;
    }

    bool ok = cJSON_AddStringToObject(header, "action", "finish-task") &&
              cJSON_AddStringToObject(header, "task_id", qwen->task_id) &&
              cJSON_AddStringToObject(header, "streaming", "duplex");
    if (!ok) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "finish-task json build failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = send_json(qwen, root);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        qwen->finish_sent = true;
    }
    return err;
}

static const char *get_header_event(cJSON *root)
{
    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event = header ? cJSON_GetObjectItem(header, "event") : NULL;
    return cJSON_GetStringValue(event);
}

static void process_result_generated(asr_qwen_t *qwen, cJSON *root)
{
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON *output = payload ? cJSON_GetObjectItem(payload, "output") : NULL;
    cJSON *sentence = output ? cJSON_GetObjectItem(output, "sentence") : NULL;
    if (!cJSON_IsObject(sentence)) {
        return;
    }

    cJSON *heartbeat = cJSON_GetObjectItem(sentence, "heartbeat");
    if (cJSON_IsBool(heartbeat) && cJSON_IsTrue(heartbeat)) {
        return;
    }

    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(sentence, "text"));
    bool sentence_end = cJSON_IsTrue(cJSON_GetObjectItem(sentence, "sentence_end"));
    cJSON *sentence_id_item = cJSON_GetObjectItem(sentence, "sentence_id");
    int64_t sentence_id = cJSON_IsNumber(sentence_id_item)
                              ? (int64_t)sentence_id_item->valuedouble
                              : -1;
    if (text && text[0]) {
        char *snapshot = NULL;
        esp_err_t err = compose_result_text(qwen, text, sentence_id,
                                            sentence_end, &snapshot);
        if (err != ESP_OK) {
            qwen->remote_error = err;
            xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_FAILED_BIT);
            return;
        }
        if (snapshot[0]) {
            if (!sentence_end) {
                ESP_LOGD(TAG, "partial result: %s", snapshot);
            }
            notify_result(qwen,
                          sentence_end ? ASR_PROVIDER_RESULT_FINAL
                                       : ASR_PROVIDER_RESULT_PARTIAL,
                          snapshot);
        }
        free(snapshot);
    }
}

static void process_ws_text(asr_qwen_t *qwen, const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGW(TAG, "invalid websocket json payload");
        qwen->remote_error = ESP_FAIL;
        xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_FAILED_BIT);
        return;
    }

    const char *event = get_header_event(root);
    if (!event) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(event, "task-started") == 0) {
        qwen->task_started = true;
        qwen->task_running = true;
        qwen->finish_sent = false;
        xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_STARTED_BIT);
    } else if (strcmp(event, "result-generated") == 0) {
        process_result_generated(qwen, root);
    } else if (strcmp(event, "task-finished") == 0) {
        qwen->task_started = false;
        qwen->task_running = false;
        qwen->finish_sent = false;
        xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_FINISHED_BIT);
    } else if (strcmp(event, "task-failed") == 0) {
        qwen->task_started = false;
        qwen->task_running = false;
        qwen->finish_sent = false;
        qwen->remote_error = ESP_FAIL;
        xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_FAILED_BIT);
        ESP_LOGE(TAG, "remote ASR task failed");
    }
    cJSON_Delete(root);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    asr_qwen_t *qwen = (asr_qwen_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (!qwen) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        qwen->connected = true;
        xEventGroupClearBits(qwen->events, ASR_QWEN_DISCONNECTED_BIT);
        xEventGroupSetBits(qwen->events, ASR_QWEN_CONNECTED_BIT);
        ESP_LOGI(TAG, "websocket connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        qwen->connected = false;
        qwen->task_started = false;
        qwen->task_running = false;
        qwen->finish_sent = false;
        xEventGroupSetBits(qwen->events, ASR_QWEN_DISCONNECTED_BIT);
        ESP_LOGW(TAG, "websocket disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!data || data->op_code != ASR_QWEN_WS_TEXT_OPCODE || data->data_len < 0 || data->payload_len < 0 || data->payload_offset < 0) {
            break;
        }
        if ((size_t)data->payload_len >= ASR_QWEN_RX_MAX_BYTES) {
            ESP_LOGE(TAG, "websocket text payload too large: %d", data->payload_len);
            qwen->remote_error = ESP_ERR_INVALID_SIZE;
            xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_FAILED_BIT);
            break;
        }
        if (data->payload_offset == 0) {
            free(qwen->rx_text_buf);
            qwen->rx_text_buf = calloc(1, (size_t)data->payload_len + 1);
            qwen->rx_text_cap = (size_t)data->payload_len + 1;
            if (!qwen->rx_text_buf) {
                ESP_LOGE(TAG, "websocket rx buffer alloc failed");
                qwen->remote_error = ESP_ERR_NO_MEM;
                xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_FAILED_BIT);
                break;
            }
        }
        if (!qwen->rx_text_buf || qwen->rx_text_cap < (size_t)data->payload_len + 1 ||
            (size_t)data->payload_offset + (size_t)data->data_len > (size_t)data->payload_len) {
            ESP_LOGE(TAG, "websocket rx fragment is invalid");
            qwen->remote_error = ESP_FAIL;
            xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_FAILED_BIT);
            break;
        }
        memcpy(qwen->rx_text_buf + data->payload_offset, data->data_ptr, data->data_len);
        if (data->payload_offset + data->data_len == data->payload_len) {
            qwen->rx_text_buf[data->payload_len] = '\0';
            process_ws_text(qwen, qwen->rx_text_buf);
            free(qwen->rx_text_buf);
            qwen->rx_text_buf = NULL;
            qwen->rx_text_cap = 0;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        qwen->remote_error = ESP_FAIL;
        xEventGroupSetBits(qwen->events, ASR_QWEN_TASK_FAILED_BIT);
        ESP_LOGE(TAG, "websocket error");
        break;
    default:
        break;
    }
}

static esp_err_t qwen_create(const asr_provider_config_t *config, asr_provider_handle_t *ret_handle)
{
    if (!config || !ret_handle) {
        ESP_LOGE(TAG, "provider create args are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    *ret_handle = NULL;

    asr_qwen_t *qwen = calloc(1, sizeof(*qwen));
    if (!qwen) {
        ESP_LOGE(TAG, "provider alloc failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = config->trial_auth
                        ? copy_optional_string(qwen->api_key, sizeof(qwen->api_key), config->api_key, "api_key")
                        : copy_required_string(qwen->api_key, sizeof(qwen->api_key), config->api_key, "api_key");
    if (err == ESP_OK) err = copy_required_string(qwen->endpoint, sizeof(qwen->endpoint), config->endpoint, "endpoint");
    if (err == ESP_OK) err = copy_required_string(qwen->model, sizeof(qwen->model), config->model, "model");
    if (err == ESP_OK) err = copy_optional_string(qwen->workspace_id, sizeof(qwen->workspace_id), config->workspace_id, "workspace_id");
    if (err == ESP_OK) err = copy_optional_string(qwen->language_hint, sizeof(qwen->language_hint), config->language_hint, "language_hint");
    if (err != ESP_OK) {
        free(qwen);
        return err;
    }

    qwen->connect_timeout_ms = config->connect_timeout_ms;
    qwen->send_timeout_ms = config->send_timeout_ms;
    qwen->trial_auth = config->trial_auth;
    qwen->result_cb = config->result_cb;
    qwen->result_user_ctx = config->result_user_ctx;
    qwen->auth_header = qwen->trial_auth ? NULL : dup_auth_header(qwen->api_key);
    qwen->events = xEventGroupCreate();
    qwen->send_lock = xSemaphoreCreateMutex();
    qwen->result_lock = xSemaphoreCreateMutex();
    if ((!qwen->trial_auth && !qwen->auth_header) || !qwen->events ||
            !qwen->send_lock || !qwen->result_lock) {
        ESP_LOGE(TAG, "provider runtime alloc failed");
        qwen_delete(qwen);
        return ESP_ERR_NO_MEM;
    }

    *ret_handle = qwen;
    return ESP_OK;
}

static esp_err_t qwen_connect_once(asr_provider_handle_t handle)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen) {
        ESP_LOGE(TAG, "connect handle is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (qwen->connected) {
        return ESP_OK;
    }

    if (qwen->ws) {
        esp_websocket_client_destroy(qwen->ws);
        qwen->ws = NULL;
    }
    reset_result(qwen);
    qwen->task_id[0] = '\0';
    qwen->task_started = false;
    qwen->task_running = false;
    qwen->finish_sent = false;
    xEventGroupClearBits(qwen->events, ASR_QWEN_CONNECTED_BIT | ASR_QWEN_DISCONNECTED_BIT | ASR_QWEN_TASK_STARTED_BIT |
                                       ASR_QWEN_TASK_FINISHED_BIT | ASR_QWEN_TASK_FAILED_BIT);

    esp_websocket_client_config_t ws_config = {
        .uri = qwen->endpoint,
        .buffer_size = ASR_QWEN_WS_BUFFER_SIZE,
        .network_timeout_ms = ASR_QWEN_NETWORK_TIMEOUT_MS,
        .reconnect_timeout_ms = ASR_QWEN_RECONNECT_TIMEOUT_MS,
        .task_prio = ASR_QWEN_WS_TASK_PRIO,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = ASR_QWEN_USER_AGENT,
        .user_context = qwen,
    };

    qwen->ws = esp_websocket_client_init(&ws_config);
    if (!qwen->ws) {
        ESP_LOGE(TAG, "websocket init failed");
        return ESP_FAIL;
    }
    esp_err_t err = esp_websocket_register_events(qwen->ws, WEBSOCKET_EVENT_ANY, websocket_event_handler, qwen);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket event register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_websocket_client_append_header(qwen->ws, "Authorization", qwen->auth_header);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket auth header failed: %s", esp_err_to_name(err));
        return err;
    }
    if (qwen->trial_auth) {
        err = esp_websocket_client_append_header(qwen->ws,
                                                 TRIAL_AUTH_FIRMWARE_VERSION_HEADER,
                                                 trial_auth_get_firmware_version());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "websocket firmware version header failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (qwen->workspace_id[0]) {
        err = esp_websocket_client_append_header(qwen->ws, "X-DashScope-WorkSpace", qwen->workspace_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "websocket workspace header failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    err = esp_websocket_client_start(qwen->ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket start failed: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(qwen->events, ASR_QWEN_CONNECTED_BIT | ASR_QWEN_DISCONNECTED_BIT | ASR_QWEN_TASK_FAILED_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(qwen->connect_timeout_ms));
    if (bits & ASR_QWEN_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & ASR_QWEN_TASK_FAILED_BIT) {
        ESP_LOGE(TAG, "websocket connect failed");
        return qwen->remote_error != ESP_OK ? qwen->remote_error : ESP_FAIL;
    }
    ESP_LOGE(TAG, "websocket connect timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t qwen_connect(asr_provider_handle_t handle)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int attempt = 0; attempt < (qwen->trial_auth ? 2 : 1); ++attempt) {
        esp_err_t auth_err = refresh_trial_auth(qwen, attempt > 0);
        if (auth_err != ESP_OK) {
            return auth_err;
        }
        esp_err_t err = qwen_connect_once(handle);
        if (err == ESP_OK || !qwen->trial_auth) {
            return err;
        }
        if (qwen->ws) {
            esp_websocket_client_stop(qwen->ws);
            esp_websocket_client_destroy(qwen->ws);
            qwen->ws = NULL;
        }
    }
    return ESP_FAIL;
}

static esp_err_t qwen_start_stream(asr_provider_handle_t handle)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen) {
        ESP_LOGE(TAG, "start stream handle is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (!qwen->connected) {
        ESP_LOGE(TAG, "start stream without websocket connection");
        return ESP_ERR_INVALID_STATE;
    }
    if (qwen->task_running) {
        ESP_LOGE(TAG, "stream is already running");
        return ESP_ERR_INVALID_STATE;
    }

    reset_result(qwen);
    generate_task_id(qwen);
    qwen->task_started = false;
    qwen->task_running = false;
    qwen->finish_sent = false;
    xEventGroupClearBits(qwen->events, ASR_QWEN_TASK_STARTED_BIT | ASR_QWEN_TASK_FINISHED_BIT | ASR_QWEN_TASK_FAILED_BIT);

    esp_err_t err = send_run_task(qwen);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "run-task send failed: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(qwen->events, ASR_QWEN_TASK_STARTED_BIT | ASR_QWEN_TASK_FAILED_BIT | ASR_QWEN_DISCONNECTED_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(qwen->connect_timeout_ms));
    if (bits & ASR_QWEN_TASK_STARTED_BIT) {
        return ESP_OK;
    }
    if (bits & ASR_QWEN_TASK_FAILED_BIT) {
        ESP_LOGE(TAG, "remote stream start failed");
        return qwen->remote_error != ESP_OK ? qwen->remote_error : ESP_FAIL;
    }
    if (bits & ASR_QWEN_DISCONNECTED_BIT) {
        ESP_LOGE(TAG, "websocket disconnected while starting stream");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGE(TAG, "stream start timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t qwen_send_audio(asr_provider_handle_t handle, const uint8_t *data, size_t len)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen || !data) {
        ESP_LOGE(TAG, "audio send args are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (!qwen->connected || !qwen->ws || !qwen->task_started || !qwen->task_running) {
        ESP_LOGE(TAG, "audio send before stream is ready");
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t timeout = pdMS_TO_TICKS(qwen->send_timeout_ms);
    if (xSemaphoreTake(qwen->send_lock, timeout) != pdTRUE) {
        ESP_LOGE(TAG, "websocket audio send lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    int sent = esp_websocket_client_send_bin(qwen->ws, (const char *)data, (int)len, timeout);
    xSemaphoreGive(qwen->send_lock);
    if (sent <= 0) {
        ESP_LOGE(TAG, "websocket audio send failed");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "audio chunk sent: %u bytes", (unsigned)len);
    return ESP_OK;
}

static bool qwen_has_final_result(asr_provider_handle_t handle)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen) {
        return false;
    }
    xSemaphoreTake(qwen->result_lock, portMAX_DELAY);
    bool have_final = qwen->have_final;
    xSemaphoreGive(qwen->result_lock);
    return have_final;
}

static esp_err_t qwen_finish_stream(asr_provider_handle_t handle)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen) {
        ESP_LOGE(TAG, "finish stream handle is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (!qwen->connected || !qwen->ws) {
        ESP_LOGE(TAG, "finish stream without websocket connection");
        return ESP_ERR_INVALID_STATE;
    }
    if (!qwen->task_started && !qwen->finish_sent) {
        ESP_LOGE(TAG, "finish stream before task start");
        return ESP_ERR_INVALID_STATE;
    }

    if (!qwen->finish_sent) {
        esp_err_t err = send_finish_task(qwen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "finish-task send failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    EventBits_t bits = xEventGroupWaitBits(qwen->events, ASR_QWEN_TASK_FINISHED_BIT | ASR_QWEN_TASK_FAILED_BIT | ASR_QWEN_DISCONNECTED_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(qwen->connect_timeout_ms));
    if (bits & ASR_QWEN_TASK_FINISHED_BIT) {
        return ESP_OK;
    }
    if (bits & ASR_QWEN_TASK_FAILED_BIT) {
        ESP_LOGE(TAG, "remote stream finish failed");
        return qwen->remote_error != ESP_OK ? qwen->remote_error : ESP_FAIL;
    }
    if (bits & ASR_QWEN_DISCONNECTED_BIT) {
        ESP_LOGE(TAG, "websocket disconnected while finishing stream");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGE(TAG, "stream finish timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t qwen_get_final_text(asr_provider_handle_t handle, char *text, size_t text_size)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen || !text || text_size == 0) {
        ESP_LOGE(TAG, "final text args are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    text[0] = '\0';
    xSemaphoreTake(qwen->result_lock, portMAX_DELAY);
    if (!qwen->final_text || !qwen->final_text[0]) {
        xSemaphoreGive(qwen->result_lock);
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = strlen(qwen->final_text);
    if (len >= text_size) {
        memcpy(text, qwen->final_text, text_size - 1);
        text[text_size - 1] = '\0';
        xSemaphoreGive(qwen->result_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(text, qwen->final_text, len + 1);
    xSemaphoreGive(qwen->result_lock);
    return ESP_OK;
}

static void qwen_disconnect(asr_provider_handle_t handle)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen) {
        return;
    }
    if (qwen->connected && qwen->task_started && !qwen->finish_sent) {
        esp_err_t err = send_finish_task(qwen);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "finish-task during disconnect failed: %s", esp_err_to_name(err));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (qwen->ws) {
        esp_websocket_client_stop(qwen->ws);
        esp_websocket_client_destroy(qwen->ws);
        qwen->ws = NULL;
    }
    free(qwen->rx_text_buf);
    qwen->rx_text_buf = NULL;
    qwen->rx_text_cap = 0;
    qwen->connected = false;
    qwen->task_started = false;
    qwen->task_running = false;
    qwen->finish_sent = false;
    if (qwen->events) {
        xEventGroupClearBits(qwen->events, ASR_QWEN_CONNECTED_BIT | ASR_QWEN_TASK_STARTED_BIT | ASR_QWEN_TASK_FINISHED_BIT | ASR_QWEN_TASK_FAILED_BIT);
    }
}

static void qwen_delete(asr_provider_handle_t handle)
{
    asr_qwen_t *qwen = (asr_qwen_t *)handle;
    if (!qwen) {
        return;
    }
    qwen_disconnect(qwen);
    free(qwen->auth_header);
    free(qwen->final_text);
    if (qwen->send_lock) {
        vSemaphoreDelete(qwen->send_lock);
    }
    if (qwen->result_lock) {
        vSemaphoreDelete(qwen->result_lock);
    }
    if (qwen->events) {
        vEventGroupDelete(qwen->events);
    }
    free(qwen);
}

static const asr_provider_ops_t s_qwen_ops = {
    .create = qwen_create,
    .connect = qwen_connect,
    .start_stream = qwen_start_stream,
    .send_audio = qwen_send_audio,
    .has_final_result = qwen_has_final_result,
    .finish_stream = qwen_finish_stream,
    .get_final_text = qwen_get_final_text,
    .disconnect = qwen_disconnect,
    .delete = qwen_delete,
};

const asr_provider_ops_t *asr_provider_qwen_ops(void)
{
    return &s_qwen_ops;
}

const asr_provider_ops_t *asr_provider_trial_ops(void)
{
    return &s_qwen_ops;
}
