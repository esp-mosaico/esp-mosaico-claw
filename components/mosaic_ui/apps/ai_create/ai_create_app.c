/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "ai_create_actions.h"
#include "ai_create_objects.h"

#include <string.h>

#include "ai_create_controller.h"
#include "ai_create_presenter.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "mosaic_ai_create_runtime.h"
#include "mosaic_runtime.h"

#define AI_CREATE_VOICE_LEFT              76
#define AI_CREATE_VOICE_RIGHT             456
#define AI_CREATE_VOICE_TOP               392
#define AI_CREATE_VOICE_BOTTOM            468
#define AI_CREATE_VOICE_CANCEL_DISTANCE   72
#define AI_CREATE_VOICE_MIN_HOLD_US        300000LL
#define AI_CREATE_SESSION_SWIPE_TOP        82
#define AI_CREATE_SESSION_SWIPE_BOTTOM     382
#define AI_CREATE_SESSION_SWIPE_DISTANCE   72
#define AI_CREATE_SESSION_PAGE_Y            82
#define AI_CREATE_SESSION_PAGE_TRAVEL       480
#define AI_CREATE_SESSION_EXIT_MS           110U
#define AI_CREATE_SESSION_ENTER_MS          160U
#define AI_CREATE_SESSION_ENTER_DELAY_US    33000LL

typedef enum {
    AI_CREATE_SESSION_TRANSITION_IDLE = 0,
    AI_CREATE_SESSION_TRANSITION_EXITING,
    AI_CREATE_SESSION_TRANSITION_ENTER_PENDING,
    AI_CREATE_SESSION_TRANSITION_ENTERING,
} ai_create_session_transition_t;

static const char *TAG = "ai_create_app";

static ai_create_presenter_handle_t s_presenter;
static EXT_RAM_BSS_ATTR ai_create_controller_snapshot_t s_model;
static EXT_RAM_BSS_ATTR claw_session_mgr_alias_map_t s_sessions;
static uint32_t s_rendered_model_revision;
static uint32_t s_rendered_sessions_revision;
static size_t s_session_page;
static ai_create_session_transition_t s_session_transition;
static int s_session_transition_direction;
static int64_t s_session_transition_due_us;
static bool s_pointer_pressed;
static bool s_voice_pressed;
static bool s_voice_notice_pressed;
static int32_t s_pointer_press_x;
static int32_t s_pointer_press_y;
static int32_t s_voice_press_y;
static int64_t s_voice_started_us;

static ai_create_controller_handle_t controller(void)
{
    return mosaic_ai_create_runtime_controller();
}

static void refresh_sessions(void)
{
    (void)mosaic_ai_create_runtime_refresh_sessions();
}

static size_t session_page_count(void)
{
    return (s_sessions.session_count + AI_CREATE_SESSION_PAGE_SIZE - 1U) /
           AI_CREATE_SESSION_PAGE_SIZE;
}

static bool move_session_page(int direction)
{
    const size_t page_count = session_page_count();
    if (direction < 0 && s_session_page > 0U) {
        s_session_page--;
        return true;
    }
    if (direction > 0 && s_session_page + 1U < page_count) {
        s_session_page++;
        return true;
    }
    return false;
}

static esp_gsp_err_t animate_session_page_x(
    esp_gsp_handle_t ui, int32_t from_x, int32_t to_x,
    uint32_t duration_ms, esp_gsp_ease_t ease)
{
    const gsp_value_t from = {
        .type = GSP_VALUE_I32,
        .data.i32 = from_x,
    };
    const gsp_value_t to = {
        .type = GSP_VALUE_I32,
        .data.i32 = to_x,
    };
    return esp_gsp_component_animate_property(
        ui, GSP_OBJ_KEY_AI_SESSION_PAGE, GSP_PROP_KEY_X,
        &from, &to, duration_ms, ease);
}

