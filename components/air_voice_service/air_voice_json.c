/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "air_voice_json.h"

#include <math.h>

static double round1(float v)
{
    return round((double)v * 10.0) / 10.0;
}

void air_voice_status_to_json(const air_voice_status_t *st, cJSON *o)
{
    cJSON_AddStringToObject(o, "state", air_voice_state_name(st->state));
    cJSON_AddBoolToObject(o, "mic_open", st->mic_open);
    cJSON_AddBoolToObject(o, "mic_busy", st->mic_busy);
    cJSON_AddBoolToObject(o, "ui_hold", st->ui_hold);
    cJSON_AddBoolToObject(o, "web_lease", st->web_lease);
    cJSON_AddNumberToObject(o, "applied_pga_db", st->applied_pga_db);
    cJSON_AddNumberToObject(o, "applied_adc_gain_db", st->applied_adc_gain_db);

    cJSON *rec = cJSON_AddObjectToObject(o, "record");
    if (rec) {
        cJSON_AddStringToObject(rec, "name", st->record_name);
        cJSON_AddNumberToObject(rec, "ms", st->record_ms);
        cJSON_AddNumberToObject(rec, "bytes", (double)st->record_bytes);
        cJSON_AddNumberToObject(rec, "dropped_bytes", (double)st->dropped_bytes);
        cJSON_AddStringToObject(rec, "error", st->record_error == ESP_OK ? "" : esp_err_to_name(st->record_error));
        cJSON_AddStringToObject(rec, "last_name", st->last_name);
        cJSON_AddNumberToObject(rec, "last_duration_ms", st->last_duration_ms);
    }

    cJSON *play = cJSON_AddObjectToObject(o, "play");
    if (play) {
        cJSON_AddStringToObject(play, "state", air_voice_play_state_name(st->play_state));
        cJSON_AddStringToObject(play, "name", st->play_name);
        cJSON_AddStringToObject(play, "track", st->play_track == AIR_VOICE_TRACK_RAW ? "raw" : "processed");
        cJSON_AddNumberToObject(play, "position_ms", (double)st->play_position_ms);
        cJSON_AddNumberToObject(play, "duration_ms", (double)st->play_duration_ms);
        cJSON_AddStringToObject(play, "error", st->play_error == ESP_OK ? "" : esp_err_to_name(st->play_error));
    }

    const air_voice_meter_t *m = &st->meter;
    cJSON *meter = cJSON_AddObjectToObject(o, "meter");
    if (meter) {
        cJSON_AddNumberToObject(meter, "in_rms_dbfs", round1(m->in_rms_dbfs));
        cJSON_AddNumberToObject(meter, "in_peak_dbfs", round1(m->in_peak_dbfs));
        cJSON_AddNumberToObject(meter, "out_rms_dbfs", round1(m->out_rms_dbfs));
        cJSON_AddNumberToObject(meter, "out_peak_dbfs", round1(m->out_peak_dbfs));
        cJSON_AddNumberToObject(meter, "noise_dbfs", round1(m->noise_dbfs));
        cJSON_AddNumberToObject(meter, "speech_dbfs", round1(m->speech_dbfs));
        cJSON_AddNumberToObject(meter, "snr_db", round1(m->snr_db));
        cJSON_AddNumberToObject(meter, "agc_gain_db", round1(m->agc_gain_db));
        cJSON_AddNumberToObject(meter, "voicing", round((double)m->voicing * 100.0) / 100.0);
        cJSON_AddBoolToObject(meter, "vad", m->vad);
        cJSON_AddBoolToObject(meter, "airflow", m->airflow);
        cJSON_AddNumberToObject(meter, "airflow_frames", m->airflow_frames);
        cJSON_AddNumberToObject(meter, "clipped", (double)m->clipped);
        cJSON_AddNumberToObject(meter, "speech_frames", m->speech_frames);
        cJSON_AddNumberToObject(meter, "whisper_frames", m->whisper_frames);
        cJSON_AddNumberToObject(meter, "voiced_frames", m->voiced_frames);
    }
    cJSON_AddStringToObject(o, "hint", air_voice_hint_name(st->hint));
    cJSON_AddNumberToObject(o, "volume", st->volume);
    cJSON_AddNumberToObject(o, "storage_free_bytes", (double)st->storage_free_bytes);
    cJSON_AddNumberToObject(o, "storage_total_bytes", (double)st->storage_total_bytes);
    cJSON_AddNumberToObject(o, "log_seq", st->log_seq);
}

void air_voice_recording_to_json(const air_voice_recording_t *rec, cJSON *o)
{
    cJSON_AddStringToObject(o, "name", rec->name);
    cJSON_AddNumberToObject(o, "duration_ms", rec->duration_ms);
    cJSON_AddNumberToObject(o, "bytes", (double)rec->bytes);
    cJSON_AddNumberToObject(o, "raw_bytes", (double)rec->raw_bytes);
    cJSON_AddBoolToObject(o, "has_raw", rec->has_raw);
    cJSON_AddBoolToObject(o, "has_meta", rec->has_meta);
    cJSON_AddNumberToObject(o, "mtime", (double)rec->mtime);
}
