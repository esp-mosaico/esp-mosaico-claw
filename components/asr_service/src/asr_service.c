/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "asr_service.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "asr_provider.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "trial_auth.h"

static const char *TAG = "asr_service";

#define ASR_SERVICE_OWNER_TAG          "asr_service/system"
#define ASR_SERVICE_DEFAULT_CONNECT_MS 30000
#define ASR_SERVICE_DEFAULT_SEND_MS    3000
#define ASR_SERVICE_DEFAULT_PREBUFFER_MS 10000
#define ASR_SERVICE_MAX_PREBUFFER_MS   30000
#define ASR_SERVICE_FRAME_MS           20
#define ASR_SERVICE_FRAME_BYTES        ((ASR_PROVIDER_SAMPLE_RATE * ASR_SERVICE_FRAME_MS / 1000) * ASR_PROVIDER_CHANNELS * (ASR_PROVIDER_BITS / 8))
#define ASR_SERVICE_SEND_CHUNK_MS      80
#define ASR_SERVICE_SEND_CHUNK_BYTES   ((ASR_PROVIDER_SAMPLE_RATE * ASR_SERVICE_SEND_CHUNK_MS / 1000) * ASR_PROVIDER_CHANNELS * (ASR_PROVIDER_BITS / 8))
#define ASR_SERVICE_BYTES_PER_MS       ((ASR_PROVIDER_SAMPLE_RATE * ASR_PROVIDER_CHANNELS * (ASR_PROVIDER_BITS / 8)) / 1000)
#define ASR_SERVICE_STR_LEN            320
#define ASR_SERVICE_MODEL_LEN          64
#define ASR_SERVICE_SHORT_STR_LEN      32
#define ASR_SERVICE_CAPTURE_TASK_STACK 3072
#define ASR_SERVICE_SENDER_TASK_STACK  4096
#define ASR_SERVICE_CAPTURE_TASK_PRIO  10
#define ASR_SERVICE_SENDER_TASK_PRIO   9
#define ASR_SERVICE_CAPTURE_DONE_BIT   BIT0
#define ASR_SERVICE_SENDER_DONE_BIT    BIT1
#define ASR_SERVICE_LOUDNESS_STRIDE     4U
#define ASR_SERVICE_LOUDNESS_GATE       64U
#define ASR_SERVICE_NOISE_INITIAL       256U
#define ASR_SERVICE_NOISE_TRACK_WINDOW  800U

typedef enum {
    ASR_SERVICE_STATE_IDLE = 0,
    ASR_SERVICE_STATE_STARTING,
    ASR_SERVICE_STATE_STREAMING,
    ASR_SERVICE_STATE_STOPPING,
} asr_service_state_t;

struct asr_service_t {
    audio_capture_handle_t capture;
    const asr_provider_ops_t *provider_ops;
    asr_provider_handle_t provider;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t control_lock;
    SemaphoreHandle_t pcm_lock;
    SemaphoreHandle_t pcm_ready;
    EventGroupHandle_t events;
    TaskHandle_t capture_task;
    TaskHandle_t sender_task;
    audio_capture_sub_handle_t sub;
    uint8_t *frame;
    uint8_t *send_frame;
    uint8_t *prebuffer;
    size_t prebuffer_capacity;
    size_t prebuffer_read;
    size_t prebuffer_used;
    size_t prebuffer_dropped;
    size_t captured_bytes;
    size_t sent_bytes;
    uint64_t capture_ring_dropped;
    uint32_t loudness_level;
    uint32_t loudness_noise_floor;
    uint32_t loudness_envelope;
    uint32_t loudness_frames;
    asr_service_state_t state;
    asr_service_event_cb_t cb;
    void *cb_user_ctx;
    char *last_text;
    volatile bool stop_requested;
    volatile bool capture_end_requested;
    volatile bool cancel_requested;
    volatile bool provider_ready;
    volatile bool capture_done;
    esp_err_t stream_error;
    char api_key[ASR_SERVICE_STR_LEN];
    char workspace_id[ASR_SERVICE_STR_LEN];
    char endpoint[ASR_SERVICE_STR_LEN];
    char model[ASR_SERVICE_MODEL_LEN];
    char language_hint[ASR_SERVICE_SHORT_STR_LEN];
    uint32_t connect_timeout_ms;
    uint32_t send_timeout_ms;
    uint32_t prebuffer_ms;
};

static void asr_service_capture_task(void *arg);
static void asr_service_sender_task(void *arg);

static const char *str_or_default(const char *value, const char *fallback)
{
    return (value && value[0]) ? value : fallback;
}

static uint32_t value_or_default(uint32_t value, uint32_t fallback)
{
    return value ? value : fallback;
}

