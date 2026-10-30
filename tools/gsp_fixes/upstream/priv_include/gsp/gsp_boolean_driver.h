/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include "gsp/gsp_component_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Boolean driver (toggle knob tween). Register it in the manager's
 * driver table, activate an instance for the toggle, then call
 * gsp_boolean_driver_toggle on each tap: the knob tweens to the
 * other rail through manager ticks. The instance deactivates itself
 * when the tween lands.
 */

/** Static storage bound for the driver's extension blob. */
#define GSP_BOOLEAN_DRIVER_EXT_SIZE 32

extern const gsp_component_driver_t gsp_boolean_driver;

/** Typed view of the ANIMATE_TOGGLE companion track-color payload. */
typedef struct {
    uint16_t bind;
    uint16_t property;  /*!< UINT16_MAX for the GSB 1.0 bind fallback */
    uint32_t off_color;
    uint32_t on_color;
} gsp_boolean_track_tween_t;

/** Decode the version-compatible 10/12-byte toggle companion payload. */
gsp_err_t gsp_boolean_track_tween_decode(
    const uint8_t *params, size_t param_len,
    gsp_boolean_track_tween_t *out_tween);

/** Reads the direct GSB 1.1 property when @p value_property is valid,
 *  otherwise the compatibility bind; picks the opposite rail
 *  (current < 50 turns ON) and starts the 130 ms ease-out tween.
 *  Reports the chosen direction so callers can drive companion
 *  effects (the toggle track color). */
gsp_err_t gsp_boolean_driver_toggle(gsp_component_manager_t *manager,
                                    gsp_component_instance_t *instance,
                                    uint16_t value_bind,
                                    uint16_t value_property,
                                    uint64_t now_us,
                                    bool *out_turn_on);

#ifdef __cplusplus
}
#endif
