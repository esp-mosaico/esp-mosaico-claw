/* SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "gsp/gsp_swipe_kinetics.h"

typedef struct {
    int32_t start_offset;
    int32_t target_offset;
    int32_t drag_origin;
    int32_t pointer_origin;
    uint64_t start_us;
    uint32_t duration_us;
    uint8_t dragging;
    uint8_t settling;
    uint8_t kinetic;
    uint8_t easing;
} gsp_viewport_track_state_t;

enum {
    GSP_VIEWPORT_EASING_LINEAR = 0,
    GSP_VIEWPORT_EASING_OUT_CUBIC = 1,
    GSP_VIEWPORT_EASING_IN_OUT_CUBIC = 2,
    GSP_VIEWPORT_EASING_OUT_QUAD = 3,
};

void gsp_viewport_track_begin_drag(gsp_viewport_track_state_t *state,
                                   int32_t offset, int32_t pointer);
int32_t gsp_viewport_track_drag(const gsp_viewport_track_state_t *state,
                                int32_t pointer,
                                int32_t anchor_a, int32_t anchor_b);
bool gsp_viewport_track_begin_settle(gsp_viewport_track_state_t *state,
                                     int32_t current, int32_t target,
                                     uint32_t duration_us, uint64_t now_us);
bool gsp_viewport_track_begin_settle_eased(
    gsp_viewport_track_state_t *state, int32_t current, int32_t target,
    uint32_t duration_us, uint8_t easing, uint64_t now_us);
bool gsp_viewport_track_begin_kinetic_settle(
    gsp_viewport_track_state_t *state, int32_t current, int32_t target,
    uint32_t span, int32_t velocity_px_s,
    const gsp_swipe_kinetics_policy_t *policy, uint64_t now_us);
int32_t gsp_viewport_track_tick(gsp_viewport_track_state_t *state,
                                uint64_t now_us, bool *out_done);
bool gsp_viewport_track_choose_second(int32_t offset,
                                      int32_t first, int32_t second);
