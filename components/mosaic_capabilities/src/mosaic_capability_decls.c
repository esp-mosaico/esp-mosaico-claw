/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_capability_decls.h"

#include <string.h>

#include "mosaic_capability_contracts.h"

/* ---------------- sensor.imu ---------------- */

static const mosaic_capability_field_t s_orientation_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_orientation_t, pitch_deg, MOSAIC_CAP_FIELD_F32),
    MOSAIC_CAP_FIELD(mosaic_cap_orientation_t, roll_deg, MOSAIC_CAP_FIELD_F32),
    MOSAIC_CAP_FIELD(mosaic_cap_orientation_t, yaw_deg, MOSAIC_CAP_FIELD_F32),
};

static const mosaic_capability_contract_t s_orientation_contract =
    MOSAIC_CAP_CONTRACT("sensor.orientation/v1", mosaic_cap_orientation_t,
        s_orientation_fields);

/* ---------------- system.battery ---------------- */

static const mosaic_capability_field_t s_battery_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_battery_t, available, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_battery_t, charging, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_battery_t, percent, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_battery_t, voltage_mv, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_battery_t, current_ma, MOSAIC_CAP_FIELD_I32),
};

static const mosaic_capability_contract_t s_battery_contract =
    MOSAIC_CAP_CONTRACT("system.battery/v1", mosaic_cap_battery_t,
        s_battery_fields);

/* ---------------- system.time ---------------- */

static const mosaic_capability_field_t s_time_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_time_t, unix_seconds, MOSAIC_CAP_FIELD_I64),
    MOSAIC_CAP_FIELD(
        mosaic_cap_time_t, utc_offset_minutes, MOSAIC_CAP_FIELD_I32),
};

static const mosaic_capability_contract_t s_time_contract =
    MOSAIC_CAP_CONTRACT("system.time/v1", mosaic_cap_time_t, s_time_fields);

/* ---------------- system.status ---------------- */

static const mosaic_capability_field_t s_status_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_status_t, battery_available, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_status_t, charging, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_status_t, network_enabled, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_status_t, network_connected, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_status_t, network_rssi, MOSAIC_CAP_FIELD_I32),
};

static const mosaic_capability_contract_t s_status_contract =
    MOSAIC_CAP_CONTRACT("system.status/v1", mosaic_cap_status_t,
        s_status_fields);

/* ---------------- system.display ---------------- */

static const mosaic_capability_field_t s_display_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_display_t, available, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_display_t, brightness, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_display_t, rotation_degrees, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_display_t, screen_timeout_ms, MOSAIC_CAP_FIELD_U32),
};

static const mosaic_capability_contract_t s_display_contract =
    MOSAIC_CAP_CONTRACT("system.display/v1", mosaic_cap_display_t,
        s_display_fields);

static const mosaic_capability_field_t s_display_brightness_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_display_brightness_args_t, brightness,
        MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_display_brightness_args_t, persist,
        MOSAIC_CAP_FIELD_BOOL),
};

static const mosaic_capability_contract_t s_display_brightness_contract =
    MOSAIC_CAP_CONTRACT("system.display.brightness/v1",
        mosaic_cap_display_brightness_args_t, s_display_brightness_fields);

static const mosaic_capability_field_t s_display_rotation_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_display_rotation_args_t, degrees, MOSAIC_CAP_FIELD_U32),
};

static const mosaic_capability_contract_t s_display_rotation_contract =
    MOSAIC_CAP_CONTRACT("system.display.rotation/v1",
        mosaic_cap_display_rotation_args_t, s_display_rotation_fields);

static const mosaic_capability_field_t s_display_timeout_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_display_timeout_args_t, timeout_ms, MOSAIC_CAP_FIELD_U32),
};

static const mosaic_capability_contract_t s_display_timeout_contract =
    MOSAIC_CAP_CONTRACT("system.display.timeout/v1",
        mosaic_cap_display_timeout_args_t, s_display_timeout_fields);

