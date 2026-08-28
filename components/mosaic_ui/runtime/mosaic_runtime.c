/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_runtime.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    bool active;
    bool repeat;
    char id[MOSAIC_RUNTIME_TIMER_ID_MAX + 1U];
    uint32_t period_ms;
    uint32_t sequence;
    uint32_t revision;
    int64_t next_us;
} mosaic_runtime_timer_t;

struct mosaic_runtime_t {
    mosaic_runtime_config_t config;
    const mosaic_app_descriptor_t* active;
    const mosaic_app_descriptor_t* pending;
    const mosaic_app_descriptor_t* history[MOSAIC_RUNTIME_APP_HISTORY_MAX];
    size_t history_count;
    bool pending_push_history;
    bool pending_pop_history;
    mosaic_platform_app_handle_t platform_app;
    int64_t now_us;
    esp_err_t pending_error;
    uint32_t timer_revision;
    bool stop_dispatched;
    mosaic_runtime_timer_t timers[MOSAIC_RUNTIME_MAX_TIMERS];
};

static bool platform_valid(const mosaic_platform_ops_t* platform)
{
    return platform != NULL && platform->open_app != NULL
        && platform->close_app != NULL && platform->step_app != NULL
        && platform->dispatch_event != NULL && platform->feed_pointer != NULL;
}

static void clear_timers(mosaic_runtime_handle_t runtime)
{
    memset(runtime->timers, 0, sizeof(runtime->timers));
}

