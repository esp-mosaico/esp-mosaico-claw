/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "tof_objects.h"

#include "mosaic_demo.h"

static void tof_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    mosaic_demo_tick(ui, MOSAIC_DEMO_TOF);
}

static void tof_started(esp_gsp_handle_t ui)
{
    tof_tick(ui, NULL);
    (void)esp_gsp_timer_create(ui, 800, tof_tick, NULL);
}

const mosaic_app_descriptor_t mosaic_tof_app = {
    .id = 4,
    .launch_action = MOSAIC_APP_NO_LAUNCH_ACTION,
    .name = "tof",
    .title = "TOF",
    .directory = &gsp_obj_directory_tof,
    .disable_swipe = true,
    .on_started = tof_started,
};
