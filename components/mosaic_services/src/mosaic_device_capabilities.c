/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

/* Device-domain capability providers.
 *
 * This file is the only bridge between the shared device model and the
 * capability layer. Apps read and command the device through capabilities;
 * nothing above this file calls the device model directly.
 */

#include "mosaic_device_capabilities.h"

#include <stdlib.h>
#include <string.h>

#include "mosaic_capability.h"
#include "mosaic_capability_contracts.h"
#include "mosaic_settings.h"

#define MOSAIC_DEVICE_HAPTIC_PULSE_MS 25U

static mosaic_haptic_pulse_fn s_haptic_pulse;
static void *s_haptic_user_ctx;

/** Copy a model string into a fixed contract field, always terminated. */
static void copy_field(char *dst, size_t capacity, const char *src)
{
    if (capacity == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    const size_t length = strnlen(src, capacity - 1U);
    memcpy(dst, src, length);
    dst[length] = '\0';
}

/* The Settings snapshot is kilobytes wide and several domains need only a
 * few of its fields, so it is collected on the heap rather than on the
 * caller's task stack. */
static esp_err_t with_snapshot(mosaic_settings_snapshot_t **ret_snapshot)
{
    mosaic_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = mosaic_settings_get_snapshot(snapshot);
    if (err != ESP_OK) {
        free(snapshot);
        return err;
    }
    *ret_snapshot = snapshot;
    return ESP_OK;
}

/* ---------------- system.display ---------------- */

static esp_err_t display_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_display_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_snapshot_t *snapshot = NULL;
    const esp_err_t err = with_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    *(mosaic_cap_display_t *)out_payload = (mosaic_cap_display_t) {
        .available = snapshot->display_available,
        .brightness = snapshot->brightness,
        .rotation_degrees = snapshot->rotation,
        .screen_timeout_ms = snapshot->screen_timeout_ms,
    };
    free(snapshot);
    return ESP_OK;
}

static esp_err_t display_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    switch (command) {
    case MOSAIC_CAP_DISPLAY_CMD_SET_BRIGHTNESS: {
        const mosaic_cap_display_brightness_args_t *request = args;
        return mosaic_settings_set_brightness(
            request->brightness, request->persist);
    }
    case MOSAIC_CAP_DISPLAY_CMD_SET_ROTATION: {
        const mosaic_cap_display_rotation_args_t *request = args;
        return mosaic_settings_set_rotation((uint16_t)request->degrees);
    }
    case MOSAIC_CAP_DISPLAY_CMD_SET_SCREEN_TIMEOUT: {
        const mosaic_cap_display_timeout_args_t *request = args;
        return mosaic_settings_set_screen_timeout(request->timeout_ms);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static const mosaic_capability_ops_t s_display_ops = {
    .read = display_read,
    .invoke = display_invoke,
};

/* ---------------- system.audio ---------------- */

static esp_err_t audio_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_audio_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_snapshot_t *snapshot = NULL;
    const esp_err_t err = with_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    *(mosaic_cap_audio_t *)out_payload = (mosaic_cap_audio_t) {
        .available = snapshot->audio_available,
        .volume = snapshot->volume,
    };
    free(snapshot);
    return ESP_OK;
}

static esp_err_t audio_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    if (command != MOSAIC_CAP_AUDIO_CMD_SET_VOLUME) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const mosaic_cap_audio_volume_args_t *request = args;
    return mosaic_settings_set_volume(request->volume, request->persist);
}

static const mosaic_capability_ops_t s_audio_ops = {
    .read = audio_read,
    .invoke = audio_invoke,
};

/* ---------------- system.haptic ---------------- */

static esp_err_t haptic_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_haptic_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_snapshot_t *snapshot = NULL;
    const esp_err_t err = with_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    *(mosaic_cap_haptic_t *)out_payload = (mosaic_cap_haptic_t) {
        .enabled = snapshot->vibration_enabled,
    };
    free(snapshot);
    return ESP_OK;
}

