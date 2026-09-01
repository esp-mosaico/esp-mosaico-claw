/* Internal built-in capability adapter registration. */
#pragma once

#include "esp_err.h"
#include "mosaic_system_capability.h"

esp_err_t mosaic_capability_battery_register(
    const mosaic_system_capability_ops_t *ops);
esp_err_t mosaic_capability_time_register(void);
esp_err_t mosaic_capability_status_register(
    const mosaic_system_capability_ops_t *ops);
