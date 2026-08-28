/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wechat_binding_service.h"

#include <stdlib.h>
#include <string.h>

#include "cap_im_wechat.h"
#include "claw_task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WECHAT_BINDING_MAX_CALLBACKS 4U
#define WECHAT_BINDING_POLL_MS 500U
#define WECHAT_BINDING_TASK_STACK (8U * 1024U)
#define WECHAT_BINDING_TASK_PRIORITY 4U

typedef struct {
    wechat_binding_event_cb_t callback;
    void *user_ctx;
} wechat_binding_observer_t;

struct wechat_binding_service_t {
    wechat_binding_service_config_t config;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t start_lock;
    SemaphoreHandle_t worker_stopped;
    TaskHandle_t worker;
    wechat_binding_status_t status;
    wechat_binding_session_t session;
    wechat_binding_observer_t
        observers[WECHAT_BINDING_MAX_CALLBACKS];
    char pending_account_id[64];
    bool pending_force;
    wechat_binding_persist_mode_t pending_persist_mode;
    bool stop_requested;
    bool worker_started;
};

static const char *TAG = "wechat_binding";

static wechat_binding_state_t wechat_binding_map_state(
    const cap_im_wechat_qr_login_status_t *raw,
    wechat_binding_persist_mode_t persist_mode)
{
    if (raw->persisted) {
        return WECHAT_BINDING_STATE_COMPLETE;
    }
    if (raw->completed) {
        return persist_mode == WECHAT_BINDING_PERSIST_AUTOMATIC
            ? WECHAT_BINDING_STATE_SAVING
            : WECHAT_BINDING_STATE_AWAITING_SAVE;
    }
    if (strcmp(raw->status, "waiting_scan") == 0 ||
            strcmp(raw->status, "redirected") == 0) {
        return WECHAT_BINDING_STATE_WAITING_SCAN;
    }
    if (strcmp(raw->status, "scanned") == 0) {
        return WECHAT_BINDING_STATE_SCANNED;
    }
    if (strcmp(raw->status, "cancelled") == 0) {
        return WECHAT_BINDING_STATE_CANCELLED;
    }
    if (strcmp(raw->status, "expired") == 0) {
        return WECHAT_BINDING_STATE_EXPIRED;
    }
    if (strcmp(raw->status, "error") == 0) {
        return WECHAT_BINDING_STATE_ERROR;
    }
    return WECHAT_BINDING_STATE_IDLE;
}

static const char *wechat_binding_state_message(
    wechat_binding_state_t state)
{
    switch (state) {
    case WECHAT_BINDING_STATE_WAITING_SCAN:
        return "Scan with WeChat";
    case WECHAT_BINDING_STATE_SCANNED:
        return "Confirm on your phone";
    case WECHAT_BINDING_STATE_SAVING:
        return "Saving binding...";
    case WECHAT_BINDING_STATE_AWAITING_SAVE:
        return "Ready to save";
    case WECHAT_BINDING_STATE_COMPLETE:
        return "WeChat is connected";
    case WECHAT_BINDING_STATE_CANCELLED:
        return "Binding cancelled";
    case WECHAT_BINDING_STATE_EXPIRED:
        return "QR code expired";
    case WECHAT_BINDING_STATE_ERROR:
        return "Binding failed";
    default:
        return "Not connected";
    }
}