static uint8_t loudness_compress(uint32_t signal)
{
    if (signal <= ASR_SERVICE_LOUDNESS_GATE) return 0;
    signal -= ASR_SERVICE_LOUDNESS_GATE;
    if (signal < 512U) {
        return (uint8_t)(signal >> 2);
    }
    if (signal < 2048U) {
        return (uint8_t)(128U + ((signal - 512U) >> 4));
    }
    uint32_t level = 224U + ((signal - 2048U) >> 7);
    return (uint8_t)(level > UINT8_MAX ? UINT8_MAX : level);
}

static void update_loudness(asr_service_handle_t handle,
                            const void *pcm, size_t bytes)
{
    const int16_t *samples = pcm;
    const size_t sample_count = bytes / sizeof(*samples);
    uint32_t sum = 0;
    uint32_t count = 0;
    for (size_t i = 0; i < sample_count; i += ASR_SERVICE_LOUDNESS_STRIDE) {
        const int32_t sample = samples[i];
        sum += (uint32_t)(sample < 0 ? -sample : sample);
        count++;
    }
    if (count == 0) return;

    const uint32_t mean = sum / count;
    uint32_t noise = handle->loudness_noise_floor;
    if (handle->loudness_frames++ == 0) {
        noise = mean < 512U ? mean : ASR_SERVICE_NOISE_INITIAL;
    } else if (mean > noise &&
               mean - noise < ASR_SERVICE_NOISE_TRACK_WINDOW) {
        noise += (mean - noise + 63U) >> 6;
    } else if (mean < noise) {
        noise -= (noise - mean + 31U) >> 5;
    }
    handle->loudness_noise_floor = noise;

    const uint32_t signal = mean > noise ? mean - noise : 0;
    const uint32_t target = loudness_compress(signal);
    uint32_t envelope = handle->loudness_envelope;
    if (target > envelope) {
        envelope += (target - envelope + 1U) >> 1;
    } else {
        envelope -= (envelope - target + 7U) >> 3;
    }
    handle->loudness_envelope = envelope;
    __atomic_store_n(&handle->loudness_level, envelope, __ATOMIC_RELAXED);
}

static void *alloc_prebuffer(size_t bytes)
{
    void *buffer = heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return buffer ? buffer : malloc(bytes);
}

