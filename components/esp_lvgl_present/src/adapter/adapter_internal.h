/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_display_present.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LVGL_PRESENT_MAX_AREAS 32
#define ESP_LVGL_PRESENT_BUF_ALIGN 128

typedef struct {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
} lvgl_present_area_t;

typedef struct {
    void *pixels;
    size_t capacity_bytes;
    size_t stride_bytes;
    uint16_t width;
    uint16_t height;
    int32_t x;
    int32_t y;
} lvgl_present_region_t;

typedef struct {
    esp_lv_adapter_display_config_t base;
    void *draw_buf_primary;
    lv_display_t *lv_disp;
    struct esp_display_presenter *presenter;
    uint32_t producer_generation;
    esp_display_present_render_alignment_t render_alignment;
    bool (*validate_generation)(void *user_ctx, uint32_t generation);
    void *validation_user_ctx;
} esp_lv_adapter_display_runtime_config_t;

typedef struct esp_lv_adapter_display_node {
    esp_lv_adapter_display_runtime_config_t cfg;
    lv_display_t *lv_disp;
    struct esp_lv_adapter_display_node *next;

    esp_display_presenter_caps_t caps;
    esp_display_presenter_buffer_t acquired;
    bool producer_bound;
    uint8_t bytes_per_pixel;
    size_t area_capacity;
    bool frame_active;
    bool buffer_held;
    bool fault;
    lvgl_present_area_t full_area;
    lvgl_present_region_t leased_buffer;
    lvgl_present_area_t damage[LVGL_PRESENT_MAX_AREAS];
    size_t damage_count;
    lvgl_present_area_t coverage[LVGL_PRESENT_MAX_AREAS];
    size_t coverage_count;
    lv_draw_buf_t *redirect_draw_buf;
    uint8_t *redirect_fallback_data;
    uint32_t redirect_fallback_size;
    uint32_t redirect_fallback_height;
} esp_lv_adapter_display_node_t;

typedef enum {
    ESP_LV_ADAPTER_INPUT_TYPE_TOUCH,
} esp_lv_adapter_input_type_t;

typedef struct esp_lv_adapter_input_node {
    lv_indev_t *indev;
    esp_lv_adapter_input_type_t type;
    void *user_ctx;
    struct esp_lv_adapter_input_node *next;
} esp_lv_adapter_input_node_t;

typedef struct {
    bool inited;
    volatile bool task_exit_requested;
    volatile bool paused;
    volatile bool pause_ack;
    SemaphoreHandle_t lvgl_mutex;
    SemaphoreHandle_t pause_done_sem;
    TaskHandle_t task;
    void *tick_timer;
    esp_lv_adapter_config_t config;
    esp_lv_adapter_display_node_t *display_list;
    esp_lv_adapter_input_node_t *input_list;
} esp_lv_adapter_context_t;

esp_lv_adapter_context_t *esp_lv_adapter_get_context(void);

esp_err_t esp_lv_adapter_register_input_device(lv_indev_t *indev,
                                               esp_lv_adapter_input_type_t type,
                                               void *user_ctx);
esp_err_t esp_lv_adapter_unregister_input_device(lv_indev_t *indev);

#ifdef __cplusplus
}
#endif
