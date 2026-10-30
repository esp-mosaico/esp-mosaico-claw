/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include "gsp/gsp_component_driver.h"
#include "gsp/gsp_value_drag.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Range driver (slider / arc drag-to-set): owns the pointer capture
 * for value controls. Capture starts with
 * gsp_range_driver_capture (decodes the compiled VALUE action arg,
 * applies the press position immediately); MOVE events re-map the
 * pointer through gsp_value_drag; UP/CANCEL deactivate the instance.
 */

/** Static storage bound for the driver's extension blob. */
#define GSP_RANGE_DRIVER_EXT_SIZE 40

extern const gsp_component_driver_t gsp_range_driver;

/** Begins a value drag: configures the mapping from the compiled
 *  action arg + hit bbox (the drag track) and stages the press
 *  position's value into @p transaction. */
gsp_err_t gsp_range_driver_capture(gsp_component_manager_t *manager,
                                   gsp_component_instance_t *instance,
                                   uint32_t action_arg,
                                   uint16_t target_bind,
                                   uint16_t target_property,
                                   gsp_rect_t track,
                                   int32_t x, int32_t y,
                                   gsp_update_transaction_t *transaction);

#ifdef __cplusplus
}
#endif
