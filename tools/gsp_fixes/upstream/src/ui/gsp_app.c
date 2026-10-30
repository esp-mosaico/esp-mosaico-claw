/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

/* Portable app core wrapper over the platform-free esp_gsp_ui slice.
 * ESP keeps its queue/task/display adapter in the ESP-IDF platform layer;
 * host/SDL/WASM drive this layer directly with synthetic time and input. */

#include "gsp/gsp_app.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gsp_ui_core_priv.h"
#include "gsp/gsp_image_cache.h"
#include "gsp_log_priv.h"

static const char *TAG = "gsp_app";

/* Product policy runs before application interceptors and hit testing. */
extern bool mosaic_ui_filter_pointer(bool pressed) __attribute__((weak));
static bool product_consumes_pointer(bool pressed)
{
    return mosaic_ui_filter_pointer != NULL && mosaic_ui_filter_pointer(pressed);
}

struct gsp_app {
    gsp_ui_core_t ui;
    uint32_t service_interval_ms;
    int32_t platform_touch_x;
    int32_t platform_touch_y;
    struct {
        bool active;
        int32_t id;
        int32_t x;
        int32_t y;
    } contacts[2];
};

static gsp_err_t map_ui_err(esp_gsp_err_t err)
{
    if (err == ESP_GSP_OK) {
        return GSP_OK;
    }
    /* The gsp layer expresses allocation failure as LIMIT_EXCEEDED (the
     * same code gsp_context_init returns on OOM). */
    if (err == ESP_GSP_ERR_NO_MEM) {
        return GSP_ERR_LIMIT_EXCEEDED;
    }
    if (err == ESP_GSP_ERR_NOT_FOUND) {
        return GSP_ERR_NOT_FOUND;
    }
    if (err == ESP_GSP_ERR_NOT_SUPPORTED) {
        return GSP_ERR_UNSUPPORTED;
    }
    return GSP_ERR_INVALID_ARG;
}

gsp_ui_core_t *gsp_app_ui(gsp_app_t *app)
{
    return app != NULL ? &app->ui : NULL;
}

int32_t gsp_app_font_outline_drivers(void)
{
#if GSP_ENABLE_FREETYPE
    return gsp_freetype_outline_drivers();
#else
    return 0;
#endif
}

size_t gsp_app_freetype_stack_requirement(const gsp_app_t *app)
{
#if GSP_ENABLE_FREETYPE
    if (app != NULL && app->ui.ttf_ready &&
            app->ui.dynamic_font_provider != NULL) {
        return app->ui.dynamic_font_provider->render_stack_requirement;
    }
#else
    (void)app;
#endif
    return 0;
}

static const gsp_ui_core_t *app_ui_const(const gsp_app_t *app)
{
    return app != NULL ? &app->ui : NULL;
}

gsp_app_t *gsp_app_from_ui(gsp_ui_core_t *ui)
{
    return ui != NULL ?
           (gsp_app_t *)((char *)ui - offsetof(gsp_app_t, ui)) : NULL;
}

void gsp_ui_core_attach_adapter(gsp_ui_core_t *core,
                                const gsp_app_adapter_ops_t *ops,
                                void *ctx)
{
    if (core != NULL) {
        if (core->adapter_ops != NULL &&
                (core->adapter_ops != ops || core->adapter_ctx != ctx)) {
            /* Viewport captures borrow this app's runtime and List rows.
             * Release them through the old adapter before it becomes
             * unreachable, including suspend/replace and app destruction. */
            esp_gsp_snapshot_content_changed_active(core, core->current_scene);
        }
        core->adapter_ops = ops;
        core->adapter_ctx = ctx;
    }
}

esp_gsp_handle_t gsp_app_handle(gsp_app_t *app)
{
    return app != NULL ? &app->ui : NULL;
}

gsp_app_t *gsp_app_from_handle(esp_gsp_handle_t handle)
{
    return gsp_app_from_ui(handle);
}

void gsp_app_attach_adapter(gsp_app_t *app,
                            const gsp_app_adapter_ops_t *ops,
                            void *ctx)
{
    gsp_ui_core_attach_adapter(gsp_app_ui(app), ops, ctx);
}

void *gsp_app_adapter_context(esp_gsp_handle_t handle,
                              const gsp_app_adapter_ops_t *expected_ops)
{
    if (handle == NULL || handle->adapter_ops != expected_ops) {
        return NULL;
    }
    return handle->adapter_ctx;
}

