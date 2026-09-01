/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_imu_platform.h"

#include <math.h>
#include <stdbool.h>

#include "bmi2.h"
#include "esp_board_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mosaic_capability_contracts.h"
#include "mosaic_device_capabilities.h"

#define RAD_TO_DEG 57.2957795f
#define BMI270_GYRO_500DPS_SCALE 65.536f
#define IMU_COMPLEMENTARY_ALPHA 0.96f
#define IMU_INIT_RETRY_INTERVAL_US (5LL * 1000 * 1000)

static const char *TAG = "mosaic_imu";

typedef struct {
    void *device;
    int64_t timestamp_us;
    float pitch;
    float roll;
    float yaw;
    int64_t next_init_attempt_us;
    esp_err_t init_error;
    bool initialized;
} mosaic_imu_platform_t;

static mosaic_imu_platform_t s_imu;

extern esp_err_t esp_mosaico_imu_read(void *device_handle,
                                      struct bmi2_sens_data *sample);

static esp_err_t get_imu_handle(void **out_handle)
{
    if (s_imu.device == NULL) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us < s_imu.next_init_attempt_us) {
            return s_imu.init_error != ESP_OK ? s_imu.init_error
                                              : ESP_ERR_INVALID_STATE;
        }
        if (!esp_board_manager_check_name("imu_sensor")) {
            return ESP_ERR_NOT_FOUND;
        }
        if (esp_board_manager_get_device_handle("imu_sensor", &s_imu.device) !=
                ESP_OK) {
            esp_err_t err =
                esp_board_manager_init_device_by_name("imu_sensor");
            if (err == ESP_OK) {
                err = esp_board_manager_get_device_handle("imu_sensor",
                                                          &s_imu.device);
            }
            if (err != ESP_OK || s_imu.device == NULL) {
                s_imu.device = NULL;
                s_imu.init_error = err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
                s_imu.next_init_attempt_us =
                    now_us + IMU_INIT_RETRY_INTERVAL_US;
                ESP_LOGW(TAG, "BMI270 unavailable; retry in 5 s: %s",
                         esp_err_to_name(s_imu.init_error));
                return s_imu.init_error;
            }
            s_imu.init_error = ESP_OK;
            s_imu.next_init_attempt_us = 0;
        }
    }
    if (s_imu.device == NULL) return ESP_ERR_INVALID_STATE;
    *out_handle = s_imu.device;
    return ESP_OK;
}

static float wrap_heading(float degrees)
{
    while (degrees >= 360.0f) degrees -= 360.0f;
    while (degrees < 0.0f) degrees += 360.0f;
    return degrees;
}

static esp_err_t imu_read(void *user_ctx, mosaic_cap_orientation_t *out)
{
    (void)user_ctx;
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    void *handle = NULL;
    esp_err_t err = get_imu_handle(&handle);
    if (err != ESP_OK) return err;
    struct bmi2_sens_data raw = {0};
    ESP_RETURN_ON_ERROR(esp_mosaico_imu_read(handle, &raw), TAG,
                        "read BMI270");

    const float ax = raw.acc.x;
    const float ay = raw.acc.y;
    const float az = raw.acc.z;
    const float acc_pitch =
        atan2f(-ax, sqrtf(ay * ay + az * az)) * RAD_TO_DEG;
    const float acc_roll = atan2f(ay, az) * RAD_TO_DEG;
    const int64_t now_us = esp_timer_get_time();
    float dt = (now_us - s_imu.timestamp_us) / 1000000.0f;
    if (!s_imu.initialized || dt <= 0.0f || dt > 0.25f) {
        s_imu.pitch = acc_pitch;
        s_imu.roll = acc_roll;
        s_imu.yaw = 0.0f;
        s_imu.initialized = true;
    } else {
        const float gx = raw.gyr.x / BMI270_GYRO_500DPS_SCALE;
        const float gy = raw.gyr.y / BMI270_GYRO_500DPS_SCALE;
        const float gz = raw.gyr.z / BMI270_GYRO_500DPS_SCALE;
        s_imu.roll = IMU_COMPLEMENTARY_ALPHA * (s_imu.roll + gx * dt) +
                     (1.0f - IMU_COMPLEMENTARY_ALPHA) * acc_roll;
        s_imu.pitch = IMU_COMPLEMENTARY_ALPHA * (s_imu.pitch + gy * dt) +
                      (1.0f - IMU_COMPLEMENTARY_ALPHA) * acc_pitch;
        s_imu.yaw = wrap_heading(s_imu.yaw + gz * dt);
    }
    s_imu.timestamp_us = now_us;
    /* The BMI270 is mounted 90 degrees clockwise relative to the display.
     * Keep the complementary filter in the sensor frame, then rotate its
     * result into the user-facing screen frame. Doing this here keeps the
     * numeric Pitch/Roll values and every UI consumer consistent:
     *
     *   screen pitch =  sensor roll
     *   screen roll  = -sensor pitch
     *
     * Without this transform, tilting the device front/back changes Roll and
     * drives the bubble horizontally. */
    out->pitch_deg = s_imu.roll;
    out->roll_deg = -s_imu.pitch;
    out->yaw_deg = s_imu.yaw;
    return ESP_OK;
}

esp_err_t mosaic_imu_platform_init(void)
{
    mosaic_device_capabilities_set_imu(imu_read, NULL);
    return ESP_OK;
}
