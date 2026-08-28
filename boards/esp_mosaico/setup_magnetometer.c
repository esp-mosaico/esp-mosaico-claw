/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gen_board_device_custom.h"
#include "bmm150.h"
#include "bmm150_defs.h"

static const char *TAG = "MOSAICO_MAG";

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t addr;
    uint8_t peripheral_count;
    const char *peripheral_name;
} magnetometer_config_t;

typedef struct {
    struct bmm150_dev dev;
    i2c_master_dev_handle_t i2c_dev;
    uint8_t i2c_addr;
} magnetometer_handle_t;

static esp_err_t magnetometer_normalize_addr(int addr, uint8_t *out_addr)
{
    if (addr >= BMM150_DEFAULT_I2C_ADDRESS && addr <= BMM150_I2C_ADDRESS_CSB_HIGH_SDO_HIGH) {
        *out_addr = (uint8_t)addr;
        return ESP_OK;
    }

    // The board YAML uses 8-bit I2C notation for the four BMM150 addresses.
    if ((addr & 0x01) == 0) {
        int shifted_addr = addr >> 1;
        if (shifted_addr >= BMM150_DEFAULT_I2C_ADDRESS && shifted_addr <= BMM150_I2C_ADDRESS_CSB_HIGH_SDO_HIGH) {
            *out_addr = (uint8_t)shifted_addr;
            return ESP_OK;
        }
    }

    return ESP_ERR_INVALID_ARG;
}

static BMM150_INTF_RET_TYPE magnetometer_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    magnetometer_handle_t *handle = (magnetometer_handle_t *)intf_ptr;
    if (handle == NULL || handle->i2c_dev == NULL || reg_data == NULL || length == 0) {
        return BMM150_E_COM_FAIL;
    }

    esp_err_t ret = i2c_master_transmit_receive(handle->i2c_dev, &reg_addr, 1, reg_data, length, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BMM150 read reg 0x%02X at addr 0x%02X failed: %s", reg_addr, handle->i2c_addr, esp_err_to_name(ret));
        return BMM150_E_COM_FAIL;
    }
    return BMM150_INTF_RET_SUCCESS;
}

static BMM150_INTF_RET_TYPE magnetometer_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    magnetometer_handle_t *handle = (magnetometer_handle_t *)intf_ptr;
    if (handle == NULL || handle->i2c_dev == NULL || reg_data == NULL || length == 0) {
        return BMM150_E_COM_FAIL;
    }

    const size_t write_len = (size_t)length + 1;
    uint8_t stack_buf[17];
    uint8_t *buf = stack_buf;
    if (write_len > sizeof(stack_buf)) {
        buf = malloc(write_len);
        if (buf == NULL) {
            ESP_LOGE(TAG, "failed to allocate BMM150 write buffer, len=%u", (unsigned int)write_len);
            return BMM150_E_COM_FAIL;
        }
    }

    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, length);
    esp_err_t ret = i2c_master_transmit(handle->i2c_dev, buf, write_len, -1);
    if (buf != stack_buf) {
        free(buf);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BMM150 write reg 0x%02X at addr 0x%02X failed: %s", reg_addr, handle->i2c_addr, esp_err_to_name(ret));
        return BMM150_E_COM_FAIL;
    }
    return BMM150_INTF_RET_SUCCESS;
}

static void magnetometer_delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_us < 1000) {
        esp_rom_delay_us(period_us);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999) / 1000));
    }
}

