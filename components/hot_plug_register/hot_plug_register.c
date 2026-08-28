/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hot_plug_register.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "subboard_support/subboard.h"
#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mosaico_camera.h"

#define HOT_PLUG_SCAN_PERIOD_MS       2000U
#define HOT_PLUG_DEBOUNCE_COUNT       2U
#define HOT_PLUG_EEPROM_I2C_FREQ_HZ   400000U
#define HOT_PLUG_EEPROM_TIMEOUT_MS    50U
#define HOT_PLUG_EEPROM_READ_ATTEMPTS 2U
#define HOT_PLUG_REGISTER_ATTEMPTS    2U
#define HOT_PLUG_TASK_STACK_SIZE      4096U
#define HOT_PLUG_TASK_PRIORITY        2U

#define HOT_PLUG_EEPROM_MAGIC          "ESP"
#define HOT_PLUG_EEPROM_MAGIC_LEN      3U
#define HOT_PLUG_EEPROM_IMAGE_SIZE     0x86U
#define HOT_PLUG_EEPROM_DESC_CRC_OFFSET 0x34U
#define HOT_PLUG_EEPROM_MFG_CRC_OFFSET  0x3EU
#define HOT_PLUG_EEPROM_PARAM_CRC_OFFSET 0x84U
#define HOT_PLUG_BOARD_TYPE_CAMERA     0x07U

static const char *TAG = "hot_plug_register";

typedef struct __attribute__((packed)) {
    char magic[3];
    uint8_t board_type;
    uint16_t board_id;
    uint16_t hw_version;
    uint16_t sw_version;
    uint16_t vendor_id;
    uint32_t board_flags;
    uint32_t serial_number;
    char board_name[32];
    uint16_t desc_crc16;
    uint32_t manufacture_date;
    uint16_t batch_number;
    uint16_t factory_id;
    uint16_t mfg_crc16;
    uint16_t param_version;
    uint16_t param_length;
    uint8_t param_data[64];
    uint16_t param_crc16;
} hot_plug_eeprom_v1_t;

_Static_assert(sizeof(hot_plug_eeprom_v1_t) == HOT_PLUG_EEPROM_IMAGE_SIZE,
               "subboard EEPROM V1 layout mismatch");

typedef esp_err_t (*hot_plug_insert_callback_t)(bsp_subboard_slot_t slot,
                                                 void **out_handle);
typedef esp_err_t (*hot_plug_remove_callback_t)(void *handle);
typedef esp_err_t (*hot_plug_probe_callback_t)(void *handle);

typedef struct {
    const char *name;
    uint8_t board_type;
    hot_plug_insert_callback_t insert;
    hot_plug_remove_callback_t remove;
    hot_plug_probe_callback_t probe;
    void *handle;
    bsp_subboard_slot_t slot;
    uint32_t borrowers;
    bool transitioning;
    SemaphoreHandle_t borrowers_released;
} hot_plug_registry_entry_t;

typedef struct {
    bsp_subboard_slot_t slot;
    i2c_master_dev_handle_t eeprom;
    hot_plug_eeprom_v1_t image;
    hot_plug_registry_entry_t *entry;
    bool stable_present;
    uint8_t invalid_attempts;
    uint8_t registration_attempts;
} hot_plug_slot_state_t;

typedef struct {
    bool initialized;
    volatile bool running;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t stopped;
    TaskHandle_t task;
    hot_plug_slot_state_t slots[BSP_SUBBOARD_SLOT_COUNT];
} hot_plug_context_t;

static const char *slot_name(bsp_subboard_slot_t slot);
static esp_err_t camera_insert(bsp_subboard_slot_t slot, void **out_handle);
static esp_err_t camera_remove(void *handle);
static esp_err_t camera_probe(void *handle);

static hot_plug_registry_entry_t s_registry[] = {
    {
        .name = HOT_PLUG_SUBBOARD_CAMERA_NAME,
        .board_type = HOT_PLUG_BOARD_TYPE_CAMERA,
        .insert = camera_insert,
        .remove = camera_remove,
        .probe = camera_probe,
        .slot = BSP_SUBBOARD_SLOT_COUNT,
    },
};

static hot_plug_context_t s_hot_plug;
static portMUX_TYPE s_notice_lock = portMUX_INITIALIZER_UNLOCKED;
static hot_plug_insert_notice_callback_t s_notice_callback;
static void *s_notice_user_ctx;
static const char *s_pending_notice_name;
static char s_pending_notice_slot;
static bool s_pending_notice_present;

