/* SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT */
#pragma once

#include "gsp/gsp_viewport_track.h"
#include "gsp/gsp_component_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GSP_PAGE_FLOW_DRIVER_EXT_SIZE 64U

typedef struct {
    gsp_viewport_track_state_t track;
    uint16_t selected;
    uint16_t target;
    uint8_t normalize;
    uint8_t coasting;
    uint8_t pending_events;
    uint8_t reserved;
    int32_t normalize_offset;
    int32_t coast_velocity;
    int64_t coast_pos_q16;
    int64_t coast_last_us;
} gsp_page_flow_state_t;

extern const gsp_component_driver_t gsp_page_flow_driver;

gsp_err_t gsp_page_flow_driver_set_page(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    uint16_t page, bool animated, uint64_t now_us);
gsp_err_t gsp_page_flow_driver_set_page_direction(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    uint16_t page, bool towards_next, uint64_t now_us);
gsp_err_t gsp_page_flow_driver_set_page_kinetic(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    uint16_t page, int32_t velocity_px_s, uint64_t now_us);
gsp_err_t gsp_page_flow_driver_set_page_direction_kinetic(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    uint16_t page, bool towards_next, int32_t velocity_px_s,
    uint64_t now_us);
gsp_err_t gsp_page_flow_driver_begin_drag(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    int32_t x, int32_t y);
gsp_err_t gsp_page_flow_driver_stop_drag_kinetic(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    int32_t velocity_px_s, uint64_t now_us);
uint16_t gsp_page_flow_driver_selected(
    const gsp_component_instance_t *instance);
bool gsp_page_flow_driver_dragging(const gsp_component_instance_t *instance);

#ifdef __cplusplus
}
#endif
