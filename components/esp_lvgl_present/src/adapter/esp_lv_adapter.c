/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "esp_lv_adapter.h"
#include "adapter_internal.h"
#include "lvgl_present_display.h"

static const char *TAG = "esp_lvgl_present";
static esp_lv_adapter_context_t s_ctx;

static esp_err_t tick_init(void);
static void lvgl_worker(void *arg);
static esp_err_t adapter_stop_tick_timer(void);
static esp_err_t adapter_start_tick_timer(void);
static esp_err_t adapter_wait_for_all_flush_done(int32_t timeout_ms);
static esp_err_t adapter_unregister_all_inputs(void);

static esp_err_t adapter_wait_for_all_flush_done(int32_t timeout_ms)
{
    for (esp_lv_adapter_display_node_t *node = s_ctx.display_list; node; node = node->next) {
        if (!node->lv_disp) {
            continue;
        }
        esp_err_t ret = lvgl_present_display_wait_flush_done(node->lv_disp, timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t adapter_unregister_all_inputs(void)
{
    while (s_ctx.input_list) {
        lv_indev_t *indev = s_ctx.input_list->indev;
        esp_err_t ret = indev ? esp_lv_adapter_unregister_touch(indev) : ESP_OK;
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t adapter_stop_tick_timer(void)
{
    if (!s_ctx.tick_timer) {
        return ESP_OK;
    }

    esp_timer_handle_t timer = (esp_timer_handle_t)s_ctx.tick_timer;
    esp_err_t ret;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
    ret = esp_timer_stop_blocking(timer, portMAX_DELAY);
    if (ret == ESP_ERR_NOT_FINISHED) {
        ret = ESP_OK;
    }
#else
    ret = esp_timer_stop(timer);
#endif

    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return ret;
}

static esp_err_t adapter_start_tick_timer(void)
{
    if (!s_ctx.tick_timer) {
        return ESP_OK;
    }

    esp_err_t ret = esp_timer_start_periodic((esp_timer_handle_t)s_ctx.tick_timer,
                                             s_ctx.config.tick_period_ms * 1000);
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return ret;
}

esp_err_t esp_lv_adapter_init(const esp_lv_adapter_config_t *config)
{
    ESP_RETURN_ON_FALSE(!s_ctx.inited, ESP_ERR_INVALID_STATE, TAG, "adapter already initialized");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "invalid adapter configuration");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.config = *config;

    lv_init();

    esp_err_t ret = tick_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "tick init failed (%d)", ret);

    s_ctx.lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lvgl_mutex, ESP_ERR_NO_MEM, TAG, "failed to create LVGL mutex");

    s_ctx.pause_done_sem = xSemaphoreCreateBinary();
    if (!s_ctx.pause_done_sem) {
        vSemaphoreDelete(s_ctx.lvgl_mutex);
        s_ctx.lvgl_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ctx.inited = true;
    ESP_LOGI(TAG, "LVGL adapter initialized");
    return ESP_OK;
}

esp_err_t esp_lv_adapter_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.inited, ESP_ERR_INVALID_STATE, TAG, "adapter not initialized");
    if (s_ctx.task) {
        return ESP_OK;
    }

    const BaseType_t core = (s_ctx.config.task_core_id < 0) ? tskNO_AFFINITY : s_ctx.config.task_core_id;
    const uint32_t stack_size = s_ctx.config.task_stack_size ?
                                s_ctx.config.task_stack_size : ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    const UBaseType_t task_priority = s_ctx.config.task_priority ?
                                      s_ctx.config.task_priority : ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY;
    uint32_t caps = s_ctx.config.stack_in_psram
                    ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                    : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
                              lvgl_worker, "lvgl", stack_size, NULL, task_priority,
                              &s_ctx.task, core, caps);

    if (task_ret != pdPASS && s_ctx.config.stack_in_psram) {
        ESP_LOGW(TAG, "LVGL task PSRAM allocation failed, retrying with internal memory");
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        task_ret = xTaskCreatePinnedToCoreWithCaps(
                       lvgl_worker, "lvgl", stack_size, NULL, task_priority,
                       &s_ctx.task, core, caps);
    }

    ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create LVGL task");
    s_ctx.task_exit_requested = false;
    s_ctx.paused = false;
    s_ctx.pause_ack = false;
    ESP_LOGI(TAG, "LVGL task started");
    return ESP_OK;
}

