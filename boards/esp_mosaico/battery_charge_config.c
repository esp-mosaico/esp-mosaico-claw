/* SPDX-License-Identifier: Apache-2.0 */
#include "battery_charge_config.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* BQ27220 TRM SLUUBD4A, sections 2, 3.1, 3.4 and 4.4.1.
 * RAM CEDV profile 1 matches the profile used by the board's gauge driver. */
#define GAUGE_ADDRESS 0x55
#define COMMAND_CONTROL 0x00
#define COMMAND_FCC 0x12
#define COMMAND_DESIGN_CAPACITY 0x3C
#define COMMAND_DATA 0x3E
#define COMMAND_CHECKSUM 0x60
#define FULL_ACCESS_KEY 0xFFFF
#define ENTER_CONFIG 0x0090
#define EXIT_CONFIG_REINIT 0x0091
#define EXIT_CONFIG 0x0092
#define SECURITY_FULL 1U
#define SECURITY_SEALED 3U
#define CHARGE_FIELD_COUNT 3U

static const char *TAG = "mosaico_charge";
static const uint16_t s_addresses[CHARGE_FIELD_COUNT] = {0x91FD, 0x9201, 0x92A5};

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static esp_err_t control(i2c_bus_device_handle_t device, uint16_t command)
{
    uint8_t bytes[] = {(uint8_t)command, (uint8_t)(command >> 8)};
    return i2c_bus_write_bytes(device, COMMAND_CONTROL, sizeof(bytes), bytes);
}

static esp_err_t read_word(i2c_bus_device_handle_t device, uint8_t command, uint16_t *value)
{
    uint8_t bytes[2];
    esp_err_t err = i2c_bus_read_bytes(device, command, sizeof(bytes), bytes);
    if (err == ESP_OK) *value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return err;
}

