/* Air Voice (whisper capture) test App for ESP-MOSAICO.
 *
 * A thin view over air_voice_service: the service owns the microphone,
 * enhancement chain, recording and playback so the web admin page can drive
 * the same session. This App only renders the published status and forwards
 * button presses as non-blocking requests.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "air_voice_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"
#include "air_voice_actions.h"
#include "air_voice_binds.h"
#include "air_voice_objects.h"

#define AIR_VOICE_APP_ID        45U
#define METER_FLOOR_DBFS        (-60.0f)
#define VOLUME_STEP             10
#define DELETE_CONFIRM_US       (3LL * 1000 * 1000)
#define NOTICE_US               (2LL * 1000 * 1000)
#define START_RETRY_US          (1000LL * 1000)
#define TEXT_MAX                96U

static const char *TAG = "air_voice_app";

typedef struct {
    bool started;
    uint32_t start_failures;
    int64_t start_retry_at_us;
    air_voice_track_t track;
    int64_t delete_armed_until_us;
    int64_t notice_until_us;
    char notice[TEXT_MAX];
} air_voice_view_t;

static air_voice_view_t s_view;

static void notice(const char *text)
{
    strlcpy(s_view.notice, text, sizeof(s_view.notice));
    s_view.notice_until_us = esp_timer_get_time() + NOTICE_US;
}

static int meter_percent(float dbfs)
{
    const float pct = (dbfs - METER_FLOOR_DBFS) * 100.0f / -METER_FLOOR_DBFS;
    return pct < 0.0f ? 0 : (pct > 100.0f ? 100 : (int)lroundf(pct));
}

static void format_clock(char *out, size_t size, uint32_t ms)
{
    snprintf(out, size, "%02" PRIu32 ":%02" PRIu32 ".%" PRIu32,
             ms / 60000U, (ms / 1000U) % 60U, (ms / 100U) % 10U);
}

static const char *preset_title(const char *preset)
{
    if (strcmp(preset, "whisper_near") == 0) return "WHISPER 3-5CM";
    if (strcmp(preset, "whisper_noisy") == 0) return "NOISY 3-5CM";
    if (strcmp(preset, "raw") == 0) return "RAW BYPASS";
    return "CUSTOM";
}

static const char *hint_text(air_voice_hint_t hint)
{
    switch (hint) {
    case AIR_VOICE_HINT_CLIPPING: return "TOO LOUD - LOWER GAIN";
    case AIR_VOICE_HINT_TOO_QUIET: return "TOO QUIET - MOVE CLOSER";
    case AIR_VOICE_HINT_LOW_SNR: return "NOISY - MOVE CLOSER";
    case AIR_VOICE_HINT_AIRFLOW: return "BREATH HITS MIC - TILT IT";
    case AIR_VOICE_HINT_GOOD: return "LEVEL GOOD";
    default: return "WHISPER INTO THE MIC";
    }
}

static void render_status_line(esp_gsp_handle_t ui, const air_voice_status_t *st)
{
    char text[TEXT_MAX];
    char a[24];
    char b[24];
    if (st->state == AIR_VOICE_STATE_RECORDING) {
        format_clock(a, sizeof(a), st->record_ms);
        snprintf(text, sizeof(text), "REC %s", a);
    } else if (st->state == AIR_VOICE_STATE_SAVING) {
        strlcpy(text, "SAVING...", sizeof(text));
    } else if (st->play_state == AIR_VOICE_PLAY_PLAYING || st->play_state == AIR_VOICE_PLAY_PAUSED) {
        format_clock(a, sizeof(a), (uint32_t)(st->play_position_ms > 0 ? st->play_position_ms : 0));
        format_clock(b, sizeof(b), (uint32_t)(st->play_duration_ms > 0 ? st->play_duration_ms : 0));
        snprintf(text, sizeof(text), "%s %s/%s",
                 st->play_state == AIR_VOICE_PLAY_PAUSED ? "PAUSED" : "PLAY", a, b);
    } else if (st->play_state == AIR_VOICE_PLAY_ERROR) {
        strlcpy(text, "PLAYBACK ERROR", sizeof(text));
    } else if (st->mic_busy) {
        strlcpy(text, "MIC BUSY", sizeof(text));
    } else if (!st->mic_open) {
        strlcpy(text, "STARTING MIC", sizeof(text));
    } else {
        strlcpy(text, "READY", sizeof(text));
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_AV_STATUS, text);
}

static void render_meters(esp_gsp_handle_t ui, const air_voice_status_t *st)
{
    const air_voice_meter_t *m = &st->meter;
    char text[TEXT_MAX];
    const bool live = st->mic_open;
    (void)esp_gsp_set_value(ui, GSP_BIND_AV_IN_BAR, live ? meter_percent(m->in_rms_dbfs) : 0);
    (void)esp_gsp_set_value(ui, GSP_BIND_AV_OUT_BAR, live ? meter_percent(m->out_rms_dbfs) : 0);
    if (live) {
        snprintf(text, sizeof(text), "%d dB", (int)lroundf(m->in_rms_dbfs));
    } else {
        strlcpy(text, "-- dB", sizeof(text));
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_AV_IN_TEXT, text);
    if (live) {
        snprintf(text, sizeof(text), "%d dB", (int)lroundf(m->out_rms_dbfs));
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_AV_OUT_TEXT, text);

    if (live && m->speech_frames > 0) {
        snprintf(text, sizeof(text), "NOISE %d dB   SNR %d dB",
                 (int)lroundf(m->noise_dbfs), (int)lroundf(m->snr_db));
    } else if (live) {
        snprintf(text, sizeof(text), "NOISE %d dB   SNR --", (int)lroundf(m->noise_dbfs));
    } else {
        strlcpy(text, "NOISE -- dB   SNR --", sizeof(text));
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_AV_NOISE, text);

    const uint32_t classified = m->whisper_frames + m->voiced_frames;
    char whisper[16];
    if (classified > 0) {
        snprintf(whisper, sizeof(whisper), "%" PRIu32 "%%", 100U * m->whisper_frames / classified);
    } else {
        strlcpy(whisper, "--", sizeof(whisper));
    }
    char airflow[16];
    if (m->speech_frames > 0) {
        snprintf(airflow, sizeof(airflow), "%" PRIu32 "%%", 100U * m->airflow_frames / m->speech_frames);
    } else {
        strlcpy(airflow, "--", sizeof(airflow));
    }
    snprintf(text, sizeof(text), "WHISPER %s  AIR %s  AGC %+d", whisper, airflow,
             (int)lroundf(m->agc_gain_db));
    (void)esp_gsp_set_text(ui, GSP_BIND_AV_DETAIL, text);
    (void)esp_gsp_set_visible(ui, GSP_BIND_AV_VAD, live && m->vad);
}

static void render(esp_gsp_handle_t ui)
{
    air_voice_status_t st;
    if (air_voice_get_status(&st) != ESP_OK) {
        (void)esp_gsp_set_text(ui, GSP_BIND_AV_STATUS, "SERVICE OFFLINE");
        return;
    }
    air_voice_config_t config;
    char text[TEXT_MAX];
    if (air_voice_get_config(&config) == ESP_OK) {
        snprintf(text, sizeof(text), "%s | GAIN %d+%d dB", preset_title(config.preset),
                 config.pga_db, config.adc_gain_db);
        (void)esp_gsp_set_text(ui, GSP_BIND_AV_MODE, text);
    }
    render_status_line(ui, &st);

    if (st.last_name[0]) {
        char clock[24];
        format_clock(clock, sizeof(clock), st.last_duration_ms);
        snprintf(text, sizeof(text), "LAST %s  %s", st.last_name, clock);
    } else {
        strlcpy(text, "NO RECORDINGS", sizeof(text));
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_AV_LAST, text);
    render_meters(ui, &st);

    const int64_t now = esp_timer_get_time();
    if (now < s_view.delete_armed_until_us) {
        (void)esp_gsp_set_text(ui, GSP_BIND_AV_HINT, "TAP DEL AGAIN TO DELETE");
    } else if (now < s_view.notice_until_us) {
        (void)esp_gsp_set_text(ui, GSP_BIND_AV_HINT, s_view.notice);
    } else {
        (void)esp_gsp_set_text(ui, GSP_BIND_AV_HINT, hint_text(st.hint));
    }

    const bool recording = st.state == AIR_VOICE_STATE_RECORDING;
    (void)esp_gsp_set_visible(ui, GSP_BIND_AV_REC_IDLE, !recording);
    (void)esp_gsp_set_visible(ui, GSP_BIND_AV_REC_ACTIVE, recording);
    const bool playing = st.play_state == AIR_VOICE_PLAY_PLAYING ||
        st.play_state == AIR_VOICE_PLAY_PAUSED;
    (void)esp_gsp_set_text(ui, GSP_BIND_AV_PLAY_TEXT, playing ? "STOP" : "PLAY");
    (void)esp_gsp_set_text(ui, GSP_BIND_AV_TRACK_TEXT,
                           s_view.track == AIR_VOICE_TRACK_RAW ? "RAW" : "PROC");
}

static void change_volume(int delta)
{
    air_voice_status_t st;
    if (air_voice_get_status(&st) != ESP_OK || st.volume < 0) {
        return;
    }
    int volume = st.volume + delta;
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    if (air_voice_set_volume(volume) == ESP_OK) {
        char text[TEXT_MAX];
        snprintf(text, sizeof(text), "VOLUME %d%%", volume);
        notice(text);
    }
}

static void delete_last(void)
{
    const int64_t now = esp_timer_get_time();
    if (now >= s_view.delete_armed_until_us) {
        s_view.delete_armed_until_us = now + DELETE_CONFIRM_US;
        return;
    }
    s_view.delete_armed_until_us = 0;
    air_voice_status_t st;
    if (air_voice_get_status(&st) != ESP_OK || !st.last_name[0]) {
        notice("NOTHING TO DELETE");
        return;
    }
    const esp_err_t err = air_voice_delete_recording(st.last_name);
    notice(err == ESP_OK ? "DELETED" : "DELETE FAILED");
}

static void handle_call(uint16_t action)
{
    if (action != GSP_ACT_ID_AV_DELETE) {
        s_view.delete_armed_until_us = 0;
    }
    esp_err_t err = ESP_OK;
    switch (action) {
    case GSP_ACT_ID_AV_RECORD:
        err = air_voice_request_record_toggle();
        break;
    case GSP_ACT_ID_AV_PLAY:
        err = air_voice_request_play_toggle(s_view.track);
        break;
    case GSP_ACT_ID_AV_TRACK:
        s_view.track = s_view.track == AIR_VOICE_TRACK_RAW ? AIR_VOICE_TRACK_PROCESSED
                                                           : AIR_VOICE_TRACK_RAW;
        notice(s_view.track == AIR_VOICE_TRACK_RAW ? "PLAY RAW MIC TRACK"
                                                   : "PLAY PROCESSED TRACK");
        break;
    case GSP_ACT_ID_AV_MODE_NEXT:
        err = air_voice_next_preset();
        break;
    case GSP_ACT_ID_AV_DELETE:
        delete_last();
        break;
    case GSP_ACT_ID_AV_VOL_DOWN:
        change_volume(-VOLUME_STEP);
        break;
    case GSP_ACT_ID_AV_VOL_UP:
        change_volume(VOLUME_STEP);
        break;
    default:
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "action %u failed: %s", action, esp_err_to_name(err));
        notice("BUSY - TRY AGAIN");
    }
}

/* The App can be restored at boot before the audio hub runs, so a failed
 * start is retried from the tick instead of leaving the buttons dead. */