static const char *wechat_binding_state_name(
    wechat_binding_state_t state)
{
    switch (state) {
    case WECHAT_BINDING_STATE_IDLE:
        return "idle";
    case WECHAT_BINDING_STATE_WAITING_SCAN:
        return "waiting_scan";
    case WECHAT_BINDING_STATE_SCANNED:
        return "scanned";
    case WECHAT_BINDING_STATE_SAVING:
        return "saving";
    case WECHAT_BINDING_STATE_AWAITING_SAVE:
        return "awaiting_save";
    case WECHAT_BINDING_STATE_COMPLETE:
        return "complete";
    case WECHAT_BINDING_STATE_CANCELLED:
        return "cancelled";
    case WECHAT_BINDING_STATE_EXPIRED:
        return "expired";
    case WECHAT_BINDING_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static void wechat_binding_convert_status(
    const cap_im_wechat_qr_login_status_t *raw,
    wechat_binding_status_t *status,
    wechat_binding_persist_mode_t persist_mode)
{
    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->persisted = raw->persisted;
    status->state = wechat_binding_map_state(raw, persist_mode);
    strlcpy(status->message,
            wechat_binding_state_message(status->state),
            sizeof(status->message));
    /*
     * qr_payload is the value encoded into the on-device QR image.
     * The API's qrcode field is only the opaque token used by
     * get_qrcode_status; qrcode_img_content/qr_data_url is the login
     * URL that the web frontend also encodes for the user to scan.
     */
    strlcpy(status->qr_payload,
            raw->qr_data_url,
            sizeof(status->qr_payload));
    strlcpy(status->account_id,
            raw->account_id,
            sizeof(status->account_id));
    strlcpy(status->user_id,
            raw->user_id,
            sizeof(status->user_id));
}

static bool wechat_binding_status_equal(
    const wechat_binding_status_t *left,
    const wechat_binding_status_t *right)
{
    return left->active == right->active &&
           left->configured == right->configured &&
           left->persisted == right->persisted &&
           left->state == right->state &&
           strcmp(left->message, right->message) == 0 &&
           strcmp(left->qr_payload, right->qr_payload) == 0 &&
           strcmp(left->account_id, right->account_id) == 0 &&
           strcmp(left->user_id, right->user_id) == 0;
}

static void wechat_binding_convert_session(
    const cap_im_wechat_qr_login_status_t *raw,
    wechat_binding_session_t *session)
{
    memset(session, 0, sizeof(*session));
    session->active = raw->active;
    session->configured = raw->configured;
    session->completed = raw->completed;
    session->persisted = raw->persisted;
    strlcpy(session->session_key,
            raw->session_key, sizeof(session->session_key));
    strlcpy(session->status,
            raw->status, sizeof(session->status));
    strlcpy(session->message,
            raw->message, sizeof(session->message));
    strlcpy(session->qr_data_url,
            raw->qr_data_url, sizeof(session->qr_data_url));
    strlcpy(session->account_id,
            raw->account_id, sizeof(session->account_id));
    strlcpy(session->user_id,
            raw->user_id, sizeof(session->user_id));
    strlcpy(session->token,
            raw->token, sizeof(session->token));
    strlcpy(session->base_url,
            raw->base_url, sizeof(session->base_url));
}

static void wechat_binding_publish(
    wechat_binding_service_handle_t handle,
    const cap_im_wechat_qr_login_status_t *raw,
    const wechat_binding_status_t *status,
    bool force)
{
    wechat_binding_observer_t
        observers[WECHAT_BINDING_MAX_CALLBACKS] = {0};
    bool changed = false;

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (handle->stop_requested &&
            status->state != WECHAT_BINDING_STATE_CANCELLED) {
        xSemaphoreGive(handle->lock);
        return;
    }
    changed = !wechat_binding_status_equal(&handle->status, status);
    handle->status = *status;
    if (raw != NULL) {
        wechat_binding_convert_session(raw, &handle->session);
    }
    memcpy(observers, handle->observers, sizeof(observers));
    xSemaphoreGive(handle->lock);
    if (!changed && !force) {
        return;
    }

    size_t observer_count = 0;
    for (size_t i = 0; i < WECHAT_BINDING_MAX_CALLBACKS; ++i) {
        if (observers[i].callback != NULL) {
            observer_count++;
        }
    }
    ESP_LOGI(TAG,
             "publish state=%s active=%d configured=%d persisted=%d"
             " qr_payload_len=%u observers=%u force=%d",
             wechat_binding_state_name(status->state),
             status->active,
             status->configured,
             status->persisted,
             (unsigned int)strlen(status->qr_payload),
             (unsigned int)observer_count,
             force);

    for (size_t i = 0; i < WECHAT_BINDING_MAX_CALLBACKS; ++i) {
        if (observers[i].callback != NULL) {
            observers[i].callback(
                handle, status, observers[i].user_ctx);
        }
    }
}

static esp_err_t wechat_binding_commit(
    wechat_binding_service_handle_t handle,
    const cap_im_wechat_qr_login_status_t *raw)
{
    ESP_RETURN_ON_FALSE(handle->config.persist != NULL &&
                        raw->token[0] != '\0',
                        ESP_ERR_INVALID_STATE, TAG,
                        "binding persistence unavailable");

    wechat_binding_credentials_t *credentials =
        calloc(1, sizeof(*credentials));
    ESP_RETURN_ON_FALSE(credentials != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate credentials failed");
    strlcpy(credentials->token,
            raw->token, sizeof(credentials->token));
    strlcpy(credentials->base_url,
            raw->base_url, sizeof(credentials->base_url));
    strlcpy(credentials->account_id,
            raw->account_id, sizeof(credentials->account_id));

    esp_err_t err = handle->config.persist(
                        credentials,
                        handle->config.persist_ctx);
    if (err == ESP_OK) {
        err = cap_im_wechat_set_client_config(
                  &(cap_im_wechat_client_config_t) {
                      .token = credentials->token,
                      .base_url = credentials->base_url,
                      .account_id = credentials->account_id,
                  });
    }
    if (err == ESP_OK) {
        err = cap_im_wechat_start();
    }
    if (err == ESP_OK) {
        err = cap_im_wechat_qr_login_mark_persisted();
    }
    free(credentials);
    return err;
}

static bool wechat_binding_should_stop(
    wechat_binding_service_handle_t handle)
{
    bool stop = false;
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE) {
        stop = handle->stop_requested;
        xSemaphoreGive(handle->lock);
    }
    return stop;
}

static void wechat_binding_set_error_status(
    wechat_binding_service_handle_t handle,
    wechat_binding_status_t *status,
    const char *message)
{
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE) {
        status->configured = handle->status.configured;
        status->persisted = handle->status.persisted;
        strlcpy(status->account_id,
                handle->status.account_id,
                sizeof(status->account_id));
        xSemaphoreGive(handle->lock);
    }
    status->active = false;
    status->state = WECHAT_BINDING_STATE_ERROR;
    strlcpy(status->message, message, sizeof(status->message));
}

static void wechat_binding_worker(void *arg)
{
    wechat_binding_service_handle_t handle =
        (wechat_binding_service_handle_t)arg;
    char account_id[sizeof(handle->pending_account_id)] = {0};
    bool force = false;
    wechat_binding_persist_mode_t persist_mode =
        WECHAT_BINDING_PERSIST_MANUAL;
    bool committed = false;
    esp_err_t start_err = ESP_OK;

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        goto stopped;
    }
    strlcpy(account_id, handle->pending_account_id,
            sizeof(account_id));
    force = handle->pending_force;
    persist_mode = handle->pending_persist_mode;
    xSemaphoreGive(handle->lock);

    if (wechat_binding_should_stop(handle)) {
        goto stopped;
    }

    ESP_LOGI(TAG,
             "worker starting QR login: force=%d auto_persist=%d"
             " account_id_present=%d",
             force,
             persist_mode == WECHAT_BINDING_PERSIST_AUTOMATIC,
             account_id[0] != '\0');
    start_err = cap_im_wechat_qr_login_start(
                    account_id[0] ? account_id : NULL,
                    force);
    if (start_err != ESP_OK) {
        wechat_binding_status_t *status =
            calloc(1, sizeof(*status));
        if (status != NULL) {
            wechat_binding_set_error_status(
                handle, status,
                "Could not prepare WeChat QR code");
            wechat_binding_publish(
                handle, NULL, status, true);
            free(status);
        }
        ESP_LOGW(TAG, "start QR login failed: %s",
                 esp_err_to_name(start_err));
        goto stopped;
    }

    if (wechat_binding_should_stop(handle)) {
        (void)cap_im_wechat_qr_login_cancel();
        goto stopped;
    }

    while (!wechat_binding_should_stop(handle)) {
        cap_im_wechat_qr_login_status_t *raw =
            calloc(1, sizeof(*raw));
        wechat_binding_status_t *status =
            calloc(1, sizeof(*status));
        if (raw == NULL || status == NULL) {
            free(raw);
            free(status);
            ESP_LOGE(TAG, "allocate binding poll snapshot failed");
            break;
        }

        esp_err_t err = cap_im_wechat_qr_login_get_status(raw);
        if (err == ESP_OK) {
            wechat_binding_convert_status(
                raw, status, persist_mode);
            wechat_binding_publish(handle, raw, status, false);
            if (raw->completed && !raw->persisted && !committed &&
                    persist_mode ==
                        WECHAT_BINDING_PERSIST_AUTOMATIC &&
                    !wechat_binding_should_stop(handle)) {
                committed = true;
                err = wechat_binding_commit(handle, raw);
                if (err == ESP_OK) {
                    raw->active = false;
                    raw->configured = true;
                    raw->persisted = true;
                    status->active = false;
                    status->configured = true;
                    status->persisted = true;
                    status->state = WECHAT_BINDING_STATE_COMPLETE;
                    strlcpy(status->message,
                            "WeChat is connected",
                            sizeof(status->message));
                } else {
                    (void)cap_im_wechat_qr_login_cancel();
                    raw->active = false;
                    status->active = false;
                    status->state = WECHAT_BINDING_STATE_ERROR;
                    strlcpy(status->message,
                            "Could not save WeChat binding",
                            sizeof(status->message));
                }
                wechat_binding_publish(handle, raw, status, false);
            }
        } else {
            wechat_binding_set_error_status(
                handle, status,
                "Could not read WeChat binding status");
            wechat_binding_publish(handle, NULL, status, false);
        }

        bool done = status != NULL &&
            !status->active &&
            (status->state == WECHAT_BINDING_STATE_COMPLETE ||
             status->state == WECHAT_BINDING_STATE_AWAITING_SAVE ||
             status->state == WECHAT_BINDING_STATE_CANCELLED ||
             status->state == WECHAT_BINDING_STATE_EXPIRED ||
             status->state == WECHAT_BINDING_STATE_ERROR);
        free(raw);
        free(status);
        if (done) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(WECHAT_BINDING_POLL_MS));
    }

