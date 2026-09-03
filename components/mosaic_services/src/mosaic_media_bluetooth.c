/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* media.bluetooth provider.
 *
 * Owns the A2DP sink runtime so that the Bluetooth App holds no handle to
 * the Bluetooth stack. The App reads a snapshot, invokes transport commands
 * and borrows the cover artwork; everything below this file is private to
 * the services layer.
 */

#include "mosaic_media_bluetooth.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "mosaic_capability.h"
#include "mosaic_capability_contracts.h"

#if defined(ESP_PLATFORM)
#include "audio_hub.h"
#include "bluetooth_audio_runtime.h"
#include "esp_log.h"

static const char *TAG = "mosaic_bt_cap";
#endif

#if defined(ESP_PLATFORM)

typedef struct {
    bluetooth_audio_runtime_handle_t runtime;
} mosaic_media_bluetooth_t;

typedef struct {
    uint8_t *data;
} mosaic_bluetooth_cover_borrow_t;

static mosaic_media_bluetooth_t s_bluetooth;
static bool s_registered;

static void snapshot_to_payload(const bluetooth_audio_snapshot_t *in,
                                mosaic_cap_bluetooth_t *out)
{
    memset(out, 0, sizeof(*out));
    out->revision = in->revision;
    out->cover_revision = in->cover_revision;
    out->state = (int32_t)in->state;
    out->connected = in->connected;
    out->playing = in->playing;
    out->has_cover = in->has_cover;
    out->volume_percent = in->volume_percent;
    out->position_ms = in->position_ms;
    out->duration_ms = in->duration_ms;
    strlcpy(out->device_name, in->device_name, sizeof(out->device_name));
    strlcpy(out->title, in->title, sizeof(out->title));
    strlcpy(out->artist, in->artist, sizeof(out->artist));
    strlcpy(out->error, in->error, sizeof(out->error));
}

static void on_runtime_changed(uint32_t revision, void *user_ctx)
{
    (void)revision;
    (void)user_ctx;
    bluetooth_audio_snapshot_t snapshot = {0};
    if (s_bluetooth.runtime == NULL ||
            bluetooth_audio_runtime_get_snapshot(
                s_bluetooth.runtime, &snapshot) != ESP_OK) {
        return;
    }
    mosaic_cap_bluetooth_t payload;
    snapshot_to_payload(&snapshot, &payload);
    mosaic_capability_publish(
        "media.bluetooth", &payload, sizeof(payload));
}

