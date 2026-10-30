/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once
#include "gsp/gsp_build_caps.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_gfb.h"
#include "gsp/gsp_bundle.h"
#include "gsp/gsp_font_catalog.h"
#include "gsp/gsp_codec.h"
#include "gsp/gsp_image_cache.h"
#include "gsp/gsp_instance.h"
#include "gsp/gsp_resource.h"
#include "gsp/gsp_runtime.h"
#include "gsp/gsp_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One-call runtime setup over gspc build outputs.
 *
 * The low-level API (gsp_scene_open / gsp_runtime_init / providers /
 * image cache / instance pool) stays available for full control; the
 * context wires the common case from a single config:
 *
 *   gsp_context_t ctx;
 *   gsp_context_config_t config = {
 *       .scene  = { scene_start, scene_size },
 *       .fonts  = { { font0_start, font0_size } },
 *       .font_count = 1,
 *       .bundle = { grb_start, grb_size },       // optional
 *   };
 *   ESP_ERROR_CHECK(gsp_context_init(&ctx, &config));
 *   ... gsp_update_begin(gsp_context_runtime(&ctx), ...) ...
 *
 * Storage (state slots, dirty rects, resource views, decoded-image
 * cache, glyph-run table) is sized from the scene and allocated
 * internally; compressed resources are synchronously prepared before the
 * scene's first render (during init unless explicitly deferred).
 */
typedef struct {
    const void *data;               /*!< gspc output bytes */
    size_t size;
} gsp_context_blob_t;

#define GSP_CONTEXT_MAX_FONTS ESP_GSP_BUILD_CAP_MAX_FONTS_PER_SCENE

/* Zero-initialize for one serialized startup. Short-lived CRC results cover
 * the current GSB and shared GRB/GFB members, not external font-catalog data.
 * All referenced bytes must remain immutable until startup ends.
 * Full entries may be evicted; a miss always performs the original check. */
typedef struct {
    gsp_context_blob_t scene;
    gsp_context_blob_t verified[GSP_CONTEXT_MAX_FONTS + 1U];
    size_t next;
} gsp_context_crc_cache_t;

typedef struct {
    gsp_context_blob_t scene;       /*!< .gsb (required) */
    gsp_context_blob_t bundle;      /*!< .grb (omit when the scene has
                                         no image resources) */
    /** .gfb packs bound to the scene's FONT entries: with
     *  font_identity_valid set, a pack serves the FONT entry whose
     *  resource-table index and Asset ID match font_resource_refs /
     *  font_asset_ids; otherwise packs are consumed in gspc output
     *  order (font0, font1, ...), the legacy ordered-array contract. */
    gsp_context_blob_t fonts[GSP_CONTEXT_MAX_FONTS];
    uint32_t font_asset_ids[GSP_CONTEXT_MAX_FONTS];
    uint16_t font_resource_refs[GSP_CONTEXT_MAX_FONTS];
    bool font_identity_valid[GSP_CONTEXT_MAX_FONTS];
    size_t font_count;
    const gsp_font_catalog_t *font_catalog;
    bool allow_missing_fonts;

    /* Everything below defaults sensibly when zero. */
    size_t dirty_capacity;          /*!< default 32 rectangles */
    size_t image_cache_entries;     /*!< default 16 decoded images */
    size_t image_cache_bytes;       /*!< zero selects a target-derived budget */
    size_t image_cache_auto_min_bytes;
    size_t image_cache_auto_max_bytes;
    size_t image_cache_host_default_bytes;
    uint8_t image_cache_shortage_retries;
    /** Tight-memory mode (stripe rendering): no decoded-image cache;
     *  compressed images region-decode per rendered band through the
     *  built-in QOI/RLE region decoder (CPU for memory). Default
     *  false — whole-image caching is faster everywhere else. */
    bool disable_image_cache;
    /** Create the cache but postpone compiled-image preparation until
     *  gsp_context_prepare_images(). Used by inactive scenes so startup only
     *  pays for the first visible scene. */
    bool defer_image_prepare;
    /** Trusted build-time assets may skip nested GSB/GRB/GFB CRC scans. */
    bool skip_crc;
    /** Optional startup-local CRC reuse; never retained by the context. */
    gsp_context_crc_cache_t *crc_cache;
    size_t glyph_run_slots;         /*!< runtime text slots, default 8 */
    /** Instance pool for template scenes; 0 uses the configured default
     *  when the scene declares templates. */
    size_t instance_slots;
    size_t instance_states_per_slot; /*!< default 8 */
    /** Runtime-published image slots (network downloads, camera
     *  frames): refs scene->resource_ref_count + slot, published via
     *  gsp_runtime_publish_image. 0 disables. */
    size_t dynamic_image_slots;
    void (*image_ready)(void *user_ctx, gsp_runtime_t *runtime,
                        uint32_t resource_id, gsp_err_t status);
    void *image_ready_ctx;
} gsp_context_config_t;