static esp_err_t haptic_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    switch (command) {
    case MOSAIC_CAP_HAPTIC_CMD_SET_ENABLED: {
        const mosaic_cap_haptic_enable_args_t *request = args;
        return mosaic_settings_set_vibration(request->enabled);
    }
    case MOSAIC_CAP_HAPTIC_CMD_PULSE:
        if (s_haptic_pulse == NULL) {
            return ESP_OK;
        }
        return s_haptic_pulse(s_haptic_user_ctx, MOSAIC_DEVICE_HAPTIC_PULSE_MS);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static const mosaic_capability_ops_t s_haptic_ops = {
    .read = haptic_read,
    .invoke = haptic_invoke,
};

/* ---------------- system.power ---------------- */

static uint32_t optional_u32(uint16_t value)
{
    return value == UINT16_MAX ? MOSAIC_CAP_VALUE_UNKNOWN_U32 : value;
}

static void power_from_model(
    const mosaic_settings_battery_t *battery, mosaic_cap_power_t *out_power)
{
    *out_power = (mosaic_cap_power_t) {
        .available = battery->available,
        .charging = battery->charging,
        .percent = battery->state_of_charge,
        .voltage_mv = battery->voltage_mv,
        .current_ma = battery->current_ma,
        .time_to_empty_min = optional_u32(battery->time_to_empty_min),
        .time_to_full_min = optional_u32(battery->time_to_full_min),
        .cycle_count = optional_u32(battery->cycle_count),
        .state_of_health = optional_u32(battery->state_of_health),
        .sequence = battery->sequence,
    };
}

static esp_err_t power_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_power_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_battery_t battery = {0};
    const esp_err_t err = mosaic_settings_get_battery(&battery);
    if (err != ESP_OK) {
        return err;
    }
    power_from_model(&battery, out_payload);
    return ESP_OK;
}

static const mosaic_capability_ops_t s_power_ops = {
    .read = power_read,
};

static void on_battery_sample(
    const mosaic_settings_battery_t *info, void *user_ctx)
{
    (void)user_ctx;
    if (info == NULL) {
        return;
    }
    mosaic_cap_power_t power = {0};
    power_from_model(info, &power);
    mosaic_capability_publish("system.power", &power, sizeof(power));
}

/* ---------------- system.update ---------------- */

static esp_err_t update_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_update_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_snapshot_t *snapshot = NULL;
    const esp_err_t err = with_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    mosaic_cap_update_t *out = out_payload;
    out->state = (int32_t)snapshot->update.state;
    out->sequence = snapshot->update.sequence;
    out->last_error = (int32_t)snapshot->update.last_error;
    copy_field(out->latest_version, sizeof(out->latest_version),
        snapshot->update.latest_version);
    copy_field(out->title, sizeof(out->title), snapshot->update.title);
    copy_field(out->summary, sizeof(out->summary), snapshot->update.summary);
    copy_field(out->published_at, sizeof(out->published_at),
        snapshot->update.published_at);
    free(snapshot);
    return ESP_OK;
}

static esp_err_t update_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    if (command != MOSAIC_CAP_UPDATE_CMD_CHECK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t err = mosaic_settings_request_update_check();
    /* The manifest fetch completes in the background; the result reaches
     * consumers through the next system.update read. */
    return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
}

static const mosaic_capability_ops_t s_update_ops = {
    .read = update_read,
    .invoke = update_invoke,
};

/* ---------------- system.lifecycle ---------------- */

static esp_err_t lifecycle_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_lifecycle_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_snapshot_t *snapshot = NULL;
    const esp_err_t err = with_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    mosaic_cap_lifecycle_t *out = out_payload;
    copy_field(out->software_version, sizeof(out->software_version),
        snapshot->software_version);
    free(snapshot);
    return ESP_OK;
}

static esp_err_t lifecycle_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    if (command != MOSAIC_CAP_LIFECYCLE_CMD_FACTORY_RESET) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t err = mosaic_settings_factory_reset();
    return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
}

static const mosaic_capability_ops_t s_lifecycle_ops = {
    .read = lifecycle_read,
    .invoke = lifecycle_invoke,
};