static void reset_session_page_transition(esp_gsp_handle_t ui)
{
    s_session_transition = AI_CREATE_SESSION_TRANSITION_IDLE;
    s_session_transition_direction = 0;
    s_session_transition_due_us = 0;
    if (ui != NULL) {
        (void)esp_gsp_component_stop_position_animation(
            ui, GSP_OBJ_KEY_AI_SESSION_PAGE);
        (void)esp_gsp_component_set_position(
            ui, GSP_OBJ_KEY_AI_SESSION_PAGE,
            0, AI_CREATE_SESSION_PAGE_Y);
    }
}

static bool start_session_page_transition(
    esp_gsp_handle_t ui, int direction, int64_t now_us)
{
    if (ui == NULL ||
            s_session_transition != AI_CREATE_SESSION_TRANSITION_IDLE) {
        return false;
    }
    const size_t page_count = session_page_count();
    const bool can_move = direction < 0
        ? s_session_page > 0U
        : direction > 0 && s_session_page + 1U < page_count;
    if (!can_move) return false;

    const int32_t exit_x = direction > 0
        ? -AI_CREATE_SESSION_PAGE_TRAVEL
        : AI_CREATE_SESSION_PAGE_TRAVEL;
    if (animate_session_page_x(
            ui, 0, exit_x, AI_CREATE_SESSION_EXIT_MS,
            ESP_GSP_EASE_IN_OUT) != ESP_GSP_OK) {
        /* Preserve paging if animation setup is unavailable in a host or a
         * freshly replaced scene. */
        return move_session_page(direction);
    }
    s_session_transition = AI_CREATE_SESSION_TRANSITION_EXITING;
    s_session_transition_direction = direction;
    s_session_transition_due_us = now_us +
        (int64_t)AI_CREATE_SESSION_EXIT_MS * 1000LL;
    return false;
}

static bool step_session_page_transition(
    esp_gsp_handle_t ui, int64_t now_us)
{
    if (s_session_transition == AI_CREATE_SESSION_TRANSITION_IDLE ||
            now_us < s_session_transition_due_us) {
        return false;
    }
    if (s_session_transition == AI_CREATE_SESSION_TRANSITION_EXITING) {
        if (!move_session_page(s_session_transition_direction)) {
            reset_session_page_transition(ui);
            return false;
        }
        const int32_t enter_x = s_session_transition_direction > 0
            ? AI_CREATE_SESSION_PAGE_TRAVEL
            : -AI_CREATE_SESSION_PAGE_TRAVEL;
        (void)esp_gsp_component_set_position(
            ui, GSP_OBJ_KEY_AI_SESSION_PAGE,
            enter_x, AI_CREATE_SESSION_PAGE_Y);
        s_session_transition = AI_CREATE_SESSION_TRANSITION_ENTER_PENDING;
        /* Give the presenter one frame to replace the retained row contents.
         * The enter tween has an explicit origin, so it does not depend on
         * when this queued position reset reaches the renderer. */
        s_session_transition_due_us = now_us +
            AI_CREATE_SESSION_ENTER_DELAY_US;
        return true;
    }
    if (s_session_transition ==
            AI_CREATE_SESSION_TRANSITION_ENTER_PENDING) {
        const int32_t enter_x = s_session_transition_direction > 0
            ? AI_CREATE_SESSION_PAGE_TRAVEL
            : -AI_CREATE_SESSION_PAGE_TRAVEL;
        if (animate_session_page_x(
                ui, enter_x, 0, AI_CREATE_SESSION_ENTER_MS,
                ESP_GSP_EASE_OUT) != ESP_GSP_OK) {
            reset_session_page_transition(ui);
            return false;
        }
        s_session_transition = AI_CREATE_SESSION_TRANSITION_ENTERING;
        s_session_transition_due_us = now_us +
            (int64_t)AI_CREATE_SESSION_ENTER_MS * 1000LL;
        return false;
    }
    reset_session_page_transition(ui);
    return false;
}

