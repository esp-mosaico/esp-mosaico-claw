/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "network_provisioning_service.h"

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"

#define NETWORK_PROVISIONING_MAX_CALLBACKS 4U

typedef struct {
    network_provisioning_event_cb_t callback;
    void *user_ctx;
} network_provisioning_observer_t;

struct network_provisioning_service_t {
    SemaphoreHandle_t lock;
    network_provisioning_status_t status;
    network_provisioning_observer_t
        observers[NETWORK_PROVISIONING_MAX_CALLBACKS];
    bool started;
};

static const char *TAG = "network_provisioning";

static size_t network_provisioning_qr_escape(
    char *output,
    size_t output_size,
    const char *input)
{
    size_t written = 0;
    if (output_size == 0) {
        return 0;
    }
    for (const char *cursor = input != NULL ? input : "";
            *cursor != '\0' && written + 1 < output_size;
            ++cursor) {
        if ((*cursor == '\\' || *cursor == ';' ||
                *cursor == ',' || *cursor == ':') &&
                written + 2 < output_size) {
            output[written++] = '\\';
        }
        output[written++] = *cursor;
    }
    output[written] = '\0';
    return written;
}

static void network_provisioning_build_qr(
    network_provisioning_status_t *status,
    const char *password)
{
    const size_t escaped_ssid_size =
        NETWORK_PROVISIONING_SSID_LEN * 2U;
    const size_t escaped_password_size =
        APP_CONFIG_STR_LEN * 2U;
    char *escaped = calloc(
        1, escaped_ssid_size + escaped_password_size);

    status->ap_join_qr[0] = '\0';
    if (!status->ap_active || status->ap_ssid[0] == '\0') {
        return;
    }
    if (escaped == NULL) {
        ESP_LOGE(TAG, "allocate Wi-Fi QR payload failed");
        return;
    }
    char *escaped_ssid = escaped;
    char *escaped_password = escaped + escaped_ssid_size;
    network_provisioning_qr_escape(
        escaped_ssid, escaped_ssid_size, status->ap_ssid);
    network_provisioning_qr_escape(
        escaped_password, escaped_password_size, password);
    if (password != NULL && password[0] != '\0') {
        snprintf(status->ap_join_qr,
                 sizeof(status->ap_join_qr),
                 "WIFI:T:WPA;S:%s;P:%s;;",
                 escaped_ssid,
                 escaped_password);
    } else {
        snprintf(status->ap_join_qr,
                 sizeof(status->ap_join_qr),
                 "WIFI:T:nopass;S:%s;;",
                 escaped_ssid);
    }
    free(escaped);
}

static esp_err_t network_provisioning_read_status(
    network_provisioning_status_t *ret_status)
{
    app_config_t *config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate config snapshot failed");

    esp_err_t err = app_config_load(config);
    if (err != ESP_OK) {
        free(config);
        return err;
    }

    wifi_manager_status_t wifi = {0};
    wifi_manager_get_status(&wifi);
    memset(ret_status, 0, sizeof(*ret_status));
    ret_status->sta_connected = wifi.sta_connected;
    ret_status->sta_configured = wifi.sta_configured;
    ret_status->ap_active = wifi.ap_active;
    strlcpy(ret_status->sta_ssid,
            config->wifi_ssid,
            sizeof(ret_status->sta_ssid));
    strlcpy(ret_status->sta_ip,
            wifi.sta_ip != NULL ? wifi.sta_ip : "",
            sizeof(ret_status->sta_ip));
    strlcpy(ret_status->ap_ssid,
            wifi.ap_ssid != NULL ? wifi.ap_ssid : "",
            sizeof(ret_status->ap_ssid));
    strlcpy(ret_status->ap_ip,
            wifi.ap_ip != NULL ? wifi.ap_ip : "",
            sizeof(ret_status->ap_ip));
    if (ret_status->ap_ip[0] != '\0') {
        snprintf(ret_status->portal_url,
                 sizeof(ret_status->portal_url),
                 "http://%s/", ret_status->ap_ip);
    }
    network_provisioning_build_qr(
        ret_status, config->ap_password);
    free(config);
    return ESP_OK;
}

static void network_provisioning_publish(
    network_provisioning_service_handle_t handle,
    const network_provisioning_status_t *status)
{
    network_provisioning_observer_t
        observers[NETWORK_PROVISIONING_MAX_CALLBACKS] = {0};

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    handle->status = *status;
    memcpy(observers, handle->observers, sizeof(observers));
    xSemaphoreGive(handle->lock);

    for (size_t i = 0; i < NETWORK_PROVISIONING_MAX_CALLBACKS; ++i) {
        if (observers[i].callback != NULL) {
            observers[i].callback(
                handle, status, observers[i].user_ctx);
        }
    }
}

