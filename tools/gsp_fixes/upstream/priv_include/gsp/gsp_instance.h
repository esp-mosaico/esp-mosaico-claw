/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_types.h"
#include "gsp/gsp_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/* gsp_instance_handle_t / GSP_INSTANCE_HANDLE_INVALID live in
 * gsp_types.h (the handle is part of the public row API). */

#define GSP_INSTANCE_FLAG_ACTIVE 0x1U
#define GSP_INSTANCE_FLAG_VISIBLE 0x2U
#define GSP_INSTANCE_FLAG_ANCHORED 0x4U
#define GSP_INSTANCE_TRANSFORM_NONE UINT16_MAX

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t template_index;
    uint16_t z_order;
    uint16_t generation;
    union {
        uint16_t next_free;   /* Inactive free-list link. */
        uint16_t prev_render; /* Active: head.prev_render is the tail. */
    };
    uint16_t next_render;
    uint16_t transform_group;
    uint8_t flags;
    uint8_t reserved;
    /* Optional screen-space viewport clip applied to every command of
     * the instance (clip_x2 <= clip_x1 disables it). The repeater sets
     * this to the list viewport so partially scrolled rows do not
     * overdraw the list bounds. */
    int16_t clip_x1;
    int16_t clip_y1;
    int16_t clip_x2;
    int16_t clip_y2;
} gsp_instance_slot_t;

/* Scene commands occupy at least 32 bytes in a uint32-sized container, so
 * 27 bits cover every valid command boundary. Reuse private ordering bytes
 * to keep the slot size unchanged. Unanchored slots retain ordinary z_order. */
static inline uint32_t gsp_instance_paint_after(const gsp_instance_slot_t *slot)
{
    return (slot->flags & GSP_INSTANCE_FLAG_ANCHORED) != 0 ?
           (uint32_t)slot->z_order | ((uint32_t)slot->reserved << 16) |
           ((uint32_t)(slot->flags >> 5) << 24) : UINT32_MAX;
}

/**
 * Fixed-capacity template instance pool over caller-owned storage. Slot
 * state lives at state_storage[slot * states_per_slot]; states_per_slot
 * must cover the largest template state_count that will be instantiated.
 * Creation checks the template quota and inserts into paint order. Removal
 * uses the reverse link in O(1); handles check the slot generation.
 * Forward paint links end at UINT16_MAX; reverse traversal is bounded by
 * active_count because the head points back to the paint tail.
 *
 * Every mutation marks the translated template bbox dirty and bumps the
 * runtime revision, so it must not run while a prepared frame is rendering.
 */
typedef struct gsp_instance_pool {
    gsp_runtime_t *runtime;
    gsp_instance_slot_t *slots;
    size_t slot_capacity;
    gsp_state_t *states;
    size_t states_per_slot;
    uint16_t free_head;
    uint16_t render_head;
    uint16_t active_count;
    uint16_t anchored_count;
} gsp_instance_pool_t;

gsp_err_t gsp_instance_pool_init(
    gsp_instance_pool_t *pool,
    gsp_runtime_t *runtime,
    gsp_instance_slot_t *slot_storage,
    size_t slot_capacity,
    gsp_state_t *state_storage,
    size_t state_capacity,
    size_t states_per_slot);

/**
 * Instantiates a template at (x, y) with instance state copied from the
 * template defaults. Fails with GSP_ERR_LIMIT_EXCEEDED when the pool or the
 * template max_instances quota is exhausted; never allocates.
 */
gsp_err_t gsp_instance_create(
    gsp_instance_pool_t *pool,
    uint16_t template_id,
    int16_t x,
    int16_t y,
    uint16_t z_order,
    gsp_instance_handle_t *out_handle);

gsp_err_t gsp_instance_create_transformed(
    gsp_instance_pool_t *pool,
    uint16_t template_id,
    int16_t x,
    int16_t y,
    uint16_t z_order,
    uint16_t transform_group,
    gsp_instance_handle_t *out_handle);

gsp_err_t gsp_instance_destroy(gsp_instance_pool_t *pool,
                               gsp_instance_handle_t handle);

/** Attach an owned instance to its canonical DRAW_INSTANCES instruction. */
gsp_err_t gsp_instance_set_paint_after(gsp_instance_pool_t *pool,
                                       gsp_instance_handle_t handle, uint32_t command_boundary);

