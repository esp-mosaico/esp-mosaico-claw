/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imu_binds.h"
#include "imu_objects.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_imu.h"

#include "mosaic_demo.h"

#define IMU_BUBBLE_X_MIN 130
#define IMU_BUBBLE_X_MAX 270
#define IMU_BUBBLE_Y_MIN 90
#define IMU_BUBBLE_Y_MAX 240

static mosaic_imu_ops_t s_imu_ops;

static int32_t imu_axis_position(float degrees, bool invert,
                                 int32_t minimum, int32_t maximum)
{
    float value = invert ? -degrees : degrees;
    if (value < -30.0f) value = -30.0f;
    if (value > 30.0f) value = 30.0f;
    return minimum +
           (int32_t)((value + 30.0f) * (maximum - minimum) / 60.0f + 0.5f);
}

esp_err_t mosaic_imu_configure(const mosaic_imu_ops_t *ops)
{
    if (ops != NULL && ops->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ops == NULL) {
        memset(&s_imu_ops, 0, sizeof(s_imu_ops));
    } else {
        s_imu_ops = *ops;
    }
    return ESP_OK;
}

static void imu_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    if (s_imu_ops.read == NULL) {
        mosaic_demo_tick(ui, MOSAIC_DEMO_IMU);
        return;
    }
    mosaic_imu_sample_t sample = {0};
    if (s_imu_ops.read(&sample, s_imu_ops.user_ctx) != ESP_OK) {
        return;
    }
    char angle[16];
    char pitch[16];
    char roll[16];
    char yaw[16];
    const float tilt = sqrtf(sample.pitch_deg * sample.pitch_deg +
                             sample.roll_deg * sample.roll_deg);
    snprintf(angle, sizeof(angle), "%.0f°", tilt);
    snprintf(pitch, sizeof(pitch), "%.0f", sample.pitch_deg);
    snprintf(roll, sizeof(roll), "%.0f", sample.roll_deg);
    snprintf(yaw, sizeof(yaw), "%.0f", sample.yaw_deg);
    const int32_t bubble_x =
        imu_axis_position(sample.roll_deg, false, IMU_BUBBLE_X_MIN,
                          IMU_BUBBLE_X_MAX);
    const int32_t bubble_y =
        imu_axis_position(sample.pitch_deg, true, IMU_BUBBLE_Y_MIN,
                          IMU_BUBBLE_Y_MAX);
    (void)gsp_imu_imu_bubble_set_position(ui, bubble_x, bubble_y);
    (void)esp_gsp_set_text(ui, GSP_BIND_IMU_ANGLE, angle);
    (void)esp_gsp_set_text(ui, GSP_BIND_IMU_PITCH, pitch);
    (void)esp_gsp_set_text(ui, GSP_BIND_IMU_ROLL, roll);
    (void)esp_gsp_set_text(ui, GSP_BIND_IMU_YAW, yaw);
}

static void imu_started(esp_gsp_handle_t ui)
{
    imu_tick(ui, NULL);
    (void)esp_gsp_timer_create(ui, s_imu_ops.read != NULL ? 50 : 800,
                               imu_tick, NULL);
}

const mosaic_app_descriptor_t mosaic_imu_app = {
    .id = 2,
    .launch_action = GSP_ACT_ID_APP_IMU,
    .name = "imu",
    .title = "IMU",
    .directory = &gsp_obj_directory_imu,
    .disable_swipe = true,
    .on_started = imu_started,
};
