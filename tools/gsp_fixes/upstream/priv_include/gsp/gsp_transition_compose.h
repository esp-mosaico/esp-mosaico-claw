/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Transition style for scene changes. */
typedef enum {
    GSP_TRANSITION_SLIDE_LEFT = 0,  /*!< to-scene enters from the right */
    GSP_TRANSITION_SLIDE_RIGHT,     /*!< to-scene enters from the left */
    GSP_TRANSITION_SLIDE_UP,        /*!< to-scene enters from the bottom */
    GSP_TRANSITION_SLIDE_DOWN,      /*!< to-scene enters from the top */
    GSP_TRANSITION_FADE,            /*!< to-scene cross-fades in */
    GSP_TRANSITION_FADE_THROUGH_BLACK, /*!< old dims, new brightens */
    GSP_TRANSITION_SLIDE_LEFT_FADE_THROUGH_BLACK,
    GSP_TRANSITION_SLIDE_RIGHT_FADE_THROUGH_BLACK,
} gsp_transition_kind_t;

static inline bool gsp_transition_kind_is_valid(gsp_transition_kind_t kind)
{
    return (unsigned)kind <=
           (unsigned)GSP_TRANSITION_SLIDE_RIGHT_FADE_THROUGH_BLACK;
}

static inline bool gsp_transition_is_slide_fade(
    gsp_transition_kind_t kind)
{
    return kind == GSP_TRANSITION_SLIDE_LEFT_FADE_THROUGH_BLACK ||
           kind == GSP_TRANSITION_SLIDE_RIGHT_FADE_THROUGH_BLACK;
}

static inline bool gsp_transition_is_fade_through_black(
    gsp_transition_kind_t kind)
{
    return kind == GSP_TRANSITION_FADE_THROUGH_BLACK ||
           gsp_transition_is_slide_fade(kind);
}

static inline bool gsp_transition_is_fade(gsp_transition_kind_t kind)
{
    return kind == GSP_TRANSITION_FADE ||
           gsp_transition_is_fade_through_black(kind);
}

/** One rectangular band of a slide composite (full height for the
 *  horizontal kinds, full width for the vertical ones). */
typedef struct {
    uint16_t dst_x;                 /*!< destination column */
    uint16_t dst_y;                 /*!< destination row */
    uint16_t src_x;                 /*!< column inside the source frame */
    uint16_t src_y;                 /*!< row inside the source frame */
    uint16_t width;                 /*!< columns, never zero */
    uint16_t height;                /*!< rows, never zero */
    bool from_source;               /*!< true = from_frame, else to_frame */
} gsp_transition_segment_t;

/**
 * Split geometry of a slide at @p progress_px (0..width for horizontal
 * kinds, 0..height for vertical ones): at most two bands, in
 * destination order. Shared by the software compositor below and the
 * hardware blit path so both stay pixel-identical. Pure fade kinds are
 * not copies and return zero segments; slide-fade kinds return their
 * slide geometry before the composed frame is dimmed.
 */
uint32_t gsp_transition_segments(uint16_t width, uint16_t height,
                                 gsp_transition_kind_t kind,
                                 uint16_t progress_px,
                                 gsp_transition_segment_t segments[2]);

/** Full progress span for a kind: width for horizontal slides,
 *  height for vertical slides and fade kinds. */
static inline uint16_t gsp_transition_span(uint16_t width,
        uint16_t height,
        gsp_transition_kind_t kind)
{
    return kind == GSP_TRANSITION_SLIDE_LEFT ||
           kind == GSP_TRANSITION_SLIDE_RIGHT ||
           gsp_transition_is_slide_fade(kind) ? width : height;
}

/**
 * Map a logical travel distance onto the compositor's progress span.
 *
 * A viewport jump reports progress in transform offset pixels, so crossing N
 * pages spans N viewports while the compositor only knows one and clamps
 * anything past it. Left unscaled the visual would finish in the first 1/N of
 * the settle, making distant jumps look faster than neighbouring ones.
 * Travel within one span passes through untouched.
 */
static inline uint16_t gsp_transition_normalize_progress(
    uint16_t progress, uint32_t travel, uint16_t span)
{
    if (travel <= span) {
        return progress;
    }
    return (uint16_t)(((uint64_t)progress * span + travel / 2U) / travel);
}

