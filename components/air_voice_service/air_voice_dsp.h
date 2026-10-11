/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* Whisper ("air voice") enhancement chain.
 *
 * Portable C99 with no ESP-IDF dependency so the exact same code can be run
 * on a host against recorded WAV files (see test/host).  Signal flow:
 *
 *   int16 in -> high-pass (2 x biquad) -> STFT (512/256, sqrt-Hann)
 *            -> MCRA noise tracking + decision-directed Wiener suppression
 *            -> breath-airflow suppression (close-talk whisper)
 *            -> presence EQ -> ISTFT -> AGC (speech-gated) -> peak limiter
 *            -> int16 out
 *
 * Side analysis: frame VAD from band SNR, and a voicing (periodicity) score
 * from a 4 kHz low-band autocorrelation used to tell whisper from voiced
 * speech.  Algorithmic latency is 2 * AIR_VOICE_DSP_HOP samples (32 ms).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIR_VOICE_DSP_SAMPLE_RATE 16000U
#define AIR_VOICE_DSP_FFT_SIZE    512U
#define AIR_VOICE_DSP_HOP         (AIR_VOICE_DSP_FFT_SIZE / 2U)

typedef struct air_voice_dsp_t air_voice_dsp_t;

typedef struct {
    float hpf_hz;            /* 0 disables the high-pass stage */
    bool ns_enable;
    float ns_depth_db;       /* maximum noise attenuation, dB (positive) */
    bool eq_enable;
    float eq_presence_db;    /* boost of the 2-5 kHz whisper region */
    bool agc_enable;
    float agc_target_dbfs;   /* speech RMS target */
    float agc_max_gain_db;
    float manual_gain_db;    /* used when the AGC is disabled */
    bool limiter_enable;
    float limiter_ceiling_dbfs;
    float vad_threshold_db;  /* band SNR above which a frame is speech */
    float gate_db;           /* extra attenuation between utterances, 0 = off */
    bool airflow_enable;     /* suppress breath airflow hitting the mic port */
    float airflow_depth_db;  /* maximum airflow attenuation, dB (positive) */
} air_voice_dsp_config_t;

typedef struct {
    float in_rms_dbfs;       /* last ~100 ms, raw input */
    float in_peak_dbfs;
    float out_rms_dbfs;      /* last ~100 ms, processed output */
    float out_peak_dbfs;
    float noise_dbfs;        /* broadband noise floor estimate */
    float speech_dbfs;       /* smoothed level of speech frames (input) */
    float snr_db;            /* speech_dbfs - noise_dbfs */
    float agc_gain_db;       /* gain currently applied after suppression */
    float voicing;           /* 0..1 periodicity of the latest speech frame */
    bool vad;
    bool airflow;            /* breath airflow detected in the latest frame */
    uint64_t samples;        /* total input samples processed */
    uint64_t clipped;        /* input samples at (or near) full scale */
    uint32_t speech_frames;
    uint32_t whisper_frames; /* speech frames with low periodicity */
    uint32_t voiced_frames;  /* speech frames with clear pitch */
    uint32_t airflow_frames; /* speech frames dominated by breath airflow */
} air_voice_dsp_metrics_t;

/** Fill a configuration with the whisper-oriented defaults. */
void air_voice_dsp_default_config(air_voice_dsp_config_t *config);

/** Allocate a processor. Returns NULL on allocation failure. Buffers are
 * obtained through @p alloc_fn when non-NULL (e.g. to place them in PSRAM),
 * otherwise through calloc(). */
typedef void *(*air_voice_dsp_alloc_fn)(size_t bytes);
typedef void (*air_voice_dsp_free_fn)(void *ptr);
air_voice_dsp_t *air_voice_dsp_create(const air_voice_dsp_config_t *config,
                                      air_voice_dsp_alloc_fn alloc_fn,
                                      air_voice_dsp_free_fn free_fn);
void air_voice_dsp_destroy(air_voice_dsp_t *dsp);

/** Apply new parameters. Adaptive state (noise estimate, AGC gain) is kept so
 * settings can be tuned live while listening. */
void air_voice_dsp_set_config(air_voice_dsp_t *dsp,
                              const air_voice_dsp_config_t *config);

/** Clear all adaptive state and statistics. */
void air_voice_dsp_reset(air_voice_dsp_t *dsp);

/** Clear only the cumulative counters (clips, frame classes). */
void air_voice_dsp_reset_counters(air_voice_dsp_t *dsp);

/** Stream @p count samples. @p out receives exactly @p count samples delayed
 * by the algorithmic latency. @p in and @p out may alias. */
void air_voice_dsp_process(air_voice_dsp_t *dsp, const int16_t *in,
                           int16_t *out, size_t count);

void air_voice_dsp_get_metrics(const air_voice_dsp_t *dsp,
                               air_voice_dsp_metrics_t *out_metrics);

#ifdef __cplusplus
}
#endif
