/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_demo.h"

#include "env_binds.h"
#include "imu_binds.h"
#include "tof_binds.h"

enum {
    DEMO_BIND_IMU_BUBBLE_X = 0,
    DEMO_BIND_IMU_BUBBLE_Y = 1,
};

#define IMU_BUBBLE_X_MIN 130
#define IMU_BUBBLE_X_MAX 270
#define IMU_BUBBLE_Y_MIN 90
#define IMU_BUBBLE_Y_MAX 240

static int32_t bubble_axis_rail(int16_t px, int32_t min, int32_t max)
{
    const int32_t span = max - min;
    if (span <= 0) {
        return 0;
    }
    const int32_t clamped = px < min ? min : (px > max ? max : px);
    return ((clamped - min) * 100 + span / 2) / span;
}

typedef struct {
    int16_t bubble_x;
    int16_t bubble_y;
    const char *angle;
    const char *pitch;
    const char *roll;
    const char *yaw;
} mosaic_imu_sample_t;

typedef struct {
    const char *temp;
    const char *humidity;
    const char *hcho;
    const char *tvoc;
} mosaic_env_sample_t;

typedef struct {
    const char *dist;
    const char *status;
} mosaic_tof_sample_t;

static const mosaic_imu_sample_t k_imu_samples[] = {
    { .bubble_x = 202, .bubble_y = 168, .angle = "12°",
      .pitch = "76", .roll = "0", .yaw = "120" },
    { .bubble_x = 248, .bubble_y = 118, .angle = "28°",
      .pitch = "82", .roll = "-6", .yaw = "115" },
    { .bubble_x = 168, .bubble_y = 205, .angle = "-8°",
      .pitch = "70", .roll = "4", .yaw = "125" },
    { .bubble_x = 228, .bubble_y = 198, .angle = "45°",
      .pitch = "88", .roll = "-12", .yaw = "98" },
};

static const mosaic_env_sample_t k_env_samples[] = {
    { .temp = "24.5", .humidity = "76", .hcho = "30", .tvoc = "260" },
    { .temp = "25.2", .humidity = "74", .hcho = "28", .tvoc = "255" },
    { .temp = "23.8", .humidity = "78", .hcho = "32", .tvoc = "270" },
    { .temp = "24.1", .humidity = "75", .hcho = "29", .tvoc = "248" },
};

static const mosaic_tof_sample_t k_tof_samples[] = {
    { .dist = "32.5", .status = "VALID" },
    { .dist = "48.2", .status = "VALID" },
    { .dist = "61.0", .status = "VALID" },
    { .dist = "92.4", .status = "INVALID" },
};

static uint8_t s_frame;

void mosaic_demo_tick(esp_gsp_handle_t ui, mosaic_demo_kind_t kind)
{
    const unsigned count = sizeof(k_imu_samples) / sizeof(k_imu_samples[0]);
    const unsigned idx = s_frame % count;
    ++s_frame;

    switch (kind) {
    case MOSAIC_DEMO_IMU: {
        const mosaic_imu_sample_t *imu = &k_imu_samples[idx];
        (void)esp_gsp_set_value(
            ui, DEMO_BIND_IMU_BUBBLE_X,
            bubble_axis_rail(imu->bubble_x, IMU_BUBBLE_X_MIN,
                             IMU_BUBBLE_X_MAX));
        (void)esp_gsp_set_value(
            ui, DEMO_BIND_IMU_BUBBLE_Y,
            bubble_axis_rail(imu->bubble_y, IMU_BUBBLE_Y_MIN,
                             IMU_BUBBLE_Y_MAX));
        (void)esp_gsp_set_text(ui, GSP_BIND_IMU_ANGLE, imu->angle);
        (void)esp_gsp_set_text(ui, GSP_BIND_IMU_PITCH, imu->pitch);
        (void)esp_gsp_set_text(ui, GSP_BIND_IMU_ROLL, imu->roll);
        (void)esp_gsp_set_text(ui, GSP_BIND_IMU_YAW, imu->yaw);
        break;
    }
    case MOSAIC_DEMO_ENV: {
        const mosaic_env_sample_t *env = &k_env_samples[idx];
        (void)esp_gsp_set_text(ui, GSP_BIND_ENV_TEMP, env->temp);
        (void)esp_gsp_set_text(ui, GSP_BIND_ENV_HUMIDITY, env->humidity);
        (void)esp_gsp_set_text(ui, GSP_BIND_ENV_HCHO, env->hcho);
        (void)esp_gsp_set_text(ui, GSP_BIND_ENV_TVOC, env->tvoc);
        break;
    }
    case MOSAIC_DEMO_TOF: {
        const mosaic_tof_sample_t *tof = &k_tof_samples[idx];
        (void)esp_gsp_set_text(ui, GSP_BIND_TOF_DIST, tof->dist);
        (void)esp_gsp_set_text(ui, GSP_BIND_TOF_STATUS, tof->status);
        break;
    }
    default:
        break;
    }
}
