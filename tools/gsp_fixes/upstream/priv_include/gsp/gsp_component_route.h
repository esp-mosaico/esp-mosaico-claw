/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include "gsp/gsp_component_driver.h"
#include "gsp/gsp_input.h"
#include "gsp/gsp_pointer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Platform-free press ownership for interactive targets. The caller
 * supplies the hit result; the router scans compiled actions,
 * activates the owning driver and takes the manager's pointer
 * capture. Press feedback, list scroll regions and page swipes remain
 * caller responsibilities.
 *
 * With a component directory registered on the manager, the press
 * resolves the target bind to its descriptor entry — the manager
 * instance is keyed by descriptor index. Without one (legacy scenes),
 * the bind id doubles as the anonymous entry key.
 */

/** Driver ids the platform registered for the behavior families. */
typedef struct {
    uint16_t boolean_driver;
    uint16_t range_driver;
} gsp_component_route_config_t;

/** gsp_component_route_resolve: a directory is registered but the bind
 *  is not retained in it (compiler must retain input-bearing
 *  components). */
#define GSP_COMPONENT_ROUTE_NO_ENTRY UINT16_MAX

/** Routes a press over @p hit. Returns CAPTURE when a drag driver
 *  took the pointer (capture staged + committed), HIT for targets the
 *  tap path owns (toggles keep their knob tween on release), NONE
 *  when no compiled action claims the press. */
gsp_pointer_claim_t gsp_component_route_press(
    gsp_component_manager_t *manager,
    const gsp_component_route_config_t *config,
    const gsp_scene_view_t *scene,
    const gsp_hit_result_t *hit,
    int32_t x, int32_t y);

/** Resolves @p bind to the manager's instance key: the descriptor
 *  index when a directory is registered and the bind belongs to a
 *  retained component, the bind id itself otherwise (legacy). */
uint16_t gsp_component_route_resolve(
    const gsp_component_manager_t *manager,
    uint16_t bind);

/** GSB 1.1 direct-property counterpart. @p property is the zero-based
 *  property index carried by gsp_action_view_t. */
uint16_t gsp_component_route_resolve_property(
    const gsp_component_manager_t *manager,
    uint16_t property);

#ifdef __cplusplus
}
#endif
