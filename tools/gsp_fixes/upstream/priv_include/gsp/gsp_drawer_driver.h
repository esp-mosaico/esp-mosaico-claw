/* SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT */
#pragma once

#include "gsp/gsp_viewport_track.h"
#include "gsp/gsp_component_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GSP_DRAWER_DRIVER_EXT_SIZE 40U

typedef struct {
    gsp_viewport_track_state_t track;
    uint8_t open;
    uint8_t target_open;
    uint8_t pending_events;
} gsp_drawer_state_t;

extern const gsp_component_driver_t gsp_drawer_driver;

gsp_err_t gsp_drawer_driver_set_open(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    bool open, bool animated, uint64_t now_us);
gsp_err_t gsp_drawer_driver_begin_drag(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    int32_t x, int32_t y);
gsp_err_t gsp_drawer_driver_end_drag(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    bool open, uint64_t now_us);
gsp_err_t gsp_drawer_driver_end_drag_kinetic(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    bool open, int32_t velocity_px_s, uint64_t now_us);
bool gsp_drawer_driver_is_open(const gsp_component_instance_t *instance);
bool gsp_drawer_driver_animating(
    const gsp_component_instance_t *instance);

#ifdef __cplusplus
}
#endif
