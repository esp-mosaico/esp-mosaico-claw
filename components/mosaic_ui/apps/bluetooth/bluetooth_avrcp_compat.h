/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t duration_ms;
    uint32_t position_ms;
    bool playing;
} bluetooth_avrcp_play_status_t;

void bluetooth_avrcp_compat_reset(void);
esp_err_t bluetooth_avrcp_compat_request_play_status(void);
bool bluetooth_avrcp_compat_get_play_status(
    bluetooth_avrcp_play_status_t *out_status, uint32_t *out_revision);
