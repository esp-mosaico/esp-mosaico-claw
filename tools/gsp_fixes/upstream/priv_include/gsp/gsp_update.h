/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_font.h"
#include "gsp/gsp_image_cache.h"
#include "gsp/gsp_resource.h"
#include "gsp/gsp_runtime.h"
#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t value;
    uint32_t color;
    uint16_t res_ref;
    uint16_t text_off;
    uint16_t glyph_run;
    uint16_t flags;
} gsp_state_t;

typedef struct {
    uint16_t bind_id;
    uint8_t type;
    uint8_t reserved;          /*!< 0: bind id, 1: GSB 1.1 property index */
    uint32_t value;
} gsp_pending_update_t;

struct gsp_instance_pool;
struct gsp_overlay;

typedef struct gsp_runtime {
    const gsp_scene_view_t *scene;
    uint32_t nested_static_dirty_mask[2];
    bool has_nested_static_transforms;
    gsp_state_t *states;
    size_t state_capacity;
    gsp_resource_view_t *resources;
    size_t resource_capacity;
    gsp_glyph_provider_t glyph_provider;
    gsp_image_decoder_t image_decoder;
    gsp_image_cache_t *image_cache;
    void **vector_workspace; /*!< Borrowed context-owned scratch; NULL disables retention. */
    gsp_glyph_metrics_cache_t *glyph_metrics; /*!< Borrowed context-owned scratch; NULL disables cross-stripe glyph culling. */
    struct gsp_instance_pool *instance_pool;
    const struct gsp_overlay *overlay;
    gsp_glyph_run_view_t *glyph_runs;
    size_t glyph_run_capacity;
    gsp_rect_t *dirty_rects;
    size_t dirty_capacity;
    size_t dirty_count;
    /* Set when gsp_runtime_add_dirty had to drop a rect: the dropped damage
     * is unrecoverable, so the next frame plan must repaint everything. */
    bool dirty_overflow;
    uint32_t revision;
    uint32_t generation; /*!< Changes when this runtime storage is initialized again. */
    /* Paint changes excluding retained translations, which snapshots check
     * separately so their own moving group does not invalidate the atlas. */
    uint32_t paint_revision;
    uint32_t visibility_revision;
    uint32_t color_revision;
    /* Captured content excludes its owner's expected translation. Offscreen
     * state updates are checked at both old and new bounds before invalidation. */
    uint32_t snapshot_revision;
    uint32_t snapshot_capture_revision;
    uint16_t snapshot_motion_group;
    const void *snapshot_owner;
    const gsp_rect_t *snapshot_viewport;
    uint32_t next_dynamic_resource_id;
    /* Pending scroll-by-blit request (one per frame, consumed by the
     * presenter; see gsp_runtime_request_scroll). Both axes may move in one
     * request. */
    gsp_rect_t scroll_viewport;
    int16_t scroll_dx;
    int16_t scroll_dy;
    bool scroll_pending;
} gsp_runtime_t;

typedef struct gsp_update_transaction {
    gsp_runtime_t *runtime;
    gsp_pending_update_t *updates;
    size_t capacity;
    size_t count;
    bool active;
} gsp_update_transaction_t;

/**
 * Records a scroll-by-blit request: every pixel of @p viewport that was
 * visible last frame merely moved by (-dx, -dy) — the presenter may
 * copy those pixels from the previous frame and rasterize only the
 * exposed band. Single-axis: exactly one of dx/dy is nonzero. The
 * request never REPLACES dirty tracking: a presenter that ignores it
 * (or cannot blit) must simply treat the viewport as dirty, so
 * correctness never depends on the fast path. One request per frame; a
 * second request in the same frame degrades to plain dirty tracking.
 * Non-translated changes inside the viewport keep their normal dirty
 * marks and are rasterized over the copy. dx and dy may both be nonzero.
 */
gsp_err_t gsp_runtime_request_scroll(gsp_runtime_t *runtime,
                                     gsp_rect_t viewport, int16_t dx,
                                     int16_t dy);

/** Consumes the pending scroll request (presenter side). */
bool gsp_runtime_take_scroll(gsp_runtime_t *runtime,
                             gsp_rect_t *out_viewport, int16_t *out_dx,
                             int16_t *out_dy);

/** Snapshots the pending dirty queue for frame planning: hands out the
 *  caller-owned rect storage and count, and consumes the sticky
 *  overflow flag through out_overflow. The queue itself is NOT cleared
 *  — a frame that fails to present must find its damage intact (the
 *  frame loop clears via gsp_runtime_clear_dirty only after a
 *  successful present or a deliberate skip). */
void gsp_runtime_take_dirty(gsp_runtime_t *runtime,
                            const gsp_rect_t **out_rects,
                            size_t *out_count,
                            bool *out_overflow);

