/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* Payload structs shared by capability providers and capability consumers.
 *
 * One section per domain. Adding a domain touches this header and the
 * registry declaration table only; no other file in the capability layer
 * changes, and no script bridge changes.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- sensor.imu ---------------- */

typedef struct {
    float pitch_deg;
    float roll_deg;
    float yaw_deg;
} mosaic_cap_orientation_t;

/* ---------------- system.time ---------------- */

typedef struct {
    int64_t unix_seconds;
    int32_t utc_offset_minutes;
} mosaic_cap_time_t;

/* ---------------- system.display ---------------- */

typedef struct {
    bool available;
    int32_t brightness;
    uint32_t rotation_degrees;
    uint32_t screen_timeout_ms;
} mosaic_cap_display_t;

typedef struct {
    int32_t brightness;
    bool persist;
} mosaic_cap_display_brightness_args_t;

typedef struct {
    uint32_t degrees;
} mosaic_cap_display_rotation_args_t;

typedef struct {
    uint32_t timeout_ms;
} mosaic_cap_display_timeout_args_t;

typedef enum {
    MOSAIC_CAP_DISPLAY_CMD_SET_BRIGHTNESS = 0,
    MOSAIC_CAP_DISPLAY_CMD_SET_ROTATION,
    MOSAIC_CAP_DISPLAY_CMD_SET_SCREEN_TIMEOUT,
} mosaic_cap_display_command_t;

/* ---------------- system.audio ---------------- */

typedef struct {
    bool available;
    int32_t volume;
} mosaic_cap_audio_t;

typedef struct {
    int32_t volume;
    bool persist;
} mosaic_cap_audio_volume_args_t;

typedef enum {
    MOSAIC_CAP_AUDIO_CMD_SET_VOLUME = 0,
} mosaic_cap_audio_command_t;

/* ---------------- system.haptic ---------------- */

typedef struct {
    bool enabled;
} mosaic_cap_haptic_t;

typedef struct {
    bool enabled;
} mosaic_cap_haptic_enable_args_t;

typedef enum {
    MOSAIC_CAP_HAPTIC_CMD_SET_ENABLED = 0,
    MOSAIC_CAP_HAPTIC_CMD_PULSE,
} mosaic_cap_haptic_command_t;

/* ---------------- system.power ---------------- */

#define MOSAIC_CAP_VALUE_UNKNOWN_U32 UINT32_MAX

typedef struct {
    bool available;
    bool charging;
    uint32_t percent;
    uint32_t voltage_mv;
    int32_t current_ma;
    /** Gauge estimates in minutes; MOSAIC_CAP_VALUE_UNKNOWN_U32 when absent. */
    uint32_t time_to_empty_min;
    uint32_t time_to_full_min;
    uint32_t cycle_count;
    uint32_t state_of_health;
    /** Monotonic publish counter. */
    uint32_t sequence;
} mosaic_cap_power_t;

/* ---------------- system.update ---------------- */

#define MOSAIC_CAP_VERSION_LEN 32U
#define MOSAIC_CAP_UPDATE_TITLE_LEN 96U
#define MOSAIC_CAP_UPDATE_SUMMARY_LEN 256U
#define MOSAIC_CAP_UPDATE_PUBLISHED_AT_LEN 40U

typedef enum {
    MOSAIC_CAP_UPDATE_IDLE = 0,
    MOSAIC_CAP_UPDATE_CHECKING,
    MOSAIC_CAP_UPDATE_UP_TO_DATE,
    MOSAIC_CAP_UPDATE_AVAILABLE,
    MOSAIC_CAP_UPDATE_DEVICE_AHEAD,
    MOSAIC_CAP_UPDATE_FAILED,
} mosaic_cap_update_state_t;

typedef struct {
    int32_t state;
    char latest_version[MOSAIC_CAP_VERSION_LEN];
    char title[MOSAIC_CAP_UPDATE_TITLE_LEN];
    char summary[MOSAIC_CAP_UPDATE_SUMMARY_LEN];
    char published_at[MOSAIC_CAP_UPDATE_PUBLISHED_AT_LEN];
    uint32_t sequence;
    int32_t last_error;
} mosaic_cap_update_t;