static const mosaic_capability_command_t s_display_commands[] = {
    {
        .name = "set_brightness",
        .command = MOSAIC_CAP_DISPLAY_CMD_SET_BRIGHTNESS,
        .permission = MOSAIC_CAP_SYSTEM_DISPLAY_CONTROL,
        .args = &s_display_brightness_contract,
    },
    {
        .name = "set_rotation",
        .command = MOSAIC_CAP_DISPLAY_CMD_SET_ROTATION,
        .permission = MOSAIC_CAP_SYSTEM_DISPLAY_CONTROL,
        .args = &s_display_rotation_contract,
    },
    {
        .name = "set_screen_timeout",
        .command = MOSAIC_CAP_DISPLAY_CMD_SET_SCREEN_TIMEOUT,
        .permission = MOSAIC_CAP_SYSTEM_DISPLAY_CONTROL,
        .args = &s_display_timeout_contract,
    },
};

/* ---------------- system.audio ---------------- */

static const mosaic_capability_field_t s_audio_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_audio_t, available, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_audio_t, volume, MOSAIC_CAP_FIELD_I32),
};

static const mosaic_capability_contract_t s_audio_contract =
    MOSAIC_CAP_CONTRACT("system.audio/v1", mosaic_cap_audio_t, s_audio_fields);

static const mosaic_capability_field_t s_audio_volume_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_audio_volume_args_t, volume, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_audio_volume_args_t, persist, MOSAIC_CAP_FIELD_BOOL),
};

static const mosaic_capability_contract_t s_audio_volume_contract =
    MOSAIC_CAP_CONTRACT("system.audio.volume/v1",
        mosaic_cap_audio_volume_args_t, s_audio_volume_fields);

static const mosaic_capability_command_t s_audio_commands[] = {
    {
        .name = "set_volume",
        .command = MOSAIC_CAP_AUDIO_CMD_SET_VOLUME,
        .permission = MOSAIC_CAP_SYSTEM_AUDIO_CONTROL,
        .args = &s_audio_volume_contract,
    },
};

/* ---------------- system.haptic ---------------- */

static const mosaic_capability_field_t s_haptic_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_haptic_t, enabled, MOSAIC_CAP_FIELD_BOOL),
};

static const mosaic_capability_contract_t s_haptic_contract =
    MOSAIC_CAP_CONTRACT("system.haptic/v1", mosaic_cap_haptic_t,
        s_haptic_fields);

static const mosaic_capability_field_t s_haptic_enable_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_haptic_enable_args_t, enabled, MOSAIC_CAP_FIELD_BOOL),
};

static const mosaic_capability_contract_t s_haptic_enable_contract =
    MOSAIC_CAP_CONTRACT("system.haptic.enable/v1",
        mosaic_cap_haptic_enable_args_t, s_haptic_enable_fields);

static const mosaic_capability_command_t s_haptic_commands[] = {
    {
        .name = "set_enabled",
        .command = MOSAIC_CAP_HAPTIC_CMD_SET_ENABLED,
        .permission = MOSAIC_CAP_SYSTEM_HAPTIC_CONTROL,
        .args = &s_haptic_enable_contract,
    },
    {
        .name = "pulse",
        .command = MOSAIC_CAP_HAPTIC_CMD_PULSE,
        .permission = MOSAIC_CAP_SYSTEM_HAPTIC_CONTROL,
    },
};

/* ---------------- system.power ---------------- */

static const mosaic_capability_field_t s_power_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_power_t, available, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_power_t, charging, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_power_t, percent, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_power_t, voltage_mv, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_power_t, current_ma, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_power_t, time_to_empty_min, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_power_t, time_to_full_min, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_power_t, cycle_count, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_power_t, state_of_health, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_power_t, sequence, MOSAIC_CAP_FIELD_U32),
};

static const mosaic_capability_contract_t s_power_contract =
    MOSAIC_CAP_CONTRACT("system.power/v1", mosaic_cap_power_t, s_power_fields);

/* ---------------- system.update ---------------- */

static const mosaic_capability_field_t s_update_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_update_t, state, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_update_t, latest_version, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_update_t, title, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_update_t, summary, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_update_t, published_at, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_update_t, sequence, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_update_t, last_error, MOSAIC_CAP_FIELD_I32),
};

static const mosaic_capability_contract_t s_update_contract =
    MOSAIC_CAP_CONTRACT("system.update/v1", mosaic_cap_update_t,
        s_update_fields);