void gsp_runtime_add_dirty(gsp_runtime_t *runtime, gsp_rect_t rect);

/** Conservative current paint bounds, including transforms, clipping and
 * resource-fit scale/rotation. An empty clipped box is a valid result. */
gsp_err_t gsp_runtime_command_bounds(const gsp_runtime_t *runtime,
                                     uint32_t command_index,
                                     gsp_rect_t *out_bounds);
/** Reserve room for an atomic damage update. A failed reservation latches
 *  dirty overflow so the next frame is repainted in full. */
bool gsp_runtime_dirty_capacity_ok(gsp_runtime_t *runtime,
                                   size_t additional);
void gsp_runtime_bump_revision(gsp_runtime_t *runtime);
void gsp_runtime_bump_transform_revision(gsp_runtime_t *runtime, uint16_t group_id);

/** Destination part of @p viewport whose pixels can be copied from the
 *  previous frame at (x + dx, y + dy); empty when the shift spans the
 *  viewport. */
static inline gsp_rect_t gsp_scroll_covered(gsp_rect_t viewport,
        int16_t dx, int16_t dy)
{
    gsp_rect_t covered = viewport;
    if (dy >= 0) {
        covered.y2 = viewport.y2 - dy > viewport.y1 ?
                     viewport.y2 - dy : viewport.y1;
    } else {
        covered.y1 = viewport.y1 - dy < viewport.y2 ?
                     viewport.y1 - dy : viewport.y2;
    }
    if (dx >= 0) {
        covered.x2 = viewport.x2 - dx > viewport.x1 ?
                     viewport.x2 - dx : viewport.x1;
    } else {
        covered.x1 = viewport.x1 - dx < viewport.x2 ?
                     viewport.x1 - dx : viewport.x2;
    }
    return covered;
}

/** First exposed strip. For a two-axis shift this is the full-width
 *  horizontal strip; use gsp_scroll_exposed_side() for the remaining
 *  vertical strip. */
static inline gsp_rect_t gsp_scroll_exposed_band(gsp_rect_t viewport,
        int16_t dx, int16_t dy)
{
    gsp_rect_t band = viewport;
    if (dy > 0) {
        band.y1 = viewport.y2 - dy > viewport.y1 ?
                  viewport.y2 - dy : viewport.y1;
    } else if (dy < 0) {
        band.y2 = viewport.y1 - dy < viewport.y2 ?
                  viewport.y1 - dy : viewport.y2;
    } else if (dx > 0) {
        band.x1 = viewport.x2 - dx > viewport.x1 ?
                  viewport.x2 - dx : viewport.x1;
    } else if (dx < 0) {
        band.x2 = viewport.x1 - dx < viewport.x2 ?
                  viewport.x1 - dx : viewport.x2;
    }
    return band;
}

/** Second, non-overlapping exposed strip for a two-axis shift. Empty for a
 *  single-axis shift. */
static inline gsp_rect_t gsp_scroll_exposed_side(gsp_rect_t viewport,
        int16_t dx, int16_t dy)
{
    gsp_rect_t side = viewport;
    if (dx > 0) {
        side.x1 = viewport.x2 - dx > viewport.x1 ?
                  viewport.x2 - dx : viewport.x1;
    } else if (dx < 0) {
        side.x2 = viewport.x1 - dx < viewport.x2 ?
                  viewport.x1 - dx : viewport.x2;
    } else {
        side.x2 = side.x1;
    }
    if (dy > 0) {
        side.y2 = viewport.y2 - dy > viewport.y1 ?
                  viewport.y2 - dy : viewport.y1;
    } else if (dy < 0) {
        side.y1 = viewport.y1 - dy < viewport.y2 ?
                  viewport.y1 - dy : viewport.y2;
    }
    return side;
}

gsp_err_t gsp_runtime_init(gsp_runtime_t *runtime, const gsp_scene_view_t *scene,
                           gsp_state_t *state_storage, size_t state_capacity,
                           gsp_rect_t *dirty_storage, size_t dirty_capacity);

gsp_err_t gsp_runtime_resolve_resources(gsp_runtime_t *runtime,
                                        const gsp_resource_bundle_t *bundle,
                                        gsp_resource_view_t *resource_storage,
                                        size_t resource_capacity);

gsp_err_t gsp_runtime_set_glyph_provider(
    gsp_runtime_t *runtime,
    const gsp_glyph_provider_t *provider);

gsp_err_t gsp_runtime_set_image_decoder(
    gsp_runtime_t *runtime,
    const gsp_image_decoder_t *decoder);

/**
 * Installs a decoded-image cache consumed by renderers for DECODE_LRU and
 * PRELOAD resources. The cache must remain valid for the runtime lifetime
 * and its serialization contract extends to frames prepared from this
 * runtime.
 */
