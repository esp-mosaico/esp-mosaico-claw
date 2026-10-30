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
#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Immutable view over one memory-mapped GFB font pack. All sections are
 * validated once at open time; glyph resolution afterwards is bounds-check
 * plus pointer arithmetic only. The backing memory must stay valid and
 * unchanged for the lifetime of the view.
 */
typedef struct {
    const uint8_t *glyph_table;
    const uint8_t *cmap;
    size_t cmap_size;
    const uint8_t *atlas_pages;
    size_t atlas_pages_size;
    uint32_t default_glyph;
    uint16_t font_size_px;
    int16_t ascent;
    int16_t descent;
    uint16_t line_height;
    uint16_t baseline;
    uint16_t page_count;
    uint16_t glyph_count;
    uint8_t atlas_format;
    uint8_t flags;
} gsp_gfb_t;

typedef struct {
    const uint8_t *a8;
    size_t data_size;
    size_t stride_bytes;
    uint8_t width;
    uint8_t height;
    int8_t bearing_x;
    int8_t bearing_y;
    uint8_t advance;
} gsp_gfb_glyph_t;

/**
 * Opens and fully validates a GFB font pack, including per-glyph atlas
 * bounds. Atlas pages must carry enough trailing bytes that every glyph
 * satisfies the renderer contract stride * height <= remaining data; the
 * exporter page alignment padding provides this. expected_version_major
 * may be GSP_RUNTIME_ANY_VERSION. A4 atlases are not supported yet.
 */
gsp_err_t gsp_gfb_open(const void *data, size_t data_size,
                       uint16_t expected_version_major,
                       gsp_gfb_t *out_font);

/** Same structural validation as gsp_gfb_open(), optionally skipping only
 *  the whole-container CRC scan for trusted build-time assets. */
gsp_err_t gsp_gfb_open_ex(const void *data, size_t data_size,
                          uint16_t expected_version_major, bool skip_crc,
                          gsp_gfb_t *out_font);

/**
 * Maps a Unicode codepoint to a glyph ID via the two-level CMAP (ASCII
 * direct table, then binary search). Unmapped codepoints resolve to the
 * default glyph. Fails with GSP_ERR_UNSUPPORTED when the pack has no CMAP.
 */
gsp_err_t gsp_gfb_map_codepoint(const gsp_gfb_t *font, uint32_t codepoint,
                                uint16_t *out_glyph_id);

/** Zero-copy A8 glyph lookup; the bitmap points into the atlas page. */
gsp_err_t gsp_gfb_get_glyph(const gsp_gfb_t *font, uint16_t glyph_id,
                            gsp_gfb_glyph_t *out_glyph);

typedef gsp_err_t (*gsp_gfb_resolve_font_cb_t)(
    void *user_ctx,
    uint16_t font_ref,
    const gsp_gfb_t **out_font);

typedef struct {
    gsp_gfb_resolve_font_cb_t resolve_font;
    void *user_ctx;
} gsp_gfb_provider_t;

/**
 * Creates the production glyph provider backed by mmapped GFB atlases:
 * no rasterization, no cache, no allocation on the resolve path. The
 * provider struct and every resolved gsp_gfb_t must outlive the runtime
 * they are installed into.
 */
/** gsp_gfb_map_codepoint without the default-glyph substitution:
 *  cmap misses return GSP_ERR_NOT_FOUND (dynamic-fallback probing). */
gsp_err_t gsp_gfb_map_codepoint_exact(const gsp_gfb_t *font,
                                      uint32_t codepoint,
                                      uint16_t *out_glyph_id);

gsp_err_t gsp_gfb_provider_init(
    gsp_gfb_provider_t *provider,
    gsp_gfb_resolve_font_cb_t resolve_font,
    void *user_ctx,
    gsp_glyph_provider_t *out_provider);

#ifdef __cplusplus
}
#endif
