/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include "chiptune.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)

#include "audio_hub.h"
#include "audio_mixer.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "chiptune"

#define CHIPTUNE_BGM_CHANNELS       4
#define CHIPTUNE_SFX_CHANNELS       2
#define CHIPTUNE_CHANNEL_COUNT      (CHIPTUNE_BGM_CHANNELS + CHIPTUNE_SFX_CHANNELS)

/* 256 frames per chunk keeps pause/stop latency at ~16 ms @16 kHz while
 * amortising the task-loop overhead. */
#define CHIPTUNE_CHUNK_FRAMES       256
#define CHIPTUNE_MAX_CHUNK_BYTES    (CHIPTUNE_CHUNK_FRAMES * 2 * 4)

/* SFX overlay uses a fixed tick so short bursts feel snappy regardless of
 * BGM tempo. */
#define CHIPTUNE_SFX_TICK_MS        32

/* Per-voice int16 peak.  6 * VOICE_PEAK is the worst-case mix bus; keep well
 * under INT16_MAX to avoid soft-clipping when every voice hits at once. */
#define CHIPTUNE_VOICE_PEAK         300

/* Amp slew per sample: VOICE_PEAK / AMP_SLEW = samples to cross full range,
 * ~a few ms at 16 kHz -- fast enough to feel immediate, slow enough to kill
 * the click a hard step would produce on note changes. */
#define CHIPTUNE_AMP_SLEW           128

#define CHIPTUNE_TASK_STACK         4096
#define CHIPTUNE_TASK_PRIORITY      6

typedef struct {
    chiptune_wave_t wave;
    uint32_t phase;                  /* Q0.32 accumulator */
    uint32_t phase_inc;
    uint16_t lfsr;                   /* 15-bit Galois state, noise voice */

    int32_t target_amp;
    int32_t amp;

    const chiptune_pattern_t *pattern;   /* NULL = idle */
    uint16_t event_index;
    uint32_t remaining_samples;
    uint32_t samples_per_tick;
    bool one_shot;
} chiptune_voice_t;

typedef struct {
    audio_mixer_track_handle_t track;
    uint32_t sample_rate;
    uint8_t  out_channels;
    uint8_t  out_bits;

    uint32_t midi_phase_inc[128];
    uint32_t bgm_samples_per_tick;
    uint32_t sfx_samples_per_tick;

    chiptune_voice_t voices[CHIPTUNE_CHANNEL_COUNT];

    uint8_t sfx_cursor;              /* round-robin over the SFX slots */
    const chiptune_pattern_t *sfx_bank[CHIPTUNE_SFX_COUNT];

    volatile bool running;
    volatile bool paused;
    TaskHandle_t task;
    SemaphoreHandle_t lock;
} chiptune_state_t;

static chiptune_state_t s_state;
static bool s_initialised;

static inline void chiptune_lock(void)
{
    if (s_state.lock) {
        xSemaphoreTake(s_state.lock, portMAX_DELAY);
    }
}

static inline void chiptune_unlock(void)
{
    if (s_state.lock) {
        xSemaphoreGive(s_state.lock);
    }
}

/* ---- Built-in SFX presets --------------------------------------------- */

static const chiptune_event_t sfx_laser_events[] = {
    { 84, 1, 200, CHIPTUNE_WAVE_PULSE_12 },
    { 79, 1, 180, CHIPTUNE_WAVE_PULSE_12 },
    { 74, 1, 150, CHIPTUNE_WAVE_PULSE_12 },
    { 69, 1, 100, CHIPTUNE_WAVE_PULSE_12 },
};
static const chiptune_pattern_t sfx_laser = {
    .events = sfx_laser_events, .event_count = 4, .loop_from = 4,
};

static const chiptune_event_t sfx_hit_events[] = {
    { 60, 1, 200, CHIPTUNE_WAVE_NOISE },
    { 60, 1,  90, CHIPTUNE_WAVE_NOISE },
};
static const chiptune_pattern_t sfx_hit = {
    .events = sfx_hit_events, .event_count = 2, .loop_from = 2,
};

