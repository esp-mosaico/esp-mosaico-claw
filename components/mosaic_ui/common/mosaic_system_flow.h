/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "mosaic_system.h"

typedef struct {
    mosaic_system_ops_t ops;
    mosaic_system_boot_stage_t volatile_stage;
} mosaic_system_flow_t;

esp_err_t mosaic_system_flow_init(mosaic_system_flow_t *flow,
                                  const mosaic_system_ops_t *ops);
esp_err_t mosaic_system_flow_initial_app(mosaic_system_flow_t *flow,
                                         const char **ret_app_name);
esp_err_t mosaic_system_flow_handle_exit(mosaic_system_flow_t *flow,
                                         const char *app_name,
                                         bool *ret_handled,
                                         const char **ret_next_app_name);
