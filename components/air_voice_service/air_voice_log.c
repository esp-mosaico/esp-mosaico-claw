/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "air_voice_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOG_CAPACITY      240U
#define TEE_LINE_MAX      256U

static const char *TAG = "air_voice";

/* System log lines kept at Info/Debug level; Warnings and Errors from every
 * tag are kept regardless. */
static const char *const TEE_INFO_TAGS[] = {
    "audio_capture", "audio_mixer", "audio_hub", "audio_player_svc",
    "esp_player", "ES8311", "Adev_Codec", "I2S", "i2s", "AUD_",
};

/* Periodic, unrelated system noise that would drown the audio messages. */
static const struct {
    const char *tag;
    const char *text;
} TEE_IGNORE[] = {
    {"mosaico_module_mgr", "Module scan error"},
    {"i2c.master", "probe device timeout"},
};

typedef struct {
    air_voice_log_line_t *lines;
    uint32_t next_seq;          /* seq of the next line to be written */
    uint32_t floor_seq;         /* lines at or below this were cleared */
    SemaphoreHandle_t lock;
    bool capture_system;
    vprintf_like_t prev_vprintf;
    SemaphoreHandle_t tee_lock;
    char tee_buf[TEE_LINE_MAX];
} av_log_t;

static av_log_t s_log;

static void ring_append(char level, uint32_t ts_ms, const char *text, size_t len)
{
    if (!s_log.lines || !s_log.lock) {
        return;
    }
    if (xSemaphoreTake(s_log.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    air_voice_log_line_t *line = &s_log.lines[s_log.next_seq % LOG_CAPACITY];
    line->seq = s_log.next_seq++;
    line->ts_ms = ts_ms;
    line->level = level;
    if (len >= sizeof(line->text)) {
        len = sizeof(line->text) - 1U;
    }
    memcpy(line->text, text, len);
    line->text[len] = '\0';
    xSemaphoreGive(s_log.lock);
}

static bool tag_wanted(const char *tag, size_t tag_len)
{
    for (size_t i = 0; i < sizeof(TEE_INFO_TAGS) / sizeof(TEE_INFO_TAGS[0]); ++i) {
        const size_t n = strlen(TEE_INFO_TAGS[i]);
        if (tag_len >= n && strncmp(tag, TEE_INFO_TAGS[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* Parse "L (ticks) tag: message" (log v1, no colors). */
static void tee_consider(const char *line)
{
    const char level = line[0];
    if ((level != 'E' && level != 'W' && level != 'I' && level != 'D') || line[1] != ' ' ||
        line[2] != '(') {
        return;
    }
    const char *close = strchr(line + 3, ')');
    if (!close || close[1] != ' ') {
        return;
    }
    const char *tag = close + 2;
    const char *colon = strstr(tag, ": ");
    if (!colon) {
        return;
    }
    const size_t tag_len = (size_t)(colon - tag);
    if (tag_len == strlen(TAG) && strncmp(tag, TAG, tag_len) == 0) {
        return; /* our own lines are already in the ring */
    }
    if ((level == 'I' || level == 'D') && !tag_wanted(tag, tag_len)) {
        return;
    }
    for (size_t i = 0; i < sizeof(TEE_IGNORE) / sizeof(TEE_IGNORE[0]); ++i) {
        if (tag_len == strlen(TEE_IGNORE[i].tag) &&
            strncmp(tag, TEE_IGNORE[i].tag, tag_len) == 0 &&
            strstr(colon, TEE_IGNORE[i].text)) {
            return;
        }
    }
    size_t len = strlen(tag);
    while (len > 0 && (tag[len - 1U] == '\n' || tag[len - 1U] == '\r')) {
        len--;
    }
    ring_append(level, (uint32_t)(esp_timer_get_time() / 1000), tag, len);
}

static int tee_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int ret = s_log.prev_vprintf ? s_log.prev_vprintf(fmt, args) : vprintf(fmt, args);
    if (s_log.capture_system && !xPortInIsrContext() &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_log.tee_lock, 0) == pdTRUE) {
        if (vsnprintf(s_log.tee_buf, sizeof(s_log.tee_buf), fmt, copy) > 0) {
            tee_consider(s_log.tee_buf);
        }
        xSemaphoreGive(s_log.tee_lock);
    }
    va_end(copy);
    return ret;
}

esp_err_t air_voice_log_init(void)
{
    if (s_log.lines) {
        return ESP_OK;
    }
    s_log.lock = xSemaphoreCreateMutex();
    s_log.tee_lock = xSemaphoreCreateMutex();
    s_log.lines = heap_caps_calloc_prefer(LOG_CAPACITY, sizeof(air_voice_log_line_t), 2,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_log.lock || !s_log.tee_lock || !s_log.lines) {
        ESP_LOGE(TAG, "log ring allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s_log.next_seq = 1;
    return ESP_OK;
}

void air_voice_log_capture_system(bool enable)
{
    if (enable && !s_log.prev_vprintf && s_log.tee_lock) {
        s_log.prev_vprintf = esp_log_set_vprintf(tee_vprintf);
    }
    s_log.capture_system = enable;
}

uint32_t air_voice_log_latest_seq(void)
{
    return s_log.next_seq > 0 ? s_log.next_seq - 1U : 0;
}

void air_voice_logf(char level, const char *fmt, ...)
{
    char text[AIR_VOICE_LOG_TEXT_MAX];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    switch (level) {
    case 'E': ESP_LOGE(TAG, "%s", text); break;
    case 'W': ESP_LOGW(TAG, "%s", text); break;
    default: ESP_LOGI(TAG, "%s", text); break;
    }
    char line[AIR_VOICE_LOG_TEXT_MAX];
    const int len = snprintf(line, sizeof(line), "%s: %s", TAG, text);
    ring_append(level, (uint32_t)(esp_timer_get_time() / 1000), line,
                len < 0 ? 0 : (size_t)len);
}

size_t air_voice_log_read(uint32_t after_seq, air_voice_log_line_t *out, size_t max)
{
    if (!out || max == 0 || !s_log.lines || !s_log.lock) {
        return 0;
    }
    xSemaphoreTake(s_log.lock, portMAX_DELAY);
    const uint32_t newest = s_log.next_seq;  /* exclusive */
    uint32_t oldest = newest > LOG_CAPACITY ? newest - LOG_CAPACITY : 1U;
    if (oldest <= s_log.floor_seq) {
        oldest = s_log.floor_seq + 1U;
    }
    uint32_t seq = after_seq + 1U < oldest ? oldest : after_seq + 1U;
    size_t count = 0;
    while (seq < newest && count < max) {
        out[count++] = s_log.lines[seq % LOG_CAPACITY];
        seq++;
    }
    xSemaphoreGive(s_log.lock);
    return count;
}

void air_voice_log_clear(void)
{
    if (!s_log.lines || !s_log.lock) {
        return;
    }
    xSemaphoreTake(s_log.lock, portMAX_DELAY);
    /* Sequence numbers stay monotonic so pollers simply see no new lines;
     * everything up to the current seq becomes unreadable. */
    s_log.floor_seq = s_log.next_seq - 1U;
    xSemaphoreGive(s_log.lock);
}
