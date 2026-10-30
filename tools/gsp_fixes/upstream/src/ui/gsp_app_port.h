/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once
#include "gsp/gsp_build_caps.h"

/* Stable internal port between gsp_app and platform runners.  Runners may
 * include this command/adapter protocol, but must not include or dereference
 * gsp_ui_core_priv.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_gsp_debug.h"
#include "esp_gsp_extension.h"
#include "esp_gsp_updates.h"
#include "gsp/gsp_app.h"
#include "gsp/gsp_visual_transition.h"

typedef enum {
    CMD_SET_VALUE,
    CMD_SET_COLOR,
    CMD_SET_VISIBLE,
    CMD_COMPONENT_SET,
    CMD_COMPONENT_BATCH,
    CMD_COMPONENT_SET_PROPERTY,
    CMD_COMPONENT_PROPERTY_BATCH,
    CMD_PAGE_FLOW_SET_PAGE,
    CMD_STACK_VIEW_PUSH,
    CMD_STACK_VIEW_POP,
    CMD_DRAWER_SET_OPEN,
    CMD_SET_TEXT,
    CMD_SET_IMAGE,
    CMD_SET_CURSOR,
    CMD_LIST_BIND,
    CMD_GRID_BIND,
    CMD_LIST_TOTAL,
    CMD_LIST_REFRESH,
    CMD_LIST_LAYOUT,
    CMD_MESSAGE_BIND,
    CMD_MESSAGE_CHANGED,
    CMD_LIST_SNAP,
    CMD_LIST_FLING,
    CMD_LIST_SCROLL,
    CMD_LIST_FADE,
    CMD_ANIMATE,
    CMD_ANIMATE_STOP,
    CMD_PLAY,
    CMD_GOTO_SCENE,
    CMD_WIDGET_CREATE,
    CMD_WIDGET_DESTROY,
    CMD_WIDGET_SET_VALUE,
    CMD_WIDGET_SET_COLOR,
    CMD_WIDGET_SET_TEXT,
    CMD_WIDGET_SET_VISIBLE,
    CMD_WIDGET_SET_POSITION,
    CMD_SET_INSTANCE_IMAGE,
    CMD_INJECT_TOUCH,
    CMD_SET_PINCH_CALLBACK,
    CMD_SET_INPUT_INTERCEPTOR,
    CMD_SET_OVERLAY_CONTRIBUTOR,
    CMD_CANVAS_SET_DRAW_CB,
    CMD_CANVAS_INVALIDATE,
    CMD_CANVAS_PUSH,
    CMD_CANVAS_STOP,
    CMD_FLUSH,
    CMD_SET_PRESS_FEEDBACK,
    /* Platform-runner controls. Never dispatched into the UI core. */
    CMD_POINTER_WAKE,
    CMD_RUNTIME_PAUSE,
    CMD_RUNTIME_ACTIVATE,
    CMD_STOP,
    CMD_CHART_SET_SERIES,
    CMD_CHART_APPEND,
    CMD_SET_COMPONENT_EVENT_CALLBACK,
    CMD_SET_EVENT_CALLBACK,
    CMD_SET_POINTER_OBSERVER,
    CMD_SET_EXTENSION_CALLBACK,
    CMD_CANVAS_CHECK,
    CMD_UPDATE_GROUP,
    CMD_DROPDOWN_SET_OPTIONS_TEXT,
    /* Runner-only maintenance; append to preserve existing command values. */
    CMD_RUNTIME_PAUSED_CALL,
} esp_gsp_cmd_type_t;

/* This is a build capability of the shipped library, not an application
 * menuconfig value. It is the command's inline fast-path capacity, not the
 * maximum logical text length. Changing it rebuilds every consumer of the
 * shared command structure (platform runner + command queue). */
#define ESP_GSP_TEXT_CAPACITY ESP_GSP_BUILD_CAP_TEXT_CAPACITY
#define ESP_GSP_CMD_FLAG_NONBLOCKING (1u << 0)
#define ESP_GSP_CMD_FLAG_LIST_SNAPSHOT (1u << 1)
#define ESP_GSP_CMD_FLAG_LIST_SNAP (1u << 3) /* CMD_LIST_BIND only */
#define ESP_GSP_CMD_FLAG_LIST_CYCLIC (1u << 4) /* CMD_LIST_BIND only */
#define ESP_GSP_CMD_FLAG_LIST_WHEEL (1u << 5) /* CMD_LIST_BIND only */
#define ESP_GSP_CMD_FLAG_LIST_PAINT (1u << 6) /* compiled viewport ordering */
#define ESP_GSP_CMD_FLAG_LIST_KEEP_OLD_OFFSETS (1u << 2)
#define ESP_GSP_CMD_FLAG_TEXT_POINTER (1u << 3)
#define ESP_GSP_CMD_FLAG_TEXT_OWNED (1u << 4)
/* Sized simulator Canvas copies: value=payload bytes, transform_group_ref=origin scene.
 * Validate again at application time, after any queued navigation. */