typedef enum {
    MOSAIC_CAP_UPDATE_CMD_CHECK = 0,
} mosaic_cap_update_command_t;

/* ---------------- system.lifecycle ---------------- */

typedef struct {
    char software_version[MOSAIC_CAP_VERSION_LEN];
} mosaic_cap_lifecycle_t;

typedef enum {
    MOSAIC_CAP_LIFECYCLE_CMD_FACTORY_RESET = 0,
} mosaic_cap_lifecycle_command_t;

/* ---------------- net.wifi ---------------- */

#define MOSAIC_CAP_SSID_LEN 33U
#define MOSAIC_CAP_PASSWORD_LEN 64U
#define MOSAIC_CAP_IP_LEN 16U
#define MOSAIC_CAP_PORTAL_URL_LEN 48U
#define MOSAIC_CAP_WIFI_SCAN_MAX 20U

/** Values carried in mosaic_cap_wifi_t::state. */
typedef enum {
    MOSAIC_CAP_WIFI_DISABLED = 0,
    MOSAIC_CAP_WIFI_IDLE,
    MOSAIC_CAP_WIFI_SCANNING,
    MOSAIC_CAP_WIFI_CONNECTING,
    MOSAIC_CAP_WIFI_CONNECTED,
    MOSAIC_CAP_WIFI_RETRY_WAIT,
    MOSAIC_CAP_WIFI_AUTH_FAILED,
    MOSAIC_CAP_WIFI_AP_NOT_FOUND,
    MOSAIC_CAP_WIFI_FAILED,
} mosaic_cap_wifi_state_t;

/** Values carried in mosaic_cap_wifi_t::radio_state. */
typedef enum {
    MOSAIC_CAP_WIFI_RADIO_OFF = 0,
    MOSAIC_CAP_WIFI_RADIO_STARTING,
    MOSAIC_CAP_WIFI_RADIO_ON,
    MOSAIC_CAP_WIFI_RADIO_STOPPING,
    MOSAIC_CAP_WIFI_RADIO_ERROR,
} mosaic_cap_wifi_radio_state_t;

/** Values carried in mosaic_cap_wifi_t::sta_state. */
typedef enum {
    MOSAIC_CAP_WIFI_STA_UNCONFIGURED = 0,
    MOSAIC_CAP_WIFI_STA_IDLE,
    MOSAIC_CAP_WIFI_STA_CONNECTING,
    MOSAIC_CAP_WIFI_STA_CONNECTED,
    MOSAIC_CAP_WIFI_STA_RETRY_WAIT,
    MOSAIC_CAP_WIFI_STA_AUTH_FAILED,
    MOSAIC_CAP_WIFI_STA_AP_NOT_FOUND,
    MOSAIC_CAP_WIFI_STA_ERROR,
} mosaic_cap_wifi_sta_state_t;

typedef struct {
    bool desired_enabled;
    bool enabled;
    bool connected;
    bool configured;
    bool ap_active;
    char ssid[MOSAIC_CAP_SSID_LEN];
    char ip[MOSAIC_CAP_IP_LEN];
    char portal_url[MOSAIC_CAP_PORTAL_URL_LEN];
    int32_t state;
    int32_t radio_state;
    int32_t sta_state;
    int32_t scan_state;
    uint32_t scan_revision;
    int32_t scan_error;
    uint32_t operation_id;
    uint32_t disconnect_reason;
    int32_t last_error;
    int32_t rssi;
} mosaic_cap_wifi_t;

typedef struct {
    char ssid[MOSAIC_CAP_SSID_LEN];
    int32_t rssi;
    bool secured;
} mosaic_cap_wifi_ap_t;

/* Scan results are a separate capability so the hot status read stays a
 * small fixed record. */
typedef struct {
    uint32_t count;
    mosaic_cap_wifi_ap_t entries[MOSAIC_CAP_WIFI_SCAN_MAX];
} mosaic_cap_wifi_scan_t;

typedef struct {
    bool enabled;
} mosaic_cap_wifi_enable_args_t;

typedef struct {
    char ssid[MOSAIC_CAP_SSID_LEN];
    char password[MOSAIC_CAP_PASSWORD_LEN];
} mosaic_cap_wifi_connect_args_t;

