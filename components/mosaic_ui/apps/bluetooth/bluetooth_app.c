/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bluetooth_actions.h"
#include "bluetooth_binds.h"
#include "bluetooth_objects.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"
#include "mosaic_capability.h"
#include "mosaic_capability_contracts.h"
#include "mosaic_top_notice.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ID 10 is owned by the ported Setup Center app. Keep this stable because
 * asynchronous Bluetooth runtime updates target the app by this ID. */
#define BLUETOOTH_APP_ID 12U
#define BLUETOOTH_IMAGE_CACHE_BYTES (512U * 1024U)
static const mosaic_app_route_t s_bluetooth_routes[] = {
    { .action_id = GSP_ACT_ID_BT_LOCAL, .target_name = "music" },
};

#if defined(ESP_PLATFORM)
static const mosaic_top_notice_config_t s_exit_notice = {
    .visible_bind = GSP_BIND_BT_TOP_NOTICE_VISIBLE,
    .title_bind = GSP_BIND_BT_TOP_NOTICE_TITLE,
    .message_bind = GSP_BIND_BT_TOP_NOTICE_MESSAGE,
};
#endif

#define BT_PROGRESS_CURSOR_X_MIN 8
#define BT_PROGRESS_CURSOR_X_MAX 464
#define BT_PROGRESS_CURSOR_Y 372
#define BT_VOLUME_X_MIN 425
#define BT_VOLUME_X_MAX 479
#define BT_VOLUME_Y_MIN 88
#define BT_VOLUME_Y_MAX 292
static bool s_volume_drag_active;

/* The App drives the A2DP sink and the shared output level, and reaches
 * nothing else on the device. */
#define BLUETOOTH_CAPABILITIES ( \
    MOSAIC_CAP_SYSTEM_AUDIO_CONTROL | \
    MOSAIC_CAP_MEDIA_BLUETOOTH_READ | MOSAIC_CAP_MEDIA_BLUETOOTH_CONTROL)

static void bluetooth_set_progress(esp_gsp_handle_t ui, int32_t progress)
{
    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }
    (void)esp_gsp_set_value(ui, GSP_BIND_BT_PROGRESS, progress);
    const int32_t x = BT_PROGRESS_CURSOR_X_MIN +
        ((BT_PROGRESS_CURSOR_X_MAX - BT_PROGRESS_CURSOR_X_MIN) * progress +
         50) / 100;
    (void)esp_gsp_component_set_position(
        ui, GSP_OBJ_KEY_BT_PROGRESS_CURSOR, x, BT_PROGRESS_CURSOR_Y);
}

static int bluetooth_volume_from_y(int32_t y)
{
    if (y < BT_VOLUME_Y_MIN) {
        y = BT_VOLUME_Y_MIN;
    } else if (y > BT_VOLUME_Y_MAX) {
        y = BT_VOLUME_Y_MAX;
    }
    return ((BT_VOLUME_Y_MAX - y) * 100 +
            (BT_VOLUME_Y_MAX - BT_VOLUME_Y_MIN) / 2) /
           (BT_VOLUME_Y_MAX - BT_VOLUME_Y_MIN);
}

#if defined(ESP_PLATFORM)

#include "esp_attr.h"
#include "esp_log.h"
#include "mosaic_loader.h"
#include "mosaic_media_bluetooth.h"

static const char *TAG = "bluetooth_app";
static bool s_media_started;
static mosaic_capability_subscription_handle_t s_snapshot_subscription;
static int s_volume_drag_value;
static EXT_RAM_BSS_ATTR mosaic_cap_bluetooth_t s_snapshot;
static uint32_t s_cover_revision = UINT32_MAX;
static atomic_bool s_cover_published;

static esp_err_t bluetooth_invoke(const char *command)
{
    return mosaic_capability_invoke("media.bluetooth",
        BLUETOOTH_CAPABILITIES, command, NULL, 0, NULL, 0);
}

/* The blob stays borrowed until GSP reports it has stopped referencing the
 * encoded payload. */
static void cover_blob_released(void *release_ctx)
{
    mosaic_capability_blob_t *blob = release_ctx;
    if (blob == NULL) {
        return;
    }
    mosaic_capability_release("media.bluetooth", blob);
    free(blob);
}

static void cover_image_complete(esp_gsp_handle_t ui, uint16_t bind,
                                 gsp_err_t status, void *user_ctx)
{
    (void)bind;
    (void)user_ctx;
    if (status != GSP_OK) {
        if (status != GSP_ERR_CANCELLED) {
            ESP_LOGW(TAG, "cover art decode failed: %d", status);
        }
        return;
    }
    atomic_store(&s_cover_published, true);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_COVER_VISIBLE, true);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_BT_COVER_PLACEHOLDER_VISIBLE, false);
    ESP_LOGI(TAG, "cover art decode complete");
}

