/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_mixer.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bluetooth_audio_runtime *bluetooth_audio_runtime_handle_t;

typedef enum {
    BLUETOOTH_AUDIO_STATE_OFF = 0,
    BLUETOOTH_AUDIO_STATE_STARTING,
    BLUETOOTH_AUDIO_STATE_DISCOVERABLE,
    BLUETOOTH_AUDIO_STATE_CONNECTED,
    BLUETOOTH_AUDIO_STATE_PLAYING,
    BLUETOOTH_AUDIO_STATE_PAUSED,
    BLUETOOTH_AUDIO_STATE_ERROR,
} bluetooth_audio_state_t;

typedef struct {
    uint32_t revision;
    uint32_t cover_revision;
    bluetooth_audio_state_t state;
    bool connected;
    bool playing;
    bool has_cover;
    int volume_percent;
    uint32_t position_ms;
    uint32_t duration_ms;
    char device_name[32];
    char title[96];
    char artist[96];
    char error[96];
} bluetooth_audio_snapshot_t;

typedef void (*bluetooth_audio_changed_cb_t)(uint32_t revision,
                                              void *user_ctx);

typedef struct {
    audio_mixer_handle_t mixer;
    bluetooth_audio_changed_cb_t on_changed;
    void *user_ctx;
} bluetooth_audio_runtime_config_t;

esp_err_t bluetooth_audio_runtime_create(
    const bluetooth_audio_runtime_config_t *config,
    bluetooth_audio_runtime_handle_t *out_runtime);
void bluetooth_audio_runtime_delete(bluetooth_audio_runtime_handle_t runtime);

esp_err_t bluetooth_audio_runtime_start(bluetooth_audio_runtime_handle_t runtime);
esp_err_t bluetooth_audio_runtime_stop(bluetooth_audio_runtime_handle_t runtime);
esp_err_t bluetooth_audio_runtime_get_snapshot(
    bluetooth_audio_runtime_handle_t runtime,
    bluetooth_audio_snapshot_t *out_snapshot);

/**
 * Transfers the newest encoded cover image to the caller. The returned
 * malloc-compatible buffer must be freed by the caller or passed to an API
 * that explicitly takes ownership. ESP_ERR_NOT_FOUND means that the current
 * track has no pending cover payload.
 */
esp_err_t bluetooth_audio_runtime_take_cover(
    bluetooth_audio_runtime_handle_t runtime, uint8_t **out_data,
    size_t *out_size, uint32_t *out_revision);

esp_err_t bluetooth_audio_runtime_toggle_play(
    bluetooth_audio_runtime_handle_t runtime);
esp_err_t bluetooth_audio_runtime_previous(
    bluetooth_audio_runtime_handle_t runtime);
esp_err_t bluetooth_audio_runtime_next(
    bluetooth_audio_runtime_handle_t runtime);
esp_err_t bluetooth_audio_runtime_set_volume(
    bluetooth_audio_runtime_handle_t runtime, int volume_percent);
esp_err_t bluetooth_audio_runtime_adjust_volume(
    bluetooth_audio_runtime_handle_t runtime, int delta_percent);

#ifdef __cplusplus
}
#endif