typedef enum {
    MOSAIC_CAP_WIFI_CMD_SET_ENABLED = 0,
    MOSAIC_CAP_WIFI_CMD_SCAN,
    MOSAIC_CAP_WIFI_CMD_CONNECT,
    MOSAIC_CAP_WIFI_CMD_FORGET,
} mosaic_cap_wifi_command_t;

/* ---------------- net.provisioning ---------------- */

#define MOSAIC_CAP_PHONE_QR_LEN 512U

typedef struct {
    char ap_ssid[MOSAIC_CAP_SSID_LEN];
    char qr_payload[MOSAIC_CAP_PHONE_QR_LEN];
} mosaic_cap_provisioning_t;

typedef enum {
    MOSAIC_CAP_PROVISIONING_CMD_RECONFIGURE = 0,
} mosaic_cap_provisioning_command_t;

/* ---------------- config.agent ---------------- */

#define MOSAIC_CAP_LLM_BACKEND_LEN 32U
#define MOSAIC_CAP_LLM_MODEL_LEN 64U
#define MOSAIC_CAP_LLM_URL_LEN 160U

typedef struct {
    char software_version[MOSAIC_CAP_VERSION_LEN];
    char llm_backend[MOSAIC_CAP_LLM_BACKEND_LEN];
    char llm_model[MOSAIC_CAP_LLM_MODEL_LEN];
    char llm_base_url[MOSAIC_CAP_LLM_URL_LEN];
    bool llm_api_key_configured;
    bool llm_supports_tools;
    bool llm_supports_vision;
    /** Trial obtains credentials on-device; other backends need an API key. */
    bool llm_configured;
    bool im_wechat_configured;
    bool im_qq_configured;
    bool im_feishu_configured;
    bool im_telegram_configured;
} mosaic_cap_agent_config_t;

/* ---------------- media.bluetooth ---------------- */

#define MOSAIC_CAP_BT_DEVICE_NAME_LEN 32U
#define MOSAIC_CAP_BT_TEXT_LEN 96U

typedef enum {
    MOSAIC_CAP_BT_STATE_OFF = 0,
    MOSAIC_CAP_BT_STATE_STARTING,
    MOSAIC_CAP_BT_STATE_DISCOVERABLE,
    MOSAIC_CAP_BT_STATE_CONNECTED,
    MOSAIC_CAP_BT_STATE_PLAYING,
    MOSAIC_CAP_BT_STATE_PAUSED,
    MOSAIC_CAP_BT_STATE_ERROR,
} mosaic_cap_bluetooth_state_t;

typedef struct {
    /** Bumps on any field change; the UI redraws only when it moves. */
    uint32_t revision;
    /** Bumps when a new cover blob becomes borrowable. */
    uint32_t cover_revision;
    int32_t state;
    bool connected;
    bool playing;
    bool has_cover;
    int32_t volume_percent;
    uint32_t position_ms;
    uint32_t duration_ms;
    char device_name[MOSAIC_CAP_BT_DEVICE_NAME_LEN];
    char title[MOSAIC_CAP_BT_TEXT_LEN];
    char artist[MOSAIC_CAP_BT_TEXT_LEN];
    char error[MOSAIC_CAP_BT_TEXT_LEN];
} mosaic_cap_bluetooth_t;

/** Blob ids borrowable from media.bluetooth. */
#define MOSAIC_CAP_BT_BLOB_COVER 0U

typedef struct {
    int32_t volume_percent;
} mosaic_cap_bluetooth_volume_args_t;

typedef enum {
    MOSAIC_CAP_BT_CMD_TOGGLE_PLAY = 0,
    MOSAIC_CAP_BT_CMD_PREVIOUS,
    MOSAIC_CAP_BT_CMD_NEXT,
    MOSAIC_CAP_BT_CMD_SET_VOLUME,
    /** Powers the A2DP sink down; the capability stays registered. */
    MOSAIC_CAP_BT_CMD_SHUTDOWN,
} mosaic_cap_bluetooth_command_t;

/* ---------------- media.player ---------------- */

