/* SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT */
#pragma once

#include "gsp/gsp_component_driver.h"
#include "gsp/gsp_viewport_track.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gsp_viewport_track_state_t track;
    uint16_t *pages;
    uint16_t target_page;
    uint8_t capacity;
    uint8_t depth;
    uint8_t pending_depth;
} gsp_stack_view_state_t;

/* Page ids are allocated per active StackView; the extension stride no longer
 * depends on the configured navigation depth. */
#define GSP_STACK_VIEW_DRIVER_EXT_SIZE \
    ((sizeof(gsp_stack_view_state_t) + 7U) & ~(size_t)7U)

extern const gsp_component_driver_t gsp_stack_view_driver;

gsp_err_t gsp_stack_view_driver_push(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    uint16_t page, bool animated, uint64_t now_us);
gsp_err_t gsp_stack_view_driver_pop(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance,
    bool animated, uint64_t now_us);
gsp_err_t gsp_stack_view_driver_finish(
    gsp_component_manager_t *manager, gsp_component_instance_t *instance);
uint16_t gsp_stack_view_driver_top(
    const gsp_component_instance_t *instance);
uint8_t gsp_stack_view_driver_depth(
    const gsp_component_instance_t *instance);
/**
 * Depth a new navigation would start from. An in-flight settle is
 * committed by the next push/pop, so callers that decide before the
 * driver runs (capacity checks, transition targets) must read this
 * rather than the still-uncommitted depth.
 */
uint8_t gsp_stack_view_driver_pending_depth(
    const gsp_component_instance_t *instance);
/**
 * Page the next pop lands on, from the same pending depth. False when
 * the stack is already at its root. Pages are authored ids, not stack
 * levels, so the caller cannot derive the landing offset from depth.
 */
bool gsp_stack_view_driver_pop_target(
    const gsp_component_instance_t *instance, uint16_t *out_page);
bool gsp_stack_view_driver_animating(
    const gsp_component_instance_t *instance);

#ifdef __cplusplus
}
#endif
