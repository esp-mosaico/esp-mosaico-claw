/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "update_check_service.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define UPDATE_CHECK_DEFAULT_TIMEOUT_MS 10000U
#define UPDATE_CHECK_DEFAULT_BODY_MAX   4096U
#define UPDATE_CHECK_URL_LEN            320U
#define UPDATE_CHECK_PRODUCT_LEN        48U
#define UPDATE_CHECK_USER_AGENT_LEN     128U
#define UPDATE_CHECK_TASK_STACK         (6U * 1024U)
#define UPDATE_CHECK_TASK_PRIORITY      4U

typedef struct {
    char *body;
    size_t length;
    size_t capacity;
    size_t maximum;
    esp_err_t append_error;
    int status_code;
} update_check_http_response_t;

typedef struct {
    update_check_event_cb_t callback;
    void *user_ctx;
} update_check_observer_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    update_check_service_config_t config;
    char manifest_url[UPDATE_CHECK_URL_LEN];
    char product[UPDATE_CHECK_PRODUCT_LEN];
    char current_version[UPDATE_CHECK_VERSION_LEN];
    char user_agent[UPDATE_CHECK_USER_AGENT_LEN];
    update_check_snapshot_t snapshot;
    update_check_observer_t observers[UPDATE_CHECK_SUBSCRIBER_MAX];
    bool initialized;
} update_check_service_state_t;

static const char *TAG = "update_check";
static update_check_service_state_t s_service;

static esp_err_t update_check_http_append(
    update_check_http_response_t *response, const void *data, size_t length)
{
    if (response == NULL || data == NULL || length == 0U) {
        return ESP_OK;
    }
    if (response->length > response->maximum ||
            length > response->maximum - response->length) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t needed = response->length + length + 1U;
    if (needed > response->capacity) {
        size_t next = response->capacity != 0U ? response->capacity : 1024U;
        while (next < needed && next < response->maximum + 1U) {
            next *= 2U;
        }
        if (next > response->maximum + 1U) {
            next = response->maximum + 1U;
        }
        char *grown = realloc(response->body, next);
        if (grown == NULL) {
            return ESP_ERR_NO_MEM;
        }
        response->body = grown;
        response->capacity = next;
    }
    memcpy(response->body + response->length, data, length);
    response->length += length;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t update_check_http_event(esp_http_client_event_t *event)
{
    update_check_http_response_t *response = event != NULL
        ? (update_check_http_response_t *)event->user_data : NULL;
    if (response != NULL && event->event_id == HTTP_EVENT_ON_DATA &&
            event->data != NULL && event->data_len > 0) {
        response->append_error = update_check_http_append(
            response, event->data, (size_t)event->data_len);
        return response->append_error;
    }
    return ESP_OK;
}

static esp_err_t update_check_http_get(update_check_http_response_t *response)
{
    memset(response, 0, sizeof(*response));
    response->maximum = s_service.config.max_body_size;
    esp_http_client_config_t config = {
        .url = s_service.config.manifest_url,
        .method = HTTP_METHOD_GET,
        .event_handler = update_check_http_event,
        .user_data = response,
        .user_agent = s_service.config.user_agent,
        .timeout_ms = (int)s_service.config.timeout_ms,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG,
                        "HTTP client allocation failed");
    (void)esp_http_client_set_header(client, "Accept", "application/json");
    (void)esp_http_client_set_header(client, "Cache-Control", "no-cache");

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && response->append_error != ESP_OK) {
        err = response->append_error;
    }
    if (err == ESP_OK) {
        response->status_code = esp_http_client_get_status_code(client);
        if (response->status_code != 200 || response->body == NULL) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t copy_optional_json_string(
    const cJSON *root, const char *key, char *destination, size_t size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL || cJSON_IsNull(item)) {
        destination[0] = '\0';
        return ESP_OK;
    }
    const char *value = cJSON_GetStringValue(item);
    if (value == NULL || strlen(value) >= size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(destination, value, size);
    return ESP_OK;
}

static esp_err_t update_check_parse_manifest(
    const char *json, size_t length, update_check_snapshot_t *result)
{
    cJSON *root = cJSON_ParseWithLength(json, length);
    ESP_RETURN_ON_FALSE(root != NULL && cJSON_IsObject(root),
                        ESP_ERR_INVALID_RESPONSE, TAG,
                        "update manifest is not a JSON object");
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(
        root, "schema_version");
    const char *product = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "product"));
    const char *latest = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "latest_version"));
    if (!cJSON_IsNumber(schema) || schema->valueint != 1 ||
            product == NULL || strcmp(product, s_service.config.product) != 0 ||
            latest == NULL || strlen(latest) >= sizeof(result->latest_version)) {
        ESP_LOGW(TAG, "update manifest identity or required fields invalid");
        goto cleanup;
    }
    int comparison = 0;
    if (update_check_compare_versions(
            result->current_version, latest, &comparison) != ESP_OK) {
        ESP_LOGW(TAG, "update manifest version is not MAJOR.MINOR.PATCH");
        goto cleanup;
    }
    strlcpy(result->latest_version, latest, sizeof(result->latest_version));
    if (copy_optional_json_string(root, "title", result->title,
                                  sizeof(result->title)) != ESP_OK ||
            copy_optional_json_string(root, "summary", result->summary,
                                      sizeof(result->summary)) != ESP_OK ||
            copy_optional_json_string(root, "published_at", result->published_at,
                                      sizeof(result->published_at)) != ESP_OK) {
        ESP_LOGW(TAG, "update manifest optional text is too long or invalid");
        goto cleanup;
    }
    result->state = comparison < 0 ? UPDATE_CHECK_AVAILABLE
        : comparison > 0 ? UPDATE_CHECK_DEVICE_AHEAD
        : UPDATE_CHECK_UP_TO_DATE;
    err = ESP_OK;

