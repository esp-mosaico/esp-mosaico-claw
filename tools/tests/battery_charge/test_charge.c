/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "battery_charge_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const uint16_t addresses[] = {0x91FD, 0x9201, 0x92A5};
static uint16_t fields[3], selected, staged;
static uint16_t design, learned;
static unsigned security, keys, created, deleted, entered, exited, reinitialized, seals, commits;
static bool cfg, deny_full, bad_echo, fail_enter_after_delivery, fail_commit, bad_readback;
static bool fail_exit_once, fail_seal, change_after_reinit;
static const void *const gauge = (void *)(uintptr_t)2;

void vTaskDelay(TickType_t ticks) { (void)ticks; }
void esp_rom_delay_us(uint32_t delay) { (void)delay; }
i2c_bus_device_handle_t i2c_bus_device_create(i2c_bus_handle_t bus, uint8_t address, uint32_t frequency)
{ assert(bus && address == 0x55 && frequency == 0); ++created; return (void *)(uintptr_t)1; }
esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device)
{ assert(*device == (void *)(uintptr_t)1); ++deleted; *device = NULL; return ESP_OK; }
static unsigned index_for(uint16_t address)
{ for (unsigned i = 0; i < 3; ++i) if (addresses[i] == address) return i; assert(false); return 0; }
esp_err_t i2c_bus_write_bytes(i2c_bus_device_handle_t device, uint8_t command, size_t count, const uint8_t *data)
{
    assert(device == (void *)(uintptr_t)1);
    if (command == 0) {
        assert(count == 2);
        uint16_t value = data[0] | ((uint16_t)data[1] << 8);
        if (value == 0xFFFF) {
            if (++keys == 2 && !deny_full) security = 1;
        } else if (value == 0x0090) {
            assert(security == 1); ++entered; cfg = true;
            if (fail_enter_after_delivery) return ESP_FAIL;
        } else if (value == 0x0091 || value == 0x0092) {
            ++exited;
            if (fail_exit_once) { fail_exit_once = false; return ESP_FAIL; }
            cfg = false; reinitialized += value == 0x0091;
            if (change_after_reinit && value == 0x0091) fields[1] ^= 1;
        } else assert(false);
    } else if (command == 0x3E) {
        assert(count == 2 || count == 4);
        selected = data[0] | ((uint16_t)data[1] << 8);
        if (count == 4) { assert(cfg && security == 1); staged = ((uint16_t)data[2] << 8) | data[3]; }
    } else if (command == 0x60) {
        assert(count == 2 && data[1] == 6 && cfg && security == 1);
        uint8_t sum = (uint8_t)selected + (uint8_t)(selected >> 8) + (uint8_t)(staged >> 8) + (uint8_t)staged;
        assert(data[0] == (uint8_t)(0xFFU - sum));
        if (fail_commit && selected == 0x92A5) { fail_commit = false; return ESP_FAIL; }
        fields[index_for(selected)] = staged; ++commits;
    } else assert(false);
    return ESP_OK;
}
esp_err_t i2c_bus_read_bytes(i2c_bus_device_handle_t device, uint8_t command, size_t count, uint8_t *data)
{
    assert(device == (void *)(uintptr_t)1);
    if (command == 0x3C || command == 0x12) {
        assert(count == 2); uint16_t value = command == 0x3C ? design : learned;
        data[0] = value; data[1] = value >> 8;
    } else {
        assert(command == 0x3E && count == 4 && security != 3);
        data[0] = selected; data[1] = (selected >> 8) ^ (bad_echo ? 1 : 0);
        uint16_t value = fields[index_for(selected)];
        if (bad_readback && commits && selected == 0x9201) { value ^= 1; bad_readback = false; }
        data[2] = value >> 8; data[3] = value;
    }
    return ESP_OK;
}
esp_err_t bq27220_get_operation_status(bq27220_handle_t handle, operation_status_t *status)
{ assert(handle == gauge); *status = (operation_status_t){.SEC=security, .CFGUPDATE=cfg}; return ESP_OK; }
esp_err_t bq27220_unseal(bq27220_handle_t handle)
{ assert(handle == gauge && security == 3); security = 2; return ESP_OK; }
esp_err_t bq27220_seal(bq27220_handle_t handle)
{
    assert(handle == gauge); ++seals; assert(!cfg);
    if (fail_seal) return ESP_FAIL;
    security = 3; return ESP_OK;
}