static const chiptune_event_t sfx_explosion_events[] = {
    { 90, 1, 255, CHIPTUNE_WAVE_NOISE },
    { 80, 1, 230, CHIPTUNE_WAVE_NOISE },
    { 70, 1, 200, CHIPTUNE_WAVE_NOISE },
    { 60, 1, 170, CHIPTUNE_WAVE_NOISE },
    { 50, 1, 130, CHIPTUNE_WAVE_NOISE },
    { 40, 1,  80, CHIPTUNE_WAVE_NOISE },
    { 30, 1,  40, CHIPTUNE_WAVE_NOISE },
};
static const chiptune_pattern_t sfx_explosion = {
    .events = sfx_explosion_events, .event_count = 7, .loop_from = 7,
};

static const chiptune_event_t sfx_powerup_events[] = {
    { 60, 1, 200, CHIPTUNE_WAVE_SQUARE },
    { 67, 1, 200, CHIPTUNE_WAVE_SQUARE },
    { 72, 1, 200, CHIPTUNE_WAVE_SQUARE },
    { 76, 1, 200, CHIPTUNE_WAVE_SQUARE },
    { 79, 2, 220, CHIPTUNE_WAVE_SQUARE },
};
static const chiptune_pattern_t sfx_powerup = {
    .events = sfx_powerup_events, .event_count = 5, .loop_from = 5,
};

static const chiptune_event_t sfx_enemy_shot_events[] = {
    { 55, 1, 160, CHIPTUNE_WAVE_SAWTOOTH },
    { 50, 1, 120, CHIPTUNE_WAVE_SAWTOOTH },
};
static const chiptune_pattern_t sfx_enemy_shot = {
    .events = sfx_enemy_shot_events, .event_count = 2, .loop_from = 2,
};

/* ---- MIDI -> phase_inc LUT -------------------------------------------- */

/* Table is populated once at engine start so the hot path is float-free. */
static void build_midi_lut(uint32_t sample_rate)
{
    const double scale = 4294967296.0 / (double)sample_rate;
    for (int n = 0; n < 128; ++n) {
        const double freq = 440.0 * pow(2.0, (n - 69) / 12.0);
        double inc = freq * scale;
        if (inc < 0.0) {
            inc = 0.0;
        } else if (inc >= 4294967295.0) {
            inc = 4294967295.0;
        }
        s_state.midi_phase_inc[n] = (uint32_t)inc;
    }
}

/* ---- Waveform samplers ------------------------------------------------ */

/* All samplers return signed 11-bit output in [-1024, +1023] so the wave *
 * amp multiply and the 6-voice sum stay comfortably inside int32. */

static inline int32_t sample_pulse(uint32_t phase, uint32_t duty_threshold)
{
    return phase < duty_threshold ? +1024 : -1024;
}

static inline int32_t sample_triangle(uint32_t phase)
{
    /* Fold the top half so each cycle rises then falls symmetrically. */
    uint32_t v = (phase & 0x80000000U) ? ~phase : phase;
    return (int32_t)((v >> 20) - 1024);
}

static inline int32_t sample_sawtooth(uint32_t phase)
{
    return (int32_t)((int32_t)phase >> 21);
}

static inline void step_lfsr(chiptune_voice_t *v)
{
    /* 15-bit Galois LFSR, taps at bits 0 XOR 1 -- NES APU mode-0 layout. */
    uint16_t feedback = (v->lfsr ^ (v->lfsr >> 1)) & 1U;
    v->lfsr = (v->lfsr >> 1) | (feedback << 14);
    if (v->lfsr == 0) {
        v->lfsr = 1;
    }
}

/* ---- Sequencer -------------------------------------------------------- */

static void voice_apply_event(chiptune_voice_t *v, const chiptune_event_t *ev)
{
    if (ev->note == 0 || ev->volume == 0) {
        /* Rest: preserve phase so a same-waveform successor has no glitch. */
        v->target_amp = 0;
        return;
    }
    v->wave      = (chiptune_wave_t)ev->waveform;
    v->phase_inc = s_state.midi_phase_inc[ev->note];
    v->target_amp = (int32_t)ev->volume * CHIPTUNE_VOICE_PEAK / 255;
}

static void voice_advance_event(chiptune_voice_t *v)
{
    const chiptune_pattern_t *pat = v->pattern;
    v->event_index++;
    if (v->event_index >= pat->event_count) {
        if (v->one_shot || pat->loop_from >= pat->event_count) {
            v->pattern = NULL;
            v->target_amp = 0;
            v->remaining_samples = 0;
            return;
        }
        v->event_index = pat->loop_from;
    }
    const chiptune_event_t *ev = &pat->events[v->event_index];
    v->remaining_samples = (uint32_t)ev->duration * v->samples_per_tick;
    voice_apply_event(v, ev);
}

