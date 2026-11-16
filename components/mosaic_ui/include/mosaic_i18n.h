/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOSAIC_UI_LANGUAGE_EN = 0,
    MOSAIC_UI_LANGUAGE_ZH_CN,
} mosaic_ui_language_t;

typedef struct {
    uint16_t bind;
    const char *english;
} mosaic_i18n_component_t;

void mosaic_i18n_set_language(mosaic_ui_language_t language);
mosaic_ui_language_t mosaic_i18n_get_language(void);
const char *mosaic_i18n_text(const char *english);
esp_err_t mosaic_i18n_apply_components(esp_gsp_handle_t ui, const mosaic_i18n_component_t *components, size_t count);

#ifdef __cplusplus
}
#endif