bool gsp_app_callback_mutation_blocked(esp_gsp_handle_t handle)
{
    return handle != NULL &&
           (atomic_load_explicit(&handle->canvas_draw_active, memory_order_relaxed) ||
            atomic_load_explicit(&handle->extension_callback_active, memory_order_relaxed));
}

void gsp_app_dispatch_command(gsp_app_t *app,
                              const esp_gsp_cmd_t *command)
{
    gsp_ui_core_t *core = gsp_app_ui(app);
    if (core != NULL && command != NULL) {
        esp_gsp_ui_apply(core, command);
    }
}

void gsp_app_prepare(gsp_app_t *app, int64_t now_us)
{
    gsp_ui_core_t *core = gsp_app_ui(app);
    if (core == NULL) {
        return;
    }
    esp_gsp_timer_slots_lock(core);
    core->now_us = now_us;
    esp_gsp_timer_slots_unlock(core);
    /* A suspended app releases decoded image surfaces to make room for the
     * temporary session app. Rebuild the current scene synchronously before
     * its first resumed frame; otherwise renderer cache misses publish
     * placeholders and the normal one-job decode pump makes images appear
     * incrementally. Initial startup and an already-resident scene are cheap
     * no-ops because images_prepared is already true. */
    gsp_ui_core_prepare_scene(core, core->current_scene);
    esp_gsp_asset_anims_open(core);
}

void gsp_app_sync_input_time(gsp_app_t *app, int64_t now_us)
{
    gsp_ui_core_t *core = gsp_app_ui(app);
    if (core != NULL) {
        esp_gsp_timer_slots_lock(core);
        core->now_us = now_us;
        esp_gsp_timer_slots_unlock(core);
    }
}

void gsp_app_frame_retired(gsp_app_t *app)
{
    gsp_ui_core_t *core = gsp_app_ui(app);
    if (core != NULL) {
        esp_gsp_canvas_fire_retired(core);
    }
}

bool gsp_app_updates_before_frame(gsp_app_t *app)
{
    gsp_ui_core_t *core = gsp_app_ui(app);
    if (core == NULL || !gsp_updates_available()) {
        return false;
    }
    gsp_updates_before_frame(core);
    return gsp_update_receipt_services.requires_present(core);
}

void gsp_app_updates_after_frame(gsp_app_t *app, gsp_err_t result,
                                 uint64_t frame_id, uint32_t generation)
{
    gsp_ui_core_t *core = gsp_app_ui(app);
    if (core != NULL) {
        gsp_updates_after_frame(core, result, frame_id, generation);
    }
}

void gsp_app_feed_platform_pointer(gsp_app_t *app,
                                   const gsp_platform_pointer_t *pointer)
{
    gsp_ui_core_t *core = gsp_app_ui(app);
    if (core == NULL || pointer == NULL) {
        return;
    }
    if (product_consumes_pointer(pointer->pressed)) {
        core->input.from_hw = false;
        return;
    }
    if (core->callbacks.input_interceptor_cb != NULL &&
            core->callbacks.input_interceptor_cb(
                gsp_app_handle(app), pointer->x, pointer->y,
                pointer->pressed, core->callbacks.input_interceptor_user_ctx)) {
        if (!pointer->pressed) {
            core->input.from_hw = false;
        }
        return;
    }
    if (pointer->pressed) {
        core->input.from_hw = true;
    }
    if (core->callbacks.pointer_observer_cb != NULL) {
        core->callbacks.pointer_observer_cb(gsp_app_handle(app), pointer->x,
                                            pointer->y, pointer->pressed,
                                            core->callbacks.pointer_observer_user_ctx);
    }
    if (core->input.from_hw) {
        esp_gsp_process_pointer(core, pointer->x, pointer->y,
                                pointer->pressed);
        if (!pointer->pressed) {
            core->input.from_hw = false;
        }
    }
}

