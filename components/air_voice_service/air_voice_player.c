/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "air_voice_player.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "air_voice_log.h"

#define PLAYER_STACK       4096U
#define PLAYER_PRIO        6U
#define READ_CHUNK         4096U
#define WRITE_SLICE        640U     /* 20 ms: stop/pause react within a frame */
#define PAUSE_POLL_MS      20U
#define EXIT_WAIT_MS       2000U
#define BYTES_PER_MS       32U      /* 16 kHz * 2 bytes */

struct av_player_t {
    FILE *file;
    audio_mixer_track_handle_t track;
    uint32_t data_bytes;
    volatile uint32_t played_bytes;
    volatile bool stop;
    volatile bool paused;
    volatile bool finished;
    volatile esp_err_t error;
    SemaphoreHandle_t exited;
    uint8_t *buf;
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Leaves the file positioned at the first PCM byte. */
static esp_err_t parse_wav(FILE *file, uint32_t rate, uint8_t channels, uint8_t bits,
                           uint32_t *out_data_bytes)
{
    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    bool fmt_ok = false;
    for (int guard = 0; guard < 16; ++guard) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t size = rd32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            fmt_ok = rd16(fmt) == 1U && rd16(fmt + 2) == channels &&
                     rd32(fmt + 4) == rate && rd16(fmt + 14) == bits;
            if (!fmt_ok) {
                AV_LOGW("player: unsupported WAV (fmt %u, %u ch, %" PRIu32 " Hz, %u bit)",
                        rd16(fmt), rd16(fmt + 2), rd32(fmt + 4), rd16(fmt + 14));
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (fseek(file, (long)(size - sizeof(fmt) + (size & 1U)), SEEK_CUR) != 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!fmt_ok) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            *out_data_bytes = size;
            return ESP_OK;
        } else if (fseek(file, (long)(size + (size & 1U)), SEEK_CUR) != 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static void player_task(void *arg)
{
    av_player_t *p = arg;
    esp_err_t err = ESP_OK;
    while (!p->stop && p->played_bytes < p->data_bytes) {
        const uint32_t remaining = p->data_bytes - p->played_bytes;
        const size_t got = fread(p->buf, 1, remaining < READ_CHUNK ? remaining : READ_CHUNK, p->file);
        if (got == 0) {
            break; /* file shorter than its header claims: treat as the end */
        }
        for (size_t off = 0; off < got && !p->stop;) {
            while (p->paused && !p->stop) {
                vTaskDelay(pdMS_TO_TICKS(PAUSE_POLL_MS));
            }
            if (p->stop) {
                break;
            }
            const size_t slice = got - off < WRITE_SLICE ? got - off : WRITE_SLICE;
            const size_t written = audio_mixer_track_write(p->track, p->buf + off, slice);
            if (written == 0) {
                err = ESP_ERR_TIMEOUT; /* mixer stalled or track closed */
                p->stop = true;
                break;
            }
            off += written;
            p->played_bytes += (uint32_t)written;
        }
    }
    p->error = err;
    p->finished = true;
    xSemaphoreGive(p->exited);
    vTaskDelete(NULL);
}

esp_err_t av_player_start(const char *path, audio_mixer_track_handle_t track,
                          av_player_t **out_player)
{
    if (!path || !track || !out_player) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t rate = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    if (audio_mixer_track_info(track, &rate, &channels, &bits) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    av_player_t *p = calloc(1, sizeof(*p));
    if (!p) {
        return ESP_ERR_NO_MEM;
    }
    p->track = track;
    p->file = fopen(path, "rb");
    p->buf = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    p->exited = xSemaphoreCreateBinary();
    esp_err_t err = (!p->file || !p->buf || !p->exited) ? ESP_ERR_NO_MEM : ESP_OK;
    if (err == ESP_OK) {
        err = parse_wav(p->file, rate, channels, bits, &p->data_bytes);
    }
    if (err == ESP_OK) {
        /* Streaming headers declare 0xFFFFFFFF bytes: trust the file size. */
        const long data_start = ftell(p->file);
        if (data_start < 0 || fseek(p->file, 0, SEEK_END) != 0) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            const long end = ftell(p->file);
            const uint32_t available = end > data_start ? (uint32_t)(end - data_start) : 0U;
            if (p->data_bytes > available) {
                p->data_bytes = available;
            }
            if (fseek(p->file, data_start, SEEK_SET) != 0) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    if (err == ESP_OK &&
        xTaskCreate(player_task, "av_player", PLAYER_STACK, p, PLAYER_PRIO, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        if (p->file) fclose(p->file);
        if (p->exited) vSemaphoreDelete(p->exited);
        heap_caps_free(p->buf);
        free(p);
        return err;
    }
    *out_player = p;
    return ESP_OK;
}

void av_player_set_paused(av_player_t *player, bool paused)
{
    if (player) {
        player->paused = paused;
    }
}

bool av_player_is_paused(const av_player_t *player)
{
    return player && player->paused;
}

bool av_player_finished(const av_player_t *player, esp_err_t *out_error)
{
    if (!player) {
        return true;
    }
    if (player->finished && out_error) {
        *out_error = player->error;
    }
    return player->finished;
}

int64_t av_player_position_ms(const av_player_t *player)
{
    return player ? player->played_bytes / BYTES_PER_MS : 0;
}

int64_t av_player_duration_ms(const av_player_t *player)
{
    return player ? player->data_bytes / BYTES_PER_MS : 0;
}

void av_player_destroy(av_player_t *player)
{
    if (!player) {
        return;
    }
    player->stop = true;
    if (xSemaphoreTake(player->exited, pdMS_TO_TICKS(EXIT_WAIT_MS)) != pdTRUE) {
        /* The task still references the player; leaking is safer than a
         * use-after-free. This only happens if the mixer is wedged. */
        AV_LOGE("player: task did not exit, leaking its state");
        return;
    }
    fclose(player->file);
    vSemaphoreDelete(player->exited);
    heap_caps_free(player->buf);
    free(player);
}