static const mosaic_capability_command_t s_update_commands[] = {
    {
        .name = "check",
        .command = MOSAIC_CAP_UPDATE_CMD_CHECK,
        .permission = MOSAIC_CAP_SYSTEM_UPDATE_CONTROL,
        .async = true,
    },
};

/* ---------------- system.lifecycle ---------------- */

static const mosaic_capability_field_t s_lifecycle_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_lifecycle_t, software_version, MOSAIC_CAP_FIELD_STRING),
};

static const mosaic_capability_contract_t s_lifecycle_contract =
    MOSAIC_CAP_CONTRACT("system.lifecycle/v1", mosaic_cap_lifecycle_t,
        s_lifecycle_fields);

static const mosaic_capability_command_t s_lifecycle_commands[] = {
    {
        .name = "factory_reset",
        .command = MOSAIC_CAP_LIFECYCLE_CMD_FACTORY_RESET,
        .permission = MOSAIC_CAP_SYSTEM_LIFECYCLE_CONTROL,
        .async = true,
    },
};

/* ---------------- net.wifi ---------------- */

static const mosaic_capability_field_t s_wifi_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, desired_enabled, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, enabled, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, connected, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, configured, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, ap_active, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, ssid, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, ip, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, portal_url, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, state, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, radio_state, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, sta_state, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, scan_state, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, scan_revision, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, scan_error, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, operation_id, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_wifi_t, disconnect_reason, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, last_error, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_t, rssi, MOSAIC_CAP_FIELD_I32),
};

static const mosaic_capability_contract_t s_wifi_contract =
    MOSAIC_CAP_CONTRACT("net.wifi/v1", mosaic_cap_wifi_t, s_wifi_fields);

static const mosaic_capability_field_t s_wifi_ap_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_ap_t, ssid, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_ap_t, rssi, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_ap_t, secured, MOSAIC_CAP_FIELD_BOOL),
};

static const mosaic_capability_contract_t s_wifi_ap_contract =
    MOSAIC_CAP_CONTRACT("net.wifi.ap/v1", mosaic_cap_wifi_ap_t,
        s_wifi_ap_fields);

static const mosaic_capability_field_t s_wifi_scan_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_wifi_scan_t, count, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_ARRAY_FIELD(
        mosaic_cap_wifi_scan_t, entries, &s_wifi_ap_contract),
};

static const mosaic_capability_contract_t s_wifi_scan_contract =
    MOSAIC_CAP_CONTRACT("net.wifi.scan/v1", mosaic_cap_wifi_scan_t,
        s_wifi_scan_fields);

static const mosaic_capability_field_t s_wifi_enable_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_wifi_enable_args_t, enabled, MOSAIC_CAP_FIELD_BOOL),
};

static const mosaic_capability_contract_t s_wifi_enable_contract =
    MOSAIC_CAP_CONTRACT("net.wifi.enable/v1", mosaic_cap_wifi_enable_args_t,
        s_wifi_enable_fields);

static const mosaic_capability_field_t s_wifi_connect_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_wifi_connect_args_t, ssid, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_wifi_connect_args_t, password, MOSAIC_CAP_FIELD_STRING),
};

static const mosaic_capability_contract_t s_wifi_connect_contract =
    MOSAIC_CAP_CONTRACT("net.wifi.connect/v1", mosaic_cap_wifi_connect_args_t,
        s_wifi_connect_fields);

static const mosaic_capability_command_t s_wifi_commands[] = {
    {
        .name = "set_enabled",
        .command = MOSAIC_CAP_WIFI_CMD_SET_ENABLED,
        .permission = MOSAIC_CAP_NET_WIFI_CONTROL,
        .args = &s_wifi_enable_contract,
    },
    {
        .name = "scan",
        .command = MOSAIC_CAP_WIFI_CMD_SCAN,
        .permission = MOSAIC_CAP_NET_WIFI_CONTROL,
        .async = true,
    },
    {
        .name = "connect",
        .command = MOSAIC_CAP_WIFI_CMD_CONNECT,
        .permission = MOSAIC_CAP_NET_WIFI_CONTROL,
        .args = &s_wifi_connect_contract,
        .async = true,
    },
    {
        .name = "forget",
        .command = MOSAIC_CAP_WIFI_CMD_FORGET,
        .permission = MOSAIC_CAP_NET_WIFI_CONTROL,
    },
};

