/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_log.h"
#include "gen_board_device_custom.h"
#include "i2c_bus.h"
#include "bq27220.h"

static const char *TAG = "MOSAICO_BATT";

static int battery_monitor_init(void *config, int cfg_size, void **device_handle)
{
    ESP_LOGI(TAG, "Initializing battery_monitor device");
    dev_custom_battery_monitor_config_t *battery_monitor_cfg = (dev_custom_battery_monitor_config_t *)config;

    ESP_RETURN_ON_FALSE(strcmp(battery_monitor_cfg->chip, "bq27220") == 0, ESP_ERR_INVALID_ARG,
                        TAG, "Unsupported battery monitor chip: %s", battery_monitor_cfg->chip);
    /* Fixed-EDV profile characterized on 80 mAh Mosaico cells at 25 C with
     * the room-low load (about 0.1 A). Keep this in sync with the BSP profile. */
    const uint16_t design_cap = battery_monitor_cfg->design_capacity;
    parameter_cedv_t default_cedv = {
        .full_charge_cap = design_cap,
        .design_cap = design_cap,
        .reserve_cap = 0,
        .near_full = 5,
        .self_discharge_rate = 20,
        .EDV0 = 3000,
        .EDV1 = 3410,
        .EDV2 = 3530,
        .EMF = 3670,
        .C0 = 115,
        .R0 = 968,
        .T0 = 4547,
        .R1 = 4764,
        .TC = 11,
        .C1 = 0,
        .DOD0 = 4147,
        .DOD10 = 4002,
        .DOD20 = 3969,
        .DOD30 = 3938,
        .DOD40 = 3880,
        .DOD50 = 3824,
        .DOD60 = 3794,
        .DOD70 = 3753,
        .DOD80 = 3677,
        .DOD90 = 3574,
        .DOD100 = 3490,
    };

    static const gauging_config_t default_config = {
        .CCT = 1,
        .CSYNC = 0,
        .EDV_CMP = 0,
        .SC = 1,
        .FIXED_EDV0 = 0,
        .FCC_LIM = 1,
        .FC_FOR_VDQ = 1,
        .IGNORE_SD = 1,
        .SME0 = 0,
    };

    i2c_master_bus_config_t *cfg = NULL;
    if (ESP_OK != esp_board_periph_get_config(battery_monitor_cfg->peripheral_name, (void **)&cfg)) {
        ESP_LOGW(TAG, "Failed to get i2c_master config, skipping BQ27220 init");
        return true;
    }
    const i2c_config_t i2c_bus_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = cfg->sda_io_num,
        .scl_io_num = cfg->scl_io_num,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {.clk_speed = 400000},
        .clk_flags = 0,
    };
    i2c_bus_handle_t i2c_bus = i2c_bus_create(cfg->i2c_port, &i2c_bus_conf);

    bq27220_config_t bq27220_cfg = {
        .i2c_bus = i2c_bus,
        .cfg = &default_config,
        .cedv = &default_cedv,
    };

    bq27220_handle_t bq27220_hdl = bq27220_create(&bq27220_cfg);
    if (!bq27220_hdl) {
        ESP_LOGE(TAG, "Failed to initialize BQ27220");
        i2c_bus_delete(&i2c_bus);
        return false;
    }
    *device_handle = (void *)bq27220_hdl;
    ESP_LOGI(TAG, "Battery monitor initialized successfully");
    return 0;
}

static int battery_monitor_deinit(void *device_handle)
{
    ESP_LOGI(TAG, "Deinitializing battery_monitor device");
    bq27220_handle_t bq27220_hdl = (bq27220_handle_t)device_handle;
    bq27220_delete(bq27220_hdl);

    return 0;
}

CUSTOM_DEVICE_IMPLEMENT(battery_monitor, battery_monitor_init, battery_monitor_deinit);