esp_err_t esp_lv_adapter_lock(int32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ctx.lvgl_mutex, ESP_ERR_INVALID_STATE, TAG, "LVGL mutex not initialized");

    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    ESP_RETURN_ON_FALSE(xSemaphoreTakeRecursive(s_ctx.lvgl_mutex, ticks) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "failed to acquire LVGL lock");
    return ESP_OK;
}

void esp_lv_adapter_unlock(void)
{
    if (s_ctx.lvgl_mutex) {
        xSemaphoreGiveRecursive(s_ctx.lvgl_mutex);
    }
}

esp_err_t esp_lv_adapter_pause(int32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ctx.inited, ESP_ERR_INVALID_STATE, TAG, "adapter not initialized");
    if (s_ctx.paused) {
        return ESP_OK;
    }

    const bool called_from_worker = (s_ctx.task && xTaskGetCurrentTaskHandle() == s_ctx.task);
    s_ctx.pause_ack = false;
    s_ctx.paused = true;

    if (called_from_worker || !s_ctx.task || !s_ctx.pause_done_sem) {
        s_ctx.pause_ack = true;
        if (s_ctx.pause_done_sem) {
            xSemaphoreGive(s_ctx.pause_done_sem);
        }
        (void)adapter_stop_tick_timer();
        return ESP_OK;
    }

    xSemaphoreTake(s_ctx.pause_done_sem, 0);
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_ctx.pause_done_sem, ticks) != pdTRUE) {
        s_ctx.paused = false;
        return ESP_ERR_TIMEOUT;
    }

    (void)adapter_stop_tick_timer();
    return ESP_OK;
}

esp_err_t esp_lv_adapter_resume(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.inited, ESP_ERR_INVALID_STATE, TAG, "adapter not initialized");
    if (!s_ctx.paused) {
        return ESP_OK;
    }

    (void)adapter_start_tick_timer();
    s_ctx.paused = false;
    s_ctx.pause_ack = false;
    if (s_ctx.task) {
        xTaskNotifyGive(s_ctx.task);
    }
    return ESP_OK;
}

lv_display_t *esp_lv_adapter_register_display_with_presenter(
    const esp_lv_adapter_display_config_t *config,
    const esp_lv_adapter_presenter_config_t *presenter_config)
{
    if (!s_ctx.inited || !config || !presenter_config || !presenter_config->presenter) {
        return NULL;
    }
    return lvgl_present_display_register(config, presenter_config);
}

esp_err_t esp_lv_adapter_unregister_display(lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(s_ctx.inited, ESP_ERR_INVALID_STATE, TAG, "adapter not initialized");
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_ARG, TAG, "invalid display handle");

    const bool pause_requested = !s_ctx.paused;
    if (pause_requested) {
        ESP_RETURN_ON_ERROR(esp_lv_adapter_pause(-1), TAG, "failed to pause adapter");
    }

    esp_err_t ret = lvgl_present_display_wait_flush_done(disp, 5000);
    if (ret != ESP_OK) {
        if (pause_requested) {
            esp_lv_adapter_resume();
        }
        return ret;
    }

    ret = esp_lv_adapter_lock(-1);
    if (ret != ESP_OK) {
        if (pause_requested) {
            esp_lv_adapter_resume();
        }
        return ret;
    }

    ret = lvgl_present_display_unregister(disp);
    esp_lv_adapter_unlock();

    if (pause_requested) {
        esp_lv_adapter_resume();
    }
    return ret;
}

