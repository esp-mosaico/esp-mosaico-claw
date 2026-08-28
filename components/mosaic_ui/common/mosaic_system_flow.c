/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_system_flow.h"

#include <string.h>

static esp_err_t read_stage(mosaic_system_flow_t *flow,
                            mosaic_system_boot_stage_t *ret_stage)
{
    if (flow == NULL || ret_stage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (flow->ops.get_boot_stage != NULL) {
        return flow->ops.get_boot_stage(flow->ops.user_ctx, ret_stage);
    }
    *ret_stage = flow->volatile_stage;
    return ESP_OK;
}

static esp_err_t write_stage(mosaic_system_flow_t *flow,
                             mosaic_system_boot_stage_t stage)
{
    if (flow->ops.set_boot_stage != NULL) {
        esp_err_t err = flow->ops.set_boot_stage(flow->ops.user_ctx, stage);
        if (err != ESP_OK) {
            return err;
        }
    }
    flow->volatile_stage = stage;
    return ESP_OK;
}

esp_err_t mosaic_system_flow_init(mosaic_system_flow_t *flow,
                                  const mosaic_system_ops_t *ops)
{
    if (flow == NULL || (ops != NULL &&
            (ops->get_boot_stage == NULL || ops->set_boot_stage == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(flow, 0, sizeof(*flow));
    if (ops != NULL) {
        flow->ops = *ops;
    }
    flow->volatile_stage = MOSAIC_SYSTEM_BOOT_SETUP;
    return ESP_OK;
}

esp_err_t mosaic_system_flow_initial_app(mosaic_system_flow_t *flow,
                                         const char **ret_app_name)
{
    if (ret_app_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaic_system_boot_stage_t stage;
    esp_err_t err = read_stage(flow, &stage);
    if (err != ESP_OK) {
        return err;
    }
    switch (stage) {
    case MOSAIC_SYSTEM_BOOT_SETUP:
        *ret_app_name = "setup_center";
        return ESP_OK;
    case MOSAIC_SYSTEM_BOOT_WELCOME:
        *ret_app_name = "welcome";
        return ESP_OK;
    case MOSAIC_SYSTEM_BOOT_HOME:
        *ret_app_name = "mosaic-hub";
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t mosaic_system_flow_handle_exit(mosaic_system_flow_t *flow,
                                         const char *app_name,
                                         bool *ret_handled,
                                         const char **ret_next_app_name)
{
    if (flow == NULL || app_name == NULL || ret_handled == NULL ||
            ret_next_app_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_handled = false;
    *ret_next_app_name = NULL;

    mosaic_system_boot_stage_t current;
    esp_err_t err = read_stage(flow, &current);
    if (err != ESP_OK) {
        return err;
    }
    if (current == MOSAIC_SYSTEM_BOOT_SETUP &&
            strcmp(app_name, "setup_center") == 0) {
        err = write_stage(flow, MOSAIC_SYSTEM_BOOT_WELCOME);
        *ret_next_app_name = "welcome";
    } else if (current == MOSAIC_SYSTEM_BOOT_WELCOME &&
               strcmp(app_name, "welcome") == 0) {
        err = write_stage(flow, MOSAIC_SYSTEM_BOOT_HOME);
        *ret_next_app_name = "mosaic-hub";
    } else {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        *ret_handled = true;
    }
    return err;
}
