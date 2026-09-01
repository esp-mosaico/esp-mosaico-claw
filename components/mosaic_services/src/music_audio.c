/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include "music_audio.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#if defined(ESP_PLATFORM)
#include "audio_hub.h"
#include "audio_playback.h"
#include "esp_log.h"

#define MUSIC_AUDIO_CHUNK_BYTES 512U

struct music_audio_t {
    audio_mixer_track_handle_t track;
    audio_playback_handle_t player;
    music_audio_status_t last_status;
};

static esp_err_t write_pcm(const void *data, size_t bytes, void *ctx)
{
    music_audio_handle_t audio = ctx;
    if (!audio || !audio->track) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *cursor = data;
    while (bytes > 0) {
        const size_t chunk = bytes > MUSIC_AUDIO_CHUNK_BYTES
            ? MUSIC_AUDIO_CHUNK_BYTES : bytes;
        const size_t written = audio_mixer_track_write(
            audio->track, cursor, chunk);
        if (!written) {
            return ESP_FAIL;
        }
        cursor += written;
        bytes -= written;
    }
    return ESP_OK;
}

static void release_session(music_audio_handle_t audio)
{
    if (!audio) {
        return;
    }
    if (audio->player) {
        audio_playback_delete(audio->player);
        audio->player = NULL;
    }
    if (audio->track) {
        audio_mixer_close_track(audio->track);
        audio->track = NULL;
    }
}

static esp_err_t open_session(music_audio_handle_t audio)
{
    if (!audio) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_mixer_handle_t mixer = NULL;
    esp_err_t err = audio_hub_get_mixer(&mixer);
    if (err != ESP_OK) {
        ESP_LOGE("music_audio", "Get shared mixer failed: %s", esp_err_to_name(err));
        return err;
    }
    err = audio_mixer_open_track(mixer, AUDIO_MIXER_TRACK_SYSTEM, "mosaic/music", &audio->track);
    if (err != ESP_OK) {
        ESP_LOGE("music_audio", "Open SYSTEM track failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t rate = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    err = audio_mixer_track_info(audio->track, &rate, &channels, &bits);
    if (err == ESP_OK) {
        const audio_playback_config_t config = {
            .output_format = {.sample_rate = rate, .channels = channels, .bits = bits},
            .write = write_pcm,
            .write_ctx = audio,
        };
        err = audio_playback_create(&config, &audio->player);
    }
    if (err != ESP_OK) {
        ESP_LOGE("music_audio", "Create playback session failed: %s", esp_err_to_name(err));
        release_session(audio);
    }
    return err;
}

esp_err_t music_audio_create(music_audio_handle_t *ret_audio)
{
    if (!ret_audio) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_audio = NULL;
    music_audio_handle_t audio = calloc(1, sizeof(*audio));
    if (!audio) {
        return ESP_ERR_NO_MEM;
    }
    *ret_audio = audio;
    return ESP_OK;
}

void music_audio_delete(music_audio_handle_t audio)
{
    if (!audio) {
        return;
    }
    release_session(audio);
    free(audio);
}

static char *path_to_uri(const char *path)
{
    if (!path || path[0] != '/') {
        return NULL;
    }
    const size_t length = strlen("file://") + strlen(path) + 1;
    char *uri = malloc(length);
    if (uri) {
        snprintf(uri, length, "file://%s", path);
    }
    return uri;
}

esp_err_t music_audio_play(music_audio_handle_t audio,
                           const char *path, uint64_t bytes)
{
    if (!audio || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    release_session(audio);
    esp_err_t err = open_session(audio);
    if (err != ESP_OK) {
        audio->last_status = (music_audio_status_t) {.state = MUSIC_AUDIO_ERROR, .error = err};
        return err;
    }

    char *uri = path_to_uri(path);
    if (!uri) {
        release_session(audio);
        return ESP_ERR_INVALID_ARG;
    }
    err = audio_playback_play(audio->player, uri, bytes, false);
    free(uri);
    if (err != ESP_OK) {
        audio->last_status = (music_audio_status_t) {.state = MUSIC_AUDIO_ERROR, .error = err};
        release_session(audio);
    }
    return err;
}

esp_err_t music_audio_stop(music_audio_handle_t audio)
{
    if (!audio) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = audio->player ? audio_playback_stop(audio->player) : ESP_OK;
    audio->last_status = (music_audio_status_t) {.state = MUSIC_AUDIO_STOPPED, .error = err};
    release_session(audio);
    return err;
}

esp_err_t music_audio_pause(music_audio_handle_t audio)
{
    return audio && audio->player ? audio_playback_pause(audio->player) : ESP_ERR_INVALID_STATE;
}

esp_err_t music_audio_resume(music_audio_handle_t audio)
{
    return audio && audio->player ? audio_playback_resume(audio->player) : ESP_ERR_INVALID_STATE;
}

esp_err_t music_audio_status(music_audio_handle_t audio,
                             music_audio_status_t *out_status)
{
    if (!audio || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio->player) {
        *out_status = audio->last_status;
        return ESP_OK;
    }
    audio_playback_status_t status;
    esp_err_t err = audio_playback_get_status(audio->player, &status);
    if (err != ESP_OK) {
        return err;
    }
    audio->last_status = (music_audio_status_t) {
        .state = (music_audio_state_t)status.state,
        .position_ms = status.position_ms,
        .duration_ms = status.duration_ms,
        .error = status.last_error,
    };
    *out_status = audio->last_status;
    if (status.state == AUDIO_PLAYER_FINISHED || status.state == AUDIO_PLAYER_ERROR) {
        release_session(audio);
    }
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