/**
 * Moves an instance WITHOUT marking dirty rectangles — for callers that
 * cover the change another way (the repeater's scroll-by-blit path).
 * Using it without such cover leaves stale pixels on incremental
 * presents.
 */
gsp_err_t gsp_instance_set_position_quiet(gsp_instance_pool_t *pool,
        gsp_instance_handle_t handle,
        int16_t x, int16_t y);

gsp_err_t gsp_instance_set_position(gsp_instance_pool_t *pool,
                                    gsp_instance_handle_t handle,
                                    int16_t x, int16_t y);

gsp_err_t gsp_instance_set_visible(gsp_instance_pool_t *pool,
                                   gsp_instance_handle_t handle,
                                   bool visible);

/** Writes an instance-local value slot (consumed by value_mode commands). */
/** Reads a state slot's current i32 value (tween starting points). */
gsp_err_t gsp_instance_get_state_i32(const gsp_instance_pool_t *pool,
                                     gsp_instance_handle_t handle,
                                     uint16_t state_slot,
                                     uint32_t *out_value);

gsp_err_t gsp_instance_set_state_i32(gsp_instance_pool_t *pool,
                                     gsp_instance_handle_t handle,
                                     uint16_t state_slot,
                                     uint32_t value);

/**
 * Writes an instance-local color slot (consumed by state-bound fill
 * commands with value_mode NONE, i.e. dynamic_color template members).
 * The color uses the scene's native pixel format (RGB565 profiles take
 * a 16-bit value).
 */
gsp_err_t gsp_instance_set_color(gsp_instance_pool_t *pool,
                                 gsp_instance_handle_t handle,
                                 uint16_t state_slot,
                                 uint32_t color);

/** Reads an instance-local RESOURCE slot. */
gsp_err_t gsp_instance_get_resource(const gsp_instance_pool_t *pool,
                                    gsp_instance_handle_t handle,
                                    uint16_t state_slot,
                                    uint16_t *out_resource_ref);

/** Checks that every template command reading this RESOURCE slot uses
 *  image-fit rendering, which permits replacement geometry to differ. */
gsp_err_t gsp_instance_resource_supports_image_fit(
    const gsp_instance_pool_t *pool,
    gsp_instance_handle_t handle,
    uint16_t state_slot);

/**
 * Points an instance-local RESOURCE slot at a resolved scene resource.
 * Native blits require the authored geometry; image-fit commands accept the
 * replacement's source geometry. Pixel format and alpha mode must match.
 * This is the low-level primitive used by recycled rows and template widgets
 * with runtime-provided images.
 */
gsp_err_t gsp_instance_set_resource(gsp_instance_pool_t *pool,
                                    gsp_instance_handle_t handle,
                                    uint16_t state_slot,
                                    uint16_t resource_ref);

/**
 * Points an instance-local TEXT slot at a runtime glyph run published via
 * gsp_runtime_publish_glyph_run() (positions relative to the bound
 * command bbox). GSP_INSTANCE_GLYPH_RUN_NONE reverts to the template's
 * static run. The repeater bind callback uses this for per-row text.
 */
#define GSP_INSTANCE_GLYPH_RUN_NONE UINT16_MAX

gsp_err_t gsp_instance_set_glyph_run(gsp_instance_pool_t *pool,
                                     gsp_instance_handle_t handle,
                                     uint16_t state_slot,
                                     uint16_t glyph_run_handle);

/**
 * Sets or clears (clip == NULL) the screen-space viewport clip of an
 * instance; commands render only inside the intersection of this rect
 * and their own clip stack.
 */
gsp_err_t gsp_instance_set_clip(gsp_instance_pool_t *pool,
                                gsp_instance_handle_t handle,
                                const gsp_rect_t *clip);

gsp_err_t gsp_instance_bounds(const gsp_instance_pool_t *pool,
                              size_t slot_index,
                              gsp_rect_t *out_bounds);

/* Repaint the consumers of a mutated runtime image without changing its ref. */
gsp_err_t gsp_instance_mark_resource_dirty(gsp_instance_pool_t *pool,
        gsp_instance_handle_t handle, uint16_t state_slot);

#ifdef __cplusplus
}
#endif
