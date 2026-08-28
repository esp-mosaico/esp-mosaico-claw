/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>

#include "esp_gsp.h"
#include "mosaic_app_catalog.h"
#include "mosaic_runtime.h"
#include "welcome_intro_actions.h"
#include "welcome_intro_objects.h"
#include "welcome_keys_objects.h"
#undef GSP_OBJ_WELCOME_RUNTIME_TEXT
#include "welcome_g1_objects.h"
#include "welcome_g2_objects.h"
#undef GSP_OBJ_GESTURE_HAND_VALUE
#undef GSP_OBJ_WELCOME_RUNTIME_TEXT
#include "welcome_g4_objects.h"

#define WELCOME_PAGE_COUNT 5U
#define WELCOME_PAGE_DURATION_US 3000000LL

static esp_gsp_handle_t s_ui;
static uint16_t s_page;
static int64_t s_next_us;
static int64_t s_hand_next_us;
static uint8_t s_hand_phase;
static int32_t s_hand_origin_x;
static int32_t s_hand_origin_y;
static bool s_hand_origin_valid;
static int32_t s_arrow_origin_x;
static int32_t s_arrow_origin_y;
static int32_t s_arrow_right_origin_x;
static int32_t s_arrow_right_origin_y;
static bool s_arrow_origin_valid;

static const gsp_component_directory_t *const s_directories[] = {
    &gsp_obj_directory_welcome_intro,
    &gsp_obj_directory_welcome_keys,
    &gsp_obj_directory_welcome_g1,
    &gsp_obj_directory_welcome_g2,
    &gsp_obj_directory_welcome_g4,
};

static void welcome_hand_start(int64_t now_us)
{
    if (s_ui == NULL || s_page < 2U || s_page >= WELCOME_PAGE_COUNT) {
        s_hand_next_us = 0;
        return;
    }

    if (!s_hand_origin_valid) {
        if (esp_gsp_component_get_position(
                    s_ui, GSP_OBJ_KEY_GESTURE_HAND,
                    &s_hand_origin_x, &s_hand_origin_y) != ESP_OK) {
            s_hand_next_us = now_us + 50000LL;
            return;
        }
        s_hand_origin_valid = true;
    }
    if (!s_arrow_origin_valid) {
        gsp_component_key_t arrow = s_page == 4U
            ? GSP_OBJ_KEY_GESTURE_ARROW_LEFT
            : GSP_OBJ_KEY_GESTURE_ARROW;
        if (esp_gsp_component_get_position(
                    s_ui, arrow, &s_arrow_origin_x,
                    &s_arrow_origin_y) != ESP_OK ||
                (s_page == 4U && esp_gsp_component_get_position(
                    s_ui, GSP_OBJ_KEY_GESTURE_ARROW_RIGHT,
                    &s_arrow_right_origin_x,
                    &s_arrow_right_origin_y) != ESP_OK)) {
            s_hand_next_us = now_us + 50000LL;
            return;
        }
        s_arrow_origin_valid = true;
    }

    int32_t x = s_hand_origin_x;
    int32_t y = s_hand_origin_y;
    if (s_page == 4U) {
        /* Commit the left-swipe start pose before starting either tween.
         * Starting a tween in the same render batch as its reset can make
         * the hand read its previous (right-swipe) position. */
        (void)esp_gsp_component_set_position(
            s_ui, GSP_OBJ_KEY_GESTURE_HAND, x, y);
        (void)esp_gsp_component_set_position(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_LEFT,
            s_arrow_origin_x, s_arrow_origin_y);
        (void)esp_gsp_component_set_position(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_RIGHT,
            s_arrow_right_origin_x, s_arrow_right_origin_y);
        (void)esp_gsp_component_set_visible(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_LEFT, true);
        (void)esp_gsp_component_set_visible(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_RIGHT, false);
        s_hand_phase = 0U;
        s_hand_next_us = now_us + 50000LL;
        return;
    }
    int32_t target_x = x;
    int32_t target_y = y + 56;
    if (s_page == 3U) {
        target_y = y - 56;
    }

    (void)esp_gsp_component_set_position(
        s_ui, GSP_OBJ_KEY_GESTURE_HAND, x, y);
    (void)esp_gsp_component_animate_position_to(
        s_ui, GSP_OBJ_KEY_GESTURE_HAND, target_x, target_y,
        1140U, ESP_GSP_EASE_IN_OUT);
    {
        int32_t arrow_x = s_arrow_origin_x;
        int32_t arrow_y = s_arrow_origin_y;
        if (s_page == 2U) {
            arrow_y += 56;
        } else if (s_page == 3U) {
            arrow_y -= 56;
        } else {
            arrow_x += 72;
        }
        (void)esp_gsp_component_set_position(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW,
            s_arrow_origin_x, s_arrow_origin_y);
        (void)esp_gsp_component_animate_position_to(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW, arrow_x, arrow_y, 1140U,
            ESP_GSP_EASE_IN_OUT);
    }
    s_hand_phase = 1U;
    s_hand_next_us = now_us + 1900000LL;
}