cleanup:
    cJSON_Delete(root);
    return err;
}

static void update_check_notify(
    const update_check_snapshot_t *snapshot,
    const update_check_observer_t *observers)
{
    for (size_t index = 0; index < UPDATE_CHECK_SUBSCRIBER_MAX; ++index) {
        if (observers[index].callback != NULL) {
            observers[index].callback(snapshot, observers[index].user_ctx);
        }
    }
}

static void update_check_worker(void *arg)
{
    (void)arg;
    update_check_snapshot_t result = {0};
    strlcpy(result.current_version, s_service.config.current_version,
            sizeof(result.current_version));
    update_check_http_response_t response = {0};
    esp_err_t err = update_check_http_get(&response);
    if (err == ESP_OK) {
        err = update_check_parse_manifest(
            response.body, response.length, &result);
    }
    free(response.body);
    result.last_error = err;
    if (err != ESP_OK) {
        result.state = UPDATE_CHECK_FAILED;
    }

    update_check_observer_t observers[UPDATE_CHECK_SUBSCRIBER_MAX] = {0};
    if (xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE) {
        result.sequence = s_service.snapshot.sequence + 1U;
        s_service.snapshot = result;
        s_service.task = NULL;
        memcpy(observers, s_service.observers, sizeof(observers));
        xSemaphoreGive(s_service.lock);
        update_check_notify(&result, observers);
    }
    vTaskDelete(NULL);
}

esp_err_t update_check_service_init(
    const update_check_service_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->product != NULL &&
                            config->product[0] != '\0' &&
                            config->current_version != NULL &&
                            config->current_version[0] != '\0' &&
                            config->user_agent != NULL &&
                            config->user_agent[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG,
                        "update checker configuration invalid");
    if (s_service.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *url = config->manifest_url != NULL ? config->manifest_url : "";
    if (url[0] != '\0' && strncmp(url, "https://", 8U) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= sizeof(s_service.manifest_url) ||
            strlen(config->product) >= sizeof(s_service.product) ||
            strlen(config->current_version) >= sizeof(s_service.current_version) ||
            strlen(config->user_agent) >= sizeof(s_service.user_agent)) {
        return ESP_ERR_INVALID_SIZE;
    }
    int version_comparison = 0;
    ESP_RETURN_ON_ERROR(update_check_compare_versions(
                            config->current_version, config->current_version,
                            &version_comparison),
                        TAG, "current version is not MAJOR.MINOR.PATCH");
    memset(&s_service, 0, sizeof(s_service));
    s_service.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_service.lock != NULL, ESP_ERR_NO_MEM, TAG,
                        "update checker mutex allocation failed");
    strlcpy(s_service.manifest_url, url, sizeof(s_service.manifest_url));
    strlcpy(s_service.product, config->product, sizeof(s_service.product));
    strlcpy(s_service.current_version, config->current_version,
            sizeof(s_service.current_version));
    strlcpy(s_service.user_agent, config->user_agent,
            sizeof(s_service.user_agent));
    s_service.config = *config;
    s_service.config.manifest_url = s_service.manifest_url;
    s_service.config.product = s_service.product;
    s_service.config.current_version = s_service.current_version;
    s_service.config.user_agent = s_service.user_agent;
    s_service.config.timeout_ms = config->timeout_ms != 0U
        ? config->timeout_ms : UPDATE_CHECK_DEFAULT_TIMEOUT_MS;
    s_service.config.max_body_size = config->max_body_size != 0U
        ? config->max_body_size : UPDATE_CHECK_DEFAULT_BODY_MAX;
    s_service.snapshot.state = UPDATE_CHECK_IDLE;
    s_service.snapshot.last_error = ESP_OK;
    strlcpy(s_service.snapshot.current_version, s_service.current_version,
            sizeof(s_service.snapshot.current_version));
    s_service.initialized = true;
    return ESP_OK;
}