typedef struct gsp_context {
    gsp_scene_view_t scene;
    gsp_resource_bundle_t bundle;
    gsp_runtime_t runtime;
    gsp_gfb_t *fonts;
    size_t font_count;
    uint16_t *font_refs; /*!< RES_REFS index per font */
    gsp_gfb_provider_t gfb_provider;
    gsp_glyph_provider_t glyph_provider;
    gsp_image_cache_t image_cache;
    gsp_instance_pool_t instance_pool;
    gsp_codec_stats_accumulator_t codec_stats;
    void (*image_ready)(void *user_ctx, gsp_runtime_t *runtime,
                        uint32_t resource_id, gsp_err_t status);
    void *image_ready_ctx;
    bool has_instance_pool;
    bool has_image_cache;
    bool images_prepared;
    size_t static_image_count;
    size_t static_image_bytes;
    /* Internally allocated storage (freed by deinit). */
    void *font_storage;
    gsp_state_t *states;
    gsp_rect_t *dirty;
    gsp_resource_view_t *resources;
    gsp_image_cache_entry_t *cache_entries;
    gsp_glyph_run_view_t *glyph_runs;
    gsp_instance_slot_t *instance_slot_storage;
    gsp_state_t *instance_state_storage;
    gsp_codec_region_decoder_context_t region_decoder_context;
    void *vector_workspace;
    gsp_glyph_metrics_cache_t *glyph_metrics;
} gsp_context_t;

gsp_err_t gsp_context_init(gsp_context_t *context,
                           const gsp_context_config_t *config);

/**
 * Synchronously decodes the compiled image working set so the next frame
 * renders complete instead of repainting as decodes land. Idempotent while
 * @c images_prepared holds; clear that flag to re-verify a scene whose
 * cache may have lost entries off screen. Images that do not fit the
 * budget stay on the lazy path rather than evicting prepared ones.
 */
gsp_err_t gsp_context_prepare_images(gsp_context_t *context);

/**
 * Bytes the decoded-image caches may share on this target. Contexts with
 * @c image_cache_bytes zero derive their own budget from it; owners of
 * several contexts use it as the cap on total retained imagery.
 */
size_t gsp_context_image_budget_hint(void);
size_t gsp_context_image_budget_hint_for_config(
    const gsp_context_config_t *config);

/**
 * Fills scene/fonts/bundle in @p out_config from a GSPB bundle (one
 * flashed or embedded file for the whole app): scene = GSB member
 * @p scene_id, fonts = the GFB members matched to the scene's FONT
 * entries by content_id (stable Asset ID identity), bundle = GRB
 * member 0 when present. Sets skip_crc to !verify_crc for subsequent
 * context initialization; other config fields use zero defaults. When
 * verify_crc is false, GSPB and nested GSB/GRB/GFB CRC checks are skipped,
 * but structure and version validation remain enabled.
 * @p data must be 64-byte aligned (member alignment is relative to the
 * bundle base): partition mmap is naturally aligned; ESP-IDF projects
 * should use gsp_add_bundle(), which generates an aligned embed.
 *
 * Font contract: with compiler-emitted Asset IDs each scene receives
 * exactly its own fonts, so multi-scene bundles with per-scene fonts
 * work directly. When no FONT entry matches a GFB content_id (legacy
 * bundles without stable Asset IDs), every GFB member is exposed in
 * pack-id order as older single-font-set bundles expected; legacy
 * multi-scene bundles with per-scene fonts should pick members
 * explicitly via gsp_bundle_find().
 */