/* ---------------- net.provisioning ---------------- */

static const mosaic_capability_field_t s_provisioning_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_provisioning_t, ap_ssid, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_provisioning_t, qr_payload, MOSAIC_CAP_FIELD_STRING),
};

static const mosaic_capability_contract_t s_provisioning_contract =
    MOSAIC_CAP_CONTRACT("net.provisioning/v1", mosaic_cap_provisioning_t,
        s_provisioning_fields);

static const mosaic_capability_command_t s_provisioning_commands[] = {
    {
        .name = "reconfigure",
        .command = MOSAIC_CAP_PROVISIONING_CMD_RECONFIGURE,
        .permission = MOSAIC_CAP_NET_PROVISIONING_CONTROL,
        .async = true,
    },
};

/* ---------------- config.agent ---------------- */

static const mosaic_capability_field_t s_agent_config_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_agent_config_t, software_version, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_agent_config_t, llm_backend, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_agent_config_t, llm_model, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_agent_config_t, llm_base_url, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_agent_config_t, llm_api_key_configured,
        MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_agent_config_t, llm_supports_tools,
        MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_agent_config_t, llm_supports_vision,
        MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_agent_config_t, llm_configured, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_agent_config_t, im_wechat_configured,
        MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_agent_config_t, im_qq_configured, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_agent_config_t, im_feishu_configured,
        MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_agent_config_t, im_telegram_configured,
        MOSAIC_CAP_FIELD_BOOL),
};

static const mosaic_capability_contract_t s_agent_config_contract =
    MOSAIC_CAP_CONTRACT("config.agent/v1", mosaic_cap_agent_config_t,
        s_agent_config_fields);

/* ---------------- media.bluetooth ---------------- */

static const mosaic_capability_field_t s_bluetooth_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_t, revision, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_bluetooth_t, cover_revision, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_t, state, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_t, connected, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_t, playing, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_t, has_cover, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_bluetooth_t, volume_percent, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_bluetooth_t, position_ms, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_bluetooth_t, duration_ms, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_bluetooth_t, device_name, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_t, title, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_t, artist, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_t, error, MOSAIC_CAP_FIELD_STRING),
};

static const mosaic_capability_contract_t s_bluetooth_contract =
    MOSAIC_CAP_CONTRACT("media.bluetooth/v1", mosaic_cap_bluetooth_t,
        s_bluetooth_fields);

static const mosaic_capability_field_t s_bluetooth_volume_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_bluetooth_volume_args_t, volume_percent,
        MOSAIC_CAP_FIELD_I32),
};

static const mosaic_capability_contract_t s_bluetooth_volume_contract =
    MOSAIC_CAP_CONTRACT("media.bluetooth.set_volume/v1",
        mosaic_cap_bluetooth_volume_args_t, s_bluetooth_volume_fields);

static const mosaic_capability_command_t s_bluetooth_commands[] = {
    {
        .name = "toggle_play",
        .command = MOSAIC_CAP_BT_CMD_TOGGLE_PLAY,
        .permission = MOSAIC_CAP_MEDIA_BLUETOOTH_CONTROL,
    },
    {
        .name = "previous",
        .command = MOSAIC_CAP_BT_CMD_PREVIOUS,
        .permission = MOSAIC_CAP_MEDIA_BLUETOOTH_CONTROL,
    },
    {
        .name = "next",
        .command = MOSAIC_CAP_BT_CMD_NEXT,
        .permission = MOSAIC_CAP_MEDIA_BLUETOOTH_CONTROL,
    },
    {
        .name = "set_volume",
        .command = MOSAIC_CAP_BT_CMD_SET_VOLUME,
        .permission = MOSAIC_CAP_MEDIA_BLUETOOTH_CONTROL,
        .args = &s_bluetooth_volume_contract,
    },
};

/* ---------------- media.player ---------------- */

static const mosaic_capability_field_t s_player_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, revision, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, state, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, track_index, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, track_count, MOSAIC_CAP_FIELD_U32),
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, title, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, artist, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, album, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, position_ms, MOSAIC_CAP_FIELD_I64),
    MOSAIC_CAP_FIELD(mosaic_cap_player_t, duration_ms, MOSAIC_CAP_FIELD_I64),
    MOSAIC_CAP_FIELD(
        mosaic_cap_player_t, shuffle_enabled, MOSAIC_CAP_FIELD_BOOL),
};