static const char *state_text(int32_t state)
{
    switch (state) {
    case MOSAIC_CAP_BT_STATE_STARTING:
        return "Starting";
    case MOSAIC_CAP_BT_STATE_DISCOVERABLE:
        return "Discoverable · Waiting for phone";
    case MOSAIC_CAP_BT_STATE_CONNECTED:
        return "Connected";
    case MOSAIC_CAP_BT_STATE_PLAYING:
        return "Playing";
    case MOSAIC_CAP_BT_STATE_PAUSED:
        return "Paused";
    case MOSAIC_CAP_BT_STATE_ERROR:
        return "Error";
    case MOSAIC_CAP_BT_STATE_OFF:
    default:
        return "Bluetooth unavailable";
    }
}

static void format_time(char *buffer, size_t capacity, uint32_t time_ms)
{
    uint32_t seconds = time_ms / 1000U;
    snprintf(buffer, capacity, "%02" PRIu32 ":%02" PRIu32,
             seconds / 60U, seconds % 60U);
}

static void render_cover(esp_gsp_handle_t ui)
{
    if (s_snapshot.cover_revision == s_cover_revision) {
        return;
    }
    if (!s_snapshot.has_cover || !s_media_started) {
        s_cover_revision = s_snapshot.cover_revision;
        /* Once a cover has been published, keep it visible while AVRCP
         * fetches and decodes the next one. GSP swaps the decoded resource
         * atomically, so there is no reason to flash the fixed placeholder. */
        const bool published = atomic_load(&s_cover_published);
        (void)esp_gsp_set_visible(ui, GSP_BIND_BT_COVER_VISIBLE, published);
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_BT_COVER_PLACEHOLDER_VISIBLE, !published);
        return;
    }

    bool visible = false;
    {
        mosaic_capability_blob_t *blob = calloc(1, sizeof(*blob));
        if (blob == NULL) {
            return;
        }
        if (mosaic_capability_borrow("media.bluetooth",
                BLUETOOTH_CAPABILITIES, MOSAIC_CAP_BT_BLOB_COVER, 0,
                blob) == ESP_OK) {
            const uint32_t revision = s_snapshot.cover_revision;
            const esp_gsp_image_options_t options = {
                .ownership = ESP_GSP_IMAGE_BORROW,
                .on_release = cover_blob_released,
                .release_ctx = blob,
                .on_complete = cover_image_complete,
            };
            esp_gsp_err_t err = esp_gsp_set_image_ex(
                ui, GSP_BIND_BT_COVER, blob->data, blob->size, &options);
            if (err == ESP_GSP_OK) {
                s_cover_revision = revision;
                visible = atomic_load(&s_cover_published);
                ESP_LOGI(TAG, "cover art submitted: revision=%" PRIu32
                         ", size=%u", revision, (unsigned)blob->size);
            } else {
                cover_blob_released(blob);
                ESP_LOGW(TAG, "publish cover art failed: 0x%x", err);
            }
        } else {
            /* Keep the old revision so the periodic render retries if the
             * snapshot became visible just before the payload handoff. */
            free(blob);
            return;
        }
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_COVER_VISIBLE, visible);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_COVER_PLACEHOLDER_VISIBLE,
                              !visible);
}

