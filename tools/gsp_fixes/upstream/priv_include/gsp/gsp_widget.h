/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_gfb.h"
#include "gsp/gsp_instance.h"
#include "gsp/gsp_text.h"
#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Thin widget facade over declared templates.
 *
 * A widget is one instance of a gspc-declared template ("template" in
 * GSP1). The compiler emits GSP_TEMPLATE_<NAME>* constants in the
 * <scene>_templates.h header: the template id, its size, and one *_SLOT
 * constant per dynamic member (TEXT, VALUE, COLOR). This layer adds no
 * state of its own beyond the pool handle; every call forwards to
 * gsp_instance with the widget's handle.
 */
typedef struct {
    gsp_instance_pool_t *pool;
    gsp_instance_handle_t instance;
    uint16_t template_id;
} gsp_widget_t;

/**
 * Per-text-slot shaping configuration. buffer must stay valid while the
 * widget shows the text: the runtime glyph-run table references it in
 * place. glyph_run_handle is a caller-chosen index into the runtime
 * glyph-run table (unique per live text).
 */
typedef struct {
    const gsp_gfb_t *font;
    uint16_t font_ref;          /*!< RES_REFS index of the font */
    uint16_t glyph_run_handle;  /*!< runtime glyph-run table index */
    uint16_t box_width;         /*!< GSP_TEMPLATE_*_TEXT_BOX_W */
    uint16_t box_height;        /*!< GSP_TEMPLATE_*_TEXT_BOX_H */
    gsp_text_align_t align;
    uint8_t *buffer;            /*!< persistent glyph-run storage */
    size_t buffer_size;
    /** Optional dynamic fallback for codepoints the pack lacks (see
     *  gsp_text_shape_ex); NULL keeps the static behavior. */
    gsp_text_dynamic_map_cb_t dynamic_map;
    void *dynamic_ctx;
} gsp_widget_text_config_t;

/** Instantiates template_id at (x, y); fails when the quota is used up. */
gsp_err_t gsp_widget_create(gsp_widget_t *widget,
                            gsp_instance_pool_t *pool,
                            uint16_t template_id,
                            int16_t x, int16_t y,
                            uint16_t z_order);

gsp_err_t gsp_widget_destroy(gsp_widget_t *widget);

gsp_err_t gsp_widget_set_position(gsp_widget_t *widget,
                                  int16_t x, int16_t y);

gsp_err_t gsp_widget_set_visible(gsp_widget_t *widget, bool visible);

/** Sets or clears (clip == NULL) the screen-space viewport clip. */
gsp_err_t gsp_widget_set_clip(gsp_widget_t *widget,
                              const gsp_rect_t *clip);

/** Drives a VALUE slot (progress members), value in 0..100. */
gsp_err_t gsp_widget_set_value(gsp_widget_t *widget,
                               uint16_t value_slot, uint32_t value);

/** Drives a COLOR slot (dynamic_color members), native pixel format. */
gsp_err_t gsp_widget_set_color(gsp_widget_t *widget,
                               uint16_t color_slot, uint32_t color);

/**
 * Shapes utf8 with the config font, publishes it under
 * config->glyph_run_handle, and points the TEXT slot at it. Returns
 * GSP_ERR_LIMIT_EXCEEDED when the buffer cannot hold every glyph (the
 * truncated prefix is still shown).
 */
gsp_err_t gsp_widget_set_text(gsp_widget_t *widget,
                              uint16_t text_slot,
                              const gsp_widget_text_config_t *config,
                              const char *utf8);

/** Reverts a TEXT slot to the template's authored static run. */
gsp_err_t gsp_widget_reset_text(gsp_widget_t *widget, uint16_t text_slot);

/**
 * Looks up the authored text box of a TEXT slot from the template
 * metadata (the same numbers the compiler emits as
 * GSP_TEMPLATE_*_TEXT_BOX_W/H) so callers can shape without the
 * generated header. GSP_ERR_NOT_FOUND when the slot has no glyph-run
 * command.
 */
gsp_err_t gsp_widget_text_box(const gsp_widget_t *widget,
                              uint16_t text_slot, uint16_t *out_width,
                              uint16_t *out_height);

/** Looks up the authored alignment and overflow flags of a template TEXT slot. */
gsp_err_t gsp_widget_text_align(const gsp_widget_t *widget,
                                uint16_t text_slot,
                                gsp_text_align_t *out_align);

/**
 * Looks up the authored font reference of a template TEXT slot.
 * GSP_ERR_NOT_FOUND when the slot has no glyph-run command.
 */
gsp_err_t gsp_widget_text_font(const gsp_widget_t *widget,
                               uint16_t text_slot,
                               uint16_t *out_font_ref);

#ifdef __cplusplus
}
#endif