static esp_err_t bluetooth_ensure(void)
{
    if (s_bluetooth.runtime != NULL) {
        return ESP_OK;
    }
    audio_mixer_handle_t mixer = NULL;
    if (audio_hub_get_mixer(&mixer) != ESP_OK || mixer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const bluetooth_audio_runtime_config_t config = {
        .mixer = mixer,
        .on_changed = on_runtime_changed,
        .user_ctx = NULL,
    };
    esp_err_t err = bluetooth_audio_runtime_create(
        &config, &s_bluetooth.runtime);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create Bluetooth runtime failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = bluetooth_audio_runtime_start(s_bluetooth.runtime);
    if (err != ESP_OK) {
        bluetooth_audio_runtime_delete(s_bluetooth.runtime);
        s_bluetooth.runtime = NULL;
        return err;
    }
    return ESP_OK;
}

static void bluetooth_shutdown(void)
{
    if (s_bluetooth.runtime == NULL) {
        return;
    }
    bluetooth_audio_runtime_delete(s_bluetooth.runtime);
    s_bluetooth.runtime = NULL;
}

static esp_err_t bluetooth_read(void *user_ctx, void *out, size_t size)
{
    (void)user_ctx;
    if (out == NULL || size != sizeof(mosaic_cap_bluetooth_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_bluetooth.runtime == NULL) {
        const esp_err_t err = bluetooth_ensure();
        if (err != ESP_OK) {
            memset(out, 0, size);
            return ESP_OK;
        }
    }
    bluetooth_audio_snapshot_t snapshot = {0};
    const esp_err_t err = bluetooth_audio_runtime_get_snapshot(
        s_bluetooth.runtime, &snapshot);
    if (err != ESP_OK) {
        return err;
    }
    snapshot_to_payload(&snapshot, out);
    return ESP_OK;
}

static esp_err_t bluetooth_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)out_result;
    (void)result_size;
    if (command == MOSAIC_CAP_BT_CMD_SHUTDOWN) {
        bluetooth_shutdown();
        return ESP_OK;
    }
    const esp_err_t err = bluetooth_ensure();
    if (err != ESP_OK) {
        return err;
    }
    switch (command) {
    case MOSAIC_CAP_BT_CMD_TOGGLE_PLAY:
        return bluetooth_audio_runtime_toggle_play(s_bluetooth.runtime);
    case MOSAIC_CAP_BT_CMD_PREVIOUS:
        return bluetooth_audio_runtime_previous(s_bluetooth.runtime);
    case MOSAIC_CAP_BT_CMD_NEXT:
        return bluetooth_audio_runtime_next(s_bluetooth.runtime);
    case MOSAIC_CAP_BT_CMD_SET_VOLUME: {
        if (args == NULL ||
                args_size != sizeof(mosaic_cap_bluetooth_volume_args_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        const mosaic_cap_bluetooth_volume_args_t *volume = args;
        return bluetooth_audio_runtime_set_volume(
            s_bluetooth.runtime, (int)volume->volume_percent);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t bluetooth_borrow(void *user_ctx, uint16_t blob_id,
    uint32_t index, mosaic_capability_blob_t *out_blob)
{
    (void)user_ctx;
    (void)index;
    if (blob_id != MOSAIC_CAP_BT_BLOB_COVER || out_blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_bluetooth.runtime == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    mosaic_bluetooth_cover_borrow_t *borrow = calloc(1, sizeof(*borrow));
    if (borrow == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t revision = 0;
    const esp_err_t err = bluetooth_audio_runtime_take_cover(
        s_bluetooth.runtime, &data, &size, &revision);
    if (err != ESP_OK) {
        free(borrow);
        return err;
    }
    (void)revision;
    borrow->data = data;
    out_blob->data = data;
    out_blob->size = size;
    out_blob->token = borrow;
    return ESP_OK;
}

static void bluetooth_release(void *user_ctx, mosaic_capability_blob_t *blob)
{
    (void)user_ctx;
    if (blob == NULL) {
        return;
    }
    mosaic_bluetooth_cover_borrow_t *borrow = blob->token;
    free(borrow != NULL ? borrow->data : NULL);
    free(borrow);
    blob->data = NULL;
    blob->size = 0;
    blob->token = NULL;
}

static const mosaic_capability_ops_t s_bluetooth_ops = {
    .read = bluetooth_read,
    .invoke = bluetooth_invoke,
    .borrow = bluetooth_borrow,
    .release = bluetooth_release,
};

esp_err_t mosaic_media_bluetooth_init(void)
{
    if (s_registered) {
        return ESP_OK;
    }
    const esp_err_t err = mosaic_capability_register(&(mosaic_capability_provider_t) {
        .name = "media.bluetooth",
        .ops = &s_bluetooth_ops,
    });
    if (err == ESP_OK) {
        s_registered = true;
    }
    return err;
}

void mosaic_media_bluetooth_deinit(void)
{
    bluetooth_shutdown();
    if (!s_registered) {
        return;
    }
    (void)mosaic_capability_unregister("media.bluetooth", NULL);
    s_registered = false;
}

#else /* !ESP_PLATFORM */

static bool s_registered;

static esp_err_t bluetooth_host_read(void *user_ctx, void *out, size_t size)
{
    (void)user_ctx;
    if (out == NULL || size != sizeof(mosaic_cap_bluetooth_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, size);
    return ESP_OK;
}

static esp_err_t bluetooth_host_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    if (command == MOSAIC_CAP_BT_CMD_SHUTDOWN) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static const mosaic_capability_ops_t s_bluetooth_host_ops = {
    .read = bluetooth_host_read,
    .invoke = bluetooth_host_invoke,
};

esp_err_t mosaic_media_bluetooth_init(void)
{
    if (s_registered) {
        return ESP_OK;
    }
    const esp_err_t err = mosaic_capability_register(&(mosaic_capability_provider_t) {
        .name = "media.bluetooth",
        .ops = &s_bluetooth_host_ops,
    });
    if (err == ESP_OK) {
        s_registered = true;
    }
    return err;
}

void mosaic_media_bluetooth_deinit(void)
{
    if (!s_registered) {
        return;
    }
    (void)mosaic_capability_unregister("media.bluetooth", NULL);
    s_registered = false;
}

#endif /* ESP_PLATFORM */
