/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_lcd_touch_mux.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_lcd_touch_mux_wake.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define TOUCH_MUX_MIN_QUEUE_DEPTH 4U
#define TOUCH_MUX_LOCK_TIMEOUT_MS 1000U

typedef enum {
    TOUCH_MUX_SOURCE_IDLE = 0,
    TOUCH_MUX_SOURCE_PHYSICAL,
    TOUCH_MUX_SOURCE_VIRTUAL,
} touch_mux_source_t;

typedef struct {
    esp_lcd_touch_mux_frame_t frame;
    bool ends_session;
} touch_mux_queue_item_t;

typedef struct touch_mux_t {
    esp_lcd_touch_t base;
    struct touch_mux_t *next;
    esp_lcd_touch_handle_t physical;
    QueueHandle_t queue;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t producer_mutex;
    esp_lcd_touch_mux_frame_t current_frame;
    esp_lcd_touch_mux_trigger_cb_t trigger_interrupt;
    void *trigger_interrupt_user_ctx;
    esp_lcd_touch_mux_stats_t stats;
    touch_mux_source_t source;
    bool own_physical_handle;
    bool irq_armed;
} touch_mux_t;

static const char *TAG = "touch_mux";
static portMUX_TYPE s_registry_lock = portMUX_INITIALIZER_UNLOCKED;
static touch_mux_t *s_registry;

static touch_mux_t *touch_mux_from_handle(esp_lcd_touch_handle_t touch)
{
    if (touch == NULL) {
        return NULL;
    }
    touch_mux_t *found = NULL;
    taskENTER_CRITICAL(&s_registry_lock);
    for (touch_mux_t *mux = s_registry; mux != NULL; mux = mux->next) {
        if (&mux->base == touch) {
            found = mux;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_registry_lock);
    return found;
}

static void touch_mux_registry_add(touch_mux_t *mux)
{
    taskENTER_CRITICAL(&s_registry_lock);
    mux->next = s_registry;
    s_registry = mux;
    taskEXIT_CRITICAL(&s_registry_lock);
}

static void touch_mux_registry_remove(touch_mux_t *mux)
{
    taskENTER_CRITICAL(&s_registry_lock);
    touch_mux_t **cursor = &s_registry;
    while (*cursor != NULL && *cursor != mux) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == mux) {
        *cursor = mux->next;
    }
    taskEXIT_CRITICAL(&s_registry_lock);
}

static TickType_t touch_mux_timeout_ticks(uint32_t timeout_ms)
{
    return timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

static bool touch_mux_lock(SemaphoreHandle_t mutex, uint32_t timeout_ms)
{
    return mutex != NULL && xSemaphoreTake(mutex, touch_mux_timeout_ticks(timeout_ms)) == pdTRUE;
}

static void touch_mux_update_high_watermark_locked(touch_mux_t *mux)
{
    UBaseType_t queued = uxQueueMessagesWaiting(mux->queue);
    if (queued > mux->stats.queue_high_watermark) {
        mux->stats.queue_high_watermark = queued > UINT8_MAX ? UINT8_MAX : (uint8_t)queued;
    }
}

static esp_err_t touch_mux_trigger(touch_mux_t *mux)
{
    if (mux->base.config.int_gpio_num == GPIO_NUM_NC || mux->base.config.interrupt_callback == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = mux->trigger_interrupt(mux->base.config.int_gpio_num,
                                           mux->trigger_interrupt_user_ctx);
    if (err == ESP_OK && touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS)) {
        mux->stats.software_interrupts++;
        xSemaphoreGive(mux->state_mutex);
    }
    return err;
}

static esp_err_t touch_mux_arm_if_needed(touch_mux_t *mux)
{
    bool trigger = false;
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "state lock timeout");
    if (!mux->irq_armed && uxQueueMessagesWaiting(mux->queue) > 0) {
        mux->irq_armed = true;
        trigger = true;
    }
    xSemaphoreGive(mux->state_mutex);
    if (!trigger) {
        return ESP_OK;
    }
    esp_err_t err = touch_mux_trigger(mux);
    if (err != ESP_OK && touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS)) {
        mux->irq_armed = false;
        mux->stats.injection_failures++;
        xSemaphoreGive(mux->state_mutex);
    }
    return err;
}