void gsp_app_feed_platform_touch_frame(
    gsp_app_t *app, const gsp_platform_touch_frame_t *frame)
{
    if (app == NULL || frame == NULL ||
            frame->count > GSP_PLATFORM_MAX_TOUCH_POINTS) {
        return;
    }
    gsp_ui_core_t *core = gsp_app_ui(app);
    if (core == NULL) {
        return;
    }
    if (frame->count > 0) {
        app->platform_touch_x = frame->contacts[0].x;
        app->platform_touch_y = frame->contacts[0].y;
    }
    int32_t x = app->platform_touch_x;
    int32_t y = app->platform_touch_y;
    if (product_consumes_pointer(frame->count > 0)) {
        core->input.from_hw = false;
        return;
    }
    if (core->callbacks.input_interceptor_cb != NULL &&
            core->callbacks.input_interceptor_cb(
                gsp_app_handle(app), x, y, frame->count > 0,
                core->callbacks.input_interceptor_user_ctx)) {
        if (frame->count == 0) {
            core->input.from_hw = false;
        }
        return;
    }
    if (frame->count > 0) {
        core->input.from_hw = true;
    }
    if (core->callbacks.pointer_observer_cb != NULL) {
        core->callbacks.pointer_observer_cb(
                           gsp_app_handle(app), x, y, frame->count > 0,
                           core->callbacks.pointer_observer_user_ctx);
    }
    if (core->input.from_hw) {
        esp_gsp_process_touch_frame(core, frame);
        if (frame->count == 0) {
            core->input.from_hw = false;
        }
    }
}

/* Decoded imagery stays resident across scenes while the total fits
 * core->scene_retain_bytes, so navigating back to a visited scene costs no
 * decode. Over the cap the least recently visited inactive scenes go first,
 * which leaves a single-scene target behaving as it did without retention. */
static void scene_images_touch(gsp_ui_core_t *core, uint16_t scene)
{
    core->scene_used_at[scene] = ++core->scene_use_clock;
}

static void scene_images_trim(gsp_ui_core_t *core, uint16_t keep_a,
                              uint16_t keep_b)
{
    /* Pinned entries survive a reset, so a scene that stays non-empty must
     * not be picked again or the search would never terminate. */
    bool inline_released[32] = { false };
    bool *released = inline_released;
    if (core->scene_count > sizeof(inline_released) /
            sizeof(inline_released[0])) {
        released = calloc(core->scene_count, sizeof(*released));
        if (released == NULL) {
            GSP_LOGW(TAG, "Cannot allocate scene-retention trim scratch:"
                     " scenes=%u", core->scene_count);
            return;
        }
    }
    for (;;) {
        size_t resident = 0;
        uint16_t victim = gsp_effective_capacity(core, ESP_GSP_FIELD_MAX_SCENES);
        for (uint16_t scene = 0; scene < core->scene_count; ++scene) {
            if (!core->scene_ready[scene] ||
                    !core->scenes[scene].has_image_cache) {
                continue;
            }
            size_t used = core->scenes[scene].image_cache.bytes_used;
            resident += used;
            if (used == 0 || scene == keep_a || scene == keep_b ||
                    released[scene]) {
                continue;
            }
            if (victim >= gsp_effective_capacity(core, ESP_GSP_FIELD_MAX_SCENES) ||
                    core->scene_used_at[scene] <
                    core->scene_used_at[victim]) {
                victim = scene;
            }
        }
        if (resident <= core->scene_retain_bytes ||
                victim >= gsp_effective_capacity(core, ESP_GSP_FIELD_MAX_SCENES)) {
            break;
        }
        gsp_context_t *context = &core->scenes[victim];
        GSP_LOGI(TAG, "releasing inactive scene images: scene=%u bytes=%zu"
                 " resident=%zu cap=%zu", victim,
                 context->image_cache.bytes_used, resident,
                 core->scene_retain_bytes);
        released[victim] = true;
        gsp_image_cache_reset(&context->image_cache);
        context->images_prepared = false;
    }
    if (released != inline_released) {
        free(released);
    }
}

/* Best effort: whatever fails to decode renders as a placeholder rather than
 * blocking navigation. */
void gsp_ui_core_prepare_scene(gsp_ui_core_t *core, uint16_t target)
{
    if (core == NULL || target >= core->scene_count ||
            !core->scene_ready[target]) {
        return;
    }
    scene_images_touch(core, target);
    /* Reclaim from scenes that are neither on screen nor about to be. The
     * two exempt scenes are both resident on purpose: a transition
     * composites the outgoing and incoming scene at the same time. */
    scene_images_trim(core, core->current_scene, target);
    gsp_err_t ret = gsp_context_prepare_images(&core->scenes[target]);
    if (ret != GSP_OK) {
        GSP_LOGE(TAG, "scene image preparation failed: target=%u error=%d",
                 target, (int)ret);
    }
}