static esp_err_t dispatch(
    mosaic_runtime_handle_t runtime, const mosaic_event_t* event)
{
    if (runtime->platform_app == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return runtime->config.platform->dispatch_event(
        runtime->config.platform_ctx, runtime->platform_app, event);
}

static void on_platform_ui_event(void* user_ctx, const esp_gsp_event_t* event)
{
    mosaic_runtime_handle_t runtime = user_ctx;
    if (runtime == NULL || event == NULL || runtime->active == NULL
        || runtime->platform_app == NULL) {
        return;
    }

    mosaic_event_t runtime_event = {
        .timestamp_us = runtime->now_us,
    };
    if (event->type == ESP_GSP_EVENT_CALL) {
        runtime_event.type = MOSAIC_EVENT_UI_CALL;
        runtime_event.data.call.action_id = event->action_id;
        runtime_event.data.call.arg = event->arg;
        runtime_event.data.call.scene_id = event->scene_id;
        runtime_event.data.call.list = event->list;
        runtime_event.data.call.item = event->item;
        const mosaic_app_descriptor_t* pending_before = runtime->pending;
        esp_err_t err = dispatch(runtime, &runtime_event);
        if (err != ESP_OK && runtime->pending_error == ESP_OK) {
            runtime->pending_error = err;
        }

        const mosaic_app_descriptor_t* target = NULL;
        if (event->action_id == runtime->active->back_action &&
                runtime->history_count != 0) {
            runtime->pending = runtime->history[runtime->history_count - 1U];
            runtime->pending_pop_history = true;
            return;
        }
        if (runtime->config.system_flow != NULL &&
                event->action_id == runtime->active->back_action) {
            bool handled = false;
            const char* next_app = NULL;
            err = mosaic_system_flow_handle_exit(
                runtime->config.system_flow, runtime->active->name,
                &handled, &next_app);
            if (err != ESP_OK) {
                if (runtime->pending_error == ESP_OK) {
                    runtime->pending_error = err;
                }
                return;
            }
            if (handled) {
                runtime->pending = mosaic_app_descriptor_for_name(next_app);
                if (runtime->pending == NULL &&
                        runtime->pending_error == ESP_OK) {
                    runtime->pending_error = ESP_ERR_NOT_FOUND;
                }
                return;
            }
        }
        /* An App callback may deliberately choose a more specific return
         * target than the descriptor's generic Back route. Preserve that
         * explicit request instead of overwriting it below. */
        if (runtime->pending != pending_before) {
            return;
        }
        if (mosaic_app_route_event(runtime->active, event, &target)) {
            if (!runtime->active->routes_without_history &&
                    runtime->active != mosaic_app_root() &&
                    target != mosaic_app_root()) {
                if (runtime->history_count >= MOSAIC_RUNTIME_APP_HISTORY_MAX) {
                    runtime->pending_error = ESP_ERR_INVALID_STATE;
                    return;
                }
                runtime->history[runtime->history_count++] = runtime->active;
                runtime->pending_push_history = true;
            }
            runtime->pending = target;
        }
        return;
    }
    if (event->type == ESP_GSP_EVENT_SCENE_CHANGED) {
        runtime_event.type = MOSAIC_EVENT_SCENE_CHANGED;
        runtime_event.data.scene.scene_id = event->scene_id;
        esp_err_t err = dispatch(runtime, &runtime_event);
        if (err != ESP_OK && runtime->pending_error == ESP_OK) {
            runtime->pending_error = err;
        }
    }
}

static esp_err_t open_app(
    mosaic_runtime_handle_t runtime, const mosaic_app_descriptor_t* descriptor)
{
    mosaic_platform_app_handle_t platform_app = NULL;
    esp_err_t err
        = runtime->config.platform->open_app(runtime->config.platform_ctx,
            descriptor, on_platform_ui_event, runtime, &platform_app);
    if (err != ESP_OK) {
        if (platform_app != NULL) {
            (void)runtime->config.platform->close_app(
                runtime->config.platform_ctx, platform_app);
        }
        return err;
    }
    if (platform_app == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    runtime->platform_app = platform_app;
    runtime->active = descriptor;
    runtime->pending = NULL;
    clear_timers(runtime);

    const mosaic_app_package_t* package
        = mosaic_app_package_for_descriptor(descriptor);
    if (package != NULL && package->tick_ms != 0) {
        mosaic_runtime_timer_t* timer = &runtime->timers[0];
        timer->active = true;
        timer->repeat = true;
        memcpy(timer->id, "app_tick", sizeof("app_tick"));
        timer->period_ms = package->tick_ms;
        timer->revision = ++runtime->timer_revision;
        timer->next_us
            = runtime->now_us + (int64_t)package->tick_ms * 1000;
    }

    const mosaic_event_t event = {
        .type = MOSAIC_EVENT_START,
        .timestamp_us = runtime->now_us,
    };
    err = dispatch(runtime, &event);
    if (err != ESP_OK) {
        esp_err_t close_err = runtime->config.platform->close_app(
            runtime->config.platform_ctx, runtime->platform_app);
        if (close_err == ESP_OK) {
            runtime->platform_app = NULL;
            runtime->active = NULL;
        } else {
            err = close_err;
        }
    }
    return err;
}

static esp_err_t replace_app(mosaic_runtime_handle_t runtime,
    const mosaic_app_descriptor_t* descriptor)
{
    mosaic_platform_app_handle_t platform_app = NULL;
    esp_err_t err = runtime->config.platform->replace_app(
        runtime->config.platform_ctx, runtime->platform_app, descriptor,
        on_platform_ui_event, runtime, &platform_app);
    if (err != ESP_OK || platform_app == NULL) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    runtime->platform_app = platform_app;
    runtime->active = descriptor;
    runtime->pending = NULL;
    runtime->stop_dispatched = false;
    clear_timers(runtime);
    const mosaic_event_t event = {
        .type = MOSAIC_EVENT_START,
        .timestamp_us = runtime->now_us,
    };
    return dispatch(runtime, &event);
}

static esp_err_t close_app(mosaic_runtime_handle_t runtime)
{
    if (runtime->platform_app == NULL) {
        runtime->active = NULL;
        runtime->pending = NULL;
        runtime->stop_dispatched = false;
        clear_timers(runtime);
        return ESP_OK;
    }
    if (!runtime->stop_dispatched) {
        const mosaic_event_t event = {
            .type = MOSAIC_EVENT_STOP,
            .timestamp_us = runtime->now_us,
        };
        esp_err_t err = dispatch(runtime, &event);
        if (err != ESP_OK) {
            return err;
        }
        runtime->stop_dispatched = true;
    }
    esp_err_t err = runtime->config.platform->close_app(
        runtime->config.platform_ctx, runtime->platform_app);
    if (err != ESP_OK) {
        return err;
    }
    runtime->platform_app = NULL;
    runtime->active = NULL;
    runtime->pending = NULL;
    runtime->stop_dispatched = false;
    clear_timers(runtime);
    return ESP_OK;
}

esp_err_t mosaic_runtime_create(
    const mosaic_runtime_config_t* config, mosaic_runtime_handle_t* ret_runtime)
{
    if (ret_runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_runtime = NULL;
    if (config == NULL || !platform_valid(config->platform)) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaic_runtime_handle_t runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) {
        return ESP_ERR_NO_MEM;
    }
    runtime->config = *config;
    *ret_runtime = runtime;
    return ESP_OK;
}

void mosaic_runtime_delete(mosaic_runtime_handle_t runtime)
{
    if (runtime == NULL) {
        return;
    }
    if (close_app(runtime) != ESP_OK) {
        return;
    }
    free(runtime);
}

esp_err_t mosaic_runtime_start(
    mosaic_runtime_handle_t runtime, const char* initial_app)
{
    if (runtime == NULL || initial_app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (runtime->platform_app != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    runtime->history_count = 0;
    runtime->pending_push_history = false;
    runtime->pending_pop_history = false;
    const mosaic_app_descriptor_t* descriptor
        = mosaic_app_descriptor_for_name(initial_app);
    return descriptor != NULL ? open_app(runtime, descriptor)
                              : ESP_ERR_NOT_FOUND;
}

esp_err_t mosaic_runtime_stop(mosaic_runtime_handle_t runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = close_app(runtime);
    if (err == ESP_OK) {
        runtime->history_count = 0;
        runtime->pending_push_history = false;
        runtime->pending_pop_history = false;
    }
    return err;
}

static esp_err_t process_timers(mosaic_runtime_handle_t runtime)
{
    for (size_t index = 0; index < MOSAIC_RUNTIME_MAX_TIMERS; ++index) {
        mosaic_runtime_timer_t* timer = &runtime->timers[index];
        if (!timer->active || runtime->now_us < timer->next_us) {
            continue;
        }
        const mosaic_event_t event = {
            .type = MOSAIC_EVENT_TIMER,
            .timestamp_us = runtime->now_us,
            .data.timer = {
                .id = timer->id,
                .sequence = timer->sequence++,
            },
        };
        const uint32_t revision = timer->revision;
        esp_err_t err = dispatch(runtime, &event);
        if (err != ESP_OK) {
            return err;
        }
        if (timer->revision != revision) {
            continue;
        }
        if (!timer->repeat) {
            timer->active = false;
        } else {
            const int64_t period_us = (int64_t)timer->period_ms * 1000;
            do {
                timer->next_us += period_us;
            } while (timer->next_us <= runtime->now_us);
        }
    }
    return ESP_OK;
}

esp_err_t mosaic_runtime_step(mosaic_runtime_handle_t runtime, int64_t now_us)
{
    if (runtime == NULL || runtime->platform_app == NULL
        || now_us < runtime->now_us) {
        return ESP_ERR_INVALID_STATE;
    }
    runtime->now_us = now_us;
    if (runtime->pending_error != ESP_OK) {
        esp_err_t err = runtime->pending_error;
        runtime->pending_error = ESP_OK;
        return err;
    }
    if (runtime->pending != NULL && runtime->pending != runtime->active) {
        const mosaic_app_descriptor_t* target = runtime->pending;
        const mosaic_app_descriptor_t* previous = runtime->active;
        const bool pushed_history = runtime->pending_push_history;
        const bool popped_history = runtime->pending_pop_history;
        const bool direct_child_transition =
            runtime->config.platform->replace_app != NULL &&
            previous != mosaic_app_root() && target != mosaic_app_root();
        esp_err_t err;
        bool target_open_attempted = false;
        if (direct_child_transition) {
            const mosaic_event_t stop_event = {
                .type = MOSAIC_EVENT_STOP,
                .timestamp_us = runtime->now_us,
            };
            err = dispatch(runtime, &stop_event);
            if (err == ESP_OK) {
                target_open_attempted = true;
                err = replace_app(runtime, target);
            }
        } else {
            err = close_app(runtime);
            if (err == ESP_OK) {
                target_open_attempted = true;
                err = open_app(runtime, target);
            }
        }
        if (err != ESP_OK) {
            if (!target_open_attempted) {
                return err;
            }
            if (pushed_history && runtime->history_count != 0) {
                runtime->history_count--;
            }
            runtime->pending_push_history = false;
            runtime->pending_pop_history = false;
            esp_err_t fallback_err = open_app(runtime, previous);
            if (fallback_err != ESP_OK && previous != mosaic_app_root()) {
                (void)open_app(runtime, mosaic_app_root());
            }
            return err;
        }
        if (popped_history && runtime->history_count != 0) {
            runtime->history_count--;
        }
        runtime->pending_push_history = false;
        runtime->pending_pop_history = false;
    }
    esp_err_t err = process_timers(runtime);
    if (err != ESP_OK) {
        return err;
    }
    return runtime->config.platform->step_app(
        runtime->config.platform_ctx, runtime->platform_app, now_us);
}

esp_err_t mosaic_runtime_request_app(
    mosaic_runtime_handle_t runtime, const char* name)
{
    if (runtime == NULL || name == NULL || runtime->active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const mosaic_app_descriptor_t* descriptor
        = mosaic_app_descriptor_for_name(name);
    if (descriptor == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    runtime->history_count = 0;
    runtime->pending_push_history = false;
    runtime->pending_pop_history = false;
    runtime->pending = descriptor;
    return ESP_OK;
}

esp_err_t mosaic_runtime_back(mosaic_runtime_handle_t runtime)
{
    if (runtime == NULL || runtime->active == NULL ||
            runtime->active->back_action == 0 ||
            runtime->active->back_action == UINT16_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_gsp_event_t event = {
        .type = ESP_GSP_EVENT_CALL,
        .action_id = runtime->active->back_action,
    };
    on_platform_ui_event(runtime, &event);
    return runtime->pending_error;
}

esp_err_t mosaic_runtime_dispatch_pointer(
    mosaic_runtime_handle_t runtime, int32_t x, int32_t y, bool pressed)
{
    if (runtime == NULL || runtime->platform_app == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const mosaic_event_t event = {
        .type = MOSAIC_EVENT_POINTER,
        .timestamp_us = runtime->now_us,
        .data.pointer = {
            .x = x,
            .y = y,
            .pressed = pressed,
        },
    };
    return dispatch(runtime, &event);
}

esp_err_t mosaic_runtime_feed_pointer(
    mosaic_runtime_handle_t runtime, int32_t x, int32_t y, bool pressed)
{
    esp_err_t err
        = mosaic_runtime_dispatch_pointer(runtime, x, y, pressed);
    if (err != ESP_OK) {
        return err;
    }
    return runtime->config.platform->feed_pointer(
        runtime->config.platform_ctx, runtime->platform_app, x, y, pressed);
}

esp_err_t mosaic_runtime_notify_model_changed(
    mosaic_runtime_handle_t runtime, uint16_t app_id, uint32_t revision)
{
    if (runtime == NULL || runtime->platform_app == NULL || revision == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (runtime->active == NULL || runtime->active->id != app_id) {
        return ESP_OK;
    }
    const mosaic_event_t event = {
        .type = MOSAIC_EVENT_MODEL_CHANGED,
        .timestamp_us = runtime->now_us,
        .data.model = {
            .app_id = app_id,
            .revision = revision,
        },
    };
    return dispatch(runtime, &event);
}

esp_err_t mosaic_runtime_timer_start(mosaic_runtime_handle_t runtime,
    const char* id, uint32_t period_ms, bool repeat)
{
    if (runtime == NULL || runtime->platform_app == NULL || id == NULL
        || id[0] == '\0' || strlen(id) > MOSAIC_RUNTIME_TIMER_ID_MAX
        || period_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaic_runtime_timer_t* free_timer = NULL;
    for (size_t index = 0; index < MOSAIC_RUNTIME_MAX_TIMERS; ++index) {
        mosaic_runtime_timer_t* timer = &runtime->timers[index];
        if (timer->active && strcmp(timer->id, id) == 0) {
            free_timer = timer;
            break;
        }
        if (!timer->active && free_timer == NULL) {
            free_timer = timer;
        }
    }
    if (free_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(free_timer, 0, sizeof(*free_timer));
    memcpy(free_timer->id, id, strlen(id) + 1U);
    free_timer->active = true;
    free_timer->repeat = repeat;
    free_timer->period_ms = period_ms;
    free_timer->revision = ++runtime->timer_revision;
    if (free_timer->revision == 0) {
        free_timer->revision = ++runtime->timer_revision;
    }
    free_timer->next_us = runtime->now_us + (int64_t)period_ms * 1000;
    return ESP_OK;
}

esp_err_t mosaic_runtime_timer_cancel(
    mosaic_runtime_handle_t runtime, const char* id)
{
    if (runtime == NULL || id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0; index < MOSAIC_RUNTIME_MAX_TIMERS; ++index) {
        mosaic_runtime_timer_t* timer = &runtime->timers[index];
        if (timer->active && strcmp(timer->id, id) == 0) {
            memset(timer, 0, sizeof(*timer));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const mosaic_app_descriptor_t* mosaic_runtime_active_app(
    mosaic_runtime_handle_t runtime)
{
    return runtime != NULL ? runtime->active : NULL;
}