static esp_err_t read_parameter(i2c_bus_device_handle_t device, uint16_t address, uint16_t *value)
{
    uint8_t select[] = {(uint8_t)address, (uint8_t)(address >> 8)};
    esp_err_t err = i2c_bus_write_bytes(device, COMMAND_DATA, sizeof(select), select);
    if (err != ESP_OK) return err;
    for (unsigned retry = 0; retry < 20; ++retry) {
        uint8_t bytes[4];
        esp_rom_delay_us(1000);
        err = i2c_bus_read_bytes(device, COMMAND_DATA, sizeof(bytes), bytes);
        if (err != ESP_OK) return err;
        if (bytes[0] == select[0] && bytes[1] == select[1]) {
            *value = ((uint16_t)bytes[2] << 8) | bytes[3];
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t write_parameter(i2c_bus_device_handle_t device, uint16_t address, uint16_t value)
{
    uint8_t bytes[] = {(uint8_t)address, (uint8_t)(address >> 8),
                       (uint8_t)(value >> 8), (uint8_t)value};
    esp_err_t err = i2c_bus_write_bytes(device, COMMAND_DATA, sizeof(bytes), bytes);
    if (err != ESP_OK) return err;
    delay_ms(10);
    uint8_t sum = 0;
    for (unsigned i = 0; i < sizeof(bytes); ++i) sum += bytes[i];
    uint8_t commit[] = {(uint8_t)(0xFFU - sum), 6};
    err = i2c_bus_write_bytes(device, COMMAND_CHECKSUM, sizeof(commit), commit);
    if (err != ESP_OK) return err;
    delay_ms(10);
    uint16_t actual = 0;
    err = read_parameter(device, address, &actual);
    return err != ESP_OK ? err : actual == value ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t wait_config(bq27220_handle_t gauge, bool expected)
{
    for (unsigned retry = 0; retry < 100; ++retry) {
        operation_status_t status = {0};
        esp_err_t err = bq27220_get_operation_status(gauge, &status);
        if (err != ESP_OK) return err;
        if (status.CFGUPDATE == expected) return ESP_OK;
        delay_ms(20);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t mosaico_battery_read_learned_capacity(
    i2c_bus_handle_t bus, uint16_t design_mah, uint16_t *capacity_mah)
{
    if (bus == NULL || capacity_mah == NULL || design_mah == 0) return ESP_ERR_INVALID_ARG;
    i2c_bus_device_handle_t device = i2c_bus_device_create(bus, GAUGE_ADDRESS, 0);
    if (device == NULL) return ESP_ERR_NO_MEM;
    uint16_t design = 0, learned = 0;
    esp_err_t err = read_word(device, COMMAND_DESIGN_CAPACITY, &design);
    if (err == ESP_OK) err = read_word(device, COMMAND_FCC, &learned);
    if (err == ESP_OK && (design != design_mah || learned == 0 || learned == UINT16_MAX)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) *capacity_mah = learned;
    esp_err_t cleanup = i2c_bus_device_delete(&device);
    return err == ESP_OK ? cleanup : err;
}

esp_err_t mosaico_battery_apply_charge_config(
    i2c_bus_handle_t bus, bq27220_handle_t gauge,
    const mosaico_battery_charge_config_t *config)
{
    if (bus == NULL || gauge == NULL || config == NULL ||
            config->charging_voltage_mv < 2500 || config->charging_voltage_mv > 4600 ||
            config->taper_current_ma == 0 || config->taper_current_ma > 1000 ||
            config->taper_voltage_mv > 1000 ||
            config->taper_voltage_mv >= config->charging_voltage_mv) return ESP_ERR_INVALID_ARG;
    i2c_bus_device_handle_t device = i2c_bus_device_create(bus, GAUGE_ADDRESS, 0);
    if (device == NULL) return ESP_ERR_NO_MEM;
    const uint16_t values[CHARGE_FIELD_COUNT] = {
        config->charging_voltage_mv, config->taper_current_ma, config->taper_voltage_mv};
    uint16_t original[CHARGE_FIELD_COUNT] = {0};
    bool config_requested = false, entered = false, changed = false, writes_started = false;
    operation_status_t status = {0};
    esp_err_t err = bq27220_get_operation_status(gauge, &status);
    if (err != ESP_OK) goto cleanup;
    if (status.SEC != SECURITY_SEALED || status.CFGUPDATE) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    err = bq27220_unseal(gauge);
    if (err != ESP_OK) goto seal;
    for (unsigned i = 0; i < CHARGE_FIELD_COUNT; ++i) {
        err = read_parameter(device, s_addresses[i], &original[i]);
        if (err != ESP_OK) goto seal;
        changed |= original[i] != values[i];
    }
    ESP_LOGI(TAG, "charge profile V=%u->%u mV taper=%u->%u mA margin=%u->%u mV",
             original[0], values[0], original[1], values[1], original[2], values[2]);
    if (!changed) goto seal;
    err = control(device, FULL_ACCESS_KEY);
    if (err != ESP_OK) goto seal;
    delay_ms(10);
    err = control(device, FULL_ACCESS_KEY);
    if (err != ESP_OK) goto seal;
    delay_ms(1000);
    err = bq27220_get_operation_status(gauge, &status);
    if (err != ESP_OK) goto seal;
    if (status.SEC != SECURITY_FULL) { err = ESP_ERR_INVALID_STATE; goto seal; }
    config_requested = true;
    err = control(device, ENTER_CONFIG);
    if (err != ESP_OK) goto leave;
    delay_ms(1100);
    err = wait_config(gauge, true);
    if (err != ESP_OK) goto leave;
    entered = true;
    for (unsigned i = 0; i < CHARGE_FIELD_COUNT; ++i) {
        if (original[i] == values[i]) continue;
        writes_started = true;
        err = write_parameter(device, s_addresses[i], values[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "charge profile write/readback failed at 0x%04x: %s",
                     s_addresses[i], esp_err_to_name(err));
            goto leave;
        }
    }
leave:
    if (err != ESP_OK && entered && writes_started) {
        for (unsigned i = 0; i < CHARGE_FIELD_COUNT; ++i) {
            esp_err_t rollback = write_parameter(device, s_addresses[i], original[i]);
            if (rollback != ESP_OK) {
                ESP_LOGE(TAG, "charge profile rollback failed at 0x%04x: %s",
                         s_addresses[i], esp_err_to_name(rollback));
            }
        }
    }
    if (config_requested) {
        esp_err_t exit = control(device, err == ESP_OK ? EXIT_CONFIG_REINIT : EXIT_CONFIG);
        if (exit == ESP_OK) {
            delay_ms(2000);
            exit = wait_config(gauge, false);
        }
        if (exit != ESP_OK) {
            ESP_LOGE(TAG, "charge profile CONFIG UPDATE exit failed: %s", esp_err_to_name(exit));
            /* Exiting configuration is idempotent. A failed acknowledgement
             * must not prevent a bounded attempt to restore normal gauging. */
            esp_err_t fallback = control(device, EXIT_CONFIG);
            if (fallback == ESP_OK) {
                delay_ms(2000);
                fallback = wait_config(gauge, false);
            }
            if (fallback != ESP_OK) {
                ESP_LOGE(TAG, "charge profile normal-mode recovery failed: %s", esp_err_to_name(fallback));
            }
            if (err == ESP_OK) err = exit;
        }
        if (err == ESP_OK) {
            for (unsigned i = 0; i < CHARGE_FIELD_COUNT; ++i) {
                uint16_t actual = 0;
                err = read_parameter(device, s_addresses[i], &actual);
                if (err == ESP_OK && actual != values[i]) err = ESP_ERR_INVALID_STATE;
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "charge profile verification after reinit failed at 0x%04x: %s",
                             s_addresses[i], esp_err_to_name(err));
                    break;
                }
            }
        }
    }
seal: {
        esp_err_t sealed = bq27220_seal(gauge);
        if (sealed != ESP_OK) {
            ESP_LOGE(TAG, "charge profile reseal failed: %s", esp_err_to_name(sealed));
            if (err == ESP_OK) err = sealed;
        }
    }
cleanup: {
        esp_err_t deleted = i2c_bus_device_delete(&device);
        if (err == ESP_OK) err = deleted;
    }
    return err;
}
