/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#ifdef ESP_PLATFORM

#include "esp_log.h"

#define GSP_LOGD(tag, format, ...) ESP_LOGD(tag, format, ##__VA_ARGS__)
#define GSP_LOGI(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#define GSP_LOGW(tag, format, ...) ESP_LOGW(tag, format, ##__VA_ARGS__)
#define GSP_LOGE(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)

#else

#include <stdarg.h>
#include <stdio.h>

static inline void gsp_port_log_error(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "E (%s): ", tag);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static inline void gsp_port_log_info(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "I (%s): ", tag);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static inline void gsp_port_log_debug(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

static inline void gsp_port_log_warn(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "W (%s): ", tag);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

#define GSP_LOGD(tag, format, ...) gsp_port_log_debug(tag, format, ##__VA_ARGS__)
#define GSP_LOGI(tag, format, ...) gsp_port_log_info(tag, format, ##__VA_ARGS__)
#define GSP_LOGW(tag, format, ...) gsp_port_log_warn(tag, format, ##__VA_ARGS__)
#define GSP_LOGE(tag, format, ...) gsp_port_log_error(tag, format, ##__VA_ARGS__)

#endif
