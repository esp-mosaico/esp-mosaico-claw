/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bluetooth_audio_runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_MOSAIC_UI_BLUETOOTH_AUDIO

#include "decoder/esp_audio_dec_default.h"
#include "esp_asrc.h"
#include "esp_bt.h"
#include "esp_bt_audio.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_event.h"
#include "esp_bt_audio_host.h"
#include "esp_bt_audio_playback.h"
#include "esp_bt_audio_stream.h"
#include "esp_bt_audio_vol.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#define BT_AUDIO_DEVICE_NAME       "ESP-Claw-Audio"
#define BT_AUDIO_TASK_STACK        8192
#define BT_AUDIO_TASK_PRIORITY     8
#define BT_AUDIO_PACKET_WAIT_MS    50
#define BT_AUDIO_TASK_STOP_MS      2000
#define BT_AUDIO_DEC_BUFFER_INIT   4096
#define BT_AUDIO_ASRC_COMPLEXITY   2
#define BT_AUDIO_ASRC_TIMEOUT_MS   100

typedef struct {
    esp_asrc_handle_t handle;
    esp_asrc_buffer_alignment_t alignment;
    uint8_t *input;
    uint8_t *output;
    uint32_t input_size;
    uint32_t output_size;
    uint16_t input_frame_bytes;
    uint16_t output_frame_bytes;
    bool bypass;
} bluetooth_audio_converter_t;

struct bluetooth_audio_runtime {
    SemaphoreHandle_t lock;
    audio_mixer_handle_t mixer;
    audio_mixer_track_handle_t track;
    bluetooth_audio_changed_cb_t on_changed;
    void *user_ctx;
    bluetooth_audio_snapshot_t model;
    esp_bt_audio_stream_handle_t stream;
    TaskHandle_t stream_task;
    volatile bool stream_task_stop;
    bool started;
    bool audio_initialized;
    bool stopping;
    uint8_t peer_addr[6];
    uint8_t *cover_data;
    size_t cover_size;
};

static const char *TAG = "mosaic_bt_audio";
static bool s_decoders_registered;

static void runtime_notify(bluetooth_audio_runtime_handle_t runtime)
{
    bluetooth_audio_changed_cb_t callback;
    void *user_ctx;
    uint32_t revision;

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    revision = ++runtime->model.revision;
    callback = runtime->on_changed;
    user_ctx = runtime->user_ctx;
    xSemaphoreGive(runtime->lock);
    if (callback) {
        callback(revision, user_ctx);
    }
}

static void runtime_set_error(bluetooth_audio_runtime_handle_t runtime,
                              const char *message)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->model.state = BLUETOOTH_AUDIO_STATE_ERROR;
    snprintf(runtime->model.error, sizeof(runtime->model.error), "%s",
             message ? message : "Unknown Bluetooth error");
    xSemaphoreGive(runtime->lock);
    runtime_notify(runtime);
}

static void converter_close(bluetooth_audio_converter_t *converter)
{
    if (converter->handle) {
        esp_asrc_close(converter->handle);
    }
    free(converter->input);
    free(converter->output);
    memset(converter, 0, sizeof(*converter));
}