static void bluetooth_render(esp_gsp_handle_t ui)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    if (!s_media_started ||
            mosaic_capability_read("media.bluetooth", BLUETOOTH_CAPABILITIES,
                &s_snapshot, sizeof(s_snapshot)) != ESP_OK) {
        (void)esp_gsp_set_text(ui, GSP_BIND_BT_STATUS,
                               "Bluetooth unavailable");
        (void)esp_gsp_set_text(ui, GSP_BIND_BT_DEVICE_NAME,
                               "Audio mixer unavailable");
        (void)esp_gsp_set_text(ui, GSP_BIND_BT_TITLE,
                               "Waiting for audio service");
        (void)esp_gsp_set_text(ui, GSP_BIND_BT_ARTIST, "");
        (void)esp_gsp_set_text(ui, GSP_BIND_BT_ELAPSED, "00:00");
        (void)esp_gsp_set_text(ui, GSP_BIND_BT_DURATION, "00:00");
        (void)esp_gsp_set_text(ui, GSP_BIND_BT_VOLUME, "--%");
        (void)esp_gsp_set_value(ui, GSP_BIND_BT_VOLUME_LEVEL, 0);
        bluetooth_set_progress(ui, 0);
        (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PLAY_VISIBLE, true);
        (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PAUSE_VISIBLE, false);
        (void)esp_gsp_set_visible(ui, GSP_BIND_BT_COVER_VISIBLE, false);
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_BT_COVER_PLACEHOLDER_VISIBLE, true);
        (void)esp_gsp_set_visible(ui, GSP_BIND_BT_WAITING_VISIBLE, true);
        (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PLAYER_VISIBLE, false);
        return;
    }

    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_WAITING_VISIBLE,
                              !s_snapshot.connected);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PLAYER_VISIBLE,
                              s_snapshot.connected);

    (void)esp_gsp_set_text(ui, GSP_BIND_BT_STATUS,
                           s_snapshot.error[0] ? s_snapshot.error :
                                                state_text(s_snapshot.state));
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_DEVICE_NAME,
                           s_snapshot.device_name);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_TITLE,
                           s_snapshot.title[0] ? s_snapshot.title :
                           (s_snapshot.connected ? "Waiting for playback" :
                                                   "Waiting for phone"));
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_ARTIST, s_snapshot.artist);

    char text[32];
    format_time(text, sizeof(text), s_snapshot.position_ms);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_ELAPSED, text);
    format_time(text, sizeof(text), s_snapshot.duration_ms);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_DURATION, text);
    uint32_t progress = s_snapshot.duration_ms == 0 ? 0 :
        (uint32_t)(((uint64_t)s_snapshot.position_ms * 100U) /
                   s_snapshot.duration_ms);
    if (progress > 100U) {
        progress = 100U;
    }
    bluetooth_set_progress(ui, (int32_t)progress);
    snprintf(text, sizeof(text), "%d%%", s_snapshot.volume_percent);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_VOLUME, text);
    (void)esp_gsp_set_value(ui, GSP_BIND_BT_VOLUME_LEVEL,
                            s_snapshot.volume_percent);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PLAY_VISIBLE,
                              !s_snapshot.playing);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PAUSE_VISIBLE,
                              s_snapshot.playing);
    render_cover(ui);
}

static void bluetooth_changed(void *user_ctx, const char *name,
    const void *payload, size_t payload_size)
{
    (void)user_ctx;
    (void)name;
    if (payload == NULL || payload_size != sizeof(mosaic_cap_bluetooth_t)) {
        return;
    }
    const mosaic_cap_bluetooth_t *snapshot = payload;
    (void)mosaic_loader_invalidate_app(BLUETOOTH_APP_ID, snapshot->revision);
}

static void bluetooth_try_start(void)
{
    if (s_media_started) {
        return;
    }
    esp_err_t err = mosaic_media_bluetooth_start();
    if (err == ESP_ERR_INVALID_STATE) {
        /* The mixer is not up yet; the next tick retries. */
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start Bluetooth App failed: %s",
                 esp_err_to_name(err));
        return;
    }
    s_media_started = true;
    if (s_snapshot_subscription == NULL) {
        (void)mosaic_capability_subscribe("media.bluetooth",
            BLUETOOTH_CAPABILITIES, bluetooth_changed, NULL,
            &s_snapshot_subscription);
    }
}

