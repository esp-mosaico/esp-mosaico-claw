/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

/* Input handling: compiled-action dispatch (taps), press feedback,
 * and the effect callbacks behind the gesture classifier — value
 * drag, list scroll, page swipe. */

#include "gsp_ui_core_priv.h"
#include "gsp/gsp_build_caps.h"
#include "gsp/format/gsp_common_format.h"

#include "gsp/gsp_component_route.h"
#include "gsp/format/gsp_gsb_format.h"
#include "gsp/gsp_input.h"
#include "gsp/gsp_transform.h"
#include "gsp/gsp_text_edit.h"
#include "gsp/gsp_update.h"
#include "gsp_log_priv.h"
#include "gsp_binary_priv.h"
#include "gsp_runtime_priv.h"

#include <limits.h>

static const char *TAG = "esp_gsp";

#define VIEWPORT_SNAPSHOT_RETRY_INITIAL_US UINT32_C(1000000)
#define VIEWPORT_SNAPSHOT_RETRY_MAX_US UINT32_C(8000000)
#define PAGE_FLOW_EDGE_OVERSCROLL_DIVISOR 3

static bool transform_group_hidden(const gsp_runtime_t *runtime,
                                   uint16_t group_id);

/* Structural gestures bypass hit actions. Honor their optional authored
 * enabled property through the same component decoder as public setters.
 * An absent property preserves the existing ungated behavior; do not infer
 * an owner from an arbitrary child hit, which may override its ancestors. */
static bool motion_component_enabled(gsp_ui_core_t *gsp,
                                     const gsp_component_entry_t *entry)
{
    const gsp_component_view_t view = {
        .runtime = gsp->components.manager.runtime,
        .entry = entry,
    };
    gsp_value_t value;
    gsp_err_t ret = gsp_component_get(&view, GSP_COMPONENT_PROP_ENABLED, &value);
    return ret == GSP_ERR_NOT_FOUND ||
           (ret == GSP_OK && value.type == GSP_VALUE_BOOL && value.data.boolean);
}

static void hit_row_identity(gsp_ui_core_t *gsp, gsp_instance_handle_t instance,
                             uint16_t *list_id, uint32_t *item)
{
    *list_id = ESP_GSP_LIST_NONE;
    *item = 0;
    if (instance == GSP_INSTANCE_HANDLE_INVALID) {
        return;
    }
    for (size_t index = 0; index < gsp_effective_capacity(gsp, ESP_GSP_FIELD_MAX_LISTS); ++index) {
        esp_gsp_list_slot_t *list = &gsp->lists.pool[index];
        gsp_list_state_t *state = esp_gsp_list_state(gsp, list);
        if (!list->used || state == NULL) {
            continue;
        }
        for (size_t slot = 0; slot < state->slot_capacity; ++slot) {
            if (state->rows[slot] == instance) {
                *list_id = (uint16_t)index;
                *item = state->items[slot];
                return;
            }
        }
    }
}

static gsp_err_t action_handler(gsp_runtime_t *runtime,
                                gsp_update_transaction_t *transaction,
                                const gsp_action_view_t *action,
                                void *user_ctx)
{
    (void)runtime;
    (void)transaction;
    gsp_ui_core_t *gsp = user_ctx;
    if (action->kind == GSP_ACTION_KIND_GOTO) {
        gsp->nav.pending_goto = (int)action->arg;
        gsp->nav.pending_transition = ESP_GSP_SLIDE_LEFT;
        return GSP_OK;
    }
    if (action->kind == GSP_ACTION_KIND_SET_PAGE) {
        uint16_t group = (uint16_t)(action->arg >> 16);
        uint16_t page = (uint16_t)action->arg;
        const gsp_component_directory_t *directory =
            gsp->components.manager.directory;
        if (directory == NULL) {
            return GSP_ERR_INVALID_STATE;
        }
        for (uint16_t entry = 0; entry < directory->count; ++entry) {
            const gsp_component_entry_t *descriptor =
                &directory->entries[entry];
            if (descriptor->kind != GSP_COMPONENT_KIND_PAGE_FLOW ||
                    descriptor->config == NULL ||
                    ((const gsp_page_flow_config_t *)descriptor->config)
                    ->group_id != group) {
                continue;
            }
            gsp_component_instance_t *instance;
            gsp_err_t ret = gsp_component_manager_activate(
                                &gsp->components.manager, entry, ESP_GSP_DRIVER_PAGE_FLOW,
                                &instance);
            return ret == GSP_OK
                   ? esp_gsp_page_flow_set_page_tracked(
                       gsp, instance, descriptor->config, page,
                       action->animated)
                   : ret;
        }
        return GSP_ERR_NOT_FOUND;
    }
    if (action->kind == GSP_ACTION_KIND_STACK_PUSH ||
            action->kind == GSP_ACTION_KIND_STACK_POP ||
            action->kind == GSP_ACTION_KIND_DRAWER_OPEN ||
            action->kind == GSP_ACTION_KIND_DRAWER_CLOSE) {
        uint16_t group = (uint16_t)(action->arg >> 16);
        uint16_t page = (uint16_t)action->arg;
        const gsp_component_directory_t *directory =
            gsp->components.manager.directory;
        if (directory == NULL) {
            return GSP_ERR_INVALID_STATE;
        }
        for (uint16_t entry = 0; entry < directory->count; ++entry) {
            const gsp_component_entry_t *descriptor =
                &directory->entries[entry];
            if (descriptor->config == NULL) {
                continue;
            }
            if (action->kind == GSP_ACTION_KIND_STACK_PUSH ||
                    action->kind == GSP_ACTION_KIND_STACK_POP) {
                if (descriptor->kind != GSP_COMPONENT_KIND_STACK_VIEW ||
                        ((const gsp_stack_view_config_t *)descriptor->config)
                        ->group_id != group) {
                    continue;
                }
                gsp_component_instance_t *instance;
                gsp_err_t ret = gsp_component_manager_activate(
                                    &gsp->components.manager, entry,
                                    ESP_GSP_DRIVER_STACK_VIEW, &instance);
                if (ret != GSP_OK) {
                    return ret;
                }
                return action->kind == GSP_ACTION_KIND_STACK_PUSH
                       ? esp_gsp_stack_view_push_tracked(
                           gsp, instance, descriptor->config, page,
                           action->animated)
                       : esp_gsp_stack_view_pop_tracked(
                           gsp, instance, descriptor->config,
                           action->animated);
            }
            if (descriptor->kind != GSP_COMPONENT_KIND_DRAWER ||
                    ((const gsp_drawer_config_t *)descriptor->config)
                    ->group_id != group) {
                continue;
            }
            gsp_component_instance_t *instance;
            gsp_err_t ret = gsp_component_manager_activate(
                                &gsp->components.manager, entry, ESP_GSP_DRIVER_DRAWER,
                                &instance);
            return ret == GSP_OK
                   ? esp_gsp_drawer_set_open_tracked(
                       gsp, instance, descriptor->config,
                       action->kind == GSP_ACTION_KIND_DRAWER_OPEN,
                       action->animated)
                   : ret;
        }
        return GSP_ERR_NOT_FOUND;
    }
    if (action->kind == GSP_ACTION_KIND_PLAY_GLOW) {
        if (action->arg < 100 || action->arg > 60000 || action->target_property == UINT16_MAX) {
            return GSP_ERR_INVALID_FORMAT;
        }
        esp_gsp_cmd_t glow = {
            .type = CMD_ANIMATE,
            .bind = action->target_property,
            .slot = ESP_GSP_ANIM_COMMAND_SLOT(ESP_GSP_EASE_OUT, ESP_GSP_ANIM_TARGET_PROPERTY | ESP_GSP_ANIM_CONFIGURED | ESP_GSP_ANIM_DISCARD_HIDDEN),
            .rect = {.x1 = 1},
            .x = (int16_t)gsp->current_scene,
            .value = 100, .value2 = 0, .pointer_size = action->arg,
        };
        /* Input and animation run on the same UI task; no application callback. */
        esp_gsp_ui_apply(gsp, &glow);
        return GSP_OK;
    }
    if (action->kind == GSP_ACTION_KIND_ANIMATE_TOGGLE) {
        /* Tween the switch value to the other rail. */
        uint32_t current = 0;
        gsp_runtime_t *current_runtime =
            gsp_context_runtime(esp_gsp_current_ctx(gsp));
        if (action->target_property != UINT16_MAX) {
            (void)gsp_update_property_i32(
                current_runtime, action->target_property, &current);
        } else {
            (void)gsp_update_bind_i32(
                current_runtime, action->target_bind, &current);
        }
        bool turn_on = current < 50;
        gsp_component_instance_t *instance;
        uint16_t entry = action->target_property != UINT16_MAX
                         ? gsp_component_route_resolve_property(
                             &gsp->components.manager, action->target_property)
                         : gsp_component_route_resolve(
                             &gsp->components.manager, action->target_bind);
        if (entry != GSP_COMPONENT_ROUTE_NO_ENTRY &&
                gsp_component_manager_activate(&gsp->components.manager,
                                               entry,
                                               ESP_GSP_DRIVER_BOOLEAN,
                                               &instance) == GSP_OK) {
            (void)gsp_boolean_driver_toggle(
                &gsp->components.manager, instance,
                action->target_bind,
                action->target_property,
                (uint64_t)gsp->now_us, &turn_on);
        } else {
            /* Instance pool exhausted: use the same direct-property
             * tween path without a managed driver instance. */
            GSP_LOGW(TAG, "component pool exhausted (capacity %d)",
                     gsp_effective_capacity(gsp, ESP_GSP_FIELD_COMPONENT_INSTANCES));
            uint8_t flags = ESP_GSP_ANIM_FROM_CURRENT;
            if (action->target_property != UINT16_MAX) {
                flags |= ESP_GSP_ANIM_TARGET_PROPERTY;
            }
            esp_gsp_cmd_t knob = {
                .type = CMD_ANIMATE,
                .bind = action->target_property != UINT16_MAX
                ? action->target_property : action->target_bind,
                .slot = ESP_GSP_ANIM_COMMAND_SLOT(
                    ESP_GSP_EASE_OUT, flags),
                .x = (int16_t)gsp->current_scene,
                .value2 = turn_on ? 100U : 0U,
                .pointer_size = 130,
            };
            esp_gsp_ui_apply(gsp, &knob);
        }
        /* Track color tween: the compiler packs the track color bind
         * and the OFF/ON native colors into the action params. */
        gsp_boolean_track_tween_t track_tween;
        if (gsp_boolean_track_tween_decode(
                    action->params, action->param_len,
                    &track_tween) == GSP_OK) {
            uint32_t to_color = turn_on
                                ? track_tween.on_color : track_tween.off_color;
            const gsp_scene_view_t *scene =
                &esp_gsp_current_ctx(gsp)->scene;
            bool direct_track = scene->property_table != NULL &&
                                track_tween.property < scene->property_count;
            uint8_t flags = ESP_GSP_ANIM_TARGET_COLOR |
                            ESP_GSP_ANIM_FROM_CURRENT;
            if (direct_track) {
                flags |= ESP_GSP_ANIM_TARGET_PROPERTY;
            }
            esp_gsp_cmd_t step = {
                .type = CMD_ANIMATE,
                .bind = direct_track
                ? track_tween.property : track_tween.bind,
                .slot = ESP_GSP_ANIM_COMMAND_SLOT(
                    ESP_GSP_EASE_OUT, flags),
                .x = (int16_t)gsp->current_scene,
                .value2 = to_color,
                .pointer_size = 130,
            };
            /* Render-task context: apply directly, no queue trip. */
            esp_gsp_ui_apply(gsp, &step);
        }
        return GSP_OK;
    }
    if (action->kind == GSP_ACTION_KIND_SET_TEXT) {
        if ((action->arg & GSP_FORMAT_GSB_ACTION_ARG_FLAG_DROPDOWN_TEXT_INDEX) != 0) {
            gsp_err_t dropdown = esp_gsp_dropdown_action_text(gsp, action->target_bind,
                                 action->arg & ~GSP_FORMAT_GSB_ACTION_ARG_FLAG_DROPDOWN_TEXT_INDEX);
            if (dropdown != GSP_ERR_NOT_FOUND) {
                return dropdown;
            }
        }
        /* Compiler-authored text: params carry the NUL-terminated
         * UTF-8 string (dropdown rows etc.). */
        if (action->param_len > 0 && action->params != NULL) {
            (void)esp_gsp_set_text(gsp, action->target_bind,
                                   (const char *)action->params);
        }
        return GSP_OK;
    }
    if (action->kind == GSP_ACTION_KIND_CALL) {
        /* Attached soft keyboard: character keys edit the framework
         * buffer and republish the field; only OK (13) reaches the
         * application callback. */
        if (action->event == GSP_INPUT_EVENT_CLICK && action->arg != 13) {
            bool keyboard_action = false;
            bool changed = false;
            esp_gsp_keyboard_lock(gsp);
            if (action->action_id == gsp->kb.action_id &&
                    gsp->kb.action_id != ESP_GSP_KEYBOARD_NONE &&
                    gsp->kb.text != NULL) {
                keyboard_action = true;
                changed = action->arg == 8 ?
                          gsp_text_edit_backspace(gsp->kb.text) :
                          gsp_text_edit_append(gsp->kb.text,
                                               gsp->kb.capacity,
                                               action->arg);
                if (changed) {
                    esp_gsp_cmd_t text_command = {
                        .type = CMD_SET_TEXT,
                        .bind = gsp->kb.text_bind,
                        .flags = ESP_GSP_CMD_FLAG_TEXT_POINTER,
                        .pointer = gsp->kb.text,
                    };
                    (void)esp_gsp_text_apply_command(gsp, &text_command);
                }
            }
            esp_gsp_keyboard_unlock(gsp);
            if (keyboard_action) {
                if (!changed) {
                    GSP_LOGD(TAG,
                             "Keyboard edit rejected: action=%u codepoint=%u",
                             action->action_id, (unsigned)action->arg);
                    return GSP_OK;
                }
                return GSP_OK;
            }
        }
        if (gsp->callbacks.event_cb != NULL ||
                gsp_extension_observed(gsp, ESP_GSP_EXTENSION_ACTION)) {
            esp_gsp_event_t event = {
                .type = ESP_GSP_EVENT_CALL,
                .action_id = action->action_id,
                .arg = action->arg,
                .scene_id = gsp->current_scene,
                .list = ESP_GSP_LIST_NONE,
            };
            if (action->event == GSP_INPUT_EVENT_CANCEL) {
                event.list = gsp->input.press_list;
                event.item = gsp->input.press_item;
            } else {
                hit_row_identity(gsp, gsp->input.tap_instance, &event.list, &event.item);
            }
            const esp_gsp_extension_event_t extension = {
                .struct_size = sizeof(extension),
                .kind = ESP_GSP_EXTENSION_ACTION,
                .data.action = event,
            };
            gsp_extension_emit(gsp, &extension);
            if (gsp->callbacks.event_cb != NULL) {
                gsp->callbacks.event_cb(gsp, &event, gsp->callbacks.event_user_ctx);
            }
        }
        return GSP_OK;
    }
    return GSP_ERR_UNSUPPORTED;
}

static bool drawer_blocks_tap(gsp_ui_core_t *gsp, int32_t x, int32_t y,
                              const gsp_hit_result_t *hit);
static void end_visual_transition(gsp_ui_core_t *gsp, bool commit);
static void cancel_visual_transition(gsp_ui_core_t *gsp);

static void handle_hit_event(gsp_ui_core_t *gsp, gsp_runtime_t *runtime,
                             const gsp_hit_result_t *hit, gsp_input_event_t input_event)
{
    size_t executed = 0;
    gsp_instance_handle_t previous_instance = gsp->input.tap_instance;
    gsp->input.tap_instance = hit->instance;
    size_t capacity = hit->action_count != 0 ? hit->action_count : 1U;
    esp_gsp_update_scratch_t scratch;
    if (esp_gsp_update_scratch_init(&scratch, capacity) != ESP_GSP_OK) {
        GSP_LOGE(TAG, "Input transaction allocation failed: actions=%zu", capacity);
        gsp->input.tap_instance = previous_instance;
        return;
    }
    gsp_update_transaction_t transaction;
    if (gsp_update_begin(runtime, &transaction, scratch.updates, scratch.capacity) != GSP_OK) {
        esp_gsp_update_scratch_deinit(&scratch);
        gsp->input.tap_instance = previous_instance;
        return;
    }
    gsp_err_t ret = gsp_action_dispatch(runtime, hit, input_event, &transaction,
                                        action_handler, gsp, &executed);
    gsp->input.tap_instance = previous_instance;
    if (ret == GSP_OK) {
        ret = gsp_update_commit(&transaction);
        if (ret == GSP_OK) {
            esp_gsp_dropdown_reconcile_selected(gsp);
        }
    }
    if (ret != GSP_OK) {
        gsp_update_cancel(&transaction);
        GSP_LOGE(TAG, "Input dispatch failed: hit=%u event=%u executed=%u result=%d",
                 hit->hit_index, input_event, (unsigned)executed, ret);
    }
    esp_gsp_update_scratch_deinit(&scratch);
}

