/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include "music_audio.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#if defined(ESP_PLATFORM)
#include <stdatomic.h>
#include "audio_hub.h"
#include "audio_playback.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MUSIC_AUDIO_CHUNK_BYTES 512U

typedef struct {
    audio_mixer_track_handle_t track;
    audio_playback_handle_t player;
    SemaphoreHandle_t io_lock;
    atomic_bool closing;
} music_session_t;

struct music_audio_t {
    music_session_t *session;
    music_audio_status_t last_status;
};

/* Only one decoder may be retired at a time. A rapid re-open renders a
 * loading state and retries, rather than allocating another live decoder. */
static QueueHandle_t s_cleanup_queue;
static atomic_bool s_cleanup_pending;

static esp_err_t write_pcm(const void *data, size_t bytes, void *ctx)
{
    music_session_t *session = ctx;
    if (session == NULL) return ESP_ERR_INVALID_ARG;
    const uint8_t *cursor = data;
    while (bytes > 0) {
        if (atomic_load(&session->closing)) return ESP_OK;
        xSemaphoreTake(session->io_lock, portMAX_DELAY);
        if (atomic_load(&session->closing) || session->track == NULL) {
            xSemaphoreGive(session->io_lock);
            return ESP_OK;
        }
        const size_t chunk = bytes > MUSIC_AUDIO_CHUNK_BYTES ? MUSIC_AUDIO_CHUNK_BYTES : bytes;
        const size_t written = audio_mixer_track_write(session->track, cursor, chunk);
        xSemaphoreGive(session->io_lock);
        if (!written) return ESP_FAIL;
        cursor += written;
        bytes -= written;
    }
    return ESP_OK;
}

static void destroy_session(music_session_t *session)
{
    if (session->player != NULL) audio_playback_delete(session->player);
    if (session->track != NULL) audio_mixer_close_track(session->track);
    vSemaphoreDelete(session->io_lock);
    free(session);
}

static void cleanup_task(void *ctx)
{
    (void)ctx;
    music_session_t *session;
    for (;;) {
        if (xQueueReceive(s_cleanup_queue, &session, portMAX_DELAY) == pdTRUE) {
            destroy_session(session);
            atomic_store(&s_cleanup_pending, false);
        }
    }
}