static esp_err_t copy_required_string(char *dst, size_t dst_size, const char *src, const char *name)
{
    if (!src || !src[0]) {
        ESP_LOGE(TAG, "ASR %s is empty", name);
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(src) >= dst_size) {
        ESP_LOGE(TAG, "ASR %s is too long", name);
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
        ESP_LOGE(TAG, "ASR %s is too long", name);
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(dst, src, dst_size);
    return ESP_OK;
}

static const asr_provider_ops_t *get_provider_ops(asr_service_provider_t provider)
{
    switch (provider) {
    case ASR_SERVICE_PROVIDER_QWEN:
        return asr_provider_qwen_ops();
    case ASR_SERVICE_PROVIDER_TRIAL:
        return asr_provider_trial_ops();
    default:
        return NULL;
    }
}

static void clear_result_locked(asr_service_handle_t handle)
{
    free(handle->last_text);
    handle->last_text = NULL;
}

static esp_err_t store_result(asr_service_handle_t handle, const char *text)
{
    if (!handle || !text) {
        ESP_LOGE(TAG, "ASR result store args are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(text) + 1;
    char *copy = malloc(len);
    if (!copy) {
        ESP_LOGE(TAG, "ASR result alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, text, len);

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    clear_result_locked(handle);
    handle->last_text = copy;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

static esp_err_t copy_result_locked(asr_service_handle_t handle, char *text, size_t text_size)
{
    if (!handle->last_text || !handle->last_text[0]) {
        text[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = strlen(handle->last_text);
    if (len >= text_size) {
        memcpy(text, handle->last_text, text_size - 1);
        text[text_size - 1] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(text, handle->last_text, len + 1);
    return ESP_OK;
}

static void prebuffer_reset(asr_service_handle_t handle)
{
    xSemaphoreTake(handle->pcm_lock, portMAX_DELAY);
    handle->prebuffer_read = 0;
    handle->prebuffer_used = 0;
    handle->prebuffer_dropped = 0;
    handle->captured_bytes = 0;
    handle->sent_bytes = 0;
    xSemaphoreGive(handle->pcm_lock);
}

static bool pcm_queue_push(asr_service_handle_t handle,
                           const uint8_t *data, size_t bytes)
{
    if (!handle->prebuffer || handle->prebuffer_capacity == 0 ||
        !data || bytes == 0) {
        return false;
    }

    xSemaphoreTake(handle->pcm_lock, portMAX_DELAY);
    handle->captured_bytes += bytes;
    const size_t free_bytes = handle->prebuffer_capacity -
                              handle->prebuffer_used;
    if (bytes > free_bytes) {
        handle->prebuffer_dropped += bytes;
        xSemaphoreGive(handle->pcm_lock);
        return false;
    }

    const size_t write = (handle->prebuffer_read +
                          handle->prebuffer_used) %
                         handle->prebuffer_capacity;
    const size_t first = bytes < handle->prebuffer_capacity - write
                             ? bytes : handle->prebuffer_capacity - write;
    memcpy(handle->prebuffer + write, data, first);
    if (first < bytes) {
        memcpy(handle->prebuffer, data + first, bytes - first);
    }
    handle->prebuffer_used += bytes;
    xSemaphoreGive(handle->pcm_lock);
    xSemaphoreGive(handle->pcm_ready);
    return true;
}

static size_t pcm_queue_pop(asr_service_handle_t handle, uint8_t *data,
                            size_t capacity)
{
    if (!handle || !data || capacity == 0) {
        return 0;
    }

    xSemaphoreTake(handle->pcm_lock, portMAX_DELAY);
    size_t bytes = handle->prebuffer_used < capacity
                       ? handle->prebuffer_used : capacity;
    const size_t first = bytes < handle->prebuffer_capacity -
                                     handle->prebuffer_read
                             ? bytes
                             : handle->prebuffer_capacity -
                                   handle->prebuffer_read;
    if (first > 0) {
        memcpy(data, handle->prebuffer + handle->prebuffer_read, first);
    }
    if (first < bytes) {
        memcpy(data + first, handle->prebuffer, bytes - first);
    }
    handle->prebuffer_read =
        (handle->prebuffer_read + bytes) % handle->prebuffer_capacity;
    handle->prebuffer_used -= bytes;
    xSemaphoreGive(handle->pcm_lock);
    return bytes;
}

static size_t pcm_queue_used(asr_service_handle_t handle)
{
    xSemaphoreTake(handle->pcm_lock, portMAX_DELAY);
    const size_t used = handle->prebuffer_used;
    xSemaphoreGive(handle->pcm_lock);
    return used;
}

static void pcm_note_sent(asr_service_handle_t handle, size_t bytes)
{
    xSemaphoreTake(handle->pcm_lock, portMAX_DELAY);
    handle->sent_bytes += bytes;
    xSemaphoreGive(handle->pcm_lock);
}

static void notify_event(asr_service_handle_t handle, asr_service_event_t event, esp_err_t error, const char *text)
{
    asr_service_event_cb_t cb = NULL;
    void *user_ctx = NULL;

    if (!handle) {
        return;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    cb = handle->cb;
    user_ctx = handle->cb_user_ctx;
    xSemaphoreGive(handle->lock);

    if (cb) {
        asr_service_event_data_t data = {
            .event = event,
            .error = error,
            .text = text,
        };
        cb(handle, &data, user_ctx);
    }
}

static void note_stream_error(asr_service_handle_t handle, esp_err_t err)
{
    bool notify = false;

    if (!handle || err == ESP_OK) {
        return;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->stream_error == ESP_OK) {
        handle->stream_error = err;
        notify = true;
    }
    handle->stop_requested = true;
    if (handle->state == ASR_SERVICE_STATE_STREAMING) {
        handle->state = ASR_SERVICE_STATE_STOPPING;
    }
    xSemaphoreGive(handle->lock);
    if (handle->pcm_ready) {
        xSemaphoreGive(handle->pcm_ready);
    }

    if (notify) {
        notify_event(handle, ASR_SERVICE_EVENT_ERROR, err, NULL);
    }
}

static void provider_result_cb(asr_provider_handle_t provider, asr_provider_result_type_t type, const char *text, void *user_ctx)
{
    (void)provider;
    asr_service_handle_t handle = (asr_service_handle_t)user_ctx;
    if (!handle || !text || !text[0]) {
        return;
    }

    if (type == ASR_PROVIDER_RESULT_FINAL) {
        esp_err_t err = store_result(handle, text);
        if (err != ESP_OK) {
            note_stream_error(handle, err);
            return;
        }
        notify_event(handle, ASR_SERVICE_EVENT_FINAL, ESP_OK, text);
        return;
    }
    notify_event(handle, ASR_SERVICE_EVENT_PARTIAL, ESP_OK, text);
}

esp_err_t asr_service_create(const asr_service_config_t *config, asr_service_handle_t *ret_handle)
{
    if (!config || !ret_handle) {
        ESP_LOGE(TAG, "ASR create args are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    *ret_handle = NULL;
    if (!config->capture) {
        ESP_LOGE(TAG, "ASR capture handle is unavailable");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->provider != ASR_SERVICE_PROVIDER_TRIAL &&
            (!config->api_key || !config->api_key[0])) {
        ESP_LOGE(TAG, "ASR api key is empty");
        return ESP_ERR_INVALID_ARG;
    }

    const asr_provider_ops_t *ops = get_provider_ops(config->provider);
    if (!ops) {
        ESP_LOGE(TAG, "ASR provider is unsupported: %d", (int)config->provider);
        return ESP_ERR_NOT_SUPPORTED;
    }

    asr_service_handle_t service = calloc(1, sizeof(*service));
    if (!service) {
        ESP_LOGE(TAG, "ASR service alloc failed");
        return ESP_ERR_NO_MEM;
    }

    service->capture = config->capture;
    service->provider_ops = ops;
    service->connect_timeout_ms = value_or_default(config->connect_timeout_ms, ASR_SERVICE_DEFAULT_CONNECT_MS);
    service->send_timeout_ms = value_or_default(config->send_timeout_ms, ASR_SERVICE_DEFAULT_SEND_MS);
    service->prebuffer_ms = value_or_default(
        config->prebuffer_ms, ASR_SERVICE_DEFAULT_PREBUFFER_MS);
    if (service->prebuffer_ms > ASR_SERVICE_MAX_PREBUFFER_MS) {
        ESP_LOGE(TAG, "ASR prebuffer is too long: %" PRIu32 " ms",
                 service->prebuffer_ms);
        free(service);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = config->provider == ASR_SERVICE_PROVIDER_TRIAL
                        ? copy_optional_string(service->api_key, sizeof(service->api_key), config->api_key, "api_key")
                        : copy_required_string(service->api_key, sizeof(service->api_key), config->api_key, "api_key");
    if (err == ESP_OK) err = copy_optional_string(service->workspace_id, sizeof(service->workspace_id), config->workspace_id, "workspace_id");
    if (err == ESP_OK) {
        err = copy_required_string(service->endpoint, sizeof(service->endpoint),
                                   str_or_default(config->endpoint,
                                                  config->provider == ASR_SERVICE_PROVIDER_TRIAL
                                                      ? TRIAL_AUTH_DEFAULT_ASR_ENDPOINT
                                                      : ASR_SERVICE_DEFAULT_ENDPOINT),
                                   "endpoint");
    }
    if (err == ESP_OK) err = copy_required_string(service->model, sizeof(service->model), str_or_default(config->model, ASR_SERVICE_DEFAULT_MODEL), "model");
    if (err == ESP_OK) {
        err = copy_optional_string(service->language_hint,
                                   sizeof(service->language_hint),
                                   str_or_default(config->language_hint, ASR_SERVICE_DEFAULT_LANGUAGE_HINT),
                                   "language_hint");
    }
    if (err != ESP_OK) {
        free(service);
        return err;
    }

    service->lock = xSemaphoreCreateMutex();
    service->control_lock = xSemaphoreCreateMutex();
    service->pcm_lock = xSemaphoreCreateMutex();
    service->pcm_ready = xSemaphoreCreateBinary();
    service->events = xEventGroupCreate();
    if (!service->lock || !service->control_lock || !service->pcm_lock ||
        !service->pcm_ready || !service->events) {
        ESP_LOGE(TAG, "ASR service runtime alloc failed");
        if (service->lock) vSemaphoreDelete(service->lock);
        if (service->control_lock) vSemaphoreDelete(service->control_lock);
        if (service->pcm_lock) vSemaphoreDelete(service->pcm_lock);
        if (service->pcm_ready) vSemaphoreDelete(service->pcm_ready);
        if (service->events) vEventGroupDelete(service->events);
        free(service);
        return ESP_ERR_NO_MEM;
    }

    asr_provider_config_t provider_config = {
        .api_key = service->api_key,
        .workspace_id = service->workspace_id[0] ? service->workspace_id : NULL,
        .endpoint = service->endpoint,
        .model = service->model,
        .language_hint = service->language_hint,
        .trial_auth = config->provider == ASR_SERVICE_PROVIDER_TRIAL,
        .connect_timeout_ms = service->connect_timeout_ms,
        .send_timeout_ms = service->send_timeout_ms,
        .result_cb = provider_result_cb,
        .result_user_ctx = service,
    };
    err = service->provider_ops->create(&provider_config, &service->provider);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ASR provider create failed: %s", esp_err_to_name(err));
        vEventGroupDelete(service->events);
        vSemaphoreDelete(service->pcm_ready);
        vSemaphoreDelete(service->pcm_lock);
        vSemaphoreDelete(service->control_lock);
        vSemaphoreDelete(service->lock);
        free(service);
        return err;
    }

    service->prebuffer_capacity =
        (size_t)service->prebuffer_ms * ASR_SERVICE_BYTES_PER_MS;
    service->frame = malloc(ASR_SERVICE_FRAME_BYTES);
    service->send_frame = malloc(ASR_SERVICE_SEND_CHUNK_BYTES);
    service->prebuffer = alloc_prebuffer(service->prebuffer_capacity);
    if (!service->frame || !service->send_frame || !service->prebuffer) {
        ESP_LOGE(TAG, "ASR capture buffer alloc failed (prebuffer=%u bytes)",
                 (unsigned)service->prebuffer_capacity);
        service->provider_ops->delete(service->provider);
        free(service->frame);
        free(service->send_frame);
        free(service->prebuffer);
        vEventGroupDelete(service->events);
        vSemaphoreDelete(service->pcm_ready);
        vSemaphoreDelete(service->pcm_lock);
        vSemaphoreDelete(service->control_lock);
        vSemaphoreDelete(service->lock);
        free(service);
        return ESP_ERR_NO_MEM;
    }
    prebuffer_reset(service);

    service->state = ASR_SERVICE_STATE_IDLE;
    *ret_handle = service;
    return ESP_OK;
}

esp_err_t asr_service_register_cb(asr_service_handle_t handle, asr_service_event_cb_t cb, void *user_ctx)
{
    if (!handle) {
        ESP_LOGE(TAG, "ASR callback register handle is invalid");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->cb = cb;
    handle->cb_user_ctx = user_ctx;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t asr_service_start(asr_service_handle_t handle)
{
    if (!handle) {
        ESP_LOGE(TAG, "ASR start handle is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->control_lock, portMAX_DELAY);

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->state != ASR_SERVICE_STATE_IDLE) {
        ESP_LOGE(TAG, "ASR start rejected in state %d", (int)handle->state);
        xSemaphoreGive(handle->lock);
        xSemaphoreGive(handle->control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    handle->state = ASR_SERVICE_STATE_STARTING;
    handle->stop_requested = false;
    handle->capture_end_requested = false;
    handle->cancel_requested = false;
    handle->provider_ready = false;
    handle->capture_done = false;
    handle->capture_ring_dropped = 0;
    handle->stream_error = ESP_OK;
    handle->loudness_noise_floor = ASR_SERVICE_NOISE_INITIAL;
    handle->loudness_envelope = 0;
    handle->loudness_frames = 0;
    __atomic_store_n(&handle->loudness_level, 0, __ATOMIC_RELAXED);
    clear_result_locked(handle);
    xEventGroupClearBits(handle->events,
                         ASR_SERVICE_CAPTURE_DONE_BIT |
                             ASR_SERVICE_SENDER_DONE_BIT);
    xSemaphoreGive(handle->lock);
    (void)xSemaphoreTake(handle->pcm_ready, 0);

    esp_err_t err = ESP_OK;
    audio_capture_sub_handle_t sub = NULL;
    uint8_t *frame = handle->frame;
    uint8_t *prebuffer = handle->prebuffer;
    TaskHandle_t capture_task = NULL;
    TaskHandle_t sender_task = NULL;
    audio_capture_sub_format_t fmt = {
        .sample_rate = ASR_PROVIDER_SAMPLE_RATE,
        .channels = ASR_PROVIDER_CHANNELS,
        .bits = ASR_PROVIDER_BITS,
    };

    if (!frame || !prebuffer) {
        ESP_LOGE(TAG, "ASR capture buffer alloc failed (prebuffer=%u bytes)",
                 (unsigned)handle->prebuffer_capacity);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = audio_capture_open_subscriber(handle->capture, AUDIO_CAPTURE_SUB_SYSTEM, &fmt, ASR_SERVICE_OWNER_TAG, &sub);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ASR system capture subscriber is busy: %s", esp_err_to_name(err));
        goto fail;
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->sub = sub;
    prebuffer_reset(handle);
    xSemaphoreGive(handle->lock);

    BaseType_t task_ok = xTaskCreate(asr_service_capture_task, "asr_capture",
        ASR_SERVICE_CAPTURE_TASK_STACK, handle, ASR_SERVICE_CAPTURE_TASK_PRIO,
        &capture_task);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "ASR capture task create failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->capture_task = capture_task;
    xSemaphoreGive(handle->lock);

    ESP_LOGI(TAG, "local capture started, connecting ASR provider "
             "(pcm_queue=%" PRIu32 " ms)", handle->prebuffer_ms);
    err = handle->provider_ops->connect(handle->provider);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ASR provider connect failed: %s", esp_err_to_name(err));
        goto fail;
    }
    if (handle->cancel_requested || handle->stop_requested) {
        ESP_LOGI(TAG, "ASR startup cancelled after provider connect");
        err = handle->stream_error != ESP_OK ? handle->stream_error
                                             : ESP_ERR_INVALID_STATE;
        goto fail;
    }
    err = handle->provider_ops->start_stream(handle->provider);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ASR stream start failed: %s", esp_err_to_name(err));
        goto fail;
    }
    if (handle->cancel_requested || handle->stop_requested) {
        ESP_LOGI(TAG, "ASR startup cancelled after stream start");
        err = handle->stream_error != ESP_OK ? handle->stream_error
                                             : ESP_ERR_INVALID_STATE;
        goto fail;
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->provider_ready = true;
    xSemaphoreGive(handle->lock);

    task_ok = xTaskCreate(asr_service_sender_task, "asr_sender",
        ASR_SERVICE_SENDER_TASK_STACK, handle, ASR_SERVICE_SENDER_TASK_PRIO,
        &sender_task);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "ASR sender task create failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->sender_task = sender_task;
    handle->state = ASR_SERVICE_STATE_STREAMING;
    xSemaphoreGive(handle->lock);
    xSemaphoreGive(handle->pcm_ready);
    xSemaphoreGive(handle->control_lock);

    notify_event(handle, ASR_SERVICE_EVENT_STARTED, ESP_OK, NULL);
    return ESP_OK;

fail:
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->stop_requested = true;
    handle->capture_end_requested = true;
    xSemaphoreGive(handle->lock);
    xSemaphoreGive(handle->pcm_ready);
    if (capture_task) {
        (void)xEventGroupWaitBits(handle->events,
            ASR_SERVICE_CAPTURE_DONE_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    }
    if (sender_task) {
        (void)xEventGroupWaitBits(handle->events,
            ASR_SERVICE_SENDER_DONE_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    }
    handle->provider_ops->disconnect(handle->provider);
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    const bool sub_still_open = sub && handle->sub == sub;
    xSemaphoreGive(handle->lock);
    if (sub_still_open) {
        esp_err_t close_err = audio_capture_close_subscriber(sub);
        if (close_err != ESP_OK) {
            ESP_LOGW(TAG, "ASR capture subscriber close failed after start error: %s", esp_err_to_name(close_err));
        }
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->sub = NULL;
    prebuffer_reset(handle);
    handle->capture_task = NULL;
    handle->sender_task = NULL;
    handle->state = ASR_SERVICE_STATE_IDLE;
    handle->stop_requested = false;
    handle->capture_end_requested = false;
    handle->cancel_requested = false;
    handle->provider_ready = false;
    handle->capture_done = false;
    handle->stream_error = err;
    xSemaphoreGive(handle->lock);
    xSemaphoreGive(handle->control_lock);

    notify_event(handle, ASR_SERVICE_EVENT_ERROR, err, NULL);
    return err;
}

esp_err_t asr_service_end_capture(asr_service_handle_t handle, bool discard)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->state == ASR_SERVICE_STATE_IDLE) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    handle->capture_end_requested = true;
    if (discard) handle->cancel_requested = true;
    xSemaphoreGive(handle->lock);
    xSemaphoreGive(handle->pcm_ready);
    return ESP_OK;
}

static void asr_service_close_capture_sub(asr_service_handle_t handle,
                                          audio_capture_sub_handle_t sub)
{
    if (!handle || !sub) {
        return;
    }

    uint64_t dropped = 0;
    esp_err_t stats_err = audio_capture_sub_get_dropped_bytes(sub, &dropped);
    if (stats_err != ESP_OK) {
        ESP_LOGW(TAG, "ASR capture drop stats unavailable: %s",
                 esp_err_to_name(stats_err));
    }
    esp_err_t close_err = audio_capture_close_subscriber(sub);
    if (close_err != ESP_OK) {
        ESP_LOGW(TAG, "ASR capture subscriber close failed at capture fence: %s",
                 esp_err_to_name(close_err));
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->capture_ring_dropped = dropped;
    if (close_err == ESP_OK && handle->sub == sub) {
        handle->sub = NULL;
    }
    xSemaphoreGive(handle->lock);
}

static void asr_service_capture_task(void *arg)
{
    asr_service_handle_t handle = (asr_service_handle_t)arg;
    audio_capture_sub_handle_t sub = NULL;
    uint8_t *frame = NULL;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    sub = handle->sub;
    frame = handle->frame;
    xSemaphoreGive(handle->lock);

    if (!sub || !frame) {
        ESP_LOGE(TAG, "ASR capture task resources are invalid");
        note_stream_error(handle, ESP_ERR_INVALID_STATE);
        asr_service_close_capture_sub(handle, sub);
        handle->capture_done = true;
        xEventGroupSetBits(handle->events, ASR_SERVICE_CAPTURE_DONE_BIT);
        xSemaphoreGive(handle->pcm_ready);
        vTaskDelete(NULL);
        return;
    }

    while (!handle->stop_requested) {
        if (handle->cancel_requested) {
            break;
        }

        if (handle->capture_end_requested) {
            /* Capture one in-flight hardware frame, then drain everything the
             * subscriber already owns before publishing the release fence. */
            size_t got = audio_capture_sub_read(
                sub, frame, ASR_SERVICE_FRAME_BYTES,
                ASR_SERVICE_FRAME_MS + 10);
            if (got > 0) {
                update_loudness(handle, frame, got);
                if (!pcm_queue_push(handle, frame, got)) {
                    ESP_LOGE(TAG, "ASR PCM queue overflow while draining tail");
                    note_stream_error(handle, ESP_ERR_NO_MEM);
                }
            }
            while (!handle->stop_requested && !handle->cancel_requested) {
                got = audio_capture_sub_read(sub, frame,
                                             ASR_SERVICE_FRAME_BYTES, 0);
                if (got == 0) {
                    break;
                }
                update_loudness(handle, frame, got);
                if (!pcm_queue_push(handle, frame, got)) {
                    ESP_LOGE(TAG, "ASR PCM queue overflow while draining tail");
                    note_stream_error(handle, ESP_ERR_NO_MEM);
                    break;
                }
            }
            break;
        }

        size_t got = audio_capture_sub_read(
            sub, frame, ASR_SERVICE_FRAME_BYTES,
            ASR_SERVICE_FRAME_MS + 10);
        if (got == 0) {
            continue;
        }
        update_loudness(handle, frame, got);
        if (!pcm_queue_push(handle, frame, got)) {
            ESP_LOGE(TAG, "ASR PCM queue overflow: capacity=%u bytes",
                     (unsigned)handle->prebuffer_capacity);
            note_stream_error(handle, ESP_ERR_NO_MEM);
            break;
        }
    }

    /* Closing here is part of the capture fence. The audio hub must stop
     * feeding this subscriber while queued PCM and cloud finalization run. */
    asr_service_close_capture_sub(handle, sub);
    handle->capture_done = true;
    xEventGroupSetBits(handle->events, ASR_SERVICE_CAPTURE_DONE_BIT);
    xSemaphoreGive(handle->pcm_ready);
    vTaskDelete(NULL);
}

static void asr_service_sender_task(void *arg)
{
    asr_service_handle_t handle = (asr_service_handle_t)arg;
    uint8_t *send_frame = handle->send_frame;
    if (!send_frame) {
        ESP_LOGE(TAG, "ASR sender task buffer is invalid");
        note_stream_error(handle, ESP_ERR_INVALID_STATE);
        xEventGroupSetBits(handle->events, ASR_SERVICE_SENDER_DONE_BIT);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ASR provider ready, sender draining PCM queue");
    while (!handle->stop_requested && !handle->cancel_requested) {
        const size_t used = pcm_queue_used(handle);
        if (used == 0) {
            if (handle->capture_done) {
                break;
            }
            (void)xSemaphoreTake(handle->pcm_ready,
                                 pdMS_TO_TICKS(ASR_SERVICE_FRAME_MS));
            continue;
        }
        if (used < ASR_SERVICE_SEND_CHUNK_BYTES && !handle->capture_done) {
            vTaskDelay(pdMS_TO_TICKS(ASR_SERVICE_FRAME_MS));
            continue;
        }

        const size_t got = pcm_queue_pop(handle, send_frame,
                                         ASR_SERVICE_SEND_CHUNK_BYTES);
        if (got == 0) {
            continue;
        }
        esp_err_t err = handle->provider_ops->send_audio(
            handle->provider, send_frame, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ASR audio send failed: %s", esp_err_to_name(err));
            note_stream_error(handle, err);
            break;
        }
        pcm_note_sent(handle, got);
    }

    xEventGroupSetBits(handle->events, ASR_SERVICE_SENDER_DONE_BIT);
    vTaskDelete(NULL);
}

static esp_err_t stop_locked(asr_service_handle_t handle, char *text, size_t text_size)
{
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (handle->state == ASR_SERVICE_STATE_IDLE || handle->state == ASR_SERVICE_STATE_STARTING) {
        ESP_LOGE(TAG, "ASR stop rejected in state %d", (int)handle->state);
        xSemaphoreGive(handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    handle->state = ASR_SERVICE_STATE_STOPPING;
    handle->capture_end_requested = true;
    bool discard = text == NULL || text_size == 0;
    if (discard) {
        handle->cancel_requested = true;
        handle->stop_requested = true;
    }
    TaskHandle_t capture_task = handle->capture_task;
    TaskHandle_t sender_task = handle->sender_task;
    xSemaphoreGive(handle->lock);
    xSemaphoreGive(handle->pcm_ready);

    EventBits_t wait_bits = 0;
    if (capture_task) {
        wait_bits |= ASR_SERVICE_CAPTURE_DONE_BIT;
    }
    if (sender_task) {
        wait_bits |= ASR_SERVICE_SENDER_DONE_BIT;
    }
    if (wait_bits != 0) {
        xEventGroupWaitBits(handle->events, wait_bits, pdTRUE, pdTRUE,
                            portMAX_DELAY);
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    discard = discard || handle->cancel_requested;
    handle->stop_requested = true;
    esp_err_t stream_err = handle->stream_error;
    handle->capture_task = NULL;
    handle->sender_task = NULL;
    xSemaphoreGive(handle->lock);

    xSemaphoreTake(handle->pcm_lock, portMAX_DELAY);
    const size_t captured_bytes = handle->captured_bytes;
    const size_t sent_bytes = handle->sent_bytes;
    const size_t queued_bytes = handle->prebuffer_used;
    const size_t dropped_bytes = handle->prebuffer_dropped;
    xSemaphoreGive(handle->pcm_lock);
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    const uint64_t capture_ring_dropped = handle->capture_ring_dropped;
    xSemaphoreGive(handle->lock);
    ESP_LOGI(TAG, "ASR PCM summary: captured=%u sent=%u queued=%u "
             "queue_dropped=%u capture_dropped=%" PRIu64,
             (unsigned)captured_bytes, (unsigned)sent_bytes,
             (unsigned)queued_bytes, (unsigned)dropped_bytes,
             capture_ring_dropped);
    if (capture_ring_dropped > 0 && stream_err == ESP_OK) {
        stream_err = ESP_ERR_INVALID_SIZE;
        notify_event(handle, ASR_SERVICE_EVENT_ERROR, stream_err, NULL);
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    discard = discard || handle->cancel_requested;
    xSemaphoreGive(handle->lock);
    esp_err_t finish_err = ESP_OK;
    if (!discard) {
        finish_err = handle->provider_ops->finish_stream(handle->provider);
        if (finish_err != ESP_OK) {
            ESP_LOGE(TAG, "ASR stream finish failed: %s",
                     esp_err_to_name(finish_err));
        }
    }

    esp_err_t text_err = ESP_OK;
    if (!discard && text && text_size > 0) {
        text_err = handle->provider_ops->get_final_text(handle->provider, text, text_size);
        if (text_err == ESP_OK || text_err == ESP_ERR_INVALID_SIZE) {
            esp_err_t store_err = store_result(handle, text);
            if (store_err != ESP_OK && stream_err == ESP_OK) {
                stream_err = store_err;
            }
        }
        if (text_err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "ASR final text not found");
        } else if (text_err == ESP_ERR_INVALID_SIZE) {
            ESP_LOGW(TAG, "ASR final text truncated");
        } else if (text_err != ESP_OK) {
            ESP_LOGE(TAG, "ASR final text copy failed: %s", esp_err_to_name(text_err));
        }
    }

    handle->provider_ops->disconnect(handle->provider);
    if (handle->sub) {
        esp_err_t close_err = audio_capture_close_subscriber(handle->sub);
        if (close_err != ESP_OK) {
            ESP_LOGW(TAG, "ASR capture subscriber close failed: %s", esp_err_to_name(close_err));
        }
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->sub = NULL;
    prebuffer_reset(handle);
    handle->state = ASR_SERVICE_STATE_IDLE;
    handle->stop_requested = false;
    handle->capture_end_requested = false;
    handle->cancel_requested = false;
    handle->provider_ready = false;
    handle->capture_done = false;
    handle->capture_ring_dropped = 0;
    handle->stream_error = ESP_OK;
    xSemaphoreGive(handle->lock);

    esp_err_t ret = stream_err != ESP_OK ? stream_err : (finish_err != ESP_OK ? finish_err : text_err);
    notify_event(handle, ASR_SERVICE_EVENT_STOPPED, ret,
                 !discard && text && text_size > 0 ? text : NULL);
    return ret;
}

esp_err_t asr_service_stop(asr_service_handle_t handle, char *text, size_t text_size)
{
    if (!handle) {
        ESP_LOGE(TAG, "ASR stop handle is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if ((text && text_size == 0) || (!text && text_size > 0)) {
        ESP_LOGE(TAG, "ASR stop text args are invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (text && text_size > 0) {
        text[0] = '\0';
    }

    xSemaphoreTake(handle->control_lock, portMAX_DELAY);
    esp_err_t err = stop_locked(handle, text, text_size);
    xSemaphoreGive(handle->control_lock);
    return err;
}

esp_err_t asr_service_get_result(asr_service_handle_t handle, char *text, size_t text_size)
{
    if (!handle || !text || text_size == 0) {
        ESP_LOGE(TAG, "ASR result args are invalid");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    esp_err_t err = copy_result_locked(handle, text, text_size);
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t asr_service_get_loudness(asr_service_handle_t handle,
                                   uint8_t *out_level)
{
    if (!handle || !out_level) return ESP_ERR_INVALID_ARG;
    *out_level = (uint8_t)__atomic_load_n(
        &handle->loudness_level, __ATOMIC_RELAXED);
    return ESP_OK;
}

void asr_service_delete(asr_service_handle_t handle)
{
    if (!handle) {
        return;
    }

    if (handle->control_lock && xSemaphoreTake(handle->control_lock, portMAX_DELAY) == pdTRUE) {
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        bool running = handle->state != ASR_SERVICE_STATE_IDLE;
        xSemaphoreGive(handle->lock);
        if (running) {
            esp_err_t err = stop_locked(handle, NULL, 0);
            if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "ASR stop during delete failed: %s", esp_err_to_name(err));
            }
        }
        if (handle->provider_ops && handle->provider_ops->delete && handle->provider) {
            handle->provider_ops->delete(handle->provider);
        }
        xSemaphoreGive(handle->control_lock);
    }

    free(handle->last_text);
    free(handle->frame);
    free(handle->send_frame);
    free(handle->prebuffer);
    if (handle->events) {
        vEventGroupDelete(handle->events);
    }
    if (handle->control_lock) {
        vSemaphoreDelete(handle->control_lock);
    }
    if (handle->pcm_ready) {
        vSemaphoreDelete(handle->pcm_ready);
    }
    if (handle->pcm_lock) {
        vSemaphoreDelete(handle->pcm_lock);
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}
