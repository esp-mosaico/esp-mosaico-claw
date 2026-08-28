/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

#include "esp_gsp.h"

typedef struct {
    uint16_t visible_bind;
    uint16_t title_bind;
    uint16_t message_bind;
} mosaic_top_notice_config_t;

/** Show one standard top capsule and replace any currently active notice.
 * Text is copied by GSP. duration_ms == 0 keeps it visible until hidden. */
esp_gsp_err_t mosaic_top_notice_show(
    esp_gsp_handle_t ui, const mosaic_top_notice_config_t *config,
    const char *title, const char *message, uint32_t duration_ms);

void mosaic_top_notice_hide(esp_gsp_handle_t ui);
void mosaic_top_notice_detach(esp_gsp_handle_t ui);