static void dispatch_call(const mosaic_event_t *event)
{
    if (!s_media_started) {
        return;
    }
    esp_err_t err = ESP_OK;
    switch (event->data.call.action_id) {
    case GSP_ACT_ID_BT_LOCAL:
        return;
    case GSP_ACT_ID_BT_TOGGLE_PLAY:
        err = bluetooth_invoke("toggle_play");
        break;
    case GSP_ACT_ID_BT_PREVIOUS:
        err = bluetooth_invoke("previous");
        break;
    case GSP_ACT_ID_BT_NEXT:
        err = bluetooth_invoke("next");
        break;
    default:
        return;
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Bluetooth action failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t bluetooth_set_volume(int32_t volume, bool persist)
{
    const mosaic_cap_audio_volume_args_t args = {
        .volume = volume,
        .persist = persist,
    };
    return mosaic_capability_invoke("system.audio",
        BLUETOOTH_CAPABILITIES, "set_volume", &args, sizeof(args), NULL, 0);
}

static void bluetooth_dispatch_volume_pointer(esp_gsp_handle_t ui,
                                              const mosaic_event_t *event)
{
    if (!event->data.pointer.pressed) {
        if (s_volume_drag_active) {
            (void)bluetooth_set_volume(s_volume_drag_value, true);
        }
        s_volume_drag_active = false;
        return;
    }
    if (!s_volume_drag_active) {
        if (event->data.pointer.x < BT_VOLUME_X_MIN ||
                event->data.pointer.x > BT_VOLUME_X_MAX ||
                event->data.pointer.y < BT_VOLUME_Y_MIN ||
                event->data.pointer.y > BT_VOLUME_Y_MAX) {
            return;
        }
        s_volume_drag_active = true;
    }
    const int volume = bluetooth_volume_from_y(event->data.pointer.y);
    s_volume_drag_value = volume;
    (void)esp_gsp_set_value(ui, GSP_BIND_BT_VOLUME_LEVEL, volume);
    char text[8];
    snprintf(text, sizeof(text), "%d%%", volume);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_VOLUME, text);
    (void)bluetooth_set_volume(volume, false);
    if (s_media_started) {
        const mosaic_cap_bluetooth_volume_args_t args = {
            .volume_percent = volume,
        };
        (void)mosaic_capability_invoke("media.bluetooth",
            BLUETOOTH_CAPABILITIES, "set_volume", &args, sizeof(args),
            NULL, 0);
    }
}

static void bluetooth_event(esp_gsp_handle_t ui,
                            const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (!event) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
        s_cover_revision = UINT32_MAX;
        atomic_store(&s_cover_published, false);
        if (!audio_hub_is_started()) {
            ESP_LOGW(TAG, "shared audio mixer is not ready");
        }
        bluetooth_try_start();
        bluetooth_render(ui);
        break;
    case MOSAIC_EVENT_UI_CALL:
        dispatch_call(event);
        break;
    case MOSAIC_EVENT_POINTER:
        bluetooth_dispatch_volume_pointer(ui, event);
        break;
    case MOSAIC_EVENT_TIMER:
        bluetooth_try_start();
        bluetooth_render(ui);
        break;
    case MOSAIC_EVENT_MODEL_CHANGED:
    case MOSAIC_EVENT_SCENE_CHANGED:
        bluetooth_render(ui);
        break;
    case MOSAIC_EVENT_STOP:
        /* This is the ownership boundary: leaving the App powers BT down. */
        if (s_snapshot.connected) {
            (void)mosaic_top_notice_show(
                ui, &s_exit_notice, "Disconnecting Bluetooth",
                "Returning to Home...", 0);
        }
        if (s_snapshot_subscription != NULL &&
                mosaic_capability_unsubscribe(
                    s_snapshot_subscription) == ESP_OK) {
            s_snapshot_subscription = NULL;
        }
        mosaic_media_bluetooth_stop();
        mosaic_top_notice_detach(ui);
        s_media_started = false;
        s_cover_revision = UINT32_MAX;
        atomic_store(&s_cover_published, false);
        break;
    default:
        break;
    }
}

#else

static bool s_host_connected;
static bool s_host_playing;
static int s_host_volume = 80;
static uint32_t s_host_position_ms;
static int64_t s_host_last_tick_us;

#define BLUETOOTH_HOST_DURATION_MS UINT32_C(213000)

static void bluetooth_host_format_time(char *buffer, size_t capacity,
                                       uint32_t time_ms)
{
    const uint32_t seconds = time_ms / 1000U;
    snprintf(buffer, capacity, "%02" PRIu32 ":%02" PRIu32,
             seconds / 60U, seconds % 60U);
}

static void bluetooth_host_render(esp_gsp_handle_t ui)
{
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_WAITING_VISIBLE,
                              !s_host_connected);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PLAYER_VISIBLE,
                              s_host_connected);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_STATUS,
                           s_host_connected ? "Connected" : "Discoverable");
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_DEVICE_NAME,
                           s_host_connected ? "Mosaico Phone" :
                                              "ESP-Claw-Audio");
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_TITLE,
                           s_host_connected ? "Midnight Drive" :
                                              "Waiting for phone");
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_ARTIST,
                           s_host_connected ? "Mosaico" : "");
    char elapsed[16];
    char duration[16];
    bluetooth_host_format_time(elapsed, sizeof(elapsed),
                               s_host_connected ? s_host_position_ms : 0U);
    bluetooth_host_format_time(duration, sizeof(duration),
                               s_host_connected ?
                                   BLUETOOTH_HOST_DURATION_MS : 0U);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_ELAPSED, elapsed);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_DURATION, duration);
    char volume[8];
    snprintf(volume, sizeof(volume), "%d%%", s_host_volume);
    (void)esp_gsp_set_text(ui, GSP_BIND_BT_VOLUME, volume);
    (void)esp_gsp_set_value(ui, GSP_BIND_BT_VOLUME_LEVEL, s_host_volume);
    const int32_t progress = s_host_connected ?
        (int32_t)(((uint64_t)s_host_position_ms * 100U) /
                  BLUETOOTH_HOST_DURATION_MS) : 0;
    bluetooth_set_progress(ui, progress);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PLAY_VISIBLE,
                              !s_host_playing);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_PAUSE_VISIBLE,
                              s_host_playing);
    (void)esp_gsp_set_visible(ui, GSP_BIND_BT_COVER_VISIBLE, false);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_BT_COVER_PLACEHOLDER_VISIBLE, true);
}