/* ---------------- net.wifi ---------------- */

static void wifi_from_model(
    const mosaic_settings_network_t *network, mosaic_cap_wifi_t *out_wifi)
{
    *out_wifi = (mosaic_cap_wifi_t) {
        .desired_enabled = network->desired_enabled,
        .enabled = network->enabled,
        .connected = network->connected,
        .configured = network->configured,
        .ap_active = network->ap_active,
        .state = (int32_t)network->state,
        .radio_state = (int32_t)network->radio_state,
        .sta_state = (int32_t)network->sta_state,
        .scan_state = (int32_t)network->scan_state,
        .scan_revision = network->scan_revision,
        .scan_error = (int32_t)network->scan_error,
        .operation_id = network->operation_id,
        .disconnect_reason = network->disconnect_reason,
        .last_error = (int32_t)network->last_error,
        .rssi = network->rssi,
    };
    copy_field(out_wifi->ssid, sizeof(out_wifi->ssid), network->ssid);
    copy_field(out_wifi->ip, sizeof(out_wifi->ip), network->ip);
    copy_field(out_wifi->portal_url, sizeof(out_wifi->portal_url),
        network->portal_url);
}

static esp_err_t wifi_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_wifi_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_network_t network = {0};
    const esp_err_t err = mosaic_settings_get_wifi(&network);
    if (err != ESP_OK) {
        return err;
    }
    wifi_from_model(&network, out_payload);
    return ESP_OK;
}

static esp_err_t wifi_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    switch (command) {
    case MOSAIC_CAP_WIFI_CMD_SET_ENABLED: {
        const mosaic_cap_wifi_enable_args_t *request = args;
        return mosaic_settings_set_wifi_enabled(request->enabled);
    }
    case MOSAIC_CAP_WIFI_CMD_SCAN: {
        const esp_err_t err = mosaic_settings_request_wifi_scan();
        /* Results land in net.wifi.scan once scan_revision advances. */
        return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
    }
    case MOSAIC_CAP_WIFI_CMD_CONNECT: {
        const mosaic_cap_wifi_connect_args_t *request = args;
        const esp_err_t err =
            mosaic_settings_connect_wifi(request->ssid, request->password);
        return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
    }
    case MOSAIC_CAP_WIFI_CMD_FORGET:
        return mosaic_settings_forget_wifi();
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static const mosaic_capability_ops_t s_wifi_ops = {
    .read = wifi_read,
    .invoke = wifi_invoke,
};

static void on_wifi_sample(
    const mosaic_settings_network_t *info, void *user_ctx)
{
    (void)user_ctx;
    if (info == NULL) {
        return;
    }
    mosaic_cap_wifi_t wifi = {0};
    wifi_from_model(info, &wifi);
    mosaic_capability_publish("net.wifi", &wifi, sizeof(wifi));
}

/* ---------------- net.wifi.scan ---------------- */

static esp_err_t wifi_scan_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_wifi_scan_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_wifi_ap_t *records =
        calloc(MOSAIC_CAP_WIFI_SCAN_MAX, sizeof(*records));
    if (records == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    const esp_err_t err = mosaic_settings_scan_wifi(
        records, MOSAIC_CAP_WIFI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        free(records);
        return err;
    }
    if (count > MOSAIC_CAP_WIFI_SCAN_MAX) {
        count = MOSAIC_CAP_WIFI_SCAN_MAX;
    }
    mosaic_cap_wifi_scan_t *out = out_payload;
    out->count = (uint32_t)count;
    for (size_t index = 0; index < count; ++index) {
        copy_field(out->entries[index].ssid,
            sizeof(out->entries[index].ssid), records[index].ssid);
        out->entries[index].rssi = records[index].rssi;
        out->entries[index].secured = records[index].secured;
    }
    free(records);
    return ESP_OK;
}

static const mosaic_capability_ops_t s_wifi_scan_ops = {
    .read = wifi_scan_read,
};

/* ---------------- net.provisioning ---------------- */

static esp_err_t provisioning_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_provisioning_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_cap_provisioning_t *out = out_payload;
    return mosaic_settings_get_phone_setup(out->ap_ssid, sizeof(out->ap_ssid),
        out->qr_payload, sizeof(out->qr_payload));
}