stopped:
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE) {
        handle->worker = NULL;
        xSemaphoreGive(handle->lock);
    }
    xSemaphoreGive(handle->worker_stopped);
    claw_task_delete(NULL);
}

esp_err_t wechat_binding_service_create(
    const wechat_binding_service_config_t *config,
    wechat_binding_service_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && ret_handle != NULL &&
                        config->persist != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "create arguments invalid");
    *ret_handle = NULL;

    wechat_binding_service_handle_t handle =
        calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate service failed");
    handle->config = (wechat_binding_service_config_t) {
        .persist = config->persist,
        .persist_ctx = config->persist_ctx,
    };
    handle->lock = xSemaphoreCreateMutex();
    handle->start_lock = xSemaphoreCreateMutex();
    handle->worker_stopped = xSemaphoreCreateBinary();
    if (handle->lock == NULL || handle->start_lock == NULL ||
            handle->worker_stopped == NULL) {
        if (handle->worker_stopped != NULL) {
            vSemaphoreDelete(handle->worker_stopped);
        }
        if (handle->lock != NULL) {
            vSemaphoreDelete(handle->lock);
        }
        if (handle->start_lock != NULL) {
            vSemaphoreDelete(handle->start_lock);
        }
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    cap_im_wechat_qr_login_status_t *raw =
        calloc(1, sizeof(*raw));
    if (raw != NULL &&
            cap_im_wechat_qr_login_get_status(raw) == ESP_OK) {
        wechat_binding_convert_status(
            raw, &handle->status,
            WECHAT_BINDING_PERSIST_MANUAL);
        wechat_binding_convert_session(raw, &handle->session);
    }
    free(raw);
    if (config->initially_configured) {
        handle->status.configured = true;
        handle->session.configured = true;
        if (!handle->status.active) {
            handle->status.persisted = true;
            handle->status.state = WECHAT_BINDING_STATE_COMPLETE;
            strlcpy(handle->status.message,
                    "WeChat is connected",
                    sizeof(handle->status.message));
            handle->session.completed = true;
            handle->session.persisted = true;
            strlcpy(handle->session.status, "complete",
                    sizeof(handle->session.status));
            strlcpy(handle->session.message,
                    "WeChat is connected",
                    sizeof(handle->session.message));
        }
        if (config->initial_account_id != NULL) {
            strlcpy(handle->status.account_id,
                    config->initial_account_id,
                    sizeof(handle->status.account_id));
            strlcpy(handle->session.account_id,
                    config->initial_account_id,
                    sizeof(handle->session.account_id));
        }
    }
    *ret_handle = handle;
    return ESP_OK;
}

