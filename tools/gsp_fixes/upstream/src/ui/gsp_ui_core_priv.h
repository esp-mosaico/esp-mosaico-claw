/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once
#include "gsp/gsp_build_caps.h"

/* UI core internals shared by gsp_app and the public UI services
 * translation units: the command protocol, slot pools, and current
 * core state container. Nothing here is public API. */

#include "esp_gsp.h"
#include "esp_gsp_debug.h"
#include "esp_gsp_assets.h"
#include "gsp_app_port.h"
#include "gsp_update_receipt_priv.h"
#include "gsp_extension_priv.h"
#include "gsp/gsp_anim.h"
#include "gsp/gsp_app.h"
#include "gsp/gsp_component_advanced.h"
#include "gsp/gsp_context.h"
#include "gsp/gsp_input.h"
#include "gsp/gsp_overlay.h"
#include "gsp/gsp_boolean_driver.h"
#include "gsp/gsp_list_driver.h"
#include "gsp/gsp_range_driver.h"
#include "gsp/gsp_page_flow_driver.h"
#include "gsp/gsp_stack_view_driver.h"
#include "gsp/gsp_config_resolver.h"
#include "gsp/gsp_drawer_driver.h"
#include "gsp/gsp_multitouch.h"
#include "gsp/gsp_pointer.h"
#include "gsp/gsp_repeater.h"
#include "gsp/gsp_scroll_phys.h"
#include "gsp/gsp_swipe_kinetics.h"
#include "gsp/gsp_transition_compose.h"
#include "gsp/gsp_widget.h"
#include "gsp_time_priv.h"
#include <stdatomic.h>
#include <stdlib.h>
#ifndef GSP_ENABLE_FREETYPE
#define GSP_ENABLE_FREETYPE 0
#endif
#if GSP_ENABLE_FREETYPE
#include "ft2build.h"
#include FT_FREETYPE_H
#include FT_SIZES_H
#include "gsp/gsp_freetype.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Component update instrumentation is opt-in and shares the render-task
 * service profile switch. Everything it needs — counters, timers and the
 * per-core storage below — exists only when the switch is on, so an ordinary
 * build gains no counter update, no clock read, no allocation, no extra
 * per-handle bytes and no atomic dependency on the property path.
 *
 * The switch changes the layout of gsp_ui_core_t, so it must be uniform
 * across every translation unit that includes this header. That is how it is
 * already used: integrations define it on the whole esp-gsp component (see
 * examples/performance/benchmark), and applications only ever see the public headers. */
#ifndef GSP_PROFILE_SERVICE
#define GSP_PROFILE_SERVICE 0
#endif

/* gsp_port_time_us() only has a clock under ESP_PLATFORM and returns 0
 * everywhere else (see gsp_time_priv.h, which keeps the host bit-exact).
 * Measuring spans against a constant would just accumulate zeros, so the
 * timing half of the probe follows the clock rather than the switch. The
 * condition is mirrored here instead of exported from the port header to
 * keep this change inside the UI layer. */
#ifdef ESP_PLATFORM
#define GSP_PROBE_HAS_CLOCK 1
#else
#define GSP_PROBE_HAS_CLOCK 0
#endif

#if GSP_PROFILE_SERVICE

/* Producers here are genuinely concurrent: any application task may validate
 * a batch while the render task applies another and a third task snapshots.
 * Every field is therefore an atomic accumulated with relaxed ordering —
 * correct because each counter is independent (and may wrap), so no
 * happens-before relationship between counters is claimed or needed.
 *
 * Atomic operations use native instructions where available, otherwise the
 * toolchain's runtime helpers. ESP-IDF implements unsupported widths with
 * short interrupt-disabled critical sections or spinlocks, so neither reads
 * nor writes are promised to be lock-free. These helpers do not acquire
 * UI/state mutexes or hold an atomic-operation lock across a yield. */
typedef struct {
    atomic_uint_least32_t validated_batches;
    atomic_uint_least32_t validated_entries;
    atomic_uint_least32_t validate_failures;
    atomic_uint_least32_t submit_failures;
    atomic_uint_least32_t applied_batches;
    atomic_uint_least32_t applied_entries;
    atomic_uint_least32_t apply_failures;
    atomic_uint_least32_t apply_dropped_batches;
    atomic_uint_least32_t heap_scratch_batches;
    atomic_uint_least32_t steps;
    atomic_uint_least32_t max_validate_us;
    atomic_uint_least32_t max_apply_us;
    atomic_uint_least32_t max_step_us;
    atomic_uint_least64_t validate_us;
    atomic_uint_least64_t apply_us;
    atomic_uint_least64_t step_us;
} gsp_property_probe_t;

/* Called before the core is visible to producers or readers. */
static inline void gsp_property_probe_init(gsp_property_probe_t *probe)
{
    atomic_init(&probe->validated_batches, 0);
    atomic_init(&probe->validated_entries, 0);
    atomic_init(&probe->validate_failures, 0);
    atomic_init(&probe->submit_failures, 0);
    atomic_init(&probe->applied_batches, 0);
    atomic_init(&probe->applied_entries, 0);
    atomic_init(&probe->apply_failures, 0);
    atomic_init(&probe->apply_dropped_batches, 0);
    atomic_init(&probe->heap_scratch_batches, 0);
    atomic_init(&probe->steps, 0);
    atomic_init(&probe->max_validate_us, 0);
    atomic_init(&probe->max_apply_us, 0);
    atomic_init(&probe->max_step_us, 0);
    atomic_init(&probe->validate_us, 0);
    atomic_init(&probe->apply_us, 0);
    atomic_init(&probe->step_us, 0);
}

static inline void gsp_probe_add32(atomic_uint_least32_t *counter,
                                   uint32_t delta)
{
    atomic_fetch_add_explicit(counter, delta, memory_order_relaxed);
}

static inline void gsp_probe_add64(atomic_uint_least64_t *counter,
                                   uint64_t delta)
{
    atomic_fetch_add_explicit(counter, delta, memory_order_relaxed);
}

static inline uint32_t gsp_probe_read32(atomic_uint_least32_t *counter)
{
    return (uint32_t)atomic_load_explicit(counter, memory_order_relaxed);
}

static inline uint64_t gsp_probe_read64(atomic_uint_least64_t *counter)
{
    return (uint64_t)atomic_load_explicit(counter, memory_order_relaxed);
}

/* Raises a peak without waiting on another producer: the loop retries
 * against the value the failed exchange reloaded, and exits as soon as the
 * peak is at least the sample, so no producer can hold the field. A weak
 * exchange may also fail spuriously, so the iteration count is not strictly
 * bounded by how often the peak grows; it is bounded in practice because
 * this is a few instructions on an uncontended field. */
