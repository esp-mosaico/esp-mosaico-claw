/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lv_adapter_display.h"
#include "esp_lv_adapter_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t task_stack_size;
    uint32_t task_priority;
    int task_core_id;
    uint32_t tick_period_ms;
    uint32_t task_min_delay_ms;
    uint32_t task_max_delay_ms;
    bool stack_in_psram;
} esp_lv_adapter_config_t;

#define ESP_LV_ADAPTER_DEFAULT_STACK_SIZE        (8 * 1024)
#define ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY     6
#define ESP_LV_ADAPTER_DEFAULT_TASK_CORE_ID      (-1)
#define ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS    1
#define ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS 1
#define ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS 15

#define ESP_LV_ADAPTER_DEFAULT_CONFIG() {                            \
    .task_stack_size   = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE,          \
    .task_priority     = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY,       \
    .task_core_id      = ESP_LV_ADAPTER_DEFAULT_TASK_CORE_ID,        \
    .tick_period_ms    = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS,      \
    .task_min_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS,   \
    .task_max_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS,   \
    .stack_in_psram    = false,                                      \
}

esp_err_t esp_lv_adapter_init(const esp_lv_adapter_config_t *config);
esp_err_t esp_lv_adapter_start(void);
esp_err_t esp_lv_adapter_deinit(void);

esp_err_t esp_lv_adapter_lock(int32_t timeout_ms);
void esp_lv_adapter_unlock(void);

esp_err_t esp_lv_adapter_pause(int32_t timeout_ms);
esp_err_t esp_lv_adapter_resume(void);

#ifdef __cplusplus
}
#endif