static void handle_input_event(gsp_ui_core_t *gsp, int32_t x, int32_t y,
                               gsp_input_event_t input_event)
{
    gsp_runtime_t *runtime = gsp_context_runtime(esp_gsp_current_ctx(gsp));
    gsp_hit_result_t hit = {0};
    if (gsp_hit_test_runtime(runtime, x, y, &hit) != GSP_OK || !hit.found ||
            drawer_blocks_tap(gsp, x, y, &hit)) {
        return;
    }
    handle_hit_event(gsp, runtime, &hit, input_event);
}

/* Routes a capture-owner pointer sample to the owning driver inside a
 * one-shot transaction. */
static void handle_tap(gsp_ui_core_t *gsp, int32_t x, int32_t y)
{
    gsp_runtime_t *r = gsp_context_runtime(esp_gsp_current_ctx(gsp));
    if (r->scene->version_minor >= 11) {
        gsp_hit_result_t hit = {0};
        if (gsp_hit_test_runtime(r, x, y, &hit) == GSP_OK && hit.found && hit.z < r->scene->command_count && !drawer_blocks_tap(gsp, x, y, &hit)) {
            const uint8_t *c = r->scene->command_data + (size_t)hit.z * GSP_FORMAT_GSB_COMMAND_SIZE;
            bool enabled = true;
            if (c[0] == GSP_FORMAT_GSB_OPCODE_DRAW_PROCEDURAL && c[26] == GSP_FORMAT_GSB_PROCEDURAL_KIND_FLIP_CARD && gsp_hit_enabled(r, &hit, &enabled) == GSP_OK && enabled) {
                uint16_t value = gsp_read_le16(c + 2);
                gsp_update_transaction_t txn = {0}; gsp_pending_update_t pending;
                gsp_err_t ret = gsp_update_begin(r, &txn, &pending, 1);
                if (ret == GSP_OK) {
                    ret = gsp_update_set_property_i32(&txn, value, r->states[value].value ? 0 : 1);
                }
                if (ret == GSP_OK) {
                    ret = gsp_update_commit(&txn);
                } else if (txn.active) {
                    gsp_update_cancel(&txn);
                }
                if (ret != GSP_OK) {
                    if (txn.active) {
                        gsp_update_cancel(&txn);
                    } GSP_LOGE(TAG, "Flip card update failed: %d", ret);
                    return;
                }
                /* Commit before notifying the application of its new face. */
                handle_input_event(gsp, x, y, GSP_INPUT_EVENT_VALUE);
                return;
            }
        }
    }
    handle_input_event(gsp, x, y, GSP_INPUT_EVENT_CLICK);
}

static bool press_has_event(gsp_ui_core_t *gsp, gsp_input_event_t event)
{
    const gsp_hit_result_t *hit = &gsp->input.press_hit;
    if (!hit->found || gsp->input.press_scene != gsp->current_scene) {
        return false;
    }
    const gsp_scene_view_t *scene = &esp_gsp_current_ctx(gsp)->scene;
    for (uint16_t offset = 0; offset < hit->action_count; ++offset) {
        gsp_action_view_t action;
        if (gsp_action_get(scene, hit->action_first + offset, &action) == GSP_OK && action.event == event) {
            return true;
        }
    }
    return false;
}

static bool press_has_compiled_pressed_state(gsp_ui_core_t *gsp)
{
    const gsp_hit_result_t *hit = &gsp->input.press_hit;
    if (!hit->found || gsp->input.press_scene != gsp->current_scene) {
        return false;
    }
    const gsp_scene_view_t *scene = &esp_gsp_current_ctx(gsp)->scene;
    if (scene->behavior_targets == NULL) {
        return false;
    }
    for (uint16_t offset = 0; offset < hit->action_count; ++offset) {
        uint32_t action_index = hit->action_first + offset;
        uint16_t flags = gsp_read_le16(scene->behavior_targets +
                                       (size_t)action_index *
                                       GSP_FORMAT_GSB_BEHAVIOR_TARGET_SIZE +
                                       GSP_FORMAT_GSB_BEHAVIOR_TARGET_FLAGS_OFFSET);
        if ((flags & GSP_FORMAT_GSB_BEHAVIOR_FLAG_CANCEL_RESET) != 0) {
            return true;
        }
    }
    return false;
}

/* Input callbacks belong to the object that received DOWN, not an object
 * subsequently revealed under the same coordinate by a callback. */
static bool press_hit_matches_at(gsp_ui_core_t *gsp, int32_t x, int32_t y)
{
    if (gsp->input.press_scene != gsp->current_scene || !gsp->input.press_hit.found) {
        return false;
    }
    gsp_runtime_t *runtime = gsp_context_runtime(esp_gsp_current_ctx(gsp));
    gsp_hit_result_t current = {0};
    bool enabled = false;
    return gsp_hit_test_runtime(runtime, x, y, &current) == GSP_OK
           && current.found && current.hit_index == gsp->input.press_hit.hit_index
           && current.instance == gsp->input.press_hit.instance
           && gsp_hit_enabled(runtime, &current, &enabled) == GSP_OK && enabled;
}

static bool press_hit_matches(gsp_ui_core_t *gsp)
{
    const gsp_pointer_t *pointer = &gsp->input.classifier;
    return press_hit_matches_at(gsp, pointer->press_x, pointer->press_y);
}

static void cancel_press(gsp_ui_core_t *gsp);

static bool range_press_enabled(gsp_ui_core_t *gsp)
{
    bool enabled = false;
    return gsp->input.press_scene == gsp->current_scene &&
           gsp_hit_enabled(gsp_context_runtime(esp_gsp_current_ctx(gsp)),
                           &gsp->input.press_hit, &enabled) == GSP_OK && enabled;
}

static void cancel_contact_until_up(gsp_ui_core_t *gsp)
{
    bool held = gsp->input.classifier.pressed;
    gsp_pointer_cancel(&gsp->input.classifier);
    if (held) {
        gsp->input.classifier.pressed = true;
        gsp->input.classifier.owner = GSP_POINTER_OWNER_CONSUMED;
    }
}

void esp_gsp_input_step(gsp_ui_core_t *gsp)
{
    const gsp_pointer_t *pointer = &gsp->input.classifier;
    /* A hidden, disabled or replaced target no longer owns this press.
     * Cancel the classifier as well as the feedback so showing it again
     * cannot resurrect a click from the earlier DOWN. */
    if (pointer->pressed &&
            ((pointer->owner == GSP_POINTER_OWNER_HIT && gsp->input.press_hit.found &&
              !press_hit_matches(gsp)) ||
             (pointer->owner == GSP_POINTER_OWNER_CAPTURE && gsp->input.range_notify &&
              !range_press_enabled(gsp)))) {
        /* Keep consuming this contact until UP; another held sample is not
         * a new DOWN, even if the application restores the target. */
        cancel_contact_until_up(gsp);
        return;
    }
    if (!gsp->input.long_press_pending) {
        return;
    }
    int64_t dx = (int64_t)pointer->last_x - pointer->press_x;
    int64_t dy = (int64_t)pointer->last_y - pointer->press_y;
    if (!pointer->pressed || pointer->tap_cancelled || pointer->owner != GSP_POINTER_OWNER_HIT ||
            dx >= pointer->tap_slop || dx <= -pointer->tap_slop ||
            dy >= pointer->tap_slop || dy <= -pointer->tap_slop || !press_hit_matches(gsp)) {
        gsp->input.long_press_pending = false;
        return;
    }
    if (gsp->input.press_outside ||
            !press_hit_matches_at(gsp, pointer->last_x, pointer->last_y)) {
        gsp->input.long_press_since_us = gsp->now_us;
        return;
    }
    if (gsp->now_us - gsp->input.long_press_since_us >= 500000) {
        gsp->input.long_press_pending = false;
        gsp->input.long_press_fired = true;
        handle_input_event(gsp, pointer->press_x, pointer->press_y, GSP_INPUT_EVENT_LONG);
    }
}

/* Read committed state through the same authored-unit conversion as the
 * component API. Anonymous legacy scenes expose the raw 0..100 rail. */
static bool range_value(gsp_ui_core_t *gsp, uint32_t *value)
{
    gsp_runtime_t *runtime = gsp_context_runtime(esp_gsp_current_ctx(gsp));
    const gsp_component_directory_t *directory = gsp->components.manager.directory;
    if (directory != NULL && gsp->input.range_entry < directory->count) {
        gsp_component_view_t view = {
            .runtime = runtime, .entry = &directory->entries[gsp->input.range_entry],
        };
        gsp_value_t current;
        if (gsp_component_get(&view, GSP_COMPONENT_PROP_VALUE, &current) != GSP_OK ||
                current.type != GSP_VALUE_I32) {
            return false;
        }
        *value = (uint32_t)current.data.i32;
        return true;
    }
    return (gsp->input.range_property != UINT16_MAX
            ? gsp_update_property_i32(runtime, gsp->input.range_property, value)
            : gsp_update_bind_i32(runtime, gsp->input.range_bind, value)) == GSP_OK;
}

static void range_notify(gsp_ui_core_t *gsp, gsp_input_event_t kind)
{
    uint32_t value;
    if (!gsp->input.range_notify || gsp->input.press_scene != gsp->current_scene ||
            !range_value(gsp, &value)) {
        return;
    }
    if (kind == GSP_INPUT_EVENT_VALUE && value == gsp->input.range_last_value) {
        return;
    }
    gsp->input.range_last_value = value;
    gsp_runtime_t *runtime = gsp_context_runtime(esp_gsp_current_ctx(gsp));
    const gsp_hit_result_t hit = gsp->input.press_hit;
    esp_gsp_update_scratch_t scratch;
    gsp_err_t ret = esp_gsp_update_scratch_init(&scratch, hit.action_count);
    if (ret != GSP_OK) {
        GSP_LOGE(TAG, "Range transaction allocation failed: actions=%u", hit.action_count);
        return;
    }
    gsp_update_transaction_t transaction = {0};
    ret = gsp_update_begin(runtime, &transaction, scratch.updates, scratch.capacity);
    if (ret == GSP_OK) {
        gsp->input.tap_instance = hit.instance;
        ret = gsp_action_dispatch_range(runtime, &hit, kind, value, &transaction,
                                        action_handler, gsp);
        gsp->input.tap_instance = GSP_INSTANCE_HANDLE_INVALID;
    }
    if (ret == GSP_OK) {
        ret = gsp_update_commit(&transaction);
    }
    if (ret != GSP_OK) {
        if (transaction.active) {
            gsp_update_cancel(&transaction);
        }
        GSP_LOGE(TAG, "Range dispatch failed: event=%u error=%d", kind, ret);
    }
    esp_gsp_update_scratch_deinit(&scratch);
}

static void capture_event(gsp_ui_core_t *gsp, uint8_t kind,
                          int32_t x, int32_t y)
{
    if (!gsp_component_manager_capture_active(&gsp->components.manager)) {
        gsp->input.range_notify = false;
        cancel_press(gsp);
        return;
    }
    if (gsp->components.manager.capture_generation != gsp->input.range_generation) {
        gsp->input.range_notify = false;
        cancel_press(gsp);
        return;
    }
    if (gsp->input.range_notify && !range_press_enabled(gsp)) {
        kind = GSP_COMPONENT_INPUT_CANCEL;
        gsp->input.range_notify = false;
        cancel_press(gsp);
    }
    gsp_runtime_t *runtime = gsp_context_runtime(esp_gsp_current_ctx(gsp));
    gsp_update_transaction_t transaction;
    gsp_pending_update_t pending[2];
    if (gsp_update_begin(runtime, &transaction, pending, 2) != GSP_OK) {
        return;
    }
    gsp_component_input_t event = { .kind = kind, .x = x, .y = y };
    gsp_err_t ret = gsp_component_manager_capture_event(&gsp->components.manager,
                    &event, &transaction);
    if (ret == GSP_OK) {
        ret = gsp_update_commit(&transaction);
    }
    if (ret != GSP_OK) {
        gsp_update_cancel(&transaction);
        gsp->input.range_notify = false;
        return;
    }
    if (kind == GSP_COMPONENT_INPUT_MOVE) {
        range_notify(gsp, GSP_INPUT_EVENT_VALUE);
    } else if (kind == GSP_COMPONENT_INPUT_UP) {
        range_notify(gsp, GSP_INPUT_EVENT_RELEASE);
    }
    if (kind == GSP_COMPONENT_INPUT_UP || kind == GSP_COMPONENT_INPUT_CANCEL) {
        gsp->input.range_notify = false;
    }
}

/* Press landing: hit test, pressed-scrim setup, value-drag capture.
 * The returned claim tells the gesture classifier how to route the
 * press: CAPTURE owns value drags; HIT still taps on release, but a
 * deliberate drag may escalate to scroll/page swipe. */
static gsp_pointer_claim_t press_feedback_show(gsp_ui_core_t *gsp,
        int32_t x, int32_t y)
{
    gsp_runtime_t *runtime = gsp_context_runtime(esp_gsp_current_ctx(gsp));
    gsp_hit_result_t hit = {0};
    if (gsp_hit_test_runtime(runtime, x, y, &hit) != GSP_OK) {
        return GSP_POINTER_CLAIM_NONE;
    }
    /* Apply the same modal boundary on press landing that handle_tap uses
     * on release.  Otherwise an open Drawer can correctly swallow the
     * eventual click while the page underneath still flashes its pressed
     * scrim for the duration of the touch. */
    if (drawer_blocks_tap(gsp, x, y, &hit)) {
        return GSP_POINTER_CLAIM_HIT;
    }
    if (!hit.found) {
        return GSP_POINTER_CLAIM_NONE;
    }
    bool enabled = true;
    if (gsp_hit_enabled(runtime, &hit, &enabled) != GSP_OK || !enabled) {
        return GSP_POINTER_CLAIM_BLOCK;
    }
    gsp->input.press_hit = hit;
    gsp->input.press_scene = gsp->current_scene;
    if (esp_gsp_carousel_press(gsp, hit.z, x, y)) {
        return GSP_POINTER_CLAIM_CAPTURE;
    }
    /* Interactive target? The portable router owns the decision:
     * CAPTURE starts the drag driver (drag-to-set instead of the
     * pressed scrim), HIT keeps the gesture with the target (toggles:
     * the knob tween IS the feedback, no scrim). */
    static const gsp_component_route_config_t ROUTE_CONFIG = {
        .boolean_driver = ESP_GSP_DRIVER_BOOLEAN,
        .range_driver = ESP_GSP_DRIVER_RANGE,
    };
    gsp->input.range_notify = false;
    for (uint16_t offset = 0; offset < hit.action_count; ++offset) {
        gsp_action_view_t action;
        if (gsp_action_get(runtime->scene, hit.action_first + offset, &action) != GSP_OK ||
                action.kind != GSP_ACTION_KIND_SET_VALUE ||
                action.event != GSP_INPUT_EVENT_VALUE) {
            continue;
        }
        gsp->input.range_bind = action.target_bind;
        gsp->input.range_property = action.target_property;
        gsp->input.range_entry = action.target_property != UINT16_MAX
                                 ? gsp_component_route_resolve_property(&gsp->components.manager, action.target_property)
                                 : gsp_component_route_resolve(&gsp->components.manager, action.target_bind);
        gsp->input.range_notify = range_value(gsp, &gsp->input.range_last_value);
        break;
    }
    gsp_pointer_claim_t claim = gsp_component_route_press(
                                    &gsp->components.manager, &ROUTE_CONFIG,
                                    &esp_gsp_current_ctx(gsp)->scene, &hit, x, y);
    if (claim == GSP_POINTER_CLAIM_CAPTURE) {
        gsp->input.range_generation = gsp->components.manager.capture_generation;
        range_notify(gsp, GSP_INPUT_EVENT_VALUE);
    } else {
        gsp->input.range_notify = false;
    }
    if (claim != GSP_POINTER_CLAIM_NONE) {
        return claim;
    }
    if (press_has_compiled_pressed_state(gsp)) {
        return GSP_POINTER_CLAIM_HIT;
    }
    /* Modal scrims and dropdown catch-alls carry (over-)screen-sized
     * hit boxes purely to swallow taps; painting the pressed shade on
     * them blacks out the whole screen for the press duration. No
     * press visual on anything bigger than half the screen. */
    {
        const gsp_scene_view_t *scene = &esp_gsp_current_ctx(gsp)->scene;
        int64_t hit_area = (int64_t)(hit.bbox.x2 - hit.bbox.x1) *
                           (hit.bbox.y2 - hit.bbox.y1);
        int64_t screen_area = (int64_t)scene->screen_width *
                              scene->screen_height;
        if (hit_area * 2 > screen_area) {
            return GSP_POINTER_CLAIM_HIT;
        }
    }
    /* The hit bbox is already intersected with the instance viewport
     * clip (hit and paint share it), so a row sliding out of its list
     * never spills its pressed shade. */
    if (gsp->chrome.press_feedback_disabled) {
        return GSP_POINTER_CLAIM_HIT;
    }
    gsp->chrome.scrim_rect = hit.bbox;
    gsp->chrome.press_feedback = true;
    esp_gsp_overlay_rebuild(gsp);
    return GSP_POINTER_CLAIM_HIT;
}