static const mosaic_capability_contract_t s_player_contract =
    MOSAIC_CAP_CONTRACT("media.player/v1", mosaic_cap_player_t,
        s_player_fields);

static const mosaic_capability_field_t s_player_select_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_player_select_args_t, track_index, MOSAIC_CAP_FIELD_U32),
};

static const mosaic_capability_contract_t s_player_select_contract =
    MOSAIC_CAP_CONTRACT("media.player.select/v1",
        mosaic_cap_player_select_args_t, s_player_select_fields);

static const mosaic_capability_field_t s_player_seek_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_player_seek_args_t, progress_permille,
        MOSAIC_CAP_FIELD_U32),
};

static const mosaic_capability_contract_t s_player_seek_contract =
    MOSAIC_CAP_CONTRACT("media.player.seek/v1",
        mosaic_cap_player_seek_args_t, s_player_seek_fields);

static const mosaic_capability_field_t s_player_time_fields[] = {
    MOSAIC_CAP_FIELD(
        mosaic_cap_player_time_args_t, now_us, MOSAIC_CAP_FIELD_I64),
};

static const mosaic_capability_contract_t s_player_time_contract =
    MOSAIC_CAP_CONTRACT("media.player.time/v1",
        mosaic_cap_player_time_args_t, s_player_time_fields);

static const mosaic_capability_command_t s_player_commands[] = {
    {
        .name = "toggle",
        .command = MOSAIC_CAP_PLAYER_CMD_TOGGLE,
        .permission = MOSAIC_CAP_MEDIA_PLAYER_CONTROL,
    },
    {
        .name = "next",
        .command = MOSAIC_CAP_PLAYER_CMD_NEXT,
        .permission = MOSAIC_CAP_MEDIA_PLAYER_CONTROL,
    },
    {
        .name = "previous",
        .command = MOSAIC_CAP_PLAYER_CMD_PREVIOUS,
        .permission = MOSAIC_CAP_MEDIA_PLAYER_CONTROL,
    },
    {
        .name = "toggle_shuffle",
        .command = MOSAIC_CAP_PLAYER_CMD_TOGGLE_SHUFFLE,
        .permission = MOSAIC_CAP_MEDIA_PLAYER_CONTROL,
    },
    {
        .name = "select",
        .command = MOSAIC_CAP_PLAYER_CMD_SELECT,
        .permission = MOSAIC_CAP_MEDIA_PLAYER_CONTROL,
        .args = &s_player_select_contract,
    },
    {
        .name = "seek",
        .command = MOSAIC_CAP_PLAYER_CMD_SEEK,
        .permission = MOSAIC_CAP_MEDIA_PLAYER_CONTROL,
        .args = &s_player_seek_contract,
    },
    {
        .name = "start",
        .command = MOSAIC_CAP_PLAYER_CMD_START,
        .permission = MOSAIC_CAP_MEDIA_PLAYER_CONTROL,
        .args = &s_player_time_contract,
    },
    {
        .name = "step",
        .command = MOSAIC_CAP_PLAYER_CMD_STEP,
        .permission = MOSAIC_CAP_MEDIA_PLAYER_CONTROL,
        .args = &s_player_time_contract,
    },
};

/* ---------------- net.weather ---------------- */

static const mosaic_capability_field_t s_weather_day_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_weather_day_t, valid, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_day_t, low_valid, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_weather_day_t, temperature_min_c,
        MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_weather_day_t, temperature_max_c,
        MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_day_t, date, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_day_t, symbol_code, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_day_t, condition, MOSAIC_CAP_FIELD_STRING),
};

static const mosaic_capability_contract_t s_weather_day_contract =
    MOSAIC_CAP_CONTRACT("net.weather.day/v1", mosaic_cap_weather_day_t,
        s_weather_day_fields);

