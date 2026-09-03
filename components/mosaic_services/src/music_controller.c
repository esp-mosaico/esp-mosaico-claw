/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include "music_controller.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "music_audio.h"

#if defined(ESP_PLATFORM)
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include "claw_paths.h"
#endif

#define MUSIC_MAX_TRACKS 16U
#define MUSIC_PATH_MAX 256U
#define MUSIC_TEXT_MAX 64U

typedef struct {
    char path[MUSIC_PATH_MAX];
    char title[MUSIC_TEXT_MAX];
    char artist[MUSIC_TEXT_MAX];
    char album[MUSIC_TEXT_MAX];
    uint64_t bytes;
    int64_t duration_ms;
} music_track_t;

struct music_controller_t {
    uint32_t revision;
    music_playback_state_t state;
    music_track_t tracks[MUSIC_MAX_TRACKS];
    size_t track_count;
    size_t track_index;
    int64_t position_ms;
    int64_t last_step_us;
    music_audio_handle_t audio;
    bool started;
    bool simulated;
    bool shuffle_enabled;
};

static size_t next_track_index(music_controller_handle_t controller)
{
    if (!controller->shuffle_enabled || controller->track_count < 2U) {
        return (controller->track_index + 1U) % controller->track_count;
    }
    const size_t offset = 1U + (size_t)rand() % (controller->track_count - 1U);
    return (controller->track_index + offset) % controller->track_count;
}

static void changed(music_controller_handle_t controller)
{
    if (++controller->revision == 0) {
        controller->revision = 1;
    }
}

static void copy_text(char *destination, size_t size, const char *source)
{
    if (!size) {
        return;
    }
    snprintf(destination, size, "%s", source ? source : "");
}

#if !defined(ESP_PLATFORM)
static void load_simulated_tracks(music_controller_handle_t controller)
{
    static const struct {
        const char *title;
        const char *artist;
        const char *album;
        int64_t duration_ms;
    } tracks[] = {
        {"Midnight Drive", "Mosaico", "Neon Memory", 213000},
        {"Blue Monday", "ESP Lab", "Pocket Signals", 184000},
        {"Quiet Circuit", "Claw Ensemble", "Soft Machines", 247000},
    };
    for (size_t index = 0; index < sizeof(tracks) / sizeof(tracks[0]); ++index) {
        music_track_t *track = &controller->tracks[index];
        copy_text(track->title, sizeof(track->title), tracks[index].title);
        copy_text(track->artist, sizeof(track->artist), tracks[index].artist);
        copy_text(track->album, sizeof(track->album), tracks[index].album);
        track->duration_ms = tracks[index].duration_ms;
    }
    controller->track_count = sizeof(tracks) / sizeof(tracks[0]);
    controller->simulated = true;
}
#endif

#if defined(ESP_PLATFORM)
static bool supported_file(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".aac") ||
        !strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".flac"));
}

static void metadata_from_name(music_track_t *track, const char *name)
{
    char stem[MUSIC_TEXT_MAX * 2U];
    copy_text(stem, sizeof(stem), name);
    char *dot = strrchr(stem, '.');
    if (dot) {
        *dot = '\0';
    }
    char *separator = strstr(stem, " - ");
    if (separator) {
        *separator = '\0';
        copy_text(track->artist, sizeof(track->artist), stem);
        copy_text(track->title, sizeof(track->title), separator + 3);
    } else {
        copy_text(track->title, sizeof(track->title), stem);
        copy_text(track->artist, sizeof(track->artist), "Local Music");
    }
    copy_text(track->album, sizeof(track->album), "On Device");
}

static int compare_tracks(const void *left, const void *right)
{
    const music_track_t *a = left;
    const music_track_t *b = right;
    return strcasecmp(a->title, b->title);
}

