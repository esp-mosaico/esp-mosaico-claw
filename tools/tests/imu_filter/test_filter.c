/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "bmi2.h"
#include "mosaic_imu.h"
#include "mosaic_imu_platform.h"

static mosaic_imu_ops_t ops;
static int64_t now_us = 1000000;
static struct bmi2_sens_data raw;
static esp_err_t read_error;
int64_t esp_timer_get_time(void) { return now_us; }
bool esp_board_manager_check_name(const char *name) { return strcmp(name, "imu_sensor") == 0; }
esp_err_t esp_board_manager_init_device_by_name(const char *name) { (void)name; return ESP_OK; }
esp_err_t esp_board_manager_get_device_handle(const char *name, void **handle)
{ (void)name; *handle = (void *)(uintptr_t)1; return ESP_OK; }
esp_err_t esp_mosaico_imu_read(void *handle, struct bmi2_sens_data *sample)
{ (void)handle; *sample = raw; return read_error; }
esp_err_t mosaic_imu_configure(const mosaic_imu_ops_t *value) { ops = *value; return ESP_OK; }

static float recover(unsigned period_ms)
{
    now_us += 1000000;
    raw = (struct bmi2_sens_data){ .acc = { .y = 8000, .z = 13856 } };
    mosaic_imu_sample_t sample;
    assert(ops.read(&sample, ops.user_ctx) == ESP_OK);
    assert(fabsf(sample.pitch_deg - 30.0f) < 0.01f);
    raw.acc.y = 0;
    raw.acc.z = 16000;
    for (unsigned elapsed = period_ms; elapsed <= 800; elapsed += period_ms) {
        now_us += period_ms * 1000;
        assert(ops.read(&sample, ops.user_ctx) == ESP_OK);
    }
    assert(fabsf(sample.pitch_deg) < 1.6f);
    return sample.pitch_deg;
}

int main(void)
{
    assert(mosaic_imu_platform_init() == ESP_OK);
    const float at_20ms = recover(20);
    const float at_50ms = recover(50);
    const float at_100ms = recover(100);
    assert(fabsf(at_20ms - at_50ms) < 0.01f);
    assert(fabsf(at_50ms - at_100ms) < 0.01f);
    puts("PASS 30-degree residual returns within 1.6 degrees in 800 ms at 20/50/100 ms sampling");

    now_us += 1000000;
    raw = (struct bmi2_sens_data){ .acc = { .z = 16000 }, .gyr = { .z = 655 } };
    mosaic_imu_sample_t sample;
    assert(ops.read(&sample, ops.user_ctx) == ESP_OK && sample.yaw_deg == 0);
    now_us += 50000;
    assert(ops.read(&sample, ops.user_ctx) == ESP_OK);
    assert(sample.yaw_deg > 0.49f && sample.yaw_deg < 0.51f);
    read_error = ESP_FAIL;
    assert(ops.read(&sample, ops.user_ctx) == ESP_FAIL);
    puts("PASS long-gap reset, yaw integration and read error propagation");
    return 0;
}
