/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include "music_presenter.h"

#include <stdio.h>
#include <stdlib.h>

#include "music_binds.h"
#include "music_objects.h"

#define MUSIC_PROGRESS_CURSOR_X_MIN 8
#define MUSIC_PROGRESS_CURSOR_X_MAX 464
#define MUSIC_PROGRESS_CURSOR_Y 372

struct music_presenter_t {
    uint32_t rendered_revision;
    uint32_t rendered_track_index;
    uint32_t rendered_elapsed_seconds;
    uint32_t rendered_remaining_seconds;
    int32_t rendered_state;
    bool rendered_shuffle_enabled;
    bool valid;
};

static void format_time(int64_t milliseconds, char text[16])
{
    if (milliseconds < 0) {
        milliseconds = 0;
    }
    const unsigned seconds = (unsigned)(milliseconds / 1000);
    snprintf(text, 16, "%u:%02u", seconds / 60U, seconds % 60U);
}

static esp_err_t set_visible(
    esp_gsp_handle_t ui, uint16_t bind, bool visible, esp_err_t first_error)
{
    const esp_err_t err = esp_gsp_set_visible(ui, bind, visible);
    return first_error == ESP_OK ? err : first_error;
}

esp_err_t music_presenter_create(music_presenter_handle_t *ret_presenter)
{
    if (ret_presenter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_presenter = calloc(1, sizeof(struct music_presenter_t));
    return *ret_presenter != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void music_presenter_delete(music_presenter_handle_t presenter)
{
    free(presenter);
}

void music_presenter_invalidate(music_presenter_handle_t presenter)
{
    if (presenter != NULL) {
        presenter->valid = false;
    }
}

esp_err_t music_presenter_render(music_presenter_handle_t presenter,
    esp_gsp_handle_t ui, const mosaic_cap_player_t *snapshot, bool force)
{
    if (presenter == NULL || ui == NULL || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!force && presenter->valid &&
            presenter->rendered_revision == snapshot->revision) {
        return ESP_OK;
    }

    char elapsed[16];
    char duration[16];
    char count[16];
    format_time(snapshot->position_ms, elapsed);
    format_time(snapshot->duration_ms, duration);
    snprintf(count, sizeof(count), "%u of %u",
        snapshot->track_count ? (unsigned)snapshot->track_index + 1U : 0U,
        (unsigned)snapshot->track_count);
    int32_t progress = snapshot->duration_ms > 0
        ? (int32_t)((snapshot->position_ms * 1000) /
                    snapshot->duration_ms)
        : 0;
    if (progress < 0) {
        progress = 0;
    } else if (progress > 1000) {
        progress = 1000;
    }
    const int32_t progress_percent = (progress + 5) / 10;
    const int32_t cursor_x = MUSIC_PROGRESS_CURSOR_X_MIN +
        ((MUSIC_PROGRESS_CURSOR_X_MAX - MUSIC_PROGRESS_CURSOR_X_MIN) *
         progress_percent + 50) / 100;
    (void)esp_gsp_component_set_position(
        ui, GSP_OBJ_KEY_MUSIC_PROGRESS_CURSOR,
        cursor_x, MUSIC_PROGRESS_CURSOR_Y);
    esp_err_t err = esp_gsp_set_value(
        ui, GSP_BIND_MUSIC_PROGRESS, progress_percent);
    const uint32_t elapsed_seconds =
        (uint32_t)(snapshot->position_ms / 1000);
    const uint32_t remaining_seconds = (uint32_t)(
        (snapshot->duration_ms - snapshot->position_ms) / 1000);
    const bool metadata_changed = force || !presenter->valid ||
        presenter->rendered_track_index != snapshot->track_index;
    const bool time_changed = metadata_changed ||
        presenter->rendered_elapsed_seconds != elapsed_seconds ||
        presenter->rendered_remaining_seconds != remaining_seconds;
    const bool state_changed = force || !presenter->valid ||
        presenter->rendered_state != snapshot->state;
    const bool shuffle_changed = force || !presenter->valid ||
        presenter->rendered_shuffle_enabled != snapshot->shuffle_enabled;

    if (metadata_changed) {
        err = esp_gsp_set_text(ui, GSP_BIND_MUSIC_TITLE,
            snapshot->title);
    }
    if (metadata_changed && err == ESP_OK) {
        err = esp_gsp_set_text(ui, GSP_BIND_MUSIC_ARTIST,
            snapshot->artist);
    }
    if (metadata_changed && err == ESP_OK) {
        err = esp_gsp_set_text(ui, GSP_BIND_MUSIC_ALBUM,
            snapshot->album);
    }
    if (time_changed && err == ESP_OK) {
        err = esp_gsp_set_text(ui, GSP_BIND_MUSIC_ELAPSED, elapsed);
    }
    if (time_changed && err == ESP_OK) {
        err = esp_gsp_set_text(ui, GSP_BIND_MUSIC_REMAINING, duration);
    }
    if (metadata_changed && err == ESP_OK) {
        err = esp_gsp_set_text(ui, GSP_BIND_MUSIC_TRACK_COUNT, count);
    }
    if (state_changed && err == ESP_OK) {
        err = esp_gsp_set_text(ui, GSP_BIND_MUSIC_TOGGLE_TEXT,
            snapshot->state == MOSAIC_CAP_PLAYER_PLAYING ? "PAUSE" : "PLAY");
    }
    if (state_changed) {
        const bool playing = snapshot->state == MOSAIC_CAP_PLAYER_PLAYING;
        err = set_visible(ui, GSP_BIND_MUSIC_PLAY_VISIBLE, !playing, err);
        err = set_visible(ui, GSP_BIND_MUSIC_PAUSE_VISIBLE, playing, err);
    }
    if (shuffle_changed) {
        err = set_visible(ui, GSP_BIND_MUSIC_REPEAT_ORDER_VISIBLE,
                          !snapshot->shuffle_enabled, err);
        err = set_visible(ui, GSP_BIND_MUSIC_REPEAT_SHUFFLE_VISIBLE,
                          snapshot->shuffle_enabled, err);
    }
    static const uint16_t playing_binds[] = {
        GSP_BIND_MUSIC_LIBRARY_PLAYING_0_VISIBLE,
        GSP_BIND_MUSIC_LIBRARY_PLAYING_1_VISIBLE,
        GSP_BIND_MUSIC_LIBRARY_PLAYING_2_VISIBLE,
    };
    static const uint16_t row_binds[] = {
        GSP_BIND_MUSIC_LIBRARY_ROW_0_VISIBLE,
        GSP_BIND_MUSIC_LIBRARY_ROW_1_VISIBLE,
        GSP_BIND_MUSIC_LIBRARY_ROW_2_VISIBLE,
    };
    static const uint16_t title_binds[] = {
        GSP_BIND_MUSIC_LIBRARY_TITLE_0,
        GSP_BIND_MUSIC_LIBRARY_TITLE_1,
        GSP_BIND_MUSIC_LIBRARY_TITLE_2,
    };
    static const uint16_t artist_binds[] = {
        GSP_BIND_MUSIC_LIBRARY_ARTIST_0,
        GSP_BIND_MUSIC_LIBRARY_ARTIST_1,
        GSP_BIND_MUSIC_LIBRARY_ARTIST_2,
    };
    if (metadata_changed) {
        for (size_t index = 0; index < 3U; ++index) {
            const bool populated = index < snapshot->track_count;
            err = set_visible(ui, row_binds[index], populated, err);
            if (populated && err == ESP_OK) {
                err = esp_gsp_set_text(ui, title_binds[index],
                                       snapshot->library[index].title);
            }
            if (populated && err == ESP_OK) {
                err = esp_gsp_set_text(ui, artist_binds[index],
                                       snapshot->library[index].artist);
            }
            err = set_visible(ui, playing_binds[index],
                              populated && index == snapshot->track_index,
                              err);
        }
        char summary[48];
        snprintf(summary, sizeof(summary), "%u song%s  |  local music",
                 (unsigned)snapshot->track_count,
                 snapshot->track_count == 1U ? "" : "s");
        if (err == ESP_OK) {
            err = esp_gsp_set_text(ui, GSP_BIND_MUSIC_LIBRARY_SUMMARY,
                                   summary);
        }
    }
    if (err == ESP_OK) {
        presenter->rendered_revision = snapshot->revision;
        presenter->rendered_track_index = snapshot->track_index;
        presenter->rendered_elapsed_seconds = elapsed_seconds;
        presenter->rendered_remaining_seconds = remaining_seconds;
        presenter->rendered_state = snapshot->state;
        presenter->rendered_shuffle_enabled = snapshot->shuffle_enabled;
        presenter->valid = true;
    } else {
        presenter->valid = false;
    }
    return err;
}