gsp_err_t gsp_context_config_from_bundle(const void *data, size_t size,
        uint16_t scene_id,
        bool verify_crc,
        gsp_context_config_t *out_config);

/** Startup-only variant. Records the checked GSB for the following context
 * initialization; the cache and all referenced bytes follow the cache lifetime
 * contract above. With verification enabled, a NULL cache retains independent
 * CRC checks. Disabled verification never populates the cache. */
gsp_err_t gsp_context_config_from_bundle_cached(const void *data, size_t size,
        uint16_t scene_id, bool verify_crc, gsp_context_crc_cache_t *cache,
        gsp_context_config_t *out_config);

/* Internal UI startup helpers. The bundle and scene views must come from
 * validation of the same immutable bytes during this initialization. These
 * helpers do not retain verification state between independent loads. */
gsp_err_t gsp_context_config_from_bundle_view(const gsp_bundle_view_t *view,
        uint16_t scene_id, bool verify_crc, gsp_context_crc_cache_t *cache,
        gsp_context_config_t *out_config, gsp_scene_view_t *out_scene);
gsp_err_t gsp_context_init_preparsed(gsp_context_t *context,
                                     const gsp_context_config_t *config,
                                     const gsp_scene_view_t *scene);

void gsp_context_deinit(gsp_context_t *context);

/** The runtime to pass to gsp_update_* / renderers / present. */
static inline gsp_runtime_t *gsp_context_runtime(gsp_context_t *context)
{
    return &context->runtime;
}

/** Instance pool for gsp_widget/gsp_repeater (NULL when disabled). */
static inline gsp_instance_pool_t *gsp_context_instances(
    gsp_context_t *context)
{
    return context->has_instance_pool ? &context->instance_pool : NULL;
}

/** The context's decoded-image cache (NULL when this context has none). */
static inline gsp_image_cache_t *gsp_context_image_cache(
    gsp_context_t *context)
{
    return context != NULL && context->has_image_cache ?
           &context->image_cache : NULL;
}

/**
 * The font pack and RES_REFS index of the i-th scene font — exactly
 * what gsp_widget_text_config_t and gsp_text_publish need.
 */
gsp_err_t gsp_context_font(gsp_context_t *context, size_t index,
                           const gsp_gfb_t **out_font,
                           uint16_t *out_font_ref);

/** Resolves a scene FONT resource-table reference to its opened GFB pack. */
gsp_err_t gsp_context_font_by_ref(gsp_context_t *context, uint16_t font_ref,
                                  const gsp_gfb_t **out_font);

/** Runs pending image decodes (also called internally by init). */
gsp_err_t gsp_context_pump_decodes(gsp_context_t *context);

/** Processes at most one pending decode job — frame-loop friendly
 *  (bounded stall per tick). Completed jobs mark their resource dirty
 *  so the next frame repaints over the placeholder. */
gsp_err_t gsp_context_pump_decode_one(gsp_context_t *context);

/** True while decode jobs are queued (keep the frame loop ticking). */
bool gsp_context_decode_pending(const gsp_context_t *context);

#ifdef ESP_PLATFORM
/**
 * Convenience: memory-map a flash partition (by label) as a context
 * blob — gspc outputs flashed to dedicated partitions load zero-copy:
 *
 *   gsp_context_blob_t scene;
 *   gsp_context_blob_from_partition("ui_scene", &scene);
 *   config.scene = scene;
 *
 * The mapping stays alive for the application lifetime.
 */
gsp_err_t gsp_context_blob_from_partition(const char *label,
        gsp_context_blob_t *out_blob);
#endif

#ifdef __cplusplus
}
#endif
