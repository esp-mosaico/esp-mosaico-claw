/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mosaic_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mosaic_logic_t* mosaic_logic_handle_t;
typedef void* mosaic_logic_instance_t;

typedef void (*mosaic_logic_log_fn_t)(
    void* user_ctx, const char* app_name, const char* message);

/** Inputs owned by the platform App session and valid until logic deletion. */
typedef struct {
    esp_gsp_handle_t ui;
    const mosaic_app_package_t* package;
    const void* program;
    size_t program_size;
    mosaic_logic_log_fn_t log;
    void* log_ctx;
} mosaic_logic_config_t;

/** Language-neutral App logic lifecycle.
 *
 * Implementations run synchronously on the one Mosaic runtime owner. They
 * must not create another event queue or take ownership of the UI callback.
 */
typedef struct mosaic_logic_ops {
    esp_err_t (*create)(const mosaic_logic_config_t* config,
        mosaic_logic_instance_t* ret_instance);
    void (*destroy)(mosaic_logic_instance_t instance);
    esp_err_t (*dispatch)(
        mosaic_logic_instance_t instance, const mosaic_event_t* event);
    esp_err_t (*step)(mosaic_logic_instance_t instance, int64_t now_us);
} mosaic_logic_ops_t;

extern const mosaic_logic_ops_t mosaic_native_logic_ops;
extern const mosaic_logic_ops_t mosaic_lua_logic_ops;

esp_err_t mosaic_logic_create(
    const mosaic_logic_config_t* config, mosaic_logic_handle_t* ret_logic);
void mosaic_logic_delete(mosaic_logic_handle_t logic);
esp_err_t mosaic_logic_dispatch(
    mosaic_logic_handle_t logic, const mosaic_event_t* event);
esp_err_t mosaic_logic_step(mosaic_logic_handle_t logic, int64_t now_us);

#ifdef __cplusplus
}
#endif