static void welcome_hand_step(int64_t now_us)
{
    if (s_hand_next_us == 0 || now_us < s_hand_next_us) {
        return;
    }
    if (s_page == 4U && s_hand_phase == 0U) {
        (void)esp_gsp_component_animate_position_to(
            s_ui, GSP_OBJ_KEY_GESTURE_HAND,
            s_hand_origin_x - 28, s_hand_origin_y, 625U,
            ESP_GSP_EASE_IN_OUT);
        (void)esp_gsp_component_animate_position_to(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_LEFT,
            s_arrow_origin_x - 28, s_arrow_origin_y, 625U,
            ESP_GSP_EASE_IN_OUT);
        s_hand_phase = 1U;
        s_hand_next_us = now_us + 730000LL;
        return;
    }
    if (s_page == 4U && s_hand_phase == 1U) {
        (void)esp_gsp_component_set_position(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_LEFT,
            s_arrow_origin_x - 28, s_arrow_origin_y);
        (void)esp_gsp_component_set_position(
            s_ui, GSP_OBJ_KEY_GESTURE_HAND,
            s_hand_origin_x, s_hand_origin_y);
        (void)esp_gsp_component_set_position(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_RIGHT,
            s_arrow_right_origin_x, s_arrow_right_origin_y);
        (void)esp_gsp_component_set_visible(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_LEFT, false);
        (void)esp_gsp_component_set_visible(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_RIGHT, true);
        s_hand_phase = 2U;
        /* Let the reset positions commit before starting the rightward
         * tweens; otherwise the arrow tween reads its off-screen position. */
        s_hand_next_us = now_us + 50000LL;
        return;
    }
    if (s_page == 4U && s_hand_phase == 2U) {
        (void)esp_gsp_component_animate_position_to(
            s_ui, GSP_OBJ_KEY_GESTURE_HAND,
            s_hand_origin_x + 40, s_hand_origin_y, 830U,
            ESP_GSP_EASE_IN_OUT);
        (void)esp_gsp_component_animate_position_to(
            s_ui, GSP_OBJ_KEY_GESTURE_ARROW_RIGHT,
            s_arrow_right_origin_x + 40, s_arrow_right_origin_y, 830U,
            ESP_GSP_EASE_IN_OUT);
        s_hand_phase = 3U;
        s_hand_next_us = now_us + 1770000LL;
        return;
    }
    welcome_hand_start(now_us);
}

static void welcome_show(uint16_t page, int64_t now_us)
{
    if (s_ui == NULL || page >= WELCOME_PAGE_COUNT) {
        return;
    }
    s_page = page;
    s_next_us = now_us + WELCOME_PAGE_DURATION_US;
    s_hand_next_us = 0;
    s_hand_origin_valid = false;
    s_arrow_origin_valid = false;
    /* A guide page replaces the previous page.  Snapshot transitions on a
     * partition presenter can otherwise leave old sparse regions visible
     * while the next page is being composed. */
    (void)esp_gsp_goto_scene(s_ui, page, ESP_GSP_NO_TRANSITION);
}

static void welcome_event(
    esp_gsp_handle_t ui, const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
        s_ui = ui;
        s_page = 0;
        s_next_us = event->timestamp_us + WELCOME_PAGE_DURATION_US;
        s_hand_next_us = 0;
        s_hand_origin_valid = false;
        s_arrow_origin_valid = false;
        break;
    case MOSAIC_EVENT_UI_CALL:
        if (event->data.call.action_id == GSP_ACT_ID_WELCOME_NEXT &&
                s_page + 1U < WELCOME_PAGE_COUNT) {
            welcome_show(s_page + 1U, event->timestamp_us);
        }
        break;
    case MOSAIC_EVENT_SCENE_CHANGED:
        if (event->data.scene.scene_id < WELCOME_PAGE_COUNT) {
            s_page = event->data.scene.scene_id;
            s_next_us = event->timestamp_us + WELCOME_PAGE_DURATION_US;
            s_hand_origin_valid = false;
            s_arrow_origin_valid = false;
            welcome_hand_start(event->timestamp_us);
        }
        break;
    case MOSAIC_EVENT_TIMER:
        welcome_hand_step(event->timestamp_us);
        if (s_page + 1U < WELCOME_PAGE_COUNT &&
                event->timestamp_us >= s_next_us) {
            welcome_show(s_page + 1U, event->timestamp_us);
        }
        break;
    case MOSAIC_EVENT_STOP:
        s_ui = NULL;
        s_page = 0;
        s_next_us = 0;
        s_hand_next_us = 0;
        s_hand_phase = 0;
        s_hand_origin_valid = false;
        s_arrow_origin_valid = false;
        break;
    default:
        break;
    }
}

const mosaic_app_descriptor_t mosaic_welcome_app = {
    .id = 11,
    .launch_action = UINT16_MAX,
    .back_action = GSP_ACT_ID_WELCOME_EXIT,
    .name = "welcome",
    .title = "Welcome",
    .directory = &gsp_obj_directory_welcome_intro,
    .directories = s_directories,
    .directory_count = sizeof(s_directories) / sizeof(s_directories[0]),
    .disable_swipe = true,
    .custom_shell = true,
    .on_event = welcome_event,
};