static esp_err_t touch_mux_read_physical(touch_mux_t *mux)
{
    esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
    uint8_t count = 0;
    esp_err_t err = esp_lcd_touch_read_data(mux->physical);
    if (err == ESP_OK) {
        err = esp_lcd_touch_get_data(mux->physical, points, &count,
                                     CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Physical touch read failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "state lock timeout");
    mux->current_frame.count = count;
    memcpy(mux->current_frame.points, points, count * sizeof(points[0]));
    mux->source = count > 0 ? TOUCH_MUX_SOURCE_PHYSICAL : TOUCH_MUX_SOURCE_IDLE;
    xSemaphoreGive(mux->state_mutex);
    return ESP_OK;
}

static esp_err_t touch_mux_read_data(esp_lcd_touch_handle_t touch)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    ESP_RETURN_ON_FALSE(mux != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid mux handle");

    touch_mux_queue_item_t item = {0};
    bool virtual_active = false;
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "state lock timeout");
    virtual_active = mux->stats.virtual_active;
    xSemaphoreGive(mux->state_mutex);
    if (!virtual_active) {
        return touch_mux_read_physical(mux);
    }

    bool consumed = xQueueReceive(mux->queue, &item, 0) == pdTRUE;
    bool retrigger = false;
    bool physical_resync = false;
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "state lock timeout");
    mux->irq_armed = false;
    if (consumed) {
        mux->current_frame = item.frame;
        mux->source = TOUCH_MUX_SOURCE_VIRTUAL;
        mux->stats.consumed_frames++;
        if (item.ends_session) {
            mux->stats.virtual_active = false;
            mux->source = TOUCH_MUX_SOURCE_IDLE;
            physical_resync = true;
        }
    }
    if (mux->stats.virtual_active && uxQueueMessagesWaiting(mux->queue) > 0) {
        mux->irq_armed = true;
        retrigger = true;
    }
    xSemaphoreGive(mux->state_mutex);

    if (retrigger || physical_resync) {
        esp_err_t err = touch_mux_trigger(mux);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to advance touch input: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static bool touch_mux_get_xy(esp_lcd_touch_handle_t touch, uint16_t *x, uint16_t *y,
                             uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    if (mux == NULL || x == NULL || y == NULL || point_num == NULL || max_point_num == 0 ||
            !touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS)) {
        return false;
    }
    uint8_t count = mux->current_frame.count < max_point_num ? mux->current_frame.count : max_point_num;
    for (uint8_t i = 0; i < count; ++i) {
        x[i] = mux->current_frame.points[i].x;
        y[i] = mux->current_frame.points[i].y;
        if (strength != NULL) {
            strength[i] = mux->current_frame.points[i].strength;
        }
    }
    *point_num = count;
    xSemaphoreGive(mux->state_mutex);
    return count > 0;
}

static esp_err_t touch_mux_get_track_id(esp_lcd_touch_handle_t touch, uint8_t *track_id,
                                        uint8_t point_num)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    ESP_RETURN_ON_FALSE(mux != NULL && track_id != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid track ID arguments");
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "state lock timeout");
    uint8_t count = point_num < mux->current_frame.count ? point_num : mux->current_frame.count;
    for (uint8_t i = 0; i < count; ++i) {
        track_id[i] = mux->current_frame.points[i].track_id;
    }
    xSemaphoreGive(mux->state_mutex);
    return ESP_OK;
}

static esp_err_t touch_mux_enter_sleep(esp_lcd_touch_handle_t touch)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    return mux != NULL ? esp_lcd_touch_enter_sleep(mux->physical) : ESP_ERR_INVALID_ARG;
}

