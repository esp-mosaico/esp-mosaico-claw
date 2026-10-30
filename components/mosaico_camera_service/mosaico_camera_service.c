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
#include "sdkconfig.h"

#ifdef CONFIG_MOSAICO_CAMERA_EQUIVALENT_ROI
#include "mosaico_camera_roi.h"
#endif

static const char *TAG = "camera_service";

typedef struct {
    SemaphoreHandle_t lock;
    mosaico_camera_handle_t camera;
    mosaico_camera_handle_t pending_cleanup;
    mosaico_module_subscription_t subscription;
    mosaico_camera_availability_callback_t callback;
    void *callback_ctx;
    bool initialized;
    bool activating;
    bool subscribed;
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

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t buffer_count;
} camera_profile_t;

/* Keep high-resolution defaults lean; VGA retries with two buffers for stable capture. */
static const camera_profile_t s_camera_profiles[] = {
    {0, 0, 1},
    {640, 480, 2},
};

static esp_err_t create_supported_camera(mosaico_module_mgr_slot_t slot,
                                         mosaico_camera_handle_t *out_camera)
{
    esp_err_t last_err = ESP_ERR_NOT_SUPPORTED;

    *out_camera = NULL;
    for (size_t i = 0; i < sizeof(s_camera_profiles) / sizeof(s_camera_profiles[0]); ++i) {
        const camera_profile_t *profile = &s_camera_profiles[i];
        mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
        config.slot = slot;
        config.width = profile->width;
        config.height = profile->height;
        config.buffer_count = profile->buffer_count;

        mosaico_camera_handle_t camera = NULL;
        mosaico_camera_info_t camera_info = {0};
        esp_err_t ret = mosaico_camera_new(&config, &camera);
#ifdef CONFIG_MOSAICO_CAMERA_EQUIVALENT_ROI
        if (ret == ESP_OK && profile->width == 0) {
            ret = mosaico_camera_configure_equivalent_roi(camera);
        }
#endif
        if (ret == ESP_OK) {
            ret = mosaico_camera_open(camera);
        }
        if (ret == ESP_OK) {
            ret = mosaico_camera_get_info(camera, &camera_info);
            const esp_err_t close_ret = mosaico_camera_close(camera);
            if (ret == ESP_OK && close_ret != ESP_OK) {
                ret = close_ret;
            }
        }

        const bool retry_vga = ret == ESP_OK && profile->width == 0 && camera_info.width == 640 && camera_info.height == 480;
        if (ret == ESP_OK && !retry_vga) {
            ESP_LOGI(TAG, "Camera profile accepted: %ux%u buffers=%u stride=%u frame=%zu",
                     (unsigned)camera_info.width, (unsigned)camera_info.height,
                     (unsigned)camera_info.buffer_count, (unsigned)camera_info.bytes_per_line,
                     camera_info.frame_buffer_size);
            *out_camera = camera;
            return ESP_OK;
        }
        if (retry_vga) {
            ESP_LOGI(TAG, "VGA camera detected; retrying with double buffering");
        } else {
            last_err = ret;
            ESP_LOGW(TAG, "Camera profile rejected: %s", esp_err_to_name(ret));
        }
        if (camera != NULL) {
            const esp_err_t del_ret = mosaico_camera_del(camera);
            if (del_ret != ESP_OK) {
                *out_camera = camera;
                ESP_LOGE(TAG, "Delete rejected camera failed: %s", esp_err_to_name(del_ret));
                return del_ret;
            }
        }
    }

    return last_err;
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

    mosaico_camera_handle_t camera = NULL;
    esp_err_t ret = create_supported_camera(info->slot, &camera);

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
    ESP_RETURN_ON_ERROR(mosaico_module_mgr_get_info(MOSAICO_MODULE_MGR_SLOT_LEFT, &info), TAG, "get camera slot info failed");
    if (info.presence != MOSAICO_MODULE_PRESENCE_PRESENT || info.descriptor_state != MOSAICO_MODULE_DESCRIPTOR_VALID ||
        info.owner_state != MOSAICO_MODULE_OWNER_FREE || info.eeprom.board_type != MOSAICO_BOARD_TYPE_CAMERA) {
        return ESP_OK;
    }
    return activate_camera(&info);
}

static void module_event(const mosaico_module_mgr_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event || event->info.slot != MOSAICO_MODULE_MGR_SLOT_LEFT) {
        return;
    }
    if ((event->changes & MOSAICO_MODULE_CHANGE_PRESENCE) && event->info.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
        if (event->info.eeprom.board_type == MOSAICO_BOARD_TYPE_CAMERA) {
            notify_availability('L', false);
        }
        return;
    }
    if (event->info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
        event->info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID && event->info.owner_state == MOSAICO_MODULE_OWNER_FREE &&
        event->info.eeprom.board_type == MOSAICO_BOARD_TYPE_CAMERA &&
        (event->changes & (MOSAICO_MODULE_CHANGE_PRESENCE | MOSAICO_MODULE_CHANGE_DESCRIPTOR | MOSAICO_MODULE_CHANGE_OWNER))) {
        esp_err_t ret = activate_camera(&event->info);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Activate camera failed: %s", esp_err_to_name(ret));
        }
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

    esp_err_t ret = mosaico_module_mgr_init(NULL);
    if (ret == ESP_OK) {
        ret = mosaico_module_mgr_subscribe(module_event, NULL, &s_service.subscription);
        s_service.subscribed = ret == ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = activate_ready_camera();
    }
    if (ret != ESP_OK) {
        if (s_service.subscribed) {
            (void)mosaico_module_mgr_unsubscribe(&s_service.subscription);
            s_service.subscribed = false;
        }
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
    if (s_service.subscribed) {
        (void)mosaico_module_mgr_unsubscribe(&s_service.subscription);
        s_service.subscribed = false;
    }

    if (camera) {
        esp_err_t ret = mosaico_camera_del(camera);
        if (ret != ESP_OK) {
            xSemaphoreTake(s_service.lock, portMAX_DELAY);
            s_service.initialized = true;
            xSemaphoreGive(s_service.lock);
            const esp_err_t subscribe_ret = mosaico_module_mgr_subscribe(module_event, NULL, &s_service.subscription);
            s_service.subscribed = subscribe_ret == ESP_OK;
            if (subscribe_ret != ESP_OK) {
                ESP_LOGE(TAG, "Restore module subscription failed: %s", esp_err_to_name(subscribe_ret));
            }
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
    if (!s_service.lock) {
        return false;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    const bool available = s_service.initialized && s_service.camera;
    xSemaphoreGive(s_service.lock);
    return available;
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