/**
 * Slice of an overlay drawer panel that is on screen at @p visible pixels
 * of entry, in viewport-local coordinates, plus where it starts inside the
 * frozen open frame.
 *
 * Overlay drawers do not slide the base page: it stays put and the panel
 * travels over it, so a composite is the base frame plus this one slice.
 * That makes it a different split from gsp_transition_segments(), which
 * moves both frames. @p kind is the direction the panel travels, matching
 * the transition kind the drawer was opened with. Returns false when the
 * panel is fully retracted and the base alone is the composite.
 */
static inline bool gsp_drawer_overlay_slice(
    uint16_t width, uint16_t height, gsp_transition_kind_t kind,
    uint16_t visible, gsp_transition_segment_t *out)
{
    if (out == NULL || visible == 0U || width == 0U || height == 0U) {
        return false;
    }
    out->from_source = false;
    switch (kind) {
    case GSP_TRANSITION_SLIDE_DOWN:
        /* Top drawer: its bottom edge enters at y=0, so the panel is
         * revealed from its own bottom up. */
        if (visible > height) {
            visible = height;
        }
        out->dst_x = 0;
        out->dst_y = 0;
        out->src_x = 0;
        out->src_y = (uint16_t)(height - visible);
        out->width = width;
        out->height = visible;
        return true;
    case GSP_TRANSITION_SLIDE_UP:
        /* Bottom drawer: its top edge enters at the bottom of the view. */
        if (visible > height) {
            visible = height;
        }
        out->dst_x = 0;
        out->dst_y = (uint16_t)(height - visible);
        out->src_x = 0;
        out->src_y = 0;
        out->width = width;
        out->height = visible;
        return true;
    case GSP_TRANSITION_SLIDE_RIGHT:
        /* Left drawer: revealed from its own right edge inward. */
        if (visible > width) {
            visible = width;
        }
        out->dst_x = 0;
        out->dst_y = 0;
        out->src_x = (uint16_t)(width - visible);
        out->src_y = 0;
        out->width = visible;
        out->height = height;
        return true;
    case GSP_TRANSITION_SLIDE_LEFT:
        /* Right drawer. */
        if (visible > width) {
            visible = width;
        }
        out->dst_x = (uint16_t)(width - visible);
        out->dst_y = 0;
        out->src_x = 0;
        out->src_y = 0;
        out->width = visible;
        out->height = height;
        return true;
    default:
        return false;
    }
}

/**
 * Pure transition compositor: writes an RGB565 frame at
 * @p progress_px (0..gsp_transition_span()) into @p out from two
 * pre-rendered scene frames. Slides are band copies; FADE is an exact
 * cross-blend, while FADE_THROUGH_BLACK uses a black midpoint. No OS
 * or panel dependencies — shared by
 * the device present wrapper and host verification, and usable
 * directly by applications that composite into their own buffers.
 */
void gsp_transition_compose(uint8_t *out, size_t out_stride,
                            const uint8_t *from_frame,
                            const uint8_t *to_frame,
                            uint16_t width, uint16_t height,
                            size_t frame_stride,
                            gsp_transition_kind_t kind,
                            uint16_t progress_px);

/** Profile-aware variant used by the device transition path. RGB888 is
 * packed in the renderer's native BGR byte order. */
void gsp_transition_compose_format(uint8_t *out, size_t out_stride,
                                   const uint8_t *from_frame,
                                   const uint8_t *to_frame,
                                   uint16_t width, uint16_t height,
                                   size_t frame_stride,
                                   gsp_transition_kind_t kind,
                                   uint16_t progress_px,
                                   uint8_t pixel_format);

/**
 * Select the active scene and brightness for fade-through-black.
 *
 * @return Brightness in the range 0..255. @p out_use_from is true before
 *         and at the black midpoint, and false while revealing the target.
 */
uint8_t gsp_transition_fade_through_black_step(
    uint16_t span, uint16_t progress_px, bool *out_use_from);

/**
 * Compose with independent geometry and brightness progress.
 *
 * Existing gsp_transition_compose_format() remains the equal-progress
 * convenience path used by programmatic transitions.
 */
void gsp_transition_compose_progress_format(
    uint8_t *out, size_t out_stride,
    const uint8_t *from_frame, const uint8_t *to_frame,
    uint16_t width, uint16_t height, size_t frame_stride,
    gsp_transition_kind_t kind, uint16_t position_progress_px,
    uint16_t fade_progress_px, uint8_t pixel_format);

/** Dim one packed RGB565/RGB888 surface in place. */
void gsp_transition_dim_format(uint8_t *pixels, size_t stride_bytes,
                               uint16_t width, uint16_t height,
                               uint8_t intensity, uint8_t pixel_format);

#ifdef __cplusplus
}
#endif
