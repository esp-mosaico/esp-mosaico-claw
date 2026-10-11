/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "air_voice_config.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_voice_json.h"
#include "air_voice_log.h"

#define CONFIG_FILE_MAX 4096
/* Bump when presets change meaning: older saved settings are discarded so a
 * device picks up the retuned defaults instead of stale gains. */
#define CONFIG_VERSION  2

typedef struct {
    const char *name;
    air_voice_config_t config;
} av_preset_t;

/* Close-talk (3-5 cm) whisper, possibly behind a cupping hand: the target
 * scenario is dictating discreetly in an office or public place. Tuned on a
 * 3/5/10/15 cm bench series: at 3 cm whisper peaks near -13 dBFS with 36 dB of
 * gain, so 30 dB keeps ~19 dB of headroom for breath bursts (the codec noise
 * is still ~20 dB below the microphone's own), and most of the degradation
 * at that distance is breath airflow, handled by the airflow suppressor. */
static const av_preset_t PRESETS[] = {
    {"whisper_near", {
        .pga_db = 30, .adc_gain_db = 0, .hpf_hz = 120,
        .ns_enable = true, .ns_depth_db = 12, .eq_enable = true, .eq_presence_db = 4,
        .agc_enable = true, .agc_target_dbfs = -20, .agc_max_gain_db = 30,
        .manual_gain_db = 0, .limiter_enable = true, .vad_threshold_db = 3,
        .gate_db = 6, .airflow_enable = true, .airflow_depth_db = 24,
        .save_raw = true, .max_record_s = 600,
    }},
    /* Same distance in a louder place: deeper suppression and gating. */
    {"whisper_noisy", {
        .pga_db = 30, .adc_gain_db = 0, .hpf_hz = 150,
        .ns_enable = true, .ns_depth_db = 18, .eq_enable = true, .eq_presence_db = 4,
        .agc_enable = true, .agc_target_dbfs = -20, .agc_max_gain_db = 30,
        .manual_gain_db = 0, .limiter_enable = true, .vad_threshold_db = 4,
        .gate_db = 10, .airflow_enable = true, .airflow_depth_db = 30,
        .save_raw = true, .max_record_s = 600,
    }},
    /* Processing bypass: the "processed" track equals the raw microphone. */
    {"raw", {
        .pga_db = 30, .adc_gain_db = 0, .hpf_hz = 0,
        .ns_enable = false, .ns_depth_db = 12, .eq_enable = false, .eq_presence_db = 0,
        .agc_enable = false, .agc_target_dbfs = -20, .agc_max_gain_db = 30,
        .manual_gain_db = 0, .limiter_enable = false, .vad_threshold_db = 3,
        .gate_db = 0, .airflow_enable = false, .airflow_depth_db = 24,
        .save_raw = false, .max_record_s = 600,
    }},
};

#define PRESET_COUNT (sizeof(PRESETS) / sizeof(PRESETS[0]))

static const char *const PRESET_NAMES[PRESET_COUNT] = {
    "whisper_near", "whisper_noisy", "raw",
};

size_t air_voice_preset_names(const char *const **out_names)
{
    if (out_names) {
        *out_names = PRESET_NAMES;
    }
    return PRESET_COUNT;
}

