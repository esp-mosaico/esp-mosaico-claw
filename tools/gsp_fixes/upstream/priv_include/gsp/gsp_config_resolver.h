/* SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT */
#pragma once

#include "gsp/gsp_config.h"
#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

gsp_err_t gsp_config_resolve(
    const esp_gsp_config_overrides_t *requirements,
    const esp_gsp_config_overrides_t *overrides,
    esp_gsp_effective_config_t *out_config,
    esp_gsp_config_field_id_t *out_error_field);

gsp_err_t gsp_platform_config_acquire(
    const esp_gsp_effective_config_t *config,
    esp_gsp_config_field_id_t *out_error_field,
    uint32_t *out_active_value, uint32_t *out_requested_value);
void gsp_platform_config_release(void);

#ifdef __cplusplus
}
#endif