static char slot_letter(bsp_subboard_slot_t slot)
{
    return slot == BSP_SUBBOARD_SLOT_RIGHT ? 'R' : 'L';
}

static void request_presence_notice(const char *subboard_name, char slot,
                                    bool present)
{
    hot_plug_insert_notice_callback_t callback = NULL;
    void *user_ctx = NULL;

    portENTER_CRITICAL(&s_notice_lock);
    callback = s_notice_callback;
    user_ctx = s_notice_user_ctx;
    if (callback == NULL) {
        if (present) {
            s_pending_notice_name = subboard_name;
            s_pending_notice_slot = slot;
            s_pending_notice_present = true;
        } else if (s_pending_notice_name == subboard_name) {
            s_pending_notice_name = NULL;
        }
    }
    portEXIT_CRITICAL(&s_notice_lock);

    if (callback != NULL) {
        callback(subboard_name, slot, present, user_ctx);
    }
}

static const char *slot_name(bsp_subboard_slot_t slot)
{
    switch (slot) {
    case BSP_SUBBOARD_SLOT_LEFT:
        return "left";
    case BSP_SUBBOARD_SLOT_RIGHT:
        return "right";
    default:
        return "unknown";
    }
}

static uint16_t eeprom_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U)
                             : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static bool eeprom_valid(const hot_plug_eeprom_v1_t *image)
{
    if (image == NULL ||
            memcmp(image->magic, HOT_PLUG_EEPROM_MAGIC,
                   HOT_PLUG_EEPROM_MAGIC_LEN) != 0) {
        return false;
    }

    const uint16_t desc_crc =
        eeprom_crc16((const uint8_t *)image,
                     HOT_PLUG_EEPROM_DESC_CRC_OFFSET);
    const uint16_t mfg_crc =
        eeprom_crc16((const uint8_t *)&image->manufacture_date,
                     HOT_PLUG_EEPROM_MFG_CRC_OFFSET - 0x36U);
    const uint16_t param_crc =
        eeprom_crc16((const uint8_t *)&image->param_version,
                     HOT_PLUG_EEPROM_PARAM_CRC_OFFSET - 0x40U);
    return image->desc_crc16 == desc_crc &&
           image->mfg_crc16 == mfg_crc &&
           image->param_crc16 == param_crc;
}

static bool eeprom_name_matches(const hot_plug_eeprom_v1_t *image,
                                const char *name)
{
    const size_t name_len = strlen(name);
    return name_len <= sizeof(image->board_name) &&
           strncmp(image->board_name, name, sizeof(image->board_name)) == 0;
}

static hot_plug_registry_entry_t *registry_find_locked(const char *name)
{
    for (size_t i = 0; i < sizeof(s_registry) / sizeof(s_registry[0]); ++i) {
        if (strcmp(s_registry[i].name, name) == 0) {
            return &s_registry[i];
        }
    }
    return NULL;
}

static hot_plug_registry_entry_t *registry_match_eeprom(
    const hot_plug_eeprom_v1_t *image)
{
    for (size_t i = 0; i < sizeof(s_registry) / sizeof(s_registry[0]); ++i) {
        if (s_registry[i].board_type == image->board_type &&
                eeprom_name_matches(image, s_registry[i].name)) {
            return &s_registry[i];
        }
    }
    return NULL;
}

