/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_CHECK_VERSION_LEN      32U
#define UPDATE_CHECK_TITLE_LEN        96U
#define UPDATE_CHECK_SUMMARY_LEN      256U
#define UPDATE_CHECK_PUBLISHED_AT_LEN 40U
#define UPDATE_CHECK_SUBSCRIBER_MAX   4U

typedef enum {
    UPDATE_CHECK_IDLE = 0,
    UPDATE_CHECK_CHECKING,
    UPDATE_CHECK_UP_TO_DATE,
    UPDATE_CHECK_AVAILABLE,
    UPDATE_CHECK_DEVICE_AHEAD,
    UPDATE_CHECK_FAILED,
} update_check_state_t;

typedef struct {
    update_check_state_t state;
    char current_version[UPDATE_CHECK_VERSION_LEN];
    char latest_version[UPDATE_CHECK_VERSION_LEN];
    char title[UPDATE_CHECK_TITLE_LEN];
    char summary[UPDATE_CHECK_SUMMARY_LEN];
    char published_at[UPDATE_CHECK_PUBLISHED_AT_LEN];
    uint32_t sequence;
    esp_err_t last_error;
} update_check_snapshot_t;

typedef struct {
    /** HTTPS URL of the JSON update manifest. Empty keeps checking disabled. */
    const char *manifest_url;
    /** Product identifier expected in the manifest. */
    const char *product;
    /** Authoritative running firmware version, normally esp_app_desc.version. */
    const char *current_version;
    const char *user_agent;
    /** Zero selects 10 seconds. */
    uint32_t timeout_ms;
    /** Zero selects 4 KiB. */
    uint32_t max_body_size;
} update_check_service_config_t;

typedef void (*update_check_event_cb_t)(
    const update_check_snapshot_t *snapshot, void *user_ctx);

/** Initialize the singleton metadata-only update checker. */
esp_err_t update_check_service_init(
    const update_check_service_config_t *config);

/** Start one asynchronous manifest request. */
esp_err_t update_check_service_request(void);

/** Return the latest immutable result. */
esp_err_t update_check_service_get_snapshot(
    update_check_snapshot_t *ret_snapshot);

esp_err_t update_check_service_subscribe(
    update_check_event_cb_t callback, void *user_ctx);
esp_err_t update_check_service_unsubscribe(
    update_check_event_cb_t callback, void *user_ctx);

/** Strict numeric MAJOR.MINOR.PATCH comparison, useful to manifest producers. */
esp_err_t update_check_compare_versions(
    const char *left, const char *right, int *ret_comparison);

#ifdef __cplusplus
}
#endif