bool gsp_press_feed(gsp_press_feed_t *feed, gsp_app_t *app,
                    const gsp_platform_pointer_t *sample)
{
    if (feed == NULL || app == NULL || sample == NULL) {
        return false;
    }
    if (sample->pressed) {
        feed->active = true;
    }
    if (!feed->active) {
        return false;
    }
    gsp_app_feed_platform_pointer(app, sample);
    if (!sample->pressed) {
        feed->active = false;
    }
    return true;
}

bool gsp_press_feed_touch_frame(gsp_press_feed_t *feed,
                                gsp_app_t *app,
                                const gsp_platform_touch_frame_t *frame)
{
    if (feed == NULL || app == NULL || frame == NULL) {
        return false;
    }
    if (frame->count > 0) {
        feed->active = true;
    }
    if (!feed->active) {
        return false;
    }
    gsp_app_feed_platform_touch_frame(app, frame);
    if (frame->count == 0) {
        feed->active = false;
    }
    return true;
}

void gsp_ui_core_switch_scene(gsp_ui_core_t *core, uint16_t target)
{
    if (core == NULL || target >= core->scene_count ||
            !core->scene_ready[target] || target == core->current_scene) {
        return;
    }
    GSP_LOGI(TAG, "scene switch: current=%u target=%u",
             core->current_scene, target);
    if (core->input.multitouch.tracking) {
        gsp_multitouch_cancel(&core->input.multitouch);
    }
    esp_gsp_input_scene_exit(core);
    uint16_t leaving = core->current_scene;
    esp_gsp_components_scene_switch(core, target);
    esp_gsp_canvas_close_scene(core, leaving);
    /* Complete the target before it becomes current so its first frame
     * renders whole. Callers that animate the switch have already prepared
     * it — the transition snapshots the target — and this is then a no-op. */
    gsp_ui_core_prepare_scene(core, target);
    core->current_scene = target;
    atomic_store_explicit(&core->asset_scene, target, memory_order_release);
    /* A scene ID alone cannot distinguish leaving and returning before a
     * pending file read or image decode finishes. Publish the ID first. */
    atomic_fetch_add_explicit(&core->asset_scene_generation, 1, memory_order_release);
    gsp_updates_scene_changed(core);
    esp_gsp_image_cancel_scene_assets(core, leaving);
    /* Scene contexts track their own incremental damage.  Damage from the
     * outgoing context cannot describe pixels which disappear in the target
     * scene, so a plain scene replacement must start with a full viewport
     * repaint.  Without this, retained/partition presenters preserve the
     * outgoing scene anywhere the incoming scene has no object damage. */
    gsp_runtime_add_dirty(&core->scenes[target].runtime, (gsp_rect_t) {
        .x1 = 0,
        .y1 = 0,
        .x2 = core->scenes[target].scene.screen_width,
        .y2 = core->scenes[target].scene.screen_height,
    });
    /* The scene just left keeps whatever it decoded, but must re-verify on
     * return: its own LRU can still drop entries to make room for runtime
     * images while it is off screen, and a scene may only come back on
     * screen with a complete working set. */
    if (leaving < core->scene_count && core->scene_ready[leaving]) {
        core->scenes[leaving].images_prepared = false;
    }
    scene_images_trim(core, target, target);
    esp_gsp_asset_anims_close(core);
    esp_gsp_asset_anims_open(core);
    /* App contributors outlive scenes; attach their first-frame commands to
     * the new runtime without waiting for pointer or caret activity. */
    esp_gsp_overlay_rebuild(core);
    const esp_gsp_extension_event_t extension = {
        .struct_size = sizeof(extension),
        .kind = ESP_GSP_EXTENSION_SCENE_CHANGED,
        .data.scene = {.previous = leaving, .current = target},
    };
    gsp_extension_emit(core, &extension);
    if (core->callbacks.event_cb != NULL) {
        esp_gsp_event_t event = {
            .type = ESP_GSP_EVENT_SCENE_CHANGED,
            .scene_id = target,
        };
        core->callbacks.event_cb(core, &event, core->callbacks.event_user_ctx);
    }
}

