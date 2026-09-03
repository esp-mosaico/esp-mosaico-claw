/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_module_mgr.h"

#include <inttypes.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "subboard_support/subboard.h"

#define EEPROM_DESC_CRC_OFFSET 0x34U
#define EEPROM_MFG_CRC_OFFSET 0x3EU
#define EEPROM_PARAM_CRC_OFFSET 0x84U
#define EEPROM_I2C_FREQ_HZ 100000U
#define EEPROM_I2C_TIMEOUT_MS 100U
#define EEPROM_READ_ATTEMPTS 3U
#define EEPROM_DETACH_ATTEMPTS 3U
#define EEPROM_DETACH_RETRY_MS 20U
#define MANAGER_TASK_STACK_SIZE 4096U
#define MANAGER_TASK_PRIORITY 5U

static const char *TAG = "mosaico_module_mgr";

typedef struct {
    mosaico_module_mgr_info_t info;
    i2c_master_dev_handle_t eeprom;
    bool stable_present;
    bool candidate_present;
    uint8_t candidate_count;
    uint8_t invalid_attempts;
    bool claimed;
} slot_state_t;

typedef struct {
    bool initialized;
    volatile bool running;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t stopped;
    TaskHandle_t task;
    mosaico_module_mgr_config_t config;
    slot_state_t slots[MOSAICO_MODULE_MGR_SLOT_COUNT];
} manager_context_t;

static manager_context_t s_manager;

_Static_assert(sizeof(mosaico_module_mgr_eeprom_v1_t) == MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE, "module EEPROM V1 layout mismatch");

uint16_t mosaico_module_mgr_crc16(const uint8_t *data, size_t len)
{
    if (data == NULL && len != 0) {
        return 0;
    }
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

bool mosaico_module_mgr_eeprom_valid(const mosaico_module_mgr_eeprom_v1_t *image)
{
    if (image == NULL || memcmp(image->magic, MOSAICO_MODULE_MGR_EEPROM_MAGIC, MOSAICO_MODULE_MGR_EEPROM_MAGIC_LEN) != 0) {
        return false;
    }
    const uint16_t desc_crc = mosaico_module_mgr_crc16((const uint8_t *)image, EEPROM_DESC_CRC_OFFSET);
    const uint16_t mfg_crc = mosaico_module_mgr_crc16((const uint8_t *)&image->manufacture_date, EEPROM_MFG_CRC_OFFSET - 0x36U);
    const uint16_t param_crc = mosaico_module_mgr_crc16((const uint8_t *)&image->param_version, EEPROM_PARAM_CRC_OFFSET - 0x40U);
    return image->desc_crc16 == desc_crc && image->mfg_crc16 == mfg_crc && image->param_crc16 == param_crc;
}

const char *mosaico_module_mgr_slot_to_name(mosaico_module_mgr_slot_t slot)
{
    return slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "left" : slot == MOSAICO_MODULE_MGR_SLOT_RIGHT ? "right" : "unknown";
}

const char *mosaico_module_mgr_type_to_name(mosaico_board_type_t type)
{
    switch (type) {
    case MOSAICO_BOARD_TYPE_CAMERA:
        return "Camera";
    case MOSAICO_BOARD_TYPE_SENSOR:
        return "Sensor";
    case MOSAICO_BOARD_TYPE_TOF:
        return "ToF";
    case MOSAICO_BOARD_TYPE_MATRIX_LED:
        return "Matrix LED";
    case MOSAICO_BOARD_TYPE_THERMAL:
        return "Thermal Camera";
    case MOSAICO_BOARD_TYPE_RELAY:
        return "Relay";
    case MOSAICO_BOARD_TYPE_BUTTON_LED:
        return "Button LED";
    case MOSAICO_BOARD_TYPE_CORE:
        return "Core";
    case MOSAICO_BOARD_TYPE_POWER:
        return "Power";
    case MOSAICO_BOARD_TYPE_DOCK:
        return "Dock";
    case MOSAICO_BOARD_TYPE_HANDLE:
        return "Handle";
    case MOSAICO_BOARD_TYPE_BALANCE_CAR:
        return "Balance Car";
    case MOSAICO_BOARD_TYPE_DISPLAY:
        return "Display";
    case MOSAICO_BOARD_TYPE_IO_EXP:
        return "I/O Expansion";
    default:
        return "Unknown";
    }
}

esp_err_t mosaico_module_mgr_slot_from_eeprom_addr(uint8_t eeprom_addr, mosaico_module_mgr_slot_t *out_slot)
{
    ESP_RETURN_ON_FALSE(out_slot != NULL, ESP_ERR_INVALID_ARG, TAG, "slot output is null");
    bsp_subboard_slot_t slot = BSP_SUBBOARD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(bsp_subboard_slot_from_eeprom_addr(eeprom_addr, &slot), TAG, "map EEPROM 0x%02X to slot failed", eeprom_addr);
    *out_slot = (mosaico_module_mgr_slot_t)slot;
    return ESP_OK;
}

static void emit_event(mosaico_module_mgr_event_t event, const mosaico_module_mgr_info_t *info)
{
    if (s_manager.config.event_callback != NULL) {
        s_manager.config.event_callback(event, info, s_manager.config.event_user_data);
    }
}

static esp_err_t attach_eeprom_devices(void)
{
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "subboard I2C bus is not initialized");
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        bsp_subboard_slot_config_t slot = {0};
        ESP_RETURN_ON_ERROR(bsp_subboard_get_slot_config((bsp_subboard_slot_t)i, &slot), TAG, "read slot %u configuration failed", (unsigned)i);
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = slot.eeprom_addr,
            .scl_speed_hz = EEPROM_I2C_FREQ_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &s_manager.slots[i].eeprom), TAG, "attach EEPROM 0x%02X failed", slot.eeprom_addr);
    }
    return ESP_OK;
}