static esp_err_t touch_mux_exit_sleep(esp_lcd_touch_handle_t touch)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    return mux != NULL ? esp_lcd_touch_exit_sleep(mux->physical) : ESP_ERR_INVALID_ARG;
}

static esp_err_t touch_mux_set_swap_xy(esp_lcd_touch_handle_t touch, bool swap)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    return mux != NULL ? esp_lcd_touch_set_swap_xy(mux->physical, swap) : ESP_ERR_INVALID_ARG;
}

static esp_err_t touch_mux_get_swap_xy(esp_lcd_touch_handle_t touch, bool *swap)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    return mux != NULL ? esp_lcd_touch_get_swap_xy(mux->physical, swap) : ESP_ERR_INVALID_ARG;
}

static esp_err_t touch_mux_set_mirror_x(esp_lcd_touch_handle_t touch, bool mirror)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    return mux != NULL ? esp_lcd_touch_set_mirror_x(mux->physical, mirror) : ESP_ERR_INVALID_ARG;
}

static esp_err_t touch_mux_get_mirror_x(esp_lcd_touch_handle_t touch, bool *mirror)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    return mux != NULL ? esp_lcd_touch_get_mirror_x(mux->physical, mirror) : ESP_ERR_INVALID_ARG;
}

static esp_err_t touch_mux_set_mirror_y(esp_lcd_touch_handle_t touch, bool mirror)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    return mux != NULL ? esp_lcd_touch_set_mirror_y(mux->physical, mirror) : ESP_ERR_INVALID_ARG;
}

static esp_err_t touch_mux_get_mirror_y(esp_lcd_touch_handle_t touch, bool *mirror)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    return mux != NULL ? esp_lcd_touch_get_mirror_y(mux->physical, mirror) : ESP_ERR_INVALID_ARG;
}

static esp_err_t touch_mux_del(esp_lcd_touch_handle_t touch)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    ESP_RETURN_ON_FALSE(mux != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid mux handle");
    if (touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS)) {
        if (mux->stats.virtual_active) {
            ESP_LOGW(TAG, "Deleting mux with active virtual touch");
        }
        xSemaphoreGive(mux->state_mutex);
    }
    if (mux->base.config.interrupt_callback != NULL && mux->base.config.int_gpio_num != GPIO_NUM_NC) {
        esp_err_t err = esp_lcd_touch_register_interrupt_callback(touch, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to unregister touch interrupt: %s", esp_err_to_name(err));
            return err;
        }
    }
    esp_err_t physical_err = ESP_OK;
    if (mux->own_physical_handle) {
        physical_err = esp_lcd_touch_del(mux->physical);
        if (physical_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete physical touch: %s", esp_err_to_name(physical_err));
        }
    }
    touch_mux_registry_remove(mux);
    vQueueDelete(mux->queue);
    vSemaphoreDelete(mux->state_mutex);
    vSemaphoreDelete(mux->producer_mutex);
    free(mux);
    return physical_err;
}