static esp_err_t converter_open(bluetooth_audio_converter_t *converter,
                                uint32_t src_rate, uint8_t src_channels,
                                uint8_t src_bits, uint32_t dst_rate,
                                uint8_t dst_channels, uint8_t dst_bits)
{
    memset(converter, 0, sizeof(*converter));
    converter->input_frame_bytes = src_channels * (src_bits / 8U);
    converter->output_frame_bytes = dst_channels * (dst_bits / 8U);
    if (src_rate == dst_rate && src_channels == dst_channels &&
            src_bits == dst_bits) {
        converter->bypass = true;
        return ESP_OK;
    }

    esp_asrc_cfg_t config = {
        .src_info = {
            .sample_rate = src_rate,
            .channel = src_channels,
            .bits_per_sample = src_bits,
        },
        .dest_info = {
            .sample_rate = dst_rate,
            .channel = dst_channels,
            .bits_per_sample = dst_bits,
        },
        .weight = NULL,
        .weight_len = 0,
        .perf_type = ESP_ASRC_PERF_TYPE_AUTO,
        .complexity = BT_AUDIO_ASRC_COMPLEXITY,
        .timeout_ms = BT_AUDIO_ASRC_TIMEOUT_MS,
    };
    if (esp_asrc_open(&config, &converter->handle) != ESP_ASRC_ERR_OK ||
            esp_asrc_get_buffer_alignment(&converter->alignment) !=
                ESP_ASRC_ERR_OK ||
            esp_asrc_get_bytes_per_sample(converter->handle,
                &converter->input_frame_bytes,
                &converter->output_frame_bytes) != ESP_ASRC_ERR_OK) {
        converter_close(converter);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t converter_resize(uint8_t **buffer, uint32_t *capacity,
                                  uint32_t size, uint32_t address_alignment,
                                  uint32_t size_alignment)
{
    if (*capacity >= size) {
        return ESP_OK;
    }
    uint32_t allocated = 0;
    uint8_t *replacement = esp_asrc_align_alloc(
        size, address_alignment, size_alignment, &allocated);
    if (!replacement) {
        return ESP_ERR_NO_MEM;
    }
    free(*buffer);
    *buffer = replacement;
    *capacity = allocated;
    return ESP_OK;
}

static esp_err_t converter_process(bluetooth_audio_converter_t *converter,
                                   const uint8_t *input, uint32_t input_size,
                                   uint8_t **out_data, uint32_t *out_size)
{
    if (converter->bypass) {
        *out_data = (uint8_t *)input;
        *out_size = input_size;
        return ESP_OK;
    }
    if (converter->input_frame_bytes == 0 ||
            input_size % converter->input_frame_bytes != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t input_frames = input_size / converter->input_frame_bytes;
    uint32_t output_frames = 0;
    if (esp_asrc_get_out_sample_num(converter->handle, input_frames,
                                    &output_frames) != ESP_ASRC_ERR_OK ||
            output_frames == 0) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(converter_resize(
        &converter->input, &converter->input_size, input_size,
        converter->alignment.inbuf_addr_align,
        converter->alignment.inbuf_size_align), TAG,
        "allocate ASRC input");
    ESP_RETURN_ON_ERROR(converter_resize(
        &converter->output, &converter->output_size,
        output_frames * converter->output_frame_bytes,
        converter->alignment.outbuf_addr_align,
        converter->alignment.outbuf_size_align), TAG,
        "allocate ASRC output");
    memcpy(converter->input, input, input_size);
    output_frames = converter->output_size / converter->output_frame_bytes;
    if (esp_asrc_process(converter->handle, converter->input, input_frames,
                         converter->output, &output_frames) !=
            ESP_ASRC_ERR_OK) {
        return ESP_FAIL;
    }
    *out_data = converter->output;
    *out_size = output_frames * converter->output_frame_bytes;
    return ESP_OK;
}

static esp_err_t stream_write_pcm(bluetooth_audio_runtime_handle_t runtime,
                                  bluetooth_audio_converter_t *converter,
                                  const uint8_t *pcm, uint32_t pcm_size)
{
    uint8_t *converted = NULL;
    uint32_t converted_size = 0;
    ESP_RETURN_ON_ERROR(converter_process(converter, pcm, pcm_size,
                                          &converted, &converted_size), TAG,
                        "convert Bluetooth PCM");
    if (converted_size == 0) {
        return ESP_OK;
    }
    size_t written = audio_mixer_track_write(runtime->track, converted,
                                             converted_size);
    return written == converted_size ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t stream_decode_packet(
    bluetooth_audio_runtime_handle_t runtime,
    esp_audio_simple_dec_handle_t decoder,
    bluetooth_audio_converter_t *converter,
    uint8_t **decode_buffer, uint32_t *decode_capacity,
    esp_bt_audio_stream_packet_t *packet)
{
    esp_audio_simple_dec_raw_t raw = {
        .buffer = packet->data,
        .len = packet->size,
        .eos = packet->is_done,
        .frame_recover = packet->bad_frame ?
            ESP_AUDIO_SIMPLE_DEC_RECOVERY_PLC :
            ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    while (raw.len > 0 && !runtime->stream_task_stop) {
        esp_audio_simple_dec_out_t output = {
            .buffer = *decode_buffer,
            .len = *decode_capacity,
        };
        esp_audio_err_t err = esp_audio_simple_dec_process(decoder, &raw,
                                                           &output);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && output.needed_size > 0) {
            uint8_t *replacement = realloc(*decode_buffer,
                                           output.needed_size);
            if (!replacement) {
                return ESP_ERR_NO_MEM;
            }
            *decode_buffer = replacement;
            *decode_capacity = output.needed_size;
            continue;
        }
        uint32_t consumed = raw.consumed;
        if (consumed > raw.len) {
            return ESP_ERR_INVALID_SIZE;
        }
        raw.buffer += consumed;
        raw.len -= consumed;
        if (err != ESP_AUDIO_ERR_OK) {
            if (consumed == 0 && raw.len > 0) {
                ++raw.buffer;
                --raw.len;
            }
            continue;
        }
        if (output.decoded_size > 0) {
            ESP_RETURN_ON_ERROR(stream_write_pcm(runtime, converter,
                                                  output.buffer,
                                                  output.decoded_size), TAG,
                                "write Bluetooth PCM");
        }
        if (consumed == 0 && output.decoded_size == 0) {
            break;
        }
    }
    return ESP_OK;
}

static void bluetooth_stream_task(void *arg)
{
    bluetooth_audio_runtime_handle_t runtime = arg;
    esp_bt_audio_stream_handle_t stream = runtime->stream;
    esp_bt_audio_stream_codec_info_t codec = {0};
    esp_audio_simple_dec_handle_t decoder = NULL;
    bluetooth_audio_converter_t converter = {0};
    uint8_t *decode_buffer = NULL;
    uint32_t decode_capacity = BT_AUDIO_DEC_BUFFER_INIT;
    uint32_t output_rate = 0;
    uint8_t output_channels = 0;
    uint8_t output_bits = 0;
    uint8_t source_channels = 0;
    esp_err_t setup_err = ESP_OK;

    if (esp_bt_audio_stream_get_codec_info(stream, &codec) != ESP_OK) {
        setup_err = ESP_FAIL;
        goto cleanup;
    }
    /* esp_bt_audio stores speaker positions in codec.channels (FL | FR),
     * despite the public field comment calling it a channel count. Convert
     * that location mask to the PCM channel count expected by ASRC.
     */
    source_channels = (uint8_t)__builtin_popcount(codec.channels);
    if (source_channels == 0) {
        setup_err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = (esp_audio_simple_dec_type_t)codec.codec_type,
        .dec_cfg = codec.codec_cfg,
        .cfg_size = codec.cfg_size,
        .use_frame_dec = true,
    };
    if (esp_audio_simple_dec_open(&decoder_config, &decoder) !=
            ESP_AUDIO_ERR_OK) {
        setup_err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    decode_buffer = malloc(decode_capacity);
    if (!decode_buffer ||
            audio_mixer_track_info(runtime->track, &output_rate,
                                   &output_channels, &output_bits) != ESP_OK ||
            converter_open(&converter, codec.sample_rate, source_channels,
                           codec.bits, output_rate, output_channels,
                           output_bits) != ESP_OK) {
        setup_err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ESP_LOGI(TAG, "stream codec=%s %" PRIu32 "Hz/%uch/%" PRIu32
             "bit -> %" PRIu32 "Hz/%uch/%ubit",
             esp_audio_simple_dec_get_name(decoder_config.dec_type),
             codec.sample_rate, source_channels, codec.bits, output_rate,
             output_channels, output_bits);
    while (!runtime->stream_task_stop) {
        esp_bt_audio_stream_packet_t packet = {0};
        esp_err_t err = esp_bt_audio_stream_acquire_read(
            stream, &packet, BT_AUDIO_PACKET_WAIT_MS);
        if (err != ESP_OK) {
            continue;
        }
        if (packet.data && packet.size > 0) {
            err = stream_decode_packet(runtime, decoder, &converter,
                                       &decode_buffer, &decode_capacity,
                                       &packet);
        }
        (void)esp_bt_audio_stream_release_read(stream, &packet);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "stream processing stopped: %s",
                     esp_err_to_name(err));
            setup_err = err;
            break;
        }
    }

cleanup:
    converter_close(&converter);
    if (decoder) {
        esp_audio_simple_dec_close(decoder);
    }
    free(decode_buffer);
    if (setup_err != ESP_OK && !runtime->stopping) {
        runtime_set_error(runtime, "Bluetooth audio decoder failed");
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->stream_task = NULL;
    xSemaphoreGive(runtime->lock);
    vTaskDelete(NULL);
}

static esp_err_t stream_task_start(bluetooth_audio_runtime_handle_t runtime,
                                   esp_bt_audio_stream_handle_t stream)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (runtime->stream_task) {
        xSemaphoreGive(runtime->lock);
        return ESP_OK;
    }
    runtime->stream = stream;
    runtime->stream_task_stop = false;
    BaseType_t ok = xTaskCreate(bluetooth_stream_task, "mosaic_bt_pcm",
                                BT_AUDIO_TASK_STACK, runtime,
                                BT_AUDIO_TASK_PRIORITY,
                                &runtime->stream_task);
    xSemaphoreGive(runtime->lock);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void stream_task_stop(bluetooth_audio_runtime_handle_t runtime)
{
    runtime->stream_task_stop = true;
    for (uint32_t waited = 0; waited < BT_AUDIO_TASK_STOP_MS; waited += 10U) {
        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        bool done = runtime->stream_task == NULL;
        xSemaphoreGive(runtime->lock);
        if (done) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    TaskHandle_t lingering_task = runtime->stream_task;
    runtime->stream_task = NULL;
    xSemaphoreGive(runtime->lock);
    if (lingering_task) {
        ESP_LOGE(TAG, "force stopping stalled Bluetooth stream task");
        vTaskDelete(lingering_task);
    }
    (void)audio_mixer_track_stop(runtime->track);
}

static void copy_metadata_text(char *destination, size_t capacity,
                               const uint8_t *value, uint32_t length)
{
    size_t count = length < capacity - 1U ? length : capacity - 1U;
    memcpy(destination, value, count);
    destination[count] = '\0';
}

static void cover_clear_locked(bluetooth_audio_runtime_handle_t runtime)
{
    free(runtime->cover_data);
    runtime->cover_data = NULL;
    runtime->cover_size = 0;
    runtime->model.has_cover = false;
    ++runtime->model.cover_revision;
}

static void cover_store(bluetooth_audio_runtime_handle_t runtime,
                        const esp_bt_audio_playback_cover_art_t *cover)
{
    if (!cover || !cover->data || cover->size == 0) {
        return;
    }
    uint8_t *copy = heap_caps_malloc_prefer(
        cover->size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_DEFAULT);
    if (!copy) {
        ESP_LOGW(TAG, "no memory for cover art (%" PRIu32 " bytes)",
                 cover->size);
        return;
    }
    memcpy(copy, cover->data, cover->size);

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    free(runtime->cover_data);
    runtime->cover_data = copy;
    runtime->cover_size = cover->size;
    runtime->model.has_cover = true;
    ++runtime->model.cover_revision;
    xSemaphoreGive(runtime->lock);
    ESP_LOGI(TAG, "cover art ready: %" PRIu32 " bytes, fourcc=0x%08" PRIx32,
             cover->size, cover->format_fourcc);
}

static void playback_metadata_event(
    bluetooth_audio_runtime_handle_t runtime,
    const esp_bt_audio_event_playback_metadata_t *metadata)
{
    if (metadata->type == ESP_BT_AUDIO_PLAYBACK_METADATA_COVER_ART) {
        if (metadata->value &&
                metadata->length >=
                    sizeof(esp_bt_audio_playback_cover_art_t)) {
            cover_store(runtime,
                (const esp_bt_audio_playback_cover_art_t *)metadata->value);
            runtime_notify(runtime);
        }
        return;
    }
    bool request_cover = false;
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (metadata->type == ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE) {
        char title[sizeof(runtime->model.title)];
        copy_metadata_text(title, sizeof(title),
                           metadata->value, metadata->length);
        if (strcmp(title, runtime->model.title) != 0) {
            cover_clear_locked(runtime);
            request_cover = true;
        }
        copy_metadata_text(runtime->model.title,
                           sizeof(runtime->model.title),
                           metadata->value, metadata->length);
    } else if (metadata->type == ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST) {
        copy_metadata_text(runtime->model.artist,
                           sizeof(runtime->model.artist),
                           metadata->value, metadata->length);
    } else if (metadata->type ==
               ESP_BT_AUDIO_PLAYBACK_METADATA_PLAYING_TIME) {
        char duration[16] = {0};
        copy_metadata_text(duration, sizeof(duration), metadata->value,
                           metadata->length);
        runtime->model.duration_ms = strtoul(duration, NULL, 10);
    }
    xSemaphoreGive(runtime->lock);
    if (request_cover) {
        (void)esp_bt_audio_playback_request_metadata(
            ESP_BT_AUDIO_PLAYBACK_METADATA_COVER_ART);
    }
    runtime_notify(runtime);
}

static void bluetooth_event(esp_bt_audio_event_t event, void *event_data,
                            void *user_data)
{
    bluetooth_audio_runtime_handle_t runtime = user_data;
    if (!runtime || runtime->stopping) {
        return;
    }
    switch (event) {
    case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
        const esp_bt_audio_event_connection_st_t *connection = event_data;
        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        runtime->model.connected = connection->connected;
        if (connection->connected) {
            memcpy(runtime->peer_addr, connection->addr,
                   sizeof(runtime->peer_addr));
            runtime->model.state = BLUETOOTH_AUDIO_STATE_CONNECTED;
            runtime->model.error[0] = '\0';
        } else {
            runtime->model.playing = false;
            runtime->model.state = BLUETOOTH_AUDIO_STATE_DISCOVERABLE;
            runtime->model.title[0] = '\0';
            runtime->model.artist[0] = '\0';
            runtime->model.position_ms = 0;
            runtime->model.duration_ms = 0;
            cover_clear_locked(runtime);
            memset(runtime->peer_addr, 0, sizeof(runtime->peer_addr));
        }
        xSemaphoreGive(runtime->lock);
        runtime_notify(runtime);
        break;
    }
    case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
        const esp_bt_audio_event_stream_st_t *stream_event = event_data;
        esp_bt_audio_stream_dir_t direction = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
        (void)esp_bt_audio_stream_get_dir(stream_event->stream_handle,
                                          &direction);
        if (direction != ESP_BT_AUDIO_STREAM_DIR_SINK) {
            break;
        }
        if (stream_event->state == ESP_BT_AUDIO_STREAM_STATE_ALLOCATED) {
            runtime->stream = stream_event->stream_handle;
            (void)esp_bt_audio_playback_reg_notifications(
                ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE |
                ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE |
                ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_POS_CHANGED |
                ESP_BT_AUDIO_PLAYBACK_EVENT_NOW_PLAYING_CHANGE);
            (void)esp_bt_audio_playback_request_metadata(
                ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE |
                ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST |
                ESP_BT_AUDIO_PLAYBACK_METADATA_PLAYING_TIME |
                ESP_BT_AUDIO_PLAYBACK_METADATA_COVER_ART);
        } else if (stream_event->state ==
                   ESP_BT_AUDIO_STREAM_STATE_STARTED) {
            if (stream_task_start(runtime, stream_event->stream_handle) !=
                    ESP_OK) {
                runtime_set_error(runtime, "Cannot start audio stream");
            }
        } else if (stream_event->state ==
                       ESP_BT_AUDIO_STREAM_STATE_STOPPED ||
                   stream_event->state ==
                       ESP_BT_AUDIO_STREAM_STATE_RELEASED) {
            stream_task_stop(runtime);
            runtime->stream = NULL;
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_PLAYBACK_STATUS_CHG: {
        const esp_bt_audio_event_playback_st_t *status = event_data;
        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        if (status->event ==
                ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE) {
            runtime->model.playing =
                status->evt_param.play_status ==
                ESP_BT_AUDIO_PLAYBACK_STATUS_PLAYING;
            runtime->model.state = runtime->model.playing ?
                BLUETOOTH_AUDIO_STATE_PLAYING :
                BLUETOOTH_AUDIO_STATE_PAUSED;
        } else if (status->event ==
                   ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_POS_CHANGED) {
            runtime->model.position_ms = status->evt_param.position;
        }
        xSemaphoreGive(runtime->lock);
        if (status->event == ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE ||
                status->event ==
                    ESP_BT_AUDIO_PLAYBACK_EVENT_NOW_PLAYING_CHANGE) {
            xSemaphoreTake(runtime->lock, portMAX_DELAY);
            runtime->model.title[0] = '\0';
            runtime->model.artist[0] = '\0';
            runtime->model.position_ms = 0;
            runtime->model.duration_ms = 0;
            cover_clear_locked(runtime);
            xSemaphoreGive(runtime->lock);
            (void)esp_bt_audio_playback_request_metadata(
                ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE |
                ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST |
                ESP_BT_AUDIO_PLAYBACK_METADATA_PLAYING_TIME |
                ESP_BT_AUDIO_PLAYBACK_METADATA_COVER_ART);
        }
        runtime_notify(runtime);
        break;
    }
    case ESP_BT_AUDIO_EVENT_PLAYBACK_METADATA:
        playback_metadata_event(runtime, event_data);
        break;
    case ESP_BT_AUDIO_EVENT_VOL_ABSOLUTE: {
        const esp_bt_audio_event_vol_absolute_t *volume = event_data;
        int percent = volume->mute ? 0 : (volume->vol * 100 + 63) / 127;
        (void)audio_mixer_set_output_volume(runtime->mixer, percent);
        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        runtime->model.volume_percent = percent;
        xSemaphoreGive(runtime->lock);
        runtime_notify(runtime);
        break;
    }
    case ESP_BT_AUDIO_EVENT_VOL_RELATIVE:
        (void)bluetooth_audio_runtime_adjust_volume(
            runtime,
            ((const esp_bt_audio_event_vol_relative_t *)event_data)->up_down ?
                5 : -5);
        break;
    default:
        break;
    }
}

esp_err_t bluetooth_audio_runtime_create(
    const bluetooth_audio_runtime_config_t *config,
    bluetooth_audio_runtime_handle_t *out_runtime)
{
    if (!config || !config->mixer || !out_runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    bluetooth_audio_runtime_handle_t runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        return ESP_ERR_NO_MEM;
    }
    runtime->lock = xSemaphoreCreateMutex();
    if (!runtime->lock) {
        free(runtime);
        return ESP_ERR_NO_MEM;
    }
    runtime->mixer = config->mixer;
    runtime->on_changed = config->on_changed;
    runtime->user_ctx = config->user_ctx;
    runtime->model.state = BLUETOOTH_AUDIO_STATE_OFF;
    snprintf(runtime->model.device_name, sizeof(runtime->model.device_name),
             "%s", BT_AUDIO_DEVICE_NAME);
    (void)audio_mixer_get_output_volume(runtime->mixer,
                                        &runtime->model.volume_percent);
    *out_runtime = runtime;
    return ESP_OK;
}

void bluetooth_audio_runtime_delete(bluetooth_audio_runtime_handle_t runtime)
{
    if (!runtime) {
        return;
    }
    (void)bluetooth_audio_runtime_stop(runtime);
    free(runtime->cover_data);
    vSemaphoreDelete(runtime->lock);
    free(runtime);
}

esp_err_t bluetooth_audio_runtime_start(bluetooth_audio_runtime_handle_t runtime)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    if (runtime->started) {
        return ESP_OK;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->model.state = BLUETOOTH_AUDIO_STATE_STARTING;
    runtime->model.error[0] = '\0';
    xSemaphoreGive(runtime->lock);
    runtime_notify(runtime);

    esp_err_t err = audio_mixer_open_track(runtime->mixer,
        AUDIO_MIXER_TRACK_APP, "mosaic/bluetooth", &runtime->track);
    if (err != ESP_OK) {
        runtime_set_error(runtime, "Audio output is busy");
        return err;
    }
    if (!s_decoders_registered) {
        if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
                esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
            runtime_set_error(runtime, "Audio decoder registration failed");
            (void)audio_mixer_close_track(runtime->track);
            runtime->track = NULL;
            return ESP_FAIL;
        }
        s_decoders_registered = true;
    }

    esp_bt_controller_config_t controller_config =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&controller_config);
    if (err == ESP_OK) {
        err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    }
    if (err != ESP_OK) {
        runtime_set_error(runtime, "Bluetooth controller start failed");
        goto fail;
    }

    esp_bt_audio_host_bluedroid_cfg_t host_config =
        ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT();
    snprintf(host_config.dev_name, sizeof(host_config.dev_name), "%s",
             runtime->model.device_name);
    esp_bt_audio_config_t audio_config = {
        .host_config = &host_config,
        .event_cb = bluetooth_event,
        .event_user_ctx = runtime,
        .classic.roles = ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK |
                         ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT |
                         ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_TG,
    };
    err = esp_bt_audio_init(&audio_config);
    runtime->audio_initialized = err == ESP_OK;
    if (err == ESP_OK) {
        err = esp_bt_audio_classic_set_scan_mode(true, true);
    }
    if (err != ESP_OK) {
        runtime_set_error(runtime, "Bluetooth audio start failed");
        goto fail;
    }

    runtime->started = true;
    runtime->stopping = false;
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->model.state = BLUETOOTH_AUDIO_STATE_DISCOVERABLE;
    xSemaphoreGive(runtime->lock);
    runtime_notify(runtime);
    ESP_LOGI(TAG, "A2DP Sink discoverable as %s", runtime->model.device_name);
    return ESP_OK;

fail:
    if (runtime->audio_initialized) {
        esp_bt_audio_deinit();
        runtime->audio_initialized = false;
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        (void)esp_bt_controller_disable();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        (void)esp_bt_controller_deinit();
    }
    if (runtime->track) {
        (void)audio_mixer_close_track(runtime->track);
        runtime->track = NULL;
    }
    return err;
}

esp_err_t bluetooth_audio_runtime_stop(bluetooth_audio_runtime_handle_t runtime)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!runtime->started && !runtime->track) {
        return ESP_OK;
    }
    runtime->stopping = true;
    (void)esp_bt_audio_classic_set_scan_mode(false, false);
    if (runtime->model.connected) {
        (void)esp_bt_audio_classic_disconnect(
            ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK, runtime->peer_addr);
    }
    if (runtime->stream_task) {
        stream_task_stop(runtime);
    }
    esp_bt_audio_deinit();
    runtime->audio_initialized = false;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        (void)esp_bt_controller_disable();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        (void)esp_bt_controller_deinit();
    }
    if (runtime->track) {
        (void)audio_mixer_close_track(runtime->track);
        runtime->track = NULL;
    }
    runtime->started = false;
    runtime->stopping = false;
    runtime->stream = NULL;
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->model.state = BLUETOOTH_AUDIO_STATE_OFF;
    runtime->model.connected = false;
    runtime->model.playing = false;
    xSemaphoreGive(runtime->lock);
    runtime_notify(runtime);
    ESP_LOGI(TAG, "Bluetooth controller stopped on App exit");
    return ESP_OK;
}

esp_err_t bluetooth_audio_runtime_get_snapshot(
    bluetooth_audio_runtime_handle_t runtime,
    bluetooth_audio_snapshot_t *out_snapshot)
{
    if (!runtime || !out_snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    *out_snapshot = runtime->model;
    xSemaphoreGive(runtime->lock);
    return ESP_OK;
}

esp_err_t bluetooth_audio_runtime_take_cover(
    bluetooth_audio_runtime_handle_t runtime, uint8_t **out_data,
    size_t *out_size, uint32_t *out_revision)
{
    if (!runtime || !out_data || !out_size || !out_revision) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_size = 0;
    *out_revision = 0;

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (!runtime->cover_data || runtime->cover_size == 0) {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out_data = runtime->cover_data;
    *out_size = runtime->cover_size;
    *out_revision = runtime->model.cover_revision;
    runtime->cover_data = NULL;
    runtime->cover_size = 0;
    xSemaphoreGive(runtime->lock);
    return ESP_OK;
}

esp_err_t bluetooth_audio_runtime_toggle_play(
    bluetooth_audio_runtime_handle_t runtime)
{
    if (!runtime || !runtime->model.connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return runtime->model.playing ? esp_bt_audio_playback_pause() :
                                    esp_bt_audio_playback_play();
}

esp_err_t bluetooth_audio_runtime_previous(
    bluetooth_audio_runtime_handle_t runtime)
{
    return runtime && runtime->model.connected ?
        esp_bt_audio_playback_prev() : ESP_ERR_INVALID_STATE;
}

esp_err_t bluetooth_audio_runtime_next(
    bluetooth_audio_runtime_handle_t runtime)
{
    return runtime && runtime->model.connected ?
        esp_bt_audio_playback_next() : ESP_ERR_INVALID_STATE;
}

esp_err_t bluetooth_audio_runtime_adjust_volume(
    bluetooth_audio_runtime_handle_t runtime, int delta_percent)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    int volume = 0;
    ESP_RETURN_ON_ERROR(audio_mixer_get_output_volume(runtime->mixer,
                                                      &volume), TAG,
                        "read output volume");
    return bluetooth_audio_runtime_set_volume(runtime,
                                              volume + delta_percent);
}

esp_err_t bluetooth_audio_runtime_set_volume(
    bluetooth_audio_runtime_handle_t runtime, int volume)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    ESP_RETURN_ON_ERROR(audio_mixer_set_output_volume(runtime->mixer, volume),
                        TAG, "set output volume");
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    const bool connected = runtime->model.connected;
    runtime->model.volume_percent = volume;
    xSemaphoreGive(runtime->lock);
    if (connected) {
        (void)esp_bt_audio_vol_set_absolute((volume * 127 + 50) / 100);
    }
    runtime_notify(runtime);
    return ESP_OK;
}

#else

esp_err_t bluetooth_audio_runtime_take_cover(
    bluetooth_audio_runtime_handle_t runtime, uint8_t **out_data,
    size_t *out_size, uint32_t *out_revision)
{
    (void)runtime;
    (void)out_data;
    (void)out_size;
    (void)out_revision;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_audio_runtime_create(
    const bluetooth_audio_runtime_config_t *config,
    bluetooth_audio_runtime_handle_t *out_runtime)
{
    (void)config;
    (void)out_runtime;
    return ESP_ERR_NOT_SUPPORTED;
}

void bluetooth_audio_runtime_delete(bluetooth_audio_runtime_handle_t runtime)
{
    (void)runtime;
}

esp_err_t bluetooth_audio_runtime_start(bluetooth_audio_runtime_handle_t runtime)
{
    (void)runtime;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_audio_runtime_stop(bluetooth_audio_runtime_handle_t runtime)
{
    (void)runtime;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_audio_runtime_get_snapshot(
    bluetooth_audio_runtime_handle_t runtime,
    bluetooth_audio_snapshot_t *out_snapshot)
{
    (void)runtime;
    (void)out_snapshot;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_audio_runtime_toggle_play(
    bluetooth_audio_runtime_handle_t runtime)
{
    (void)runtime;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_audio_runtime_previous(
    bluetooth_audio_runtime_handle_t runtime)
{
    (void)runtime;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_audio_runtime_next(
    bluetooth_audio_runtime_handle_t runtime)
{
    (void)runtime;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_audio_runtime_adjust_volume(
    bluetooth_audio_runtime_handle_t runtime, int delta_percent)
{
    (void)runtime;
    (void)delta_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_audio_runtime_set_volume(
    bluetooth_audio_runtime_handle_t runtime, int volume_percent)
{
    (void)runtime;
    (void)volume_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
