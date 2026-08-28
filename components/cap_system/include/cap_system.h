/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*cap_system_network_ready_fn)(void *ctx);
typedef void (*cap_system_sync_success_fn)(bool had_valid_time, void *ctx);

typedef struct {
    cap_system_network_ready_fn network_ready;
    void *network_ready_ctx;
    cap_system_sync_success_fn on_sync_success;
    void *on_sync_success_ctx;
    uint32_t disconnected_retry_ms;
    uint32_t sync_retry_ms;
} cap_system_time_sync_service_config_t;

/**
 * Application/board adapter for operations which do not belong in the reusable
 * cap_system component. Both callbacks return a complete JSON value in output.
 */
typedef esp_err_t (*cap_system_platform_get_info_fn)(const char *section,
                                                     char *output,
                                                     size_t output_size,
                                                     void *user_ctx);
typedef esp_err_t (*cap_system_platform_execute_fn)(const char *operation,
                                                    const char *input_json,
                                                    char *output,
                                                    size_t output_size,
                                                    void *user_ctx);

typedef struct {
    cap_system_platform_get_info_fn get_info;
    cap_system_platform_execute_fn execute;
    void *user_ctx;
} cap_system_platform_provider_t;

esp_err_t cap_system_register_group(void);
esp_err_t cap_system_time_sync_service_start(const cap_system_time_sync_service_config_t *config);
esp_err_t cap_system_set_platform_provider(const cap_system_platform_provider_t *provider);

#ifdef __cplusplus
}
#endif