static void try_start(void)
{
    const esp_err_t err = air_voice_service_start();
    if (err != ESP_OK) {
        if (s_view.start_failures++ == 0) {
            ESP_LOGW(TAG, "service start failed: %s; retrying", esp_err_to_name(err));
        }
        s_view.start_retry_at_us = esp_timer_get_time() + START_RETRY_US;
        return;
    }
    if (s_view.start_failures > 0) {
        ESP_LOGI(TAG, "service started after %" PRIu32 " retries", s_view.start_failures);
    }
    s_view.started = true;
    s_view.start_failures = 0;
    (void)air_voice_hold(AIR_VOICE_CLIENT_UI, true);
}

static void air_voice_event(esp_gsp_handle_t ui, const struct mosaic_event *raw)
{
    const mosaic_event_t *event = raw;
    if (!event) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
        try_start();
        render(ui);
        break;
    case MOSAIC_EVENT_TIMER:
        if (!s_view.started && esp_timer_get_time() >= s_view.start_retry_at_us) {
            try_start();
        }
        render(ui);
        break;
    case MOSAIC_EVENT_UI_CALL:
        if (s_view.started) {
            handle_call(event->data.call.action_id);
            render(ui);
        }
        break;
    case MOSAIC_EVENT_STOP:
        /* A recording in progress keeps running; it can be stopped from the
         * web page or when the App is opened again. */
        if (s_view.started) {
            (void)air_voice_hold(AIR_VOICE_CLIENT_UI, false);
        }
        s_view.started = false;
        s_view.start_failures = 0;
        break;
    default:
        break;
    }
}

const mosaic_app_descriptor_t mosaic_air_voice_app = {
    .id = AIR_VOICE_APP_ID,
    .launch_action = GSP_ACT_ID_APP_AIR_VOICE,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "air_voice",
    .title = "Air Voice",
    .directory = &gsp_obj_directory_air_voice,
    .disable_swipe = true,
    .on_event = air_voice_event,
};
