/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "update_check_service.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

static esp_err_t parse_version_component(
    const char **cursor, uint32_t *ret_value, bool final)
{
    if (cursor == NULL || *cursor == NULL || ret_value == NULL ||
            **cursor < '0' || **cursor > '9') {
        return ESP_ERR_INVALID_ARG;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(*cursor, &end, 10);
    if (errno == ERANGE || value > UINT32_MAX || end == *cursor) {
        return ESP_ERR_INVALID_ARG;
    }
    if (final ? *end != '\0' : *end != '.') {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_value = (uint32_t)value;
    *cursor = final ? end : end + 1;
    return ESP_OK;
}

esp_err_t update_check_compare_versions(
    const char *left, const char *right, int *ret_comparison)
{
    if (left == NULL || right == NULL || ret_comparison == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t left_parts[3] = {0};
    uint32_t right_parts[3] = {0};
    const char *left_cursor = left;
    const char *right_cursor = right;
    for (size_t index = 0; index < 3U; ++index) {
        const bool final = index == 2U;
        if (parse_version_component(
                &left_cursor, &left_parts[index], final) != ESP_OK ||
                parse_version_component(
                &right_cursor, &right_parts[index], final) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    *ret_comparison = 0;
    for (size_t index = 0; index < 3U; ++index) {
        if (left_parts[index] != right_parts[index]) {
            *ret_comparison = left_parts[index] < right_parts[index] ? -1 : 1;
            break;
        }
    }
    return ESP_OK;
}