static esp_err_t scan_tracks(music_controller_handle_t controller)
{
    char directory[MUSIC_PATH_MAX];
    esp_err_t err = claw_paths_join(CLAW_PATH_DATA, "music", directory,
                                    sizeof(directory));
    if (err != ESP_OK) {
        return err;
    }
    DIR *dir = opendir(directory);
    if (!dir) {
        (void)mkdir(directory, 0775);
        return ESP_ERR_NOT_FOUND;
    }
    controller->track_count = 0;
    struct dirent *entry;
    while (controller->track_count < MUSIC_MAX_TRACKS &&
           (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !supported_file(entry->d_name)) {
            continue;
        }
        music_track_t *track = &controller->tracks[controller->track_count];
        memset(track, 0, sizeof(*track));
        const int written = snprintf(track->path, sizeof(track->path),
                                     "%s/%s", directory, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(track->path)) {
            continue;
        }
        struct stat info;
        if (stat(track->path, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size <= 0) {
            continue;
        }
        track->bytes = (uint64_t)info.st_size;
        metadata_from_name(track, entry->d_name);
        ++controller->track_count;
    }
    closedir(dir);
    qsort(controller->tracks, controller->track_count,
          sizeof(controller->tracks[0]), compare_tracks);
    return controller->track_count ? ESP_OK : ESP_ERR_NOT_FOUND;
}
#endif

static esp_err_t play_current(music_controller_handle_t controller)
{
    if (!controller->track_count) {
        controller->state = MUSIC_PLAYBACK_STOPPED;
        return ESP_ERR_NOT_FOUND;
    }
    controller->position_ms = 0;
    if (controller->simulated) {
        controller->state = MUSIC_PLAYBACK_PLAYING;
        changed(controller);
        return ESP_OK;
    }
    const music_track_t *track = &controller->tracks[controller->track_index];
    const esp_err_t err = music_audio_play(
        controller->audio, track->path, track->bytes);
    controller->state = err == ESP_OK
        ? MUSIC_PLAYBACK_PLAYING : MUSIC_PLAYBACK_ERROR;
    changed(controller);
    return err;
}

esp_err_t music_controller_create(music_controller_handle_t *ret_controller)
{
    if (!ret_controller) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_controller = calloc(1, sizeof(struct music_controller_t));
    return *ret_controller ? ESP_OK : ESP_ERR_NO_MEM;
}

void music_controller_delete(music_controller_handle_t controller)
{
    if (!controller) {
        return;
    }
    music_audio_delete(controller->audio);
    free(controller);
}

esp_err_t music_controller_start(
    music_controller_handle_t controller, int64_t now_us)
{
    if (!controller || controller->started) {
        return !controller ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    controller->started = true;
    controller->last_step_us = now_us;
#if defined(ESP_PLATFORM)
    const esp_err_t scan_err = scan_tracks(controller);
    if (scan_err != ESP_OK) {
        controller->state = MUSIC_PLAYBACK_STOPPED;
        changed(controller);
        return ESP_OK;
    }
    const esp_err_t audio_err = music_audio_create(&controller->audio);
    if (audio_err != ESP_OK) {
        controller->state = MUSIC_PLAYBACK_ERROR;
        changed(controller);
        return ESP_OK;
    }
#else
    load_simulated_tracks(controller);
#endif
    /* Opening the App itself succeeds even when the first resource is bad;
     * the controller exposes the playback error for the UI to render. */
    (void)play_current(controller);
    return ESP_OK;
}

esp_err_t music_controller_step(
    music_controller_handle_t controller, int64_t now_us)
{
    if (!controller || !controller->started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (controller->simulated) {
        if (now_us < controller->last_step_us) {
            controller->last_step_us = now_us;
            return ESP_OK;
        }
        const int64_t elapsed_us = now_us - controller->last_step_us;
        controller->last_step_us = now_us;
        if (controller->state == MUSIC_PLAYBACK_PLAYING) {
            controller->position_ms += elapsed_us / 1000;
            if (controller->position_ms >=
                controller->tracks[controller->track_index].duration_ms) {
                controller->track_index = next_track_index(controller);
                controller->position_ms = 0;
            }
            changed(controller);
        }
        return ESP_OK;
    }
    music_audio_status_t audio_status;
    const esp_err_t err = music_audio_status(controller->audio, &audio_status);
    if (err != ESP_OK) {
        return err;
    }
    if (audio_status.state == MUSIC_AUDIO_FINISHED) {
        controller->track_index = next_track_index(controller);
        return play_current(controller);
    }
    const music_playback_state_t state =
        audio_status.state == MUSIC_AUDIO_PLAYING ? MUSIC_PLAYBACK_PLAYING :
        audio_status.state == MUSIC_AUDIO_PAUSED ? MUSIC_PLAYBACK_PAUSED :
        audio_status.state == MUSIC_AUDIO_ERROR ? MUSIC_PLAYBACK_ERROR :
        MUSIC_PLAYBACK_STOPPED;
    music_track_t *track = &controller->tracks[controller->track_index];
    if (audio_status.duration_ms > 0) {
        track->duration_ms = audio_status.duration_ms;
    }
    if (controller->position_ms != audio_status.position_ms ||
        controller->state != state) {
        controller->position_ms = audio_status.position_ms;
        controller->state = state;
        changed(controller);
    }
    return ESP_OK;
}

esp_err_t music_controller_toggle(music_controller_handle_t controller)
{
    if (!controller || !controller->started || !controller->track_count) {
        return ESP_ERR_INVALID_STATE;
    }
    if (controller->simulated) {
        controller->state = controller->state == MUSIC_PLAYBACK_PLAYING
            ? MUSIC_PLAYBACK_PAUSED : MUSIC_PLAYBACK_PLAYING;
        changed(controller);
        return ESP_OK;
    }
    esp_err_t err;
    if (controller->state == MUSIC_PLAYBACK_PLAYING) {
        err = music_audio_pause(controller->audio);
        if (err == ESP_OK) {
            controller->state = MUSIC_PLAYBACK_PAUSED;
        }
    } else if (controller->state == MUSIC_PLAYBACK_PAUSED) {
        err = music_audio_resume(controller->audio);
        if (err == ESP_OK) {
            controller->state = MUSIC_PLAYBACK_PLAYING;
        }
    } else {
        err = play_current(controller);
    }
    if (err == ESP_OK) {
        changed(controller);
    }
    return err;
}

esp_err_t music_controller_next(music_controller_handle_t controller)
{
    if (!controller || !controller->started || !controller->track_count) {
        return ESP_ERR_INVALID_STATE;
    }
    controller->track_index = next_track_index(controller);
    return play_current(controller);
}

esp_err_t music_controller_previous(music_controller_handle_t controller)
{
    if (!controller || !controller->started || !controller->track_count) {
        return ESP_ERR_INVALID_STATE;
    }
    if (controller->position_ms <= 3000) {
        controller->track_index = controller->shuffle_enabled
            ? next_track_index(controller)
            : controller->track_index == 0
                ? controller->track_count - 1U
                : controller->track_index - 1U;
    }
    return play_current(controller);
}

esp_err_t music_controller_toggle_shuffle(
    music_controller_handle_t controller)
{
    if (!controller || !controller->started) {
        return ESP_ERR_INVALID_STATE;
    }
    controller->shuffle_enabled = !controller->shuffle_enabled;
    changed(controller);
    return ESP_OK;
}

esp_err_t music_controller_select(
    music_controller_handle_t controller, size_t track_index)
{
    if (!controller || !controller->started ||
        track_index >= controller->track_count) {
        return ESP_ERR_INVALID_ARG;
    }
    controller->track_index = track_index;
    return play_current(controller);
}

esp_err_t music_controller_seek(
    music_controller_handle_t controller, uint16_t progress_permille)
{
    (void)controller;
    (void)progress_permille;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t music_controller_snapshot(
    music_controller_handle_t controller, music_snapshot_t *out_snapshot)
{
    if (!controller || !out_snapshot || !controller->started) {
        return !controller || !out_snapshot
            ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    static const music_track_t empty = {
        .title = "No Local Music",
        .artist = "Copy audio files to music/",
        .album = "On Device",
    };
    const music_track_t *track = controller->track_count
        ? &controller->tracks[controller->track_index] : &empty;
    *out_snapshot = (music_snapshot_t) {
        .revision = controller->revision,
        .state = controller->state,
        .track_index = controller->track_index,
        .track_count = controller->track_count,
        .title = track->title,
        .artist = track->artist,
        .album = track->album,
        .position_ms = controller->position_ms,
        .duration_ms = track->duration_ms,
        .shuffle_enabled = controller->shuffle_enabled,
    };
    for (size_t index = 0; index < 3U; ++index) {
        if (index < controller->track_count) {
            out_snapshot->library_titles[index] =
                controller->tracks[index].title;
            out_snapshot->library_artists[index] =
                controller->tracks[index].artist;
        } else {
            out_snapshot->library_titles[index] = "";
            out_snapshot->library_artists[index] = "";
        }
    }
    return ESP_OK;
}

esp_err_t music_controller_track(music_controller_handle_t controller,
    size_t track_index, music_track_snapshot_t *out_track)
{
    if (!controller || !out_track || track_index >= controller->track_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const music_track_t *track = &controller->tracks[track_index];
    *out_track = (music_track_snapshot_t) {
        .title = track->title,
        .artist = track->artist,
        .album = track->album,
    };
    return ESP_OK;
}