static void render(esp_gsp_handle_t ui, int64_t now_us,
                   bool force, bool refresh_session_cache)
{
    ai_create_controller_handle_t current = controller();
    if (!current || !s_presenter) return;

    uint32_t model_revision = 0;
    if (ai_create_controller_get_revision(
            current, &model_revision) != ESP_OK) return;
    const bool model_changed = force ||
        s_rendered_model_revision != model_revision;
    if (model_changed) {
        if (ai_create_controller_get_snapshot(current, &s_model) != ESP_OK) {
            return;
        }
        s_rendered_model_revision = s_model.revision;
    }

    bool sessions_changed = false;
    if (force || refresh_session_cache) {
        uint32_t revision = 0;
        if (mosaic_ai_create_runtime_get_sessions_revision(
                &revision) == ESP_OK &&
            (force || revision != s_rendered_sessions_revision)) {
            if (mosaic_ai_create_runtime_get_sessions(
                    &s_sessions, &revision) == ESP_OK) {
                s_rendered_sessions_revision = revision;
                sessions_changed = true;
            }
        }
    }
    const size_t page_count = session_page_count();
    const size_t previous_page = s_session_page;
    if (page_count == 0U) s_session_page = 0U;
    else if (s_session_page >= page_count) s_session_page = page_count - 1U;
    const bool page_changed = previous_page != s_session_page;
    if (s_model.voice_state == AI_CREATE_VOICE_STATE_IDLE ||
        s_model.voice_state == AI_CREATE_VOICE_STATE_ERROR) {
        s_voice_started_us = 0;
        if (s_model.voice_operation_id == 0) {
            s_voice_pressed = false;
        }
    }
    const bool voice_animated =
        s_model.voice_state == AI_CREATE_VOICE_STATE_PREPARING ||
        s_model.voice_state == AI_CREATE_VOICE_STATE_LISTENING ||
        s_model.voice_state == AI_CREATE_VOICE_STATE_FINALIZING ||
        s_model.voice_state == AI_CREATE_VOICE_STATE_CANCELLING;
    if (!force && !model_changed && !sessions_changed && !page_changed &&
        !voice_animated) {
        return;
    }
    uint8_t voice_loudness = 0;
    if (s_model.voice_state == AI_CREATE_VOICE_STATE_PREPARING ||
        s_model.voice_state == AI_CREATE_VOICE_STATE_LISTENING) {
        (void)ai_create_controller_get_voice_loudness(
            current, &voice_loudness);
    }
    (void)ai_create_presenter_render(s_presenter, ui, &s_model,
                                     &s_sessions, s_session_page, now_us,
                                     s_voice_started_us,
                                     voice_loudness);
}

static cap_im_ai_create_mode_t mode_for_action(uint16_t action)
{
    switch (action) {
    case GSP_ACT_ID_AI_MODE_CREATE:
        return CAP_IM_AI_CREATE_MODE_CREATE;
    case GSP_ACT_ID_AI_MODE_SKILL:
        return CAP_IM_AI_CREATE_MODE_SKILL;
    case GSP_ACT_ID_AI_MODE_MEMORY:
        return CAP_IM_AI_CREATE_MODE_MEMORY;
    case GSP_ACT_ID_AI_MODE_PLAN:
        return CAP_IM_AI_CREATE_MODE_PLAN;
    case GSP_ACT_ID_AI_MODE_SCHEDULE:
        return CAP_IM_AI_CREATE_MODE_SCHEDULE;
    default:
        return CAP_IM_AI_CREATE_MODE_QUICK;
    }
}

static bool is_mode_action(uint16_t action)
{
    return action == GSP_ACT_ID_AI_MODE_CREATE ||
           action == GSP_ACT_ID_AI_MODE_SKILL ||
           action == GSP_ACT_ID_AI_MODE_MEMORY ||
           action == GSP_ACT_ID_AI_MODE_PLAN ||
           action == GSP_ACT_ID_AI_MODE_QUICK ||
           action == GSP_ACT_ID_AI_MODE_SCHEDULE;
}

