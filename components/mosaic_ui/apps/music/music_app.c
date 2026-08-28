/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"
#include "music_actions.h"
#include "music_binds.h"
#include "music_objects.h"
#include "mosaic_settings.h"

#include "music_controller.h"
#include "music_presenter.h"

#include <stdio.h>

#define MUSIC_APP_ID          5U
#define MUSIC_LIBRARY_PAGE    0U
#define MUSIC_PLAYER_PAGE     1U
#define MUSIC_VOLUME_X_MIN    425
#define MUSIC_VOLUME_X_MAX    479
#define MUSIC_VOLUME_Y_MIN    88
#define MUSIC_VOLUME_Y_MAX    292
static music_controller_handle_t s_controller;
static music_presenter_handle_t s_presenter;
static uint16_t s_page = MUSIC_PLAYER_PAGE;
static bool s_volume_drag_active;
static int s_volume = 65;

static const mosaic_app_route_t s_music_routes[] = {
    { .action_id = GSP_ACT_ID_MUSIC_BLUETOOTH, .target_name = "bluetooth" },
};

static void show_page(esp_gsp_handle_t ui, uint16_t page, bool animated)
{
    if (page == s_page) {
        return;
    }
    const esp_err_t err = page == MUSIC_LIBRARY_PAGE
        ? esp_gsp_stack_view_push(
            ui, GSP_OBJ_KEY_MUSIC_STACK, MUSIC_LIBRARY_PAGE, animated)
        : esp_gsp_stack_view_pop(ui, GSP_OBJ_KEY_MUSIC_STACK, animated);
    if (err == ESP_OK) {
        s_page = page;
    }
}

static void render(esp_gsp_handle_t ui, bool force)
{
    music_snapshot_t snapshot;
    if (s_controller == NULL || s_presenter == NULL ||
            music_controller_snapshot(s_controller, &snapshot) != ESP_OK) {
        return;
    }
    (void)music_presenter_render(s_presenter, ui, &snapshot, force);
    (void)esp_gsp_set_value(ui, GSP_BIND_MUSIC_VOLUME_LEVEL, s_volume);
    char text[8];
    snprintf(text, sizeof(text), "%d%%", s_volume);
    (void)esp_gsp_set_text(ui, GSP_BIND_MUSIC_VOLUME_TEXT, text);
}

static int volume_from_y(int32_t y)
{
    if (y < MUSIC_VOLUME_Y_MIN) {
        y = MUSIC_VOLUME_Y_MIN;
    } else if (y > MUSIC_VOLUME_Y_MAX) {
        y = MUSIC_VOLUME_Y_MAX;
    }
    return ((MUSIC_VOLUME_Y_MAX - y) * 100 +
            (MUSIC_VOLUME_Y_MAX - MUSIC_VOLUME_Y_MIN) / 2) /
           (MUSIC_VOLUME_Y_MAX - MUSIC_VOLUME_Y_MIN);
}

static void handle_volume_pointer(esp_gsp_handle_t ui,
                                  const mosaic_event_t *event)
{
    if (!event->data.pointer.pressed) {
        if (s_volume_drag_active) {
            (void)mosaic_settings_set_volume(s_volume, true);
        }
        s_volume_drag_active = false;
        return;
    }
    if (!s_volume_drag_active) {
        if (event->data.pointer.x < MUSIC_VOLUME_X_MIN ||
                event->data.pointer.x > MUSIC_VOLUME_X_MAX ||
                event->data.pointer.y < MUSIC_VOLUME_Y_MIN ||
                event->data.pointer.y > MUSIC_VOLUME_Y_MAX) {
            return;
        }
        s_volume_drag_active = true;
    }
    s_volume = volume_from_y(event->data.pointer.y);
    (void)esp_gsp_set_value(ui, GSP_BIND_MUSIC_VOLUME_LEVEL, s_volume);
    char text[8];
    snprintf(text, sizeof(text), "%d%%", s_volume);
    (void)esp_gsp_set_text(ui, GSP_BIND_MUSIC_VOLUME_TEXT, text);
    (void)mosaic_settings_set_volume(s_volume, false);
}