static esp_err_t prepare_cleanup(void)
{
    if (s_cleanup_queue != NULL) return ESP_OK;
    s_cleanup_queue = xQueueCreate(1, sizeof(music_session_t *));
    if (s_cleanup_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(cleanup_task, "music_cleanup", 3072, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_cleanup_queue);
        s_cleanup_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void release_session(music_audio_handle_t audio)
{
    music_session_t *session = audio->session;
    if (session == NULL) return;
    audio->session = NULL;
    atomic_store(&session->closing, true);
    /* Fence the current PCM write, then remove queued sound and release the
     * mixer track before the next App acquires audio ownership. */
    xSemaphoreTake(session->io_lock, portMAX_DELAY);
    if (session->track != NULL) {
        audio_mixer_close_track(session->track);
        session->track = NULL;
    }
    xSemaphoreGive(session->io_lock);
    if (session->player == NULL) {
        destroy_session(session);
        return;
    }
    atomic_store(&s_cleanup_pending, true);
    if (xQueueSend(s_cleanup_queue, &session, 0) != pdTRUE) {
        /* The one-decoder gate makes this exceptional. Keep cleanup safe if
         * a future caller violates it, without leaking decoder callbacks. */
        destroy_session(session);
        atomic_store(&s_cleanup_pending, false);
    }
}

static esp_err_t open_session(music_audio_handle_t audio)
{
    if (atomic_load(&s_cleanup_pending)) return ESP_ERR_NOT_FINISHED;
    music_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) return ESP_ERR_NO_MEM;
    atomic_init(&session->closing, false);
    session->io_lock = xSemaphoreCreateMutex();
    if (session->io_lock == NULL) { free(session); return ESP_ERR_NO_MEM; }
    audio->session = session;
    audio_mixer_handle_t mixer = NULL;
    esp_err_t err = audio_hub_get_mixer(&mixer);
    if (err == ESP_OK) {
        err = audio_mixer_open_track(mixer, AUDIO_MIXER_TRACK_SYSTEM,
                                     "mosaic/music", &session->track);
    }
    uint32_t rate = 0;
    uint8_t channels = 0, bits = 0;
    if (err == ESP_OK) err = audio_mixer_track_info(session->track, &rate, &channels, &bits);
    if (err == ESP_OK) {
        const audio_playback_config_t config = {
            .output_format = {.sample_rate=rate, .channels=channels, .bits=bits},
            .write=write_pcm, .write_ctx=session,
        };
        err = audio_playback_create(&config, &session->player);
    }
    if (err != ESP_OK) release_session(audio);
    return err;
}

esp_err_t music_audio_create(music_audio_handle_t *ret_audio)
{
    if (ret_audio == NULL) return ESP_ERR_INVALID_ARG;
    *ret_audio = NULL;
    esp_err_t err = prepare_cleanup();
    if (err != ESP_OK) return err;
    *ret_audio = calloc(1, sizeof(**ret_audio));
    return *ret_audio != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void music_audio_delete(music_audio_handle_t audio)
{
    if (audio == NULL) return;
    release_session(audio);
    free(audio);
}

esp_err_t music_audio_play(music_audio_handle_t audio, const char *path, uint64_t bytes)
{
    if (audio == NULL || path == NULL || path[0] != '/') return ESP_ERR_INVALID_ARG;
    esp_err_t err = audio->session != NULL ? ESP_OK : open_session(audio);
    if (err != ESP_OK) return err;
    const size_t length = strlen("file://") + strlen(path) + 1;
    char *uri = malloc(length);
    if (uri == NULL) { release_session(audio); return ESP_ERR_NO_MEM; }
    snprintf(uri, length, "file://%s", path);
    err = audio_playback_play(audio->session->player, uri, bytes, false);
    free(uri);
    if (err != ESP_OK) {
        audio->last_status = (music_audio_status_t){.state=MUSIC_AUDIO_ERROR,.error=err};
        release_session(audio);
    }
    return err;
}

esp_err_t music_audio_stop(music_audio_handle_t audio)
{
    if (audio == NULL) return ESP_ERR_INVALID_ARG;
    release_session(audio);
    audio->last_status = (music_audio_status_t){.state=MUSIC_AUDIO_STOPPED};
    return ESP_OK;
}

esp_err_t music_audio_pause(music_audio_handle_t audio)
{
    return audio != NULL && audio->session != NULL ?
        audio_playback_pause(audio->session->player) : ESP_ERR_INVALID_STATE;
}
esp_err_t music_audio_resume(music_audio_handle_t audio)
{
    return audio != NULL && audio->session != NULL ?
        audio_playback_resume(audio->session->player) : ESP_ERR_INVALID_STATE;
}
esp_err_t music_audio_status(music_audio_handle_t audio, music_audio_status_t *out_status)
{
    if (audio == NULL || out_status == NULL) return ESP_ERR_INVALID_ARG;
    if (audio->session == NULL) { *out_status=audio->last_status; return ESP_OK; }
    audio_playback_status_t status;
    esp_err_t err = audio_playback_get_status(audio->session->player, &status);
    if (err != ESP_OK) return err;
    audio->last_status = (music_audio_status_t){
        .state=(music_audio_state_t)status.state, .position_ms=status.position_ms,
        .duration_ms=status.duration_ms, .error=status.last_error,
    };
    *out_status = audio->last_status;
    if (status.state == AUDIO_PLAYER_ERROR) release_session(audio);
    return ESP_OK;
}

#else

struct music_audio_t { int unused; };
esp_err_t music_audio_create(music_audio_handle_t *ret_audio)
{
    if (ret_audio) { *ret_audio = NULL; }
    return ESP_ERR_NOT_SUPPORTED;
}
void music_audio_delete(music_audio_handle_t audio) { (void)audio; }
esp_err_t music_audio_play(music_audio_handle_t a, const char *p, uint64_t b)
{ (void)a; (void)p; (void)b; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t music_audio_stop(music_audio_handle_t a)
{ (void)a; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t music_audio_pause(music_audio_handle_t a)
{ (void)a; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t music_audio_resume(music_audio_handle_t a)
{ (void)a; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t music_audio_status(music_audio_handle_t a, music_audio_status_t *s)
{ (void)a; (void)s; return ESP_ERR_NOT_SUPPORTED; }

#endif