static esp_err_t detach_eeprom_devices(void)
{
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if (s_manager.slots[i].eeprom == NULL) {
            continue;
        }
        esp_err_t ret = ESP_ERR_INVALID_STATE;
        for (unsigned attempt = 0; attempt < EEPROM_DETACH_ATTEMPTS && ret != ESP_OK; ++attempt) {
            ret = i2c_master_bus_rm_device(s_manager.slots[i].eeprom);
            if (ret != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(EEPROM_DETACH_RETRY_MS));
            }
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Detach %s EEPROM failed; handle retained: %s", mosaico_module_mgr_slot_to_name((mosaico_module_mgr_slot_t)i), esp_err_to_name(ret));
            result = ret;
            continue;
        }
        s_manager.slots[i].eeprom = NULL;
    }
    return result;
}

static esp_err_t read_eeprom(mosaico_module_mgr_slot_t slot, mosaico_module_mgr_eeprom_v1_t *image)
{
    const uint8_t address = 0;
    return i2c_master_transmit_receive(s_manager.slots[slot].eeprom, &address, sizeof(address), (uint8_t *)image, sizeof(*image), EEPROM_I2C_TIMEOUT_MS);
}

static void publish_presence(mosaico_module_mgr_slot_t slot, bool present, uint8_t eeprom_addr)
{
    mosaico_module_mgr_info_t event_info = {0};
    mosaico_module_mgr_event_t event = MOSAICO_MODULE_MGR_EVENT_REMOVED;
    unsigned attempt = 0;

    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    slot_state_t *state = &s_manager.slots[slot];
    if (state->claimed) {
        xSemaphoreGive(s_manager.lock);
        return;
    }
    state->info.generation++;
    state->info.eeprom_addr = eeprom_addr;
    memset(&state->info.eeprom, 0, sizeof(state->info.eeprom));
    if (!present) {
        state->info.state = MOSAICO_MODULE_MGR_STATE_EMPTY;
        state->invalid_attempts = 0;
    } else {
        mosaico_module_mgr_eeprom_v1_t image = {0};
        xSemaphoreGive(s_manager.lock);
        const esp_err_t ret = read_eeprom(slot, &image);
        xSemaphoreTake(s_manager.lock, portMAX_DELAY);
        state = &s_manager.slots[slot];
        if (ret == ESP_OK && mosaico_module_mgr_eeprom_valid(&image)) {
            state->info.eeprom = image;
            state->info.state = MOSAICO_MODULE_MGR_STATE_READY;
            state->invalid_attempts = 0;
            event = MOSAICO_MODULE_MGR_EVENT_INSERTED;
        } else {
            state->info.state = MOSAICO_MODULE_MGR_STATE_INVALID;
            attempt = ++state->invalid_attempts;
            event = MOSAICO_MODULE_MGR_EVENT_INVALID;
        }
    }
    state->stable_present = present;
    event_info = state->info;
    xSemaphoreGive(s_manager.lock);

    if (event == MOSAICO_MODULE_MGR_EVENT_INSERTED) {
        ESP_LOGI(TAG, "Module inserted: slot=%s eeprom=0x%02X type=0x%02X id=0x%04X name=%.32s", mosaico_module_mgr_slot_to_name(slot), eeprom_addr, event_info.eeprom.board_type, event_info.eeprom.board_id, event_info.eeprom.board_name);
    } else if (event == MOSAICO_MODULE_MGR_EVENT_INVALID) {
        ESP_LOGW(TAG, "Invalid module EEPROM: slot=%s address=0x%02X attempt=%u/%u", mosaico_module_mgr_slot_to_name(slot), eeprom_addr, attempt, EEPROM_READ_ATTEMPTS);
    } else {
        ESP_LOGI(TAG, "Module removed: slot=%s eeprom=0x%02X", mosaico_module_mgr_slot_to_name(slot), eeprom_addr);
    }
    emit_event(event, &event_info);
}

