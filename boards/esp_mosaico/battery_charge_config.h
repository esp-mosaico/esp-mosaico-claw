/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include "bq27220.h"

typedef struct {
    uint16_t charging_voltage_mv;
    uint16_t taper_current_ma;
    /** Margin below ChargingVoltage, not an absolute cell voltage. */
    uint16_t taper_voltage_mv;
} mosaico_battery_charge_config_t;

/** Preserve learned FCC only when the existing design capacity matches. */
esp_err_t mosaico_battery_read_learned_capacity(
    i2c_bus_handle_t bus, uint16_t design_mah, uint16_t *capacity_mah);

/** Boot-only operation, before the gauge handle is shared with readers.
 * Expects the sealed handle returned by bq27220_create(). Applies RAM data
 * memory, verifies writes, and attempts CFGUPDATE exit/resealing even on errors.
 * Cleanup failures are returned; successful writes alone do not imply success.
 * No OTP programming or charger settings are changed. */
esp_err_t mosaico_battery_apply_charge_config(
    i2c_bus_handle_t bus, bq27220_handle_t gauge,
    const mosaico_battery_charge_config_t *config);