static esp_err_t registry_activate(hot_plug_registry_entry_t *entry,
                                   bsp_subboard_slot_t slot)
{
    void *handle = NULL;

    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    if (entry->handle != NULL || entry->transitioning) {
        xSemaphoreGive(s_hot_plug.lock);
        return ESP_ERR_INVALID_STATE;
    }
    entry->transitioning = true;
    xSemaphoreGive(s_hot_plug.lock);

    esp_err_t ret = entry->insert(slot, &handle);

    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    if (ret == ESP_OK && handle != NULL) {
        entry->handle = handle;
        entry->slot = slot;
        entry->borrowers = 0;
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    entry->transitioning = false;
    xSemaphoreGive(s_hot_plug.lock);
    return ret;
}

static esp_err_t registry_deactivate(hot_plug_registry_entry_t *entry)
{
    void *handle = NULL;

    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    if (entry->handle == NULL) {
        entry->transitioning = false;
        entry->slot = BSP_SUBBOARD_SLOT_COUNT;
        xSemaphoreGive(s_hot_plug.lock);
        return ESP_OK;
    }
    entry->transitioning = true;
    while (xSemaphoreTake(entry->borrowers_released, 0) == pdTRUE) {
    }
    while (entry->borrowers != 0) {
        xSemaphoreGive(s_hot_plug.lock);
        xSemaphoreTake(entry->borrowers_released, portMAX_DELAY);
        xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    }
    handle = entry->handle;
    xSemaphoreGive(s_hot_plug.lock);

    const esp_err_t ret = entry->remove(handle);

    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    entry->handle = NULL;
    entry->slot = BSP_SUBBOARD_SLOT_COUNT;
    entry->transitioning = false;
    xSemaphoreGive(s_hot_plug.lock);
    return ret;
}

static esp_err_t attach_eeprom_devices(void)
{
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "subboard I2C bus is not initialized");

    for (size_t i = 0; i < BSP_SUBBOARD_SLOT_COUNT; ++i) {
        bsp_subboard_slot_config_t slot_config = {0};
        ESP_RETURN_ON_ERROR(
            bsp_subboard_get_slot_config((bsp_subboard_slot_t)i,
                                         &slot_config),
            TAG, "read slot %u configuration failed", (unsigned)i);
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = slot_config.eeprom_addr,
            .scl_speed_hz = HOT_PLUG_EEPROM_I2C_FREQ_HZ,
        };
        ESP_RETURN_ON_ERROR(
            i2c_master_bus_add_device(
                bus, &config, &s_hot_plug.slots[i].eeprom),
            TAG, "attach EEPROM 0x%02X failed", slot_config.eeprom_addr);
    }
    return ESP_OK;
}

static void detach_eeprom_devices(void)
{
    for (size_t i = 0; i < BSP_SUBBOARD_SLOT_COUNT; ++i) {
        if (s_hot_plug.slots[i].eeprom != NULL) {
            esp_err_t ret =
                i2c_master_bus_rm_device(s_hot_plug.slots[i].eeprom);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Detach %s EEPROM failed: %s",
                         slot_name((bsp_subboard_slot_t)i),
                         esp_err_to_name(ret));
            }
            s_hot_plug.slots[i].eeprom = NULL;
        }
    }
}

static esp_err_t read_eeprom(bsp_subboard_slot_t slot,
                             hot_plug_eeprom_v1_t *image)
{
    const uint8_t address = 0;
    return i2c_master_transmit_receive(
        s_hot_plug.slots[slot].eeprom, &address, sizeof(address),
        (uint8_t *)image, sizeof(*image), HOT_PLUG_EEPROM_TIMEOUT_MS);
}

static void reset_slot(hot_plug_slot_state_t *state)
{
    state->entry = NULL;
    state->stable_present = false;
    state->invalid_attempts = 0;
    state->registration_attempts = 0;
    memset(&state->image, 0, sizeof(state->image));
}

static void remove_active_slot(hot_plug_slot_state_t *state)
{
    hot_plug_registry_entry_t *entry = state->entry;
    const bsp_subboard_slot_t slot = state->slot;

    if (entry == NULL) {
        reset_slot(state);
        return;
    }

    ESP_LOGI(TAG, "Subboard removed: slot=%s name=%s",
             slot_name(slot), entry->name);
    const char *name = entry->name;
    esp_err_t ret = registry_deactivate(entry);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Remove callback failed for %s: %s",
                 name, esp_err_to_name(ret));
    }
    if (strcmp(name, HOT_PLUG_SUBBOARD_CAMERA_NAME) == 0) {
        request_presence_notice(name, slot_letter(slot), false);
    }
    reset_slot(state);
}

static void register_identified_slot(hot_plug_slot_state_t *state)
{
    hot_plug_registry_entry_t *entry = registry_match_eeprom(&state->image);
    if (entry == NULL) {
        ESP_LOGW(TAG,
                 "No registry entry: slot=%s type=0x%02X name=%.32s",
                 slot_name(state->slot), state->image.board_type,
                 state->image.board_name);
        state->registration_attempts = HOT_PLUG_REGISTER_ATTEMPTS;
        return;
    }
    if (state->registration_attempts >= HOT_PLUG_REGISTER_ATTEMPTS) {
        return;
    }
    state->registration_attempts++;

    esp_err_t ret = registry_activate(entry, state->slot);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Insert callback failed: slot=%s name=%s attempt=%u/%u: %s",
                 slot_name(state->slot), entry->name,
                 state->registration_attempts, HOT_PLUG_REGISTER_ATTEMPTS,
                 esp_err_to_name(ret));
        return;
    }

    state->entry = entry;
    ESP_LOGI(TAG,
             "Subboard registered: slot=%s type=0x%02X id=0x%04X name=%s",
             slot_name(state->slot), state->image.board_type,
             state->image.board_id, entry->name);
}

