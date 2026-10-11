/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* Minimal WAV player for Air Voice recordings.
 *
 * Recordings are 16 kHz mono 16-bit PCM, the shared mixer's native format,
 * so samples are streamed straight into a mixer track; the mixer's bounded
 * blocking write paces playback in real time. This avoids esp_player, whose
 * product configuration ships without a PCM decoder (WAV playback fails with
 * "Decoder PCM not registered"). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_mixer.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct av_player_t av_player_t;

/** Validate @p path and start streaming it into @p track (not owned).
 * Returns ESP_ERR_NOT_SUPPORTED when the WAV format differs from the track. */
esp_err_t av_player_start(const char *path, audio_mixer_track_handle_t track,
                          av_player_t **out_player);
void av_player_set_paused(av_player_t *player, bool paused);
bool av_player_is_paused(const av_player_t *player);
/** True once streaming ended; @p out_error reports ESP_OK on a clean end. */
bool av_player_finished(const av_player_t *player, esp_err_t *out_error);
int64_t av_player_position_ms(const av_player_t *player);
int64_t av_player_duration_ms(const av_player_t *player);
/** Stop streaming, wait for the task to exit and free the player. The mixer
 * track can be closed safely afterwards. */
void av_player_destroy(av_player_t *player);

#ifdef __cplusplus
}
#endif
