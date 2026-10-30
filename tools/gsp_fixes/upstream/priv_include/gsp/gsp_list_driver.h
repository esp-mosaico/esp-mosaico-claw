/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once
#include "gsp/gsp_build_caps.h"

#include "gsp/gsp_component_driver.h"
#include "gsp/gsp_repeater.h"
#include "gsp/gsp_scroll_phys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A scrolling viewport keeps row instances, recycle bookkeeping and
 * scroll physics in its instance extension while visible. A hidden
 * viewport returns rows and the extension to their pools while its
 * binding and persisted offset survive. Instances remain active for
 * the visible lifetime but request frame ticks only while coasting.
 */

/** The typed extension: everything a visible list needs. The
 *  platform owner allocates row arrays at activation using the effective
 *  list slot capacity; deactivation deinits and frees them. */
typedef struct {
    gsp_repeater_t repeater;
    gsp_scroll_phys_t phys;
    gsp_instance_handle_t *rows;
    uint32_t *items;
    size_t slot_capacity;
    uint32_t logical_total;
    int32_t cycle_extent;
    int32_t cycle_anchor;
} gsp_list_state_t;

void gsp_list_cyclic_normalize(gsp_list_state_t *state);

/** Exact extension stride for the active target ABI. The old fixed
 *  64-bit-host upper bound wasted 156 bytes per list on 32-bit devices. */
#define GSP_LIST_DRIVER_EXT_SIZE sizeof(gsp_list_state_t)

extern const gsp_component_driver_t gsp_list_driver;

#ifdef __cplusplus
}
#endif