static esp_err_t provisioning_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    if (command != MOSAIC_CAP_PROVISIONING_CMD_RECONFIGURE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t err = mosaic_settings_request_network_reconfigure();
    return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
}

static const mosaic_capability_ops_t s_provisioning_ops = {
    .read = provisioning_read,
    .invoke = provisioning_invoke,
};

/* ---------------- config.agent ---------------- */

static esp_err_t agent_config_read(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (payload_size != sizeof(mosaic_cap_agent_config_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    mosaic_settings_snapshot_t *snapshot = NULL;
    const esp_err_t err = with_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    mosaic_cap_agent_config_t *out = out_payload;
    copy_field(out->software_version, sizeof(out->software_version),
        snapshot->software_version);
    copy_field(
        out->llm_backend, sizeof(out->llm_backend), snapshot->llm.backend);
    copy_field(out->llm_model, sizeof(out->llm_model), snapshot->llm.model);
    copy_field(out->llm_base_url, sizeof(out->llm_base_url),
        snapshot->llm.base_url);
    out->llm_api_key_configured = snapshot->llm.api_key_configured;
    out->llm_supports_tools = snapshot->llm.supports_tools;
    out->llm_supports_vision = snapshot->llm.supports_vision;
    out->llm_configured = mosaic_settings_llm_is_configured(snapshot);
    out->im_wechat_configured = snapshot->im.wechat_configured;
    out->im_qq_configured = snapshot->im.qq_configured;
    out->im_feishu_configured = snapshot->im.feishu_configured;
    out->im_telegram_configured = snapshot->im.telegram_configured;
    free(snapshot);
    return ESP_OK;
}

static const mosaic_capability_ops_t s_agent_config_ops = {
    .read = agent_config_read,
};

/* ---------------- registration ---------------- */

void mosaic_device_capabilities_set_haptic(
    mosaic_haptic_pulse_fn pulse, void *user_ctx)
{
    s_haptic_pulse = pulse;
    s_haptic_user_ctx = user_ctx;
}

esp_err_t mosaic_device_capabilities_init(void)
{
    static const mosaic_capability_provider_t providers[] = {
        { .name = "system.display", .ops = &s_display_ops },
        { .name = "system.audio", .ops = &s_audio_ops },
        { .name = "system.haptic", .ops = &s_haptic_ops },
        { .name = "system.power", .ops = &s_power_ops },
        { .name = "system.update", .ops = &s_update_ops },
        { .name = "system.lifecycle", .ops = &s_lifecycle_ops },
        { .name = "net.provisioning", .ops = &s_provisioning_ops },
        { .name = "config.agent", .ops = &s_agent_config_ops },
    };
    for (size_t index = 0;
            index < sizeof(providers) / sizeof(providers[0]); ++index) {
        const esp_err_t err = mosaic_capability_register(&providers[index]);
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = mosaic_settings_subscribe_battery(on_battery_sample, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* A board without a Wi-Fi backend leaves the net.wifi capabilities
     * unbound rather than bound to a provider that answers
     * ESP_ERR_NOT_SUPPORTED, so mosaic_capability_available() stays an
     * honest feature probe for consumers. */
    if (!mosaic_settings_wifi_backend_available()) {
        return ESP_OK;
    }
    static const mosaic_capability_provider_t wifi_providers[] = {
        { .name = "net.wifi", .ops = &s_wifi_ops },
        { .name = "net.wifi.scan", .ops = &s_wifi_scan_ops },
    };
    for (size_t index = 0;
            index < sizeof(wifi_providers) / sizeof(wifi_providers[0]);
            ++index) {
        err = mosaic_capability_register(&wifi_providers[index]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return mosaic_settings_subscribe_wifi(on_wifi_sample, NULL);
}