static void reset(void)
{
    fields[0] = 4200; fields[1] = fields[2] = 100;
    selected = staged = 0; design = 65; learned = 72; security = 3;
    keys = created = deleted = entered = exited = reinitialized = seals = commits = 0;
    cfg = deny_full = bad_echo = fail_enter_after_delivery = fail_commit = bad_readback = false;
    fail_exit_once = fail_seal = change_after_reinit = false;
}
static esp_err_t apply(const mosaico_battery_charge_config_t *config)
{ return mosaico_battery_apply_charge_config((void *)(uintptr_t)1, (void *)gauge, config); }
static void closed(void) { assert(security == 3 && !cfg && created == deleted && seals == 1); }

int main(void)
{
    const mosaico_battery_charge_config_t unchanged = {4200, 100, 100};
    /* These deliberately different values test transactions, not cell tuning. */
    const mosaico_battery_charge_config_t changed = {4200, 30, 50};
    reset(); uint16_t capacity = 0;
    assert(mosaico_battery_read_learned_capacity((void *)(uintptr_t)1, 65, &capacity) == ESP_OK && capacity == 72);
    assert(created == deleted && commits == 0 && security == 3);
    assert(mosaico_battery_read_learned_capacity((void *)(uintptr_t)1, 80, &capacity) == ESP_ERR_INVALID_STATE);
    learned = UINT16_MAX;
    assert(mosaico_battery_read_learned_capacity((void *)(uintptr_t)1, 65, &capacity) == ESP_ERR_INVALID_STATE);
    puts("PASS learned capacity is retained only for the same battery design");

    reset(); assert(apply(&unchanged) == ESP_OK); closed(); assert(entered == 0 && commits == 0);
    puts("PASS matching charge parameters skip CONFIG UPDATE and all writes");

    reset(); assert(apply(&changed) == ESP_OK); closed();
    assert(fields[0] == 4200 && fields[1] == 30 && fields[2] == 50);
    assert(commits == 2 && entered == 1 && exited == 1 && reinitialized == 1);
    puts("PASS full-access, CFGUPDATE, endian/checksum, readback, exit and reseal");

    reset(); fail_commit = true;
    assert(apply(&changed) == ESP_FAIL); closed();
    assert(fields[0] == 4200 && fields[1] == 100 && fields[2] == 100 && reinitialized == 0);
    reset(); bad_readback = true;
    assert(apply(&changed) == ESP_ERR_INVALID_STATE); closed();
    assert(fields[0] == 4200 && fields[1] == 100 && fields[2] == 100);
    puts("PASS failed writes/readback restore original thresholds before leaving configuration mode");

    reset(); deny_full = true;
    assert(apply(&changed) == ESP_ERR_INVALID_STATE); closed(); assert(!entered && !commits);
    reset(); fail_enter_after_delivery = true;
    assert(apply(&changed) == ESP_FAIL); closed(); assert(exited == 1 && !commits);
    reset(); bad_echo = true;
    assert(apply(&changed) == ESP_ERR_TIMEOUT); closed(); assert(!entered && !commits);
    puts("PASS security denial, ambiguous mode-entry failure and stale data-memory echo fail closed");
    reset(); fail_exit_once = true;
    assert(apply(&changed) == ESP_FAIL); closed(); assert(exited == 2);
    reset(); change_after_reinit = true;
    assert(apply(&changed) == ESP_ERR_INVALID_STATE); closed();
    reset(); fail_seal = true;
    assert(apply(&unchanged) == ESP_FAIL && seals == 1 && created == deleted);
    puts("PASS failed exit, lost post-reinit settings and reseal failure cannot be reported as success");
    reset(); mosaico_battery_charge_config_t invalid = {4200, 0, 100};
    assert(apply(&invalid) == ESP_ERR_INVALID_ARG && !created);
    puts("PASS invalid detection thresholds never access the gauge");
    return 0;
}
