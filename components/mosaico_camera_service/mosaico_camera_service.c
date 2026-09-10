/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_camera_service.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mosaico_module_mgr.h"

static const char *TAG = "camera_service";

typedef struct {
    SemaphoreHandle_t lock;
    mosaico_camera_handle_t camera;
    mosaico_camera_handle_t pending_cleanup;
    mosaico_camera_availability_callback_t callback;
    void *callback_ctx;
    bool initialized;
    bool activating;
} mosaico_camera_service_state_t;

static mosaico_camera_service_state_t s_service;

static void notify_availability(char slot, bool available)
{
    mosaico_camera_availability_callback_t callback;
    void *callback_ctx;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    callback = s_service.initialized ? s_service.callback : NULL;
    callback_ctx = s_service.initialized ? s_service.callback_ctx : NULL;
    xSemaphoreGive(s_service.lock);
    if (callback) {
        callback(slot, available, callback_ctx);
    }
}

static esp_err_t retry_pending_cleanup(void)
{
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    mosaico_camera_handle_t camera = s_service.pending_cleanup;
    xSemaphoreGive(s_service.lock);
    if (!camera) {
        return ESP_OK;
    }

    const esp_err_t ret = mosaico_camera_del(camera);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Retry camera cleanup failed: %s", esp_err_to_name(ret));
        return ret;
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.pending_cleanup == camera) {
        s_service.pending_cleanup = NULL;
    }
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}

static esp_err_t activate_camera(const mosaico_module_mgr_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "module info is null");
    ESP_RETURN_ON_FALSE(info->slot == MOSAICO_MODULE_MGR_SLOT_LEFT, ESP_ERR_NOT_SUPPORTED, TAG,
                        "camera module is only supported in the left slot");

    for (;;) {
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        if (!s_service.initialized || s_service.camera) {
            const esp_err_t ret = s_service.initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
            xSemaphoreGive(s_service.lock);
            return ret;
        }
        if (!s_service.activating) {
            s_service.activating = true;
            xSemaphoreGive(s_service.lock);
            break;
        }
        xSemaphoreGive(s_service.lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.slot = info->slot;
    config.width = 1280;
    config.height = 720;
    config.buffer_count = 1;
    mosaico_camera_handle_t camera = NULL;
    esp_err_t ret = mosaico_camera_new(&config, &camera);

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (ret == ESP_OK && s_service.initialized) {
        s_service.camera = camera;
    } else if (camera) {
        s_service.pending_cleanup = camera;
        if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    xSemaphoreGive(s_service.lock);
    if (ret != ESP_OK) {
        (void)retry_pending_cleanup();
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        s_service.activating = false;
        xSemaphoreGive(s_service.lock);
        ESP_LOGE(TAG, "Create default camera failed: %s", esp_err_to_name(ret));
        return ret;
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.activating = false;
    xSemaphoreGive(s_service.lock);

    ESP_LOGI(TAG, "Camera ready: slot=left path=/dev/video2");
    notify_availability('L', true);
    return ESP_OK;
}

static esp_err_t activate_ready_camera(void)
{
    mosaico_module_mgr_info_t info = {0};
    const esp_err_t ret = mosaico_module_mgr_find(MOSAICO_BOARD_TYPE_CAMERA, MOSAICO_MODULE_MGR_SLOT_LEFT, &info);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "find ready camera failed");
    if (info.state != MOSAICO_MODULE_MGR_STATE_READY) {
        return ESP_OK;
    }
    return activate_camera(&info);
}

static void module_event(mosaico_module_mgr_event_t event, const mosaico_module_mgr_info_t *info, void *user_data)
{
    (void)user_data;
    if (!info || info->eeprom.board_type != MOSAICO_BOARD_TYPE_CAMERA) {
        return;
    }
    if (event == MOSAICO_MODULE_MGR_EVENT_INSERTED) {
        esp_err_t ret = activate_camera(info);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Activate camera failed: %s", esp_err_to_name(ret));
        }
    } else if (event == MOSAICO_MODULE_MGR_EVENT_REMOVED) {
        notify_availability(info->slot == MOSAICO_MODULE_MGR_SLOT_RIGHT ? 'R' : 'L', false);
    }
}

esp_err_t mosaico_camera_service_init(void)
{
    if (!s_service.lock) {
        /* Keep the singleton lock alive for manager callbacks already in flight. */
        s_service.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_service.lock, ESP_ERR_NO_MEM, TAG, "create service mutex failed");
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.initialized) {
        xSemaphoreGive(s_service.lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_service.lock);
    ESP_RETURN_ON_ERROR(retry_pending_cleanup(), TAG, "clean up previous camera failed");
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.initialized) {
        xSemaphoreGive(s_service.lock);
        return ESP_OK;
    }
    s_service.initialized = true;
    xSemaphoreGive(s_service.lock);

    esp_err_t ret = mosaico_module_mgr_subscribe(module_event, NULL);
    if (ret == ESP_OK) {
        ret = mosaico_module_mgr_init(NULL);
    }
    if (ret == ESP_OK) {
        ret = activate_ready_camera();
    }
    if (ret != ESP_OK) {
        (void)mosaico_module_mgr_unsubscribe(module_event);
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        s_service.initialized = false;
        xSemaphoreGive(s_service.lock);
        ESP_LOGE(TAG, "Initialize camera service failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mosaico_camera_service_deinit(void)
{
    if (!s_service.lock) {
        return ESP_OK;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (!s_service.initialized) {
        xSemaphoreGive(s_service.lock);
        return retry_pending_cleanup();
    }
    s_service.initialized = false;
    mosaico_camera_handle_t camera = s_service.camera;
    xSemaphoreGive(s_service.lock);
    (void)mosaico_module_mgr_unsubscribe(module_event);

    if (camera) {
        esp_err_t ret = mosaico_camera_del(camera);
        if (ret != ESP_OK) {
            xSemaphoreTake(s_service.lock, portMAX_DELAY);
            s_service.initialized = true;
            xSemaphoreGive(s_service.lock);
            (void)mosaico_module_mgr_subscribe(module_event, NULL);
            ESP_LOGE(TAG, "Delete default camera failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.camera = NULL;
    xSemaphoreGive(s_service.lock);
    return retry_pending_cleanup();
}

esp_err_t mosaico_camera_service_get_default(mosaico_camera_handle_t *out_camera)
{
    ESP_RETURN_ON_FALSE(out_camera, ESP_ERR_INVALID_ARG, TAG, "camera output is null");
    *out_camera = NULL;
    ESP_RETURN_ON_FALSE(s_service.lock, ESP_ERR_INVALID_STATE, TAG, "camera service is not initialized");

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    const bool initialized = s_service.initialized;
    if (initialized) {
        *out_camera = s_service.camera;
    }
    xSemaphoreGive(s_service.lock);
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG, "camera service is not initialized");
    return *out_camera ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool mosaico_camera_service_is_available(void)
{
    mosaico_camera_handle_t camera = NULL;
    return mosaico_camera_service_get_default(&camera) == ESP_OK;
}

void mosaico_camera_service_set_callback(mosaico_camera_availability_callback_t callback, void *user_ctx)
{
    if (!s_service.lock) {
        s_service.callback = callback;
        s_service.callback_ctx = user_ctx;
        return;
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.callback = callback;
    s_service.callback_ctx = user_ctx;
    xSemaphoreGive(s_service.lock);
}
