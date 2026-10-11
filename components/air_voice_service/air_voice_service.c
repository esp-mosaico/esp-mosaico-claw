/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* Air Voice service.
 *
 * Task layout:
 *  - worker ("av_worker"): owns the capture subscriber, the DSP chain, codec
 *    gain staging, playback objects and every state transition. It never
 *    touches the filesystem, so a slow NAND write cannot overflow the 160 ms
 *    capture ring.
 *  - writer ("av_writer"): drains two PSRAM stream buffers (processed and
 *    raw PCM) into WAV files and performs every other file write (settings,
 *    recording metadata).
 * Public calls post commands to the worker; calls that need a result wait on
 * a reference-counted waiter so a timed-out caller can never race the worker.
 */
#include "air_voice_service.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "audio_hub.h"
#include "dev_audio_codec.h"
#include "esp_board_manager.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "air_voice_config.h"
#include "air_voice_dsp.h"
#include "air_voice_json.h"
#include "air_voice_log.h"
#include "air_voice_player.h"
#include "air_voice_store.h"

#define AV_OWNER_TAG          "air_voice"
#define AV_ADC_DEVICE         "audio_adc"
#define ES8311_REG_PGA        0x14
#define ES8311_PGA_MASK       0x0F

#define FRAME_SAMPLES         320U                      /* 20 ms */
#define FRAME_BYTES           (FRAME_SAMPLES * sizeof(int16_t))
#define ALIGN_SAMPLES         (2U * AIR_VOICE_DSP_HOP)  /* DSP latency */
#define RING_BYTES            (128U * 1024U)            /* 4 s per track */
#define WRITE_CHUNK           4096U
#define CMD_QUEUE_LEN         8U
#define WRITER_QUEUE_LEN      6U
#define WORKER_STACK          8192U
#define WRITER_STACK          6144U
#define WORKER_PRIO           6U
#define WRITER_PRIO           4U
#define CAPTURE_READ_MS       30U
#define IDLE_WAIT_MS          50U

#define WEB_LEASE_US          (6LL * 1000 * 1000)
#define MIC_LINGER_US         (1500LL * 1000)
#define MIC_RETRY_US          (2LL * 1000 * 1000)
#define PUBLISH_US            (100LL * 1000)
#define HINT_US               (500LL * 1000)
/* LittleFS statfs walks the whole NAND filesystem (~0.5 s), so it runs only
 * in the writer task, whose 4 s ring buffers absorb the stall. */
#define SPACE_CHECK_US        (30LL * 1000 * 1000)
#define REC_LOG_US            (5LL * 1000 * 1000)
#define CLIP_HOLD_US          (3LL * 1000 * 1000)
#define SPEECH_HOLD_US        (3LL * 1000 * 1000)
#define SYNC_WAIT_MS          5000U
#define MIN_FREE_START        (2ULL * 1024 * 1024)
#define MIN_FREE_RUNNING      (1ULL * 1024 * 1024)
#define HINT_CLIP_SAMPLES     8U
#define HINT_SNR_LOW_DB       10.0f
#define HINT_QUIET_MARGIN_DB  6.0f
/* Below this the VAD is reacting to room noise, not to someone whispering. */
#define HINT_SPEECH_SNR_DB    6.0f
/* Share of recent speech frames dominated by breath airflow. */
#define HINT_AIRFLOW_RATIO    0.5f
#define HINT_AIRFLOW_SMOOTH   0.5f

typedef enum {
    CMD_RECORD_START = 1,
    CMD_RECORD_STOP,
    CMD_RECORD_TOGGLE,
    CMD_PLAY,
    CMD_PLAY_TOGGLE,
    CMD_PLAY_STOP,
    CMD_PLAY_PAUSE,
    CMD_SET_CONFIG,
} av_cmd_type_t;

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
    int refs;
} av_waiter_t;

typedef struct {
    av_cmd_type_t type;
    char name[AIR_VOICE_NAME_MAX];
    air_voice_track_t track;
    air_voice_config_t config;
    av_waiter_t *waiter;
} av_cmd_t;

typedef enum {
    WR_OPEN = 1,
    WR_CLOSE,
    WR_SAVE_CONFIG,
    WR_REFRESH_SPACE,
} av_writer_op_t;

/* Statistics written next to a recording. */
typedef struct {
    air_voice_config_t config;
    int applied_pga_db;
    int applied_adc_gain_db;
    uint64_t dropped_bytes;
    uint64_t clipped;
    uint32_t speech_frames;
    uint32_t whisper_frames;
    uint32_t voiced_frames;
    uint32_t airflow_frames;
    float peak_in_dbfs;
    float noise_dbfs;
    float speech_dbfs;
    float snr_db;
    int64_t started_epoch;
} av_meta_t;

typedef struct {
    av_writer_op_t op;
    char name[AIR_VOICE_NAME_MAX];
    bool save_raw;
    av_meta_t meta;
    air_voice_config_t config;
} av_writer_msg_t;

typedef struct {
    SemaphoreHandle_t lock;
    QueueHandle_t cmd_q;
    QueueHandle_t writer_q;
    TaskHandle_t worker;
    TaskHandle_t writer;

    air_voice_config_t config;            /* worker-owned; copy under lock */
    air_voice_volume_hooks_t hooks;

    /* Microphone. */
    audio_capture_handle_t capture;
    audio_capture_sub_handle_t mic;
    bool gains_saved;
    int saved_pga_reg;
    float saved_adc_gain;
    int applied_pga_db;
    int applied_adc_gain_db;
    int64_t mic_needed_us;
    int64_t mic_retry_us;
    bool mic_busy;
    int16_t *frame_in;
    int16_t *frame_out;
    int16_t *raw_delay;
    size_t raw_delay_pos;
    air_voice_dsp_t *dsp;
    uint64_t capture_dropped_base;

    /* Clients. */
    bool ui_hold;
    int64_t web_lease_until_us;

    /* Recording (worker side). */
    StreamBufferHandle_t proc_ring;
    StreamBufferHandle_t raw_ring;
    bool recording;
    bool record_save_raw;
    char record_name[AIR_VOICE_NAME_MAX];
    int64_t record_start_us;
    int64_t record_epoch;
    uint64_t ring_dropped;
    float record_peak_dbfs;
    int64_t next_rec_log_us;

    /* Writer side, read by the worker/status under lock. */
    bool writer_busy;
    esp_err_t writer_error;
    uint64_t written_bytes;
    char last_name[AIR_VOICE_NAME_MAX];
    uint32_t last_duration_ms;

    /* Playback (worker-owned objects; state mirrored from callbacks). */
    av_player_t *player;
    audio_mixer_track_handle_t track;
    air_voice_play_state_t play_state;
    char play_name[AIR_VOICE_NAME_MAX];
    air_voice_track_t play_track;
    int64_t play_position_ms;
    int64_t play_duration_ms;
    esp_err_t play_error;

    /* Metering / hints (worker-owned). */
    air_voice_dsp_metrics_t metrics;
    air_voice_hint_t hint;
    uint64_t hint_clipped;
    uint32_t hint_speech_frames;
    uint32_t hint_airflow_frames;
    float airflow_share;
    int64_t clip_until_us;
    int64_t speech_seen_us;
    int64_t next_publish_us;
    int64_t next_hint_us;

    uint64_t storage_free;
    uint64_t storage_total;

    /* Worker-published view for other tasks; guarded by `lock`. */
    air_voice_status_t snap;
} av_service_t;

