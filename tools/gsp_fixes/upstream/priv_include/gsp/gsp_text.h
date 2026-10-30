/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

/* Runtime text shaping: UTF-8 -> GSB glyph run using a GFB font pack.
 *
 * Layout matches the compiler's bound-text rules (left/center/right
 * alignment, baseline at (box_h + ascent + descent) / 2, positions
 * relative to the bound command bbox), so runtime-shaped text lands
 * where the compiled default run did. Kerning is not applied (GFB v1
 * carries no kerning data); missing codepoints fall back to the pack's
 * default glyph. No allocation: the run is written into a caller-owned
 * buffer, which doubles as the published glyph-run storage.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_gfb.h"
#include "gsp/gsp_update.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GSP_TEXT_ALIGN_LEFT = 0,
    GSP_TEXT_ALIGN_CENTER = 1,
    GSP_TEXT_ALIGN_RIGHT = 2,
} gsp_text_align_t;

/** OR into the align argument: greedy word wrap at box_width (spaces
 *  break words, CJK breaks per glyph), top-aligned lines of
 *  ascent-descent height, truncated at box_height. LEFT only. */
#define GSP_TEXT_FLAG_WRAP 0x10U

/** OR into the align argument: single-line text wider than box_width
 *  ends with U+2026 instead of running past the box (ignored with
 *  WRAP; falls back to the plain overflow when the font lacks the
 *  ellipsis glyph). */
#define GSP_TEXT_FLAG_ELLIPSIS 0x20U

/** Bytes needed for a run of glyph_count glyphs. */
#define GSP_TEXT_RUN_SIZE(glyph_count) (8U + (size_t)(glyph_count) * 6U)

/** Glyph ids with this bit set route to the DYNAMIC provider of a
 *  gsp_font_chain (runtime-rasterized glyphs, e.g. FreeType). */
#define GSP_GLYPH_ID_DYNAMIC 0x8000U

/** Metrics of a dynamically mapped glyph (26.6-free integer pixels,
 *  same conventions as gsp_gfb_glyph_t). */
typedef struct {
    uint16_t glyph_id;      /*!< provider id; set GSP_GLYPH_ID_DYNAMIC */
    int16_t advance;
    int16_t bearing_x;
    int16_t bearing_y;
    uint16_t width;
    uint16_t height;
} gsp_dynamic_glyph_info_t;

/**
 * Fallback codepoint mapper consulted when the GFB cmap misses (chat
 * text with unbaked CJK, emoji, ...). Return GSP_ERR_NOT_FOUND to skip
 * the character.
 */
typedef gsp_err_t (*gsp_text_dynamic_map_cb_t)(
    void *user_ctx, uint16_t font_ref, uint32_t codepoint,
    gsp_dynamic_glyph_info_t *out_info);

/** Content metrics produced by the same pass that emits a glyph run. */
typedef struct {
    int32_t natural_width;   /*!< widest unwrapped paragraph advance */
    int32_t used_width;      /*!< widest emitted line advance */
    int32_t used_height;     /*!< emitted line_count * line height */
    uint16_t line_count;
    uint16_t glyph_count;
    bool truncated;
} gsp_text_layout_t;

/**
 * Shapes UTF-8 text into an encoded GSB glyph run.
 *
 * Glyphs that would not fit the buffer are dropped (the run stays
 * valid) and GSP_ERR_LIMIT_EXCEEDED is returned; out_size is always the
 * written byte count. font_ref must be the RES_REFS index the bound
 * command references.
 */
gsp_err_t gsp_text_shape(const gsp_gfb_t *font, const char *utf8,
                         uint16_t box_width, uint16_t box_height,
                         gsp_text_align_t align, uint16_t font_ref,
                         uint8_t *buffer, size_t capacity,
                         size_t *out_size);

/** Advance width in pixels of @p utf8 under the same lookup rules as
 *  gsp_text_shape_ex (caret placement, layout). */
gsp_err_t gsp_text_measure_ex(const gsp_gfb_t *font,
                              gsp_text_dynamic_map_cb_t dynamic_map,
                              void *dynamic_ctx, const char *utf8,
                              uint16_t font_ref, int32_t *out_width);

/** gsp_text_shape with a dynamic fallback for codepoints the GFB pack
 *  does not carry; pass dynamic_map = NULL for the static behavior. */
gsp_err_t gsp_text_shape_ex(const gsp_gfb_t *font,
                            gsp_text_dynamic_map_cb_t dynamic_map,
                            void *dynamic_ctx, const char *utf8,
                            uint16_t box_width, uint16_t box_height,
                            gsp_text_align_t align, uint16_t font_ref,
                            uint8_t *buffer, size_t capacity,
                            size_t *out_size);

/** gsp_text_shape_ex with content metrics for intrinsic-size controls. */
gsp_err_t gsp_text_shape_layout_ex(
    const gsp_gfb_t *font, gsp_text_dynamic_map_cb_t dynamic_map,
    void *dynamic_ctx, const char *utf8, uint16_t box_width,
    uint16_t box_height, gsp_text_align_t align, uint16_t font_ref,
    uint8_t *buffer, size_t capacity, size_t *out_size,
    gsp_text_layout_t *out_layout);

/**
 * Shapes and publishes in one call: the run is written into buffer and
 * published as runtime glyph-run handle. The buffer must stay valid and
 * unchanged until the handle is republished, and must not be touched
 * while a prepared frame is rendering. Bind the handle to a TEXT slot
 * with gsp_update_set_glyph_run() (scene text) or
 * gsp_instance_set_glyph_run() (template rows).
 */
gsp_err_t gsp_text_publish(gsp_runtime_t *runtime, uint16_t handle,
                           const gsp_gfb_t *font, const char *utf8,
                           uint16_t box_width, uint16_t box_height,
                           gsp_text_align_t align, uint16_t font_ref,
                           uint8_t *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif
