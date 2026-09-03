/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_loader.h"

#include <stdio.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mosaic_gsp_backend.h"
#include "mosaic_runtime.h"
#include "mosaic_ui.h"

#define MOSAIC_LOADER_QUEUE_DEPTH 32U
#define MOSAIC_LOADER_STEP_MS 16U
#define MOSAIC_LOADER_TASK_STACK 12288U
#define MOSAIC_LOADER_TASK_PRIORITY 5U

typedef enum {
    MOSAIC_LOADER_COMMAND_UI_EVENT = 0,
    MOSAIC_LOADER_COMMAND_POINTER,
    MOSAIC_LOADER_COMMAND_REQUEST_APP,
    MOSAIC_LOADER_COMMAND_INVALIDATE_APP,
    MOSAIC_LOADER_COMMAND_BACK,
    MOSAIC_LOADER_COMMAND_SYSTEM_NOTICE,
} mosaic_loader_command_type_t;

typedef struct {
    mosaic_loader_command_type_t type;
    union {
        struct {
            uint32_t generation;
            esp_gsp_event_t event;
        } ui;
        struct {
            uint32_t generation;
            int32_t x;
            int32_t y;
            bool pressed;
        } pointer;
        const mosaic_app_descriptor_t* app;
        struct {
            uint16_t app_id;
            uint32_t revision;
        } invalidate;
        struct {
            mosaic_system_notice_t notice;
            uint32_t duration_ms;
        } system_notice;
    } data;
} mosaic_loader_command_t;

static mosaic_loader_config_t s_config;
static mosaic_gsp_backend_handle_t s_backend;
static mosaic_runtime_handle_t s_runtime;
static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_runtime_lock;
static TaskHandle_t s_loader_task;
static mosaic_loader_deferred_cb_t s_deferred_callback;
static void* s_deferred_user_ctx;
static bool s_runtime_started;
static bool s_hub_foreground;
static bool s_quiesced;
static uint16_t s_hub_scene;

static bool post_ui_event(
    void* user_ctx, uint32_t generation, const esp_gsp_event_t* event)
{
    (void)user_ctx;
    if (s_command_queue == NULL || event == NULL) {
        return false;
    }
    const mosaic_loader_command_t command = {
        .type = MOSAIC_LOADER_COMMAND_UI_EVENT,
        .data.ui = {
            .generation = generation,
            .event = *event,
        },
    };
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE;
}

static bool post_pointer(void* user_ctx, uint32_t generation, int32_t x,
    int32_t y, bool pressed)
{
    (void)user_ctx;
    if (s_command_queue == NULL) {
        return false;
    }
    const mosaic_loader_command_t command = {
        .type = MOSAIC_LOADER_COMMAND_POINTER,
        .data.pointer = {
            .generation = generation,
            .x = x,
            .y = y,
            .pressed = pressed,
        },
    };
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE;
}

static void post_app_request(void* user_ctx, const mosaic_app_descriptor_t* app)
{
    (void)user_ctx;
    (void)mosaic_loader_request(app);
}

static void process_ui_event(const mosaic_loader_command_t* command)
{
    const mosaic_app_descriptor_t* active
        = mosaic_runtime_active_app(s_runtime);
    if (!mosaic_gsp_backend_deliver_event(
            s_backend, command->data.ui.generation, &command->data.ui.event)) {
        return;
    }
    if (active == mosaic_app_root()
        && command->data.ui.event.type == ESP_GSP_EVENT_SCENE_CHANGED) {
        s_hub_scene = command->data.ui.event.scene_id;
    }
    if (s_config.on_event != NULL) {
        s_config.on_event(mosaic_gsp_backend_ui(s_backend), active,
            &command->data.ui.event, s_config.on_event_ctx);
    }
}

