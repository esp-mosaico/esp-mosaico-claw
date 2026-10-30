/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_camera_roi.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "esp_cam_sensor_types.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "sc101iot_types.h"

#define SC101IOT_PID 0xda4aU
#define ROI_WIDTH 768U
#define ROI_HEIGHT 720U
#define ROI_MAX_REGS 256U

static const char *TAG = "camera_roi";

/* VIDIOC_S_SENSOR_FMT borrows both descriptor and registers. Initialize once
 * under the service's activation fence, then never overwrite or free them,
 * including when native cleanup must be retried. */
static sc101iot_reginfo_t s_roi_regs[ROI_MAX_REGS];
static esp_cam_sensor_format_t s_roi_format;
static const void *s_parent_regs;

static const esp_cam_sensor_format_t *prepare_roi(const esp_cam_sensor_format_t *base)
{
    if (s_parent_regs != NULL && base->regs == s_roi_regs &&
        base->width == ROI_WIDTH && base->height == ROI_HEIGHT &&
        base->format == s_roi_format.format && base->port == s_roi_format.port &&
        base->xclk == s_roi_format.xclk && base->fps == s_roi_format.fps &&
        base->regs_size == s_roi_format.regs_size) {
        return &s_roi_format;
    }
    if (base->width != 1280 || base->height != 720 ||
        base->format != ESP_CAM_SENSOR_PIXFORMAT_YUV422_UYVY ||
        base->port != ESP_CAM_SENSOR_DVP || base->xclk != 20000000 ||
        (base->fps != 15 && base->fps != 25) || base->regs == NULL ||
        base->regs_size <= 0 || (unsigned)base->regs_size > ROI_MAX_REGS) {
        return NULL;
    }
    if (s_parent_regs != NULL) {
        return base->regs == s_parent_regs && base->fps == s_roi_format.fps &&
                       base->xclk == s_roi_format.xclk ? &s_roi_format : NULL;
    }

    const sc101iot_reginfo_t *regs = base->regs;
    uint8_t window[6] = {0};
    size_t positions[6] = {0};
    unsigned counts[6] = {0};
    uint8_t page = 0;
    for (int i = 0; i < base->regs_size; ++i) {
        if (regs[i].reg == 0xf0) {
            page = regs[i].val;
        } else if (page == 1 && regs[i].reg >= 0x70 && regs[i].reg <= 0x75) {
            const unsigned index = regs[i].reg - 0x70;
            window[index] = regs[i].val;
            positions[index] = (size_t)i;
            counts[index]++;
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (counts[i] != 1 || positions[i] != positions[0] + i) {
            return NULL;
        }
    }
    const unsigned x0 = ((window[2] & 0x0fU) << 8) | window[0];
    const unsigned x1 = ((window[2] & 0xf0U) << 4) | window[1];
    const unsigned y0 = ((window[5] & 0x0fU) << 8) | window[3];
    const unsigned y1 = ((window[5] & 0xf0U) << 4) | window[4];
    /* Keep the real two-pixel origin. Mosaic's existing PPA crop starts at
     * x=256 within this 1280x720 frame, hence sensor x=[258,1026). */
    if (x0 != 2 || x1 != 1282 || y0 != 2 || y1 != 722) {
        return NULL;
    }
    memcpy(s_roi_regs, regs, (size_t)base->regs_size * sizeof(*regs));
    s_roi_regs[positions[2]].val = 0x41;
    s_roi_format = *base;
    s_roi_format.name = "Mosaico_SC101IOT_UYVY_768x720_equivalent";
    s_roi_format.width = ROI_WIDTH;
    s_roi_format.height = ROI_HEIGHT;
    s_roi_format.regs = s_roi_regs;
    s_parent_regs = base->regs;
    return &s_roi_format;
}

esp_err_t mosaico_camera_configure_equivalent_roi(mosaico_camera_handle_t camera)
{
    mosaico_camera_pipeline_stats_t stats = {0};
    esp_err_t ret = mosaico_camera_get_pipeline_stats(camera, &stats);
    if (ret != ESP_OK) {
        return ret;
    }
    if (stats.streaming || stats.buffer_count != 0 || stats.outstanding_count != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stats.power_down) {
        ret = mosaico_camera_set_power_down(camera, false);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    const int fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        ret = ESP_FAIL;
        goto restore_power;
    }
    esp_cam_sensor_id_t id = {0};
    struct v4l2_ext_control control = {
        .id = ESP_CAM_SENSOR_IOC_G_CHIP_ID,
        .size = sizeof(id),
        .p_u8 = (uint8_t *)&id,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL,
        .count = 1,
        .controls = &control,
    };
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        ret = ESP_FAIL;
        goto close_device;
    }
    if (id.pid != SC101IOT_PID) {
        goto close_device;
    }
    esp_cam_sensor_format_t base = {0};
    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &base) != 0) {
        ret = ESP_FAIL;
        goto close_device;
    }
    const esp_cam_sensor_format_t *roi = prepare_roi(&base);
    if (roi == NULL) {
        ESP_LOGW(TAG, "Unrecognized SC101IOT mode; retaining native format");
        goto close_device;
    }
    if (ioctl(fd, VIDIOC_S_SENSOR_FMT, roi) != 0) {
        ret = ESP_FAIL;
        goto close_device;
    }
    ESP_LOGI(TAG, "Equivalent ROI: %ux%u UYVY, %u fps",
             (unsigned)roi->width, (unsigned)roi->height, (unsigned)roi->fps);

close_device:
    if (close(fd) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
restore_power:
    if (stats.power_down) {
        const esp_err_t power_ret = mosaico_camera_set_power_down(camera, true);
        if (ret == ESP_OK) {
            ret = power_ret;
        }
    }
    return ret;
}
