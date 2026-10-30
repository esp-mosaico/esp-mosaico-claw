#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2c_bus_handle_t;
typedef void *i2c_bus_device_handle_t;
i2c_bus_device_handle_t i2c_bus_device_create(i2c_bus_handle_t bus, uint8_t address, uint32_t frequency);
esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device);
esp_err_t i2c_bus_write_bytes(i2c_bus_device_handle_t device, uint8_t command, size_t count, const uint8_t *data);
esp_err_t i2c_bus_read_bytes(i2c_bus_device_handle_t device, uint8_t command, size_t count, uint8_t *data);
