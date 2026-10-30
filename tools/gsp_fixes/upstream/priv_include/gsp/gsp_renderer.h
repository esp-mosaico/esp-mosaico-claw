/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_render_profile.h"
#include "gsp/gsp_runtime.h"
#include "gsp/gsp_surface_priv.h"
#include "gsp/gsp_update.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uintptr_t opaque[16];
} gsp_reference_frame_t;

typedef struct {
    uintptr_t opaque[16];
} gsp_renderer_frame_t;

/**
 * Production renderer backed by the format-specific kernel fast paths.
 * Pixel-for-pixel identical to the reference rasterizer for every supported
 * command; unsupported features fail with GSP_ERR_UNSUPPORTED.
 *
 * Error semantics: a capability gap (a feature or surface pixel format the
 * fast path cannot draw) fails with GSP_ERR_UNSUPPORTED and is eligible
 * for reference-rasterizer fallback; malformed scene or resource data
 * fails with GSP_ERR_INVALID_FORMAT and is fatal.
 */
gsp_err_t gsp_renderer_render_runtime(const gsp_runtime_t *runtime,
                                      gsp_surface_t *surface);

/** Draw an anchored overlay over existing pixels, with its parent clipping.
 * The caller must establish a matching background without group effects or a legacy clip stack. */
gsp_err_t gsp_renderer_render_overlay_at(const gsp_runtime_t *runtime,
        uint32_t paint_after, gsp_surface_t *surface);

/** Clears the surface, then renders only @p group_id and descendants. */
gsp_err_t gsp_renderer_render_transform_group(
    const gsp_runtime_t *runtime, uint16_t group_id, gsp_surface_t *surface);

/**
 * Stripe/dirty-region rendering: begin captures the runtime revision,
 * render draws one viewport (tile-index culled when available). The
 * runtime must not change until every region has been rendered.
 */
gsp_err_t gsp_renderer_frame_begin(const gsp_runtime_t *runtime,
                                   gsp_renderer_frame_t *out_frame);

gsp_err_t gsp_renderer_frame_render(const gsp_renderer_frame_t *frame,
                                    gsp_surface_t *surface);

gsp_err_t gsp_reference_render(const gsp_scene_view_t *scene, gsp_surface_t *surface);

gsp_err_t gsp_reference_render_runtime(const gsp_runtime_t *runtime, gsp_surface_t *surface);

gsp_err_t gsp_reference_render_transform_group(
    const gsp_runtime_t *runtime, uint16_t group_id, gsp_surface_t *surface);

/**
 * @brief Preflight one immutable runtime snapshot for region rendering.
 *
 * The frame object must not be copied. Runtime state, resources, and glyph
 * provider must remain unchanged until all frame regions have been rendered.
 */
gsp_err_t gsp_reference_frame_begin(
    const gsp_runtime_t *runtime,
    gsp_reference_frame_t *out_frame);

gsp_err_t gsp_reference_frame_render(
    const gsp_reference_frame_t *frame,
    gsp_surface_t *surface);

/** Entry points of the optional reference rasterizer member. */
typedef struct {
    gsp_err_t (*render_runtime)(const gsp_runtime_t *runtime,
                                gsp_surface_t *surface);
    gsp_err_t (*render_transform_group)(const gsp_runtime_t *runtime,
                                        uint16_t group_id,
                                        gsp_surface_t *surface);
    gsp_err_t (*frame_begin)(const gsp_runtime_t *runtime,
                             gsp_reference_frame_t *out_frame);
    gsp_err_t (*frame_render)(const gsp_reference_frame_t *frame,
                              gsp_surface_t *surface);
} gsp_reference_provider_t;

/* gsp_reference.c follows the JPEG member's link contract (see gsp_codec.h):
 * only the weak provider reaches it, the component build roots it while the
 * reference renderer stays enabled, and GSP_REFERENCE_OPTIONAL lets host
 * gates rebuild every gsp_reference() user against the weak form. Nothing
 * outside that member may call the
 * gsp_reference_* functions directly. */
#if (defined(ESP_PLATFORM) || defined(GSP_REFERENCE_OPTIONAL)) && \
    !defined(GSP_REFERENCE_PROVIDER_IMPL)
extern const gsp_reference_provider_t gsp_reference_provider
__attribute__((weak));
#else
extern const gsp_reference_provider_t gsp_reference_provider;
#endif

/** Returns the linked reference rasterizer, or NULL when it was excluded. */
static inline const gsp_reference_provider_t *gsp_reference(void)
{
    const gsp_reference_provider_t *provider = &gsp_reference_provider;
    return provider;
}

/**
 * Kernel render with reference fallback: a GSP_ERR_UNSUPPORTED capability gap
 * is retried on the reference rasterizer, and stays GSP_ERR_UNSUPPORTED when
 * that rasterizer is not linked.
 */
gsp_err_t gsp_raster_render_runtime(const gsp_runtime_t *runtime,
                                    gsp_surface_t *surface);

gsp_err_t gsp_raster_render_transform_group(
    const gsp_runtime_t *runtime, uint16_t group_id, gsp_surface_t *surface);

/**
 * Raster engine selection with built-in fallback. begin tries the
 * kernel renderer first and falls back to the reference rasterizer on
 * GSP_ERR_UNSUPPORTED (capability gaps only — malformed scenes fail
 * fatally). The frame storage fits either engine; the kind says which.
 * Without the reference member the capability gap is returned as is.
 */
typedef struct {
    uintptr_t opaque[16];
} gsp_raster_frame_t;

typedef enum {
    GSP_RASTER_KERNEL = 0,
    GSP_RASTER_REFERENCE,
} gsp_raster_kind_t;

gsp_err_t gsp_raster_frame_begin(const gsp_runtime_t *runtime,
                                 gsp_raster_frame_t *out_frame,
                                 gsp_raster_kind_t *out_kind);

gsp_err_t gsp_raster_frame_render(const gsp_raster_frame_t *frame,
                                  gsp_raster_kind_t kind,
                                  gsp_surface_t *surface);

/** Native rectangle used by viewport composition. */
void gsp_raster_fill_rect(gsp_surface_t *surface, gsp_rect_t rect,
                          uint32_t color, uint8_t opacity);

#ifdef __cplusplus
}
#endif
