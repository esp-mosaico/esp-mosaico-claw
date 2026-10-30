/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *a8;
    size_t data_size;
    uint16_t width;
    uint16_t height;
    size_t stride_bytes;
} gsp_glyph_bitmap_t;

typedef gsp_err_t (*gsp_glyph_resolve_cb_t)(
    void *user_ctx,
    uint16_t font_ref,
    uint16_t glyph_id,
    gsp_glyph_bitmap_t *out_glyph);

typedef struct {
    /**
     * Resolution must be deterministic and side-effect free. Returned bitmap
     * storage must remain valid and unchanged until the next resolve call on
     * this provider begins. Calls to one provider must be serialized.
     */
    gsp_glyph_resolve_cb_t resolve;
    void *user_ctx;
} gsp_glyph_provider_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} gsp_glyph_run_view_t;

#define GSP_GLYPH_METRICS_CACHE_SLOTS 128U

typedef struct {
    uint32_t key;
    uint16_t width;
    uint16_t height;
} gsp_glyph_metrics_entry_t;

/**
 * Glyph sizes the renderer learns while drawing, so later stripes of the
 * same frame cull glyphs without resolving them. Valid only while the
 * glyph provider is unchanged; the renderer clears it at every frame begin
 * and full render.
 */
typedef struct {
    gsp_glyph_metrics_entry_t entries[GSP_GLYPH_METRICS_CACHE_SLOTS];
} gsp_glyph_metrics_cache_t;

/**
 * Two-stage provider: glyph ids with bit 15 set
 * (GSP_GLYPH_ID_DYNAMIC) resolve through @p dynamic, everything else
 * through @p primary — one runtime provider serving AOT GFB glyphs and
 * runtime-rasterized fallbacks side by side.
 */
typedef struct {
    gsp_glyph_provider_t primary;
    gsp_glyph_provider_t dynamic;
} gsp_font_chain_t;

gsp_err_t gsp_font_chain_init(gsp_font_chain_t *chain,
                              const gsp_glyph_provider_t *primary,
                              const gsp_glyph_provider_t *dynamic,
                              gsp_glyph_provider_t *out_provider);

#ifdef __cplusplus
}
#endif
