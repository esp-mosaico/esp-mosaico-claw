/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once
#include "gsp/gsp_build_caps.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The velocity estimator uses an 80 ms motion window. Ten entries retain that
 * full window at the ESP adapter's 10 ms active-input cadence. */
#define GSP_SWIPE_KINETICS_SAMPLES ESP_GSP_BUILD_CAP_SWIPE_KINETICS_SAMPLES

typedef struct {
    uint32_t velocity_window_ms;
    uint32_t release_grace_ms;
    uint32_t settle_min_ms;
    uint32_t settle_base_ms;
    uint32_t settle_distance_ms;
    uint32_t settle_max_ms;
    uint32_t settle_speed_reduction_ms;
    uint32_t settle_max_speed_reduction_ms;
    uint32_t max_velocity_px_s;
} gsp_swipe_kinetics_policy_t;

static inline gsp_swipe_kinetics_policy_t gsp_swipe_policy_from_config(
    const esp_gsp_policy_t *policy)
{
    return (gsp_swipe_kinetics_policy_t) {
        .velocity_window_ms = policy->swipe_velocity_window_ms,
        .release_grace_ms = policy->swipe_release_grace_ms,
        .settle_min_ms = policy->swipe_settle_min_ms,
        .settle_base_ms = policy->swipe_settle_base_ms,
        .settle_distance_ms = policy->swipe_settle_distance_ms,
        .settle_max_ms = policy->swipe_settle_max_ms,
        .settle_speed_reduction_ms = policy->swipe_settle_speed_reduction_ms,
        .settle_max_speed_reduction_ms =
            policy->swipe_settle_max_speed_reduction_ms,
            .max_velocity_px_s = policy->scroll_max_release_speed_px_s,
    };
}

/** Portable page-swipe velocity tracker and release settle state. */
typedef struct {
    uint32_t policy_magic;
    gsp_swipe_kinetics_policy_t policy;
    struct {
        uint16_t position;
        int64_t time_us;
    } samples[GSP_SWIPE_KINETICS_SAMPLES];
    uint8_t sample_count;
    int64_t last_observed_time_us;
    bool settling;
    uint16_t settle_from;
    uint16_t settle_to;
    int32_t settle_tangent;
    int64_t settle_start_us;
    uint32_t settle_duration_us;
} gsp_swipe_kinetics_t;

void gsp_swipe_kinetics_configure(
    gsp_swipe_kinetics_t *kinetics,
    const gsp_swipe_kinetics_policy_t *policy);

/** Shared bounded settle duration for a one-axis gesture. */
uint32_t gsp_kinetic_settle_duration(uint32_t distance, uint32_t span,
                                     int32_t velocity_px_s);
uint32_t gsp_kinetic_settle_duration_with_policy(
    const gsp_swipe_kinetics_policy_t *policy, uint32_t distance,
    uint32_t span, int32_t velocity_px_s);

/** Monotonic Hermite tangent derived from signed axis velocity. */
int32_t gsp_kinetic_settle_tangent(int32_t delta, int32_t velocity_px_s,
                                   uint32_t duration_us);

/** Reset tracking at the start of a newly claimed page swipe. */
void gsp_swipe_kinetics_reset(gsp_swipe_kinetics_t *kinetics,
                              uint16_t position, int64_t now_us);

/** Record the newest direction-normalized drag position. */
void gsp_swipe_kinetics_sample(gsp_swipe_kinetics_t *kinetics,
                               uint16_t position, int64_t now_us);

/** Recent release velocity in direction-normalized pixels per second. */
int32_t gsp_swipe_kinetics_velocity(const gsp_swipe_kinetics_t *kinetics,
                                    bool *out_valid);

/**
 * Start a bounded monotonic settle. Release velocity shortens the duration
 * and supplies the initial tangent when it points toward the destination.
 */
void gsp_swipe_kinetics_settle_begin(gsp_swipe_kinetics_t *kinetics,
                                     uint16_t from, uint16_t to,
                                     uint16_t span, int32_t velocity_px_s,
                                     int64_t now_us);

/** Advance the time-based settle and return its absolute progress. */
uint16_t gsp_swipe_kinetics_settle_step(gsp_swipe_kinetics_t *kinetics,
                                        int64_t now_us, bool *out_done);

#ifdef __cplusplus
}
#endif