void wechat_binding_service_delete(
    wechat_binding_service_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    (void)wechat_binding_service_cancel(handle);
    bool wait_for_worker = false;
    if (handle->lock != NULL &&
            xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE) {
        wait_for_worker = handle->worker_started;
        xSemaphoreGive(handle->lock);
    }
    if (wait_for_worker) {
        (void)xSemaphoreTake(handle->worker_stopped, portMAX_DELAY);
    }
    if (handle->worker_stopped != NULL) {
        vSemaphoreDelete(handle->worker_stopped);
    }
    if (handle->lock != NULL) {
        vSemaphoreDelete(handle->lock);
    }
    if (handle->start_lock != NULL) {
        vSemaphoreDelete(handle->start_lock);
    }
    free(handle);
}

esp_err_t wechat_binding_service_start(
    wechat_binding_service_handle_t handle,
    const char *account_id,
    bool force,
    wechat_binding_persist_mode_t persist_mode)
{
    bool wait_for_previous = false;
    esp_err_t result = ESP_OK;

    ESP_RETURN_ON_FALSE(handle != NULL &&
                        (persist_mode == WECHAT_BINDING_PERSIST_MANUAL ||
                         persist_mode ==
                            WECHAT_BINDING_PERSIST_AUTOMATIC),
                        ESP_ERR_INVALID_ARG, TAG,
                        "start arguments invalid");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->start_lock, portMAX_DELAY) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "start lock failed");
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        result = ESP_ERR_TIMEOUT;
        goto done;
    }
    if (handle->worker != NULL) {
        xSemaphoreGive(handle->lock);
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (handle->worker_started) {
        wait_for_previous = true;
    }
    xSemaphoreGive(handle->lock);

    if (wait_for_previous) {
        if (xSemaphoreTake(handle->worker_stopped, portMAX_DELAY) !=
                pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            goto done;
        }
        if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            goto done;
        }
        handle->worker_started = false;
        xSemaphoreGive(handle->lock);
    }

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        result = ESP_ERR_TIMEOUT;
        goto done;
    }
    if (handle->worker != NULL || handle->worker_started) {
        xSemaphoreGive(handle->lock);
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }
    bool configured = handle->status.configured;
    bool persisted = handle->status.persisted;
    char existing_account_id[sizeof(handle->status.account_id)];
    strlcpy(existing_account_id, handle->status.account_id,
            sizeof(existing_account_id));
    memset(&handle->status, 0, sizeof(handle->status));
    handle->status.active = true;
    handle->status.configured = configured;
    handle->status.persisted = persisted;
    handle->status.state = WECHAT_BINDING_STATE_IDLE;
    strlcpy(handle->status.message,
            "Preparing WeChat QR code",
            sizeof(handle->status.message));
    strlcpy(handle->status.account_id, existing_account_id,
            sizeof(handle->status.account_id));
    memset(&handle->session, 0, sizeof(handle->session));
    handle->session.active = true;
    handle->session.configured = configured;
    handle->session.persisted = persisted;
    strlcpy(handle->session.status, "starting",
            sizeof(handle->session.status));
    strlcpy(handle->session.message,
            "Preparing WeChat QR code",
            sizeof(handle->session.message));
    strlcpy(handle->session.account_id, existing_account_id,
            sizeof(handle->session.account_id));
    strlcpy(handle->pending_account_id,
            account_id ? account_id : "",
            sizeof(handle->pending_account_id));
    handle->pending_force = force;
    handle->pending_persist_mode = persist_mode;
    handle->stop_requested = false;
    ESP_LOGI(TAG,
             "queue QR login worker: force=%d auto_persist=%d"
             " account_id_present=%d",
             force,
             persist_mode == WECHAT_BINDING_PERSIST_AUTOMATIC,
             handle->pending_account_id[0] != '\0');
    BaseType_t created = claw_task_create(
                             &(claw_task_config_t) {
                                 .name = "wechat_binding",
                                 .stack_size = WECHAT_BINDING_TASK_STACK,
                                 .priority = WECHAT_BINDING_TASK_PRIORITY,
                                 .core_id = tskNO_AFFINITY,
                                 .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                             },
                             wechat_binding_worker, handle, &handle->worker);
    handle->worker_started = created == pdPASS;
    if (created != pdPASS) {
        handle->worker = NULL;
        handle->status.active = false;
        handle->status.state = WECHAT_BINDING_STATE_ERROR;
        strlcpy(handle->status.message,
                "Could not start WeChat binding",
                sizeof(handle->status.message));
        handle->session.active = false;
        strlcpy(handle->session.status, "error",
                sizeof(handle->session.status));
        strlcpy(handle->session.message,
                "Could not start WeChat binding",
                sizeof(handle->session.message));
    }
    xSemaphoreGive(handle->lock);
    if (created != pdPASS) {
        result = ESP_ERR_NO_MEM;
    }