static inline void gsp_probe_max32(atomic_uint_least32_t *peak,
                                   uint32_t sample)
{
    uint_least32_t observed = atomic_load_explicit(peak, memory_order_relaxed);
    while (sample > observed &&
            !atomic_compare_exchange_weak_explicit(
                peak, &observed, sample,
                memory_order_relaxed, memory_order_relaxed)) {
    }
}

/* Peaks are reported in 32 bits; a span beyond 71 minutes saturates. */
static inline uint32_t gsp_probe_clamp_us(uint64_t elapsed_us)
{
    return elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;
}

/* Folds one span into its running total and its peak. Compiled out where
 * the port has no clock, so no 64-bit atomic is touched to accumulate a
 * difference that is always zero. */
static inline void gsp_probe_span(atomic_uint_least64_t *total_us,
                                  atomic_uint_least32_t *peak_us,
                                  uint64_t start_us)
{
#if GSP_PROBE_HAS_CLOCK
    const uint64_t elapsed_us = gsp_port_time_us() - start_us;
    gsp_probe_add64(total_us, elapsed_us);
    gsp_probe_max32(peak_us, gsp_probe_clamp_us(elapsed_us));
#else
    (void)total_us;
    (void)peak_us;
    (void)start_us;
#endif
}

#endif /* GSP_PROFILE_SERVICE */

/* Opens a span, or returns 0 when the instrumentation or the port clock is
 * absent. The paired gsp_probe_span() is compiled out in both cases, so the
 * 0 is never used as a timestamp. */
static inline uint64_t gsp_probe_now_us(void)
{
#if GSP_PROFILE_SERVICE && GSP_PROBE_HAS_CLOCK
    return gsp_port_time_us();
#else
    return 0;
#endif
}

/* Static driver registry (ids index esp_gsp_drivers). */
#define ESP_GSP_DRIVER_BOOLEAN 0
#define ESP_GSP_DRIVER_RANGE 1
#define ESP_GSP_DRIVER_LIST 2
#define ESP_GSP_DRIVER_PAGE_FLOW 3
#define ESP_GSP_DRIVER_STACK_VIEW 4
#define ESP_GSP_DRIVER_DRAWER 5
#define ESP_GSP_DRIVER_COUNT 6
#define ESP_GSP_COMPONENT_EXT_STRIDE GSP_PAGE_FLOW_DRIVER_EXT_SIZE
extern const gsp_component_driver_t *const esp_gsp_drivers[];
#define ESP_GSP_MAX_IMAGE_TARGETS ESP_GSP_DYNAMIC_IMAGE_TARGETS_MAX
#define ESP_GSP_SLOT_AUTO UINT16_MAX
/* Flick commit: a release whose recent pointer movement (sum of the last 3
 * pointer samples, ~30 ms) exceeds this many px toward the target
 * commits the page swipe regardless of total drag distance.
 * A reverse flick cancels the same way. */
/* Loop pacing: continuous activity (finger down, tweens, momentum,
 * playing animations) keeps the frame tick; an idle UI sleeps until
 * the nearest timed deadline, capped so the first touch contact is
 * still picked up promptly. The idle cadence is a runtime knob
 * (gsp_app_config_t.idle_poll_ms), so only the active tick stays
 * compile-time here. */

/* The command protocol (esp_gsp_cmd_type_t / esp_gsp_cmd_t) lives in
 * gsp_app_port.h so platform runners can share it without pulling this
 * private core header. The animation-command payload packing below is
 * an internal UI-core detail layered on top of that shared command. */

/* CMD_ANIMATE reuses the fixed command payload without growing the queue:
 * bind=target, slot=(flags << 8)|ease, x=origin scene,
 * value/value2=full-width from/to, pointer_size=duration_ms. */
#define ESP_GSP_ANIM_TARGET_WIDGET   (1U << 0)
#define ESP_GSP_ANIM_TARGET_COLOR    (1U << 1)
#define ESP_GSP_ANIM_TARGET_PROPERTY (1U << 2)
#define ESP_GSP_ANIM_FROM_CURRENT    (1U << 3)
/* Paired x/y: y=second target, value/value2=the two destinations. Both
 * axes land in one transaction so no frame ever shows a half-applied
 * move. Implies FROM_CURRENT; the axes have no separate origin. */
#define ESP_GSP_ANIM_PAIRED_POSITION (1U << 4)
#define ESP_GSP_ANIM_CONFIGURED (1U << 5)
#define ESP_GSP_ANIM_DISCARD_HIDDEN (1U << 6)
#define ESP_GSP_ANIM_FLAGS_SHIFT 8U
#define ESP_GSP_ANIM_COMMAND_SLOT(ease, flags) \
    ((uint16_t)(ease) | (uint16_t)((flags) << ESP_GSP_ANIM_FLAGS_SHIFT))
#define ESP_GSP_ANIM_COMMAND_FLAGS(command) \
    ((uint8_t)((command)->slot >> ESP_GSP_ANIM_FLAGS_SHIFT))
#define ESP_GSP_ANIM_COMMAND_EASE(command) ((uint8_t)((command)->slot))

typedef struct {
    uint32_t period_ms;
    int64_t next_us;
    esp_gsp_timer_cb_t cb;
    void *user_ctx;
    bool active;
} esp_gsp_timer_slot_t;

typedef struct gsp_ui_core gsp_ui_core_t;

#if GSP_ENABLE_FREETYPE
typedef struct {
    esp_gsp_err_t (*setup)(gsp_ui_core_t *gsp, const gsp_app_config_t *config);
    void (*deinit)(gsp_ui_core_t *gsp);
    gsp_text_dynamic_map_cb_t map;
    size_t render_stack_requirement;
} gsp_dynamic_font_provider_t;
#endif

typedef struct {
    uint64_t key;                   /*!< scene<<16|bind, or widget<<16|slot
                                         with ESP_GSP_TEXT_KEY_WIDGET set */
    uint8_t *run_buffer;
    bool used;
    size_t run_capacity;
    uint8_t inline_run_buffer[8 + ESP_GSP_TEXT_CAPACITY * 6];
} esp_gsp_text_slot_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    bool allocated;
} esp_gsp_text_buffer_t;

/* Only selected-property batches need this temporary state. The common
 * single-Dropdown case keeps its record inline; prepared dynamic labels
 * borrow their existing runs instead of allocating another one. */
typedef struct {
    uint64_t key;
    gsp_glyph_run_view_t previous;
    gsp_glyph_run_view_t run;
    const char *text;
    uint8_t *buffer;
    size_t capacity;
    char *cursor_buffer;
    size_t cursor_capacity;
    struct esp_gsp_dropdown_state *state;
    uint32_t selected;
    uint16_t bind;
    uint16_t handle;
    bool cursor;
} esp_gsp_dropdown_selection_t;