static void dispatch_call(esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    ai_create_controller_handle_t current = controller();
    if (!current) return;
    const uint16_t action = event->data.call.action_id;
    ai_create_command_t command = {0};
    bool handled = true;
    if (is_mode_action(action)) {
        command.type = AI_CREATE_COMMAND_SELECT_MODE;
        command.data.mode = mode_for_action(action);
    } else {
        switch (action) {
        case GSP_ACT_ID_AI_CLEAR_MODE:
            command.type = AI_CREATE_COMMAND_CLEAR_MODE;
            break;
        case GSP_ACT_ID_AI_NEW:
            reset_session_page_transition(ui);
            command.type = AI_CREATE_COMMAND_NEW_SESSION;
            break;
        case GSP_ACT_ID_AI_RESUME_WORK:
            reset_session_page_transition(ui);
            command.type = AI_CREATE_COMMAND_OPEN_SESSION_PICKER;
            s_session_page = 0U;
            refresh_sessions();
            break;
        case GSP_ACT_ID_AI_SESSION_BACK:
            reset_session_page_transition(ui);
            command.type = AI_CREATE_COMMAND_CLOSE_SESSION_PICKER;
            break;
        case GSP_ACT_ID_AI_RESUME_SESSION:
            handled = false;
            if (s_session_transition != AI_CREATE_SESSION_TRANSITION_IDLE) {
                break;
            }
            (void)mosaic_ai_create_runtime_select_session(
                s_session_page * AI_CREATE_SESSION_PAGE_SIZE +
                event->data.call.arg);
            break;
        case GSP_ACT_ID_AI_DELETE_SESSION: {
            if (s_session_transition != AI_CREATE_SESSION_TRANSITION_IDLE) {
                handled = false;
                break;
            }
            const size_t index =
                s_session_page * AI_CREATE_SESSION_PAGE_SIZE +
                event->data.call.arg;
            if (event->data.call.arg >= AI_CREATE_SESSION_PAGE_SIZE ||
                index >= s_sessions.session_count) {
                handled = false;
                break;
            }
            command.type = AI_CREATE_COMMAND_REQUEST_DELETE_SESSION;
            strlcpy(command.data.session_alias,
                    s_sessions.sessions[index],
                    sizeof(command.data.session_alias));
            break;
        }
        case GSP_ACT_ID_AI_CANCEL_DELETE_SESSION:
            command.type = AI_CREATE_COMMAND_CANCEL_DELETE_SESSION;
            break;
        case GSP_ACT_ID_AI_CONFIRM_DELETE_SESSION:
            handled = false;
            (void)mosaic_ai_create_runtime_confirm_session_delete();
            break;
        case GSP_ACT_ID_AI_LEAVE_CHAT:
            command.type = AI_CREATE_COMMAND_LEAVE_CHAT;
            break;
        default:
            handled = false;
            break;
        }
    }
    if (handled) {
        (void)ai_create_controller_dispatch(current, &command);
        if (command.type == AI_CREATE_COMMAND_NEW_SESSION) {
            refresh_sessions();
        }
    }
}

static bool voice_input_allowed(
    const ai_create_controller_snapshot_t *model)
{
    return model &&
        (model->screen == AI_CREATE_SCREEN_WELCOME ||
         model->screen == AI_CREATE_SCREEN_CHAT) &&
        (model->composer == AI_CREATE_COMPOSER_DEFAULT ||
         model->composer == AI_CREATE_COMPOSER_MODE);
}

static bool ai_create_back(esp_gsp_handle_t ui, int64_t timestamp_us)
{
    ai_create_controller_handle_t current = controller();
    if (current == NULL ||
            ai_create_controller_get_snapshot(current, &s_model) != ESP_OK) {
        return false;
    }
    ai_create_command_t command = {0};
    if (s_model.session_delete_confirm_visible) {
        if (s_model.session_deleting) return true;
        command.type = AI_CREATE_COMMAND_CANCEL_DELETE_SESSION;
        if (ai_create_controller_dispatch(current, &command) != ESP_OK) {
            return false;
        }
        render(ui, timestamp_us, true, false);
        return true;
    }
    if (s_model.screen == AI_CREATE_SCREEN_SESSION_PICKER) {
        reset_session_page_transition(ui);
        command.type = AI_CREATE_COMMAND_CLOSE_SESSION_PICKER;
    } else if (s_model.screen == AI_CREATE_SCREEN_CHAT) {
        command.type = AI_CREATE_COMMAND_LEAVE_CHAT;
    } else {
        /* Physical Back will route the root page to Hub after this hook. */
        (void)ai_create_controller_abort_current(current);
        return false;
    }
    if (ai_create_controller_dispatch(current, &command) != ESP_OK) {
        return false;
    }
    render(ui, timestamp_us, true, false);
    return true;
}