esp_err_t esp_lcd_touch_mux_new(esp_lcd_touch_handle_t physical,
                                const esp_lcd_touch_mux_config_t *config,
                                esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(physical != NULL && config != NULL && out_touch != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid create arguments");
    *out_touch = NULL;
    ESP_RETURN_ON_FALSE(physical->read_data != NULL && physical->get_xy != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "physical touch is not initialized");
    ESP_RETURN_ON_FALSE(physical->config.interrupt_callback == NULL,
                        ESP_ERR_INVALID_STATE, TAG, "physical touch interrupt is already owned");
    ESP_RETURN_ON_FALSE(config->queue_depth >= TOUCH_MUX_MIN_QUEUE_DEPTH,
                        ESP_ERR_INVALID_ARG, TAG, "queue depth is too small");
    touch_mux_t *mux = calloc(1, sizeof(*mux));
    ESP_RETURN_ON_FALSE(mux != NULL, ESP_ERR_NO_MEM, TAG, "allocate mux");
    mux->queue = xQueueCreate(config->queue_depth, sizeof(touch_mux_queue_item_t));
    mux->state_mutex = xSemaphoreCreateMutex();
    mux->producer_mutex = xSemaphoreCreateMutex();
    if (mux->queue == NULL || mux->state_mutex == NULL || mux->producer_mutex == NULL) {
        if (mux->queue != NULL) vQueueDelete(mux->queue);
        if (mux->state_mutex != NULL) vSemaphoreDelete(mux->state_mutex);
        if (mux->producer_mutex != NULL) vSemaphoreDelete(mux->producer_mutex);
        free(mux);
        return ESP_ERR_NO_MEM;
    }

    mux->physical = physical;
    mux->own_physical_handle = config->own_physical_handle;
    mux->trigger_interrupt = config->trigger_interrupt != NULL ?
        config->trigger_interrupt : esp_lcd_touch_mux_gpio_trigger;
    mux->trigger_interrupt_user_ctx = config->trigger_interrupt_user_ctx;
    mux->base = (esp_lcd_touch_t) {
        .enter_sleep = touch_mux_enter_sleep,
        .exit_sleep = touch_mux_exit_sleep,
        .read_data = touch_mux_read_data,
        .get_xy = touch_mux_get_xy,
        .get_track_id = touch_mux_get_track_id,
        .set_swap_xy = touch_mux_set_swap_xy,
        .get_swap_xy = touch_mux_get_swap_xy,
        .set_mirror_x = touch_mux_set_mirror_x,
        .get_mirror_x = touch_mux_get_mirror_x,
        .set_mirror_y = touch_mux_set_mirror_y,
        .get_mirror_y = touch_mux_get_mirror_y,
        .del = touch_mux_del,
        .config = physical->config,
        .io = physical->io,
    };
    mux->base.config.process_coordinates = NULL;
    mux->base.config.interrupt_callback = NULL;
    mux->base.config.user_data = NULL;
    mux->base.config.driver_data = NULL;
    touch_mux_registry_add(mux);
    *out_touch = &mux->base;
    return ESP_OK;
}

esp_err_t esp_lcd_touch_mux_begin(esp_lcd_touch_handle_t touch, uint32_t timeout_ms)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    ESP_RETURN_ON_FALSE(mux != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid mux handle");
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->producer_mutex, timeout_ms), ESP_ERR_TIMEOUT,
                        TAG, "producer lock timeout");
    esp_err_t err = ESP_OK;
    bool state_locked = touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS);
    if (!state_locked) {
        err = ESP_ERR_TIMEOUT;
    } else if (mux->stats.virtual_active || mux->source == TOUCH_MUX_SOURCE_PHYSICAL) {
        err = ESP_ERR_INVALID_STATE;
    } else if (mux->base.config.interrupt_callback == NULL) {
        ESP_LOGE(TAG, "Touch interrupt callback is not registered");
        err = ESP_ERR_INVALID_STATE;
    } else {
        xQueueReset(mux->queue);
        memset(&mux->current_frame, 0, sizeof(mux->current_frame));
        mux->stats.virtual_active = true;
        mux->source = TOUCH_MUX_SOURCE_VIRTUAL;
        mux->irq_armed = false;
    }
    if (state_locked) {
        xSemaphoreGive(mux->state_mutex);
    }
    xSemaphoreGive(mux->producer_mutex);
    return err;
}

static esp_err_t touch_mux_enqueue(touch_mux_t *mux, const esp_lcd_touch_mux_frame_t *frame,
                                   bool ends_session, uint32_t timeout_ms)
{
    touch_mux_queue_item_t item = {
        .frame = *frame,
        .ends_session = ends_session,
    };
    if (xQueueSend(mux->queue, &item, touch_mux_timeout_ticks(timeout_ms)) != pdTRUE) {
        if (touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS)) {
            mux->stats.injection_failures++;
            xSemaphoreGive(mux->state_mutex);
        }
        ESP_LOGW(TAG, "Virtual touch queue timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS)) {
        mux->stats.injected_frames++;
        touch_mux_update_high_watermark_locked(mux);
        xSemaphoreGive(mux->state_mutex);
    }
    return touch_mux_arm_if_needed(mux);
}