typedef struct {
    esp_gsp_dropdown_selection_t *entries;
    size_t count;
    gsp_runtime_t *runtime;
    uint32_t revision;
    uint32_t snapshot_revision;
    bool published;
    esp_gsp_dropdown_selection_t single;
} esp_gsp_dropdown_selection_batch_t;

/* Keep scene and widget identities disjoint across the full 16-bit ranges. */
#define ESP_GSP_TEXT_KEY_WIDGET (UINT64_C(1) << 63)

static inline uint64_t esp_gsp_text_scene_key(uint16_t scene, uint16_t bind)
{
    return ((uint64_t)scene << 16) | bind;
}

static inline uint64_t esp_gsp_text_widget_key(uint16_t widget, uint16_t slot)
{
    return ESP_GSP_TEXT_KEY_WIDGET | ((uint64_t)widget << 16) | slot;
}

typedef struct {
    gsp_widget_t widget;
    uint16_t scene;
    bool used;
    /* Handle generation (12-bit, wraps): a stale handle from a
     * destroyed widget must not address the recycled slot. */
    uint16_t generation;
    /* Reserved by esp_gsp_widget_create (any task) until the render
     * task either creates or fails the instance; cleared on destroy.
     * Handles recycle — a monotonic counter would exhaust the pool. */
    volatile uint8_t reserved;
} esp_gsp_widget_slot_t;

/*
 * One accepted runtime-image request. Exactly one place owns it at a
 * time — the queued command, then the slot's pending generation, then
 * its active one — so the handoff needs no reference count, and the
 * ownership contract in esp_gsp.h lives entirely in image_release().
 */
typedef struct {
    void *data;                     /*!< encoded bytes */
    const gsp_resource_view_t *prepared; /*!< validated external resource, optional */
    size_t size;
    atomic_uint ref_count;
    esp_gsp_image_ownership_t ownership;
    uint32_t cache_key;
    uint32_t scene_generation;      /*!< external-image scene visit */
    esp_gsp_image_release_cb_t on_release;
    void *release_ctx;
    esp_gsp_image_complete_cb_t on_complete;
    void *complete_ctx;
    esp_gsp_handle_t handle;
    uint16_t bind;
    gsp_instance_handle_t expected_instance;
} esp_gsp_image_submission_t;

typedef enum {
    ESP_GSP_IMAGE_TARGET_BIND = 0,
    ESP_GSP_IMAGE_TARGET_WIDGET,
    ESP_GSP_IMAGE_TARGET_ROW,
} esp_gsp_image_target_kind_t;

/*
 * A dynamic image publishes into the spare ref of its pair while the
 * bind keeps showing the live one, so the bind moves in a single update
 * once the decode lands. One ref per slot would mean overwriting the
 * resource the bind is already pointing at, which shows through as the
 * placeholder for as long as the decode takes.
 */
#define ESP_GSP_IMAGE_REFS_PER_SLOT 2
typedef struct {
    esp_gsp_image_target_kind_t target_kind;
    uint16_t bind;
    uint16_t scene;
    uint16_t target_slot;
    uint16_t row_slot;
    uint32_t item;
    gsp_instance_handle_t instance;
    uint16_t placeholder_ref;
    uint16_t active_ref;            /*!< ref the bind reads from */
    uint16_t pending_ref;           /*!< ref the decode writes into */
    uint32_t pending_resource_id;   /*!< generation awaiting its decode */
    uint32_t pinned_resource_id;    /*!< held against eviction until swap */
    esp_gsp_image_submission_t *active;   /*!< behind active_ref */
    esp_gsp_image_submission_t *pending;  /*!< behind pending_ref */
    bool used;
} esp_gsp_image_slot_t;

/* External frame sink (canvas): the scene's placeholder image view is
 * overwritten with user-produced frames; retired frames release after
 * the frame that replaced them has rendered. */
typedef struct {
    gsp_ui_core_t *owner;
    uint16_t bind;
    uint16_t scene;
    uint16_t ref;
    bool used;
    gsp_resource_view_t saved;      /*!< placeholder view (restore) */
    esp_gsp_canvas_release_cb_t live_cb;    /*!< frame on screen */
    void *live_ctx;
    esp_gsp_canvas_release_cb_t retire_cb;  /*!< fires after render */
    void *retire_ctx;
    esp_gsp_canvas_draw_cb_t draw_cb;
    void *draw_ctx;
} esp_gsp_canvas_slot_t;

typedef struct {
    bool active;
    bool widget_target;
    bool property_target;
    bool color_mode;                /*!< from/to are native RGB565 */
    bool position_mode;             /*!< paired direct x/y properties */
    bool configured;
    uint16_t target;                /*!< bind id or widget handle */
    uint16_t target_y;              /*!< paired y property */
    uint16_t scene;                 /*!< scene that owns the target */
    int32_t from;
    int32_t to;
    /* These modes are exclusive. Preserve the original slot size and the
     * hot active/target/timing field offsets for ordinary animations. */
    union {
        struct {
            int32_t from_y, to_y;
        };
        int64_t last_tick_us;
    };
    int64_t start_us;
    uint32_t duration_us;
    uint8_t ease;
    bool alternate, paused, phase_wrap;
    int32_t last_value;
    union {
        int32_t last_y;
        struct {
            uint16_t iterations;
            bool discard_hidden; /*!< ephemeral click feedback, reuses union padding */
        };
    };
} esp_gsp_anim_slot_t;

#define ESP_GSP_ROW_RUN_BYTES (8U + ESP_GSP_TEXT_CAPACITY * 6U)

#define ESP_GSP_TRANSACTION_UPDATE_CAPACITY \
    ESP_GSP_BUILD_CAP_TRANSACTION_UPDATE_CAPACITY

typedef struct {
    gsp_pending_update_t inline_updates[ESP_GSP_TRANSACTION_UPDATE_CAPACITY];
    gsp_pending_update_t *updates;
    size_t capacity;
} esp_gsp_update_scratch_t;