static bool dispatch_pointer(esp_gsp_handle_t ui,
                             const mosaic_event_t *event)
{
    ai_create_controller_handle_t current = controller();
    if (!current ||
        ai_create_controller_get_snapshot(current, &s_model) != ESP_OK) {
        return false;
    }
    const bool in_voice = event->data.pointer.x >= AI_CREATE_VOICE_LEFT &&
        event->data.pointer.x <= AI_CREATE_VOICE_RIGHT &&
        event->data.pointer.y >= AI_CREATE_VOICE_TOP &&
        event->data.pointer.y <= AI_CREATE_VOICE_BOTTOM;
    const bool pointer_down = event->data.pointer.pressed &&
        !s_pointer_pressed;
    const bool pointer_up = !event->data.pointer.pressed &&
        s_pointer_pressed;
    if (pointer_down) {
        s_pointer_press_x = event->data.pointer.x;
        s_pointer_press_y = event->data.pointer.y;
    }
    s_pointer_pressed = event->data.pointer.pressed;
    if (pointer_down && in_voice && !s_voice_pressed &&
        voice_input_allowed(&s_model)) {
        const esp_err_t err = ai_create_controller_voice_start(current);
        if (err == ESP_OK) {
            s_voice_pressed = true;
            s_voice_press_y = event->data.pointer.y;
            s_voice_started_us = event->timestamp_us;
            ESP_LOGI(TAG, "voice press accepted x=%ld y=%ld",
                     (long)event->data.pointer.x,
                     (long)event->data.pointer.y);
        } else {
            /* Unavailable voice services publish recovery copy on press.
             * Track this failed gesture separately because there is no voice
             * operation for the normal release path to finish. */
            s_voice_notice_pressed = true;
        }
    } else if (!event->data.pointer.pressed && s_voice_pressed) {
        const int64_t held_us = event->timestamp_us >= s_voice_started_us
                                    ? event->timestamp_us - s_voice_started_us
                                    : 0;
        const bool gesture_send = event->data.pointer.y >=
            s_voice_press_y - AI_CREATE_VOICE_CANCEL_DISTANCE;
        const bool short_press = held_us < AI_CREATE_VOICE_MIN_HOLD_US;
        const bool send = gesture_send && !short_press;
        s_voice_pressed = false;
        if (short_press) {
            ESP_LOGW(TAG, "voice short press cancelled duration=%lldms "
                     "minimum=%lldms",
                     (long long)(held_us / 1000),
                     (long long)(AI_CREATE_VOICE_MIN_HOLD_US / 1000));
        }
        esp_err_t err = ai_create_controller_voice_stop(current, send);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "voice release accepted action=%s y=%ld "
                     "duration=%lldms",
                     short_press ? "short_cancel"
                                 : (send ? "send" : "cancel"),
                     (long)event->data.pointer.y,
                     (long long)(held_us / 1000));
        } else {
            ESP_LOGE(TAG, "voice release failed action=%s: %s",
                     send ? "send" : "cancel", esp_err_to_name(err));
        }
    } else if (!event->data.pointer.pressed && s_voice_notice_pressed) {
        s_voice_notice_pressed = false;
        const ai_create_command_t dismiss = {
            .type = AI_CREATE_COMMAND_DISMISS_NOTICE,
        };
        (void)ai_create_controller_dispatch(current, &dismiss);
    }
    if (pointer_up && !s_voice_pressed &&
        s_model.screen == AI_CREATE_SCREEN_SESSION_PICKER &&
        !s_model.session_delete_confirm_visible &&
        s_pointer_press_y >= AI_CREATE_SESSION_SWIPE_TOP &&
        s_pointer_press_y <= AI_CREATE_SESSION_SWIPE_BOTTOM) {
        const int32_t dx = event->data.pointer.x - s_pointer_press_x;
        const int32_t dy = event->data.pointer.y - s_pointer_press_y;
        const int32_t abs_dx = dx < 0 ? -dx : dx;
        const int32_t abs_dy = dy < 0 ? -dy : dy;
        if (abs_dx >= AI_CREATE_SESSION_SWIPE_DISTANCE && abs_dx > abs_dy) {
            return start_session_page_transition(
                ui, dx < 0 ? 1 : -1, event->timestamp_us);
        }
    }
    return false;
}