static esp_err_t magnetometer_configure_runtime(magnetometer_handle_t *handle)
{
    struct bmm150_settings settings = {0};
    settings.preset_mode = BMM150_PRESETMODE_REGULAR;

    int8_t rslt = bmm150_set_presetmode(&settings, &handle->dev);
    if (rslt != BMM150_OK) {
        ESP_LOGE(TAG, "BMM150 set preset mode failed at addr 0x%02X: %d", handle->i2c_addr, rslt);
        return ESP_FAIL;
    }

    settings.pwr_mode = BMM150_POWERMODE_NORMAL;
    rslt = bmm150_set_op_mode(&settings, &handle->dev);
    if (rslt != BMM150_OK) {
        ESP_LOGE(TAG, "BMM150 set normal mode failed at addr 0x%02X: %d", handle->i2c_addr, rslt);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static int magnetometer_init_common(const magnetometer_config_t *cfg, void **device_handle)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && device_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid magnetometer init arguments");
    ESP_RETURN_ON_FALSE(cfg->chip != NULL && strcmp(cfg->chip, "bmm150") == 0, ESP_ERR_INVALID_ARG, TAG,
                        "unsupported magnetometer chip: %s", cfg->chip ? cfg->chip : "(null)");
    ESP_RETURN_ON_FALSE(cfg->peripheral_name != NULL, ESP_ERR_INVALID_ARG, TAG, "magnetometer I2C peripheral is NULL");

    *device_handle = NULL;
    uint8_t i2c_addr = 0;
    esp_err_t ret = magnetometer_normalize_addr(cfg->addr, &i2c_addr);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid BMM150 address 0x%02X for %s", cfg->addr, cfg->name);

    i2c_master_bus_handle_t i2c_bus = NULL;
    ret = esp_board_periph_get_handle(cfg->peripheral_name, (void **)&i2c_bus);
    ESP_RETURN_ON_ERROR(ret, TAG, "get magnetometer I2C peripheral '%s' failed: %s", cfg->peripheral_name, esp_err_to_name(ret));
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "magnetometer I2C peripheral '%s' handle is NULL", cfg->peripheral_name);

    magnetometer_handle_t *handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG, "failed to allocate magnetometer handle");
    handle->i2c_addr = i2c_addr;

    const i2c_device_config_t i2c_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = 400000,
    };
    ret = i2c_master_bus_add_device(i2c_bus, &i2c_dev_cfg, &handle->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add BMM150 I2C device %s at addr 0x%02X failed: %s", cfg->name, i2c_addr, esp_err_to_name(ret));
        free(handle);
        return ret;
    }

    handle->dev.intf = BMM150_I2C_INTF;
    handle->dev.intf_ptr = handle;
    handle->dev.read = magnetometer_i2c_read;
    handle->dev.write = magnetometer_i2c_write;
    handle->dev.delay_us = magnetometer_delay_us;

    int8_t rslt = bmm150_init(&handle->dev);
    if (rslt != BMM150_OK || handle->dev.chip_id != BMM150_CHIP_ID) {
        ESP_LOGE(TAG, "BMM150 init failed for %s at addr 0x%02X: rslt=%d chip_id=0x%02X", cfg->name, i2c_addr, rslt, handle->dev.chip_id);
        esp_err_t rm_ret = i2c_master_bus_rm_device(handle->i2c_dev);
        if (rm_ret != ESP_OK) {
            ESP_LOGE(TAG, "remove failed BMM150 I2C device %s failed: %s", cfg->name, esp_err_to_name(rm_ret));
        }
        free(handle);
        return ESP_ERR_NOT_FOUND;
    }

    ret = magnetometer_configure_runtime(handle);
    if (ret != ESP_OK) {
        esp_err_t rm_ret = i2c_master_bus_rm_device(handle->i2c_dev);
        if (rm_ret != ESP_OK) {
            ESP_LOGE(TAG, "remove failed BMM150 I2C device %s after config failed: %s", cfg->name, esp_err_to_name(rm_ret));
        }
        free(handle);
        return ret;
    }

    *device_handle = handle;
    ESP_LOGI(TAG, "BMM150 magnetometer %s initialized: yaml_addr=0x%02X i2c_addr=0x%02X", cfg->name, cfg->addr, i2c_addr);
    return ESP_OK;
}

static int magnetometer_deinit_common(void *device_handle)
{
    magnetometer_handle_t *handle = (magnetometer_handle_t *)device_handle;
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid magnetometer handle");

    struct bmm150_settings settings = {0};
    settings.pwr_mode = BMM150_POWERMODE_SUSPEND;
    int8_t rslt = bmm150_set_op_mode(&settings, &handle->dev);
    if (rslt != BMM150_OK) {
        ESP_LOGE(TAG, "BMM150 suspend failed at addr 0x%02X: %d", handle->i2c_addr, rslt);
    }

    esp_err_t ret = i2c_master_bus_rm_device(handle->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "remove BMM150 I2C device at addr 0x%02X failed: %s", handle->i2c_addr, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BMM150 magnetometer deinitialized: addr=0x%02X", handle->i2c_addr);
    free(handle);
    return rslt == BMM150_OK ? ESP_OK : ESP_FAIL;
}

static int magnetometer_1_init(void *config, int cfg_size, void **device_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && cfg_size == sizeof(dev_custom_magnetometer_1_config_t), ESP_ERR_INVALID_SIZE,
                        TAG, "invalid magnetometer_1 config size: %d", cfg_size);
    return magnetometer_init_common((const magnetometer_config_t *)config, device_handle);
}

static int magnetometer_2_init(void *config, int cfg_size, void **device_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && cfg_size == sizeof(dev_custom_magnetometer_2_config_t), ESP_ERR_INVALID_SIZE,
                        TAG, "invalid magnetometer_2 config size: %d", cfg_size);
    return magnetometer_init_common((const magnetometer_config_t *)config, device_handle);
}

CUSTOM_DEVICE_IMPLEMENT(magnetometer_1, magnetometer_1_init, magnetometer_deinit_common);
CUSTOM_DEVICE_IMPLEMENT(magnetometer_2, magnetometer_2_init, magnetometer_deinit_common);
