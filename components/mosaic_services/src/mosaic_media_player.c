/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* media.player provider.
 *
 * Owns the local track controller and its decoder so that the Music App
 * holds no handle to the audio pipeline. The controller's snapshot exposes
 * borrowed string pointers; this file copies them into the fixed-size
 * contract so nothing App-side depends on controller-owned storage.
 */

#include "mosaic_media_player.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "mosaic_capability.h"
#include "mosaic_capability_contracts.h"
#include "music_controller.h"

static const char *TAG = "mosaic_player_cap";

static bool s_registered;
static music_controller_handle_t s_controller;

static void copy_text(char *out, size_t capacity, const char *value)
{
    if (value == NULL) {
        out[0] = '\0';
        return;
    }
    strlcpy(out, value, capacity);
}

static esp_err_t player_ensure(void)
{
    if (s_controller != NULL) {
        return ESP_OK;
    }
    const esp_err_t err = music_controller_create(&s_controller);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create track controller failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static void player_close(void)
{
    if (s_controller == NULL) {
        return;
    }
    music_controller_delete(s_controller);
    s_controller = NULL;
}

static esp_err_t player_read(void *user_ctx, void *out, size_t size)
{
    (void)user_ctx;
    if (out == NULL || size != sizeof(mosaic_cap_player_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_controller == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    music_snapshot_t snapshot = {0};
    const esp_err_t snap_err = music_controller_snapshot(s_controller, &snapshot);
    if (snap_err != ESP_OK) {
        return snap_err;
    }
    mosaic_cap_player_t *player = out;
    memset(player, 0, sizeof(*player));
    player->revision = snapshot.revision;
    player->state = (int32_t)snapshot.state;
    player->track_index = (uint32_t)snapshot.track_index;
    player->track_count = (uint32_t)snapshot.track_count;
    copy_text(player->title, sizeof(player->title), snapshot.title);
    copy_text(player->artist, sizeof(player->artist), snapshot.artist);
    copy_text(player->album, sizeof(player->album), snapshot.album);
    player->position_ms = snapshot.position_ms;
    player->duration_ms = snapshot.duration_ms;
    player->shuffle_enabled = snapshot.shuffle_enabled;
    for (size_t row = 0; row < MOSAIC_CAP_PLAYER_LIBRARY_ROWS; ++row) {
        copy_text(player->library[row].title,
                  sizeof(player->library[row].title),
                  snapshot.library_titles[row]);
        copy_text(player->library[row].artist,
                  sizeof(player->library[row].artist),
                  snapshot.library_artists[row]);
    }
    return ESP_OK;
}

static esp_err_t player_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)out_result;
    (void)result_size;
    if (command == MOSAIC_CAP_PLAYER_CMD_CLOSE) {
        player_close();
        return ESP_OK;
    }
    const esp_err_t err = player_ensure();
    if (err != ESP_OK) {
        return err;
    }
    switch (command) {
    case MOSAIC_CAP_PLAYER_CMD_TOGGLE:
        return music_controller_toggle(s_controller);
    case MOSAIC_CAP_PLAYER_CMD_NEXT:
        return music_controller_next(s_controller);
    case MOSAIC_CAP_PLAYER_CMD_PREVIOUS:
        return music_controller_previous(s_controller);
    case MOSAIC_CAP_PLAYER_CMD_TOGGLE_SHUFFLE:
        return music_controller_toggle_shuffle(s_controller);
    case MOSAIC_CAP_PLAYER_CMD_SELECT: {
        if (args == NULL ||
                args_size != sizeof(mosaic_cap_player_select_args_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        const mosaic_cap_player_select_args_t *select = args;
        return music_controller_select(s_controller, select->track_index);
    }
    case MOSAIC_CAP_PLAYER_CMD_SEEK: {
        if (args == NULL ||
                args_size != sizeof(mosaic_cap_player_seek_args_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        const mosaic_cap_player_seek_args_t *seek = args;
        return music_controller_seek(
            s_controller, (uint16_t)seek->progress_permille);
    }
    case MOSAIC_CAP_PLAYER_CMD_START:
    case MOSAIC_CAP_PLAYER_CMD_STEP: {
        if (args == NULL ||
                args_size != sizeof(mosaic_cap_player_time_args_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        const mosaic_cap_player_time_args_t *time = args;
        return command == MOSAIC_CAP_PLAYER_CMD_START
            ? music_controller_start(s_controller, time->now_us)
            : music_controller_step(s_controller, time->now_us);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static const mosaic_capability_ops_t s_player_ops = {
    .read = player_read,
    .invoke = player_invoke,
};

esp_err_t mosaic_media_player_init(void)
{
    if (s_registered) {
        return ESP_OK;
    }
    const esp_err_t err = mosaic_capability_register(&(mosaic_capability_provider_t) {
        .name = "media.player",
        .ops = &s_player_ops,
    });
    if (err == ESP_OK) {
        s_registered = true;
    }
    return err;
}

void mosaic_media_player_deinit(void)
{
    player_close();
    if (!s_registered) {
        return;
    }
    (void)mosaic_capability_unregister("media.player", NULL);
    s_registered = false;
}