static void process_command(const mosaic_loader_command_t* command)
{
    if (!s_runtime_started) {
        return;
    }
    if (command->type == MOSAIC_LOADER_COMMAND_UI_EVENT) {
        process_ui_event(command);
        return;
    }
    if (command->type == MOSAIC_LOADER_COMMAND_POINTER) {
        if (mosaic_ui_absorb_wake_pointer(command->data.pointer.pressed)) {
            return;
        }
        (void)mosaic_gsp_backend_deliver_pointer(s_backend, s_runtime,
            command->data.pointer.generation, command->data.pointer.x,
            command->data.pointer.y, command->data.pointer.pressed);
        return;
    }
    if (command->type == MOSAIC_LOADER_COMMAND_REQUEST_APP
        && command->data.app != NULL) {
        esp_err_t err
            = mosaic_runtime_request_app(s_runtime, command->data.app->name);
        if (err != ESP_OK) {
            printf("mosaic_loader: request %s failed: %s\n",
                command->data.app->name, esp_err_to_name(err));
        }
        return;
    }
    if (command->type == MOSAIC_LOADER_COMMAND_INVALIDATE_APP) {
        (void)mosaic_runtime_notify_model_changed(s_runtime,
            command->data.invalidate.app_id,
            command->data.invalidate.revision);
        return;
    }
    if (command->type == MOSAIC_LOADER_COMMAND_BACK) {
        const mosaic_app_descriptor_t* active =
            mosaic_runtime_active_app(s_runtime);
        esp_gsp_handle_t ui = mosaic_gsp_backend_ui(s_backend);
        if (active == NULL || ui == NULL) {
            return;
        }
        if (active->on_back != NULL &&
                active->on_back(ui, esp_timer_get_time())) {
            return;
        }
        uint16_t page = 0;
        if (!active->back_exits_app && active->root_stack_key != 0 &&
                esp_gsp_stack_view_get_top(
                    ui, active->root_stack_key, &page) == ESP_GSP_OK &&
                page != 0) {
            (void)esp_gsp_stack_view_pop(
                ui, active->root_stack_key, true);
            return;
        }
        if (active == mosaic_app_root()) {
            if (active->on_root_back != NULL) {
                (void)active->on_root_back(ui);
            }
            return;
        }
        if (mosaic_runtime_back(s_runtime) != ESP_OK) {
            (void)mosaic_runtime_request_app(
                s_runtime, mosaic_app_root()->name);
        }
    }
    if (command->type == MOSAIC_LOADER_COMMAND_SYSTEM_NOTICE) {
        esp_gsp_handle_t ui = mosaic_gsp_backend_ui(s_backend);
        (void)mosaic_app_shell_show_system_notice(
            ui, command->data.system_notice.notice,
            command->data.system_notice.duration_ms);
    }
}

static void process_deferred(void)
{
    if (s_deferred_callback == NULL || mosaic_loader_poll()) {
        return;
    }
    mosaic_loader_deferred_cb_t callback = s_deferred_callback;
    void* user_ctx = s_deferred_user_ctx;

    s_deferred_callback = NULL;
    s_deferred_user_ctx = NULL;
    if (callback != NULL) {
        callback(user_ctx);
    }
}

static void loader_task(void* arg)
{
    (void)arg;
    for (;;) {
        mosaic_loader_command_t command;
        const BaseType_t received = xQueueReceive(
            s_command_queue, &command, pdMS_TO_TICKS(MOSAIC_LOADER_STEP_MS));
        xSemaphoreTake(s_runtime_lock, portMAX_DELAY);
        if (s_quiesced) {
            if (received == pdTRUE) {
                (void)xQueueSendToFront(s_command_queue, &command, 0);
            }
            xSemaphoreGive(s_runtime_lock);
            vTaskDelay(pdMS_TO_TICKS(MOSAIC_LOADER_STEP_MS));
            continue;
        }
        if (received == pdTRUE) {
            process_command(&command);
        }
        if (s_runtime_started) {
            esp_err_t err
                = mosaic_runtime_step(s_runtime, esp_timer_get_time());
            if (err != ESP_OK) {
                printf("mosaic_loader: runtime step failed: %s\n",
                    esp_err_to_name(err));
            }
            const bool hub_foreground =
                mosaic_runtime_active_app(s_runtime) == mosaic_app_root();
            if (hub_foreground != s_hub_foreground) {
                s_hub_foreground = hub_foreground;
                mosaic_ui_set_hub_foreground(hub_foreground);
            }
        }
        xSemaphoreGive(s_runtime_lock);
        process_deferred();
    }
}

