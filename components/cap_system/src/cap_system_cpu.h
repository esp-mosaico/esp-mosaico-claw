/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Normalize summed per-core Idle counters against the shared runtime clock.
 * This is a cumulative counter snapshot, not an interval utilization sample.
 */
static inline bool cap_system_cpu_usage(uint32_t clock_ticks, uint64_t idle_ticks,
                                       unsigned cores, double *usage_percent)
{
    if (clock_ticks == 0 || cores == 0 || usage_percent == NULL) {
        return false;
    }
    const uint64_t capacity = (uint64_t)clock_ticks * cores;
    if (idle_ticks > capacity) {
        return false;
    }
    *usage_percent = 100.0 * (1.0 - (double)idle_ticks / (double)capacity);
    return true;
}