gsp_err_t gsp_runtime_set_image_cache(
    gsp_runtime_t *runtime,
    gsp_image_cache_t *cache);

/**
 * Marks the bounding boxes of all blit commands whose effective resource
 * matches resource_id as dirty and bumps the runtime revision. Intended as
 * the gsp_image_cache_ready_cb_t follow-up once a decode job finishes.
 */
gsp_err_t gsp_runtime_mark_resource_dirty(
    gsp_runtime_t *runtime,
    uint32_t resource_id);

/** Marks only the consumers of a resource-local changed rectangle dirty.
 *  Each blit's source offset and destination position are applied before the
 *  resulting scene-space rectangle is inserted. */
gsp_err_t gsp_runtime_mark_resource_region_dirty(
    gsp_runtime_t *runtime, uint32_t resource_id, gsp_rect_t region);

/**
 * Installs caller-owned slots for dynamic glyph runs. Existing slot contents
 * are cleared. The storage must remain valid for the runtime lifetime.
 */
gsp_err_t gsp_runtime_set_glyph_run_storage(
    gsp_runtime_t *runtime,
    gsp_glyph_run_view_t *storage,
    size_t capacity);

/**
 * Publishes exactly one encoded GSB glyph run into a slot. The run bytes must
 * remain valid and unchanged until the slot is republished, and must not be
 * republished while a prepared reference frame is being rendered.
 */
gsp_err_t gsp_runtime_publish_glyph_run(
    gsp_runtime_t *runtime,
    uint16_t handle,
    const void *data,
    size_t size);

void gsp_runtime_clear_dirty(gsp_runtime_t *runtime);

gsp_err_t gsp_update_begin(gsp_runtime_t *runtime, gsp_update_transaction_t *transaction,
                           gsp_pending_update_t *update_storage, size_t update_capacity);

gsp_err_t gsp_update_set_i32(gsp_update_transaction_t *transaction,
                             uint16_t bind_id, uint32_t value);
gsp_err_t gsp_runtime_get_i32(const gsp_runtime_t *runtime,
                              uint16_t bind_id, uint32_t *out_value);

gsp_err_t gsp_update_set_color(gsp_update_transaction_t *transaction,
                               uint16_t bind_id, uint32_t color);

/** Static GRB refs, or a runtime-published dynamic slot
 *  (scene->resource_ref_count + slot, see
 *  gsp_runtime_publish_image). */
gsp_err_t gsp_update_set_resource(gsp_update_transaction_t *transaction,
                                  uint16_t bind_id, uint16_t res_ref);

/**
 * Publishes a runtime image (network download, camera frame) into
 * dynamic slot @p slot (0..dynamic_image_slots). @p data must stay
 * valid while any bind references the slot; re-publishing the slot
 * replaces the image (decoded-cache entries age out via a fresh
 * resource id). Marks binds already pointing at the slot dirty.
 */
gsp_err_t gsp_runtime_publish_image(gsp_runtime_t *runtime,
                                    uint16_t slot, const void *data,
                                    size_t data_size, uint16_t width,
                                    uint16_t height, uint8_t codec,
                                    uint8_t pixel_format,
                                    uint8_t alpha_mode,
                                    uint32_t cache_key);

/** The runtime resource slot an image bind currently points at —
 *  external frame publishers (canvas) overwrite this view in place. */
gsp_err_t gsp_update_bind_resource_ref(const gsp_runtime_t *runtime,
                                       uint16_t bind_id,
                                       uint16_t *out_ref);

gsp_err_t gsp_update_bind_resource_view(const gsp_runtime_t *runtime,
                                        uint16_t bind_id,
                                        gsp_resource_view_t *out_view);

/** Rejects a resource view also used outside this bind. Canvas mutates its
 * view in place; encoded payload deduplication must not alias mutable views. */
gsp_err_t gsp_update_bind_canvas_exclusive(
    const gsp_runtime_t *runtime, uint16_t bind_id);

/** Checks that every command currently reading a RESOURCE bind is a native
 *  opaque blit suitable for a procedural Canvas draw callback. */
gsp_err_t gsp_update_bind_supports_canvas_draw(
    const gsp_runtime_t *runtime, uint16_t bind_id);

/** Checks that every command reading a RESOURCE bind uses image-fit
 *  rendering, which permits replacement images to keep their own geometry. */
gsp_err_t gsp_update_bind_supports_image_fit(
    const gsp_runtime_t *runtime, uint16_t bind_id);

/** Marks every command of @p bind_id dirty (repaint without a value
 *  change — e.g. after re-publishing the image a bind points at). */
gsp_err_t gsp_update_mark_bind_dirty(gsp_runtime_t *runtime,
                                     uint16_t bind_id);