static bool camera_claim_blocks_discovery(void)
{
    bool blocked = false;
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    const slot_state_t *left = &s_manager.slots[MOSAICO_MODULE_MGR_SLOT_LEFT];
    blocked = left->claimed && left->info.eeprom.board_type == MOSAICO_BOARD_TYPE_CAMERA;
    xSemaphoreGive(s_manager.lock);
    return blocked;
}

static void scan_slot(mosaico_module_mgr_slot_t slot)
{
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    const bool claimed = s_manager.slots[slot].claimed;
    xSemaphoreGive(s_manager.lock);
    if (claimed || camera_claim_blocks_discovery()) {
        return;
    }

    bsp_subboard_slot_config_t slot_config = {0};
    if (bsp_subboard_get_slot_config((bsp_subboard_slot_t)slot, &slot_config) != ESP_OK) {
        return;
    }
    const bool present = i2c_master_probe(bsp_subboard_get_i2c_bus(), slot_config.eeprom_addr, EEPROM_I2C_TIMEOUT_MS) == ESP_OK;
    bool publish = false;
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    slot_state_t *state = &s_manager.slots[slot];
    if (present != state->candidate_present) {
        state->candidate_present = present;
        state->candidate_count = 1;
    } else if (state->candidate_count < s_manager.config.debounce_count) {
        state->candidate_count++;
    }
    if (state->candidate_count >= s_manager.config.debounce_count) {
        publish = present != state->stable_present || (present && state->info.state == MOSAICO_MODULE_MGR_STATE_INVALID && state->invalid_attempts < EEPROM_READ_ATTEMPTS);
    }
    xSemaphoreGive(s_manager.lock);
    if (publish) {
        publish_presence(slot, present, slot_config.eeprom_addr);
    }
}

static void manager_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Module manager started: period=%" PRIu32 " ms debounce=%u", s_manager.config.scan_period_ms, s_manager.config.debounce_count);
    while (s_manager.running) {
        if (!camera_claim_blocks_discovery()) {
            scan_slot(MOSAICO_MODULE_MGR_SLOT_LEFT);
            scan_slot(MOSAICO_MODULE_MGR_SLOT_RIGHT);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_manager.config.scan_period_ms));
    }
    s_manager.task = NULL;
    xSemaphoreGive(s_manager.stopped);
    vTaskDelete(NULL);
}

