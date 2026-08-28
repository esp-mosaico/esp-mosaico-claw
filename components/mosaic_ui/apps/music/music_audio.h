/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct music_audio_t *music_audio_handle_t;

typedef enum {
    MUSIC_AUDIO_IDLE = 0,
    MUSIC_AUDIO_PLAYING,
    MUSIC_AUDIO_PAUSED,
    MUSIC_AUDIO_STOPPED,
    MUSIC_AUDIO_FINISHED,
    MUSIC_AUDIO_ERROR,
} music_audio_state_t;

typedef struct {
    music_audio_state_t state;
    int64_t position_ms;
    int64_t duration_ms;
    esp_err_t error;
} music_audio_status_t;

esp_err_t music_audio_create(music_audio_handle_t *ret_audio);
void music_audio_delete(music_audio_handle_t audio);
esp_err_t music_audio_play(music_audio_handle_t audio,
                           const char *path, uint64_t bytes);
esp_err_t music_audio_stop(music_audio_handle_t audio);
esp_err_t music_audio_pause(music_audio_handle_t audio);
esp_err_t music_audio_resume(music_audio_handle_t audio);
esp_err_t music_audio_status(music_audio_handle_t audio,
                             music_audio_status_t *out_status);