static inline esp_gsp_err_t esp_gsp_update_scratch_init(
    esp_gsp_update_scratch_t *scratch, size_t required)
{
    if (scratch == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    scratch->capacity = required != 0 ? required : 1U;
    scratch->updates = scratch->inline_updates;
    if (scratch->capacity <= ESP_GSP_TRANSACTION_UPDATE_CAPACITY) {
        return ESP_GSP_OK;
    }
    if (scratch->capacity > SIZE_MAX / sizeof(*scratch->updates)) {
        return ESP_GSP_ERR_INVALID_SIZE;
    }
    scratch->updates = malloc(scratch->capacity * sizeof(*scratch->updates));
    return scratch->updates != NULL ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
}

static inline void esp_gsp_update_scratch_deinit(
    esp_gsp_update_scratch_t *scratch)
{
    if (scratch != NULL && scratch->updates != scratch->inline_updates) {
        free(scratch->updates);
    }
}

typedef struct {
    uint64_t id;
    uint32_t revision;
    uint32_t text_hash;
    size_t text_bytes;
    uint16_t height;
    uint8_t direction;
} esp_gsp_message_layout_t;

/* The COLD binding of a list: what survives while its page is
 * hidden. The hot state (repeater, row instances, physics) lives in
 * a gsp_list_state_t extension while visible — the render loop's
 * reconcile pass activates and parks it off the visibility slot. */
typedef struct {
    gsp_ui_core_t *owner;
    esp_gsp_list_bind_cb_t bind_item;
    void *bind_user_ctx;
    esp_gsp_grid_bind_cb_t grid_bind_item;
    void *grid_user_ctx;
    const gsp_grid_config_t *grid_config;
    uint8_t *run_buffers;           /*!< text_slot_count x slot_count runs */
    uint8_t *run_used;              /*!< published row runs */
    uint32_t *item_offsets;         /*!< variable rows: total + 1 prefix */
    esp_gsp_message_layout_t *message_layouts; /*!< cached message measures */
    uint32_t message_layout_count;
    size_t message_layout_capacity;
    esp_gsp_message_layout_t *message_layout_staging;
    size_t message_layout_staging_capacity;
    uint32_t *message_offset_staging;
    size_t message_offset_staging_capacity;
    uint8_t *message_changed;
    size_t message_changed_capacity;
    uint8_t *message_shape_scratch;
    size_t message_shape_scratch_capacity;
    size_t run_buffer_bytes;        /*!< bytes per row TEXT run */
    const gsp_message_list_config_t *message_config;
    esp_gsp_message_source_t message_source;
    gsp_rect_t region;              /*!< drag-scroll viewport */
    uint32_t total;                 /*!< desired item count */
    int32_t saved_offset;           /*!< scroll offset across parking */
    uint16_t template_id;
    uint16_t row_height;
    uint16_t columns;
    uint16_t cell_width;
    uint16_t column_gap;
    uint16_t row_gap;
    uint16_t slot_count;            /*!< active row slots */
    uint16_t text_slot_count;       /*!< dynamic TEXT fields per row */
    uint16_t transform_group_ref;   /*!< parent transform group + 1 */
    /* Nearest ancestor visibility slot from the generated config
     * (GSP_LIST_VIS_SLOT_NONE for advanced binds): hidden pages'
     * lists auto-park, no manual set_total(0) needed. */
    uint16_t vis_slot;
    uint16_t value_bind;           /*!< selected business index, or UINT16_MAX */
    uint32_t selected;
    uint32_t paint_after;
    uint16_t scene;
    uint8_t reserved;             /*!< producer-side handle reservation */
    bool used;
    bool snap;
    bool fade;
    bool scroll_snapshot;
    bool cyclic;
    bool wheel;
    bool message_update_active;
    bool message_refresh_pending;
    bool paint_mirrored;
    uint32_t snapshot_generation;  /*!< invalidates frozen endpoint pixels */
    uint32_t fade_color;            /*!< roller fade bands (native) */
    /* Active instance cache (generation-validated; NULL when
     * parked). Resolve through esp_gsp_list_state(). */
    gsp_component_instance_t *instance;
    uint16_t instance_gen;
} esp_gsp_list_slot_t;

/* Portable UI state. OS scheduling, command queues, display handles and
 * presentation state are owned by the attached platform adapter. */
typedef struct {
    esp_gsp_event_cb_t event_cb;
    void *event_user_ctx;
    esp_gsp_component_event_cb_t component_event_cb;
    void *component_event_user_ctx;
    esp_gsp_pointer_observer_cb_t pointer_observer_cb;
    void *pointer_observer_user_ctx;
    esp_gsp_pinch_cb_t pinch_cb;
    void *pinch_user_ctx;
    esp_gsp_input_interceptor_cb_t input_interceptor_cb;
    void *input_interceptor_user_ctx;
    esp_gsp_overlay_contributor_cb_t overlay_contributor_cb;
    void *overlay_contributor_user_ctx;
    esp_gsp_extension_cb_t extension_cb;
    void *extension_ctx;
    uint32_t extension_event_mask;
    uint32_t extension_canvas_mask;
} gsp_app_callback_ops_t;

struct gsp_ui_core {
    gsp_app_config_t config;
    esp_gsp_effective_config_t effective_config;
    bool platform_config_acquired;
    gsp_context_t *scenes;
    bool *scene_ready;
    uint16_t scene_count;
    uint16_t current_scene;
    /* Published only at scene switches; file requests can capture it safely. */
    atomic_uint asset_scene;
    atomic_uint asset_scene_generation;
    /* Cap on decoded imagery held across all scenes; zero releases each
     * scene as soon as it leaves the screen. */
    size_t scene_retain_bytes;
    uint32_t *scene_used_at;
    uint32_t scene_use_clock;
    /* Set once teardown starts so queued work cannot borrow resources
     * that will never be released. */
    bool shutting_down;
    bool extension_lifecycle_ready;
    const gsp_app_adapter_ops_t *adapter_ops;
    void *adapter_ctx;
    _Atomic(gsp_update_receipts_t *) update_receipts;
    /* Frame clock: the platform owner samples it once per loop
     * iteration; every core-side event/deadline timestamp reads it
     * (deterministic host tests inject synthetic time). Intra-frame
     * DURATION measurements keep the platform timer. */
    int64_t now_us;
    esp_gsp_timer_slot_t *timers;

    atomic_bool canvas_draw_active;
    atomic_bool extension_callback_active;
    /* Application callbacks delivered from the render task. Keep callback
     * slots together so registration lifetime rules have one internal home;
     * public APIs and dispatch order remain unchanged. The atomic flag stays
     * in its original position to avoid perturbing hot state layout. */
    gsp_app_callback_ops_t callbacks;

    /* Input normalization and gesture routing. */
    struct {
        /* Gesture arbitration (tap / capture / scroll / swipe) lives
         * in the portable classifier; a CAPTURE claim routes to the
         * manager's capture owner. */
        gsp_pointer_t classifier;
        gsp_multitouch_t multitouch;
        int32_t pinch_center_x;
        int32_t pinch_center_y;
        uint32_t pinch_scale_q16;
        int64_t press_time_us;
        int64_t long_press_since_us;
        /* True while the hardware touch owns the pointer state
         * machine: synthetic input is dropped so a real finger always
         * wins. */
        bool from_hw;
        gsp_instance_handle_t tap_instance; /*!< hit under last tap */
        /* Pointer callbacks follow the pressed source, including captured range releases. */
        gsp_hit_result_t press_hit;
        uint16_t range_bind;
        uint16_t range_property;
        uint16_t range_entry;
        uint16_t press_scene;
        uint16_t press_list;
        uint32_t press_item;
        uint16_t range_generation;
        uint32_t range_last_value;
        bool range_notify;
        bool long_press_pending;
        bool long_press_fired;
        bool press_cancelled; /*!< click cancelled; parent arbitration remains active */
        bool press_finished;
        bool press_outside;

        /* One vertical scroll region (app-registered viewport). */
        gsp_rect_t scroll_region;
        esp_gsp_scroll_cb_t scroll_cb;
        void *scroll_user_ctx;
        uint16_t page_flow_origin;
        uint16_t page_flow_target;
        gsp_component_instance_t *gesture;
        uint16_t gesture_generation;
        uint16_t gesture_group;
        uint32_t gesture_visibility_revision;
        uint8_t gesture_kind;
        bool gesture_towards_next;
        gsp_swipe_kinetics_t gesture_kinetics;
        bool adapter_visual_transition;
        uint16_t adapter_visual_transition_entry;
        uint16_t adapter_visual_transition_generation;
        int32_t adapter_visual_transition_from;
        int32_t adapter_visual_transition_target;
        uint16_t adapter_visual_transition_last_progress;
        bool adapter_visual_transition_commit;
        int64_t adapter_visual_transition_retry_after_us;
        uint32_t adapter_visual_transition_retry_delay_us;
    } input;

    /* Typed drivers own transient component state. Instances activate
     * on demand and recycle when done. Anonymous entries use bind ids
     * when no generated component directory is registered. */
    struct {
        gsp_component_manager_t manager;
        gsp_component_instance_t *instances;
        /* Persistent overflow scratch for configured pools above the inline
         * transaction threshold; NULL on the normal stack-only path. */
        gsp_pending_update_t *transaction_updates;
        /* Size classes: transient behaviors (drags, tweens) in the
         * small pool; visible-list state in the large one. */
        uint8_t *extensions;
        uint8_t *list_extensions;
    } components;

    /* Scene transitions, swipe state and pending navigation. */
    struct {
        esp_gsp_transition_t pending_transition;
        int pending_goto;
        uint16_t drag_target;   /*!< swipe target scene */
        uint16_t drag_progress; /*!< host/SDL fallback progress */
        gsp_transition_kind_t drag_kind;
        /* Swipe visual, chosen by the app rather than the bundle. */
        esp_gsp_swipe_transition_t swipe_transition;
        uint8_t swipe_fade_black_point_percent;
        gsp_swipe_kinetics_t swipe_kinetics;
        uint16_t drag_presented_progress;
        bool settle_commit;
        bool drag_fallback;
        bool adapter_transition;
    } nav;

    /* Soft-keyboard aggregation: key CALL events edit text and
     * republish text_bind; the app only sees the OK key.
     * ESP_GSP_KEYBOARD_NONE while detached. */
    struct {
        uint16_t action_id;
        uint16_t text_bind;
        char *text;
        size_t capacity;
        char inline_text[ESP_GSP_TEXT_CAPACITY + 1];
    } kb;

    /* Transient chrome rebuilt on state changes: pressed-target scrim
     * and text-input caret. */
    struct {
        gsp_overlay_t overlay;
        gsp_overlay_command_t *storage;
        bool press_feedback;
        bool press_feedback_disabled;
        esp_gsp_drag_snapshot_stats_t drag_snapshot_stats;
        gsp_rect_t scrim_rect;
        uint16_t cursor_bind;
        char *cursor_text;
        size_t cursor_capacity;
        char cursor_inline_text[ESP_GSP_TEXT_CAPACITY + 1];
        bool cursor_phase;
        int64_t cursor_next_us;
    } chrome;

    /* UI-facing List/Wheel slots and the active drag owner. */
    struct {
        esp_gsp_list_slot_t *pool;
        int32_t active;         /*!< list owning the current drag */
        int32_t snapshot;
        int32_t snapshot_from;
        int32_t snapshot_target;
        int32_t snapshot_offset;
        uint16_t snapshot_progress;
        uint64_t snapshot_last_progress_us;
        bool snapshot_indicator_visible;
        bool snapshot_blocked;
        bool snapshot_reused_window;
        int32_t snapshot_cache_list;
        uint32_t snapshot_cache_generation;
        int32_t snapshot_cache_from;
        int32_t snapshot_cache_target;
        gsp_rect_t snapshot_cache_viewport;
        /* Held MessageList captures keep the repeater at the captured
         * offset to avoid rebinding expensive rows on every move. */
        bool snapshot_hold_repeater;
    } lists;

    /* Dynamic text, image, canvas and template-widget pools. */
    esp_gsp_text_slot_t *text_slots;   /*!< heap-allocated, text_slot_count */
    size_t text_slot_count;
    struct esp_gsp_dropdown_state *dropdowns; /*!< allocated only after a text replacement */
    size_t image_slot_count;
    esp_gsp_image_slot_t *image_slots;
    esp_gsp_canvas_slot_t *canvas_slots;
    esp_gsp_widget_slot_t *widgets;
    struct {
        uint32_t image_requests;
        uint32_t image_queued;
        uint32_t image_published;
        uint32_t image_failures;
        uint32_t image_cancelled;
        uint64_t image_probe_us;
        uint32_t canvas_applied;
        uint32_t canvas_rejected;
    } media;

#if GSP_PROFILE_SERVICE
    /* Diagnostic counters reported by esp_gsp_property_stats(). Absent from
     * an ordinary build, so neither this storage nor its updates exist. */
    gsp_property_probe_t property_probe;
#endif

    /* Runtime and compiled animation state. */
    esp_gsp_anim_slot_t *animations;
    gsp_anim_t frame_anim;
    bool frame_anim_open;
    /* Compiled animations (GRB anim_qoi), auto-played per scene. */
    gsp_anim_t *asset_anims;
    uint8_t asset_anim_count;
    uint32_t asset_anim_visibility_revision;
    uint32_t closed_animation_patches;
    uint64_t closed_animation_work_us;
    uint32_t closed_max_animation_work_us;
    uint64_t closed_animation_patch_pixels;

    /* Keep optional-effect state after the existing UI hot fields. */
    int64_t procedural_last_us;
    uint32_t procedural_visibility_revision;
    bool procedural_active;
    bool procedural_indexed;
    uint32_t procedural_first, procedural_end;
    struct {
        bool captured, moved;
        uint16_t scene;
        uint32_t command, phase, clicked;
        int32_t press_x, press_y, last_x, velocity;
        int64_t last_us;
        gsp_rect_t box;
    } carousel;

    /* Failure-only diagnostics stay after existing hot fields, but before
     * the optional font tail so their layout does not depend on FreeType.
     * A try-lock never spins on a preempted application task. */
    atomic_bool update_error_busy;
    atomic_uint_least32_t update_error_count;
    atomic_uint_least32_t update_error_dropped;
    esp_gsp_update_error_stats_t update_error;

    /* Fonts (platform resources, stay). */
#if GSP_ENABLE_FREETYPE
    const gsp_dynamic_font_provider_t *dynamic_font_provider;
    FT_Library ft_library;
    FT_Face ft_face;
    /* One FT_Size per distinct compiled font size. A single face can
     * only be metric-configured for one size at a time, so shaping a
     * scene that mixes sizes has to switch the active size per glyph
     * rather than rasterize everything at the default. */
    FT_Size ft_sizes[GSP_CONTEXT_MAX_FONTS];
    uint16_t ft_size_px[GSP_CONTEXT_MAX_FONTS];
    size_t ft_size_count;
    uint16_t ft_active_size_px;
    gsp_freetype_t freetype;
    /* Glyph cache is sized from the app config; the compile-time
     * constants are only the fallback. */
    gsp_freetype_cache_entry_t *ft_entries;
    size_t ft_entry_capacity;
    uint16_t ft_glyph_max_px;
    uint8_t *ft_pixels;
    gsp_font_chain_t *font_chains;
    bool ttf_ready;
#endif
    const gsp_gfb_t *font;
    uint16_t font_ref;

};

/* Timer metadata and publication of its 64-bit clock use the adapter's short
 * state mutex. Never hold it while invoking callbacks or submitting commands.
 * Unattached portable apps keep their existing single-threaded contract. */
static inline void esp_gsp_timer_slots_lock(const gsp_ui_core_t *gsp)
{
    if (gsp->adapter_ops != NULL && gsp->adapter_ops->state_lock != NULL) {
        gsp->adapter_ops->state_lock(gsp->adapter_ctx);
    }
}

static inline void esp_gsp_timer_slots_unlock(const gsp_ui_core_t *gsp)
{
    if (gsp->adapter_ops != NULL && gsp->adapter_ops->state_unlock != NULL) {
        gsp->adapter_ops->state_unlock(gsp->adapter_ctx);
    }
}

static inline const esp_gsp_effective_config_t *gsp_effective_config(
    const gsp_ui_core_t *gsp)
{
    return gsp != NULL ? &gsp->effective_config : NULL;
}

/* Preserve the caller's constant field ID through the config accessor. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline size_t gsp_effective_capacity(
    const gsp_ui_core_t *gsp, esp_gsp_config_field_id_t field)
{
    return (size_t)esp_gsp_config_value(gsp_effective_config(gsp), field);
}

static inline gsp_context_t *esp_gsp_current_ctx(gsp_ui_core_t *gsp)
{
    return &gsp->scenes[gsp->current_scene];
}

static inline bool esp_gsp_decode_is_async(const gsp_ui_core_t *gsp)
{
    return gsp->adapter_ops != NULL &&
           gsp->adapter_ops->decode_async != NULL &&
           gsp->adapter_ops->decode_async(gsp->adapter_ctx);
}

static inline void esp_gsp_decode_kick(gsp_ui_core_t *gsp)
{
    if (gsp->adapter_ops != NULL &&
            gsp->adapter_ops->decode_kick != NULL) {
        gsp->adapter_ops->decode_kick(gsp->adapter_ctx);
    }
}

/* Cross-file UI service internals. */
esp_gsp_err_t esp_gsp_send_command(esp_gsp_handle_t gsp,
                                   const esp_gsp_cmd_t *command);
esp_gsp_err_t esp_gsp_send_text_command(esp_gsp_handle_t gsp,
                                        esp_gsp_cmd_type_t type,
                                        uint16_t bind, uint16_t slot,
                                        const char *utf8);
esp_gsp_err_t esp_gsp_text_command_init(esp_gsp_cmd_t *command,
                                        esp_gsp_cmd_type_t type,
                                        uint16_t bind, uint16_t slot,
                                        const char *utf8);
gsp_err_t esp_gsp_text_buffer_prepare(esp_gsp_text_slot_t *slot,
                                      const char *utf8,
                                      esp_gsp_text_buffer_t *out_buffer);
size_t esp_gsp_text_run_capacity(const char *utf8);
gsp_err_t esp_gsp_text_shape_layout(gsp_ui_core_t *gsp,
                                    const gsp_bind_text_layout_t *layout, const char *text,
                                    void *buffer, size_t capacity, size_t *out_size);
gsp_err_t esp_gsp_dropdown_apply(gsp_ui_core_t *gsp, const esp_gsp_cmd_t *command);
void esp_gsp_dropdown_command_release(const esp_gsp_cmd_t *command);
void esp_gsp_dropdown_deinit(gsp_ui_core_t *gsp);
void esp_gsp_dropdown_reconcile_selected(gsp_ui_core_t *gsp);
gsp_err_t esp_gsp_dropdown_sync_selected(gsp_ui_core_t *gsp,
        const gsp_component_entry_t *entry, uint32_t selected);
size_t esp_gsp_dropdown_selected_count(
    const gsp_component_directory_t *directory, gsp_runtime_t *runtime,
    const gsp_component_property_update_t *updates, size_t count);
gsp_err_t esp_gsp_dropdown_stage_selected(gsp_ui_core_t *gsp,
        const gsp_component_directory_t *directory,
        const gsp_component_property_update_t *updates, size_t count,
        size_t selected_count, gsp_update_transaction_t *transaction,
        esp_gsp_dropdown_selection_batch_t *prepared);
void esp_gsp_dropdown_finish_selected(gsp_ui_core_t *gsp,
                                      esp_gsp_dropdown_selection_batch_t *prepared, bool committed);
gsp_err_t esp_gsp_dropdown_action_text(gsp_ui_core_t *gsp,
                                       uint16_t text_bind, uint32_t selected);
void esp_gsp_record_dropdown_error(gsp_ui_core_t *gsp, uint16_t scene,
                                   esp_gsp_update_error_stage_t stage, gsp_err_t error, uint32_t key);
void esp_gsp_text_buffer_commit(esp_gsp_text_slot_t *slot,
                                esp_gsp_text_buffer_t *buffer);
void esp_gsp_text_buffer_discard(esp_gsp_text_buffer_t *buffer);
void esp_gsp_keyboard_lock(gsp_ui_core_t *gsp);
void esp_gsp_keyboard_unlock(gsp_ui_core_t *gsp);
/* Portable core init (state, drivers, scenes from bundle bytes,
 * fonts); backend/platform pieces stay outside the UI core. The host
 * adapter calls this on a zeroed struct. */
esp_gsp_err_t esp_gsp_ui_init(gsp_ui_core_t *gsp,
                              const gsp_app_config_t *config);
gsp_ui_core_t *gsp_app_ui(gsp_app_t *app);
gsp_app_t *gsp_app_from_ui(gsp_ui_core_t *ui);
void gsp_ui_core_attach_adapter(gsp_ui_core_t *core,
                                const gsp_app_adapter_ops_t *ops,
                                void *ctx);
void gsp_ui_core_prepare_scene(gsp_ui_core_t *core, uint16_t target);
void gsp_ui_core_switch_scene(gsp_ui_core_t *core, uint16_t target);
/* Core apply: the render-task dispatch chain behind the queue.
 * Render-task context only (services use it for same-frame steps). */
void esp_gsp_ui_apply(gsp_ui_core_t *gsp,
                      const esp_gsp_cmd_t *command);
/* Core step: per-iteration state advance on the frame clock (timers,
 * tweens, driver ticks, list reconcile, decode pump, caret). The
 * platform loop handles pending scene transitions before calling it
 * (transitions render). */
void esp_gsp_ui_step(gsp_ui_core_t *gsp);
void esp_gsp_transform_snapshot_step(gsp_ui_core_t *gsp);
void esp_gsp_snapshot_reconcile_content(gsp_ui_core_t *gsp);
void esp_gsp_snapshot_content_changed_active(gsp_ui_core_t *gsp,
        uint16_t scene);
static inline void esp_gsp_snapshot_content_changed(gsp_ui_core_t *gsp,
        uint16_t scene)
{
    if (gsp->input.adapter_visual_transition || gsp->lists.snapshot >= 0 ||
            gsp->lists.snapshot_cache_list >= 0) {
        esp_gsp_snapshot_content_changed_active(gsp, scene);
    }
}
gsp_err_t esp_gsp_page_flow_set_page_tracked(
    gsp_ui_core_t *gsp, gsp_component_instance_t *instance,
    const gsp_page_flow_config_t *config, uint16_t page, bool animated);
gsp_err_t esp_gsp_drawer_set_open_tracked(
    gsp_ui_core_t *gsp, gsp_component_instance_t *instance,
    const gsp_drawer_config_t *config, bool open, bool animated);
gsp_err_t esp_gsp_stack_view_push_tracked(
    gsp_ui_core_t *gsp, gsp_component_instance_t *instance,
    const gsp_stack_view_config_t *config, uint16_t page, bool animated);
gsp_err_t esp_gsp_stack_view_pop_tracked(
    gsp_ui_core_t *gsp, gsp_component_instance_t *instance,
    const gsp_stack_view_config_t *config, bool animated);
bool esp_gsp_list_indicator_region(gsp_ui_core_t *gsp,
                                   const esp_gsp_list_slot_t *list,
                                   gsp_rect_t *out_region);
void esp_gsp_overlay_rebuild(gsp_ui_core_t *gsp);
/* Pointer samples shared by the hardware touch poll and synthetic
 * input; coordinates are LOGICAL (scene space, rotation mapped). */
void esp_gsp_process_pointer(gsp_ui_core_t *gsp, int32_t touch_x,
                             int32_t touch_y, bool pressed);
void esp_gsp_process_touch_frame(
    gsp_ui_core_t *gsp, const gsp_platform_touch_frame_t *frame);
void esp_gsp_swipe_step(gsp_ui_core_t *gsp);
/* Binds the gesture classifier to the UI effect callbacks. */
void esp_gsp_input_init(gsp_ui_core_t *gsp);
void esp_gsp_input_scene_exit(gsp_ui_core_t *gsp);
void esp_gsp_input_reconcile(gsp_ui_core_t *gsp);
void esp_gsp_input_step(gsp_ui_core_t *gsp);

/* Scene-switch funnel: cancels active component instances into the
 * OUTGOING scene and rebinds the manager to the new runtime. */
void esp_gsp_components_scene_switch(gsp_ui_core_t *gsp,
                                     uint16_t next_scene);

/* Registered component directory for @p scene (NULL without
 * registration). */
const gsp_component_directory_t *esp_gsp_scene_directory(
    const gsp_ui_core_t *gsp, uint16_t scene);

/* Canvas/image service (esp_gsp_canvas.c): user frame sinks and
 * runtime-published encoded images. */
bool esp_gsp_canvas_apply_command(gsp_ui_core_t *gsp,
                                  const esp_gsp_cmd_t *command);
/* Hands retired canvas frames back to their producers (called by the
 * render loop once the replacing frame has rendered). */
void esp_gsp_canvas_fire_retired(gsp_ui_core_t *gsp);
bool esp_gsp_canvas_has_retired(const gsp_ui_core_t *gsp);
/* Returns every frame a scene still borrows and restores its compiled
 * placeholder views: a scene leaving the screen must not keep producer
 * buffers alive, and the slot pool only holds ESP_GSP_CANVAS_SLOTS. */
void esp_gsp_canvas_close_scene(gsp_ui_core_t *gsp, uint16_t scene);
void esp_gsp_canvas_close_all(gsp_ui_core_t *gsp);

/* Adopts a decoded dynamic-image generation: moves the bind onto it and
 * retires the one it replaces. Registered as the context image_ready
 * hook, so a failed decode simply leaves the previous image up. */
void esp_gsp_image_decode_ready(void *user_ctx, gsp_runtime_t *runtime,
                                uint32_t resource_id, gsp_err_t status);
/* Returns every encoded payload the dynamic image slots still hold, so
 * shutdown honours the release-exactly-once contract. */
void esp_gsp_image_close_all(gsp_ui_core_t *gsp);
/* Settle external requests before scene-cache eviction can drop their jobs.
 * Already published images and ordinary set_image requests stay intact. */
void esp_gsp_image_cancel_scene_assets(gsp_ui_core_t *gsp, uint16_t scene);
void esp_gsp_image_reconcile_row(gsp_ui_core_t *gsp, esp_gsp_row_t row);
void esp_gsp_image_close_list(gsp_ui_core_t *gsp, uint16_t list);
void esp_gsp_image_close_widget(gsp_ui_core_t *gsp, uint16_t widget);

/* Text service (esp_gsp_text.c): shaping, caret, keyboard, dynamic
 * (FreeType) fallback. */
bool esp_gsp_text_apply_command(gsp_ui_core_t *gsp,
                                const esp_gsp_cmd_t *command);
esp_gsp_err_t esp_gsp_text_setup_dynamic(gsp_ui_core_t *gsp,
        const gsp_app_config_t *config);
#if GSP_ENABLE_FREETYPE
/* Releases every FT_Size, the face, the library and the glyph storage.
 * Safe to call on a partially initialized setup. */
void esp_gsp_text_deinit_dynamic(gsp_ui_core_t *gsp);
#endif

/* Animation service (esp_gsp_anim.c): value/color tweens, EAF
 * playback, compiled-animation auto-players. */
bool esp_gsp_anim_apply_command(gsp_ui_core_t *gsp,
                                const esp_gsp_cmd_t *command);
void esp_gsp_anims_run(gsp_ui_core_t *gsp);
bool esp_gsp_carousel_press(gsp_ui_core_t *gsp, uint32_t command, int32_t x, int32_t y);
bool esp_gsp_carousel_move(gsp_ui_core_t *gsp, int32_t x);
bool esp_gsp_carousel_release(gsp_ui_core_t *gsp, bool cancel);
void esp_gsp_procedural_step(gsp_ui_core_t *gsp);
void esp_gsp_asset_anims_open(gsp_ui_core_t *gsp);
void esp_gsp_asset_anims_reconcile(gsp_ui_core_t *gsp);
void esp_gsp_asset_anims_close(gsp_ui_core_t *gsp);

/* Widget service (esp_gsp_widget.c): the dynamic template-instance
 * pool behind the ADVANCED esp_gsp_widget_* API. */
bool esp_gsp_widget_apply_command(gsp_ui_core_t *gsp,
                                  const esp_gsp_cmd_t *command);
esp_gsp_widget_slot_t *esp_gsp_widget_resolve(gsp_ui_core_t *gsp,
        uint16_t handle,
        uint16_t *out_index);
uint16_t esp_gsp_widget_auto_slot(gsp_ui_core_t *gsp,
                                  const esp_gsp_widget_slot_t *entry,
                                  uint32_t kind_code);
/* Glyph-run slot pool, shared by bind text and widget text. */
esp_gsp_text_slot_t *esp_gsp_claim_text_slot(gsp_ui_core_t *gsp,
        uint64_t key);

/* List service (esp_gsp_list.c). */
bool esp_gsp_list_apply_command(gsp_ui_core_t *gsp,
                                const esp_gsp_cmd_t *command);
/* Visibility reconcile (render loop, once per iteration): activates
 * viewports whose page became visible, parks hidden ones (rows and
 * extension recycle; the offset persists in the binding). */
void esp_gsp_lists_reconcile(gsp_ui_core_t *gsp);
void esp_gsp_list_snapshot_move(gsp_ui_core_t *gsp, uint16_t index,
                                int32_t old_offset, int32_t new_offset);
void esp_gsp_list_snapshot_step(gsp_ui_core_t *gsp);
void esp_gsp_list_snapshot_cancel(gsp_ui_core_t *gsp);
void esp_gsp_list_content_changed(gsp_ui_core_t *gsp, uint16_t index);
void esp_gsp_lists_activate_group(gsp_ui_core_t *gsp, uint16_t group_id,
                                  gsp_rect_t viewport,
                                  int32_t first_offset,
                                  int32_t second_offset, bool vertical);
/* Parks every active list (scene-switch funnel, BEFORE cancel_all,
 * so offsets persist). */
void esp_gsp_lists_park_all(gsp_ui_core_t *gsp);
/* The live extension of a bound list, or NULL while parked. */
gsp_list_state_t *esp_gsp_list_state(gsp_ui_core_t *gsp,
                                     const esp_gsp_list_slot_t *list);
/* True unless the list's compiled visibility gate says its page is
 * hidden (a hidden page's list must not swallow drags). */
bool esp_gsp_list_page_visible(gsp_ui_core_t *gsp,
                               const esp_gsp_list_slot_t *list);
bool esp_gsp_list_visible_region(gsp_ui_core_t *gsp,
                                 const esp_gsp_list_slot_t *list,
                                 gsp_rect_t *out_region);
/* First template slot whose baked kind matches (ESP_GSP_SLOT_AUTO
 * resolution for the convenience setters). */
uint16_t esp_gsp_template_auto_slot(gsp_ui_core_t *gsp,
                                    uint16_t scene_index,
                                    uint16_t template_id,
                                    uint32_t kind_code);

void esp_gsp_snapshot_skip(gsp_ui_core_t *gsp, esp_gsp_snapshot_reason_t reason);

/* Recheck entries only on validation/staging failure, never on the hot path.
 * semantic selects property-key batches versus compatibility-property batches. */
void esp_gsp_record_update_error(
    gsp_ui_core_t *gsp, uint16_t scene, esp_gsp_update_error_stage_t stage,
    esp_gsp_err_t error, gsp_runtime_t *runtime,
    const gsp_component_directory_t *directory, const void *updates,
    size_t count, bool semantic, bool inspect_entries);
void esp_gsp_record_update_apply_error(
    gsp_ui_core_t *gsp, uint16_t scene, gsp_err_t error,
    gsp_runtime_t *runtime, const gsp_component_directory_t *directory,
    const void *updates, size_t count, bool semantic, bool inspect_entries);

esp_gsp_err_t esp_gsp_submit_asset_target(esp_gsp_handle_t gsp,
        const esp_gsp_asset_target_t *target, const gsp_resource_view_t *view,
        const esp_gsp_image_options_t *options, uint16_t scene, uint32_t scene_generation);
/* Simulator/internal counterpart of widget_set_image with lifecycle options. */
esp_gsp_err_t esp_gsp_widget_set_image_ex_private(esp_gsp_handle_t gsp,
        esp_gsp_widget_t widget, uint16_t resource_slot,
        const void *data, size_t size, const esp_gsp_image_options_t *options);

/* Widget capacity is runtime-configurable, so the handle layout reserves the
 * full uint8_t index range and leaves the upper byte for generations. */
#define ESP_GSP_WIDGET_INDEX_BITS 8U
#define ESP_GSP_WIDGET_INDEX_MASK \
    ((UINT16_C(1) << ESP_GSP_WIDGET_INDEX_BITS) - 1U)
#define ESP_GSP_WIDGET_GENERATION_MASK \
    (UINT16_MAX >> ESP_GSP_WIDGET_INDEX_BITS)

/* Internal asset-loader handoff; view/payload validated before submission. */
esp_gsp_err_t esp_gsp_submit_asset_image(esp_gsp_handle_t gsp, uint16_t bind,
        const gsp_resource_view_t *view, const esp_gsp_image_options_t *options, uint16_t scene, uint32_t scene_generation);

#ifdef __cplusplus
}
#endif

static inline bool gsp_updates_active(const gsp_ui_core_t *core)
{
    gsp_update_receipts_t *tracker = atomic_load_explicit(
                                         &core->update_receipts, memory_order_acquire);
    return tracker != NULL && atomic_load_explicit(&tracker->count, memory_order_acquire) != 0;
}
