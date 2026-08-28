/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "bmi270.h"
#include "driver/gpio.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_log.h"
#include "gen_board_device_custom.h"
#include "i2c_bus.h"

static const char *TAG = "MOSAICO_IMU";

typedef struct {
    bmi270_handle_t sensor;
    /* Borrowed shared Board Manager bus. Never delete it from this device. */
    i2c_bus_handle_t bus;
} mosaico_imu_handle_t;

static bool bmi270_probe(i2c_bus_handle_t bus, uint8_t addr)
{
    i2c_bus_device_handle_t probe =
        i2c_bus_device_create(bus, addr, 400000);
    if (probe == NULL) return false;

    uint8_t chip_id = 0;
    const esp_err_t err =
        i2c_bus_read_byte(probe, BMI2_CHIP_ID_ADDR, &chip_id);
    (void)i2c_bus_device_delete(&probe);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "no I2C response at 0x%02x: %s", addr,
                 esp_err_to_name(err));
        return false;
    }
    if (chip_id != BMI270_CHIP_ID) {
        ESP_LOGW(TAG, "I2C device at 0x%02x has chip id 0x%02x, not BMI270",
                 addr, chip_id);
        return false;
    }
    return true;
}

static esp_err_t bmi270_configure(bmi270_handle_t sensor)
{
    const uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    struct bmi2_sens_config config[2] = {0};
    config[0].type = BMI2_ACCEL;
    config[1].type = BMI2_GYRO;

    int8_t result = bmi2_set_adv_power_save(BMI2_DISABLE, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "disable advanced power save failed: %d", result);
    result = bmi2_get_sensor_config(config, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "get sensor config failed: %d", result);

    config[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
    config[0].cfg.acc.range = BMI2_ACC_RANGE_2G;
    config[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    config[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    config[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
    config[1].cfg.gyr.range = BMI2_GYR_RANGE_500;
    config[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    config[1].cfg.gyr.noise_perf = BMI2_PERF_OPT_MODE;
    config[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;

    result = bmi2_set_sensor_config(config, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "set sensor config failed: %d", result);
    result = bmi270_sensor_enable(sensors, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "enable accel/gyro failed: %d", result);
    return ESP_OK;
}

static int imu_sensor_init(void *config, int cfg_size, void **device_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && device_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid init arguments");
    ESP_RETURN_ON_FALSE(cfg_size == sizeof(dev_custom_imu_sensor_config_t),
                        ESP_ERR_INVALID_SIZE, TAG, "invalid config size: %d",
                        cfg_size);
    const dev_custom_imu_sensor_config_t *imu_cfg = config;
    ESP_RETURN_ON_FALSE(imu_cfg->chip != NULL &&
                            strcmp(imu_cfg->chip, "bmi270") == 0,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported IMU chip");

    i2c_master_bus_config_t *master_cfg = NULL;
    ESP_RETURN_ON_ERROR(esp_board_periph_get_config(
                            imu_cfg->peripheral_name, (void **)&master_cfg),
                        TAG, "get I2C configuration failed");
    const i2c_config_t bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = master_cfg->sda_io_num,
        .scl_io_num = master_cfg->scl_io_num,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    mosaico_imu_handle_t *handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate IMU handle failed");
    handle->bus = i2c_bus_create(master_cfg->i2c_port, &bus_cfg);
    if (handle->bus == NULL) {
        free(handle);
        return ESP_FAIL;
    }
    uint8_t bmi_addr = (uint8_t)imu_cfg->addr;
    if (!bmi270_probe(handle->bus, bmi_addr)) {
        const uint8_t alternate_addr = bmi_addr == 0x68 ? 0x69 : 0x68;
        if (!bmi270_probe(handle->bus, alternate_addr)) {
            ESP_LOGE(TAG,
                     "BMI270 not detected at 0x%02x or 0x%02x; check power and I2C wiring",
                     bmi_addr, alternate_addr);
            free(handle);
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGW(TAG, "BMI270 configured at 0x%02x but detected at 0x%02x",
                 bmi_addr, alternate_addr);
        bmi_addr = alternate_addr;
    }
    const bmi270_i2c_config_t bmi_cfg = {
        .i2c_handle = handle->bus,
        .i2c_addr = bmi_addr,
    };
    esp_err_t err = bmi270_sensor_create(&bmi_cfg, &handle->sensor);
    if (err == ESP_OK) {
        err = bmi270_configure(handle->sensor);
    }
    if (err != ESP_OK) {
        if (handle->sensor != NULL) {
            (void)bmi270_sensor_del(handle->sensor);
        }
        /* i2c_bus_create() returns the shared bus for this port. Calling
         * i2c_bus_delete() here can strand the upstream bus mutex when other
         * devices still hold references, taking touch/battery/audio down. */
        free(handle);
        return err;
    }
    *device_handle = handle;
    ESP_LOGI(TAG, "BMI270 initialized at 0x%02x", bmi_addr);
    return ESP_OK;
}

static int imu_sensor_deinit(void *device_handle)
{
    mosaico_imu_handle_t *handle = device_handle;
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid IMU handle");
    esp_err_t err = bmi270_sensor_del(handle->sensor);
    free(handle);
    return err;
}

esp_err_t esp_mosaico_imu_read(void *device_handle,
                               struct bmi2_sens_data *sample)
{
    mosaico_imu_handle_t *handle = device_handle;
    ESP_RETURN_ON_FALSE(handle != NULL && handle->sensor != NULL &&
                            sample != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid read arguments");
    memset(sample, 0, sizeof(*sample));
    return bmi2_get_sensor_data(sample, handle->sensor) == BMI2_OK
               ? ESP_OK
               : ESP_FAIL;
}

CUSTOM_DEVICE_IMPLEMENT(imu_sensor, imu_sensor_init, imu_sensor_deinit);