static void ai_create_event(esp_gsp_handle_t ui,
                            const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    ai_create_controller_handle_t current = controller();
    if (!event || !current) return;
    mosaic_ai_create_runtime_begin_ui_update();
    bool force_render = false;
    bool refresh_session_cache = false;
    switch (event->type) {
    case MOSAIC_EVENT_START:
        if (!s_presenter &&
            ai_create_presenter_create(&s_presenter) != ESP_OK) {
            mosaic_ai_create_runtime_end_ui_update();
            return;
        }
        s_session_page = 0U;
        reset_session_page_transition(ui);
        s_pointer_pressed = false;
        s_voice_pressed = false;
        s_voice_notice_pressed = false;
        s_voice_started_us = 0;
        memset(&s_sessions, 0, sizeof(s_sessions));
        s_rendered_model_revision = 0;
        s_rendered_sessions_revision = 0;
        (void)ai_create_controller_start(current);
        refresh_sessions();
        force_render = true;
        refresh_session_cache = true;
        break;
    case MOSAIC_EVENT_STOP:
        reset_session_page_transition(ui);
        s_pointer_pressed = false;
        s_voice_pressed = false;
        s_voice_notice_pressed = false;
        s_voice_started_us = 0;
        (void)ai_create_controller_stop(current);
        ai_create_presenter_delete(s_presenter);
        s_presenter = NULL;
        mosaic_ai_create_runtime_end_ui_update();
        return;
    case MOSAIC_EVENT_UI_CALL:
        dispatch_call(ui, event);
        /* Host adapters may complete deterministic session operations during
         * the call itself. Firmware workers publish a later invalidation, so
         * this opportunistic revision check is cheap on device and prevents
         * a stale picker frame in simulation. */
        refresh_session_cache = true;
        break;
    case MOSAIC_EVENT_POINTER:
        force_render = dispatch_pointer(ui, event);
        break;
    case MOSAIC_EVENT_TIMER:
        (void)mosaic_ai_create_runtime_step(event->timestamp_us);
        force_render = step_session_page_transition(
            ui, event->timestamp_us);
        break;
    case MOSAIC_EVENT_MODEL_CHANGED:
        refresh_session_cache = true;
        break;
    case MOSAIC_EVENT_SCENE_CHANGED:
        /* A new GSP scene has fresh bind storage. Presenter-side diff caches
         * refer to the previous instance and must be replayed in full. */
        ai_create_presenter_invalidate(s_presenter);
        reset_session_page_transition(ui);
        force_render = true;
        refresh_session_cache = true;
        break;
    default:
        break;
    }
    render(ui, event->timestamp_us, force_render, refresh_session_cache);
    mosaic_ai_create_runtime_end_ui_update();
}

const mosaic_app_descriptor_t mosaic_ai_create_app = {
    .id = 8,
    .launch_action = GSP_ACT_ID_APP_AI_CREATE,
    .back_action = GSP_ACT_ID_AI_EXIT_APP,
    .name = "ai_create",
    .title = "AI Create",
    .directory = &gsp_obj_directory_ai_create,
    /* Message rows require 15 instances; the retained delete-confirmation
     * overlay adds nested modal/control state while the picker stays alive. */
    .instance_slots = 20,
    .disable_swipe = true,
    /* Keep the full-canvas AI layout while attaching the shared Shell's
     * bottom home indicator and upward-exit input interceptor. */
    .root_header_in_stack = true,
    .on_event = ai_create_event,
    .on_back = ai_create_back,
};