static void network_provisioning_wifi_cb(
    bool connected,
    void *user_ctx)
{
    (void)connected;
    network_provisioning_service_handle_t handle =
        (network_provisioning_service_handle_t)user_ctx;
    network_provisioning_status_t *status =
        calloc(1, sizeof(*status));
    if (status == NULL) {
        ESP_LOGE(TAG, "allocate network status failed");
        return;
    }
    if (network_provisioning_read_status(status) == ESP_OK) {
        network_provisioning_publish(handle, status);
    }
    free(status);
}

esp_err_t network_provisioning_service_create(
    network_provisioning_service_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(ret_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "output handle missing");
    *ret_handle = NULL;

    network_provisioning_service_handle_t handle =
        calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate service failed");
    handle->lock = xSemaphoreCreateMutex();
    if (handle->lock == NULL) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    *ret_handle = handle;
    return ESP_OK;
}

void network_provisioning_service_delete(
    network_provisioning_service_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    (void)network_provisioning_service_stop(handle);
    vSemaphoreDelete(handle->lock);
    free(handle);
}

esp_err_t network_provisioning_service_start(
    network_provisioning_service_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    if (handle->started) {
        return ESP_OK;
    }
    handle->started = true;
    esp_err_t err = wifi_manager_register_state_callback(
                        network_provisioning_wifi_cb, handle);
    if (err != ESP_OK) {
        handle->started = false;
        return err;
    }
    network_provisioning_wifi_cb(false, handle);
    return ESP_OK;
}

esp_err_t network_provisioning_service_stop(
    network_provisioning_service_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    if (!handle->started) {
        return ESP_OK;
    }
    handle->started = false;
    return wifi_manager_register_state_callback(NULL, NULL);
}

esp_err_t network_provisioning_service_get_status(
    network_provisioning_service_handle_t handle,
    network_provisioning_status_t *ret_status)
{
    ESP_RETURN_ON_FALSE(handle != NULL && ret_status != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "status arguments invalid");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "state lock failed");
    *ret_status = handle->status;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t network_provisioning_service_register_cb(
    network_provisioning_service_handle_t handle,
    network_provisioning_event_cb_t callback,
    void *user_ctx)
{
    ESP_RETURN_ON_FALSE(handle != NULL && callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "callback arguments invalid");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "state lock failed");

    esp_err_t err = ESP_ERR_NO_MEM;
    size_t registered_index = NETWORK_PROVISIONING_MAX_CALLBACKS;
    for (size_t i = 0; i < NETWORK_PROVISIONING_MAX_CALLBACKS; ++i) {
        if (handle->observers[i].callback == callback &&
                handle->observers[i].user_ctx == user_ctx) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
    }
    for (size_t i = 0;
            err == ESP_ERR_NO_MEM &&
            i < NETWORK_PROVISIONING_MAX_CALLBACKS; ++i) {
        if (handle->observers[i].callback == NULL) {
            handle->observers[i] = (network_provisioning_observer_t) {
                .callback = callback,
                .user_ctx = user_ctx,
            };
            registered_index = i;
            err = ESP_OK;
            break;
        }
    }
    network_provisioning_status_t *status =
        malloc(sizeof(*status));
    if (status != NULL) {
        *status = handle->status;
    } else {
        err = ESP_ERR_NO_MEM;
        if (registered_index < NETWORK_PROVISIONING_MAX_CALLBACKS) {
            memset(&handle->observers[registered_index], 0,
                   sizeof(handle->observers[registered_index]));
        }
    }
    xSemaphoreGive(handle->lock);
    if (err == ESP_OK) {
        callback(handle, status, user_ctx);
    }
    free(status);
    return err;
}

esp_err_t network_provisioning_service_unregister_cb(
    network_provisioning_service_handle_t handle,
    network_provisioning_event_cb_t callback,
    void *user_ctx)
{
    ESP_RETURN_ON_FALSE(handle != NULL && callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "callback arguments invalid");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "state lock failed");

    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < NETWORK_PROVISIONING_MAX_CALLBACKS; ++i) {
        if (handle->observers[i].callback == callback &&
                handle->observers[i].user_ctx == user_ctx) {
            memset(&handle->observers[i], 0,
                   sizeof(handle->observers[i]));
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t network_provisioning_service_reload_and_apply(
    network_provisioning_service_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    app_config_t *config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate config snapshot failed");

    esp_err_t err = app_config_load(config);
    if (err == ESP_OK) {
        err = wifi_manager_apply_sta_config(
                  &(wifi_manager_config_t) {
                      .sta_ssid = config->wifi_ssid,
                      .sta_password = config->wifi_password,
                      .ap_ssid = config->ap_ssid[0]
                          ? config->ap_ssid : NULL,
                      .ap_password = config->ap_password[0]
                          ? config->ap_password : NULL,
                      .ap_behavior = config->ap_behavior,
                  });
    }
    free(config);
    if (err == ESP_OK) {
        network_provisioning_wifi_cb(false, handle);
    }
    return err;
}