#define ESP_GSP_CMD_FLAG_CANVAS_SIZED (1u << 5)
/* Public borrowed-frame bounds, independent of the simulator's scene check.
 * value/value2 contain the low/high 32 bits of size_bytes; no queue growth. */
#define ESP_GSP_CMD_FLAG_CANVAS_BOUNDED (1u << 2)
/* External image frames stay bound to the scene that requested them. */
#define ESP_GSP_CMD_FLAG_IMAGE_SCENE (1u << 6)
#define ESP_GSP_CMD_FLAG_ASSET_DELTA (1u << 7)

typedef struct {
    esp_gsp_cmd_type_t type;
    uint8_t flags;
    uint16_t bind;
    uint16_t slot;
    int16_t x;
    int16_t y;
    uint16_t transform_group_ref;
    uint32_t value;
    uint32_t value2;
    const void *pointer;
    size_t pointer_size;
    void *pointer2;
    /* Commands carry one callback or a compiled viewport paint boundary. */
    union {
        uint32_t paint_after;
        esp_gsp_pinch_cb_t pinch_cb;
        esp_gsp_component_event_cb_t component_event_cb;
        esp_gsp_input_interceptor_cb_t input_interceptor_cb;
        esp_gsp_overlay_contributor_cb_t overlay_contributor_cb;
        esp_gsp_canvas_draw_cb_t canvas_draw_cb;
        esp_gsp_event_cb_t event_cb;
        esp_gsp_pointer_observer_cb_t pointer_observer_cb;
        esp_gsp_extension_cb_t extension_cb;
    };
    esp_gsp_canvas_release_cb_t release_cb;
    union {
        char text[ESP_GSP_TEXT_CAPACITY + 1];
        gsp_rect_t rect;
        uint32_t initial_total;   /*!< list binding and initial count are one command */
        uint32_t image_scene_generation; /*!< external-image scene visit */
    };
} esp_gsp_cmd_t;

/** Returns the inline or external UTF-8 payload of a text command. */
const char *esp_gsp_command_text(const esp_gsp_cmd_t *command);

/** Releases framework-owned text storage carried by one accepted command. */
void esp_gsp_command_release(const esp_gsp_cmd_t *command);

/* Folds an older queued canvas push into the newer one for the same
 * bind, widening the dirty rect so nothing the dropped frame touched is
 * left stale. Platform queues use this to coalesce a backlog. */
void esp_gsp_canvas_merge_dirty(esp_gsp_cmd_t *newer,
                                const esp_gsp_cmd_t *older);

/* Diagnostic reads are grouped so adding a metric does not extend the main
 * command/lifecycle adapter contract. The legacy public getters remain
 * separate wrappers while the internal port has one obvious home for future
 * snapshot/query plumbing. */
typedef struct {
    void (*render_stats)(void *ctx, uint32_t *out_frames,
                         uint64_t *out_busy_us);
    void (*transition_stats)(void *ctx,
                             esp_gsp_transition_stats_t *out_stats);
    void (*region_stats)(void *ctx, esp_gsp_region_stats_t *out_stats);
    void (*render_phases)(void *ctx, uint64_t *out_render_us,
                          uint64_t *out_submit_us);
    void (*render_error_stats)(void *ctx, uint32_t *out_failures,
                               gsp_err_t *out_last_error);
    void (*service_stats)(void *ctx, uint32_t *out_iterations,
                          uint64_t *out_service_us,
                          uint32_t *out_commands);
    void (*media_stats)(void *ctx, esp_gsp_media_stats_t *out_stats);
} gsp_app_diagnostic_ops_t;

typedef struct gsp_update_frame_status {
    uint64_t submitted;
    uint64_t transferred;
    uint64_t presented;
    uint32_t generation;
    esp_gsp_update_confirmation_t confirmation;
    gsp_err_t error;
} gsp_update_frame_status_t;

