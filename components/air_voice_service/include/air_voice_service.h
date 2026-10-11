/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* Air Voice (whispered speech) capture service.
 *
 * Owns the microphone subscriber, the whisper enhancement chain, WAV
 * recording (processed + raw), on-device playback, persistent settings and a
 * log ring. It runs independently of any UI: the on-device App and the web
 * admin page are both thin clients of this API. All functions are thread
 * safe and may be called from any task once air_voice_service_start() has
 * returned ESP_OK.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIR_VOICE_NAME_MAX      48U
#define AIR_VOICE_PRESET_MAX    16U
#define AIR_VOICE_LOG_TEXT_MAX  168U
#define AIR_VOICE_SAMPLE_RATE   16000U

/* ES8311 gain staging: analog PGA in 3 dB steps, digital ADC scale in 6 dB
 * steps. The factory capture default is 30 + 42 = 72 dB, which clips even a
 * close whisper. */
#define AIR_VOICE_PGA_DB_MAX    30
#define AIR_VOICE_PGA_DB_STEP   3
#define AIR_VOICE_ADC_DB_MAX    42
#define AIR_VOICE_ADC_DB_STEP   6

typedef enum {
    AIR_VOICE_CLIENT_UI = 0,   /* on-device App: explicit hold/release */
    AIR_VOICE_CLIENT_WEB,      /* web page: lease refreshed by polling */
} air_voice_client_t;

typedef enum {
    AIR_VOICE_STATE_IDLE = 0,  /* microphone closed */
    AIR_VOICE_STATE_MONITOR,   /* microphone open, metering only */
    AIR_VOICE_STATE_RECORDING,
    AIR_VOICE_STATE_SAVING,    /* draining buffers / finalizing files */
} air_voice_state_t;

typedef enum {
    AIR_VOICE_PLAY_IDLE = 0,
    AIR_VOICE_PLAY_PLAYING,
    AIR_VOICE_PLAY_PAUSED,
    AIR_VOICE_PLAY_FINISHED,
    AIR_VOICE_PLAY_ERROR,
} air_voice_play_state_t;

typedef enum {
    AIR_VOICE_TRACK_PROCESSED = 0,
    AIR_VOICE_TRACK_RAW,
} air_voice_track_t;

/** Operator guidance derived from the live meters. */
typedef enum {
    AIR_VOICE_HINT_NONE = 0,   /* no recent speech */
    AIR_VOICE_HINT_GOOD,
    AIR_VOICE_HINT_CLIPPING,   /* input reached full scale: lower the gain */
    AIR_VOICE_HINT_TOO_QUIET,  /* speech far below range: raise gain/move closer */
    AIR_VOICE_HINT_LOW_SNR,    /* speech close to the noise floor: move closer */
    AIR_VOICE_HINT_AIRFLOW,    /* breath hits the mic port: angle it off the mouth */
} air_voice_hint_t;

typedef struct {
    char preset[AIR_VOICE_PRESET_MAX]; /* informational: last applied preset */
    /* Codec gain staging. */
    int pga_db;                /* 0..30, step 3 (analog) */
    int adc_gain_db;           /* 0..42, step 6 (digital) */
    /* Enhancement chain. */
    int hpf_hz;                /* 0 = off, else 40..400 */
    bool ns_enable;
    float ns_depth_db;         /* 0..30 */
    bool eq_enable;
    float eq_presence_db;      /* -6..12 */
    bool agc_enable;
    float agc_target_dbfs;     /* -36..-6 */
    float agc_max_gain_db;     /* 0..42 */
    float manual_gain_db;      /* -20..40, when the AGC is off */
    bool limiter_enable;
    float vad_threshold_db;    /* 1..12 */
    float gate_db;             /* 0..20 */
    bool airflow_enable;       /* breath airflow suppression (close talk) */
    float airflow_depth_db;    /* 0..36 */
    /* Recording. */
    bool save_raw;             /* also keep the unprocessed microphone track */
    int max_record_s;          /* 10..3600 */
} air_voice_config_t;

typedef struct {
    float in_rms_dbfs;
    float in_peak_dbfs;
    float out_rms_dbfs;
    float out_peak_dbfs;
    float noise_dbfs;
    float speech_dbfs;
    float snr_db;
    float agc_gain_db;
    float voicing;             /* 0..1, low = whisper, high = voiced */
    bool vad;
    bool airflow;              /* breath airflow in the latest frame */
    uint64_t clipped;          /* since the microphone opened / recording began */
    uint32_t speech_frames;
    uint32_t whisper_frames;
    uint32_t voiced_frames;
    uint32_t airflow_frames;   /* speech frames dominated by breath airflow */
} air_voice_meter_t;