static void press_feedback_hide(gsp_ui_core_t *gsp)
{
    if (gsp->chrome.press_feedback) {
        gsp->chrome.press_feedback = false;
        esp_gsp_overlay_rebuild(gsp);
    }
}

/* Clear only compiler-owned CANCEL_RESET actions, so gesture cancellation,
 * release outside the button and scene switches cannot leave pressed artwork
 * active. Authored release callbacks and state changes are never dispatched. */
static void button_pressed_state_clear(gsp_ui_core_t *gsp)
{
    const gsp_hit_result_t *hit = &gsp->input.press_hit;
    uint16_t scene_index = gsp->input.press_scene;
    if (!hit->found || scene_index >= gsp->scene_count ||
            !gsp->scene_ready[scene_index]) {
        return;
    }
    gsp_runtime_t *runtime =
        gsp_context_runtime(&gsp->scenes[scene_index]);
    for (uint16_t offset = 0; offset < hit->action_count; ++offset) {
        gsp_action_view_t action;
        uint32_t action_index = hit->action_first + offset;
        const gsp_scene_view_t *scene = runtime->scene;
        uint16_t behavior_flags = scene->behavior_targets == NULL ? 0 :
                                  gsp_read_le16(scene->behavior_targets +
                                                (size_t)action_index *
                                                GSP_FORMAT_GSB_BEHAVIOR_TARGET_SIZE +
                                                GSP_FORMAT_GSB_BEHAVIOR_TARGET_FLAGS_OFFSET);
        if (gsp_action_get(scene, action_index, &action) != GSP_OK ||
                (behavior_flags &
                 GSP_FORMAT_GSB_BEHAVIOR_FLAG_CANCEL_RESET) == 0 ||
                action.event != GSP_INPUT_EVENT_RELEASE ||
                action.kind != GSP_ACTION_KIND_SET_VALUE || action.arg != 0) {
            continue;
        }
        gsp_pending_update_t pending;
        gsp_update_transaction_t transaction = {0};
        gsp_err_t ret = gsp_update_begin(runtime, &transaction, &pending, 1);
        if (ret == GSP_OK) {
            ret = action.target_property != UINT16_MAX
                  ? gsp_update_set_property_i32(
                      &transaction, action.target_property, 0)
                  : gsp_update_set_i32(
                      &transaction, action.target_bind, 0);
        }
        if (ret == GSP_OK) {
            ret = gsp_update_commit(&transaction);
        } else if (transaction.active) {
            gsp_update_cancel(&transaction);
        }
        if (ret != GSP_OK) {
            GSP_LOGE(TAG, "Button pressed-state clear failed: %d", ret);
        }
        return;
    }
}

static void cancel_press(gsp_ui_core_t *gsp)
{
    if (!gsp->input.press_hit.found || gsp->input.press_finished) {
        return;
    }
    bool notify = press_has_event(gsp, GSP_INPUT_EVENT_CANCEL);
    gsp_hit_result_t hit = gsp->input.press_hit;
    gsp->input.press_finished = true;
    gsp->input.press_cancelled = true;
    gsp->input.long_press_pending = false;
    press_feedback_hide(gsp);
    button_pressed_state_clear(gsp);
    if (notify) {
        handle_hit_event(gsp, gsp_context_runtime(esp_gsp_current_ctx(gsp)), &hit,
                         GSP_INPUT_EVENT_CANCEL);
    }
}

/* Components and navigation cancel their own work during a scene switch.
 * Retain the physical contact latch so HELD cannot become a new DOWN. */
void esp_gsp_input_scene_exit(gsp_ui_core_t *gsp)
{
    cancel_press(gsp);
    button_pressed_state_clear(gsp);
    press_feedback_hide(gsp);
    gsp->input.long_press_pending = false;
    gsp->input.press_hit.found = false;
    if (gsp->input.classifier.pressed) {
        gsp->input.classifier.owner = GSP_POINTER_OWNER_CONSUMED;
    }
}

/* Gesture-classifier effect callbacks: the classifier decides who owns
 * the press; these apply the effects on the UI state. */

static gsp_pointer_claim_t pointer_op_press(void *ctx, int32_t x,
        int32_t y)
{
    gsp_ui_core_t *gsp = ctx;
    gsp->input.press_time_us = gsp->now_us;
    gsp->input.long_press_since_us = gsp->now_us;
    gsp->input.press_scene = gsp->current_scene;
    gsp->input.long_press_pending = false;
    gsp->input.long_press_fired = false;
    gsp->input.press_cancelled = false;
    gsp->input.press_finished = false;
    gsp->input.press_outside = false;
    gsp->input.press_hit.found = false;
    gsp->input.press_list = ESP_GSP_LIST_NONE;
    gsp->input.press_item = 0;
    /* A snapshot session owns the panel and makes the frame loop yield;
     * leaving one behind delays this press's own feedback by a tick. */
    esp_gsp_list_snapshot_cancel(gsp);
    for (size_t index = 0; index < gsp_effective_capacity(gsp, ESP_GSP_FIELD_MAX_LISTS); ++index) {
        gsp_list_state_t *state =
            esp_gsp_list_state(gsp, &gsp->lists.pool[index]);
        if (state != NULL) {
            gsp_scroll_phys_press(&state->phys, gsp->now_us);
        }
    }
    gsp_pointer_claim_t claim = press_feedback_show(gsp, x, y);
    hit_row_identity(gsp, gsp->input.press_hit.instance,
                     &gsp->input.press_list, &gsp->input.press_item);
    if (claim == GSP_POINTER_CLAIM_HIT) {
        gsp->input.long_press_pending = press_has_event(gsp, GSP_INPUT_EVENT_LONG);
    }
    if ((claim == GSP_POINTER_CLAIM_HIT || claim == GSP_POINTER_CLAIM_CAPTURE) &&
            press_has_event(gsp, GSP_INPUT_EVENT_PRESS)) {
        handle_input_event(gsp, x, y, GSP_INPUT_EVENT_PRESS);
    }
    return gsp->input.press_finished ? GSP_POINTER_CLAIM_BLOCK : claim;
}

static void pointer_op_capture_move(void *ctx, int32_t x, int32_t y)
{
    if (!esp_gsp_carousel_move(ctx, x)) {
        capture_event(ctx, GSP_COMPONENT_INPUT_MOVE, x, y);
    }
}

static void pointer_op_capture_release(void *ctx)
{
    gsp_ui_core_t *gsp = ctx;
    int32_t x = gsp->carousel.press_x, y = gsp->carousel.press_y;
    bool moved = gsp->carousel.moved;
    if (esp_gsp_carousel_release(gsp, false)) {
        handle_input_event(gsp, x, y, GSP_INPUT_EVENT_VALUE);
        if (!moved) {
            handle_tap(gsp, x, y);
        }
    } else {
        capture_event(ctx, GSP_COMPONENT_INPUT_UP, 0, 0);
    }
}

static bool point_in_rect(int32_t x, int32_t y, int16_t rx, int16_t ry,
                          uint16_t width, uint16_t height);

/* Band along the attached edge where a closed drawer accepts its pull.
 * Shared with gesture_begin so arbitration and eligibility cannot drift. */
#define GSP_POLICY(gsp_, field_) ((uint32_t)esp_gsp_config_value( \
    &(gsp_)->effective_config, (field_)))
#define DRAWER_EDGE_SLOP(gsp_) \
    GSP_POLICY((gsp_), ESP_GSP_FIELD_DRAWER_EDGE_SLOP_PX)
#define GESTURE_TERMINAL_ZONE_NUMERATOR 3U
#define GESTURE_TERMINAL_ZONE_DENOMINATOR 4U

/*
 * Distance still provides stable intent for slow drags, but a deliberate
 * release-time reversal should cancel until the destination is nearly
 * reached. This mirrors phone pagers: recent velocity wins in the ambiguous
 * middle, while the last quarter remains a terminal hysteresis zone.
 */
static bool gesture_release_reverses(
    const gsp_ui_core_t *gsp, uint32_t progress, uint32_t extent,
    int32_t velocity, bool velocity_valid, int32_t directed_flick)
{
    if (extent == 0U ||
            (uint64_t)progress * GESTURE_TERMINAL_ZONE_DENOMINATOR >=
            (uint64_t)extent * GESTURE_TERMINAL_ZONE_NUMERATOR) {
        return false;
    }
    int32_t flick_threshold =
        (int32_t)GSP_POLICY(gsp, ESP_GSP_FIELD_SWIPE_FLICK_PX);
    /*
     * gsp_swipe_kinetics_velocity() only exposes a negative tail after two
     * consecutive reverse reports. At that point direction itself is the
     * intent; reusing the forward-fling speed threshold would make a slow,
     * controlled reversal lose on large screens.
     */
    bool timed_reverse = velocity_valid && velocity < 0;
    bool fallback_reverse = !velocity_valid && directed_flick < 0 &&
                            directed_flick <= -flick_threshold;
    return timed_reverse || fallback_reverse;
}

static int32_t gesture_settle_velocity(
    const gsp_ui_core_t *gsp, int32_t measured_velocity,
    int32_t directed_flick, uint32_t extent)
{
    if (directed_flick <= 0) {
        return measured_velocity;
    }
    uint32_t sample_window_ms =
        GSP_POLICY(gsp, ESP_GSP_FIELD_ACTIVE_TICK_MS) * 3U;
    int64_t inferred = (int64_t)directed_flick * 1000 /
                       sample_window_ms;
    int64_t maximum = GSP_POLICY(
                          gsp, ESP_GSP_FIELD_SCROLL_MAX_RELEASE_SPEED_PX_S);
    int64_t normalized_maximum = (int64_t)extent * 3 / 2;
    if (normalized_maximum > 0 && normalized_maximum < maximum) {
        maximum = normalized_maximum;
    }
    if (inferred > maximum) {
        inferred = maximum;
    }
    return inferred > measured_velocity ? (int32_t)inferred :
           measured_velocity;
}

static bool drawer_edge_band_contains(gsp_ui_core_t *gsp,
                                      const gsp_drawer_config_t *config,
                                      int32_t x, int32_t y)
{
    switch (config->edge) {
    case GSP_DRAWER_EDGE_TOP:
        return y <= config->y + DRAWER_EDGE_SLOP(gsp);
    case GSP_DRAWER_EDGE_BOTTOM:
        return y >= config->y + config->height - DRAWER_EDGE_SLOP(gsp);
    case GSP_DRAWER_EDGE_LEFT:
        return x <= config->x + DRAWER_EDGE_SLOP(gsp);
    default:
        return x >= config->x + config->width - DRAWER_EDGE_SLOP(gsp);
    }
}

/* A visible open/settling drawer owns its whole viewport. A closed enabled
 * one owns only the edge band its pull starts in, which matters for scroll
 * arbitration but not modal blocking — hence @p include_closed_edge. */
static bool drawer_hit(gsp_ui_core_t *gsp, int32_t x, int32_t y,
                       bool include_closed_edge)
{
    const gsp_component_directory_t *directory =
        gsp->components.manager.directory;
    if (directory == NULL) {
        return false;
    }
    for (uint16_t entry = 0; entry < directory->count; ++entry) {
        const gsp_component_entry_t *descriptor =
            &directory->entries[entry];
        if (descriptor->kind != GSP_COMPONENT_KIND_DRAWER ||
                descriptor->config == NULL) {
            continue;
        }
        const gsp_drawer_config_t *config = descriptor->config;
        bool in_viewport = point_in_rect(x, y, config->x, config->y,
                                         config->width, config->height);
        bool in_edge_band = include_closed_edge &&
                            drawer_edge_band_contains(gsp, config, x, y);
        if (!in_viewport && !in_edge_band) {
            continue;
        }
        if (transform_group_hidden(gsp->components.manager.runtime,
                                   config->group_id)) {
            continue;
        }
        gsp_component_instance_t *instance;
        if (gsp_component_manager_activate(
                    &gsp->components.manager, entry, ESP_GSP_DRIVER_DRAWER,
                    &instance) != GSP_OK) {
            continue;
        }
        /* Disabling stops new edge pulls, not the modal protection of a
         * visible panel that is still open or settling closed. */
        bool modal = gsp_drawer_driver_is_open(instance) ||
                     gsp_drawer_driver_animating(instance);
        if (modal ? in_viewport :
                (in_edge_band && motion_component_enabled(gsp, descriptor))) {
            return true;
        }
    }
    return false;
}

static bool open_drawer_covers_point(gsp_ui_core_t *gsp,
                                     int32_t x, int32_t y)
{
    return drawer_hit(gsp, x, y, false);
}

/* An open drawer paints over everything below it, so a tap that lands on
 * the drawer must never reach the page behind it — including the drawer's
 * own blank areas, which carry no interactive target of their own. */
static bool drawer_blocks_tap(gsp_ui_core_t *gsp, int32_t x, int32_t y,
                              const gsp_hit_result_t *hit)
{
    const gsp_component_directory_t *directory =
        gsp->components.manager.directory;
    if (directory == NULL) {
        return false;
    }
    for (uint16_t entry = 0; entry < directory->count; ++entry) {
        const gsp_component_entry_t *descriptor = &directory->entries[entry];
        if (descriptor->kind != GSP_COMPONENT_KIND_DRAWER ||
                descriptor->config == NULL) {
            continue;
        }
        const gsp_drawer_config_t *config = descriptor->config;
        if (!point_in_rect(x, y, config->x, config->y,
                           config->width, config->height)) {
            continue;
        }
        if (transform_group_hidden(gsp->components.manager.runtime,
                                   config->group_id)) {
            continue;
        }
        gsp_component_instance_t *instance;
        if (gsp_component_manager_activate(
                    &gsp->components.manager, entry, ESP_GSP_DRIVER_DRAWER,
                    &instance) != GSP_OK ||
                (!gsp_drawer_driver_is_open(instance) &&
                 !gsp_drawer_driver_animating(instance))) {
            continue;
        }
        if (!hit->found) {
            return true;
        }
        const gsp_runtime_t *runtime = gsp->components.manager.runtime;
        bool in_drawer = false;
        if (hit->instance == GSP_INSTANCE_HANDLE_INVALID) {
            in_drawer = gsp_transform_hit_in_group(
                            runtime->scene, hit->hit_index, config->group_id);
        } else if (runtime->instance_pool != NULL) {
            const gsp_instance_pool_t *pool = runtime->instance_pool;
            uint16_t index = (uint16_t)hit->instance;
            if (index < pool->slot_capacity) {
                const gsp_instance_slot_t *slot = &pool->slots[index];
                in_drawer = (slot->flags & GSP_INSTANCE_FLAG_ACTIVE) != 0 &&
                            slot->generation == (uint16_t)(hit->instance >> 16) &&
                            slot->transform_group != GSP_INSTANCE_TRANSFORM_NONE &&
                            gsp_transform_group_in_group(runtime->scene,
                                    slot->transform_group, config->group_id);
            }
        }
        if (!in_drawer) {
            return true;
        }
    }
    return false;
}

static bool drawer_claims_point(gsp_ui_core_t *gsp, int32_t x, int32_t y)
{
    return drawer_hit(gsp, x, y, true);
}