typedef struct {
    esp_gsp_err_t (*submit)(void *ctx, esp_gsp_handle_t handle,
                            const esp_gsp_cmd_t *command);
    esp_gsp_err_t (*flush)(void *ctx, esp_gsp_handle_t handle,
                           uint32_t timeout_ms);
    esp_gsp_err_t (*stop)(void *ctx, esp_gsp_handle_t handle);
    bool (*visual_transition_begin)(
        void *ctx, esp_gsp_handle_t handle,
        const gsp_visual_transition_request_t *request);
    void (*visual_transition_progress)(void *ctx, uint16_t progress);
    void (*visual_transition_end)(void *ctx, bool commit);
    gsp_app_diagnostic_ops_t diagnostics;
    bool (*decode_async)(void *ctx);
    void (*decode_kick)(void *ctx);
    void (*state_lock)(void *ctx);
    void (*state_unlock)(void *ctx);
    /* Attached runners use their monotonic clock for timer registration and
     * wake a sleeping service after changes. Stepped apps may omit both. */
    int64_t (*timer_now_us)(void *ctx);
    void (*timers_changed)(void *ctx);
    gsp_err_t (*frame_status)(void *ctx, gsp_update_frame_status_t *out);
    void (*cancel_frame_tracking)(void *ctx, uint64_t frame_id, uint32_t generation);
} gsp_app_adapter_ops_t;

esp_gsp_handle_t gsp_app_handle(gsp_app_t *app);
gsp_app_t *gsp_app_from_handle(esp_gsp_handle_t handle);
/** Minimum render stack for the installed dynamic-font provider, or zero. */
size_t gsp_app_freetype_stack_requirement(const gsp_app_t *app);
int32_t gsp_app_font_outline_drivers(void);
/** Replacing/detaching an adapter retires viewport captures through the old
 *  adapter while the app runtime is still alive. Same-adapter reattachment
 *  preserves an active session. Call only from the app's owning task. */
void gsp_app_attach_adapter(gsp_app_t *app,
                            const gsp_app_adapter_ops_t *ops,
                            void *ctx);
void *gsp_app_adapter_context(esp_gsp_handle_t handle,
                              const gsp_app_adapter_ops_t *expected_ops);
bool gsp_app_callback_mutation_blocked(esp_gsp_handle_t handle);
void gsp_app_dispatch_command(gsp_app_t *app,
                              const esp_gsp_cmd_t *command);
void gsp_app_prepare(gsp_app_t *app, int64_t now_us);
/** Synchronize input callbacks before the frame step uses the same time. */
void gsp_app_sync_input_time(gsp_app_t *app, int64_t now_us);
void gsp_app_frame_retired(gsp_app_t *app);
/** Prepare target groups immediately before a normal scene render. Returns
 * true when a group requires correlated framebuffer confirmation. */
bool gsp_app_updates_before_frame(gsp_app_t *app);
void gsp_app_updates_after_frame(gsp_app_t *app, gsp_err_t result,
                                 uint64_t frame_id, uint32_t generation);
void gsp_app_media_stats(gsp_app_t *app,
                         esp_gsp_media_stats_t *out_stats);
/** Release decoded image surfaces while preserving every scene and widget state. */
void gsp_app_release_image_caches(gsp_app_t *app);
void gsp_app_feed_platform_pointer(gsp_app_t *app,
                                   const gsp_platform_pointer_t *pointer);
void gsp_app_feed_platform_touch_frame(
    gsp_app_t *app, const gsp_platform_touch_frame_t *frame);
/** Read synthetic contacts without cancelling or changing the pointer. */
bool gsp_app_has_active_contacts(const gsp_app_t *app);

/* Press-activated pointer feeding, shared by platform runners: polled
 * controllers deliver samples every cycle, so latch on press, forward
 * while the finger is down, deliver the release once, and drop idle
 * samples. Event-driven runners may feed every event — the latch
 * passes them through. Returns true when the sample was forwarded. */
typedef struct {
    bool active;
} gsp_press_feed_t;

bool gsp_press_feed(gsp_press_feed_t *feed, gsp_app_t *app,
                    const gsp_platform_pointer_t *sample);
bool gsp_press_feed_touch_frame(gsp_press_feed_t *feed,
                                gsp_app_t *app,
                                const gsp_platform_touch_frame_t *frame);

void esp_gsp_snapshot_backend_result(esp_gsp_handle_t gsp, int32_t error);
/** A captured backdrop changed: resume live paint at the current offset. */
void esp_gsp_snapshot_backend_invalidated(esp_gsp_handle_t gsp, int32_t error);
