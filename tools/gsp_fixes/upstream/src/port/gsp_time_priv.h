/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Compiled with the consuming firmware, outside the prebuilt core archive. */
uint32_t esp_gsp_project_tick_rate_hz(void);
#ifdef __cplusplus
}
#endif
#endif

static inline uint32_t gsp_port_tick_rate_hz(void)
{
#ifdef ESP_PLATFORM
    return esp_gsp_project_tick_rate_hz();
#else
    return 1000U;
#endif
}

/* Preserve FreeRTOS's round-down conversion without baking the archive's
 * tick rate into waits. IDF supports 1..1000 Hz, so the result fits uint32_t. */
static inline uint32_t gsp_port_ms_to_ticks(uint32_t milliseconds)
{
    return (uint32_t)((uint64_t)milliseconds * gsp_port_tick_rate_hz() / 1000U);
}

/*
 * Monotonic microseconds for the profiling counters, or 0 where the port
 * has no clock.
 *
 * The host build deliberately has none: the bit-exact gates compare
 * rendered frames and reported counters across runs, so a real clock
 * would make them report a different number every time. Timing is
 * measured on device, where it means something.
 */
static inline uint64_t gsp_port_time_us(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    return 0;
#endif
}