static void publish_presence(hot_plug_slot_state_t *state, bool present)
{
    if (!present) {
        if (state->stable_present) {
            ESP_LOGI(TAG, "Subboard removed: slot=%s",
                     slot_name(state->slot));
        }
        reset_slot(state);
        return;
    }

    hot_plug_eeprom_v1_t image = {0};
    esp_err_t ret = read_eeprom(state->slot, &image);
    state->stable_present = true;
    if (ret != ESP_OK || !eeprom_valid(&image)) {
        state->invalid_attempts++;
        ESP_LOGW(TAG,
                 "Invalid subboard EEPROM: slot=%s attempt=%u/%u error=%s",
                 slot_name(state->slot), state->invalid_attempts,
                 HOT_PLUG_EEPROM_READ_ATTEMPTS,
                 ret == ESP_OK ? "CRC" : esp_err_to_name(ret));
        return;
    }

    state->image = image;
    state->invalid_attempts = 0;
    ESP_LOGI(TAG,
             "Subboard inserted: slot=%s type=0x%02X id=0x%04X name=%.32s",
             slot_name(state->slot), image.board_type, image.board_id,
             image.board_name);
    register_identified_slot(state);
}

static void scan_active_slot(hot_plug_slot_state_t *state)
{
    esp_err_t ret = state->entry->probe(state->entry->handle);
    if (ret == ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "Subboard probe failed: slot=%s name=%s: %s",
             slot_name(state->slot), state->entry->name,
             esp_err_to_name(ret));
    remove_active_slot(state);
}

static void scan_idle_slot(hot_plug_slot_state_t *state)
{
    bsp_subboard_slot_config_t slot_config = {0};
    if (bsp_subboard_get_slot_config(state->slot, &slot_config) != ESP_OK) {
        return;
    }

    const bool present =
        i2c_master_probe(bsp_subboard_get_i2c_bus(), slot_config.eeprom_addr,
                         HOT_PLUG_EEPROM_TIMEOUT_MS) == ESP_OK;
    if (present != state->stable_present) {
        publish_presence(state, present);
        return;
    }
    if (present && state->entry == NULL) {
        if (state->invalid_attempts > 0 &&
                state->invalid_attempts < HOT_PLUG_EEPROM_READ_ATTEMPTS) {
            publish_presence(state, true);
        } else if (state->invalid_attempts == 0 &&
                   state->registration_attempts <
                       HOT_PLUG_REGISTER_ATTEMPTS) {
            register_identified_slot(state);
        }
    }
}

static void hot_plug_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Hot-plug registry started: period=%u ms",
             HOT_PLUG_SCAN_PERIOD_MS);

    while (s_hot_plug.running) {
        for (size_t i = 0; i < BSP_SUBBOARD_SLOT_COUNT; ++i) {
            hot_plug_slot_state_t *state = &s_hot_plug.slots[i];
            if (state->entry != NULL) {
                scan_active_slot(state);
            } else {
                scan_idle_slot(state);
            }
        }
        ulTaskNotifyTake(pdTRUE,
                         pdMS_TO_TICKS(HOT_PLUG_SCAN_PERIOD_MS));
    }

    s_hot_plug.task = NULL;
    xSemaphoreGive(s_hot_plug.stopped);
    vTaskDelete(NULL);
}