done:
    xSemaphoreGive(handle->start_lock);
    return result;
}

esp_err_t wechat_binding_service_cancel(
    wechat_binding_service_handle_t handle)
{
    wechat_binding_status_t *status = NULL;
    bool state_updated = false;
    esp_err_t result = ESP_OK;

    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->start_lock, portMAX_DELAY) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "start lock failed");
    status = calloc(1, sizeof(*status));
    if (handle->lock != NULL &&
            xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE) {
        handle->stop_requested = true;
        handle->status.active = false;
        handle->status.state = WECHAT_BINDING_STATE_CANCELLED;
        handle->status.qr_payload[0] = '\0';
        strlcpy(handle->status.message, "Binding cancelled",
                sizeof(handle->status.message));
        handle->session.active = false;
        handle->session.completed = false;
        handle->session.session_key[0] = '\0';
        handle->session.qr_data_url[0] = '\0';
        handle->session.user_id[0] = '\0';
        handle->session.token[0] = '\0';
        handle->session.base_url[0] = '\0';
        strlcpy(handle->session.status, "cancelled",
                sizeof(handle->session.status));
        strlcpy(handle->session.message, "Binding cancelled",
                sizeof(handle->session.message));
        if (status != NULL) {
            *status = handle->status;
        }
        state_updated = true;
        xSemaphoreGive(handle->lock);
    } else {
        result = ESP_ERR_TIMEOUT;
    }
    esp_err_t err = cap_im_wechat_qr_login_cancel();
    if (status != NULL && state_updated) {
        wechat_binding_publish(handle, NULL, status, true);
    } else if (status == NULL) {
        ESP_LOGW(TAG, "cancelled binding without observer snapshot");
    }
    free(status);
    if (result == ESP_OK && err != ESP_OK &&
            err != ESP_ERR_INVALID_STATE) {
        result = err;
    }
    xSemaphoreGive(handle->start_lock);
    return result;
}