/** Marks only the consumers of a RESOURCE bind's local changed rectangle. */
gsp_err_t gsp_update_mark_bind_region_dirty(
    gsp_runtime_t *runtime, uint16_t bind_id, gsp_rect_t region);

gsp_err_t gsp_update_set_visible(gsp_update_transaction_t *transaction,
                                 uint16_t bind_id, bool visible);

/** GSB 1.1 direct-property counterparts. These bypass the legacy bind
 *  lookup and apply the property's compiler-declared effect span.
 *  A 1.0 scene returns GSP_ERR_UNSUPPORTED; generated component and
 *  behavior adapters automatically retain their bind fallback. */
gsp_err_t gsp_update_set_property_i32(
    gsp_update_transaction_t *transaction,
    uint16_t property,
    uint32_t value);

gsp_err_t gsp_update_set_property_color(
    gsp_update_transaction_t *transaction,
    uint16_t property,
    uint32_t color);

gsp_err_t gsp_update_set_property_resource(
    gsp_update_transaction_t *transaction,
    uint16_t property,
    uint16_t res_ref);

gsp_err_t gsp_update_set_property_visible(
    gsp_update_transaction_t *transaction,
    uint16_t property,
    bool visible);

gsp_err_t gsp_update_property_i32(const gsp_runtime_t *runtime,
                                  uint16_t property,
                                  uint32_t *out_value);

gsp_err_t gsp_update_property_color(const gsp_runtime_t *runtime,
                                    uint16_t property,
                                    uint32_t *out_color);

gsp_err_t gsp_update_property_visible(const gsp_runtime_t *runtime,
                                      uint16_t property,
                                      bool *out_visible);

/**
 * Last committed value of an I32 bind — reads the same state slot the
 * renderers draw from, so applications never mirror what they set.
 * Values staged in an open transaction become visible after commit.
 * GSP_ERR_NOT_FOUND for an unknown bind id, GSP_ERR_INVALID_ARG on a
 * bind type mismatch.
 */
gsp_err_t gsp_update_bind_i32(const gsp_runtime_t *runtime,
                              uint16_t bind_id, uint32_t *out_value);

/** COLOR counterpart of gsp_update_bind_i32 (native pixel format). */
gsp_err_t gsp_update_bind_color(const gsp_runtime_t *runtime,
                                uint16_t bind_id, uint32_t *out_color);

/** VISIBLE counterpart of gsp_update_bind_i32. */
gsp_err_t gsp_update_bind_visible(const gsp_runtime_t *runtime,
                                  uint16_t bind_id, bool *out_visible);

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t font_ref;
    uint8_t flags;
} gsp_bind_text_layout_t;

/** Authored box, font and alignment/overflow bind flags from one lookup. */
gsp_err_t gsp_update_bind_text_layout(const gsp_runtime_t *runtime,
                                      uint16_t bind_id,
                                      gsp_bind_text_layout_t *out_layout);

/** Looks up the authored text box of a text bind (the box the compiler
 *  shaped the static run against) from the bind's glyph-run command —
 *  pass the result to gsp_text_publish so dynamic text lands on the
 *  same baseline. GSP_ERR_NOT_FOUND for non-text binds. */
gsp_err_t gsp_update_bind_text_box(const gsp_runtime_t *runtime,
                                   uint16_t bind_id, uint16_t *out_width,
                                   uint16_t *out_height);

/** Font pack reference of the bind's compiled glyph run (dynamic text
 *  must shape with the same pack). */
gsp_err_t gsp_update_bind_text_font(const gsp_runtime_t *runtime,
                                    uint16_t bind_id,
                                    uint16_t *out_font_ref);

/** Screen-space rectangle of a text bind's glyph-run command (cursor
 *  placement, highlights). */
/** True when the bind's text commands all sit in hidden visibility
 *  groups — a caret overlay must not float over other pages. False
 *  when visible or when the bind has no text commands. */
bool gsp_update_bind_text_hidden(const gsp_runtime_t *runtime,
                                 uint16_t bind_id);

gsp_err_t gsp_update_bind_text_rect(const gsp_runtime_t *runtime,
                                    uint16_t bind_id,
                                    gsp_rect_t *out_rect);

/** Selects a published glyph-run handle; UINT16_MAX restores the static run. */
gsp_err_t gsp_update_set_glyph_run(
    gsp_update_transaction_t *transaction,
    uint16_t bind_id,
    uint16_t glyph_run);

/** True when a visible vis group in the scene carries
 *  GSB VIS_GROUP_FLAG_BLOCK_SCENE_SWIPE. */
bool gsp_runtime_scene_swipe_blocked(const gsp_runtime_t *runtime);

gsp_err_t gsp_update_commit(gsp_update_transaction_t *transaction);

void gsp_update_cancel(gsp_update_transaction_t *transaction);

#ifdef __cplusplus
}
#endif
