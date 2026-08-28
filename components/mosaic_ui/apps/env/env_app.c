/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "env_objects.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"

#include "mosaic_demo.h"

static void env_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    mosaic_demo_tick(ui, MOSAIC_DEMO_ENV);
}

static void env_started(esp_gsp_handle_t ui)
{
    env_tick(ui, NULL);
    (void)esp_gsp_timer_create(ui, 800, env_tick, NULL);
}

const mosaic_app_descriptor_t mosaic_env_app = {
    .id = 3,
    .launch_action = MOSAIC_APP_NO_LAUNCH_ACTION,
    .name = "env",
    .title = "Env",
    .directory = &gsp_obj_directory_env,
    .disable_swipe = true,
    .on_started = env_started,
};