static void music_started(esp_gsp_handle_t ui)
{
    (void)ui;
    s_page = MUSIC_PLAYER_PAGE;
    s_volume_drag_active = false;
    mosaic_settings_snapshot_t settings;
    if (mosaic_settings_get_snapshot(&settings) == ESP_OK) {
        s_volume = settings.volume;
    }
    if (music_controller_create(&s_controller) != ESP_OK) {
        return;
    }
    if (music_presenter_create(&s_presenter) != ESP_OK) {
        music_controller_delete(s_controller);
        s_controller = NULL;
        return;
    }
}

static void music_stopping(esp_gsp_handle_t ui)
{
    (void)ui;
    music_presenter_delete(s_presenter);
    music_controller_delete(s_controller);
    s_presenter = NULL;
    s_controller = NULL;
}

static void handle_call(esp_gsp_handle_t ui, uint16_t action)
{
    switch (action) {
    case GSP_ACT_ID_MUSIC_BLUETOOTH:
        /* The catalog owns the App switch. Returning here lets STOP tear down
         * the local decoder and release its mixer track before A2DP starts. */
        return;
    case GSP_ACT_ID_MUSIC_MENU:
        show_page(ui, MUSIC_LIBRARY_PAGE, true);
        return;
    case GSP_ACT_ID_MUSIC_NOW_PLAYING:
        show_page(ui, MUSIC_PLAYER_PAGE, true);
        return;
    case GSP_ACT_ID_MUSIC_TRACK_0:
    case GSP_ACT_ID_MUSIC_TRACK_1:
    case GSP_ACT_ID_MUSIC_TRACK_2: {
        const size_t index = action == GSP_ACT_ID_MUSIC_TRACK_0 ? 0U
            : action == GSP_ACT_ID_MUSIC_TRACK_1 ? 1U : 2U;
        (void)music_controller_select(s_controller, index);
        render(ui, false);
        show_page(ui, MUSIC_PLAYER_PAGE, true);
        return;
    }
    case GSP_ACT_ID_MUSIC_PREVIOUS:
        (void)music_controller_previous(s_controller);
        break;
    case GSP_ACT_ID_MUSIC_TOGGLE:
        (void)music_controller_toggle(s_controller);
        break;
    case GSP_ACT_ID_MUSIC_NEXT:
        (void)music_controller_next(s_controller);
        break;
    case GSP_ACT_ID_MUSIC_REPEAT:
        (void)music_controller_toggle_shuffle(s_controller);
        break;
    default:
        return;
    }
    render(ui, false);
}

static void music_event(
    esp_gsp_handle_t ui, const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL || s_controller == NULL || s_presenter == NULL) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
        if (music_controller_start(s_controller, event->timestamp_us) != ESP_OK) {
            break;
        }
        render(ui, true);
        break;
    case MOSAIC_EVENT_UI_CALL:
        handle_call(ui, event->data.call.action_id);
        break;
    case MOSAIC_EVENT_POINTER:
        handle_volume_pointer(ui, event);
        break;
    case MOSAIC_EVENT_TIMER:
        if (music_controller_step(s_controller, event->timestamp_us) == ESP_OK) {
            render(ui, false);
        }
        break;
    case MOSAIC_EVENT_SCENE_CHANGED:
        music_presenter_invalidate(s_presenter);
        render(ui, true);
        break;
    case MOSAIC_EVENT_STOP:
    case MOSAIC_EVENT_MODEL_CHANGED:
    default:
        break;
    }
}

const mosaic_app_descriptor_t mosaic_music_app = {
    .id = MUSIC_APP_ID,
    .launch_action = GSP_ACT_ID_APP_MUSIC,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .routes = s_music_routes,
    .route_count = sizeof(s_music_routes) / sizeof(s_music_routes[0]),
    .routes_without_history = true,
    .back_exits_app = true,
    .name = "music",
    .title = "Music",
    .directory = &gsp_obj_directory_music,
    .root_stack_key = GSP_OBJ_KEY_MUSIC_STACK,
    .disable_swipe = true,
    /* Music owns its top header; shared Shell contributes bottom exit. */
    .root_header_in_stack = true,
    .on_started = music_started,
    .on_stopping = music_stopping,
    .on_event = music_event,
};
