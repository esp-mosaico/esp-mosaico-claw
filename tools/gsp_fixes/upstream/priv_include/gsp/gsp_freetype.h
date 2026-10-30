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
#include "gsp/gsp_text.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef gsp_err_t (*gsp_freetype_resolve_face_cb_t)(
    void *user_ctx,
    uint16_t font_ref,
    void **out_face);

enum {
    GSP_FREETYPE_DRIVER_TRUETYPE = 1,
    GSP_FREETYPE_DRIVER_CFF = 2,
};

/** Registered outline drivers, or a negative error if the probe cannot run. */
int32_t gsp_freetype_outline_drivers(void);

/* Wide fields first keep this per-glyph array compact and naturally aligned. */
typedef struct {
    size_t data_size;
    uint32_t last_used;
    uint16_t font_ref;
    uint16_t glyph_id;
    uint16_t pixel_size;
    uint16_t width;
    uint16_t height;
    bool valid;
} gsp_freetype_cache_entry_t;

typedef struct {
    gsp_freetype_resolve_face_cb_t resolve_face;
    void *user_ctx;
    gsp_freetype_cache_entry_t *entries;
    size_t entry_capacity;
    uint8_t *pixel_storage;
    size_t pixel_storage_size;
    size_t bytes_per_entry;
    /** Optional dimension bound in addition to bytes_per_entry; 0 = bytes only. */
    uint16_t max_cached_glyph_px;
    /** Compact large-face indices into stable provider IDs. Such IDs must
     * come from gsp_freetype_map_codepoint, not caller-encoded FT indices. */
    bool remap_large_faces;
} gsp_freetype_config_t;

typedef struct {
    gsp_freetype_config_t config;
    uint8_t *uncached_pixels;
    size_t uncached_capacity;
    uint32_t clock;
    struct gsp_freetype_face_map *face_maps;
    uint8_t mapping_warnings;
} gsp_freetype_t;

/**
 * Creates a fixed-capacity FreeType-backed A8 glyph provider. FT_Face objects
 * are owned and size-configured by the caller and must remain stable while the
 * provider is installed. The bounded cache uses LRU replacement. Larger
 * glyphs use one reusable heap workspace, valid until the next resolve call;
 * they do not consume cache entries. Calls must be serialized as required by
 * gsp_glyph_provider_t. Zero-initialize the configuration; opt-in remapping
 * preserves up to 32768 distinct glyph IDs per large face until deinit.
 */
gsp_err_t gsp_freetype_init(
    gsp_freetype_t *freetype,
    const gsp_freetype_config_t *config,
    gsp_glyph_provider_t *out_provider);

/**
 * Clears bitmap entries and the uncached workspace, preserving stable glyph
 * IDs. Use deinit for final cleanup. Call only when no prepared/rendering
 * frame can still reference this provider.
 */
void gsp_freetype_reset_cache(gsp_freetype_t *freetype);

/** Release provider-owned pixels and mappings before reinit or face teardown.
 * Bitmap-cache reset alone preserves mappings referenced by live glyph runs. */
void gsp_freetype_deinit(gsp_freetype_t *freetype);

/**
 * gsp_text_dynamic_map_cb_t backend: maps a codepoint through the
 * font_ref's FT_Face and returns chain-routable metrics (glyph id has
 * GSP_GLYPH_ID_DYNAMIC set). Pass gsp_freetype_map as the callback and
 * the gsp_freetype_t as its context.
 */
gsp_err_t gsp_freetype_map_codepoint(gsp_freetype_t *freetype,
                                     uint16_t font_ref,
                                     uint32_t codepoint,
                                     gsp_dynamic_glyph_info_t *out_info);

/** Adapter matching gsp_text_dynamic_map_cb_t. */
static inline gsp_err_t gsp_freetype_map(void *user_ctx,
        uint16_t font_ref,
        uint32_t codepoint,
        gsp_dynamic_glyph_info_t *out)
{
    return gsp_freetype_map_codepoint((gsp_freetype_t *)user_ctx,
                                      font_ref, codepoint, out);
}

#ifdef __cplusplus
}
#endif
