/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pointer gesture classifier: one press is claimed by exactly one
 * gesture owner — a capturing control (value drag), a tap-capable hit,
 * a vertical scroll viewport, or a horizontal page swipe. A plain hit
 * target taps only if the entire contact stays within the slop, and may
 * escalate to vertical scroll or deliberate horizontal page swipe.
 * Capturing controls keep ownership until release; disabled targets
 * consume the sequence. The classifier owns the decision state machine
 * only; every effect is a
 * callback, so the same machine serves hardware touch, synthetic
 * input, and host simulations.
 *
 * Coordinates are logical (scene space). The lifted-finger sample of
 * touch controllers carries no coordinates: release logic reuses the
 * last pressed position.
 */

/** What the press landed on (returned by ops->press). */
typedef enum {
    GSP_POINTER_CLAIM_NONE = 0, /*!< empty space: swipe + tap allowed */
    GSP_POINTER_CLAIM_HIT,      /*!< tap-capable target: tap on release;
                                     deliberate drag may become
                                     scroll/swipe */
    GSP_POINTER_CLAIM_CAPTURE,  /*!< control tracks the pointer until
                                     release (slider/arc drag) */
    GSP_POINTER_CLAIM_BLOCK,    /*!< topmost disabled target consumes the
                                     complete pointer sequence */
} gsp_pointer_claim_t;

typedef enum {
    GSP_POINTER_GESTURE_NONE = 0,
    GSP_POINTER_GESTURE_TRACK_X,
    GSP_POINTER_GESTURE_TRACK_Y,
    /** A topmost input barrier consumed the drag without tracking an axis. */
    GSP_POINTER_GESTURE_CONSUMED,
} gsp_pointer_gesture_result_t;

typedef struct {
    /** Pointer down at (x, y): hit test, press feedback, capture. */
    gsp_pointer_claim_t (*press)(void *ctx, int32_t x, int32_t y);
    /** Captured control follows the pointer. */
    void (*capture_move)(void *ctx, int32_t x, int32_t y);
    void (*capture_release)(void *ctx);
    /** True when the press position lies in a vertical scroll
     *  viewport; evaluated when the drag crosses the slop. */
    bool (*scroll_hit)(void *ctx, int32_t press_x, int32_t press_y);
    /** Starts a scroll after @p consumed_delta_y was reserved for tap
     *  arbitration. The value is signed finger motion and must contribute to
     *  fling velocity, but not to the first visible content delta. */
    void (*scroll_begin)(void *ctx, int32_t consumed_delta_y);
    /** Incremental vertical delta (only nonzero deltas). */
    void (*scroll_move)(void *ctx, int32_t delta_y);
    void (*scroll_release)(void *ctx);
    /** Resolve a component-local gesture after control capture and vertical
     * list-scroll arbitration. */
    gsp_pointer_gesture_result_t (*gesture_begin)(
        void *ctx, int32_t dx, int32_t dy);
    void (*gesture_move)(void *ctx, int32_t x, int32_t y);
    /** Flick is the signed sum of the last three deltas on the locked axis. */
    void (*gesture_release)(void *ctx, int32_t x, int32_t y, int32_t flick);
    /** Start a page swipe; false when unavailable (single scene,
     *  disabled, target not ready) — retried while the drag holds. */
    bool (*swipe_begin)(void *ctx, bool towards_next);
    void (*swipe_move)(void *ctx, uint32_t distance);
    /** @p flick sums the last 3 swipe-distance deltas (~30 ms of
     *  movement): positive = still moving toward the target on release
     *  (commit the page turn), negative = flicked back (abort). */
    void (*swipe_release)(void *ctx, uint32_t distance, int32_t flick);
    /** Every pressed sample stayed within the tap slop. */
    void (*tap)(void *ctx, int32_t x, int32_t y);
    /** Every release, before the owner callback (feedback teardown). */
    void (*release)(void *ctx);
    void (*cancel)(void *ctx);
} gsp_pointer_ops_t;

typedef enum {
    GSP_POINTER_OWNER_NONE = 0, /*!< idle, or press not yet classified */
    GSP_POINTER_OWNER_HIT,
    GSP_POINTER_OWNER_CAPTURE,
    GSP_POINTER_OWNER_SCROLL,
    GSP_POINTER_OWNER_GESTURE,
    GSP_POINTER_OWNER_SWIPE,
    GSP_POINTER_OWNER_CONSUMED,
} gsp_pointer_owner_t;

typedef struct {
    const gsp_pointer_ops_t *ops;
    void *ctx;
    int16_t tap_slop;
    int16_t drag_start;
    bool pressed;
    /** Latched until the next DOWN; returning to the origin cannot restore a tap. */
    bool tap_cancelled;
    gsp_pointer_owner_t owner;
    int32_t press_x;
    int32_t press_y;
    int32_t last_x;
    int32_t last_y;
    int32_t scroll_last_y;
    /**
     * Locked at swipe_begin: true = finger-left / next page.
     * Progress is displacement along this axis only (reversing past
     * the press point clamps to 0); never abs(dx), which inverted the
     * slide when the finger crossed back.
     */
    bool swipe_towards_next;
    /* Flick sampling: px deltas of the swipe distance over the last
     * 3 samples (feeds swipe_release's flick). */
    int32_t swipe_recent[3];
    uint8_t swipe_head;
    uint32_t swipe_last_distance;
    gsp_pointer_gesture_result_t gesture_axis;
    int32_t gesture_recent[3];
    uint8_t gesture_head;
    int32_t gesture_last;
} gsp_pointer_t;

/** Every op must be non-NULL (use empty stubs, not NULL checks). */
void gsp_pointer_init(gsp_pointer_t *pointer,
                      const gsp_pointer_ops_t *ops, void *ctx,
                      int16_t tap_slop, int16_t drag_start);

/** Feeds one pointer sample (any source, logical coordinates). */
void gsp_pointer_feed(gsp_pointer_t *pointer, int32_t x, int32_t y,
                      bool pressed);
void gsp_pointer_cancel(gsp_pointer_t *pointer);

static inline bool gsp_pointer_pressed(const gsp_pointer_t *pointer)
{
    return pointer->pressed;
}

static inline bool gsp_pointer_scrolling(const gsp_pointer_t *pointer)
{
    return pointer->owner == GSP_POINTER_OWNER_SCROLL;
}

static inline bool gsp_pointer_swiping(const gsp_pointer_t *pointer)
{
    return pointer->owner == GSP_POINTER_OWNER_SWIPE;
}

#ifdef __cplusplus
}
#endif