static const mosaic_capability_field_t s_weather_fields[] = {
    MOSAIC_CAP_FIELD(mosaic_cap_weather_t, online, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_t, location_valid, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_t, weather_valid, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(mosaic_cap_weather_t, stale, MOSAIC_CAP_FIELD_BOOL),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_t, temperature_c, MOSAIC_CAP_FIELD_I32),
    MOSAIC_CAP_FIELD(mosaic_cap_weather_t, city, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_t, symbol_code, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_t, condition, MOSAIC_CAP_FIELD_STRING),
    MOSAIC_CAP_ARRAY_FIELD(
        mosaic_cap_weather_t, forecast, &s_weather_day_contract),
    MOSAIC_CAP_FIELD(
        mosaic_cap_weather_t, last_updated_epoch, MOSAIC_CAP_FIELD_I64),
    MOSAIC_CAP_FIELD(mosaic_cap_weather_t, sequence, MOSAIC_CAP_FIELD_U32),
};

static const mosaic_capability_contract_t s_weather_contract =
    MOSAIC_CAP_CONTRACT("net.weather/v1", mosaic_cap_weather_t,
        s_weather_fields);

static const mosaic_capability_command_t s_weather_commands[] = {
    {
        .name = "refresh",
        .command = MOSAIC_CAP_WEATHER_CMD_REFRESH,
        .permission = MOSAIC_CAP_NET_WEATHER_CONTROL,
        .async = true,
    },
};

/* ---------------- declarations ---------------- */

#define MOSAIC_CAP_COMMANDS(table) \
    .commands = (table), \
    .command_count = (uint8_t)(sizeof(table) / sizeof((table)[0]))

static const mosaic_capability_declaration_t s_declarations[] = {
    {
        .name = "sensor.imu",
        .read_permission = MOSAIC_CAP_SENSOR_IMU_READ,
        .read_contract = &s_orientation_contract,
    },
    {
        .name = "system.battery",
        .read_permission = MOSAIC_CAP_SYSTEM_BATTERY_READ,
        .read_contract = &s_battery_contract,
        .publishes = true,
    },
    {
        .name = "system.time",
        .read_permission = MOSAIC_CAP_SYSTEM_TIME_READ,
        .read_contract = &s_time_contract,
    },
    {
        .name = "system.status",
        .read_permission = MOSAIC_CAP_SYSTEM_STATUS_READ,
        .read_contract = &s_status_contract,
        .publishes = true,
    },
    {
        .name = "system.display",
        .read_permission = MOSAIC_CAP_SYSTEM_DISPLAY_READ,
        .read_contract = &s_display_contract,
        MOSAIC_CAP_COMMANDS(s_display_commands),
    },
    {
        .name = "system.audio",
        .read_permission = MOSAIC_CAP_SYSTEM_AUDIO_READ,
        .read_contract = &s_audio_contract,
        MOSAIC_CAP_COMMANDS(s_audio_commands),
    },
    {
        .name = "system.haptic",
        .read_permission = MOSAIC_CAP_SYSTEM_HAPTIC_READ,
        .read_contract = &s_haptic_contract,
        MOSAIC_CAP_COMMANDS(s_haptic_commands),
    },
    {
        .name = "system.power",
        .read_permission = MOSAIC_CAP_SYSTEM_POWER_READ,
        .read_contract = &s_power_contract,
        .publishes = true,
    },
    {
        .name = "system.update",
        .read_permission = MOSAIC_CAP_SYSTEM_UPDATE_READ,
        .read_contract = &s_update_contract,
        MOSAIC_CAP_COMMANDS(s_update_commands),
        .publishes = true,
    },
    {
        .name = "system.lifecycle",
        .read_permission = MOSAIC_CAP_SYSTEM_LIFECYCLE_READ,
        .read_contract = &s_lifecycle_contract,
        MOSAIC_CAP_COMMANDS(s_lifecycle_commands),
    },
    {
        .name = "net.wifi",
        .read_permission = MOSAIC_CAP_NET_WIFI_READ,
        .read_contract = &s_wifi_contract,
        MOSAIC_CAP_COMMANDS(s_wifi_commands),
        .publishes = true,
    },
    {
        .name = "net.wifi.scan",
        .read_permission = MOSAIC_CAP_NET_WIFI_READ,
        .read_contract = &s_wifi_scan_contract,
    },
    {
        .name = "net.provisioning",
        .read_permission = MOSAIC_CAP_NET_PROVISIONING_READ,
        .read_contract = &s_provisioning_contract,
        MOSAIC_CAP_COMMANDS(s_provisioning_commands),
    },
    {
        .name = "config.agent",
        .read_permission = MOSAIC_CAP_CONFIG_AGENT_READ,
        .read_contract = &s_agent_config_contract,
    },
    {
        .name = "media.bluetooth",
        .read_permission = MOSAIC_CAP_MEDIA_BLUETOOTH_READ,
        .read_contract = &s_bluetooth_contract,
        MOSAIC_CAP_COMMANDS(s_bluetooth_commands),
        .publishes = true,
    },
    {
        .name = "media.player",
        .read_permission = MOSAIC_CAP_MEDIA_PLAYER_READ,
        .read_contract = &s_player_contract,
        MOSAIC_CAP_COMMANDS(s_player_commands),
    },
    {
        .name = "net.weather",
        .read_permission = MOSAIC_CAP_NET_WEATHER_READ,
        .read_contract = &s_weather_contract,
        MOSAIC_CAP_COMMANDS(s_weather_commands),
        .publishes = true,
    },
};