static av_service_t *s_av;
static portMUX_TYPE s_start_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_waiter_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_start_lock;
static air_voice_volume_hooks_t s_pending_hooks;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void *alloc_prefer_internal(size_t bytes)
{
    return heap_caps_calloc_prefer(1, bytes, 2,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *alloc_prefer_psram(size_t bytes)
{
    return heap_caps_calloc_prefer(1, bytes, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void lock(void)
{
    xSemaphoreTake(s_av->lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_av->lock);
}

static void waiter_release(av_waiter_t *w)
{
    if (!w) {
        return;
    }
    portENTER_CRITICAL(&s_waiter_mux);
    const int refs = --w->refs;
    portEXIT_CRITICAL(&s_waiter_mux);
    if (refs == 0) {
        vSemaphoreDelete(w->done);
        free(w);
    }
}

static esp_err_t post_command(av_cmd_t *cmd, bool wait)
{
    if (!s_av) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s_av->worker) {
        return ESP_ERR_INVALID_STATE; /* would deadlock */
    }
    av_waiter_t *w = NULL;
    if (wait) {
        w = calloc(1, sizeof(*w));
        if (!w || !(w->done = xSemaphoreCreateBinary())) {
            free(w);
            return ESP_ERR_NO_MEM;
        }
        w->refs = 2;
        w->result = ESP_ERR_TIMEOUT;
    }
    cmd->waiter = w;
    if (xQueueSend(s_av->cmd_q, cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (w) {
            waiter_release(w);
            waiter_release(w);
        }
        AV_LOGW("command queue full (cmd=%d)", (int)cmd->type);
        return ESP_ERR_TIMEOUT;
    }
    if (!w) {
        return ESP_OK;
    }
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(w->done, pdMS_TO_TICKS(SYNC_WAIT_MS)) == pdTRUE) {
        result = w->result;
    }
    waiter_release(w);
    return result;
}

static void complete_command(av_cmd_t *cmd, esp_err_t result)
{
    if (cmd->waiter) {
        cmd->waiter->result = result;
        xSemaphoreGive(cmd->waiter->done);
        waiter_release(cmd->waiter);
    }
}

/* ------------------------------------------------------------------ */
/* Codec gain staging                                                  */
/* ------------------------------------------------------------------ */

static esp_codec_dev_handle_t adc_codec(void)
{
    void *handle = NULL;
    if (esp_board_manager_get_device_handle(AV_ADC_DEVICE, &handle) != ESP_OK || !handle) {
        return NULL;
    }
    return ((dev_audio_codec_handles_t *)handle)->codec_dev;
}

static void read_back_gains(av_service_t *s)
{
    esp_codec_dev_handle_t codec = adc_codec();
    int reg = 0;
    s->applied_pga_db = -1;
    if (codec && esp_codec_dev_read_reg(codec, ES8311_REG_PGA, &reg) == ESP_CODEC_DEV_OK) {
        s->applied_pga_db = (reg & ES8311_PGA_MASK) * AIR_VOICE_PGA_DB_STEP;
    }
    float gain = -1.0f;
    if (s->capture && audio_capture_get_input_gain(s->capture, &gain) == ESP_OK) {
        s->applied_adc_gain_db = (int)lroundf(gain);
    } else {
        s->applied_adc_gain_db = -1;
    }
}

static void save_gains(av_service_t *s)
{
    esp_codec_dev_handle_t codec = adc_codec();
    int reg = 0;
    float gain = 0.0f;
    s->gains_saved = codec && s->capture &&
        esp_codec_dev_read_reg(codec, ES8311_REG_PGA, &reg) == ESP_CODEC_DEV_OK &&
        audio_capture_get_input_gain(s->capture, &gain) == ESP_OK;
    if (s->gains_saved) {
        s->saved_pga_reg = reg;
        s->saved_adc_gain = gain;
        AV_LOGI("saved codec gains: PGA reg 0x%02x (%d dB), ADC %.0f dB",
                reg, (reg & ES8311_PGA_MASK) * AIR_VOICE_PGA_DB_STEP, (double)gain);
    } else {
        AV_LOGW("could not read codec gains; they will not be restored");
    }
}

static void apply_gains(av_service_t *s)
{
    esp_codec_dev_handle_t codec = adc_codec();
    int reg = 0;
    if (!codec || esp_codec_dev_read_reg(codec, ES8311_REG_PGA, &reg) != ESP_CODEC_DEV_OK) {
        AV_LOGE("ES8311 not reachable; PGA unchanged");
    } else {
        const int value = (reg & ~ES8311_PGA_MASK) |
            ((s->config.pga_db / AIR_VOICE_PGA_DB_STEP) & ES8311_PGA_MASK);
        if (esp_codec_dev_write_reg(codec, ES8311_REG_PGA, value) != ESP_CODEC_DEV_OK) {
            AV_LOGE("write PGA reg failed");
        }
    }
    if (s->capture) {
        const esp_err_t err = audio_capture_set_input_gain(s->capture, (float)s->config.adc_gain_db);
        if (err != ESP_OK) {
            AV_LOGE("set ADC gain %d dB failed: %s", s->config.adc_gain_db, esp_err_to_name(err));
        }
    }
    read_back_gains(s);
    AV_LOGI("codec gain applied: PGA %d dB + ADC %d dB = %d dB",
            s->applied_pga_db, s->applied_adc_gain_db,
            s->applied_pga_db + s->applied_adc_gain_db);
}

static void restore_gains(av_service_t *s)
{
    if (!s->gains_saved) {
        return;
    }
    esp_codec_dev_handle_t codec = adc_codec();
    if (codec) {
        (void)esp_codec_dev_write_reg(codec, ES8311_REG_PGA, s->saved_pga_reg);
    }
    if (s->capture) {
        (void)audio_capture_set_input_gain(s->capture, s->saved_adc_gain);
    }
    s->gains_saved = false;
    AV_LOGI("restored system codec gains");
}

/* ------------------------------------------------------------------ */
/* Microphone                                                          */
/* ------------------------------------------------------------------ */

static void reset_counters_for_session(av_service_t *s)
{
    air_voice_dsp_reset_counters(s->dsp);
    s->hint_clipped = 0;
    s->hint_speech_frames = 0;
    s->hint_airflow_frames = 0;
    s->airflow_share = 0.0f;
    uint64_t dropped = 0;
    if (s->mic && audio_capture_sub_get_dropped_bytes(s->mic, &dropped) == ESP_OK) {
        s->capture_dropped_base = dropped;
    }
}

static esp_err_t open_mic(av_service_t *s, int64_t now)
{
    if (s->mic) {
        return ESP_OK;
    }
    if (!s->capture && audio_hub_get_capture(&s->capture) != ESP_OK) {
        s->capture = NULL;
        AV_LOGE("audio hub capture unavailable");
        s->mic_retry_us = now + MIC_RETRY_US;
        return ESP_ERR_INVALID_STATE;
    }
    const audio_capture_sub_format_t fmt = {
        .sample_rate = AIR_VOICE_SAMPLE_RATE, .channels = 1, .bits = 16,
    };
    const esp_err_t err = audio_capture_open_subscriber(s->capture, AUDIO_CAPTURE_SUB_APP,
                                                        &fmt, AV_OWNER_TAG, &s->mic);
    if (err != ESP_OK) {
        s->mic = NULL;
        if (!s->mic_busy) {
            AV_LOGW("microphone busy (App capture role in use): %s", esp_err_to_name(err));
        }
        s->mic_busy = true;
        s->mic_retry_us = now + MIC_RETRY_US;
        return err;
    }
    s->mic_busy = false;
    save_gains(s);
    apply_gains(s);
    air_voice_dsp_reset(s->dsp);
    memset(s->raw_delay, 0, ALIGN_SAMPLES * sizeof(int16_t));
    s->raw_delay_pos = 0;
    (void)audio_capture_sub_flush(s->mic);
    reset_counters_for_session(s);
    AV_LOGI("microphone open (16 kHz mono)");
    return ESP_OK;
}

static void close_mic(av_service_t *s)
{
    if (!s->mic) {
        return;
    }
    (void)audio_capture_close_subscriber(s->mic);
    s->mic = NULL;
    restore_gains(s);
    AV_LOGI("microphone closed");
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static void release_playback(av_service_t *s)
{
    if (s->player) {
        av_player_destroy(s->player);
        s->player = NULL;
    }
    if (s->track) {
        (void)audio_mixer_close_track(s->track);
        s->track = NULL;
    }
}

static esp_err_t start_playback(av_service_t *s, const char *name, air_voice_track_t which)
{
    char path[AIR_VOICE_PATH_MAX];
    esp_err_t err = av_store_path(name, which == AIR_VOICE_TRACK_RAW ? AV_FILE_RAW : AV_FILE_PROCESSED,
                                  path, sizeof(path));
    struct stat st;
    if (err != ESP_OK || stat(path, &st) != 0) {
        AV_LOGW("play: %s (%s) not found", name, which == AIR_VOICE_TRACK_RAW ? "raw" : "processed");
        return ESP_ERR_NOT_FOUND;
    }
    release_playback(s);

    audio_mixer_handle_t mixer = NULL;
    err = audio_hub_get_mixer(&mixer);
    if (err != ESP_OK || !mixer) {
        AV_LOGE("play: shared mixer unavailable: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    /* The App track may be held by Bluetooth audio or a Lua script; fall back
     * to the System track (as the Music App does) instead of failing. */
    err = audio_mixer_open_track(mixer, AUDIO_MIXER_TRACK_APP, AV_OWNER_TAG, &s->track);
    if (err != ESP_OK) {
        AV_LOGW("play: App mixer track busy (%s), trying System track", esp_err_to_name(err));
        err = audio_mixer_open_track(mixer, AUDIO_MIXER_TRACK_SYSTEM, AV_OWNER_TAG, &s->track);
    }
    if (err != ESP_OK) {
        s->track = NULL;
        AV_LOGE("play: no mixer track available: %s", esp_err_to_name(err));
        return err;
    }
    err = av_player_start(path, s->track, &s->player);
    if (err != ESP_OK) {
        s->player = NULL;
        AV_LOGE("play: cannot stream %s: %s", path, esp_err_to_name(err));
        release_playback(s);
        return err;
    }
    strlcpy(s->play_name, name, sizeof(s->play_name));
    s->play_track = which;
    s->play_state = AIR_VOICE_PLAY_PLAYING;
    s->play_error = ESP_OK;
    s->play_position_ms = 0;
    s->play_duration_ms = av_player_duration_ms(s->player);
    AV_LOGI("play %s [%s] %" PRId64 " ms", name, which == AIR_VOICE_TRACK_RAW ? "raw" : "processed",
            s->play_duration_ms);
    return ESP_OK;
}

static void poll_playback(av_service_t *s)
{
    if (!s->player) {
        return;
    }
    s->play_position_ms = av_player_position_ms(s->player);
    esp_err_t err = ESP_OK;
    if (av_player_finished(s->player, &err)) {
        if (err == ESP_OK) {
            AV_LOGI("playback finished %s (%" PRId64 " ms)", s->play_name, s->play_position_ms);
            s->play_state = AIR_VOICE_PLAY_FINISHED;
        } else {
            AV_LOGE("playback error %s at %" PRId64 " ms: %s", s->play_name,
                    s->play_position_ms, esp_err_to_name(err));
            s->play_state = AIR_VOICE_PLAY_ERROR;
            s->play_error = err;
        }
        release_playback(s);
        return;
    }
    s->play_state = av_player_is_paused(s->player) ? AIR_VOICE_PLAY_PAUSED : AIR_VOICE_PLAY_PLAYING;
}

/* ------------------------------------------------------------------ */
/* Recording (worker side)                                             */
/* ------------------------------------------------------------------ */

static void fill_meta(av_service_t *s, av_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));
    meta->config = s->config;
    meta->applied_pga_db = s->applied_pga_db;
    meta->applied_adc_gain_db = s->applied_adc_gain_db;
    uint64_t capture_dropped = 0;
    if (s->mic) {
        (void)audio_capture_sub_get_dropped_bytes(s->mic, &capture_dropped);
    }
    meta->dropped_bytes = s->ring_dropped +
        (capture_dropped > s->capture_dropped_base ? capture_dropped - s->capture_dropped_base : 0);
    air_voice_dsp_metrics_t m;
    air_voice_dsp_get_metrics(s->dsp, &m);
    meta->clipped = m.clipped;
    meta->speech_frames = m.speech_frames;
    meta->whisper_frames = m.whisper_frames;
    meta->voiced_frames = m.voiced_frames;
    meta->airflow_frames = m.airflow_frames;
    meta->noise_dbfs = m.noise_dbfs;
    meta->speech_dbfs = m.speech_dbfs;
    meta->snr_db = m.snr_db;
    meta->peak_in_dbfs = s->record_peak_dbfs;
    meta->started_epoch = s->record_epoch;
}

static esp_err_t record_start(av_service_t *s)
{
    if (s->recording) {
        return ESP_OK;
    }
    lock();
    const bool busy = s->writer_busy;
    unlock();
    if (busy) {
        AV_LOGW("record: previous recording is still being saved");
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t now = esp_timer_get_time();
    s->mic_needed_us = now;
    esp_err_t err = open_mic(s, now);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    const uint64_t free_bytes = s->storage_free;
    const uint64_t total_bytes = s->storage_total;
    unlock();
    if (total_bytes > 0 && free_bytes < MIN_FREE_START) {
        AV_LOGE("record: storage almost full (%" PRIu64 " bytes free)", free_bytes);
        return ESP_ERR_NO_MEM;
    }
    if (s->player) {
        release_playback(s);
        s->play_state = AIR_VOICE_PLAY_IDLE;
    }
    av_writer_msg_t msg = {.op = WR_OPEN, .save_raw = s->config.save_raw};
    err = av_store_new_name(msg.name, sizeof(msg.name));
    if (err != ESP_OK) {
        AV_LOGE("record: cannot allocate a file name");
        return err;
    }
    lock();
    s->writer_busy = true;
    s->writer_error = ESP_OK;
    s->written_bytes = 0;
    unlock();
    if (xQueueSend(s->writer_q, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        lock();
        s->writer_busy = false;
        unlock();
        return ESP_ERR_TIMEOUT;
    }
    strlcpy(s->record_name, msg.name, sizeof(s->record_name));
    s->record_save_raw = s->config.save_raw;
    s->record_start_us = now;
    s->record_epoch = (int64_t)time(NULL);
    s->ring_dropped = 0;
    s->record_peak_dbfs = -120.0f;
    s->next_rec_log_us = now + REC_LOG_US;
    reset_counters_for_session(s);
    s->recording = true;
    AV_LOGI("record start %s (gain %d+%d dB, preset %s, raw %s)", s->record_name,
            s->applied_pga_db, s->applied_adc_gain_db, s->config.preset,
            s->record_save_raw ? "on" : "off");
    return ESP_OK;
}

static esp_err_t record_stop(av_service_t *s, const char *reason)
{
    if (!s->recording) {
        return ESP_OK;
    }
    s->recording = false;
    av_writer_msg_t msg = {.op = WR_CLOSE};
    strlcpy(msg.name, s->record_name, sizeof(msg.name));
    fill_meta(s, &msg.meta);
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - s->record_start_us) / 1000);
    AV_LOGI("record stop %s after %" PRIu32 " ms (%s), dropped %" PRIu64 " bytes, clipped %" PRIu64,
            s->record_name, ms, reason, msg.meta.dropped_bytes, msg.meta.clipped);
    if (xQueueSend(s->writer_q, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        AV_LOGE("record: writer queue full, file left as .part (recovered on reboot)");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void push_ring(av_service_t *s, StreamBufferHandle_t ring, const void *data, size_t bytes)
{
    const size_t sent = xStreamBufferSend(ring, data, bytes, 0);
    if (sent < bytes) {
        s->ring_dropped += bytes - sent;
    }
}

static void process_frame(av_service_t *s, size_t samples)
{
    air_voice_dsp_process(s->dsp, s->frame_in, s->frame_out, samples);
    /* Delay the raw signal by the DSP latency so both files line up exactly.
     * The line runs continuously, so a recording's raw track starts with the
     * audio just before the button press rather than zero padding. */
    for (size_t i = 0; i < samples; ++i) {
        const int16_t delayed = s->raw_delay[s->raw_delay_pos];
        s->raw_delay[s->raw_delay_pos] = s->frame_in[i];
        s->frame_in[i] = delayed;
        if (++s->raw_delay_pos >= ALIGN_SAMPLES) {
            s->raw_delay_pos = 0;
        }
    }
    if (!s->recording) {
        return;
    }
    push_ring(s, s->proc_ring, s->frame_out, samples * sizeof(int16_t));
    if (s->record_save_raw) {
        push_ring(s, s->raw_ring, s->frame_in, samples * sizeof(int16_t));
    }
}

static void check_recording(av_service_t *s, int64_t now)
{
    if (!s->recording) {
        return;
    }
    lock();
    const esp_err_t writer_error = s->writer_error;
    unlock();
    if (writer_error != ESP_OK) {
        record_stop(s, writer_error == ESP_ERR_NO_MEM ? "storage full" : "write error");
        return;
    }
    if (now - s->record_start_us >= (int64_t)s->config.max_record_s * 1000000LL) {
        record_stop(s, "max duration reached");
        return;
    }
    if (now >= s->next_rec_log_us) {
        s->next_rec_log_us = now + REC_LOG_US;
        const air_voice_dsp_metrics_t *m = &s->metrics;
        const uint32_t sp = m->whisper_frames + m->voiced_frames;
        uint64_t capture_dropped = 0;
        (void)audio_capture_sub_get_dropped_bytes(s->mic, &capture_dropped);
        const uint64_t dropped = s->ring_dropped + (capture_dropped > s->capture_dropped_base
            ? capture_dropped - s->capture_dropped_base : 0);
        AV_LOGI("rec %s %" PRId64 "s in %.0f/%.0f dBFS noise %.0f snr %.0f agc %+.0f dB "
                "whisper %u%% airflow %u%% dropped %" PRIu64 " B",
                s->record_name, (now - s->record_start_us) / 1000000,
                (double)m->in_rms_dbfs, (double)m->in_peak_dbfs, (double)m->noise_dbfs,
                (double)m->snr_db, (double)m->agc_gain_db,
                sp ? (unsigned)(100U * m->whisper_frames / sp) : 0U,
                m->speech_frames ? (unsigned)(100U * m->airflow_frames / m->speech_frames) : 0U,
                dropped);
    }
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

static void publish_status(av_service_t *s, int64_t now, bool force);

static void update_hint(av_service_t *s, int64_t now)
{
    const air_voice_dsp_metrics_t *m = &s->metrics;
    if (m->clipped > s->hint_clipped + HINT_CLIP_SAMPLES || m->in_peak_dbfs > -0.5f) {
        s->clip_until_us = now + CLIP_HOLD_US;
    }
    s->hint_clipped = m->clipped;
    /* Airflow share over the speech frames seen since the previous update. */
    const uint32_t speech = m->speech_frames - s->hint_speech_frames;
    const uint32_t airflow = m->airflow_frames - s->hint_airflow_frames;
    s->hint_speech_frames = m->speech_frames;
    s->hint_airflow_frames = m->airflow_frames;
    if (speech > 0) {
        s->airflow_share += ((float)airflow / (float)speech - s->airflow_share) * HINT_AIRFLOW_SMOOTH;
    }
    if (m->vad && m->snr_db >= HINT_SPEECH_SNR_DB) {
        s->speech_seen_us = now;
    }
    if (!s->mic) {
        s->hint = AIR_VOICE_HINT_NONE;
    } else if (now < s->clip_until_us) {
        s->hint = AIR_VOICE_HINT_CLIPPING;
    } else if (s->speech_seen_us > 0 && now - s->speech_seen_us < SPEECH_HOLD_US) {
        const float reachable = m->speech_dbfs + s->config.agc_max_gain_db;
        if (s->config.airflow_enable && s->airflow_share > HINT_AIRFLOW_RATIO) {
            s->hint = AIR_VOICE_HINT_AIRFLOW;
        } else if (s->config.agc_enable && reachable < s->config.agc_target_dbfs - HINT_QUIET_MARGIN_DB) {
            s->hint = AIR_VOICE_HINT_TOO_QUIET;
        } else if (m->snr_db < HINT_SNR_LOW_DB) {
            s->hint = AIR_VOICE_HINT_LOW_SNR;
        } else {
            s->hint = AIR_VOICE_HINT_GOOD;
        }
    } else {
        s->hint = AIR_VOICE_HINT_NONE;
    }
}

static void manage_mic(av_service_t *s, int64_t now)
{
    lock();
    const bool need = s->recording || s->ui_hold || now < s->web_lease_until_us;
    unlock();
    if (need) {
        s->mic_needed_us = now;
        if (!s->mic && now >= s->mic_retry_us) {
            (void)open_mic(s, now);
        }
    } else {
        s->mic_busy = false;
        if (s->mic && now - s->mic_needed_us > MIC_LINGER_US) {
            close_mic(s);
        }
    }
}

static void handle_command(av_service_t *s, av_cmd_t *cmd)
{
    esp_err_t err = ESP_OK;
    switch (cmd->type) {
    case CMD_RECORD_START:
        err = record_start(s);
        break;
    case CMD_RECORD_STOP:
        err = record_stop(s, "requested");
        break;
    case CMD_RECORD_TOGGLE:
        err = s->recording ? record_stop(s, "requested") : record_start(s);
        break;
    case CMD_PLAY_TOGGLE:
        if (s->player) {
            release_playback(s);
            s->play_state = AIR_VOICE_PLAY_IDLE;
            AV_LOGI("playback stopped");
            break;
        }
        cmd->name[0] = '\0'; /* not playing: play the latest recording */
        __attribute__((fallthrough));
    case CMD_PLAY: {
        if (s->recording) {
            err = ESP_ERR_INVALID_STATE;
            AV_LOGW("play rejected while recording");
            break;
        }
        char name[AIR_VOICE_NAME_MAX];
        if (cmd->name[0]) {
            strlcpy(name, cmd->name, sizeof(name));
        } else {
            lock();
            strlcpy(name, s->last_name, sizeof(name));
            unlock();
        }
        if (!name[0]) {
            err = ESP_ERR_NOT_FOUND;
            AV_LOGW("play: no recording yet");
            break;
        }
        err = start_playback(s, name, cmd->track);
        if (err != ESP_OK) {
            s->play_state = AIR_VOICE_PLAY_ERROR;
            s->play_error = err;
            strlcpy(s->play_name, name, sizeof(s->play_name));
        }
        break;
    }
    case CMD_PLAY_STOP:
        if (s->player) {
            release_playback(s);
            AV_LOGI("playback stopped");
        }
        s->play_state = AIR_VOICE_PLAY_IDLE;
        break;
    case CMD_PLAY_PAUSE:
        if (!s->player) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            const bool pause = !av_player_is_paused(s->player);
            av_player_set_paused(s->player, pause);
            s->play_state = pause ? AIR_VOICE_PLAY_PAUSED : AIR_VOICE_PLAY_PLAYING;
        }
        break;
    case CMD_SET_CONFIG: {
        const bool gain_changed = cmd->config.pga_db != s->config.pga_db ||
            cmd->config.adc_gain_db != s->config.adc_gain_db;
        lock();
        s->config = cmd->config;
        unlock();
        air_voice_dsp_config_t dsp_cfg;
        air_voice_config_to_dsp(&s->config, &dsp_cfg);
        air_voice_dsp_set_config(s->dsp, &dsp_cfg);
        if (gain_changed && s->mic) {
            apply_gains(s);
        }
        av_writer_msg_t msg = {.op = WR_SAVE_CONFIG, .config = s->config};
        (void)xQueueSend(s->writer_q, &msg, pdMS_TO_TICKS(100));
        AV_LOGI("settings applied: preset %s gain %d+%d hpf %d ns %s/%.0f eq %s/%+.0f agc %s/%.0f/%.0f "
                "gate %.0f vad %.1f raw %s",
                s->config.preset, s->config.pga_db, s->config.adc_gain_db, s->config.hpf_hz,
                s->config.ns_enable ? "on" : "off", (double)s->config.ns_depth_db,
                s->config.eq_enable ? "on" : "off", (double)s->config.eq_presence_db,
                s->config.agc_enable ? "on" : "off", (double)s->config.agc_target_dbfs,
                (double)s->config.agc_max_gain_db, (double)s->config.gate_db,
                (double)s->config.vad_threshold_db, s->config.save_raw ? "on" : "off");
        break;
    }
    default:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    /* Publish before releasing the caller so an immediate status read
     * already reflects the command. */
    publish_status(s, esp_timer_get_time(), true);
    complete_command(cmd, err);
}

/* Copy worker-owned state into the lock-protected snapshot read by other
 * tasks. Writer-owned fields are folded in under the same lock. */
static void publish_status(av_service_t *s, int64_t now, bool force)
{
    if (!force && now < s->next_publish_us) {
        return;
    }
    s->next_publish_us = now + PUBLISH_US;
    air_voice_dsp_get_metrics(s->dsp, &s->metrics);
    if (s->recording && s->metrics.in_peak_dbfs > s->record_peak_dbfs) {
        s->record_peak_dbfs = s->metrics.in_peak_dbfs;
    }
    if (now >= s->next_hint_us) {
        s->next_hint_us = now + HINT_US;
        update_hint(s, now);
    }
    const air_voice_dsp_metrics_t *m = &s->metrics;
    air_voice_status_t snap = {
        .mic_open = s->mic != NULL,
        .mic_busy = s->mic_busy,
        .applied_pga_db = s->mic ? s->applied_pga_db : -1,
        .applied_adc_gain_db = s->mic ? s->applied_adc_gain_db : -1,
        .play_state = s->play_state,
        .play_track = s->play_track,
        .play_position_ms = s->play_position_ms,
        .play_duration_ms = s->play_duration_ms,
        .play_error = s->play_error,
        .meter = {
            .in_rms_dbfs = m->in_rms_dbfs, .in_peak_dbfs = m->in_peak_dbfs,
            .out_rms_dbfs = m->out_rms_dbfs, .out_peak_dbfs = m->out_peak_dbfs,
            .noise_dbfs = m->noise_dbfs, .speech_dbfs = m->speech_dbfs, .snr_db = m->snr_db,
            .agc_gain_db = m->agc_gain_db, .voicing = m->voicing, .vad = m->vad,
            .airflow = m->airflow, .clipped = m->clipped, .speech_frames = m->speech_frames,
            .whisper_frames = m->whisper_frames, .voiced_frames = m->voiced_frames,
            .airflow_frames = m->airflow_frames,
        },
        .hint = s->hint,
    };
    strlcpy(snap.play_name, s->play_name, sizeof(snap.play_name));
    strlcpy(snap.record_name, s->record_name, sizeof(snap.record_name));
    if (s->recording) {
        snap.record_ms = (uint32_t)((now - s->record_start_us) / 1000);
        uint64_t capture_dropped = 0;
        if (s->mic) {
            (void)audio_capture_sub_get_dropped_bytes(s->mic, &capture_dropped);
        }
        snap.dropped_bytes = s->ring_dropped + (capture_dropped > s->capture_dropped_base
            ? capture_dropped - s->capture_dropped_base : 0);
    }
    lock();
    snap.state = s->recording ? AIR_VOICE_STATE_RECORDING
        : s->writer_busy ? AIR_VOICE_STATE_SAVING
        : s->mic ? AIR_VOICE_STATE_MONITOR : AIR_VOICE_STATE_IDLE;
    snap.ui_hold = s->ui_hold;
    snap.storage_free_bytes = s->storage_free;
    snap.storage_total_bytes = s->storage_total;
    snap.record_bytes = s->recording ? s->written_bytes : 0;
    snap.record_error = s->writer_error;
    strlcpy(snap.last_name, s->last_name, sizeof(snap.last_name));
    snap.last_duration_ms = s->last_duration_ms;
    s->snap = snap;
    unlock();
}

static void worker_task(void *arg)
{
    av_service_t *s = arg;
    for (;;) {
        av_cmd_t cmd;
        TickType_t wait = s->mic ? 0 : pdMS_TO_TICKS(IDLE_WAIT_MS);
        while (xQueueReceive(s->cmd_q, &cmd, wait) == pdTRUE) {
            handle_command(s, &cmd);
            wait = 0;
        }
        const int64_t now = esp_timer_get_time();
        manage_mic(s, now);
        if (s->mic) {
            const size_t got = audio_capture_sub_read(s->mic, s->frame_in, FRAME_BYTES, CAPTURE_READ_MS);
            if (got >= sizeof(int16_t)) {
                process_frame(s, got / sizeof(int16_t));
            }
        }
        poll_playback(s);
        check_recording(s, now);
        publish_status(s, now, false);
    }
}

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *proc;
    FILE *raw;
    uint64_t proc_bytes;
    uint64_t raw_bytes;
    char name[AIR_VOICE_NAME_MAX];
    uint8_t *chunk;
    int64_t next_space_check_us;
} av_writer_t;

static void writer_fail(av_service_t *s, const char *what, esp_err_t err)
{
    AV_LOGE("writer: %s failed (errno=%d)", what, errno);
    lock();
    s->writer_error = err;
    unlock();
}

/* Refresh the cached storage figures; returns the free byte count. */
static uint64_t writer_refresh_space(av_service_t *s)
{
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
    av_store_space(&free_bytes, &total_bytes);
    lock();
    s->storage_free = free_bytes;
    s->storage_total = total_bytes;
    unlock();
    return total_bytes > 0 ? free_bytes : UINT64_MAX;
}

static FILE *open_part(const char *name, av_file_kind_t kind)
{
    char path[AIR_VOICE_PATH_MAX];
    if (av_store_path(name, kind, path, sizeof(path)) != ESP_OK) {
        return NULL;
    }
    FILE *file = fopen(path, "w+b");
    if (!file) {
        return NULL;
    }
    (void)setvbuf(file, NULL, _IOFBF, WRITE_CHUNK);
    if (av_wav_begin(file) != ESP_OK) {
        fclose(file);
        (void)remove(path);
        return NULL;
    }
    return file;
}

static size_t drain_ring(av_service_t *s, StreamBufferHandle_t ring, FILE *file,
                         uint64_t *bytes, uint8_t *chunk, TickType_t wait)
{
    const size_t got = xStreamBufferReceive(ring, chunk, WRITE_CHUNK, wait);
    if (got == 0) {
        return 0;
    }
    if (!file) {
        return got; /* discard (file failed to open) */
    }
    if (fwrite(chunk, 1, got, file) != got) {
        writer_fail(s, "write", ESP_FAIL);
        return got;
    }
    *bytes += got;
    return got;
}

static void finalize_track(FILE *file, const char *name, av_file_kind_t part_kind,
                           av_file_kind_t final_kind)
{
    if (!file) {
        return;
    }
    const bool closed = fclose(file) == 0;
    char part[AIR_VOICE_PATH_MAX];
    char final_path[AIR_VOICE_PATH_MAX];
    if (av_store_path(name, part_kind, part, sizeof(part)) != ESP_OK ||
        av_store_path(name, final_kind, final_path, sizeof(final_path)) != ESP_OK) {
        return;
    }
    if (!closed || rename(part, final_path) != 0) {
        AV_LOGE("writer: finalize %s failed errno=%d (kept as .part)", part, errno);
    }
}

static void write_meta(const char *name, const av_meta_t *meta, uint64_t proc_bytes,
                       uint64_t raw_bytes)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *stats = cJSON_CreateObject();
    cJSON *cfg = cJSON_CreateObject();
    if (!root || !stats || !cfg) {
        cJSON_Delete(root);
        cJSON_Delete(stats);
        cJSON_Delete(cfg);
        return;
    }
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddNumberToObject(root, "sample_rate", AIR_VOICE_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "duration_ms", av_store_duration_ms(proc_bytes + AIR_VOICE_WAV_HEADER));
    cJSON_AddNumberToObject(root, "started_epoch", (double)meta->started_epoch);
    cJSON_AddNumberToObject(root, "processed_bytes", (double)proc_bytes);
    cJSON_AddNumberToObject(root, "raw_bytes", (double)raw_bytes);
    cJSON_AddNumberToObject(root, "pga_db", meta->applied_pga_db);
    cJSON_AddNumberToObject(root, "adc_gain_db", meta->applied_adc_gain_db);
    cJSON_AddNumberToObject(root, "latency_aligned_samples", ALIGN_SAMPLES);
    cJSON_AddNumberToObject(stats, "dropped_bytes", (double)meta->dropped_bytes);
    cJSON_AddNumberToObject(stats, "clipped_samples", (double)meta->clipped);
    cJSON_AddNumberToObject(stats, "peak_in_dbfs", roundf(meta->peak_in_dbfs * 10.0f) / 10.0f);
    cJSON_AddNumberToObject(stats, "noise_dbfs", roundf(meta->noise_dbfs * 10.0f) / 10.0f);
    cJSON_AddNumberToObject(stats, "speech_dbfs", roundf(meta->speech_dbfs * 10.0f) / 10.0f);
    cJSON_AddNumberToObject(stats, "snr_db", roundf(meta->snr_db * 10.0f) / 10.0f);
    cJSON_AddNumberToObject(stats, "speech_frames", meta->speech_frames);
    cJSON_AddNumberToObject(stats, "whisper_frames", meta->whisper_frames);
    cJSON_AddNumberToObject(stats, "voiced_frames", meta->voiced_frames);
    cJSON_AddNumberToObject(stats, "airflow_frames", meta->airflow_frames);
    cJSON_AddItemToObject(root, "stats", stats);
    air_voice_config_to_json(&meta->config, cfg);
    cJSON_AddItemToObject(root, "config", cfg);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        return;
    }
    char path[AIR_VOICE_PATH_MAX];
    if (av_store_path(name, AV_FILE_META, path, sizeof(path)) == ESP_OK) {
        FILE *file = fopen(path, "wb");
        if (file) {
            fputs(text, file);
            fclose(file);
        }
    }
    free(text);
}

static void writer_open(av_service_t *s, av_writer_t *w, const av_writer_msg_t *msg)
{
    memset(w->name, 0, sizeof(w->name));
    strlcpy(w->name, msg->name, sizeof(w->name));
    w->proc_bytes = w->raw_bytes = 0;
    w->next_space_check_us = esp_timer_get_time() + SPACE_CHECK_US;
    if (writer_refresh_space(s) < MIN_FREE_START) {
        writer_fail(s, "storage check", ESP_ERR_NO_MEM);
        return;
    }
    w->proc = open_part(msg->name, AV_FILE_PART);
    w->raw = msg->save_raw ? open_part(msg->name, AV_FILE_RAW_PART) : NULL;
    if (!w->proc || (msg->save_raw && !w->raw)) {
        writer_fail(s, "open recording file", ESP_FAIL);
    }
}

static void writer_close(av_service_t *s, av_writer_t *w, const av_writer_msg_t *msg)
{
    /* The worker stopped producing before sending CLOSE: drain everything. */
    const int64_t drain_start = esp_timer_get_time();
    const uint64_t before = w->proc_bytes + w->raw_bytes;
    while (drain_ring(s, s->proc_ring, w->proc, &w->proc_bytes, w->chunk, 0) > 0) {
    }
    while (drain_ring(s, s->raw_ring, w->raw, &w->raw_bytes, w->chunk, 0) > 0) {
    }
    const uint64_t drained = w->proc_bytes + w->raw_bytes - before;
    const bool had_file = w->proc != NULL;
    const int64_t close_start = esp_timer_get_time();
    const uint32_t drain_ms = (uint32_t)((close_start - drain_start) / 1000);
    finalize_track(w->proc, w->name, AV_FILE_PART, AV_FILE_PROCESSED);
    finalize_track(w->raw, w->name, AV_FILE_RAW_PART, AV_FILE_RAW);
    w->proc = w->raw = NULL;
    if (had_file) {
        write_meta(w->name, &msg->meta, w->proc_bytes, w->raw_bytes);
    }
    const uint32_t close_ms = (uint32_t)((esp_timer_get_time() - close_start) / 1000);
    const uint32_t ms = av_store_duration_ms(w->proc_bytes + AIR_VOICE_WAV_HEADER);
    lock();
    if (had_file) {
        strlcpy(s->last_name, w->name, sizeof(s->last_name));
        s->last_duration_ms = ms;
    }
    s->writer_busy = false;
    unlock();
    (void)writer_refresh_space(s);
    if (had_file) {
        AV_LOGI("saved %s: %" PRIu32 " ms, %" PRIu64 " + %" PRIu64 " bytes (drained %" PRIu64
                " KB in %" PRIu32 " ms, closed in %" PRIu32 " ms)",
                w->name, ms, w->proc_bytes, w->raw_bytes, drained / 1024U, drain_ms, close_ms);
    }
}

static void writer_task(void *arg)
{
    av_service_t *s = arg;
    /* Messages carry a metadata snapshot; keep the buffer off the stack. */
    static av_writer_msg_t msg;
    av_writer_t w = {.chunk = alloc_prefer_internal(WRITE_CHUNK)};
    char config_path[AIR_VOICE_PATH_MAX] = {0};
    (void)av_store_config_path(config_path, sizeof(config_path));
    for (;;) {
        const bool open = w.proc || w.raw;
        if (xQueueReceive(s->writer_q, &msg, open ? 0 : pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (msg.op) {
            case WR_OPEN:
                writer_open(s, &w, &msg);
                break;
            case WR_CLOSE:
                writer_close(s, &w, &msg);
                break;
            case WR_SAVE_CONFIG:
                if (config_path[0]) {
                    (void)air_voice_config_save(config_path, &msg.config);
                }
                break;
            case WR_REFRESH_SPACE:
                (void)writer_refresh_space(s);
                break;
            }
        }
        /* Only drain while a file is open: data produced before OPEN is
         * handled waits in the ring, and CLOSE drains whatever remains. */
        if ((w.proc || w.raw) && w.chunk) {
            const size_t got = drain_ring(s, s->proc_ring, w.proc, &w.proc_bytes, w.chunk,
                                          pdMS_TO_TICKS(40));
            while (drain_ring(s, s->raw_ring, w.raw, &w.raw_bytes, w.chunk, 0) > 0) {
            }
            if (got > 0) {
                lock();
                s->written_bytes = w.proc_bytes;
                unlock();
            }
            const int64_t now = esp_timer_get_time();
            if (now >= w.next_space_check_us) {
                w.next_space_check_us = now + SPACE_CHECK_US;
                if (writer_refresh_space(s) < MIN_FREE_RUNNING) {
                    writer_fail(s, "storage check", ESP_ERR_NO_MEM);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static void init_lock_once(void)
{
    portENTER_CRITICAL(&s_start_mux);
    if (!s_start_lock) {
        static StaticSemaphore_t storage;
        s_start_lock = xSemaphoreCreateMutexStatic(&storage);
    }
    portEXIT_CRITICAL(&s_start_mux);
}

void air_voice_service_set_volume_hooks(const air_voice_volume_hooks_t *hooks)
{
    init_lock_once();
    xSemaphoreTake(s_start_lock, portMAX_DELAY);
    s_pending_hooks = hooks ? *hooks : (air_voice_volume_hooks_t){0};
    if (s_av) {
        lock();
        s_av->hooks = s_pending_hooks;
        unlock();
    }
    xSemaphoreGive(s_start_lock);
}

static esp_err_t create_service(av_service_t **out)
{
    av_service_t *s = alloc_prefer_psram(sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    s->lock = xSemaphoreCreateMutex();
    s->cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(av_cmd_t));
    s->writer_q = xQueueCreate(WRITER_QUEUE_LEN, sizeof(av_writer_msg_t));
    s->proc_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->raw_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->frame_in = alloc_prefer_internal(FRAME_BYTES);
    s->frame_out = alloc_prefer_internal(FRAME_BYTES);
    s->raw_delay = alloc_prefer_internal(ALIGN_SAMPLES * sizeof(int16_t));
    air_voice_config_defaults(&s->config);
    air_voice_dsp_config_t dsp_cfg;
    air_voice_config_to_dsp(&s->config, &dsp_cfg);
    s->dsp = air_voice_dsp_create(&dsp_cfg, alloc_prefer_internal, heap_caps_free);
    if (!s->lock || !s->cmd_q || !s->writer_q || !s->proc_ring || !s->raw_ring ||
        !s->frame_in || !s->frame_out || !s->raw_delay || !s->dsp) {
        AV_LOGE("service allocation failed");
        return ESP_ERR_NO_MEM; /* leaked by design: start failure is fatal for the feature */
    }
    s->applied_pga_db = -1;
    s->applied_adc_gain_db = -1;
    s->hooks = s_pending_hooks;
    *out = s;
    return ESP_OK;
}

esp_err_t air_voice_service_start(void)
{
    init_lock_once();
    xSemaphoreTake(s_start_lock, portMAX_DELAY);
    if (s_av) {
        xSemaphoreGive(s_start_lock);
        return ESP_OK;
    }
    esp_err_t err = air_voice_log_init();
    if (err == ESP_OK) {
        air_voice_log_capture_system(true);
        err = audio_hub_is_started() ? ESP_OK : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            AV_LOGE("audio hub is not running");
        }
    }
    if (err == ESP_OK) {
        err = av_store_init();
    }
    av_service_t *s = NULL;
    if (err == ESP_OK) {
        err = create_service(&s);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_start_lock);
        return err;
    }
    av_store_recover_partials();
    char path[AIR_VOICE_PATH_MAX];
    if (av_store_config_path(path, sizeof(path)) == ESP_OK) {
        const esp_err_t load = air_voice_config_load(path, &s->config);
        if (load == ESP_OK) {
            AV_LOGI("loaded settings (preset %s, gain %d+%d dB)", s->config.preset,
                    s->config.pga_db, s->config.adc_gain_db);
        } else {
            air_voice_config_defaults(&s->config);
            AV_LOGI("using default settings (%s)", s->config.preset);
            if (load == ESP_ERR_INVALID_VERSION) {
                (void)air_voice_config_save(path, &s->config);
            }
        }
    }
    air_voice_dsp_config_t dsp_cfg;
    air_voice_config_to_dsp(&s->config, &dsp_cfg);
    air_voice_dsp_set_config(s->dsp, &dsp_cfg);
    (void)av_store_newest(s->last_name, sizeof(s->last_name), &s->last_duration_ms);
    av_store_space(&s->storage_free, &s->storage_total);

    s_av = s;
    if (xTaskCreate(worker_task, "av_worker", WORKER_STACK, s, WORKER_PRIO, &s->worker) != pdPASS ||
        xTaskCreate(writer_task, "av_writer", WRITER_STACK, s, WRITER_PRIO, &s->writer) != pdPASS) {
        AV_LOGE("task creation failed");
        s_av = NULL;
        xSemaphoreGive(s_start_lock);
        return ESP_ERR_NO_MEM;
    }
    AV_LOGI("service started, storage %s, %" PRIu64 " MB free, latest '%s'",
            av_store_dir(), s->storage_free / (1024 * 1024), s->last_name);
    xSemaphoreGive(s_start_lock);
    return ESP_OK;
}

bool air_voice_service_is_started(void)
{
    return s_av != NULL;
}

esp_err_t air_voice_hold(air_voice_client_t client, bool hold)
{
    if (!s_av) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    if (client == AIR_VOICE_CLIENT_UI) {
        s_av->ui_hold = hold;
    } else {
        s_av->web_lease_until_us = hold ? esp_timer_get_time() + WEB_LEASE_US : 0;
    }
    unlock();
    return ESP_OK;
}

esp_err_t air_voice_record_start(void)
{
    av_cmd_t cmd = {.type = CMD_RECORD_START};
    return post_command(&cmd, true);
}

esp_err_t air_voice_record_stop(void)
{
    av_cmd_t cmd = {.type = CMD_RECORD_STOP};
    return post_command(&cmd, true);
}

esp_err_t air_voice_record_toggle(void)
{
    if (!s_av) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const bool recording = s_av->snap.state == AIR_VOICE_STATE_RECORDING;
    unlock();
    return recording ? air_voice_record_stop() : air_voice_record_start();
}

esp_err_t air_voice_play(const char *name, air_voice_track_t track)
{
    if (name && name[0] && !air_voice_name_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    av_cmd_t cmd = {.type = CMD_PLAY, .track = track};
    if (name) {
        strlcpy(cmd.name, name, sizeof(cmd.name));
    }
    return post_command(&cmd, true);
}

esp_err_t air_voice_request_record_toggle(void)
{
    av_cmd_t cmd = {.type = CMD_RECORD_TOGGLE};
    return post_command(&cmd, false);
}

esp_err_t air_voice_request_play_toggle(air_voice_track_t track)
{
    av_cmd_t cmd = {.type = CMD_PLAY_TOGGLE, .track = track};
    return post_command(&cmd, false);
}

esp_err_t air_voice_play_stop(void)
{
    av_cmd_t cmd = {.type = CMD_PLAY_STOP};
    return post_command(&cmd, true);
}

esp_err_t air_voice_play_pause_toggle(void)
{
    av_cmd_t cmd = {.type = CMD_PLAY_PAUSE};
    return post_command(&cmd, true);
}

esp_err_t air_voice_set_volume(int volume)
{
    if (!s_av) {
        return ESP_ERR_INVALID_STATE;
    }
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    lock();
    const air_voice_volume_hooks_t hooks = s_av->hooks;
    unlock();
    esp_err_t err;
    if (hooks.set_volume) {
        err = hooks.set_volume(hooks.ctx, volume);
    } else {
        audio_mixer_handle_t mixer = NULL;
        err = audio_hub_get_mixer(&mixer);
        if (err == ESP_OK) {
            err = audio_mixer_set_output_volume(mixer, volume);
        }
    }
    if (err == ESP_OK) {
        AV_LOGI("speaker volume %d%%", volume);
    }
    return err;
}

static int current_volume(const air_voice_volume_hooks_t *hooks)
{
    int volume = -1;
    if (hooks->get_volume) {
        if (hooks->get_volume(hooks->ctx, &volume) != ESP_OK) {
            volume = -1;
        }
    } else {
        audio_mixer_handle_t mixer = NULL;
        if (audio_hub_get_mixer(&mixer) != ESP_OK ||
            audio_mixer_get_output_volume(mixer, &volume) != ESP_OK) {
            volume = -1;
        }
    }
    return volume;
}

esp_err_t air_voice_get_status(air_voice_status_t *out)
{
    if (!s_av || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    av_service_t *s = s_av;
    const int64_t now = esp_timer_get_time();
    lock();
    *out = s->snap;
    out->web_lease = now < s->web_lease_until_us;
    const air_voice_volume_hooks_t hooks = s->hooks;
    unlock();
    out->volume = current_volume(&hooks);
    out->log_seq = air_voice_log_latest_seq();
    return ESP_OK;
}

esp_err_t air_voice_get_config(air_voice_config_t *out)
{
    if (!s_av || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    *out = s_av->config;
    unlock();
    return ESP_OK;
}

esp_err_t air_voice_set_config(const air_voice_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    av_cmd_t cmd = {.type = CMD_SET_CONFIG, .config = *config};
    air_voice_config_sanitize(&cmd.config);
    return post_command(&cmd, true);
}

esp_err_t air_voice_apply_preset(const char *preset)
{
    air_voice_config_t config;
    esp_err_t err = air_voice_config_preset(preset, &config);
    if (err != ESP_OK) {
        return err;
    }
    return air_voice_set_config(&config);
}

esp_err_t air_voice_next_preset(void)
{
    air_voice_config_t config;
    esp_err_t err = air_voice_get_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    const char *const *names = NULL;
    const size_t count = air_voice_preset_names(&names);
    size_t next = 0;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(names[i], config.preset) == 0) {
            next = (i + 1U) % count;
            break;
        }
    }
    return air_voice_apply_preset(names[next]);
}

size_t air_voice_list_recordings(air_voice_recording_t *out, size_t max, size_t *out_total)
{
    if (!s_av) {
        if (out_total) *out_total = 0;
        return 0;
    }
    return av_store_list(out, max, out_total);
}

esp_err_t air_voice_delete_recording(const char *name)
{
    if (!s_av) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!air_voice_name_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    const air_voice_state_t state = s_av->snap.state;
    const bool in_use = (state == AIR_VOICE_STATE_RECORDING || state == AIR_VOICE_STATE_SAVING) &&
        strcmp(s_av->snap.record_name, name) == 0;
    const bool playing = (s_av->snap.play_state == AIR_VOICE_PLAY_PLAYING ||
                          s_av->snap.play_state == AIR_VOICE_PLAY_PAUSED) &&
        strcmp(s_av->snap.play_name, name) == 0;
    unlock();
    if (in_use) {
        return ESP_ERR_INVALID_STATE;
    }
    if (playing) {
        (void)air_voice_play_stop();
    }
    const esp_err_t err = av_store_delete(name);
    if (err == ESP_OK) {
        AV_LOGI("deleted %s", name);
        const av_writer_msg_t refresh = {.op = WR_REFRESH_SPACE};
        (void)xQueueSend(s_av->writer_q, &refresh, 0);
        lock();
        if (strcmp(s_av->last_name, name) == 0) {
            s_av->last_name[0] = '\0';
            s_av->last_duration_ms = 0;
            (void)av_store_newest(s_av->last_name, sizeof(s_av->last_name),
                                  &s_av->last_duration_ms);
        }
        unlock();
    }
    return err;
}

esp_err_t air_voice_recording_path(const char *name, air_voice_track_t track,
                                   char *out_path, size_t out_size)
{
    return av_store_path(name, track == AIR_VOICE_TRACK_RAW ? AV_FILE_RAW : AV_FILE_PROCESSED,
                         out_path, out_size);
}

esp_err_t air_voice_meta_path(const char *name, char *out_path, size_t out_size)
{
    return av_store_path(name, AV_FILE_META, out_path, out_size);
}

void air_voice_wav_fix_header(uint8_t header[44], uint64_t file_bytes)
{
    if (header) {
        av_wav_patch_header(header, file_bytes);
    }
}

const char *air_voice_state_name(air_voice_state_t state)
{
    static const char *const names[] = {"idle", "monitor", "recording", "saving"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}

const char *air_voice_play_state_name(air_voice_play_state_t state)
{
    static const char *const names[] = {"idle", "playing", "paused", "finished", "error"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}

const char *air_voice_hint_name(air_voice_hint_t hint)
{
    static const char *const names[] = {"none", "good", "clipping", "too_quiet", "low_snr", "airflow"};
    return (unsigned)hint < sizeof(names) / sizeof(names[0]) ? names[hint] : "none";
}
