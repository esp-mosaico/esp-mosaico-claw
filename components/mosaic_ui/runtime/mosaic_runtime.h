/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mosaic_app_catalog.h"
#include "mosaic_system_flow.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAIC_RUNTIME_MAX_TIMERS 8U
#define MOSAIC_RUNTIME_TIMER_ID_MAX 31U
#define MOSAIC_RUNTIME_APP_HISTORY_MAX 8U

typedef struct mosaic_runtime_t* mosaic_runtime_handle_t;
typedef void* mosaic_platform_app_handle_t;

typedef enum {
    MOSAIC_EVENT_START = 0,
    MOSAIC_EVENT_STOP,
    MOSAIC_EVENT_UI_CALL,
    MOSAIC_EVENT_SCENE_CHANGED,
    MOSAIC_EVENT_POINTER,
    MOSAIC_EVENT_TIMER,
    MOSAIC_EVENT_MODEL_CHANGED,
} mosaic_event_type_t;

typedef struct mosaic_event {
    mosaic_event_type_t type;
    int64_t timestamp_us;
    union {
        struct {
            uint16_t action_id;
            uint32_t arg;
            uint16_t scene_id;
            uint16_t list;
            uint32_t item;
        } call;
        struct {
            uint16_t scene_id;
        } scene;
        struct {
            int32_t x;
            int32_t y;
            bool pressed;
        } pointer;
        struct {
            const char* id;
            uint32_t sequence;
        } timer;
        struct {
            uint16_t app_id;
            uint32_t revision;
        } model;
    } data;
} mosaic_event_t;

typedef void (*mosaic_platform_ui_event_cb_t)(
    void* user_ctx, const esp_gsp_event_t* event);

/** Platform boundary for the portable runtime.
 *
 * ESP-IDF implements this with the esp_lcd presenter and partition assets. App
 * logic (Lua or native C) is owned by the returned platform app session.
 */
typedef struct {
    esp_err_t (*open_app)(void* ctx, const mosaic_app_descriptor_t* app,
        mosaic_platform_ui_event_cb_t event_cb, void* event_ctx,
        mosaic_platform_app_handle_t* ret_app);
    /** Replace one non-root App with another without exposing the root UI. */
    esp_err_t (*replace_app)(void* ctx, mosaic_platform_app_handle_t current,
        const mosaic_app_descriptor_t* next,
        mosaic_platform_ui_event_cb_t event_cb, void* event_ctx,
        mosaic_platform_app_handle_t* ret_app);
    esp_err_t (*close_app)(void* ctx, mosaic_platform_app_handle_t app);
    esp_err_t (*step_app)(
        void* ctx, mosaic_platform_app_handle_t app, int64_t now_us);
    esp_err_t (*dispatch_event)(void* ctx, mosaic_platform_app_handle_t app,
        const mosaic_event_t* event);
    esp_err_t (*feed_pointer)(void* ctx, mosaic_platform_app_handle_t app,
        int32_t x, int32_t y, bool pressed);
} mosaic_platform_ops_t;

typedef struct {
    const mosaic_platform_ops_t* platform;
    void* platform_ctx;
    /** Optional shared Setup -> Welcome -> Home lifecycle. */
    mosaic_system_flow_t* system_flow;
} mosaic_runtime_config_t;

esp_err_t mosaic_runtime_create(const mosaic_runtime_config_t* config,
    mosaic_runtime_handle_t* ret_runtime);
void mosaic_runtime_delete(mosaic_runtime_handle_t runtime);

esp_err_t mosaic_runtime_start(
    mosaic_runtime_handle_t runtime, const char* initial_app);
esp_err_t mosaic_runtime_stop(mosaic_runtime_handle_t runtime);
esp_err_t mosaic_runtime_step(mosaic_runtime_handle_t runtime, int64_t now_us);

esp_err_t mosaic_runtime_request_app(
    mosaic_runtime_handle_t runtime, const char* name);
/** Request the active App's authored back action on the runtime owner task. */
esp_err_t mosaic_runtime_back(mosaic_runtime_handle_t runtime);
/** Dispatches a platform-originated pointer sample to app logic only. */
esp_err_t mosaic_runtime_dispatch_pointer(
    mosaic_runtime_handle_t runtime, int32_t x, int32_t y, bool pressed);
esp_err_t mosaic_runtime_feed_pointer(
    mosaic_runtime_handle_t runtime, int32_t x, int32_t y, bool pressed);
esp_err_t mosaic_runtime_notify_model_changed(
    mosaic_runtime_handle_t runtime, uint16_t app_id, uint32_t revision);

esp_err_t mosaic_runtime_timer_start(mosaic_runtime_handle_t runtime,
    const char* id, uint32_t period_ms, bool repeat);
esp_err_t mosaic_runtime_timer_cancel(
    mosaic_runtime_handle_t runtime, const char* id);

const mosaic_app_descriptor_t* mosaic_runtime_active_app(
    mosaic_runtime_handle_t runtime);

#ifdef __cplusplus
}
#endif