static const mosaic_capability_permission_def_t s_permissions[] = {
    { "sensor.imu.read", MOSAIC_CAP_SENSOR_IMU_READ },
    { "system.battery.read", MOSAIC_CAP_SYSTEM_BATTERY_READ },
    { "system.time.read", MOSAIC_CAP_SYSTEM_TIME_READ },
    { "system.status.read", MOSAIC_CAP_SYSTEM_STATUS_READ },
    { "system.display.read", MOSAIC_CAP_SYSTEM_DISPLAY_READ },
    { "system.display.control", MOSAIC_CAP_SYSTEM_DISPLAY_CONTROL },
    { "system.audio.read", MOSAIC_CAP_SYSTEM_AUDIO_READ },
    { "system.audio.control", MOSAIC_CAP_SYSTEM_AUDIO_CONTROL },
    { "system.haptic.read", MOSAIC_CAP_SYSTEM_HAPTIC_READ },
    { "system.haptic.control", MOSAIC_CAP_SYSTEM_HAPTIC_CONTROL },
    { "system.power.read", MOSAIC_CAP_SYSTEM_POWER_READ },
    { "system.update.read", MOSAIC_CAP_SYSTEM_UPDATE_READ },
    { "system.update.control", MOSAIC_CAP_SYSTEM_UPDATE_CONTROL },
    { "system.lifecycle.read", MOSAIC_CAP_SYSTEM_LIFECYCLE_READ },
    { "system.lifecycle.control", MOSAIC_CAP_SYSTEM_LIFECYCLE_CONTROL },
    { "net.wifi.read", MOSAIC_CAP_NET_WIFI_READ },
    { "net.wifi.control", MOSAIC_CAP_NET_WIFI_CONTROL },
    { "net.provisioning.read", MOSAIC_CAP_NET_PROVISIONING_READ },
    { "net.provisioning.control", MOSAIC_CAP_NET_PROVISIONING_CONTROL },
    { "config.agent.read", MOSAIC_CAP_CONFIG_AGENT_READ },
    { "media.bluetooth.read", MOSAIC_CAP_MEDIA_BLUETOOTH_READ },
    { "media.bluetooth.control", MOSAIC_CAP_MEDIA_BLUETOOTH_CONTROL },
    { "media.player.read", MOSAIC_CAP_MEDIA_PLAYER_READ },
    { "media.player.control", MOSAIC_CAP_MEDIA_PLAYER_CONTROL },
    { "net.weather.read", MOSAIC_CAP_NET_WEATHER_READ },
    { "net.weather.control", MOSAIC_CAP_NET_WEATHER_CONTROL },
};

const mosaic_capability_declaration_t *mosaic_capability_declarations(
    size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(s_declarations) / sizeof(s_declarations[0]);
    }
    return s_declarations;
}

const mosaic_capability_declaration_t *mosaic_capability_declaration_for(
    const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    const size_t count = sizeof(s_declarations) / sizeof(s_declarations[0]);
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(s_declarations[index].name, name) == 0) {
            return &s_declarations[index];
        }
    }
    return NULL;
}

const mosaic_capability_permission_def_t *mosaic_capability_permissions(
    size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(s_permissions) / sizeof(s_permissions[0]);
    }
    return s_permissions;
}
