/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_instance.h"
#include "gsp/gsp_update.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GSP_INPUT_EVENT_NONE = 0,
    GSP_INPUT_EVENT_CLICK = 1,
    GSP_INPUT_EVENT_PRESS = 2,
    GSP_INPUT_EVENT_RELEASE = 3,
    GSP_INPUT_EVENT_LONG = 4,
    GSP_INPUT_EVENT_VALUE = 5,
    GSP_INPUT_EVENT_CANCEL = 6,
} gsp_input_event_t;

typedef enum {
    GSP_ACTION_KIND_NONE = 0,
    GSP_ACTION_KIND_SHOW = 1,
    GSP_ACTION_KIND_HIDE = 2,
    GSP_ACTION_KIND_TOGGLE = 3,
    GSP_ACTION_KIND_SET_TEXT = 4,
    GSP_ACTION_KIND_SET_COLOR = 5,
    GSP_ACTION_KIND_SET_OPACITY = 6,
    GSP_ACTION_KIND_CALL = 7,
    GSP_ACTION_KIND_GOTO = 8,
    GSP_ACTION_KIND_BACK = 9,
    GSP_ACTION_KIND_SET_VALUE = 10,
    GSP_ACTION_KIND_TOGGLE_VALUE = 11,
    /** value = clamp(value + (int32_t)arg, 0, 100). */
    GSP_ACTION_KIND_ADD_VALUE = 12,
    /** Switch flip: handled by the application/facade handler, which
     *  tweens the value bind between 0 and 100 (slide animation). */
    GSP_ACTION_KIND_ANIMATE_TOGGLE = 13,
    GSP_ACTION_KIND_SET_PAGE = 14,
    GSP_ACTION_KIND_STACK_PUSH = 15,
    GSP_ACTION_KIND_STACK_POP = 16,
    GSP_ACTION_KIND_DRAWER_OPEN = 17,
    GSP_ACTION_KIND_DRAWER_CLOSE = 18,
    /** Input guard emitted before a control's authored actions. */
    GSP_ACTION_KIND_REQUIRE_ENABLED = 19,
    /** One-shot click glow: the facade animates a paused pulse from 100 to 0. */
    GSP_ACTION_KIND_PLAY_GLOW = 20,
} gsp_action_kind_t;

/** Compiler-selected pointer ownership; component routing does not infer it
 * from widget or action kinds on GSB 1.2 scenes. */
typedef enum {
    GSP_ACTION_ROUTE_NONE = 0,
    GSP_ACTION_ROUTE_HIT = 1,
    GSP_ACTION_ROUTE_RANGE = 2,
} gsp_action_route_t;

typedef struct {
    bool found;
    uint32_t hit_index;
    /** GSP_INSTANCE_HANDLE_INVALID for scene-level hits. */
    gsp_instance_handle_t instance;
    uint16_t z;
    uint16_t action_first;
    uint16_t action_count;
    uint16_t flags;
    /** Screen-space bounds of the hit target (instance hits are
     *  already translated) — e.g. for pressed-state feedback. */
    gsp_rect_t bbox;
} gsp_hit_result_t;

typedef struct {
    uint16_t action_id;
    gsp_input_event_t event;
    gsp_action_kind_t kind;
    gsp_action_route_t route;
    uint16_t target_bind;
    /** GSB 1.1 direct property index, UINT16_MAX for no target or 1.0. */
    uint16_t target_property;
    uint32_t arg;
    const uint8_t *params;
    uint16_t param_len;
    /** Navigation actions settle with an animation unless authored instant. */
    bool animated;
} gsp_action_view_t;

typedef gsp_err_t (*gsp_action_handler_t)(gsp_runtime_t *runtime,
        gsp_update_transaction_t *transaction,
        const gsp_action_view_t *action,
        void *user_ctx);

/** Scene-level hit test; template-local entries are skipped. */
gsp_err_t gsp_hit_test(const gsp_scene_view_t *scene, int32_t x, int32_t y,
                       gsp_hit_result_t *out_hit);

/**
 * Runtime hit test covering template instances. Visible instances are
 * tested topmost-first ((z_order, slot) descending) against their
 * translated template-local hit entries, then the scene entries. Instance
 * hits report the owning instance handle.
 */
gsp_err_t gsp_hit_test_runtime(const gsp_runtime_t *runtime,
                               int32_t x, int32_t y,
                               gsp_hit_result_t *out_hit);

gsp_err_t gsp_action_get(const gsp_scene_view_t *scene, uint32_t action_index,
                         gsp_action_view_t *out_action);

/** Resolves an optional REQUIRE_ENABLED guard without dispatching actions. */
gsp_err_t gsp_hit_enabled(const gsp_runtime_t *runtime,
                          const gsp_hit_result_t *hit, bool *out_enabled);

gsp_err_t gsp_action_dispatch(gsp_runtime_t *runtime, const gsp_hit_result_t *hit,
                              gsp_input_event_t event,
                              gsp_update_transaction_t *transaction,
                              gsp_action_handler_t handler, void *user_ctx,
                              size_t *out_executed_count);

/** Dispatch to the captured hit after a range update. Skip driver routing
 * actions; CALL receives the committed value in authored min/max units.
 * Other actions retain their authored arguments and transaction semantics. */
gsp_err_t gsp_action_dispatch_range(gsp_runtime_t *runtime, const gsp_hit_result_t *hit,
                                    gsp_input_event_t event, uint32_t value,
                                    gsp_update_transaction_t *transaction,
                                    gsp_action_handler_t handler, void *user_ctx);

gsp_err_t gsp_input_dispatch(gsp_runtime_t *runtime, int32_t x, int32_t y,
                             gsp_input_event_t event,
                             gsp_update_transaction_t *transaction,
                             gsp_action_handler_t handler, void *user_ctx,
                             gsp_hit_result_t *out_hit,
                             size_t *out_executed_count);

/** Maximum transaction entries required by any one hit/event. */
size_t gsp_input_max_updates_per_hit(const gsp_scene_view_t *scene);

/** Conservative dirty storage recommendation for interactive updates. */
size_t gsp_input_recommended_dirty_capacity(const gsp_scene_view_t *scene);

#ifdef __cplusplus
}
#endif
