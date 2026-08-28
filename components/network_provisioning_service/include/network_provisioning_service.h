/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_PROVISIONING_SSID_LEN 33U
#define NETWORK_PROVISIONING_IP_LEN 16U
#define NETWORK_PROVISIONING_QR_PAYLOAD_LEN 512U

typedef struct network_provisioning_service_t *
    network_provisioning_service_handle_t;

typedef struct {
    bool sta_connected;
    bool sta_configured;
    bool ap_active;
    char sta_ssid[NETWORK_PROVISIONING_SSID_LEN];
    char sta_ip[NETWORK_PROVISIONING_IP_LEN];
    char ap_ssid[NETWORK_PROVISIONING_SSID_LEN];
    char ap_ip[NETWORK_PROVISIONING_IP_LEN];
    char portal_url[48];
    char ap_join_qr[NETWORK_PROVISIONING_QR_PAYLOAD_LEN];
} network_provisioning_status_t;

typedef void (*network_provisioning_event_cb_t)(
    network_provisioning_service_handle_t handle,
    const network_provisioning_status_t *status,
    void *user_ctx);

esp_err_t network_provisioning_service_create(
    network_provisioning_service_handle_t *ret_handle);
void network_provisioning_service_delete(
    network_provisioning_service_handle_t handle);

esp_err_t network_provisioning_service_start(
    network_provisioning_service_handle_t handle);
esp_err_t network_provisioning_service_stop(
    network_provisioning_service_handle_t handle);
esp_err_t network_provisioning_service_get_status(
    network_provisioning_service_handle_t handle,
    network_provisioning_status_t *ret_status);
esp_err_t network_provisioning_service_register_cb(
    network_provisioning_service_handle_t handle,
    network_provisioning_event_cb_t callback,
    void *user_ctx);
esp_err_t network_provisioning_service_unregister_cb(
    network_provisioning_service_handle_t handle,
    network_provisioning_event_cb_t callback,
    void *user_ctx);

/** Reloads the persisted Wi-Fi configuration and applies it immediately. */
esp_err_t network_provisioning_service_reload_and_apply(
    network_provisioning_service_handle_t handle);

#ifdef __cplusplus
}
#endif