gsp_err_t gsp_app_create(const gsp_app_config_t *config,
                         gsp_app_t **out_app)
{
    if (config == NULL || out_app == NULL || config->bundle == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    gsp_app_t *app = calloc(1, sizeof(*app));
    if (app == NULL) {
        return GSP_ERR_IO;
    }
    atomic_init(&app->ui.update_receipts, NULL);
    atomic_init(&app->ui.asset_scene, 0);
    atomic_init(&app->ui.asset_scene_generation, 0);
    atomic_init(&app->ui.extension_callback_active, false);
    atomic_init(&app->ui.update_error_busy, false);
    atomic_init(&app->ui.update_error_count, 0);
    atomic_init(&app->ui.update_error_dropped, 0);
#if GSP_PROFILE_SERVICE
    gsp_property_probe_init(&app->ui.property_probe);
#endif
    esp_gsp_err_t ret = esp_gsp_ui_init(&app->ui, config);
    if (ret != ESP_GSP_OK) {
        /* Init can fail after loading scenes, image caches and font state,
         * so the partial core needs a full teardown, not just a free. Every
         * step is guarded by the counters init fills in as it goes. */
        gsp_app_destroy(app);
        return map_ui_err(ret);
    }
    app->service_interval_ms = config->idle_poll_ms != 0
                               ? config->idle_poll_ms
                               : (uint32_t)esp_gsp_config_value(
                                   &app->ui.effective_config,
                                   config->use_pointer_poll
                                   ? ESP_GSP_FIELD_POINTER_POLL_MS
                                   : ESP_GSP_FIELD_IDLE_POLL_MS);
    *out_app = app;
    app->ui.extension_lifecycle_ready = true;
    const esp_gsp_extension_event_t extension = {
        .struct_size = sizeof(extension),
        .kind = ESP_GSP_EXTENSION_APP_CREATED,
    };
    gsp_extension_emit(&app->ui, &extension);
    return GSP_OK;
}

const esp_gsp_effective_config_t *gsp_app_effective_config(
    const gsp_app_t *app)
{
    return app != NULL ? &app->ui.effective_config : NULL;
}

void gsp_app_destroy(gsp_app_t *app)
{
    if (app == NULL) {
        return;
    }
    gsp_ui_core_t *gsp = &app->ui;
    gsp->shutting_down = true;
    if (gsp->adapter_ops != NULL) {
        /* Retire borrowed snapshots before freeing scenes, keeping the
         * adapter available to the remaining destruction callbacks. */
        esp_gsp_snapshot_content_changed_active(gsp, gsp->current_scene);
    }
    esp_gsp_input_scene_exit(gsp);
    if (gsp->extension_lifecycle_ready) {
        gsp->extension_lifecycle_ready = false;
        const esp_gsp_extension_event_t extension = {
            .struct_size = sizeof(extension),
            .kind = ESP_GSP_EXTENSION_APP_DESTROYING,
        };
        gsp_extension_emit(gsp, &extension);
    }
    esp_gsp_canvas_close_all(gsp);
    gsp_updates_abort(gsp);
    esp_gsp_image_close_all(gsp);
    if (gsp->components.instances != NULL) {
        for (size_t index = 0; index < gsp_effective_capacity(
                    gsp, ESP_GSP_FIELD_COMPONENT_INSTANCES); ++index) {
            gsp_component_manager_deactivate(&gsp->components.manager,
                                             &gsp->components.instances[index]);
        }
    }
    if (gsp->chrome.overlay.runtime != NULL) {
        gsp_overlay_deinit(&gsp->chrome.overlay);
    }
    if (gsp->text_slots != NULL) {
        for (size_t index = 0; index < gsp->text_slot_count; ++index) {
            if (gsp->text_slots[index].run_buffer != NULL &&
                    gsp->text_slots[index].run_buffer !=
                    gsp->text_slots[index].inline_run_buffer) {
                free(gsp->text_slots[index].run_buffer);
            }
        }
    }
    esp_gsp_dropdown_deinit(gsp);
    free(gsp->text_slots);
    if (gsp->kb.text != NULL && gsp->kb.text != gsp->kb.inline_text) {
        free(gsp->kb.text);
    }
    if (gsp->chrome.cursor_text != NULL &&
            gsp->chrome.cursor_text != gsp->chrome.cursor_inline_text) {
        free(gsp->chrome.cursor_text);
    }
    if (gsp->lists.pool != NULL) {
        for (size_t index = 0; index < gsp_effective_capacity(
                    gsp, ESP_GSP_FIELD_MAX_LISTS); ++index) {
            free(gsp->lists.pool[index].run_buffers);
            free(gsp->lists.pool[index].run_used);
            free(gsp->lists.pool[index].item_offsets);
            free(gsp->lists.pool[index].message_layouts);
            free(gsp->lists.pool[index].message_layout_staging);
            free(gsp->lists.pool[index].message_offset_staging);
            free(gsp->lists.pool[index].message_changed);
            free(gsp->lists.pool[index].message_shape_scratch);
        }
    }
    if (gsp->frame_anim_open) {
        gsp_anim_close(&gsp->frame_anim);
    }
    esp_gsp_asset_anims_close(gsp);
    gsp_codec_release_scratch(true);
#if GSP_ENABLE_FREETYPE
    esp_gsp_text_deinit_dynamic(gsp);
#endif
    if (gsp->scenes != NULL) {
        for (uint16_t scene = 0; scene < gsp->scene_count; ++scene) {
            gsp_context_deinit(&gsp->scenes[scene]);
        }
    }
#if GSP_ENABLE_FREETYPE
    /* The scene providers borrow these chains until context teardown. */
    free(gsp->font_chains);
#endif
    free(gsp->asset_anims);
    free(gsp->animations);
    free(gsp->widgets);
    free(gsp->canvas_slots);
    free(gsp->image_slots);
    free(gsp->lists.pool);
    free(gsp->chrome.storage);
    free(gsp->components.list_extensions);
    free(gsp->components.extensions);
    free(gsp->components.transaction_updates);
    free(gsp->components.instances);
    free(gsp->timers);
    free(gsp->scene_used_at);
    free(gsp->scene_ready);
    free(gsp->scenes);
    if (gsp->platform_config_acquired) {
        gsp_platform_config_release();
        gsp->platform_config_acquired = false;
    }
    free(app);
}

gsp_err_t gsp_app_apply(gsp_app_t *app,
                        const gsp_app_command_t *command)
{
    gsp_ui_core_t *gsp = gsp_app_ui(app);
    if (gsp == NULL || command == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    esp_gsp_cmd_t ui_command = { 0 };
    switch (command->type) {
    case GSP_APP_CMD_SET_VALUE:
        ui_command.type = CMD_SET_VALUE;
        ui_command.bind = command->target;
        ui_command.value = command->value;
        break;
    case GSP_APP_CMD_SET_COLOR:
        ui_command.type = CMD_SET_COLOR;
        ui_command.bind = command->target;
        ui_command.value = command->value;
        break;
    case GSP_APP_CMD_SET_VISIBLE:
        ui_command.type = CMD_SET_VISIBLE;
        ui_command.bind = command->target;
        ui_command.value = command->value != 0 ? 1U : 0U;
        break;
    case GSP_APP_CMD_SET_TEXT:
        if (command->text == NULL) {
            return GSP_ERR_INVALID_ARG;
        }
        ui_command.type = CMD_SET_TEXT;
        ui_command.bind = command->target;
        ui_command.flags = ESP_GSP_CMD_FLAG_TEXT_POINTER;
        ui_command.pointer = command->text;
        ui_command.pointer_size = strlen(command->text);
        break;
    case GSP_APP_CMD_GOTO_SCENE:
        ui_command.type = CMD_GOTO_SCENE;
        ui_command.bind = command->target;
        ui_command.value = command->value;
        break;
    default:
        return GSP_ERR_INVALID_ARG;
    }
    esp_gsp_ui_apply(gsp, &ui_command);
    return GSP_OK;
}

gsp_err_t gsp_app_resolve_navigation(gsp_app_t *app)
{
    gsp_ui_core_t *gsp = gsp_app_ui(app);
    if (gsp->nav.pending_goto >= 0 &&
            !gsp->nav.swipe_kinetics.settling) {
        uint16_t target = (uint16_t)gsp->nav.pending_goto;
        gsp->nav.pending_goto = -1;
        GSP_LOGI(TAG,
                 "pending goto consumed: current=%u target=%u transition=%u",
                 gsp->current_scene, target, gsp->nav.pending_transition);
        if (target < gsp->scene_count && gsp->scene_ready[target] &&
                target != gsp->current_scene) {
            gsp_ui_core_prepare_scene(gsp, target);
            /* ESP_GSP_NO_TRANSITION asks for a plain switch, so it never
             * becomes a request. That is what lets the adapter treat every
             * kind it does receive as one it is expected to render, rather
             * than having to recognize a sentinel that is not a kind. */
            bool wants_visual =
                gsp->nav.pending_transition != ESP_GSP_NO_TRANSITION;
            gsp_visual_transition_request_t request = {
                .source = GSP_VISUAL_SOURCE_SCENE,
                .kind = gsp->nav.pending_transition,
                .scene.target_scene = target,
            };
            bool visual = wants_visual &&
                          gsp->adapter_ops != NULL &&
                          gsp->adapter_ops->visual_transition_begin != NULL &&
                          gsp->adapter_ops->visual_transition_begin(
                              gsp->adapter_ctx, gsp, &request);
            if (visual &&
                    gsp->adapter_ops->visual_transition_end != NULL) {
                gsp->adapter_ops->visual_transition_end(
                    gsp->adapter_ctx, true);
            }
            gsp_ui_core_switch_scene(gsp, target);
        }
    }
    return GSP_OK;
}

void gsp_app_step(gsp_app_t *app, int64_t now_us)
{
    gsp_ui_core_t *gsp = gsp_app_ui(app);
    if (gsp == NULL) {
        return;
    }
    esp_gsp_timer_slots_lock(gsp);
    gsp->now_us = now_us;
    esp_gsp_timer_slots_unlock(gsp);
    if (gsp->nav.pending_goto >= 0) {
        (void)gsp_app_resolve_navigation(app);
    }
    esp_gsp_ui_step(gsp);
    gsp_updates_service(gsp);
}

int64_t gsp_app_next_deadline_us(const gsp_app_t *app)
{
    const gsp_ui_core_t *gsp = app_ui_const(app);
    if (gsp == NULL) {
        return 0;
    }
    bool active = gsp->procedural_active || gsp_pointer_pressed(&gsp->input.classifier) ||
                  gsp_pointer_swiping(&gsp->input.classifier) ||
                  gsp->input.multitouch.tracking ||
                  gsp_component_manager_active(&gsp->components.manager) ||
                  (gsp->frame_anim_open && !gsp->frame_anim.finished) ||
                  gsp->nav.swipe_kinetics.settling ||
                  gsp->nav.pending_goto >= 0 ||
                  gsp_updates_active(gsp) ||
                  gsp_context_decode_pending(
                      &gsp->scenes[gsp->current_scene]);
    /* Completed players still own the visible last frame, but no longer
     * need the active service cadence. Keep their buffers until normal close. */
    for (uint8_t index = 0; !active && index < gsp->asset_anim_count; ++index) {
        active = !gsp->asset_anims[index].finished;
    }
    int64_t animation_deadline = INT64_MAX;
    for (size_t index = 0; !active && index < gsp_effective_capacity(gsp, ESP_GSP_FIELD_MAX_ANIMATIONS);
            ++index) {
        const esp_gsp_anim_slot_t *animation = &gsp->animations[index];
        if (animation->active && !animation->paused) {
            if (animation->configured && animation->start_us > gsp->now_us) {
                if (animation->start_us < animation_deadline) {
                    animation_deadline = animation->start_us;
                }
            } else {
                active = true;
            }
        }
    }
    if (active) {
        return gsp->now_us + (int64_t)esp_gsp_config_value(
                   &gsp->effective_config, ESP_GSP_FIELD_ACTIVE_TICK_MS) * 1000;
    }
    int64_t deadline = gsp->now_us +
                       (int64_t)app->service_interval_ms * 1000;
    if (animation_deadline < deadline) {
        deadline = animation_deadline;
    }
    esp_gsp_timer_slots_lock(gsp);
    for (size_t index = 0; index < gsp_effective_capacity(gsp, ESP_GSP_FIELD_MAX_TIMERS); ++index) {
        if (gsp->timers[index].active &&
                gsp->timers[index].next_us < deadline) {
            deadline = gsp->timers[index].next_us;
        }
    }
    esp_gsp_timer_slots_unlock(gsp);
    if (gsp->chrome.cursor_bind != ESP_GSP_NO_CURSOR &&
            gsp->chrome.cursor_next_us < deadline) {
        deadline = gsp->chrome.cursor_next_us;
    }
    return deadline;
}

void gsp_app_feed_pointer(gsp_app_t *app, int32_t x, int32_t y,
                          bool pressed)
{
    gsp_ui_core_t *gsp = gsp_app_ui(app);
    if (gsp != NULL) {
        esp_gsp_process_pointer(gsp, x, y, pressed);
    }
}

bool gsp_app_has_active_contacts(const gsp_app_t *app)
{
    return app != NULL && (app->contacts[0].active || app->contacts[1].active);
}

void gsp_app_feed_touch(gsp_app_t *app, int32_t id, int32_t x, int32_t y,
                        bool pressed)
{
    if (app == NULL) {
        return;
    }
    int slot = -1;
    for (int index = 0; index < 2; ++index) {
        if (app->contacts[index].active && app->contacts[index].id == id) {
            slot = index;
            break;
        }
    }
    if (slot < 0 && pressed) {
        for (int index = 0; index < 2; ++index) {
            if (!app->contacts[index].active) {
                slot = index;
                app->contacts[index].id = id;
                break;
            }
        }
    }
    if (slot < 0) {
        return;
    }
    app->contacts[slot].x = x;
    app->contacts[slot].y = y;
    app->contacts[slot].active = pressed;
    int count = 0;
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    for (int index = 0; index < 2; ++index) {
        if (app->contacts[index].active) {
            sum_x += app->contacts[index].x;
            sum_y += app->contacts[index].y;
            ++count;
        }
    }
    if (count > 0) {
        gsp_app_feed_pointer(app, (int32_t)(sum_x / count),
                             (int32_t)(sum_y / count), true);
    } else {
        gsp_app_feed_pointer(app, x, y, false);
    }
}

uint16_t gsp_app_current_scene(const gsp_app_t *app)
{
    const gsp_ui_core_t *gsp = app_ui_const(app);
    return gsp != NULL ? gsp->current_scene : 0;
}

uint16_t gsp_app_scene_count(const gsp_app_t *app)
{
    const gsp_ui_core_t *gsp = app_ui_const(app);
    return gsp != NULL ? gsp->scene_count : 0;
}

gsp_context_t *gsp_app_current_context(gsp_app_t *app)
{
    gsp_ui_core_t *gsp = gsp_app_ui(app);
    return gsp != NULL ? esp_gsp_current_ctx(gsp) : NULL;
}

gsp_context_t *gsp_app_scene_context(gsp_app_t *app, uint16_t scene)
{
    gsp_ui_core_t *gsp = gsp_app_ui(app);
    if (gsp == NULL || scene >= gsp->scene_count ||
            !gsp->scene_ready[scene]) {
        return NULL;
    }
    return &gsp->scenes[scene];
}

void gsp_app_release_image_caches(gsp_app_t *app)
{
    if (app == NULL) {
        return;
    }
    gsp_codec_release_scratch(false);
    for (uint16_t scene = 0; scene < gsp_app_scene_count(app); ++scene) {
        gsp_context_t *context = gsp_app_scene_context(app, scene);
        if (context != NULL && context->has_image_cache) {
            gsp_image_cache_reset(&context->image_cache);
            /* Keep the logical preparation state consistent with the cache.
             * The next activation must not mistake an empty cache for a scene
             * whose complete static image set is still resident. */
            context->images_prepared = false;
        }
    }
}

bool gsp_app_transition(const gsp_app_t *app,
                        gsp_app_transition_t *out_transition)
{
    const gsp_ui_core_t *gsp = app_ui_const(app);
    if (gsp == NULL || out_transition == NULL) {
        return false;
    }
    if (gsp->nav.drag_fallback) {
        *out_transition = (gsp_app_transition_t) {
            .active = true,
            .from_scene = gsp->current_scene,
            .to_scene = gsp->nav.drag_target,
            .kind = (gsp_app_transition_kind_t)gsp->nav.drag_kind,
            .progress = gsp->nav.drag_progress,
            .fade_black_point_percent =
                gsp->nav.swipe_fade_black_point_percent,
        };
        return true;
    }
    memset(out_transition, 0, sizeof(*out_transition));
    return false;
}
