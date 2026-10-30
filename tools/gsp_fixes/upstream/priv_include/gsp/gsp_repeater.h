/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_instance.h"
#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called when a slot starts showing a new data item. The implementation
 * binds item data onto the instance (gsp_instance_set_state_i32 etc.);
 * instance position and visibility are managed by the repeater.
 */
typedef gsp_err_t (*gsp_repeater_bind_cb_t)(
    void *user_ctx,
    gsp_instance_pool_t *pool,
    gsp_instance_handle_t instance,
    uint32_t item_index);

typedef struct {
    uint16_t template_id;
    int16_t origin_x;
    int16_t origin_y;
    uint16_t row_height;
    /* Fixed-column mode (Grid): zero columns keeps the legacy one-column
     * List behavior. Variable-height rows require one column. */
    uint16_t columns;
    uint16_t item_width;
    uint16_t column_gap;
    uint16_t row_gap;
    uint16_t viewport_height;
    /* Viewport width for the per-row clip; 0 uses the template width.
     * Rows are clipped to (origin, viewport) so partially scrolled rows
     * never overdraw the list bounds. */
    uint16_t viewport_width;
    uint16_t overscan_rows;
    uint16_t z_order;
    uint16_t transform_group_ref;
    /* Smooth-scroll fast path: surviving rows move without dirty marks
     * and each set_scroll emits a gsp_runtime_request_scroll() so the
     * presenter copies their pixels from the previous frame and
     * rasterizes only the exposed band (an incremental display backend
     * consumes this request). Presenters that render full frames or call
     * gsp_runtime_take_scroll() are covered; a custom incremental
     * presenter that does neither must keep this off. */
    bool scroll_blit;
    bool paint_anchored;
    uint32_t paint_after;
    /* Optional variable-row offsets (total_count + 1 monotonically
     * increasing entries). NULL keeps the fixed-row fast path. row_height
     * remains the minimum row height used for the instance budget. */
    const uint32_t *item_offsets;
    size_t item_offset_count;
    gsp_repeater_bind_cb_t bind_item;
    void *user_ctx;
    gsp_instance_handle_t *slot_storage;
    uint32_t *item_storage;
    size_t slot_count;
} gsp_repeater_config_t;

/**
 * Fixed-row-height virtual list over a template instance pool: N data items
 * are shown through slot_count recycled instances (visible rows plus
 * overscan), never one instance per item. Scrolling reuses slots and only
 * rebinds rows entering the window; positions update in O(slots).
 */
typedef struct gsp_repeater {
    gsp_instance_pool_t *pool;
    gsp_repeater_config_t config;
    gsp_rect_t viewport_clip;
    uint32_t total_count;
    int32_t scroll_offset;
} gsp_repeater_t;

/**
 * slot_count must cover the worst visible window plus overscan. Fixed rows
 * use ceil(viewport_height / row_height) plus one partial row; variable rows
 * derive the exact bound from item_offsets. slot_storage/item_storage are
 * caller-owned arrays of slot_count entries.
 */
gsp_err_t gsp_repeater_init(gsp_repeater_t *repeater,
                            gsp_instance_pool_t *pool,
                            const gsp_repeater_config_t *config);

/** Sets the data size and refreshes the window. */
gsp_err_t gsp_repeater_set_total(gsp_repeater_t *repeater,
                                 uint32_t total_count);

/** Replaces the variable-row offset index and reconciles the viewport. */
gsp_err_t gsp_repeater_set_item_offsets(gsp_repeater_t *repeater,
                                        const uint32_t *item_offsets,
                                        size_t item_offset_count);

/** Checks an offset index and its visible-row capacity without changing state. */
gsp_err_t gsp_repeater_check_variable_layout(
    const gsp_repeater_config_t *config, const uint32_t *item_offsets,
    size_t item_offset_count, uint32_t total_count);

/** Atomically replaces total, offsets and scroll before one reconcile. */
gsp_err_t gsp_repeater_set_variable_layout(
    gsp_repeater_t *repeater, const uint32_t *item_offsets,
    size_t item_offset_count, uint32_t total_count, int32_t scroll_offset);

/**
 * Applies one scroll offset (pixels, clamped to content) and reconciles the
 * window: exiting rows are recycled, entering rows are bound via bind_item,
 * surviving rows only move. Call at most once per frame with the latest
 * offset; intermediate offsets must not be queued.
 */
gsp_err_t gsp_repeater_set_scroll(gsp_repeater_t *repeater,
                                  int32_t scroll_offset);

/** Rebinds every populated slot, e.g. after the backing data changed. */
gsp_err_t gsp_repeater_refresh(gsp_repeater_t *repeater);

/** Rebinds populated slots whose item index is marked nonzero. */
gsp_err_t gsp_repeater_refresh_items(gsp_repeater_t *repeater,
                                     const uint8_t *changed_items,
                                     size_t item_count);

void gsp_repeater_deinit(gsp_repeater_t *repeater);

#ifdef __cplusplus
}
#endif