esp_err_t mosaico_module_mgr_init(const mosaico_module_mgr_config_t *config)
{
    if (s_manager.initialized) {
        return ESP_OK;
    }
    const mosaico_module_mgr_config_t active = config != NULL ? *config : (mosaico_module_mgr_config_t)MOSAICO_MODULE_MGR_DEFAULT_CONFIG();
    ESP_RETURN_ON_FALSE(active.scan_period_ms > 0 && active.debounce_count > 0, ESP_ERR_INVALID_ARG, TAG, "invalid manager timing");
    ESP_RETURN_ON_ERROR(bsp_subboard_init(), TAG, "initialize BSP subboard slots failed");
    s_manager.lock = xSemaphoreCreateMutex();
    s_manager.stopped = xSemaphoreCreateBinary();
    if (s_manager.lock == NULL || s_manager.stopped == NULL) {
        if (s_manager.lock != NULL) {
            vSemaphoreDelete(s_manager.lock);
        }
        if (s_manager.stopped != NULL) {
            vSemaphoreDelete(s_manager.stopped);
        }
        memset(&s_manager, 0, sizeof(s_manager));
        return ESP_ERR_NO_MEM;
    }
    s_manager.config = active;
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        bsp_subboard_slot_config_t slot = {0};
        const esp_err_t ret = bsp_subboard_get_slot_config((bsp_subboard_slot_t)i, &slot);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Read slot %u configuration failed: %s", (unsigned)i, esp_err_to_name(ret));
            vSemaphoreDelete(s_manager.lock);
            vSemaphoreDelete(s_manager.stopped);
            memset(&s_manager, 0, sizeof(s_manager));
            return ret;
        }
        s_manager.slots[i].info.slot = (mosaico_module_mgr_slot_t)i;
        s_manager.slots[i].info.eeprom_addr = slot.eeprom_addr;
    }
    esp_err_t ret = attach_eeprom_devices();
    if (ret != ESP_OK) {
        (void)detach_eeprom_devices();
        vSemaphoreDelete(s_manager.lock);
        vSemaphoreDelete(s_manager.stopped);
        memset(&s_manager, 0, sizeof(s_manager));
        return ret;
    }
    s_manager.running = true;
    s_manager.initialized = true;
    if (xTaskCreate(manager_task, "module_mgr", MANAGER_TASK_STACK_SIZE, NULL, MANAGER_TASK_PRIORITY, &s_manager.task) != pdPASS) {
        s_manager.running = false;
        s_manager.initialized = false;
        (void)detach_eeprom_devices();
        vSemaphoreDelete(s_manager.lock);
        vSemaphoreDelete(s_manager.stopped);
        memset(&s_manager, 0, sizeof(s_manager));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mosaico_module_mgr_deinit(void)
{
    if (!s_manager.initialized) {
        return ESP_OK;
    }
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if (s_manager.slots[i].claimed) {
            xSemaphoreGive(s_manager.lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    s_manager.running = false;
    TaskHandle_t task = s_manager.task;
    xSemaphoreGive(s_manager.lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
        if (xSemaphoreTake(s_manager.stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }
    const esp_err_t detach_ret = detach_eeprom_devices();
    if (detach_ret != ESP_OK) {
        return detach_ret;
    }
    vSemaphoreDelete(s_manager.lock);
    vSemaphoreDelete(s_manager.stopped);
    memset(&s_manager, 0, sizeof(s_manager));
    return ESP_OK;
}

esp_err_t mosaico_module_mgr_get_info(mosaico_module_mgr_slot_t slot, mosaico_module_mgr_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(s_manager.initialized, ESP_ERR_INVALID_STATE, TAG, "module manager is not initialized");
    ESP_RETURN_ON_FALSE(out_info != NULL && slot < MOSAICO_MODULE_MGR_SLOT_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid slot info request");
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    *out_info = s_manager.slots[slot].info;
    xSemaphoreGive(s_manager.lock);
    return ESP_OK;
}

esp_err_t mosaico_module_mgr_find(mosaico_board_type_t type, mosaico_module_mgr_slot_t preferred_slot, mosaico_module_mgr_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(s_manager.initialized, ESP_ERR_INVALID_STATE, TAG, "module manager is not initialized");
    ESP_RETURN_ON_FALSE(out_info != NULL && (preferred_slot == MOSAICO_MODULE_MGR_SLOT_AUTO || preferred_slot < MOSAICO_MODULE_MGR_SLOT_COUNT), ESP_ERR_INVALID_ARG, TAG, "invalid find request");
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        const size_t index = preferred_slot == MOSAICO_MODULE_MGR_SLOT_AUTO ? i : (size_t)preferred_slot;
        const slot_state_t *state = &s_manager.slots[index];
        if ((state->info.state == MOSAICO_MODULE_MGR_STATE_READY || state->info.state == MOSAICO_MODULE_MGR_STATE_CLAIMED) && state->info.eeprom.board_type == (uint8_t)type) {
            *out_info = state->info;
            xSemaphoreGive(s_manager.lock);
            return ESP_OK;
        }
        if (preferred_slot != MOSAICO_MODULE_MGR_SLOT_AUTO) {
            break;
        }
    }
    xSemaphoreGive(s_manager.lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t mosaico_module_mgr_wait_for(mosaico_board_type_t type, mosaico_module_mgr_slot_t preferred_slot, uint32_t timeout_ms, mosaico_module_mgr_info_t *out_info)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    do {
        const esp_err_t ret = mosaico_module_mgr_find(type, preferred_slot, out_info);
        if (ret != ESP_ERR_NOT_FOUND) {
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while ((xTaskGetTickCount() - start) < timeout);
    return ESP_ERR_TIMEOUT;
}

esp_err_t mosaico_module_mgr_claim(mosaico_module_mgr_slot_t slot, mosaico_board_type_t expected_type)
{
    ESP_RETURN_ON_FALSE(s_manager.initialized, ESP_ERR_INVALID_STATE, TAG, "module manager is not initialized");
    ESP_RETURN_ON_FALSE(slot < MOSAICO_MODULE_MGR_SLOT_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid claim slot");
    mosaico_module_mgr_info_t info = {0};
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    slot_state_t *state = &s_manager.slots[slot];
    if (state->claimed || state->info.state != MOSAICO_MODULE_MGR_STATE_READY || state->info.eeprom.board_type != (uint8_t)expected_type) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    state->claimed = true;
    state->info.state = MOSAICO_MODULE_MGR_STATE_CLAIMED;
    state->info.generation++;
    info = state->info;
    xSemaphoreGive(s_manager.lock);
    ESP_LOGI(TAG, "Module claimed: slot=%s type=0x%02X", mosaico_module_mgr_slot_to_name(slot), expected_type);
    emit_event(MOSAICO_MODULE_MGR_EVENT_CLAIMED, &info);
    return ESP_OK;
}

esp_err_t mosaico_module_mgr_claim_unidentified(mosaico_module_mgr_slot_t slot)
{
    ESP_RETURN_ON_FALSE(s_manager.initialized, ESP_ERR_INVALID_STATE, TAG, "module manager is not initialized");
    ESP_RETURN_ON_FALSE(slot < MOSAICO_MODULE_MGR_SLOT_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid claim slot");
    mosaico_module_mgr_info_t info = {0};
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    slot_state_t *state = &s_manager.slots[slot];
    if (state->claimed) {
        xSemaphoreGive(s_manager.lock);
        ESP_LOGE(TAG, "Claim unidentified slot %s failed: already claimed", mosaico_module_mgr_slot_to_name(slot));
        return ESP_ERR_INVALID_STATE;
    }
    state->claimed = true;
    state->info.state = MOSAICO_MODULE_MGR_STATE_CLAIMED;
    state->info.generation++;
    info = state->info;
    xSemaphoreGive(s_manager.lock);
    ESP_LOGW(TAG, "Module claimed without identification: slot=%s", mosaico_module_mgr_slot_to_name(slot));
    emit_event(MOSAICO_MODULE_MGR_EVENT_CLAIMED, &info);
    return ESP_OK;
}

esp_err_t mosaico_module_mgr_release(mosaico_module_mgr_slot_t slot)
{
    ESP_RETURN_ON_FALSE(s_manager.initialized, ESP_ERR_INVALID_STATE, TAG, "module manager is not initialized");
    ESP_RETURN_ON_FALSE(slot < MOSAICO_MODULE_MGR_SLOT_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid release slot");
    mosaico_module_mgr_info_t info = {0};
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    slot_state_t *state = &s_manager.slots[slot];
    if (!state->claimed) {
        xSemaphoreGive(s_manager.lock);
        return ESP_OK;
    }
    state->claimed = false;
    state->info.state = MOSAICO_MODULE_MGR_STATE_READY;
    state->info.generation++;
    state->candidate_count = 0;
    info = state->info;
    xSemaphoreGive(s_manager.lock);
    ESP_LOGI(TAG, "Module released: slot=%s; EEPROM discovery resumed", mosaico_module_mgr_slot_to_name(slot));
    emit_event(MOSAICO_MODULE_MGR_EVENT_RELEASED, &info);
    mosaico_module_mgr_request_rescan();
    return ESP_OK;
}

void mosaico_module_mgr_request_rescan(void)
{
    if (s_manager.initialized && s_manager.task != NULL) {
        xTaskNotifyGive(s_manager.task);
    }
}