esp_err_t update_check_service_request(void)
{
    ESP_RETURN_ON_FALSE(s_service.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "update checker is not initialized");
    ESP_RETURN_ON_FALSE(s_service.config.manifest_url[0] != '\0',
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "update manifest URL is not configured");
    update_check_snapshot_t checking = {0};
    update_check_observer_t observers[UPDATE_CHECK_SUBSCRIBER_MAX] = {0};
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "update checker lock failed");
    if (s_service.task != NULL ||
            s_service.snapshot.state == UPDATE_CHECK_CHECKING) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }
    checking.state = UPDATE_CHECK_CHECKING;
    checking.last_error = ESP_OK;
    checking.sequence = s_service.snapshot.sequence + 1U;
    strlcpy(checking.current_version, s_service.current_version,
            sizeof(checking.current_version));
    s_service.snapshot = checking;
    BaseType_t created = xTaskCreate(
        update_check_worker, "update_check", UPDATE_CHECK_TASK_STACK,
        NULL, UPDATE_CHECK_TASK_PRIORITY, &s_service.task);
    if (created != pdPASS) {
        s_service.task = NULL;
        s_service.snapshot.state = UPDATE_CHECK_FAILED;
        s_service.snapshot.last_error = ESP_ERR_NO_MEM;
        s_service.snapshot.sequence++;
        checking = s_service.snapshot;
        memcpy(observers, s_service.observers, sizeof(observers));
        xSemaphoreGive(s_service.lock);
        update_check_notify(&checking, observers);
        return ESP_ERR_NO_MEM;
    }
    memcpy(observers, s_service.observers, sizeof(observers));
    xSemaphoreGive(s_service.lock);
    update_check_notify(&checking, observers);
    return ESP_OK;
}

esp_err_t update_check_service_get_snapshot(
    update_check_snapshot_t *ret_snapshot)
{
    ESP_RETURN_ON_FALSE(s_service.initialized && ret_snapshot != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "update snapshot arguments invalid");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "update snapshot lock failed");
    *ret_snapshot = s_service.snapshot;
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}

esp_err_t update_check_service_subscribe(
    update_check_event_cb_t callback, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_service.initialized && callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "update subscriber arguments invalid");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "update subscriber lock failed");
    size_t free_index = UPDATE_CHECK_SUBSCRIBER_MAX;
    for (size_t index = 0; index < UPDATE_CHECK_SUBSCRIBER_MAX; ++index) {
        if (s_service.observers[index].callback == callback &&
                s_service.observers[index].user_ctx == user_ctx) {
            xSemaphoreGive(s_service.lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (free_index == UPDATE_CHECK_SUBSCRIBER_MAX &&
                s_service.observers[index].callback == NULL) {
            free_index = index;
        }
    }
    if (free_index == UPDATE_CHECK_SUBSCRIBER_MAX) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_NO_MEM;
    }
    s_service.observers[free_index] = (update_check_observer_t) {
        .callback = callback,
        .user_ctx = user_ctx,
    };
    update_check_snapshot_t snapshot = s_service.snapshot;
    xSemaphoreGive(s_service.lock);
    callback(&snapshot, user_ctx);
    return ESP_OK;
}

esp_err_t update_check_service_unsubscribe(
    update_check_event_cb_t callback, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_service.initialized && callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "update subscriber arguments invalid");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "update subscriber lock failed");
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t index = 0; index < UPDATE_CHECK_SUBSCRIBER_MAX; ++index) {
        if (s_service.observers[index].callback == callback &&
                s_service.observers[index].user_ctx == user_ctx) {
            memset(&s_service.observers[index], 0,
                   sizeof(s_service.observers[index]));
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_service.lock);
    return err;
}