static bool pointer_op_scroll_hit(void *ctx, int32_t press_x,
                                  int32_t press_y)
{
    gsp_ui_core_t *gsp = ctx;
    /* The drawer's own gesture is resolved by gesture_begin; lists and
     * scroll regions below it must not win the earlier arbitration. */
    if (drawer_claims_point(gsp, press_x, press_y)) {
        return false;
    }
    bool in_region = gsp->input.scroll_cb != NULL &&
                     press_x >= gsp->input.scroll_region.x1 &&
                     press_x < gsp->input.scroll_region.x2 &&
                     press_y >= gsp->input.scroll_region.y1 &&
                     press_y < gsp->input.scroll_region.y2;
    int32_t press_list = -1;
    for (size_t index = 0; index < gsp_effective_capacity(gsp, ESP_GSP_FIELD_MAX_LISTS); ++index) {
        esp_gsp_list_slot_t *list = &gsp->lists.pool[index];
        /* Only ACTIVE viewports take drags: parked lists (hidden
         * page) resolve no state, and total 0 keeps the legacy
         * app-side parking working. */
        if (list->used && list->scene == gsp->current_scene &&
                list->total != 0 &&
                esp_gsp_list_state(gsp, list) != NULL) {
            gsp_rect_t region;
            if (esp_gsp_list_visible_region(gsp, list, &region) &&
                    press_x >= region.x1 && press_x < region.x2 &&
                    press_y >= region.y1 && press_y < region.y2) {
                press_list = (int32_t)index;
                break;
            }
        }
    }
    if (press_list >= 0) {
        in_region = true;
    }
    if (in_region) {
        gsp->lists.active = press_list;
    }
    return in_region;
}

static void pointer_op_scroll_begin(void *ctx, int32_t consumed_delta_y)
{
    gsp_ui_core_t *gsp = ctx;
    cancel_press(gsp);
    gsp->lists.snapshot_blocked = false;
    if (gsp->lists.active >= 0) {
        gsp_list_state_t *state = esp_gsp_list_state(
                                      gsp, &gsp->lists.pool[gsp->lists.active]);
        if (state != NULL) {
            gsp_scroll_phys_claim(&state->phys, consumed_delta_y);
        }
    }
    press_feedback_hide(gsp);
}

static void pointer_op_scroll_move(void *ctx, int32_t delta_y)
{
    gsp_ui_core_t *gsp = ctx;
    if (gsp->lists.active >= 0) {
        gsp_list_state_t *state = esp_gsp_list_state(
                                      gsp, &gsp->lists.pool[gsp->lists.active]);
        if (state != NULL) {
            int32_t old_offset = state->phys.offset;
            gsp_scroll_phys_drag(&state->phys, delta_y, gsp->now_us);
            bool snapshot_list =
                gsp->lists.pool[gsp->lists.active].scroll_snapshot;
            if (gsp->lists.snapshot != gsp->lists.active && !snapshot_list) {
                (void)gsp_repeater_set_scroll(&state->repeater,
                                              state->phys.offset);
            }
            esp_gsp_list_snapshot_move(
                gsp, (uint16_t)gsp->lists.active, old_offset,
                state->phys.offset);
            if (snapshot_list && !gsp->lists.snapshot_hold_repeater &&
                    state->repeater.scroll_offset != state->phys.offset) {
                (void)gsp_repeater_set_scroll(&state->repeater,
                                              state->phys.offset);
            }
            esp_gsp_overlay_rebuild(gsp);
        }
    } else if (gsp->input.scroll_cb != NULL) {
        gsp->input.scroll_cb(gsp, delta_y, gsp->input.scroll_user_ctx);
    }
}

static void pointer_op_scroll_release(void *ctx)
{
    gsp_ui_core_t *gsp = ctx;
    if (gsp->lists.active >= 0) {
        gsp_list_state_t *state = esp_gsp_list_state(
                                      gsp, &gsp->lists.pool[gsp->lists.active]);
        if (state != NULL) {
            /* Coasting/snap easing runs on the persistent list
             * instance's tick from here (busy() holds the pacing). */
            gsp_scroll_phys_release(&state->phys, gsp->now_us);
        }
    }
    gsp->lists.active = -1;
    esp_gsp_overlay_rebuild(gsp);   /* drop the scroll indicator */
}

static gsp_transition_kind_t swipe_kind(esp_gsp_swipe_transition_t style,
                                        bool towards_next)
{
    switch (style) {
    case ESP_GSP_SWIPE_CROSS_FADE:
        return GSP_TRANSITION_FADE;
    case ESP_GSP_SWIPE_SLIDE_FADE_THROUGH_BLACK:
        return towards_next
               ? GSP_TRANSITION_SLIDE_LEFT_FADE_THROUGH_BLACK
               : GSP_TRANSITION_SLIDE_RIGHT_FADE_THROUGH_BLACK;
    default:
        return towards_next ? GSP_TRANSITION_SLIDE_LEFT
               : GSP_TRANSITION_SLIDE_RIGHT;
    }
}

static bool pointer_op_swipe_begin(void *ctx, bool towards_next)
{
    gsp_ui_core_t *gsp = ctx;
    if (gsp->config.disable_swipe || gsp->scene_count <= 1) {
        return false;
    }
    gsp_runtime_t *runtime =
        gsp_context_runtime(esp_gsp_current_ctx(gsp));
    if (runtime == NULL || runtime->scene == NULL ||
            (runtime->scene->flags &
             GSP_FORMAT_GSB_SCENE_FLAG_SWIPE_DISABLED) != 0 ||
            gsp_runtime_scene_swipe_blocked(runtime)) {
        return false;
    }
    uint16_t target = towards_next ?
                      (uint16_t)((gsp->current_scene + 1U) % gsp->scene_count) :
                      (uint16_t)((gsp->current_scene + gsp->scene_count - 1U) %
                                 gsp->scene_count);
    if (!gsp->scene_ready[target]) {
        return false;
    }
    uint16_t source_scene = gsp->current_scene;
    cancel_press(gsp);
    if (gsp->current_scene != source_scene || gsp->shutting_down) {
        return true; /* Keep consuming the old contact after navigation. */
    }
    gsp_ui_core_prepare_scene(gsp, target);
    /* A completed component drag may still be settling its frozen viewport.
     * A new scene gesture supersedes that session at the controller layer as
     * well as at the display, so its observer cannot later drive this swipe. */
    cancel_visual_transition(gsp);
    GSP_LOGI(TAG,
             "scene swipe begin: current=%u target=%u direction=%s "
             "press=(%ld,%ld)",
             gsp->current_scene, target,
             towards_next ? "next" : "previous",
             (long)gsp->input.classifier.press_x,
             (long)gsp->input.classifier.press_y);
    gsp->nav.drag_target = target;
    gsp->nav.drag_progress = 0;
    gsp->nav.drag_presented_progress = 0;
    gsp->nav.settle_commit = false;
    gsp_swipe_kinetics_reset(&gsp->nav.swipe_kinetics, 0,
                             gsp->input.press_time_us);
    gsp->nav.drag_kind = swipe_kind(gsp->nav.swipe_transition, towards_next);
    gsp_visual_transition_request_t request = {
        .source = GSP_VISUAL_SOURCE_SCENE,
        .kind = gsp->nav.drag_kind,
        .scene = {
            .target_scene = target,
            .fade_black_point_percent =
            gsp->nav.swipe_fade_black_point_percent,
            .interactive = true,
        },
    };
    gsp->nav.adapter_transition =
        gsp->adapter_ops != NULL &&
        gsp->adapter_ops->visual_transition_begin != NULL &&
        gsp->adapter_ops->visual_transition_begin(
            gsp->adapter_ctx, gsp, &request);
    gsp->nav.drag_fallback = !gsp->nav.adapter_transition;
    return true;
}

static bool point_in_rect(int32_t x, int32_t y, int16_t rx, int16_t ry,
                          uint16_t width, uint16_t height)
{
    return x >= rx && y >= ry &&
           x < (int32_t)rx + width && y < (int32_t)ry + height;
}

static bool page_flow_viewport(
    const gsp_ui_core_t *gsp, const gsp_page_flow_config_t *config,
    gsp_rect_t *out)
{
    if (gsp == NULL || config == NULL || out == NULL ||
            gsp->components.manager.runtime == NULL) {
        return false;
    }
    const gsp_runtime_t *runtime = gsp->components.manager.runtime;
    gsp_transform_resolved_t resolved;
    if (gsp_transform_resolve(
                runtime->scene, runtime->states, runtime->state_capacity,
                config->group_id, &resolved) != GSP_OK ||
            !resolved.has_viewport ||
            resolved.viewport.x2 <= resolved.viewport.x1 ||
            resolved.viewport.y2 <= resolved.viewport.y1) {
        return false;
    }
    *out = resolved.viewport;
    return true;
}

static int32_t page_flow_step(const gsp_page_flow_config_t *config)
{
    return config->page_extent != 0 ? config->page_extent :
           (config->axis == GSP_GESTURE_AXIS_VERTICAL
            ? config->height : config->width);
}

/*
 * One adjacent compositor endpoint.  The frozen slide only knows two
 * neighbouring captures; a two-step window would drop the current page.
 */
static void page_flow_stop_anywhere_window(
    const gsp_page_flow_config_t *config, int32_t start_offset,
    bool towards_next, int32_t *from_offset, int32_t *target_offset,
    esp_gsp_transition_t *kind)
{
    int32_t step = page_flow_step(config);
    bool axis_y = config->axis == GSP_GESTURE_AXIS_VERTICAL;
    int32_t minimum = -(int32_t)(config->page_count - 1U) * step;
    int32_t elastic = step / PAGE_FLOW_EDGE_OVERSCROLL_DIVISOR;
    *from_offset = start_offset;
    if (towards_next) {
        *target_offset = start_offset - step;
        if (*target_offset < minimum - elastic) {
            *target_offset = minimum - elastic;
        }
        *kind = axis_y ? ESP_GSP_SLIDE_UP : ESP_GSP_SLIDE_LEFT;
    } else {
        *target_offset = start_offset + step;
        if (*target_offset > elastic) {
            *target_offset = elastic;
        }
        *kind = axis_y ? ESP_GSP_SLIDE_DOWN : ESP_GSP_SLIDE_RIGHT;
    }
}

static bool component_page_visible(gsp_ui_core_t *gsp,
                                   uint16_t vis_slot_ref)
{
    if (vis_slot_ref == 0) {
        return true;
    }
    uint16_t vis_slot = vis_slot_ref - 1U;
    gsp_runtime_t *runtime =
        gsp_context_runtime(esp_gsp_current_ctx(gsp));
    if (runtime == NULL || vis_slot >= runtime->state_capacity) {
        return true;            /* malformed gate: fail open */
    }
    return (runtime->states[vis_slot].flags &
            GSP_FORMAT_GSB_STATE_FLAG_VISIBLE) != 0;
}

static bool transform_group_hidden(const gsp_runtime_t *runtime,
                                   uint16_t group_id)
{
    if (runtime == NULL || runtime->scene == NULL ||
            group_id >= runtime->scene->transform_group_count) {
        return true;
    }
    for (uint32_t command = 0;
            command < runtime->scene->command_count; ++command) {
        if (gsp_transform_command_in_group(
                    runtime->scene, command, group_id) &&
                !gsp_runtime_command_hidden(runtime, command)) {
            return false;
        }
    }
    return true;
}

static void gesture_set(gsp_ui_core_t *gsp,
                        gsp_component_instance_t *instance,
                        uint8_t kind, uint16_t group_id)
{
    gsp->input.gesture = instance;
    gsp->input.gesture_generation = instance->generation;
    gsp->input.gesture_kind = kind;
    gsp->input.gesture_group = group_id;
    gsp->input.gesture_visibility_revision =
        gsp->components.manager.runtime->visibility_revision;
}

void esp_gsp_input_reconcile(gsp_ui_core_t *gsp)
{
    gsp_runtime_t *runtime = gsp->components.manager.runtime;
    if (gsp->input.gesture == NULL || runtime == NULL) {
        return;
    }
    const gsp_component_entry_t *entry =
        &gsp->components.manager.directory->entries[
            gsp->input.gesture->entry_index];
    if (!motion_component_enabled(gsp, entry)) {
        cancel_contact_until_up(gsp);
        return;
    }
    if (gsp->input.gesture_visibility_revision == runtime->visibility_revision) {
        return;
    }
    gsp->input.gesture_visibility_revision = runtime->visibility_revision;
    if (!transform_group_hidden(runtime, gsp->input.gesture_group)) {
        return;
    }
    cancel_contact_until_up(gsp);
}

static bool begin_viewport_transition(
    gsp_ui_core_t *gsp, uint16_t group_id, gsp_rect_t viewport,
    int32_t from_offset, int32_t target_offset, bool vertical,
    bool overlay, gsp_transition_kind_t kind)
{
    ++gsp->chrome.drag_snapshot_stats.attempts;
    gsp->chrome.drag_snapshot_stats.last_reason = ESP_GSP_SNAPSHOT_NONE;
    gsp->chrome.drag_snapshot_stats.last_backend_error = 0;
    if (gsp->adapter_ops == NULL ||
            gsp->adapter_ops->visual_transition_begin == NULL) {
        esp_gsp_snapshot_skip(gsp, ESP_GSP_SNAPSHOT_UNSUPPORTED);
        return false;
    }
    if (gsp->input.adapter_visual_transition_retry_after_us > gsp->now_us) {
        esp_gsp_snapshot_skip(gsp, ESP_GSP_SNAPSHOT_RETRY_BLOCKED);
        return false;
    }
    int32_t first_capture = overlay && from_offset == 0 ?
                            target_offset : from_offset;
    int32_t second_capture = overlay ? 0 : target_offset;
    esp_gsp_lists_activate_group(gsp, group_id, viewport,
                                 first_capture, second_capture, vertical);
    gsp_visual_transition_request_t request = {
        .source = GSP_VISUAL_SOURCE_VIEWPORT_GROUP,
        .kind = kind,
        .viewport = {
            .group_id = group_id,
            .viewport = viewport,
            .from_offset = from_offset,
            .target_offset = target_offset,
            .vertical = vertical,
            .overlay = overlay,
        },
    };
    if (gsp->adapter_ops->visual_transition_begin(
                gsp->adapter_ctx, gsp, &request)) {
        gsp->input.adapter_visual_transition_retry_after_us = 0;
        gsp->input.adapter_visual_transition_retry_delay_us = 0;
        ++gsp->chrome.drag_snapshot_stats.started;
        return true;
    }
    esp_gsp_snapshot_skip(gsp, gsp->chrome.drag_snapshot_stats.last_reason != ESP_GSP_SNAPSHOT_NONE ?
                          gsp->chrome.drag_snapshot_stats.last_reason : ESP_GSP_SNAPSHOT_BACKEND_ERROR);
    if (gsp->chrome.drag_snapshot_stats.last_reason == ESP_GSP_SNAPSHOT_UNSUPPORTED) {
        /* An intentionally unsupported layout needs live composition;
         * retry throttling is reserved for transient backend failures. */
        gsp->input.adapter_visual_transition_retry_after_us = 0;
        gsp->input.adapter_visual_transition_retry_delay_us = 0;
        return false;
    }
    uint32_t delay_us =
        gsp->input.adapter_visual_transition_retry_delay_us;
    if (delay_us < VIEWPORT_SNAPSHOT_RETRY_INITIAL_US) {
        delay_us = VIEWPORT_SNAPSHOT_RETRY_INITIAL_US;
    }
    gsp->input.adapter_visual_transition_retry_after_us =
        gsp->now_us > INT64_MAX - delay_us
        ? INT64_MAX : gsp->now_us + delay_us;
    gsp->input.adapter_visual_transition_retry_delay_us =
        delay_us >= VIEWPORT_SNAPSHOT_RETRY_MAX_US / 2U
        ? VIEWPORT_SNAPSHOT_RETRY_MAX_US : delay_us * 2U;
    GSP_LOGW(TAG, "Viewport snapshot unavailable for group=%u; safe fallback"
             " enabled and retries paused for %u ms", group_id,
             (unsigned)(delay_us / 1000U));
    return false;
}

/*
 * Whether a jump may animate without a frozen snapshot. Live transform
 * scrolling drags every page between the two anchors across the viewport, so
 * a multi-page jump smears the pages in between. Landing instantly loses the
 * animation but never shows a page the navigation did not ask for.
 */
static bool snapshotless_jump_is_safe(int32_t from, int32_t target,
                                      int32_t extent)
{
    int32_t travel = target > from ? target - from : from - target;
    return extent <= 0 || travel <= extent;
}