esp_err_t wechat_binding_service_get_status(
    wechat_binding_service_handle_t handle,
    wechat_binding_status_t *ret_status)
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

esp_err_t wechat_binding_service_get_session(
    wechat_binding_service_handle_t handle,
    wechat_binding_session_t *ret_session)
{
    ESP_RETURN_ON_FALSE(handle != NULL && ret_session != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "session arguments invalid");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "state lock failed");
    *ret_session = handle->session;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t wechat_binding_service_register_cb(
    wechat_binding_service_handle_t handle,
    wechat_binding_event_cb_t callback,
    void *user_ctx)
{
    ESP_RETURN_ON_FALSE(handle != NULL && callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "callback arguments invalid");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "state lock failed");

    esp_err_t err = ESP_ERR_NO_MEM;
    size_t registered_index = WECHAT_BINDING_MAX_CALLBACKS;
    for (size_t i = 0; i < WECHAT_BINDING_MAX_CALLBACKS; ++i) {
        if (handle->observers[i].callback == callback &&
                handle->observers[i].user_ctx == user_ctx) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
    }
    for (size_t i = 0;
            err == ESP_ERR_NO_MEM &&
            i < WECHAT_BINDING_MAX_CALLBACKS; ++i) {
        if (handle->observers[i].callback == NULL) {
            handle->observers[i] = (wechat_binding_observer_t) {
                .callback = callback,
                .user_ctx = user_ctx,
            };
            registered_index = i;
            err = ESP_OK;
            break;
        }
    }
    wechat_binding_status_t *status = malloc(sizeof(*status));
    if (status != NULL) {
        *status = handle->status;
    } else {
        err = ESP_ERR_NO_MEM;
        if (registered_index < WECHAT_BINDING_MAX_CALLBACKS) {
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

esp_err_t wechat_binding_service_unregister_cb(
    wechat_binding_service_handle_t handle,
    wechat_binding_event_cb_t callback,
    void *user_ctx)
{
    ESP_RETURN_ON_FALSE(handle != NULL && callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "callback arguments invalid");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "state lock failed");

    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < WECHAT_BINDING_MAX_CALLBACKS; ++i) {
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