static esp_err_t camera_insert(bsp_subboard_slot_t slot, void **out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "camera handle output is null");
    ESP_RETURN_ON_FALSE(slot == BSP_SUBBOARD_SLOT_LEFT,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "CameraBoard supports the left slot only");

    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.slot = slot;
    mosaico_camera_handle_t camera = NULL;
    ESP_RETURN_ON_ERROR(mosaico_camera_new(&config, &camera), TAG,
                        "register CameraBoard video device failed");

    esp_err_t ret = esp_board_manager_init_device_by_name("camera");
    if (ret != ESP_OK) {
        (void)mosaico_camera_del(camera);
        ESP_LOGE(TAG, "Publish CameraBoard video path failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    *out_handle = camera;
    ESP_LOGI(TAG,
             "CameraBoard initialized; /dev/video0 is ready for App or Lua open");
    request_presence_notice(HOT_PLUG_SUBBOARD_CAMERA_NAME, 'L', true);
    return ESP_OK;
}

static esp_err_t camera_remove(void *handle)
{
    if (handle == NULL) {
        return ESP_OK;
    }

    esp_err_t camera_ret =
        mosaico_camera_del((mosaico_camera_handle_t)handle);
    esp_err_t board_ret =
        esp_board_manager_deinit_device_by_name("camera");
    if (camera_ret == ESP_OK && board_ret == ESP_OK) {
        ESP_LOGI(TAG, "CameraBoard camera deinitialized");
    }
    return camera_ret != ESP_OK ? camera_ret : board_ret;
}

static esp_err_t camera_probe(void *handle)
{
    return mosaico_camera_probe((mosaico_camera_handle_t)handle);
}

esp_err_t hot_plug_register_init(void)
{
    if (s_hot_plug.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_subboard_init(), TAG,
                        "initialize subboard BSP failed");

    s_hot_plug.lock = xSemaphoreCreateMutex();
    s_hot_plug.stopped = xSemaphoreCreateBinary();
    if (s_hot_plug.lock == NULL || s_hot_plug.stopped == NULL) {
        hot_plug_register_deinit();
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < sizeof(s_registry) / sizeof(s_registry[0]); ++i) {
        s_registry[i].borrowers_released = xSemaphoreCreateBinary();
        if (s_registry[i].borrowers_released == NULL) {
            hot_plug_register_deinit();
            return ESP_ERR_NO_MEM;
        }
    }
    for (size_t i = 0; i < BSP_SUBBOARD_SLOT_COUNT; ++i) {
        s_hot_plug.slots[i].slot = (bsp_subboard_slot_t)i;
    }

    esp_err_t ret = attach_eeprom_devices();
    if (ret != ESP_OK) {
        hot_plug_register_deinit();
        return ret;
    }

    s_hot_plug.running = true;
    s_hot_plug.initialized = true;
    if (xTaskCreate(hot_plug_task, "hot_plug_register",
                    HOT_PLUG_TASK_STACK_SIZE, NULL, HOT_PLUG_TASK_PRIORITY,
                    &s_hot_plug.task) != pdPASS) {
        s_hot_plug.running = false;
        s_hot_plug.initialized = false;
        hot_plug_register_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t hot_plug_register_deinit(void)
{
    if (s_hot_plug.running) {
        s_hot_plug.running = false;
        if (s_hot_plug.task != NULL) {
            xTaskNotifyGive(s_hot_plug.task);
            if (s_hot_plug.stopped != NULL &&
                    xSemaphoreTake(s_hot_plug.stopped,
                                   pdMS_TO_TICKS(1000)) != pdTRUE) {
                return ESP_ERR_TIMEOUT;
            }
        }
    }

    if (s_hot_plug.lock != NULL) {
        for (size_t i = 0;
                i < sizeof(s_registry) / sizeof(s_registry[0]); ++i) {
            (void)registry_deactivate(&s_registry[i]);
        }
    }
    detach_eeprom_devices();
    for (size_t i = 0; i < sizeof(s_registry) / sizeof(s_registry[0]); ++i) {
        if (s_registry[i].borrowers_released != NULL) {
            vSemaphoreDelete(s_registry[i].borrowers_released);
        }
        memset(&s_registry[i].handle, 0,
               sizeof(s_registry[i]) -
                   offsetof(hot_plug_registry_entry_t, handle));
        s_registry[i].slot = BSP_SUBBOARD_SLOT_COUNT;
    }
    if (s_hot_plug.lock != NULL) {
        vSemaphoreDelete(s_hot_plug.lock);
    }
    if (s_hot_plug.stopped != NULL) {
        vSemaphoreDelete(s_hot_plug.stopped);
    }
    memset(&s_hot_plug, 0, sizeof(s_hot_plug));
    ESP_LOGI(TAG, "Hot-plug registry stopped");
    return ESP_OK;
}

esp_err_t hot_plug_register_get_handle(const char *subboard_name,
                                       void **out_handle)
{
    ESP_RETURN_ON_FALSE(subboard_name != NULL && out_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid handle lookup");
    *out_handle = NULL;
    ESP_RETURN_ON_FALSE(s_hot_plug.initialized && s_hot_plug.lock != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "hot-plug registry is not initialized");

    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    hot_plug_registry_entry_t *entry =
        registry_find_locked(subboard_name);
    if (entry == NULL) {
        xSemaphoreGive(s_hot_plug.lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (entry->handle == NULL || entry->transitioning) {
        xSemaphoreGive(s_hot_plug.lock);
        return ESP_ERR_INVALID_STATE;
    }

    entry->borrowers++;
    *out_handle = entry->handle;
    xSemaphoreGive(s_hot_plug.lock);
    return ESP_OK;
}

esp_err_t hot_plug_register_put_handle(const char *subboard_name,
                                       void *handle)
{
    ESP_RETURN_ON_FALSE(subboard_name != NULL && handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid handle release");
    ESP_RETURN_ON_FALSE(s_hot_plug.initialized && s_hot_plug.lock != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "hot-plug registry is not initialized");

    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    hot_plug_registry_entry_t *entry =
        registry_find_locked(subboard_name);
    if (entry == NULL) {
        xSemaphoreGive(s_hot_plug.lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (entry->handle != handle || entry->borrowers == 0) {
        xSemaphoreGive(s_hot_plug.lock);
        return ESP_ERR_INVALID_STATE;
    }
    entry->borrowers--;
    if (entry->borrowers == 0) {
        xSemaphoreGive(entry->borrowers_released);
    }
    xSemaphoreGive(s_hot_plug.lock);
    return ESP_OK;
}

esp_err_t hot_plug_register_is_present(const char *subboard_name,
                                       bool *out_present)
{
    ESP_RETURN_ON_FALSE(subboard_name != NULL && out_present != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid presence lookup");
    *out_present = false;
    ESP_RETURN_ON_FALSE(s_hot_plug.initialized && s_hot_plug.lock != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "hot-plug registry is not initialized");

    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    hot_plug_registry_entry_t *entry =
        registry_find_locked(subboard_name);
    if (entry != NULL && entry->handle != NULL && !entry->transitioning) {
        *out_present = true;
    }
    xSemaphoreGive(s_hot_plug.lock);
    return ESP_OK;
}

esp_err_t hot_plug_register_release_device(const char *subboard_name)
{
    ESP_RETURN_ON_FALSE(subboard_name != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid device release");
    ESP_RETURN_ON_FALSE(
        strcmp(subboard_name, HOT_PLUG_SUBBOARD_CAMERA_NAME) == 0,
        ESP_ERR_NOT_SUPPORTED, TAG,
        "release_device is implemented for CameraBoard only");
    ESP_RETURN_ON_FALSE(s_hot_plug.initialized && s_hot_plug.lock != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "hot-plug registry is not initialized");

    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    hot_plug_registry_entry_t *entry =
        registry_find_locked(subboard_name);
    if (entry == NULL) {
        xSemaphoreGive(s_hot_plug.lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (entry->handle == NULL) {
        xSemaphoreGive(s_hot_plug.lock);
        return ESP_OK;
    }
    while (xSemaphoreTake(entry->borrowers_released, 0) == pdTRUE) {
    }
    while (entry->borrowers != 0) {
        xSemaphoreGive(s_hot_plug.lock);
        xSemaphoreTake(entry->borrowers_released, portMAX_DELAY);
        xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
        if (entry->handle == NULL) {
            xSemaphoreGive(s_hot_plug.lock);
            return ESP_OK;
        }
    }
    void *handle = entry->handle;
    entry->transitioning = true;
    xSemaphoreGive(s_hot_plug.lock);

    const esp_err_t ret =
        mosaico_camera_close((mosaico_camera_handle_t)handle);
    xSemaphoreTake(s_hot_plug.lock, portMAX_DELAY);
    entry = registry_find_locked(subboard_name);
    if (entry != NULL && entry->handle == handle) {
        entry->transitioning = false;
    }
    xSemaphoreGive(s_hot_plug.lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "CameraBoard camera closed; mmap released, device retained");
    }
    return ret;
}

void hot_plug_register_set_insert_notice_callback(
    hot_plug_insert_notice_callback_t callback, void *user_ctx)
{
    const char *pending_name = NULL;
    char pending_slot = 'L';
    bool pending_present = false;

    portENTER_CRITICAL(&s_notice_lock);
    s_notice_callback = callback;
    s_notice_user_ctx = user_ctx;
    if (callback != NULL && s_pending_notice_name != NULL) {
        pending_name = s_pending_notice_name;
        pending_slot = s_pending_notice_slot;
        pending_present = s_pending_notice_present;
        s_pending_notice_name = NULL;
    }
    portEXIT_CRITICAL(&s_notice_lock);

    if (callback != NULL && pending_name != NULL) {
        callback(pending_name, pending_slot, pending_present, user_ctx);
    }
}