static void voice_start_pattern(chiptune_voice_t *v,
                                const chiptune_pattern_t *pat,
                                uint32_t samples_per_tick,
                                bool one_shot)
{
    if (!pat || pat->event_count == 0) {
        v->pattern = NULL;
        v->target_amp = 0;
        return;
    }
    v->pattern            = pat;
    v->one_shot           = one_shot;
    v->samples_per_tick   = samples_per_tick;
    v->event_index        = 0;
    v->lfsr               = v->lfsr ? v->lfsr : 0x7FFFU;
    v->phase              = 0;
    v->remaining_samples  = (uint32_t)pat->events[0].duration * samples_per_tick;
    voice_apply_event(v, &pat->events[0]);
}

/* ---- Per-sample rendering --------------------------------------------- */

static int32_t voice_render(chiptune_voice_t *v)
{
    if (v->amp < v->target_amp) {
        v->amp += CHIPTUNE_AMP_SLEW;
        if (v->amp > v->target_amp) v->amp = v->target_amp;
    } else if (v->amp > v->target_amp) {
        v->amp -= CHIPTUNE_AMP_SLEW;
        if (v->amp < v->target_amp) v->amp = v->target_amp;
    }
    if (v->pattern == NULL && v->amp <= 0) {
        v->amp = 0;
        return 0;
    }

    int32_t s;
    switch (v->wave) {
    case CHIPTUNE_WAVE_TRIANGLE:
        v->phase += v->phase_inc;
        s = sample_triangle(v->phase);
        break;
    case CHIPTUNE_WAVE_SAWTOOTH:
        v->phase += v->phase_inc;
        s = sample_sawtooth(v->phase);
        break;
    case CHIPTUNE_WAVE_NOISE: {
        /* Clock the LFSR on bit-20 flips so higher notes shift the noise
         * spectrum brighter, lower notes towards a rumble. */
        uint32_t old = v->phase;
        v->phase += v->phase_inc;
        if ((old ^ v->phase) & 0x00100000U) {
            step_lfsr(v);
        }
        s = (v->lfsr & 1U) ? +1024 : -1024;
        break;
    }
    case CHIPTUNE_WAVE_PULSE_25:
        v->phase += v->phase_inc;
        s = sample_pulse(v->phase, 0x40000000U);
        break;
    case CHIPTUNE_WAVE_PULSE_12:
        v->phase += v->phase_inc;
        s = sample_pulse(v->phase, 0x20000000U);
        break;
    case CHIPTUNE_WAVE_SQUARE:
    default:
        v->phase += v->phase_inc;
        s = sample_pulse(v->phase, 0x80000000U);
        break;
    }

    return (s * v->amp) >> 10;
}

static void voices_tick_time(void)
{
    for (int i = 0; i < CHIPTUNE_CHANNEL_COUNT; ++i) {
        chiptune_voice_t *v = &s_state.voices[i];
        if (v->pattern == NULL) {
            continue;
        }
        if (v->remaining_samples > 0) {
            v->remaining_samples--;
        }
        if (v->remaining_samples == 0) {
            voice_advance_event(v);
        }
    }
}

static void render_mono(int32_t *mono, size_t frames)
{
    if (s_state.paused) {
        memset(mono, 0, frames * sizeof(int32_t));
        return;
    }
    for (size_t f = 0; f < frames; ++f) {
        int32_t sum = 0;
        for (int i = 0; i < CHIPTUNE_CHANNEL_COUNT; ++i) {
            sum += voice_render(&s_state.voices[i]);
        }
        mono[f] = sum;
        voices_tick_time();
    }
}

/* ---- Output formatter ------------------------------------------------- */