typedef struct {
    air_voice_state_t state;
    bool mic_open;
    bool mic_busy;             /* another feature holds the App capture role */
    bool ui_hold;
    bool web_lease;
    int applied_pga_db;        /* read back from the codec, -1 if unknown */
    int applied_adc_gain_db;

    char record_name[AIR_VOICE_NAME_MAX];
    uint32_t record_ms;
    uint64_t record_bytes;     /* processed track PCM bytes written */
    uint64_t dropped_bytes;    /* capture + buffer overflow during recording */
    esp_err_t record_error;

    char last_name[AIR_VOICE_NAME_MAX];
    uint32_t last_duration_ms;

    air_voice_play_state_t play_state;
    char play_name[AIR_VOICE_NAME_MAX];
    air_voice_track_t play_track;
    int64_t play_position_ms;
    int64_t play_duration_ms;
    esp_err_t play_error;

    air_voice_meter_t meter;
    air_voice_hint_t hint;
    int volume;                /* speaker volume 0..100, -1 if unknown */
    uint64_t storage_free_bytes;
    uint64_t storage_total_bytes;
    uint32_t log_seq;          /* latest log sequence number */
} air_voice_status_t;

typedef struct {
    char name[AIR_VOICE_NAME_MAX];
    uint32_t duration_ms;
    uint64_t bytes;            /* processed track size */
    uint64_t raw_bytes;        /* 0 when no raw track */
    bool has_raw;
    bool has_meta;
    int64_t mtime;             /* seconds since epoch (0 if unknown) */
} air_voice_recording_t;

typedef struct {
    uint32_t seq;
    uint32_t ts_ms;            /* milliseconds since boot */
    char level;                /* 'E' 'W' 'I' 'D' */
    char text[AIR_VOICE_LOG_TEXT_MAX];
} air_voice_log_line_t;

/** Optional integration with the product's persistent volume setting. When
 * absent, the shared mixer volume is used directly (not persisted). */
typedef struct {
    esp_err_t (*set_volume)(void *ctx, int volume);
    esp_err_t (*get_volume)(void *ctx, int *out_volume);
    void *ctx;
} air_voice_volume_hooks_t;

/** Idempotent. Loads settings, recovers interrupted recordings and starts the
 * worker tasks. Requires the shared audio hub to be running. */
esp_err_t air_voice_service_start(void);
bool air_voice_service_is_started(void);

/** Register volume hooks (may be called before start). */
void air_voice_service_set_volume_hooks(const air_voice_volume_hooks_t *hooks);

/** Keep the microphone open for metering. UI clients hold/release; the web
 * client refreshes a short lease with hold=true on every poll. */
esp_err_t air_voice_hold(air_voice_client_t client, bool hold);

esp_err_t air_voice_record_start(void);
esp_err_t air_voice_record_stop(void);
esp_err_t air_voice_record_toggle(void);

/** Play a recording on the device speaker. name NULL/"" plays the latest. */
esp_err_t air_voice_play(const char *name, air_voice_track_t track);
esp_err_t air_voice_play_stop(void);
esp_err_t air_voice_play_pause_toggle(void);

/** Non-blocking variants for UI threads: they return once the request is
 * queued; the outcome is visible through the status and the log. */
esp_err_t air_voice_request_record_toggle(void);
/** Stop playback if active, otherwise play the latest recording. */
esp_err_t air_voice_request_play_toggle(air_voice_track_t track);

esp_err_t air_voice_set_volume(int volume);

esp_err_t air_voice_get_status(air_voice_status_t *out_status);
esp_err_t air_voice_get_config(air_voice_config_t *out_config);
/** Validate, apply live and persist. Out-of-range fields are clamped. */
esp_err_t air_voice_set_config(const air_voice_config_t *config);
/** Apply a named preset ("whisper_near", "whisper_noisy", "raw"). */
esp_err_t air_voice_apply_preset(const char *preset);
/** Preset names for UI listing; returns the number of presets. */
size_t air_voice_preset_names(const char *const **out_names);
/** Cycle to the next preset (on-device mode button). */
esp_err_t air_voice_next_preset(void);

/** List recordings, newest first. Returns the number written to @p out;
 * @p out_total (optional) receives the total available. */
size_t air_voice_list_recordings(air_voice_recording_t *out, size_t max,
                                 size_t *out_total);
esp_err_t air_voice_delete_recording(const char *name);
/** Resolve the absolute path of a recording track or its metadata JSON. */
esp_err_t air_voice_recording_path(const char *name, air_voice_track_t track,
                                   char *out_path, size_t out_size);
esp_err_t air_voice_meta_path(const char *name, char *out_path, size_t out_size);
/** Recordings carry streaming WAV headers (sizes 0xFFFFFFFF, so finishing a
 * take never rewrites the file). Fill the real sizes into a copy of the first
 * 44 bytes before serving it; other headers are left untouched. */
void air_voice_wav_fix_header(uint8_t header[44], uint64_t file_bytes);
/** True when @p name is a safe recording base name. */
bool air_voice_name_is_valid(const char *name);

/** Copy log lines with seq > after_seq (oldest first). Returns the count. */
size_t air_voice_log_read(uint32_t after_seq, air_voice_log_line_t *out, size_t max);
void air_voice_log_clear(void);

const char *air_voice_state_name(air_voice_state_t state);
const char *air_voice_play_state_name(air_voice_play_state_t state);
const char *air_voice_hint_name(air_voice_hint_t hint);

#ifdef __cplusplus
}
#endif