esp_err_t air_voice_config_preset(const char *name, air_voice_config_t *config)
{
    if (!name || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < PRESET_COUNT; ++i) {
        if (strcmp(PRESETS[i].name, name) == 0) {
            *config = PRESETS[i].config;
            strlcpy(config->preset, name, sizeof(config->preset));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void air_voice_config_defaults(air_voice_config_t *config)
{
    (void)air_voice_config_preset(AIR_VOICE_DEFAULT_PRESET, config);
}

static int clamp_step(int v, int lo, int hi, int step)
{
    v = v < lo ? lo : (v > hi ? hi : v);
    return lo + ((v - lo + step / 2) / step) * step;
}

static float clamp_f(float v, float lo, float hi)
{
    if (isnan(v)) {
        return lo;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

void air_voice_config_sanitize(air_voice_config_t *c)
{
    if (!c) {
        return;
    }
    c->preset[sizeof(c->preset) - 1U] = '\0';
    c->pga_db = clamp_step(c->pga_db, 0, AIR_VOICE_PGA_DB_MAX, AIR_VOICE_PGA_DB_STEP);
    c->adc_gain_db = clamp_step(c->adc_gain_db, 0, AIR_VOICE_ADC_DB_MAX, AIR_VOICE_ADC_DB_STEP);
    c->hpf_hz = c->hpf_hz <= 0 ? 0 : clamp_step(c->hpf_hz, 40, 400, 1);
    c->ns_depth_db = clamp_f(c->ns_depth_db, 0, 30);
    c->eq_presence_db = clamp_f(c->eq_presence_db, -6, 12);
    c->agc_target_dbfs = clamp_f(c->agc_target_dbfs, -36, -6);
    c->agc_max_gain_db = clamp_f(c->agc_max_gain_db, 0, 42);
    c->manual_gain_db = clamp_f(c->manual_gain_db, -20, 40);
    c->vad_threshold_db = clamp_f(c->vad_threshold_db, 1, 12);
    c->gate_db = clamp_f(c->gate_db, 0, 20);
    c->airflow_depth_db = clamp_f(c->airflow_depth_db, 0, 36);
    c->max_record_s = clamp_step(c->max_record_s, 10, 3600, 1);
}

void air_voice_config_to_dsp(const air_voice_config_t *c, air_voice_dsp_config_t *out)
{
    air_voice_dsp_default_config(out);
    out->hpf_hz = (float)c->hpf_hz;
    out->ns_enable = c->ns_enable;
    out->ns_depth_db = c->ns_depth_db;
    out->eq_enable = c->eq_enable;
    out->eq_presence_db = c->eq_presence_db;
    out->agc_enable = c->agc_enable;
    out->agc_target_dbfs = c->agc_target_dbfs;
    out->agc_max_gain_db = c->agc_max_gain_db;
    out->manual_gain_db = c->manual_gain_db;
    out->limiter_enable = c->limiter_enable;
    out->vad_threshold_db = c->vad_threshold_db;
    out->gate_db = c->gate_db;
    out->airflow_enable = c->airflow_enable;
    out->airflow_depth_db = c->airflow_depth_db;
}

void air_voice_config_to_json(const air_voice_config_t *c, cJSON *o)
{
    cJSON_AddStringToObject(o, "preset", c->preset);
    cJSON_AddNumberToObject(o, "pga_db", c->pga_db);
    cJSON_AddNumberToObject(o, "adc_gain_db", c->adc_gain_db);
    cJSON_AddNumberToObject(o, "hpf_hz", c->hpf_hz);
    cJSON_AddBoolToObject(o, "ns_enable", c->ns_enable);
    cJSON_AddNumberToObject(o, "ns_depth_db", c->ns_depth_db);
    cJSON_AddBoolToObject(o, "eq_enable", c->eq_enable);
    cJSON_AddNumberToObject(o, "eq_presence_db", c->eq_presence_db);
    cJSON_AddBoolToObject(o, "agc_enable", c->agc_enable);
    cJSON_AddNumberToObject(o, "agc_target_dbfs", c->agc_target_dbfs);
    cJSON_AddNumberToObject(o, "agc_max_gain_db", c->agc_max_gain_db);
    cJSON_AddNumberToObject(o, "manual_gain_db", c->manual_gain_db);
    cJSON_AddBoolToObject(o, "limiter_enable", c->limiter_enable);
    cJSON_AddNumberToObject(o, "vad_threshold_db", c->vad_threshold_db);
    cJSON_AddNumberToObject(o, "gate_db", c->gate_db);
    cJSON_AddBoolToObject(o, "airflow_enable", c->airflow_enable);
    cJSON_AddNumberToObject(o, "airflow_depth_db", c->airflow_depth_db);
    cJSON_AddBoolToObject(o, "save_raw", c->save_raw);
    cJSON_AddNumberToObject(o, "max_record_s", c->max_record_s);
}

static void read_int(const cJSON *o, const char *key, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(item)) {
        *out = (int)lround(item->valuedouble);
    }
}

static void read_float(const cJSON *o, const char *key, float *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(item)) {
        *out = (float)item->valuedouble;
    }
}

static void read_bool(const cJSON *o, const char *key, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
    }
}

void air_voice_config_from_json(air_voice_config_t *c, const cJSON *o)
{
    if (!c || !cJSON_IsObject(o)) {
        return;
    }
    const cJSON *preset = cJSON_GetObjectItemCaseSensitive(o, "preset");
    if (cJSON_IsString(preset) && preset->valuestring) {
        strlcpy(c->preset, preset->valuestring, sizeof(c->preset));
    }
    read_int(o, "pga_db", &c->pga_db);
    read_int(o, "adc_gain_db", &c->adc_gain_db);
    read_int(o, "hpf_hz", &c->hpf_hz);
    read_bool(o, "ns_enable", &c->ns_enable);
    read_float(o, "ns_depth_db", &c->ns_depth_db);
    read_bool(o, "eq_enable", &c->eq_enable);
    read_float(o, "eq_presence_db", &c->eq_presence_db);
    read_bool(o, "agc_enable", &c->agc_enable);
    read_float(o, "agc_target_dbfs", &c->agc_target_dbfs);
    read_float(o, "agc_max_gain_db", &c->agc_max_gain_db);
    read_float(o, "manual_gain_db", &c->manual_gain_db);
    read_bool(o, "limiter_enable", &c->limiter_enable);
    read_float(o, "vad_threshold_db", &c->vad_threshold_db);
    read_float(o, "gate_db", &c->gate_db);
    read_bool(o, "airflow_enable", &c->airflow_enable);
    read_float(o, "airflow_depth_db", &c->airflow_depth_db);
    read_bool(o, "save_raw", &c->save_raw);
    read_int(o, "max_record_s", &c->max_record_s);
    air_voice_config_sanitize(c);
}

esp_err_t air_voice_config_load(const char *path, air_voice_config_t *config)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    char *text = calloc(1, CONFIG_FILE_MAX);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const size_t len = fread(text, 1, CONFIG_FILE_MAX - 1U, file);
    fclose(file);
    cJSON *root = cJSON_ParseWithLength(text, len);
    free(text);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version) || version->valueint < CONFIG_VERSION) {
        cJSON_Delete(root);
        AV_LOGW("saved settings predate v%d presets; using new defaults", CONFIG_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    air_voice_config_from_json(config, root);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t air_voice_config_save(const char *path, const air_voice_config_t *config)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "version", CONFIG_VERSION);
    air_voice_config_to_json(config, root);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *file = fopen(tmp, "wb");
    esp_err_t err = ESP_OK;
    if (!file) {
        err = ESP_FAIL;
    } else {
        const size_t len = strlen(text);
        if (fwrite(text, 1, len, file) != len) {
            err = ESP_FAIL;
        }
        if (fclose(file) != 0) {
            err = ESP_FAIL;
        }
        if (err == ESP_OK) {
            (void)remove(path);
            if (rename(tmp, path) != 0) {
                err = ESP_FAIL;
            }
        }
    }
    if (err != ESP_OK) {
        AV_LOGE("save settings %s failed errno=%d", path, errno);
        (void)remove(tmp);
    }
    free(text);
    return err;
}