static inline int16_t clip_i16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static size_t format_output(const int32_t *mono, size_t frames, uint8_t *out)
{
    const uint8_t ch   = s_state.out_channels;
    const uint8_t bits = s_state.out_bits;

    if (bits == 16) {
        int16_t *dst = (int16_t *)out;
        for (size_t f = 0; f < frames; ++f) {
            int16_t s = clip_i16(mono[f]);
            *dst++ = s;
            if (ch == 2) *dst++ = s;
        }
        return frames * ch * sizeof(int16_t);
    }
    if (bits == 32) {
        int32_t *dst = (int32_t *)out;
        for (size_t f = 0; f < frames; ++f) {
            /* <<16 so loudness is invariant across mixer bit depths. */
            int32_t s = (int32_t)clip_i16(mono[f]) << 16;
            *dst++ = s;
            if (ch == 2) *dst++ = s;
        }
        return frames * ch * sizeof(int32_t);
    }
    return 0;
}

/* ---- Producer task ---------------------------------------------------- */

/* audio_mixer_track_write() blocks when the mixer ring fills, so this loop
 * naturally paces to real-time output rate. */
static void chiptune_producer_task(void *arg)
{
    (void)arg;
    static int32_t mono[CHIPTUNE_CHUNK_FRAMES];
    static uint8_t out[CHIPTUNE_MAX_CHUNK_BYTES];

    while (s_state.running) {
        chiptune_lock();
        render_mono(mono, CHIPTUNE_CHUNK_FRAMES);
        size_t bytes = format_output(mono, CHIPTUNE_CHUNK_FRAMES, out);
        chiptune_unlock();

        if (bytes == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        (void)audio_mixer_track_write(s_state.track, out, bytes);
    }
    s_state.task = NULL;
    vTaskDelete(NULL);
}

static uint32_t samples_per_tick_from_song(const chiptune_song_t *song)
{
    if (!song || song->tempo_bpm == 0 || song->ppq == 0) {
        return s_state.sample_rate / 8;    /* 125 ms fallback */
    }
    /* samples/tick = sample_rate * 60 / (bpm * ppq); u64 to avoid overflow. */
    uint64_t num = (uint64_t)60u * s_state.sample_rate;
    uint64_t den = (uint64_t)song->tempo_bpm * song->ppq;
    return (uint32_t)(num / den);
}

/* ---- Public API ------------------------------------------------------- */

esp_err_t chiptune_engine_start(void)
{
    if (s_state.running) {
        return ESP_OK;
    }

    if (!s_initialised) {
        memset(&s_state, 0, sizeof(s_state));
        s_state.lock = xSemaphoreCreateMutex();
        if (!s_state.lock) {
            return ESP_ERR_NO_MEM;
        }
        s_state.sfx_bank[CHIPTUNE_SFX_LASER]      = &sfx_laser;
        s_state.sfx_bank[CHIPTUNE_SFX_HIT]        = &sfx_hit;
        s_state.sfx_bank[CHIPTUNE_SFX_EXPLOSION]  = &sfx_explosion;
        s_state.sfx_bank[CHIPTUNE_SFX_POWERUP]    = &sfx_powerup;
        s_state.sfx_bank[CHIPTUNE_SFX_ENEMY_SHOT] = &sfx_enemy_shot;
        s_initialised = true;
    }

    audio_mixer_handle_t mixer = NULL;
    esp_err_t err = audio_hub_get_mixer(&mixer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_hub_get_mixer failed: %s", esp_err_to_name(err));
        return err;
    }
    err = audio_mixer_open_track(mixer, AUDIO_MIXER_TRACK_SYSTEM,
                                 "chiptune/air_battle", &s_state.track);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_mixer_open_track failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t rate = 0;
    uint8_t  ch   = 0;
    uint8_t  bits = 0;
    err = audio_mixer_track_info(s_state.track, &rate, &ch, &bits);
    if (err != ESP_OK) {
        audio_mixer_close_track(s_state.track);
        s_state.track = NULL;
        return err;
    }
    if (rate == 0 || (bits != 16 && bits != 32) || (ch != 1 && ch != 2)) {
        ESP_LOGE(TAG, "unsupported mixer format: %u Hz, %u ch, %u bits",
                 (unsigned)rate, (unsigned)ch, (unsigned)bits);
        audio_mixer_close_track(s_state.track);
        s_state.track = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_state.sample_rate  = rate;
    s_state.out_channels = ch;
    s_state.out_bits     = bits;
    s_state.sfx_samples_per_tick = (rate * CHIPTUNE_SFX_TICK_MS) / 1000u;
    build_midi_lut(rate);

    for (int i = 0; i < CHIPTUNE_CHANNEL_COUNT; ++i) {
        s_state.voices[i].lfsr = 0x7FFFU;
    }
    s_state.paused  = false;
    s_state.running = true;

    BaseType_t ok = xTaskCreate(chiptune_producer_task, "chiptune",
                                CHIPTUNE_TASK_STACK, NULL,
                                CHIPTUNE_TASK_PRIORITY, &s_state.task);
    if (ok != pdPASS) {
        s_state.running = false;
        audio_mixer_close_track(s_state.track);
        s_state.track = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "chiptune engine up: %u Hz, %u ch, %u bits",
             (unsigned)rate, (unsigned)ch, (unsigned)bits);
    return ESP_OK;
}

void chiptune_engine_stop(void)
{
    if (!s_state.running) {
        return;
    }
    s_state.running = false;
    /* Give the producer a couple of chunk periods to unwind on its own. */
    for (int i = 0; i < 20 && s_state.task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_state.track) {
        (void)audio_mixer_track_stop(s_state.track);
        (void)audio_mixer_close_track(s_state.track);
        s_state.track = NULL;
    }
    chiptune_lock();
    for (int i = 0; i < CHIPTUNE_CHANNEL_COUNT; ++i) {
        s_state.voices[i].pattern = NULL;
        s_state.voices[i].amp = 0;
        s_state.voices[i].target_amp = 0;
    }
    chiptune_unlock();
}

void chiptune_engine_set_song(const chiptune_song_t *song)
{
    if (!s_state.running) {
        return;
    }
    chiptune_lock();
    s_state.bgm_samples_per_tick = samples_per_tick_from_song(song);
    for (int i = 0; i < CHIPTUNE_BGM_CHANNELS; ++i) {
        chiptune_voice_t *v = &s_state.voices[i];
        if (song && i < song->pattern_count) {
            voice_start_pattern(v, &song->patterns[i],
                                s_state.bgm_samples_per_tick, false);
        } else {
            v->pattern = NULL;
            v->target_amp = 0;
        }
    }
    chiptune_unlock();
}

void chiptune_engine_pause(void)
{
    s_state.paused = true;
    chiptune_lock();
    for (int i = 0; i < CHIPTUNE_BGM_CHANNELS; ++i) {
        s_state.voices[i].target_amp = 0;
    }
    chiptune_unlock();
}

void chiptune_engine_resume(void)
{
    if (!s_state.running) {
        return;
    }
    chiptune_lock();
    for (int i = 0; i < CHIPTUNE_BGM_CHANNELS; ++i) {
        chiptune_voice_t *v = &s_state.voices[i];
        if (v->pattern != NULL) {
            voice_apply_event(v, &v->pattern->events[v->event_index]);
        }
    }
    chiptune_unlock();
    s_state.paused = false;
}

void chiptune_engine_trigger_sfx(chiptune_sfx_t sfx)
{
    if (!s_state.running || sfx >= CHIPTUNE_SFX_COUNT) {
        return;
    }
    const chiptune_pattern_t *pat = s_state.sfx_bank[sfx];
    if (pat == NULL) {
        return;
    }
    chiptune_lock();
    /* Prefer an idle slot, otherwise steal via the round-robin cursor. */
    int slot = -1;
    for (int i = 0; i < CHIPTUNE_SFX_CHANNELS; ++i) {
        if (s_state.voices[CHIPTUNE_BGM_CHANNELS + i].pattern == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = s_state.sfx_cursor % CHIPTUNE_SFX_CHANNELS;
        s_state.sfx_cursor = (s_state.sfx_cursor + 1) % CHIPTUNE_SFX_CHANNELS;
    }
    voice_start_pattern(&s_state.voices[CHIPTUNE_BGM_CHANNELS + slot],
                        pat, s_state.sfx_samples_per_tick, true);
    chiptune_unlock();
}

#else /* !ESP_PLATFORM -- host / wasm simulator stubs */

esp_err_t chiptune_engine_start(void) { return ESP_OK; }
void chiptune_engine_stop(void) {}
void chiptune_engine_set_song(const chiptune_song_t *song) { (void)song; }
void chiptune_engine_pause(void) {}
void chiptune_engine_resume(void) {}
void chiptune_engine_trigger_sfx(chiptune_sfx_t sfx) { (void)sfx; }

#endif /* ESP_PLATFORM */