static void bluetooth_event(esp_gsp_handle_t ui,
                            const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL) {
        return;
    }
    if (event->type == MOSAIC_EVENT_START) {
        s_host_connected = false;
        s_host_playing = false;
        s_host_position_ms = 0U;
        s_host_volume = 80;
        s_host_last_tick_us = event->timestamp_us;
        bluetooth_host_render(ui);
    } else if (event->type == MOSAIC_EVENT_POINTER) {
        if (!event->data.pointer.pressed) {
            s_volume_drag_active = false;
        } else if (s_volume_drag_active ||
                   (event->data.pointer.x >= BT_VOLUME_X_MIN &&
                    event->data.pointer.x <= BT_VOLUME_X_MAX &&
                    event->data.pointer.y >= BT_VOLUME_Y_MIN &&
                    event->data.pointer.y <= BT_VOLUME_Y_MAX)) {
            s_volume_drag_active = true;
            s_host_volume = bluetooth_volume_from_y(event->data.pointer.y);
            (void)esp_gsp_set_value(
                ui, GSP_BIND_BT_VOLUME_LEVEL, s_host_volume);
        }
    } else if (event->type == MOSAIC_EVENT_UI_CALL) {
        switch (event->data.call.action_id) {
        case GSP_ACT_ID_BT_SIM_CONNECT:
            s_host_connected = true;
            s_host_playing = true;
            s_host_position_ms = 39000U;
            s_host_last_tick_us = event->timestamp_us;
            bluetooth_host_render(ui);
            break;
        case GSP_ACT_ID_BT_TOGGLE_PLAY:
            if (s_host_connected) {
                s_host_playing = !s_host_playing;
                bluetooth_host_render(ui);
            }
            break;
        case GSP_ACT_ID_BT_PREVIOUS:
        case GSP_ACT_ID_BT_NEXT:
            if (s_host_connected) {
                s_host_position_ms = 0U;
                s_host_last_tick_us = event->timestamp_us;
                bluetooth_host_render(ui);
            }
            break;
        default:
            break;
        }
    } else if (event->type == MOSAIC_EVENT_TIMER && s_host_connected) {
        if (s_host_playing && event->timestamp_us > s_host_last_tick_us) {
            const uint64_t delta_ms =
                (uint64_t)(event->timestamp_us - s_host_last_tick_us) / 1000U;
            const uint64_t next = (uint64_t)s_host_position_ms + delta_ms;
            s_host_position_ms = next < BLUETOOTH_HOST_DURATION_MS ?
                (uint32_t)next : BLUETOOTH_HOST_DURATION_MS;
            if (s_host_position_ms == BLUETOOTH_HOST_DURATION_MS) {
                s_host_playing = false;
            }
        }
        s_host_last_tick_us = event->timestamp_us;
        bluetooth_host_render(ui);
    }
}

#endif

const mosaic_app_descriptor_t mosaic_bluetooth_app = {
    .id = BLUETOOTH_APP_ID,
    .launch_action = MOSAIC_APP_NO_LAUNCH_ACTION,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .routes = s_bluetooth_routes,
    .route_count = sizeof(s_bluetooth_routes) /
                   sizeof(s_bluetooth_routes[0]),
    .routes_without_history = true,
    .back_exits_app = true,
    .name = "bluetooth",
    .title = "Bluetooth Audio",
    .directory = &gsp_obj_directory_bluetooth,
    .disable_swipe = true,
    /* The player owns the full canvas. Shared Shell keeps only the bottom
     * home indicator and upward-exit gesture, without a top header. */
    .root_header_in_stack = true,
    /* The generated bundle requirements own the single cover-art target.
     * Leave this at AUTO so an application override cannot lower the GSPB
     * requirement. */
    .dynamic_image_slots = 0,
    .image_cache_bytes = BLUETOOTH_IMAGE_CACHE_BYTES,
    .on_event = bluetooth_event,
};