#define MOSAIC_CAP_TRACK_TEXT_LEN 64U
#define MOSAIC_CAP_PLAYER_LIBRARY_ROWS 3U

typedef enum {
    MOSAIC_CAP_PLAYER_PAUSED = 0,
    MOSAIC_CAP_PLAYER_PLAYING,
    MOSAIC_CAP_PLAYER_STOPPED,
    MOSAIC_CAP_PLAYER_ERROR,
} mosaic_cap_player_state_t;

typedef struct {
    char title[MOSAIC_CAP_TRACK_TEXT_LEN];
    char artist[MOSAIC_CAP_TRACK_TEXT_LEN];
} mosaic_cap_track_t;

typedef struct {
    /** Bumps on any field change; the UI redraws only when it moves. */
    uint32_t revision;
    int32_t state;
    uint32_t track_index;
    uint32_t track_count;
    char title[MOSAIC_CAP_TRACK_TEXT_LEN];
    char artist[MOSAIC_CAP_TRACK_TEXT_LEN];
    char album[MOSAIC_CAP_TRACK_TEXT_LEN];
    int64_t position_ms;
    int64_t duration_ms;
    bool shuffle_enabled;
    /** Rows shown on the library page, in display order. */
    mosaic_cap_track_t library[MOSAIC_CAP_PLAYER_LIBRARY_ROWS];
} mosaic_cap_player_t;

typedef struct {
    uint32_t track_index;
} mosaic_cap_player_select_args_t;

typedef struct {
    uint32_t progress_permille;
} mosaic_cap_player_seek_args_t;

typedef struct {
    int64_t now_us;
} mosaic_cap_player_time_args_t;

typedef enum {
    MOSAIC_CAP_PLAYER_CMD_TOGGLE = 0,
    MOSAIC_CAP_PLAYER_CMD_NEXT,
    MOSAIC_CAP_PLAYER_CMD_PREVIOUS,
    MOSAIC_CAP_PLAYER_CMD_TOGGLE_SHUFFLE,
    MOSAIC_CAP_PLAYER_CMD_SELECT,
    MOSAIC_CAP_PLAYER_CMD_SEEK,
    MOSAIC_CAP_PLAYER_CMD_START,
    MOSAIC_CAP_PLAYER_CMD_STEP,
    /** Releases the decoder; the capability stays registered. */
    MOSAIC_CAP_PLAYER_CMD_CLOSE,
} mosaic_cap_player_command_t;

/* ---------------- net.weather ---------------- */

#define MOSAIC_CAP_WEATHER_CITY_LEN 48U
#define MOSAIC_CAP_WEATHER_SYMBOL_LEN 40U
#define MOSAIC_CAP_WEATHER_CONDITION_LEN 24U
#define MOSAIC_CAP_WEATHER_DATE_LEN 6U
#define MOSAIC_CAP_WEATHER_FORECAST_DAYS 7U

typedef struct {
    bool valid;
    /** False when the day has a single reading and no distinct low. */
    bool low_valid;
    int32_t temperature_min_c;
    int32_t temperature_max_c;
    char date[MOSAIC_CAP_WEATHER_DATE_LEN];
    char symbol_code[MOSAIC_CAP_WEATHER_SYMBOL_LEN];
    char condition[MOSAIC_CAP_WEATHER_CONDITION_LEN];
} mosaic_cap_weather_day_t;

typedef struct {
    bool online;
    bool location_valid;
    bool weather_valid;
    /** The reading is cached and older than the refresh interval. */
    bool stale;
    int32_t temperature_c;
    char city[MOSAIC_CAP_WEATHER_CITY_LEN];
    char symbol_code[MOSAIC_CAP_WEATHER_SYMBOL_LEN];
    char condition[MOSAIC_CAP_WEATHER_CONDITION_LEN];
    mosaic_cap_weather_day_t forecast[MOSAIC_CAP_WEATHER_FORECAST_DAYS];
    int64_t last_updated_epoch;
    /** Bumps on every completed refresh. */
    uint32_t sequence;
} mosaic_cap_weather_t;

typedef enum {
    MOSAIC_CAP_WEATHER_CMD_REFRESH = 0,
} mosaic_cap_weather_command_t;

#ifdef __cplusplus
}
#endif