esp_err_t esp_lv_adapter_deinit(void)
{
    if (!s_ctx.inited) {
        return ESP_OK;
    }

    const bool pause_requested = !s_ctx.paused;
    if (pause_requested) {
        ESP_RETURN_ON_ERROR(esp_lv_adapter_pause(-1), TAG, "failed to pause adapter during deinit");
    }

    esp_err_t first_err = adapter_wait_for_all_flush_done(5000);
    if (first_err != ESP_OK) {
        ESP_LOGW(TAG, "flush wait during deinit: %s", esp_err_to_name(first_err));
    }

    esp_err_t ret = adapter_unregister_all_inputs();
    if (ret != ESP_OK && first_err == ESP_OK) {
        first_err = ret;
    }

    if (s_ctx.task) {
        TaskHandle_t task = s_ctx.task;
        s_ctx.task_exit_requested = true;
        s_ctx.paused = false;
        s_ctx.pause_ack = false;
        xTaskNotifyGive(task);

        uint32_t wait_count = 0;
        while (eTaskGetState(task) != eSuspended && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        vTaskDeleteWithCaps(task);
        s_ctx.task = NULL;
    }

    if (s_ctx.tick_timer) {
        esp_timer_handle_t timer = (esp_timer_handle_t)s_ctx.tick_timer;
        esp_timer_stop(timer);
        esp_timer_delete(timer);
        s_ctx.tick_timer = NULL;
    }

    lvgl_present_display_clear();

    if (s_ctx.lvgl_mutex) {
        vSemaphoreDelete(s_ctx.lvgl_mutex);
        s_ctx.lvgl_mutex = NULL;
    }
    if (s_ctx.pause_done_sem) {
        vSemaphoreDelete(s_ctx.pause_done_sem);
        s_ctx.pause_done_sem = NULL;
    }

    lv_deinit();
    memset(&s_ctx, 0, sizeof(s_ctx));
    return first_err;
}

esp_lv_adapter_context_t *esp_lv_adapter_get_context(void)
{
    return &s_ctx;
}

esp_err_t esp_lv_adapter_register_input_device(lv_indev_t *indev,
                                               esp_lv_adapter_input_type_t type,
                                               void *user_ctx)
{
    ESP_RETURN_ON_FALSE(indev, ESP_ERR_INVALID_ARG, TAG, "input device handle cannot be NULL");

    esp_lv_adapter_input_node_t *node = calloc(1, sizeof(*node));
    ESP_RETURN_ON_FALSE(node, ESP_ERR_NO_MEM, TAG, "failed to allocate input device node");

    node->indev = indev;
    node->type = type;
    node->user_ctx = user_ctx;
    node->next = s_ctx.input_list;
    s_ctx.input_list = node;
    return ESP_OK;
}

esp_err_t esp_lv_adapter_unregister_input_device(lv_indev_t *indev)
{
    ESP_RETURN_ON_FALSE(indev, ESP_ERR_INVALID_ARG, TAG, "input device handle cannot be NULL");

    esp_lv_adapter_input_node_t **cursor = &s_ctx.input_list;
    while (*cursor) {
        if ((*cursor)->indev == indev) {
            esp_lv_adapter_input_node_t *node = *cursor;
            *cursor = node->next;
            free(node);
            return ESP_OK;
        }
        cursor = &(*cursor)->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static void lvgl_worker(void *arg)
{
    (void)arg;
    uint32_t task_delay_ms = s_ctx.config.task_max_delay_ms;

    while (!s_ctx.task_exit_requested) {
        if (s_ctx.paused) {
            if (!s_ctx.pause_ack) {
                s_ctx.pause_ack = true;
                if (s_ctx.pause_done_sem) {
                    xSemaphoreGive(s_ctx.pause_done_sem);
                }
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        uint32_t next_delay_ms = s_ctx.config.task_max_delay_ms;
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            next_delay_ms = lv_timer_handler();
            esp_lv_adapter_unlock();
        }

        task_delay_ms = next_delay_ms;
        if (task_delay_ms > s_ctx.config.task_max_delay_ms) {
            task_delay_ms = s_ctx.config.task_max_delay_ms;
        } else if (task_delay_ms < s_ctx.config.task_min_delay_ms) {
            task_delay_ms = s_ctx.config.task_min_delay_ms;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(task_delay_ms));
    }

    s_ctx.paused = false;
    s_ctx.pause_ack = false;
    vTaskSuspend(NULL);
}

static void tick_increment(void *arg)
{
    lv_tick_inc((uint32_t)(uintptr_t)arg);
}

static esp_err_t tick_init(void)
{
    const uint32_t tick_period_ms = s_ctx.config.tick_period_ms;
    const esp_timer_create_args_t args = {
        .callback = tick_increment,
        .arg = (void *)(uintptr_t)tick_period_ms,
        .name = "LVGL tick",
    };

    esp_timer_handle_t timer = NULL;
    esp_err_t ret = esp_timer_create(&args, &timer);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to create tick timer");

    s_ctx.tick_timer = timer;
    ret = esp_timer_start_periodic(timer, tick_period_ms * 1000);
    if (ret != ESP_OK) {
        esp_timer_delete(timer);
        s_ctx.tick_timer = NULL;
    }
    return ret;
}