static void end_visual_transition(gsp_ui_core_t *gsp, bool commit)
{
    gsp->input.adapter_visual_transition = false;
    if (gsp->adapter_ops == NULL) {
        return;
    }
    int32_t terminal = 0;
    if (commit) {
        terminal = gsp->input.adapter_visual_transition_target -
                   gsp->input.adapter_visual_transition_from;
        if (terminal < 0) {
            terminal = -terminal;
        }
    }
    if (gsp->adapter_ops->visual_transition_progress != NULL &&
            (uint16_t)terminal !=
            gsp->input.adapter_visual_transition_last_progress) {
        gsp->adapter_ops->visual_transition_progress(
            gsp->adapter_ctx, (uint16_t)terminal);
        gsp->input.adapter_visual_transition_last_progress =
            (uint16_t)terminal;
    }
    if (gsp->adapter_ops->visual_transition_end != NULL) {
        gsp->adapter_ops->visual_transition_end(gsp->adapter_ctx, commit);
    }
}

/* Stop-anywhere leaves the live transform at its final offset. End without
 * driving the compositor back to an anchor; the backend retains only verified
 * presented damage and otherwise invalidates the live page. */
static void end_visual_transition_at_current(gsp_ui_core_t *gsp)
{
    gsp->input.adapter_visual_transition = false;
    if (gsp->adapter_ops != NULL &&
            gsp->adapter_ops->visual_transition_end != NULL) {
        gsp->adapter_ops->visual_transition_end(gsp->adapter_ctx, false);
    }
}

static gsp_component_instance_t *visual_transition_owner(gsp_ui_core_t *gsp)
{
    for (uint16_t i = 0; i < gsp->components.manager.capacity; ++i) {
        gsp_component_instance_t *instance = &gsp->components.manager.instances[i];
        if ((instance->flags & 1U) != 0 &&
                instance->entry_index == gsp->input.adapter_visual_transition_entry &&
                instance->generation == gsp->input.adapter_visual_transition_generation) {
            return instance;
        }
    }
    return NULL;
}

/* Without captured endpoints, a multi-page settle would expose pages that
 * navigation skipped. Land at the committed target before resuming live paint. */
static bool finish_snapshotless_jump(gsp_ui_core_t *gsp)
{
    gsp_component_instance_t *instance = visual_transition_owner(gsp);
    if (instance == NULL || instance->extension == NULL) {
        return false;
    }
    const gsp_component_entry_t *entry =
        &gsp->components.manager.directory->entries[instance->entry_index];
    if (entry->config == NULL) {
        return false;
    }
    int32_t extent;
    if (entry->kind == GSP_COMPONENT_KIND_STACK_VIEW) {
        const gsp_stack_view_config_t *config = entry->config;
        extent = config->axis == GSP_GESTURE_AXIS_HORIZONTAL ? config->width : config->height;
    } else if (entry->kind == GSP_COMPONENT_KIND_PAGE_FLOW) {
        extent = page_flow_step(entry->config);
    } else {
        return false;
    }
    if (snapshotless_jump_is_safe(gsp->input.adapter_visual_transition_from,
                                  gsp->input.adapter_visual_transition_target, extent)) {
        return false;
    }
    end_visual_transition_at_current(gsp);
    if (entry->kind == GSP_COMPONENT_KIND_STACK_VIEW) {
        (void)gsp_stack_view_driver_finish(&gsp->components.manager, instance);
    } else {
        gsp_page_flow_state_t *state = instance->extension;
        (void)gsp_page_flow_driver_set_page(&gsp->components.manager, instance,
                                            state->target, false, (uint64_t)gsp->now_us);
    }
    return true;
}

/* Publications invalidate frozen pixels; adjacent motion can resume at its
 * live offset, while jumps must finish without revealing intermediate pages. */
void esp_gsp_snapshot_content_changed_active(gsp_ui_core_t *gsp,
        uint16_t scene)
{
    if (scene != gsp->current_scene) {
        return;
    }
    if (gsp->input.adapter_visual_transition) {
        /* Captures include fixed paint as well as the moving owner. A
         * publication can change their occlusion even in another group. */
        if (!finish_snapshotless_jump(gsp)) {
            end_visual_transition_at_current(gsp);
        }
    }
    gsp->lists.snapshot_cache_list = -1;
    if (gsp->lists.snapshot >= 0) {
        esp_gsp_list_snapshot_cancel(gsp);
    }
}

void esp_gsp_snapshot_backend_invalidated(esp_gsp_handle_t gsp, int32_t error)
{
    if (gsp != NULL && gsp->input.adapter_visual_transition) {
        esp_gsp_snapshot_backend_result(gsp, error);
        esp_gsp_snapshot_content_changed_active(gsp, gsp->current_scene);
    }
}

static void cancel_visual_transition(gsp_ui_core_t *gsp)
{
    if (gsp->input.adapter_visual_transition) {
        end_visual_transition(gsp, false);
    }
}

static void finish_visual_transition_at(
    gsp_ui_core_t *gsp, const gsp_component_instance_t *instance,
    int32_t target)
{
    bool owns_transition =
        gsp->input.adapter_visual_transition && instance != NULL &&
        gsp->input.adapter_visual_transition_entry == instance->entry_index &&
        gsp->input.adapter_visual_transition_generation == instance->generation;
    if (!owns_transition) {
        return;
    }
    bool commit = target == gsp->input.adapter_visual_transition_target;
    end_visual_transition(gsp, commit);
}

static void arm_visual_transition(
    gsp_ui_core_t *gsp, const gsp_component_instance_t *instance,
    int32_t from, int32_t target, bool commit)
{
    gsp->input.adapter_visual_transition = true;
    gsp->input.adapter_visual_transition_entry = instance->entry_index;
    gsp->input.adapter_visual_transition_generation = instance->generation;
    gsp->input.adapter_visual_transition_from = from;
    gsp->input.adapter_visual_transition_target = target;
    gsp->input.adapter_visual_transition_last_progress = UINT16_MAX;
    gsp->input.adapter_visual_transition_commit = commit;
}

static void replace_visual_transition(gsp_ui_core_t *gsp,
                                      const gsp_component_instance_t *instance)
{
    if (gsp->input.adapter_visual_transition &&
            gsp->input.adapter_visual_transition_entry == instance->entry_index &&
            gsp->input.adapter_visual_transition_generation == instance->generation) {
        end_visual_transition_at_current(gsp);
    } else {
        cancel_visual_transition(gsp);
    }
}

gsp_err_t esp_gsp_drawer_set_open_tracked(
    gsp_ui_core_t *gsp, gsp_component_instance_t *instance,
    const gsp_drawer_config_t *config, bool open, bool animated)
{
    int32_t x = 0;
    int32_t y = 0;
    if (gsp == NULL || instance == NULL || config == NULL ||
            gsp_transform_get_offset(gsp->components.manager.runtime,
                                     config->group_id, &x, &y) != GSP_OK) {
        return GSP_ERR_INVALID_ARG;
    }
    const gsp_drawer_state_t *state = instance->extension;
    /* Keep a repeated target's existing animation and captured endpoints. */
    if (animated && state->track.settling && state->target_open == open) {
        return GSP_OK;
    }
    bool axis_y = config->edge == GSP_DRAWER_EDGE_TOP ||
                  config->edge == GSP_DRAWER_EDGE_BOTTOM;
    int32_t from = axis_y ? y : x;
    int32_t closed =
        config->edge == GSP_DRAWER_EDGE_TOP ? -(int32_t)config->height :
        config->edge == GSP_DRAWER_EDGE_BOTTOM ? config->height :
        config->edge == GSP_DRAWER_EDGE_LEFT ? -(int32_t)config->width :
        config->width;
    int32_t target = open ? 0 : closed;
    bool snapshot = false;
    if (from != target && gsp->input.adapter_visual_transition) {
        (void)finish_snapshotless_jump(gsp);
    }
    bool owns_transition = gsp->input.adapter_visual_transition &&
                           gsp->input.adapter_visual_transition_entry == instance->entry_index &&
                           gsp->input.adapter_visual_transition_generation == instance->generation;
    if (!animated) {
        finish_visual_transition_at(gsp, instance, target);
        if (gsp->input.gesture == instance &&
                gsp->input.gesture_generation == instance->generation) {
            cancel_contact_until_up(gsp);
        }
    } else if (owns_transition && (from != target ||
                                   target != gsp->input.adapter_visual_transition_target)) {
        end_visual_transition_at_current(gsp);
    } else if (from != target) {
        cancel_visual_transition(gsp);
    }
    if (animated && from != target) {
        esp_gsp_transition_t kind =
            config->edge == GSP_DRAWER_EDGE_TOP ? ESP_GSP_SLIDE_DOWN :
            config->edge == GSP_DRAWER_EDGE_BOTTOM ? ESP_GSP_SLIDE_UP :
            config->edge == GSP_DRAWER_EDGE_LEFT ? ESP_GSP_SLIDE_RIGHT :
            ESP_GSP_SLIDE_LEFT;
        gsp_rect_t viewport = {
            config->x, config->y,
            config->x + config->width, config->y + config->height,
        };
        snapshot = begin_viewport_transition(
                       gsp, config->group_id, viewport, from, target,
                       axis_y, true, (gsp_transition_kind_t)kind);
    }
    gsp_err_t ret = gsp_drawer_driver_set_open(
                        &gsp->components.manager, instance, open, animated,
                        (uint64_t)gsp->now_us);
    if (ret != GSP_OK) {
        if (snapshot) {
            end_visual_transition(gsp, false);
        }
        return ret;
    }
    if (snapshot) {
        arm_visual_transition(gsp, instance, from, target, true);
    }
    return GSP_OK;
}

gsp_err_t esp_gsp_page_flow_set_page_tracked(
    gsp_ui_core_t *gsp, gsp_component_instance_t *instance,
    const gsp_page_flow_config_t *config, uint16_t page, bool animated)
{
    int32_t x = 0;
    int32_t y = 0;
    if (gsp == NULL || instance == NULL || config == NULL ||
            page >= config->page_count) {
        return GSP_ERR_INVALID_ARG;
    }
    if (gsp->input.adapter_visual_transition) {
        (void)finish_snapshotless_jump(gsp);
    }
    if (gsp_transform_get_offset(gsp->components.manager.runtime,
                                 config->group_id, &x, &y) != GSP_OK) {
        return GSP_ERR_INVALID_ARG;
    }
    bool axis_y = config->axis != GSP_GESTURE_AXIS_HORIZONTAL;
    int32_t from = axis_y ? y : x;
    int32_t step = page_flow_step(config);
    int32_t target = -(int32_t)page * step;
    bool snapshot = false;
    if (from != target) {
        replace_visual_transition(gsp, instance);
    }
    if (animated && from != target) {
        gsp_transition_kind_t kind = target < from
                                     ? (axis_y ? GSP_TRANSITION_SLIDE_UP
                                        : GSP_TRANSITION_SLIDE_LEFT)
                                     : (axis_y ? GSP_TRANSITION_SLIDE_DOWN
                                        : GSP_TRANSITION_SLIDE_RIGHT);
        gsp_rect_t viewport;
        snapshot = page_flow_viewport(gsp, config, &viewport) && begin_viewport_transition(
                       gsp, config->group_id, viewport, from, target,
                       axis_y, false, kind);
    }
    bool settle = animated &&
                  (snapshot || snapshotless_jump_is_safe(from, target, step));
    gsp_err_t ret = gsp_page_flow_driver_set_page(
                        &gsp->components.manager, instance, page, settle,
                        (uint64_t)gsp->now_us);
    if (ret != GSP_OK) {
        if (snapshot) {
            end_visual_transition(gsp, false);
        }
        return ret;
    }
    if (snapshot) {
        arm_visual_transition(gsp, instance, from, target, true);
    }
    return GSP_OK;
}

gsp_err_t esp_gsp_stack_view_push_tracked(
    gsp_ui_core_t *gsp, gsp_component_instance_t *instance,
    const gsp_stack_view_config_t *config, uint16_t page, bool animated)
{
    int32_t x = 0;
    int32_t y = 0;
    if (gsp == NULL || instance == NULL || config == NULL ||
            page >= config->page_count) {
        return GSP_ERR_INVALID_ARG;
    }
    /* Reject before the snapshot: begin_viewport_transition can retain a
     * full-screen buffer, so a push the driver would refuse must not pay
     * for one. The pending depth is what the driver commits to first. */
    if (gsp_stack_view_driver_pending_depth(instance) >= config->capacity) {
        return GSP_ERR_LIMIT_EXCEEDED;
    }
    if (gsp->input.adapter_visual_transition) {
        (void)finish_snapshotless_jump(gsp);
    }
    if (gsp_transform_get_offset(gsp->components.manager.runtime,
                                 config->group_id, &x, &y) != GSP_OK) {
        return GSP_ERR_INVALID_ARG;
    }
    bool axis_y = config->axis != GSP_GESTURE_AXIS_HORIZONTAL;
    int32_t from = axis_y ? y : x;
    int32_t step = axis_y ? (int32_t)config->height : (int32_t)config->width;
    int32_t target = -(int32_t)page * step;
    bool snapshot = false;
    if (from != target) {
        replace_visual_transition(gsp, instance);
    }
    if (animated && from != target) {
        gsp_transition_kind_t kind = axis_y
                                     ? GSP_TRANSITION_SLIDE_UP
                                     : GSP_TRANSITION_SLIDE_LEFT;
        gsp_rect_t viewport = {
            config->x, config->y,
            config->x + config->width, config->y + config->height,
        };
        snapshot = begin_viewport_transition(
                       gsp, config->group_id, viewport, from, target,
                       axis_y, false, kind);
    }
    bool settle = animated &&
                  (snapshot ||
                   snapshotless_jump_is_safe(from, target, step));
    gsp_err_t ret = gsp_stack_view_driver_push(
                        &gsp->components.manager, instance, page, settle,
                        (uint64_t)gsp->now_us);
    if (ret != GSP_OK) {
        if (snapshot) {
            end_visual_transition(gsp, false);
        }
        return ret;
    }
    if (snapshot) {
        arm_visual_transition(gsp, instance, from, target, true);
    }
    return GSP_OK;
}