static void cleanup_partial_init(void)
{
    s_deferred_callback = NULL;
    s_deferred_user_ctx = NULL;
    if (s_runtime != NULL) {
        mosaic_runtime_delete(s_runtime);
        s_runtime = NULL;
    }
    if (s_backend != NULL) {
        mosaic_gsp_backend_delete(s_backend);
        s_backend = NULL;
    }
    if (s_runtime_lock != NULL) {
        vSemaphoreDelete(s_runtime_lock);
        s_runtime_lock = NULL;
    }
    if (s_command_queue != NULL) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
    }
}

esp_err_t mosaic_loader_init(const mosaic_loader_config_t* config)
{
    if (config == NULL || config->presenter == NULL
        || config->producer_generation == 0 || s_runtime != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t catalog_err = mosaic_app_catalog_load_dynamic();
    if (catalog_err != ESP_OK) {
        return catalog_err;
    }
    if (!mosaic_app_catalog_validate()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_config = *config;
    s_command_queue = xQueueCreate(
        MOSAIC_LOADER_QUEUE_DEPTH, sizeof(mosaic_loader_command_t));
    s_runtime_lock = xSemaphoreCreateMutex();
    if (s_command_queue == NULL || s_runtime_lock == NULL) {
        cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }
    const mosaic_gsp_backend_config_t backend_config = {
        .presenter = config->presenter,
        .render_alignment = config->render_alignment,
        .touch = config->touch,
        .post_event = post_ui_event,
        .post_pointer = post_pointer,
        .request_app = post_app_request,
    };
    esp_err_t err = mosaic_gsp_backend_create(&backend_config, &s_backend);
    if (err != ESP_OK) {
        cleanup_partial_init();
        return err;
    }
    const mosaic_runtime_config_t runtime_config = {
        .platform = mosaic_gsp_backend_ops(),
        .platform_ctx = s_backend,
    };
    err = mosaic_runtime_create(&runtime_config, &s_runtime);
    if (err != ESP_OK) {
        cleanup_partial_init();
        return err;
    }
    BaseType_t task_ok
        = xTaskCreate(loader_task, "mosaic_runtime", MOSAIC_LOADER_TASK_STACK,
            NULL, MOSAIC_LOADER_TASK_PRIORITY, &s_loader_task);
    if (task_ok != pdPASS) {
        cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mosaic_loader_start_hub(void)
{
    if (s_runtime == NULL || s_runtime_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_runtime_lock, portMAX_DELAY);
    /* Child apps run on top of the hub session. The system lifecycle owner
     * decides whether Setup, Welcome, or no child App is opened next. */
    esp_err_t err = mosaic_runtime_start(s_runtime, mosaic_app_root()->name);
    if (err == ESP_OK) {
        s_runtime_started = true;
        err = mosaic_runtime_step(s_runtime, esp_timer_get_time());
        if (err == ESP_OK) {
            s_hub_foreground = true;
            mosaic_ui_set_hub_foreground(true);
        }
    }
    xSemaphoreGive(s_runtime_lock);
    return err;
}

esp_err_t mosaic_loader_request(const mosaic_app_descriptor_t* app)
{
    if (s_command_queue == NULL || app == NULL) {
        return app == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    const mosaic_loader_command_t command = {
        .type = MOSAIC_LOADER_COMMAND_REQUEST_APP,
        .data.app = app,
    };
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        printf("mosaic_loader: request queue full for %s\n", app->name);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t mosaic_loader_request_back(void)
{
    if (s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const mosaic_loader_command_t command = {
        .type = MOSAIC_LOADER_COMMAND_BACK,
    };
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t mosaic_loader_invalidate_app(uint16_t app_id, uint32_t revision)
{
    if (s_command_queue == NULL || revision == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const mosaic_loader_command_t command = {
        .type = MOSAIC_LOADER_COMMAND_INVALIDATE_APP,
        .data.invalidate = {
            .app_id = app_id,
            .revision = revision,
        },
    };
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t mosaic_loader_show_system_notice(
    mosaic_system_notice_t notice, uint32_t duration_ms)
{
    if (s_command_queue == NULL ||
            notice > MOSAIC_SYSTEM_NOTICE_BATTERY_CRITICAL) {
        return ESP_ERR_INVALID_STATE;
    }
    const mosaic_loader_command_t command = {
        .type = MOSAIC_LOADER_COMMAND_SYSTEM_NOTICE,
        .data.system_notice = {
            .notice = notice,
            .duration_ms = duration_ms,
        },
    };
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t mosaic_loader_defer(
    mosaic_loader_deferred_cb_t callback, void* user_ctx)
{
    if (callback == NULL || s_loader_task == NULL
        || xTaskGetCurrentTaskHandle() != s_loader_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_deferred_callback != NULL) {
        return ESP_ERR_TIMEOUT;
    }
    s_deferred_callback = callback;
    s_deferred_user_ctx = user_ctx;
    return ESP_OK;
}

bool mosaic_loader_poll(void)
{
    return s_command_queue != NULL
        && uxQueueMessagesWaiting(s_command_queue) != 0;
}

const mosaic_app_descriptor_t* mosaic_loader_app(void)
{
    return s_runtime != NULL ? mosaic_runtime_active_app(s_runtime)
                             : mosaic_app_root();
}

esp_gsp_handle_t mosaic_loader_ui(void)
{
    return mosaic_gsp_backend_ui(s_backend);
}

esp_err_t mosaic_loader_quiesce(uint32_t timeout_ms)
{
    if (timeout_ms == 0 || s_runtime_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A deferred callback runs on the loader task after the runtime lock is
     * released and may initiate a presenter handoff (for example, Settings
     * rotation). Reject the loader task only when it still owns the mutex;
     * taking it again in that state would self-deadlock. */
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    if (current_task == s_loader_task &&
            xSemaphoreGetMutexHolder(s_runtime_lock) == current_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_runtime_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int64_t start_us = esp_timer_get_time();
    esp_err_t err = mosaic_gsp_backend_quiesce(s_backend, timeout_ms);
    if (err == ESP_OK) {
        s_quiesced = true;
    }
    xSemaphoreGive(s_runtime_lock);
    printf("mosaic_loader: presenter quiesce %lld ms: %s\n",
        (long long)((esp_timer_get_time() - start_us) / 1000),
        esp_err_to_name(err));
    return err;
}

esp_err_t mosaic_loader_activate(
    struct esp_display_presenter* presenter, uint32_t producer_generation)
{
    if (presenter == NULL || producer_generation == 0
        || s_runtime_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime_lock, portMAX_DELAY);
    esp_err_t err = mosaic_gsp_backend_activate(s_backend, presenter);
    if (err == ESP_OK) {
        s_config.producer_generation = producer_generation;
        s_quiesced = false;
    }
    xSemaphoreGive(s_runtime_lock);
    return err;
}

esp_err_t mosaic_loader_lock_and_pause_hub(uint32_t timeout_ms)
{
    if (timeout_ms == 0 || s_runtime_lock == NULL || !s_runtime_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_runtime_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const mosaic_app_descriptor_t* root = mosaic_app_root();
    esp_err_t err = mosaic_runtime_active_app(s_runtime) == root
        ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        esp_gsp_handle_t ui = mosaic_gsp_backend_ui(s_backend);
        if (root->on_idle_lock != NULL) {
            root->on_idle_lock(ui);
        }
        /* The panel retains its last GRAM contents while it is off. Fence the
         * asynchronous Lock Screen setters so Display On cannot expose the
         * previous Hub/Home frame. */
        const esp_gsp_err_t flush_err = esp_gsp_flush(ui, timeout_ms);
        err = flush_err == ESP_GSP_OK ? ESP_OK : (esp_err_t)flush_err;
        if (err == ESP_OK) {
            err = mosaic_gsp_backend_pause_screen(s_backend, timeout_ms);
        }
    }
    xSemaphoreGive(s_runtime_lock);
    return err;
}

esp_err_t mosaic_loader_resume_screen(void)
{
    if (s_runtime_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_runtime_lock, portMAX_DELAY);
    esp_err_t err = mosaic_gsp_backend_resume_screen(s_backend);
    xSemaphoreGive(s_runtime_lock);
    return err;
}

uint16_t mosaic_loader_hub_scene(void) { return s_hub_scene; }