static esp_err_t touch_mux_virtual_active(touch_mux_t *mux, bool *active)
{
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "state lock timeout");
    *active = mux->stats.virtual_active;
    xSemaphoreGive(mux->state_mutex);
    return ESP_OK;
}

static esp_err_t touch_mux_force_release(touch_mux_t *mux)
{
    xQueueReset(mux->queue);
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "state lock timeout");
    mux->irq_armed = false;
    xSemaphoreGive(mux->state_mutex);

    const esp_lcd_touch_mux_frame_t release = {0};
    esp_err_t err = touch_mux_enqueue(mux, &release, true, 0);
    if (err == ESP_OK && touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS)) {
        mux->stats.forced_releases++;
        xSemaphoreGive(mux->state_mutex);
    }
    return err;
}

esp_err_t esp_lcd_touch_mux_inject(esp_lcd_touch_handle_t touch,
                                   const esp_lcd_touch_mux_frame_t *frame,
                                   uint32_t timeout_ms)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    ESP_RETURN_ON_FALSE(mux != NULL && frame != NULL &&
                        frame->count <= CONFIG_ESP_LCD_TOUCH_MAX_POINTS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid injected frame");
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->producer_mutex, timeout_ms), ESP_ERR_TIMEOUT,
                        TAG, "producer lock timeout");
    bool active = false;
    esp_err_t err = touch_mux_virtual_active(mux, &active);
    if (err == ESP_OK) {
        err = active ? touch_mux_enqueue(mux, frame, false, timeout_ms) : ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(mux->producer_mutex);
    return err;
}

esp_err_t esp_lcd_touch_mux_end(esp_lcd_touch_handle_t touch, uint32_t timeout_ms)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    ESP_RETURN_ON_FALSE(mux != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid mux handle");
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->producer_mutex, timeout_ms), ESP_ERR_TIMEOUT,
                        TAG, "producer lock timeout");
    bool active = false;
    esp_err_t err = touch_mux_virtual_active(mux, &active);
    const esp_lcd_touch_mux_frame_t release = {0};
    if (err == ESP_OK) {
        err = active ? touch_mux_enqueue(mux, &release, true, timeout_ms) : ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_ERR_TIMEOUT) {
        err = touch_mux_force_release(mux);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to queue forced release: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(mux->producer_mutex);
    return err;
}

esp_err_t esp_lcd_touch_mux_cancel(esp_lcd_touch_handle_t touch)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    ESP_RETURN_ON_FALSE(mux != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid mux handle");
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->producer_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "producer lock timeout");
    bool active = false;
    esp_err_t err = touch_mux_virtual_active(mux, &active);
    if (err == ESP_OK && active) {
        err = touch_mux_force_release(mux);
    }
    xSemaphoreGive(mux->producer_mutex);
    return err;
}

esp_err_t esp_lcd_touch_mux_get_stats(esp_lcd_touch_handle_t touch,
                                      esp_lcd_touch_mux_stats_t *out_stats)
{
    touch_mux_t *mux = touch_mux_from_handle(touch);
    ESP_RETURN_ON_FALSE(mux != NULL && out_stats != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid stats arguments");
    ESP_RETURN_ON_FALSE(touch_mux_lock(mux->state_mutex, TOUCH_MUX_LOCK_TIMEOUT_MS),
                        ESP_ERR_TIMEOUT, TAG, "state lock timeout");
    *out_stats = mux->stats;
    xSemaphoreGive(mux->state_mutex);
    return ESP_OK;
}