gsp_err_t esp_gsp_stack_view_pop_tracked(
    gsp_ui_core_t *gsp, gsp_component_instance_t *instance,
    const gsp_stack_view_config_t *config, bool animated)
{
    int32_t x = 0;
    int32_t y = 0;
    if (gsp == NULL || instance == NULL || config == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    /* pages[] holds authored page ids in push order and they need not be
     * consecutive, so the landing offset cannot come from the current
     * position. Reading the pending depth also lets a pop interrupt an
     * unfinished push instead of being rejected as a pop from the root. */
    uint16_t page;
    if (!gsp_stack_view_driver_pop_target(instance, &page)) {
        return GSP_ERR_NOT_FOUND;
    }
    if (gsp->input.adapter_visual_transition) {
        (void)finish_snapshotless_jump(gsp);
    }
    if (gsp_transform_get_offset(gsp->components.manager.runtime,
                                 config->group_id, &x, &y) != GSP_OK) {
        return GSP_ERR_INVALID_ARG;
    }
    bool axis_y = config->axis != GSP_GESTURE_AXIS_HORIZONTAL;
    int32_t from = axis_y ? y : x;
    int32_t step = axis_y ? (int32_t)config->height : (int32_t)config->width;
    int32_t target = -(int32_t)page * step;
    bool snapshot = false;
    if (from != target) {
        replace_visual_transition(gsp, instance);
    }
    if (animated && from != target) {
        gsp_transition_kind_t kind = axis_y
                                     ? GSP_TRANSITION_SLIDE_DOWN
                                     : GSP_TRANSITION_SLIDE_RIGHT;
        gsp_rect_t viewport = {
            config->x, config->y,
            config->x + config->width, config->y + config->height,
        };
        snapshot = begin_viewport_transition(
                       gsp, config->group_id, viewport, from, target,
                       axis_y, false, kind);
    }
    bool settle = animated &&
                  (snapshot ||
                   snapshotless_jump_is_safe(from, target, step));
    gsp_err_t ret = gsp_stack_view_driver_pop(
                        &gsp->components.manager, instance, settle,
                        (uint64_t)gsp->now_us);
    if (ret != GSP_OK) {
        if (snapshot) {
            end_visual_transition(gsp, false);
        }
        return ret;
    }
    if (snapshot) {
        arm_visual_transition(gsp, instance, from, target, true);
    }
    return GSP_OK;
}

static gsp_pointer_gesture_result_t pointer_op_gesture_begin(void *ctx, int32_t dx, int32_t dy)
{
    gsp_ui_core_t *gsp = ctx;
    const gsp_component_directory_t *directory =
        gsp->components.manager.directory;
    if (directory == NULL) {
        return GSP_POINTER_GESTURE_NONE;
    }
    int32_t px = gsp->input.classifier.press_x;
    int32_t py = gsp->input.classifier.press_y;
    bool overlay_blocks = open_drawer_covers_point(gsp, px, py);
    /* Overlay gestures resolve before ordinary page gestures. Directory order is
     * reversed so the last authored eligible overlay is topmost. */
    for (uint16_t cursor = directory->count; cursor != 0; --cursor) {
        uint16_t entry = cursor - 1U;
        const gsp_component_entry_t *descriptor =
            &directory->entries[entry];
        if (descriptor->kind != GSP_COMPONENT_KIND_DRAWER ||
                descriptor->config == NULL) {
            continue;
        }
        const gsp_drawer_config_t *config = descriptor->config;
        if (!motion_component_enabled(gsp, descriptor) ||
                transform_group_hidden(
                    gsp->components.manager.runtime, config->group_id)) {
            continue;
        }
        bool axis_y = gsp_drawer_axis_is_y(config);
        if ((axis_y && (dy == 0 || (dy < 0 ? -dy : dy) <=
                        (dx < 0 ? -dx : dx))) ||
                (!axis_y && (dx == 0 || (dx < 0 ? -dx : dx) <=
                             (dy < 0 ? -dy : dy)))) {
            continue;
        }
        gsp_component_instance_t *instance;
        if (gsp_component_manager_activate(
                    &gsp->components.manager, entry, ESP_GSP_DRIVER_DRAWER,
                    &instance) != GSP_OK) {
            continue;
        }
        bool open = gsp_drawer_driver_is_open(instance);
        bool settling = gsp_drawer_driver_animating(instance);
        bool eligible = (open || settling) && point_in_rect(
                            px, py, config->x, config->y, config->width, config->height);
        if (!open && !settling) {
            bool towards_open =
                config->edge == GSP_DRAWER_EDGE_TOP ? dy > 0 :
                config->edge == GSP_DRAWER_EDGE_BOTTOM ? dy < 0 :
                config->edge == GSP_DRAWER_EDGE_LEFT ? dx > 0 : dx < 0;
            eligible = towards_open &&
                       drawer_edge_band_contains(gsp, config, px, py);
        }
        if (!eligible || gsp_drawer_driver_begin_drag(
                    &gsp->components.manager, instance, px, py) != GSP_OK) {
            continue;
        }
        uint16_t source_scene = gsp->current_scene;
        uint16_t source_generation = instance->generation;
        cancel_press(gsp);
        if (gsp->current_scene != source_scene || instance->generation != source_generation || instance->extension == NULL ||
                !((const gsp_drawer_state_t *)instance->extension)->track.dragging) {
            return GSP_POINTER_GESTURE_CONSUMED;
        }
        if (!motion_component_enabled(gsp, descriptor) ||
                transform_group_hidden(gsp->components.manager.runtime, config->group_id)) {
            const gsp_component_input_t event = {.kind = GSP_COMPONENT_INPUT_CANCEL};
            (void)gsp_component_manager_event(&gsp->components.manager, instance,
                                              source_generation, &event, NULL);
            return GSP_POINTER_GESTURE_CONSUMED;
        }
        gesture_set(gsp, instance, GSP_COMPONENT_KIND_DRAWER,
                    config->group_id);
        /* A re-grab can start anywhere between the anchors and reverse more
         * than once. Track the finger from its actual offset in the driver,
         * but retain the full closed/open interval for snapshot progress. */
        int32_t from_offset = open ? 0 : gsp_drawer_closed_offset(config);
        int32_t target_offset = open ? gsp_drawer_closed_offset(config) : 0;
        esp_gsp_transition_t kind =
            config->edge == GSP_DRAWER_EDGE_TOP ? ESP_GSP_SLIDE_DOWN :
            config->edge == GSP_DRAWER_EDGE_BOTTOM ? ESP_GSP_SLIDE_UP :
            config->edge == GSP_DRAWER_EDGE_LEFT ? ESP_GSP_SLIDE_RIGHT :
            ESP_GSP_SLIDE_LEFT;
        gsp_rect_t viewport = {
            .x1 = config->x, .y1 = config->y,
            .x2 = config->x + config->width,
            .y2 = config->y + config->height,
        };
        replace_visual_transition(gsp, instance);
        gsp->input.adapter_visual_transition =
            begin_viewport_transition(
                gsp, config->group_id, viewport,
                from_offset, target_offset, axis_y, true,
                (gsp_transition_kind_t)kind);
        gsp->input.adapter_visual_transition_from = from_offset;
        gsp->input.adapter_visual_transition_target = target_offset;
        gsp_swipe_kinetics_reset(
            &gsp->input.gesture_kinetics, 0,
            gsp->input.press_time_us);
        if (gsp->input.adapter_visual_transition) {
            gsp->input.adapter_visual_transition_entry = instance->entry_index;
            gsp->input.adapter_visual_transition_generation = instance->generation;
            gsp->input.adapter_visual_transition_last_progress = UINT16_MAX;
            gsp->input.adapter_visual_transition_commit = true;
        }
        GSP_LOGI(TAG,
                 "drawer gesture begin: scene=%u entry=%u group=%u open=%u "
                 "press=(%ld,%ld) delta=(%ld,%ld)",
                 gsp->current_scene, entry, config->group_id, open,
                 (long)px, (long)py, (long)dx, (long)dy);
        press_feedback_hide(gsp);
        return axis_y ? GSP_POINTER_GESTURE_TRACK_Y : GSP_POINTER_GESTURE_TRACK_X;
    }
    if (overlay_blocks) {
        /* The gesture is perpendicular to the open Drawer (or the Drawer
         * could not begin a drag). Consume it at this z-plane instead of
         * leaking it to a PageFlow or scene swipe underneath. */
        cancel_press(gsp);
        return GSP_POINTER_GESTURE_CONSUMED;
    }
    for (uint16_t cursor = directory->count; cursor != 0; --cursor) {
        uint16_t entry = cursor - 1U;
        const gsp_component_entry_t *descriptor =
            &directory->entries[entry];
        if (descriptor->kind != GSP_COMPONENT_KIND_PAGE_FLOW ||
                descriptor->config == NULL) {
            continue;
        }
        const gsp_page_flow_config_t *config = descriptor->config;
        if (!motion_component_enabled(gsp, descriptor) ||
                !component_page_visible(gsp, config->vis_slot_ref) ||
                transform_group_hidden(
                    gsp->components.manager.runtime, config->group_id)) {
            continue;
        }
        bool axis_y = config->axis == GSP_GESTURE_AXIS_VERTICAL;
        gsp_rect_t viewport;
        if (!page_flow_viewport(gsp, config, &viewport) ||
                px < viewport.x1 || py < viewport.y1 ||
                px >= viewport.x2 || py >= viewport.y2 ||
                (axis_y && (dy < 0 ? -dy : dy) <= (dx < 0 ? -dx : dx)) ||
                (!axis_y && (dx < 0 ? -dx : dx) <=
                 (dy < 0 ? -dy : dy))) {
            continue;
        }
        bool towards_next = axis_y ? dy < 0 : dx < 0;
        gsp_component_instance_t *instance;
        if (gsp_component_manager_activate(
                    &gsp->components.manager, entry, ESP_GSP_DRIVER_PAGE_FLOW,
                    &instance) != GSP_OK) {
            continue;
        }
        uint16_t selected = gsp_page_flow_driver_selected(instance);
        uint16_t target = towards_next
                          ? (selected + 1U < config->page_count ? selected + 1U
                             : (config->cyclic ? 0 : selected))
                          : (selected != 0 ? selected - 1U
                             : (config->cyclic ? config->page_count - 1U : selected));
        int32_t flow_extent = config->page_extent != 0
                              ? config->page_extent
                              : (axis_y ? config->height : config->width);
        int32_t current_offset = -(int32_t)selected * flow_extent;
        bool edge_bounce = !config->cyclic && target == selected;
        bool can_drag = target != selected || edge_bounce;
        if (config->stop_anywhere && !config->cyclic) {
            int32_t offset_x = 0;
            int32_t offset_y = 0;
            if (gsp_transform_get_offset(
                        gsp->components.manager.runtime, config->group_id,
                        &offset_x, &offset_y) != GSP_OK) {
                continue;
            }
            current_offset = axis_y ? offset_y : offset_x;
            can_drag = config->page_count > 1;
        }
        if (!can_drag || gsp_page_flow_driver_begin_drag(
                    &gsp->components.manager, instance, px, py) != GSP_OK) {
            continue;
        }
        uint16_t source_scene = gsp->current_scene;
        uint16_t source_generation = instance->generation;
        cancel_press(gsp);
        if (gsp->current_scene != source_scene || instance->generation != source_generation || instance->extension == NULL ||
                !gsp_page_flow_driver_dragging(instance)) {
            return GSP_POINTER_GESTURE_CONSUMED;
        }
        if (!motion_component_enabled(gsp, descriptor) ||
                !component_page_visible(gsp, config->vis_slot_ref) ||
                transform_group_hidden(gsp->components.manager.runtime, config->group_id)) {
            (void)gsp_page_flow_driver.cancel(&gsp->components.manager, instance, NULL);
            return GSP_POINTER_GESTURE_CONSUMED;
        }
        gesture_set(gsp, instance, GSP_COMPONENT_KIND_PAGE_FLOW,
                    config->group_id);
        gsp->input.gesture_towards_next = towards_next;
        gsp->input.page_flow_origin = selected;
        gsp->input.page_flow_target = target;
        int32_t step = flow_extent;
        int32_t from_offset;
        int32_t target_offset;
        esp_gsp_transition_t kind;
        if (config->stop_anywhere) {
            page_flow_stop_anywhere_window(
                config, current_offset, towards_next,
                &from_offset, &target_offset, &kind);
        } else {
            from_offset = -(int32_t)selected * step;
            target_offset = -(int32_t)target * step;
            if (edge_bounce) {
                int32_t elastic = step / PAGE_FLOW_EDGE_OVERSCROLL_DIVISOR;
                target_offset = from_offset +
                                (towards_next ? -elastic : elastic);
            }
            if (config->cyclic && towards_next &&
                    selected == config->page_count - 1U && target == 0) {
                target_offset = -(int32_t)config->page_count * step;
            } else if (config->cyclic && !towards_next && selected == 0 &&
                       target == config->page_count - 1U) {
                target_offset = step;
            }
            kind = axis_y
                   ? (towards_next ? ESP_GSP_SLIDE_UP : ESP_GSP_SLIDE_DOWN)
                   : (towards_next ? ESP_GSP_SLIDE_LEFT : ESP_GSP_SLIDE_RIGHT);
        }
        replace_visual_transition(gsp, instance);
        gsp->input.adapter_visual_transition =
            begin_viewport_transition(
                gsp, config->group_id, viewport,
                from_offset, target_offset, axis_y, false,
                (gsp_transition_kind_t)kind);
        gsp->input.adapter_visual_transition_from = from_offset;
        gsp->input.adapter_visual_transition_target = target_offset;
        gsp_swipe_kinetics_reset(
            &gsp->input.gesture_kinetics, 0,
            gsp->input.press_time_us);
        if (gsp->input.adapter_visual_transition) {
            gsp->input.adapter_visual_transition_entry = instance->entry_index;
            gsp->input.adapter_visual_transition_generation = instance->generation;
            gsp->input.adapter_visual_transition_last_progress = UINT16_MAX;
            gsp->input.adapter_visual_transition_commit = false;
        }
        press_feedback_hide(gsp);
        return axis_y ? GSP_POINTER_GESTURE_TRACK_Y : GSP_POINTER_GESTURE_TRACK_X;
    }
    return GSP_POINTER_GESTURE_NONE;
}

static void sample_gesture_progress(gsp_ui_core_t *gsp)
{
    if (gsp->input.gesture == NULL) {
        return;
    }
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    if (gsp_transform_get_offset(
                gsp->components.manager.runtime, gsp->input.gesture_group,
                &offset_x, &offset_y) != GSP_OK) {
        return;
    }
    int32_t at = offset_x;
    const gsp_component_entry_t *entry =
        &gsp->components.manager.directory->entries[
            gsp->input.gesture->entry_index];
    if ((entry->kind == GSP_COMPONENT_KIND_PAGE_FLOW &&
            ((const gsp_page_flow_config_t *)entry->config)->axis ==
            GSP_GESTURE_AXIS_VERTICAL) ||
            (entry->kind == GSP_COMPONENT_KIND_DRAWER &&
             gsp_drawer_axis_is_y(entry->config))) {
        at = offset_y;
    }
    int32_t direction =
        gsp->input.adapter_visual_transition_target >=
        gsp->input.adapter_visual_transition_from ? 1 : -1;
    int32_t progress =
        (at - gsp->input.adapter_visual_transition_from) * direction;
    int32_t span =
        (gsp->input.adapter_visual_transition_target -
         gsp->input.adapter_visual_transition_from) * direction;
    if (progress < 0) {
        progress = 0;
    }
    if (progress > span) {
        progress = span;
    }
    gsp_swipe_kinetics_sample(
        &gsp->input.gesture_kinetics, (uint16_t)progress, gsp->now_us);
}

static void rebuild_stop_anywhere_transition(
    gsp_ui_core_t *gsp, const gsp_page_flow_config_t *config,
    gsp_component_instance_t *instance, int32_t start_offset,
    bool towards_next)
{
    gsp_rect_t viewport;
    if (!page_flow_viewport(gsp, config, &viewport)) {
        return;
    }
    int32_t from_offset;
    int32_t target_offset;
    esp_gsp_transition_t kind;
    bool axis_y = config->axis == GSP_GESTURE_AXIS_VERTICAL;
    page_flow_stop_anywhere_window(
        config, start_offset, towards_next,
        &from_offset, &target_offset, &kind);
    replace_visual_transition(gsp, instance);
    gsp->input.adapter_visual_transition =
        begin_viewport_transition(
            gsp, config->group_id, viewport,
            from_offset, target_offset, axis_y, false,
            (gsp_transition_kind_t)kind);
    gsp->input.adapter_visual_transition_from = from_offset;
    gsp->input.adapter_visual_transition_target = target_offset;
    gsp->input.gesture_towards_next = towards_next;
    /* Progress is now measured in the opposite direction; keep the
     * previous swipe's samples out of the release-velocity window. */
    gsp_swipe_kinetics_reset(
        &gsp->input.gesture_kinetics, 0, gsp->now_us);
    if (gsp->input.adapter_visual_transition) {
        gsp->input.adapter_visual_transition_entry = instance->entry_index;
        gsp->input.adapter_visual_transition_generation = instance->generation;
        gsp->input.adapter_visual_transition_last_progress = UINT16_MAX;
        gsp->input.adapter_visual_transition_commit = false;
        esp_gsp_transform_snapshot_step(gsp);
    }
}

static void maybe_rebuild_stop_anywhere_transition(gsp_ui_core_t *gsp)
{
    gsp_component_instance_t *instance = gsp->input.gesture;
    if (instance == NULL ||
            gsp->input.gesture_kind != GSP_COMPONENT_KIND_PAGE_FLOW ||
            !gsp->input.adapter_visual_transition) {
        return;
    }
    const gsp_page_flow_config_t *config =
        gsp->components.manager.directory->entries[
            instance->entry_index].config;
    if (config == NULL || !config->stop_anywhere) {
        return;
    }
    int32_t offset_x = 0;
    int32_t offset_y = 0;
    if (gsp_transform_get_offset(
                gsp->components.manager.runtime, config->group_id,
                &offset_x, &offset_y) != GSP_OK) {
        return;
    }
    int32_t at = config->axis == GSP_GESTURE_AXIS_VERTICAL
                 ? offset_y : offset_x;
    int32_t start = gsp->input.adapter_visual_transition_from;
    int32_t target = gsp->input.adapter_visual_transition_target;
    if ((target - start) * (int64_t)(at - start) >= 0) {
        return;
    }
    rebuild_stop_anywhere_transition(
        gsp, config, instance, start, at < start);
}

static void pointer_op_gesture_move(void *ctx, int32_t x, int32_t y)
{
    gsp_ui_core_t *gsp = ctx;
    if (gsp->input.gesture == NULL) {
        return;
    }
    gsp_update_transaction_t transaction;
    gsp_pending_update_t pending[1];
    if (gsp_update_begin(gsp->components.manager.runtime,
                         &transaction, pending, 1) != GSP_OK) {
        return;
    }
    gsp_component_input_t event = {
        .kind = GSP_COMPONENT_INPUT_MOVE, .x = x, .y = y,
    };
    (void)gsp_component_manager_event(
        &gsp->components.manager, gsp->input.gesture,
        gsp->input.gesture_generation, &event, &transaction);
    (void)gsp_update_commit(&transaction);
    maybe_rebuild_stop_anywhere_transition(gsp);
    sample_gesture_progress(gsp);
}

static void pointer_op_gesture_release(void *ctx, int32_t x, int32_t y,
                                       int32_t flick)
{
    gsp_ui_core_t *gsp = ctx;
    gsp_component_instance_t *instance = gsp->input.gesture;
    if (instance == NULL) {
        return;
    }
    uint8_t gesture_kind = gsp->input.gesture_kind;
    sample_gesture_progress(gsp);
    bool velocity_valid = false;
    int32_t velocity = gsp_swipe_kinetics_velocity(
                           &gsp->input.gesture_kinetics, &velocity_valid);
    int32_t axis_direction =
        gsp->input.adapter_visual_transition_target >=
        gsp->input.adapter_visual_transition_from ? 1 : -1;
    int32_t directed_flick = axis_direction > 0 ? flick : -flick;
    int64_t transition_extent =
        (int64_t)gsp->input.adapter_visual_transition_target -
        gsp->input.adapter_visual_transition_from;
    if (transition_extent < 0) {
        transition_extent = -transition_extent;
    }
    int32_t settle_velocity = gesture_settle_velocity(
                                  gsp, velocity, directed_flick,
                                  (uint32_t)transition_extent);
    int32_t axis_velocity = axis_direction > 0 ? settle_velocity :
                            (settle_velocity == INT32_MIN ? INT32_MAX :
                             -settle_velocity);
    if (gesture_kind == GSP_COMPONENT_KIND_PAGE_FLOW) {
        const gsp_page_flow_config_t *config =
            gsp->components.manager.directory->entries[
                instance->entry_index].config;
        if (config->stop_anywhere) {
            if (gsp_page_flow_driver_stop_drag_kinetic(
                        &gsp->components.manager, instance, axis_velocity,
                        (uint64_t)gsp->now_us) != GSP_OK) {
                gsp_component_input_t event = {
                    .kind = GSP_COMPONENT_INPUT_UP, .x = x, .y = y,
                };
                (void)gsp_component_manager_event(
                    &gsp->components.manager, instance,
                    gsp->input.gesture_generation, &event, NULL);
            }
            /* Coast on the live transform, same split as list: the
             * adjacent snapshot is only for the finger-down drag. */
            if (gsp->input.adapter_visual_transition) {
                end_visual_transition_at_current(gsp);
            }
            gsp->input.gesture = NULL;
            return;
        }
        int32_t offset_x = 0;
        int32_t offset_y = 0;
        (void)gsp_transform_get_offset(
            gsp->components.manager.runtime, config->group_id,
            &offset_x, &offset_y);
        int32_t current_offset =
            config->axis == GSP_GESTURE_AXIS_HORIZONTAL
            ? offset_x : offset_y;
        uint16_t extent = config->page_extent != 0 ? config->page_extent :
                          (config->axis == GSP_GESTURE_AXIS_HORIZONTAL
                           ? config->width : config->height);
        int32_t origin_offset =
            -(int32_t)gsp->input.page_flow_origin * extent;
        int32_t directed_distance = gsp->input.gesture_towards_next
                                    ? origin_offset - current_offset
                                    : current_offset - origin_offset;
        if (directed_distance < 0) {
            directed_distance = 0;
        }
        int32_t toward_flick = gsp->input.gesture_towards_next
                               ? -flick : flick;
        /*
         * Position selects commit after the normal distance threshold, while
         * a confirmed release-time reversal may still cancel before the
         * terminal hysteresis zone. Velocity only decides shorter drags.
         */
        bool crossed = (uint32_t)directed_distance >
                       extent / GSP_POLICY(gsp,
                                           ESP_GSP_FIELD_COMPONENT_SWIPE_COMMIT_DIVISOR);
        int32_t velocity_threshold =
            (int32_t)extent * GSP_POLICY(
                gsp, ESP_GSP_FIELD_SWIPE_VELOCITY_NUMERATOR) /
            GSP_POLICY(gsp, ESP_GSP_FIELD_SWIPE_VELOCITY_DENOMINATOR);
        bool fast_forward =
            (uint32_t)directed_distance >= GSP_POLICY(
                gsp, ESP_GSP_FIELD_SCENE_SWIPE_MIN_FLING_PX) &&
            ((velocity_valid && velocity >= velocity_threshold) ||
             toward_flick >= GSP_POLICY(
                 gsp, ESP_GSP_FIELD_SWIPE_FLICK_PX));
        bool edge_bounce =
            gsp->input.page_flow_target == gsp->input.page_flow_origin;
        bool commit = !edge_bounce &&
                      (crossed || (!crossed && fast_forward));
        if (commit && gesture_release_reverses(
                    gsp, (uint32_t)directed_distance, extent,
                    velocity, velocity_valid, toward_flick)) {
            commit = false;
        }
        gsp->input.adapter_visual_transition_commit = commit;
        if (commit) {
            (void)gsp_page_flow_driver_set_page_direction_kinetic(
                &gsp->components.manager, instance,
                gsp->input.page_flow_target,
                gsp->input.gesture_towards_next, axis_velocity,
                (uint64_t)gsp->now_us);
        } else {
            (void)gsp_page_flow_driver_set_page_kinetic(
                &gsp->components.manager, instance,
                gsp->input.page_flow_origin, axis_velocity,
                (uint64_t)gsp->now_us);
        }
    } else if (gesture_kind == GSP_COMPONENT_KIND_DRAWER) {
        const gsp_drawer_config_t *config =
            gsp->components.manager.directory->entries[
                instance->entry_index].config;
        bool axis_y = gsp_drawer_axis_is_y(config);
        int32_t offset_x = 0;
        int32_t offset_y = 0;
        (void)gsp_transform_get_offset(
            gsp->components.manager.runtime, config->group_id,
            &offset_x, &offset_y);
        int32_t current = axis_y ? offset_y : offset_x;
        bool was_open = gsp_drawer_driver_is_open(instance);
        int32_t closed = gsp_drawer_closed_offset(config);
        int32_t from = was_open ? 0 : closed;
        int32_t to = was_open ? closed : 0;
        uint16_t extent = axis_y ? config->height : config->width;
        int32_t travelled = current - from;
        if (travelled < 0) {
            travelled = -travelled;
        }
        if (travelled > (int32_t)extent) {
            travelled = extent;
        }
        int32_t toward_flick = to > from ? flick : -flick;
        /*
         * Same release policy as PageFlow: position wins once the drawer
         * crosses the commit threshold, and velocity decides the short
         * drags below it. A bare midpoint rule strands a quick half pull.
         */
        bool crossed = (uint32_t)travelled >
                       extent / GSP_POLICY(gsp,
                                           ESP_GSP_FIELD_COMPONENT_SWIPE_COMMIT_DIVISOR);
        int32_t velocity_threshold =
            (int32_t)extent * GSP_POLICY(
                gsp, ESP_GSP_FIELD_SWIPE_VELOCITY_NUMERATOR) /
            GSP_POLICY(gsp, ESP_GSP_FIELD_SWIPE_VELOCITY_DENOMINATOR);
        bool fast_forward =
            (uint32_t)travelled >= GSP_POLICY(
                gsp, ESP_GSP_FIELD_SCENE_SWIPE_MIN_FLING_PX) &&
            ((velocity_valid && velocity >= velocity_threshold) ||
             toward_flick >= GSP_POLICY(
                 gsp, ESP_GSP_FIELD_SWIPE_FLICK_PX));
        bool commit = crossed || fast_forward;
        if (commit && gesture_release_reverses(
                    gsp, (uint32_t)travelled, extent,
                    velocity, velocity_valid, toward_flick)) {
            commit = false;
        }
        /*
         * The frozen presentation must end where the drawer actually
         * settles. Committing a snapped-back drawer publishes the opposite
         * terminal frame and forces a full-screen repair to correct it.
         */
        gsp->input.adapter_visual_transition_commit = commit;
        (void)gsp_drawer_driver_end_drag_kinetic(
            &gsp->components.manager, instance,
            commit ? !was_open : was_open, axis_velocity,
            (uint64_t)gsp->now_us);
        GSP_LOGI(TAG,
                 "drawer gesture release: scene=%u entry=%u "
                 "release=(%ld,%ld) velocity=%ldpx/s timed=%u flick=%ld "
                 "travelled=%ld/%u settle=%lums commit=%u",
                 gsp->current_scene, instance->entry_index,
                 (long)x, (long)y, (long)velocity, velocity_valid,
                 (long)toward_flick, (long)travelled, (unsigned)extent,
                 (unsigned long)(
                     ((gsp_drawer_state_t *)instance->extension)
                     ->track.duration_us / 1000U),
                 (unsigned)commit);
    } else {
        gsp_update_transaction_t transaction;
        gsp_pending_update_t pending[1];
        if (gsp_update_begin(gsp->components.manager.runtime,
                             &transaction, pending, 1) == GSP_OK) {
            gsp_component_input_t event = {
                .kind = GSP_COMPONENT_INPUT_UP, .x = x, .y = y,
            };
            (void)gsp_component_manager_event(
                &gsp->components.manager, instance,
                gsp->input.gesture_generation, &event, &transaction);
            (void)gsp_update_commit(&transaction);
        }
    }
    gsp->input.gesture = NULL;
}

void esp_gsp_snapshot_reconcile_content(gsp_ui_core_t *gsp)
{
    gsp_runtime_t *runtime = gsp->components.manager.runtime;
    if (runtime != NULL && runtime->snapshot_owner != NULL &&
            runtime->snapshot_revision != runtime->snapshot_capture_revision) {
        esp_gsp_snapshot_content_changed_active(gsp, gsp->current_scene);
    }
}

void esp_gsp_transform_snapshot_step(gsp_ui_core_t *gsp)
{
    if (gsp != NULL) {
        esp_gsp_snapshot_reconcile_content(gsp);
    }
    if (gsp == NULL || !gsp->input.adapter_visual_transition) {
        return;
    }
    bool page_flow_stop_anywhere = false;
    gsp_component_instance_t *instance = visual_transition_owner(gsp);
    if (instance == NULL) {
        goto end;
    }
    const gsp_component_entry_t *entry =
        &gsp->components.manager.directory->entries[instance->entry_index];
    int32_t x;
    int32_t y;
    uint16_t group_id;
    bool busy;
    bool page_flow_committed = false;
    uint8_t axis;
    if (entry->kind == GSP_COMPONENT_KIND_PAGE_FLOW) {
        const gsp_page_flow_config_t *config = entry->config;
        gsp_page_flow_state_t *state = instance->extension;
        if (config == NULL || state == NULL) {
            goto end;
        }
        group_id = config->group_id;
        axis = config->axis;
        busy = state->track.dragging || state->track.settling;
        page_flow_stop_anywhere = config->stop_anywhere;
        page_flow_committed =
            !busy && gsp->input.adapter_visual_transition_commit &&
            state->selected == state->target;
    } else if (entry->kind == GSP_COMPONENT_KIND_DRAWER) {
        const gsp_drawer_config_t *config = entry->config;
        gsp_drawer_state_t *state = instance->extension;
        if (config == NULL || state == NULL) {
            goto end;
        }
        group_id = config->group_id;
        axis = (config->edge == GSP_DRAWER_EDGE_TOP ||
                config->edge == GSP_DRAWER_EDGE_BOTTOM)
               ? GSP_GESTURE_AXIS_VERTICAL : GSP_GESTURE_AXIS_HORIZONTAL;
        busy = state->track.dragging || state->track.settling;
    } else if (entry->kind == GSP_COMPONENT_KIND_STACK_VIEW) {
        const gsp_stack_view_config_t *config = entry->config;
        gsp_stack_view_state_t *state = instance->extension;
        if (config == NULL || state == NULL) {
            goto end;
        }
        group_id = config->group_id;
        axis = config->axis;
        busy = state->track.settling;
    } else {
        goto end;
    }
    if (
        gsp_transform_get_offset(gsp->components.manager.runtime,
                                 group_id, &x, &y) != GSP_OK) {
        goto end;
    }
    int32_t at = axis == GSP_GESTURE_AXIS_HORIZONTAL ? x : y;
    /*
     * A cyclic PageFlow normalizes its mirrored boundary anchor in the same
     * tick that completes the settle.  That normalized offset is visually
     * equivalent, but it is outside this frozen transition's from/target
     * coordinate interval.  Select the terminal snapshot explicitly before
     * teardown; otherwise the normalized offset clamps progress back to zero
     * and flashes the source page for one frame.
     */
    if (!busy && entry->kind == GSP_COMPONENT_KIND_PAGE_FLOW &&
            !page_flow_stop_anywhere) {
        at = page_flow_committed
             ? gsp->input.adapter_visual_transition_target
             : gsp->input.adapter_visual_transition_from;
    }
    int32_t direction = gsp->input.adapter_visual_transition_target >=
                        gsp->input.adapter_visual_transition_from ? 1 : -1;
    int32_t progress =
        (at - gsp->input.adapter_visual_transition_from) * direction;
    if (progress < 0) {
        progress = 0;
    }
    int32_t span = (gsp->input.adapter_visual_transition_target -
                    gsp->input.adapter_visual_transition_from) * direction;
    if (progress > span) {
        progress = span;
    }
    if ((uint16_t)progress != gsp->input.adapter_visual_transition_last_progress &&
            gsp->adapter_ops != NULL &&
            gsp->adapter_ops->visual_transition_progress != NULL) {
        gsp->adapter_ops->visual_transition_progress(
            gsp->adapter_ctx, (uint16_t)progress);
        gsp->input.adapter_visual_transition_last_progress = (uint16_t)progress;
    }
    if (busy) {
        return;
    }
end:
    /*
     * The component may normalize a cyclic anchor or retire its instance
     * before this observer sees the settled offset.  Finish the frozen
     * presentation from the release decision, not from component storage.
     */
    if (page_flow_stop_anywhere) {
        end_visual_transition_at_current(gsp);
    } else {
        end_visual_transition(
            gsp, gsp->input.adapter_visual_transition_commit);
    }
}

static void pointer_op_swipe_move(void *ctx, uint32_t distance)
{
    gsp_ui_core_t *gsp = ctx;
    gsp->nav.drag_progress = (uint16_t)distance;
    gsp_swipe_kinetics_sample(&gsp->nav.swipe_kinetics,
                              (uint16_t)distance, gsp->now_us);
}

static void pointer_op_swipe_release(void *ctx, uint32_t distance,
                                     int32_t flick)
{
    gsp_ui_core_t *gsp = ctx;
    uint16_t width = esp_gsp_current_ctx(gsp)->scene.screen_width;
    gsp_swipe_kinetics_sample(&gsp->nav.swipe_kinetics,
                              (uint16_t)distance, gsp->now_us);
    bool velocity_valid = false;
    int32_t velocity = gsp_swipe_kinetics_velocity(
                           &gsp->nav.swipe_kinetics, &velocity_valid);
    bool commit = distance >
                  width / GSP_POLICY(
                      gsp, ESP_GSP_FIELD_SCENE_SWIPE_COMMIT_DIVISOR);
    if (!commit && distance >= GSP_POLICY(
                gsp, ESP_GSP_FIELD_SCENE_SWIPE_MIN_FLING_PX)) {
        int32_t velocity_threshold =
            (int32_t)width * GSP_POLICY(
                gsp, ESP_GSP_FIELD_SWIPE_VELOCITY_NUMERATOR) /
            GSP_POLICY(gsp, ESP_GSP_FIELD_SWIPE_VELOCITY_DENOMINATOR);
        /*
         * Sparse polling can retain only two coordinates for a real flick;
         * its elapsed-time estimate then understates release intent. The
         * last-three-delta fallback remains valid even with timed input,
         * while a held coordinate naturally ages to a zero flick.
         */
        commit = (velocity_valid && velocity >= velocity_threshold) ||
                 flick >= GSP_POLICY(
                     gsp, ESP_GSP_FIELD_SWIPE_FLICK_PX);
    }
    if (commit && gesture_release_reverses(
                gsp, distance, width, velocity, velocity_valid, flick)) {
        commit = false;
    }
    gsp->nav.drag_progress = (uint16_t)distance;
    gsp->nav.settle_commit = commit;
    int32_t settle_velocity = gesture_settle_velocity(
                                  gsp, velocity, flick, width);
    gsp_swipe_kinetics_settle_begin(
        &gsp->nav.swipe_kinetics, (uint16_t)distance,
        commit ? width : 0U, width, settle_velocity, gsp->now_us);
    GSP_LOGI(TAG,
             "scene swipe release: current=%u target=%u distance=%lu "
             "velocity=%ldpx/s settle_velocity=%ldpx/s timed=%u "
             "flick=%ld samples=%u "
             "delta=(%ld,%ld) duration=%lldms settle=%lums commit=%u",
             gsp->current_scene, gsp->nav.drag_target,
             (unsigned long)distance, (long)velocity,
             (long)settle_velocity, velocity_valid,
             (long)flick, (unsigned)gsp->input.classifier.swipe_head,
             (long)(gsp->input.classifier.last_x -
                    gsp->input.classifier.press_x),
             (long)(gsp->input.classifier.last_y -
                    gsp->input.classifier.press_y),
             (long long)((gsp->now_us - gsp->input.press_time_us) / 1000),
             (unsigned long)(
                 gsp->nav.swipe_kinetics.settle_duration_us / 1000U),
             commit);
}

void esp_gsp_swipe_step(gsp_ui_core_t *gsp)
{
    if (gsp == NULL ||
            (!gsp->nav.adapter_transition && !gsp->nav.drag_fallback)) {
        return;
    }

    bool done = false;
    if (gsp->nav.swipe_kinetics.settling) {
        gsp->nav.drag_progress = gsp_swipe_kinetics_settle_step(
                                     &gsp->nav.swipe_kinetics, gsp->now_us, &done);
    }
    if (gsp->nav.adapter_transition &&
            gsp->nav.drag_progress != gsp->nav.drag_presented_progress &&
            gsp->adapter_ops != NULL &&
            gsp->adapter_ops->visual_transition_progress != NULL) {
        gsp->adapter_ops->visual_transition_progress(
            gsp->adapter_ctx, gsp->nav.drag_progress);
        gsp->nav.drag_presented_progress = gsp->nav.drag_progress;
    }
    if (!done) {
        return;
    }

    bool commit = gsp->nav.settle_commit;
    uint16_t target = gsp->nav.drag_target;
    if (gsp->nav.adapter_transition && gsp->adapter_ops != NULL &&
            gsp->adapter_ops->visual_transition_end != NULL) {
        gsp->adapter_ops->visual_transition_end(gsp->adapter_ctx, commit);
    }
    gsp->nav.adapter_transition = false;
    gsp->nav.drag_fallback = false;
    gsp->nav.drag_progress = 0;
    gsp->nav.drag_presented_progress = 0;
    gsp->nav.settle_commit = false;
    if (commit) {
        gsp_ui_core_switch_scene(gsp, target);
    }
}

static void pointer_op_tap(void *ctx, int32_t x, int32_t y)
{
    gsp_ui_core_t *gsp = ctx;
    if (!gsp->input.press_cancelled && !gsp->input.long_press_fired && press_hit_matches(gsp)) {
        handle_tap(gsp, x, y);
    }
}

static void pointer_op_release(void *ctx)
{
    gsp_ui_core_t *gsp = ctx;
    const gsp_pointer_t *pointer = &gsp->input.classifier;
    if (pointer->owner == GSP_POINTER_OWNER_CAPTURE && !gsp->input.press_finished) {
        if (gsp->input.range_notify && !range_press_enabled(gsp)) {
            cancel_press(gsp);
        } else {
            gsp->input.press_finished = true;
        }
    }
    if (pointer->owner == GSP_POINTER_OWNER_HIT && !gsp->input.press_finished) {
        if (pointer->tap_cancelled ||
                !press_hit_matches_at(gsp, pointer->last_x, pointer->last_y)) {
            cancel_press(gsp);
        } else {
            gsp->input.press_finished = true;
            press_feedback_hide(gsp);
            button_pressed_state_clear(gsp);
            if (press_has_event(gsp, GSP_INPUT_EVENT_RELEASE)) {
                handle_input_event(gsp, pointer->press_x, pointer->press_y, GSP_INPUT_EVENT_RELEASE);
            }
        }
    }
    press_feedback_hide(gsp);
    gsp->input.long_press_pending = false;
    button_pressed_state_clear(gsp);
}

static void pointer_op_cancel(void *ctx)
{
    gsp_ui_core_t *gsp = ctx;
    gsp_pointer_owner_t owner = gsp->input.classifier.owner;
    cancel_press(gsp);
    gsp->input.long_press_pending = false;
    press_feedback_hide(gsp);
    button_pressed_state_clear(gsp);
    (void)esp_gsp_carousel_release(gsp, true);
    if (owner == GSP_POINTER_OWNER_CAPTURE) {
        capture_event(gsp, GSP_COMPONENT_INPUT_CANCEL, 0, 0);
    } else if (owner == GSP_POINTER_OWNER_SCROLL) {
        gsp->lists.active = -1;
        esp_gsp_overlay_rebuild(gsp);
    } else if (owner == GSP_POINTER_OWNER_GESTURE &&
               gsp->input.gesture != NULL) {
        gsp_component_input_t event = {
            .kind = GSP_COMPONENT_INPUT_CANCEL,
        };
        (void)gsp_component_manager_event(
            &gsp->components.manager, gsp->input.gesture,
            gsp->input.gesture_generation, &event, NULL);
        gsp->input.gesture = NULL;
    } else if (owner == GSP_POINTER_OWNER_SWIPE) {
        pointer_op_swipe_release(gsp, 0, 0);
    }
    cancel_visual_transition(gsp);
}

static const gsp_pointer_ops_t POINTER_OPS = {
    .press = pointer_op_press,
    .capture_move = pointer_op_capture_move,
    .capture_release = pointer_op_capture_release,
    .scroll_hit = pointer_op_scroll_hit,
    .scroll_begin = pointer_op_scroll_begin,
    .scroll_move = pointer_op_scroll_move,
    .scroll_release = pointer_op_scroll_release,
    .gesture_begin = pointer_op_gesture_begin,
    .gesture_move = pointer_op_gesture_move,
    .gesture_release = pointer_op_gesture_release,
    .swipe_begin = pointer_op_swipe_begin,
    .swipe_move = pointer_op_swipe_move,
    .swipe_release = pointer_op_swipe_release,
    .tap = pointer_op_tap,
    .release = pointer_op_release,
    .cancel = pointer_op_cancel,
};

static void multitouch_cancel_single(void *ctx)
{
    gsp_ui_core_t *gsp = ctx;
    gsp_pointer_cancel(&gsp->input.classifier);
}

static bool multitouch_pinch_begin(void *ctx, int32_t center_x,
                                   int32_t center_y)
{
    gsp_ui_core_t *gsp = ctx;
    if (gsp->callbacks.pinch_cb == NULL) {
        return false;
    }
    gsp->input.pinch_center_x = center_x;
    gsp->input.pinch_center_y = center_y;
    gsp->input.pinch_scale_q16 = UINT32_C(1) << 16;
    const esp_gsp_pinch_event_t event = {
        .phase = ESP_GSP_PINCH_BEGIN,
        .center_x = center_x,
        .center_y = center_y,
        .relative_scale_q16 = UINT32_C(1) << 16,
    };
    return gsp->callbacks.pinch_cb(gsp, &event, gsp->callbacks.pinch_user_ctx);
}

static void multitouch_pinch_update(void *ctx, int32_t center_x,
                                    int32_t center_y,
                                    uint32_t scale_q16)
{
    gsp_ui_core_t *gsp = ctx;
    gsp->input.pinch_center_x = center_x;
    gsp->input.pinch_center_y = center_y;
    gsp->input.pinch_scale_q16 = scale_q16;
    if (gsp->callbacks.pinch_cb != NULL) {
        const esp_gsp_pinch_event_t event = {
            .phase = ESP_GSP_PINCH_UPDATE,
            .center_x = center_x,
            .center_y = center_y,
            .relative_scale_q16 = scale_q16,
        };
        (void)gsp->callbacks.pinch_cb(gsp, &event, gsp->callbacks.pinch_user_ctx);
    }
}

static void multitouch_pinch_end(void *ctx, bool cancelled)
{
    gsp_ui_core_t *gsp = ctx;
    if (gsp->callbacks.pinch_cb != NULL) {
        const esp_gsp_pinch_event_t event = {
            .phase = cancelled ? ESP_GSP_PINCH_CANCEL : ESP_GSP_PINCH_END,
            .center_x = gsp->input.pinch_center_x,
            .center_y = gsp->input.pinch_center_y,
            .relative_scale_q16 = gsp->input.pinch_scale_q16,
        };
        (void)gsp->callbacks.pinch_cb(gsp, &event, gsp->callbacks.pinch_user_ctx);
    }
}

static const gsp_multitouch_ops_t MULTITOUCH_OPS = {
    .cancel_single = multitouch_cancel_single,
    .pinch_begin = multitouch_pinch_begin,
    .pinch_update = multitouch_pinch_update,
    .pinch_end = multitouch_pinch_end,
};

void esp_gsp_input_init(gsp_ui_core_t *gsp)
{
    gsp_swipe_kinetics_policy_t swipe_policy =
        gsp_swipe_policy_from_config(&gsp->effective_config.policy);
    gsp_swipe_kinetics_configure(
        &gsp->nav.swipe_kinetics, &swipe_policy);
    gsp_swipe_kinetics_configure(
        &gsp->input.gesture_kinetics, &swipe_policy);
    gsp_pointer_init(&gsp->input.classifier, &POINTER_OPS, gsp,
                     GSP_POLICY(gsp, ESP_GSP_FIELD_TAP_SLOP_PX),
                     GSP_POLICY(gsp, ESP_GSP_FIELD_DRAG_START_PX));
    gsp_multitouch_init(
        &gsp->input.multitouch, &MULTITOUCH_OPS, gsp);
}

void esp_gsp_process_pointer(gsp_ui_core_t *gsp, int32_t touch_x,
                             int32_t touch_y, bool pressed)
{
    if (gsp->nav.swipe_kinetics.settling) {
        return;
    }
    if (pressed && !gsp_pointer_pressed(&gsp->input.classifier) &&
            gsp->input.adapter_visual_transition) {
        (void)finish_snapshotless_jump(gsp);
    }
    esp_gsp_input_reconcile(gsp);
    gsp_pointer_feed(&gsp->input.classifier, touch_x, touch_y, pressed);
    const gsp_pointer_t *pointer = &gsp->input.classifier;
    if (pressed && gsp->input.press_hit.found && !gsp->input.press_finished) {
        const gsp_rect_t *bounds = &gsp->input.press_hit.bbox;
        int32_t margin = pointer->tap_slop / 4;
        if (margin > 4) {
            margin = 4;
        }
        bool inside = touch_x >= bounds->x1 && touch_x < bounds->x2 &&
                      touch_y >= bounds->y1 && touch_y < bounds->y2;
        bool escaped = touch_x < (int64_t)bounds->x1 - margin || touch_x >= (int64_t)bounds->x2 + margin ||
                       touch_y < (int64_t)bounds->y1 - margin || touch_y >= (int64_t)bounds->y2 + margin;
        if (pointer->owner != GSP_POINTER_OWNER_CAPTURE &&
                (pointer->owner != GSP_POINTER_OWNER_HIT || pointer->tap_cancelled || escaped)) {
            cancel_press(gsp);
        } else if (pointer->owner == GSP_POINTER_OWNER_HIT) {
            if (!inside || gsp->input.press_outside) {
                gsp->input.long_press_since_us = gsp->now_us;
            }
            gsp->input.press_outside = !inside;
        }
    }
}

void esp_gsp_process_touch_frame(
    gsp_ui_core_t *gsp, const gsp_platform_touch_frame_t *frame)
{
    if (gsp->nav.swipe_kinetics.settling) {
        return;
    }
    if (gsp->callbacks.pinch_cb == NULL) {
        if (gsp->input.multitouch.tracking) {
            gsp_multitouch_cancel(&gsp->input.multitouch);
        }
        if (frame->count != 0) {
            esp_gsp_process_pointer(
                gsp, frame->contacts[0].x,
                frame->contacts[0].y, true);
        } else {
            esp_gsp_process_pointer(gsp, 0, 0, false);
        }
        return;
    }
    gsp_multitouch_feed(&gsp->input.multitouch, frame);
    if (gsp->input.multitouch.tracking ||
            gsp->input.multitouch.blocked_until_clear) {
        return;
    }
    if (frame->count != 0) {
        esp_gsp_process_pointer(
            gsp, frame->contacts[0].x,
            frame->contacts[0].y, true);
    } else {
        esp_gsp_process_pointer(gsp, 0, 0, false);
    }
}

void esp_gsp_snapshot_skip(gsp_ui_core_t *gsp, esp_gsp_snapshot_reason_t reason)
{
    if (reason > ESP_GSP_SNAPSHOT_NONE && reason < ESP_GSP_SNAPSHOT_REASON_COUNT) {
        ++gsp->chrome.drag_snapshot_stats.skipped[reason];
        gsp->chrome.drag_snapshot_stats.last_reason = reason;
    }
}

void esp_gsp_snapshot_backend_result(esp_gsp_handle_t gsp, int32_t error)
{
    if (gsp != NULL) {
        gsp->chrome.drag_snapshot_stats.last_backend_error = error;
        gsp->chrome.drag_snapshot_stats.last_reason = error == GSP_OK ? ESP_GSP_SNAPSHOT_NONE :
                (error == GSP_ERR_UNSUPPORTED ? ESP_GSP_SNAPSHOT_UNSUPPORTED :
                 error == GSP_ERR_LIMIT_EXCEEDED ? ESP_GSP_SNAPSHOT_BACKEND_LIMIT : ESP_GSP_SNAPSHOT_BACKEND_ERROR);
        if (error == GSP_ERR_LIMIT_EXCEEDED && esp_gsp_config_value(
                    &gsp->effective_config, ESP_GSP_FIELD_ENABLE_TRANSITION_SNAPSHOTS) == 0) {
            gsp->chrome.drag_snapshot_stats.last_reason = ESP_GSP_SNAPSHOT_DISABLED;
        }
    }
}

/* Mosaico product bridge: run on the UI task before a shade gesture. The
 * StackView's own group includes the underlying page but excludes the shade
 * and independent system notices. No runtime state or damage is modified. */
esp_gsp_err_t mosaic_gsp_capture_stack(esp_gsp_handle_t handle,
    uint32_t stack_key, void *pixels, uint16_t width, uint16_t height,
    size_t stride, size_t capacity)
{
    gsp_ui_core_t *gsp=handle;
    if (gsp==NULL || pixels==NULL || height==0 || stride<(size_t)width*2U ||
            stride>SIZE_MAX/height || capacity<stride*height) return GSP_ERR_INVALID_ARG;
    gsp_runtime_t *runtime=gsp->components.manager.runtime;
    const gsp_component_directory_t *directory=gsp->components.manager.directory;
    if (runtime==NULL || directory==NULL ||
            runtime->scene->color_profile!=GSP_FORMAT_COMMON_PIXEL_FORMAT_RGB565 ||
            width!=runtime->scene->screen_width || height!=runtime->scene->screen_height)
        return GSP_ERR_UNSUPPORTED;
    for (uint16_t i=0;i<directory->count;++i) {
        const gsp_component_entry_t *entry=&directory->entries[i];
        if (entry->key!=stack_key || entry->kind!=GSP_COMPONENT_KIND_STACK_VIEW ||
                entry->config==NULL) continue;
        gsp_component_instance_t *instance;
        gsp_err_t err=gsp_component_manager_activate(&gsp->components.manager,i,
                            ESP_GSP_DRIVER_STACK_VIEW,&instance);
        if (err!=GSP_OK) return err;
        if (gsp_stack_view_driver_animating(instance)) return GSP_ERR_INVALID_STATE;
        const gsp_stack_view_config_t *config=entry->config;
        memset(pixels,0,stride*height);
        gsp_surface_t surface={.pixels=pixels,.stride_bytes=stride,.width=width,
            .height=height,.pixel_format=GSP_FORMAT_COMMON_PIXEL_FORMAT_RGB565};
        return gsp_raster_render_transform_group(runtime,config->group_id,&surface);
    }
    return GSP_ERR_NOT_FOUND;
}
