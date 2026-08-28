/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_gsp.h"
#include "music_controller.h"

typedef struct music_presenter_t *music_presenter_handle_t;

esp_err_t music_presenter_create(music_presenter_handle_t *ret_presenter);
void music_presenter_delete(music_presenter_handle_t presenter);
void music_presenter_invalidate(music_presenter_handle_t presenter);
esp_err_t music_presenter_render(music_presenter_handle_t presenter,
    esp_gsp_handle_t ui, const music_snapshot_t *snapshot, bool force);
