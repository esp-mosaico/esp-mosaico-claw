/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct music_controller_t *music_controller_handle_t;

typedef enum {
    MUSIC_PLAYBACK_PAUSED = 0,
    MUSIC_PLAYBACK_PLAYING,
    MUSIC_PLAYBACK_STOPPED,
    MUSIC_PLAYBACK_ERROR,
} music_playback_state_t;

typedef struct {
    const char *title;
    const char *artist;
    const char *album;
} music_track_snapshot_t;

typedef struct {
    uint32_t revision;
    music_playback_state_t state;
    size_t track_index;
    size_t track_count;
    const char *title;
    const char *artist;
    const char *album;
    int64_t position_ms;
    int64_t duration_ms;
    bool shuffle_enabled;
    const char *library_titles[3];
    const char *library_artists[3];
} music_snapshot_t;

esp_err_t music_controller_create(music_controller_handle_t *ret_controller);
void music_controller_delete(music_controller_handle_t controller);
esp_err_t music_controller_start(
    music_controller_handle_t controller, int64_t now_us);
esp_err_t music_controller_step(
    music_controller_handle_t controller, int64_t now_us);
esp_err_t music_controller_toggle(music_controller_handle_t controller);
esp_err_t music_controller_next(music_controller_handle_t controller);
esp_err_t music_controller_previous(music_controller_handle_t controller);
esp_err_t music_controller_toggle_shuffle(
    music_controller_handle_t controller);
esp_err_t music_controller_select(
    music_controller_handle_t controller, size_t track_index);
esp_err_t music_controller_seek(
    music_controller_handle_t controller, uint16_t progress_permille);
esp_err_t music_controller_snapshot(
    music_controller_handle_t controller, music_snapshot_t *out_snapshot);
esp_err_t music_controller_track(music_controller_handle_t controller,
    size_t track_index, music_track_snapshot_t *out_track);
