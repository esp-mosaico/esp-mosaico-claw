/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* See air_voice_dsp.h for the signal flow. References:
 *  - I. Cohen, B. Berdugo, "Noise estimation by minima controlled recursive
 *    averaging for robust speech enhancement", IEEE SPL 9(1), 2002 (MCRA).
 *  - Y. Ephraim, D. Malah, "Speech enhancement using a MMSE short-time
 *    spectral amplitude estimator", IEEE TASSP 32(6), 1984 (decision-directed
 *    a priori SNR).
 *  - O. Cappe, "Elimination of the musical noise phenomenon with the Ephraim
 *    and Malah noise suppressor", IEEE TSAP 2(2), 1994 (a priori SNR floor).
 */
#include "air_voice_dsp.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N_FFT        AIR_VOICE_DSP_FFT_SIZE
#define HOP          AIR_VOICE_DSP_HOP
#define N_BINS       (N_FFT / 2U + 1U)
#define FS           ((float)AIR_VOICE_DSP_SAMPLE_RATE)
#define LOG2_N_FFT   9U

#define PI_F         3.14159265358979f
#define EPS_POWER    1e-12f
#define DB_FLOOR     (-120.0f)
#define INT16_SCALE  32768.0f
#define CLIP_LEVEL   32700

/* MCRA (Cohen & Berdugo 2002) tuned for a 16 ms hop. */
#ifndef MCRA_ALPHA_S
#define MCRA_ALPHA_S       0.8f    /* periodogram time smoothing */
#endif
#ifndef MCRA_ALPHA_D
#define MCRA_ALPHA_D       0.95f   /* noise update in noise-only bins */
#endif
#ifndef MCRA_ALPHA_P
#define MCRA_ALPHA_P       0.2f    /* speech presence smoothing */
#endif
#ifndef MCRA_DELTA
#define MCRA_DELTA         5.0f    /* S/Smin ratio declaring speech */
#endif
#define MCRA_MIN_WINDOW    62U     /* ~1 s minimum search window */
#define DD_ALPHA           0.98f   /* decision-directed weight */
#define NOISE_WARMUP_FRAMES 20U    /* ~320 ms running-mean initialisation */
#ifndef NOISE_LONG_WINDOWS
#define NOISE_LONG_WINDOWS 4U      /* cap uses the minimum over ~4-5 s */
#endif
#ifndef NOISE_MIN_BIAS
#define NOISE_MIN_BIAS     5.0f    /* noise <= bias * long-term minimum (unbiased for stationary noise) */
#endif
#ifndef NOISE_FAST_ALPHA
#define NOISE_FAST_ALPHA   0.5f    /* smoothing behind the long minimum */
#endif
/* Per-bin power of white noise at -110 dBFS: keeps digital silence (e.g. the
 * zero padding at stream start) from pinning the minimum trackers at zero. */
#define BIN_POWER_FLOOR    2.56e-9f

/* Breath airflow model, measured on close-talk (3 cm) whisper: airflow is
 * strongest below 250 Hz (+27 dB over clean whisper), falls ~5-7 dB/octave
 * and is gone above ~1.5 kHz, while whispered speech (no F0, F1 >= 300 Hz)
 * carries almost nothing below 250 Hz. Airflow is read in 125-250 Hz
 * relative to the 1.5-4 kHz whisper band; the excess is extrapolated with
 * the fitted slope and spectrally subtracted, tapering out before F2.
 * Voiced frames are left alone: their F0 harmonics also sit in that band. */
#define AIR_LO_K0          4U      /* 125 Hz */
#define AIR_LO_K1          8U      /* 250 Hz */
#define AIR_HI_K0          48U     /* 1.5 kHz */
#define AIR_HI_K1          128U    /* 4 kHz */
#define AIR_REF_HZ         180.0f
#ifndef AIR_SLOPE
#define AIR_SLOPE          1.8f    /* power ~ f^-1.8 (-5.4 dB/octave), fit on 3-15 cm takes */
#endif
#define AIR_TAPER_LO_HZ    1000.0f
#define AIR_TAPER_HI_HZ    1500.0f
#define AIR_SHAPE_MAX      30.0f   /* cap below the reference frequency */
#ifndef AIR_WHISPER_RATIO
#define AIR_WHISPER_RATIO  0.5f    /* low/high per-bin power of whisper alone */
#endif
#define AIR_DETECT_RATIO   4.0f    /* low > 6 dB above high: airflow frame */
#ifndef AIR_OVERSUB
#define AIR_OVERSUB        2.5f
#endif
#define AIR_RELEASE        0.5f    /* per hop: estimate decays over ~50 ms */

/* Frame VAD over the speech band, with a hangover to bridge short gaps. The
 * band starts above the airflow region so breath alone is not speech. */
#define VAD_BIN_LO         19U     /* ~600 Hz */
#define VAD_BIN_HI         160U    /* 5 kHz */
#define VAD_HANGOVER       12U     /* ~190 ms */
#define SPEECH_LEVEL_ALPHA 0.9f

/* AGC time constants (seconds) and gain limits. */
#define AGC_ATTACK_S       0.05f
#ifndef AGC_RELEASE_S
#define AGC_RELEASE_S      2.0f    /* phrase-level: avoid lifting pauses */
#endif
#define AGC_MIN_GAIN_DB    (-12.0f)
#define AGC_INITIAL_DB     12.0f
#define LIMITER_RELEASE_S  0.08f
#ifndef GATE_HANGOVER
#define GATE_HANGOVER      4U      /* ~64 ms: gate follows pauses between words */
#endif
#ifndef GATE_OPEN_COEF
#define GATE_OPEN_COEF     0.5f    /* per hop, ~25 ms */
#endif
#ifndef GATE_CLOSE_COEF
#define GATE_CLOSE_COEF    0.25f   /* per hop, ~60 ms */
#endif

/* Presence EQ: flat top 2.5-4.5 kHz, raised-cosine skirts (log-frequency)
 * down to 1.5 kHz and up to 6.5 kHz. */
#define EQ_LO_EDGE_HZ      1500.0f
#define EQ_LO_TOP_HZ       2500.0f
#define EQ_HI_TOP_HZ       4500.0f
#define EQ_HI_EDGE_HZ      6500.0f

/* Voicing analysis: 900 Hz low-pass, decimate 16 kHz -> 4 kHz, normalized
 * autocorrelation over 32 ms for pitch lags of 70-400 Hz. */
#define VOICING_LP_HZ      900.0f
#define VOICING_DECIM      4U
#define VOICING_WINDOW     128U
#define VOICING_LAG_MIN    10U
#define VOICING_LAG_MAX    57U
#define VOICING_BUF        (VOICING_WINDOW + VOICING_LAG_MAX + 8U)
#define VOICING_STAGE      (HOP / VOICING_DECIM)
#define VOICED_THRESHOLD   0.60f
#define WHISPER_THRESHOLD  0.45f

#define METER_WINDOW       1600U   /* 100 ms */

/* 4th order Butterworth high-pass as two biquads. */
#define HPF_Q1 0.54119610f
#define HPF_Q2 1.30656296f

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

struct air_voice_dsp_t {
    air_voice_dsp_config_t cfg;
    air_voice_dsp_free_fn free_fn;

    /* Derived parameters. */
    bool hpf_on;
    biquad_t hpf[2];
    biquad_t voicing_lp;
    float gain_floor;
    float xi_min;
    float limiter_ceiling;
    float manual_gain;
    float agc_attack;
    float agc_release;
    float limiter_release;

    /* FFT tables and work buffers. */
    float *window;
    float *cos_tab;
    float *sin_tab;
    uint16_t *bitrev;
    float *re;
    float *im;
    float *analysis;    /* last N_FFT conditioned input samples */
    float *ola;         /* synthesis overlap tail (HOP) */
    float *hop_in;
    float *hop_out;
    size_t pos;

    /* Per-bin state. */
    float *power;
    float *smooth;
    float *smin;
    float *stmp;
    float *presence;
    float *noise;
    float *prev_clean;
    float *gain;
    float *eq_curve;
    float *air_shape;   /* airflow spectrum relative to AIR_REF_HZ */
    float *fast;        /* lightly smoothed power for the long minimum */
    float *fast_min;    /* running minimum of `fast` in this window */
    float *min_hist;    /* NOISE_LONG_WINDOWS past window minima of `fast` */
    uint32_t min_slot;
    uint32_t frames;
    uint32_t min_count;

    /* VAD / AGC / limiter. */
    uint32_t hangover;
    uint32_t gate_hangover;
    bool vad;
    bool airflow_now;
    float air_ref;
    float air_floor;
    float air_floor_sq;
    float agc_gain_db;
    float gate_floor;
    float gate_gain;
    float level_gain_db;   /* AGC or manual gain, excluding the gate */
    float applied_gain;
    float limiter_gain;
    float speech_level_db;
    float noise_db;

    /* Voicing. Decimated samples are staged per hop and appended to low_buf
     * (newest last) once per hop instead of shifting on every sample. */
    float *low_buf;
    float low_stage[VOICING_STAGE];
    size_t low_stage_count;
    size_t low_count;
    unsigned decim_phase;
    float voicing;

    /* Meters. */
    double in_acc;
    double out_acc;
    float in_peak;
    float out_peak;
    uint32_t meter_count;
    air_voice_dsp_metrics_t metrics;
};

static float db_to_lin(float db)
{
    return powf(10.0f, db / 20.0f);
}

static float power_to_db(double power)
{
    if (power <= 1e-12) {
        return DB_FLOOR;
    }
    float db = 10.0f * log10f((float)power);
    return db < DB_FLOOR ? DB_FLOOR : db;
}

static float amp_to_db(float amp)
{
    if (amp <= 1e-6f) {
        return DB_FLOOR;
    }
    float db = 20.0f * log10f(amp);
    return db < DB_FLOOR ? DB_FLOOR : db;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void biquad_design(biquad_t *bq, bool highpass, float f0, float q)
{
    const float w0 = 2.0f * PI_F * f0 / FS;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * q);
    const float a0 = 1.0f + alpha;
    if (highpass) {
        bq->b0 = (1.0f + cw) * 0.5f / a0;
        bq->b1 = -(1.0f + cw) / a0;
    } else {
        bq->b0 = (1.0f - cw) * 0.5f / a0;
        bq->b1 = (1.0f - cw) / a0;
    }
    bq->b2 = bq->b0;
    bq->a1 = -2.0f * cw / a0;
    bq->a2 = (1.0f - alpha) / a0;
}

static inline float biquad_run(biquad_t *bq, float x)
{
    const float y = bq->b0 * x + bq->z1;
    bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2 = bq->b2 * x - bq->a2 * y;
    return y;
}

/* In-place iterative radix-2 complex FFT. inverse=true computes the
 * unnormalized inverse transform. */
static void fft(air_voice_dsp_t *dsp, bool inverse)
{
    float *re = dsp->re;
    float *im = dsp->im;
    for (uint32_t i = 0; i < N_FFT; ++i) {
        const uint32_t j = dsp->bitrev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    const float sign = inverse ? 1.0f : -1.0f;
    for (uint32_t size = 2; size <= N_FFT; size <<= 1) {
        const uint32_t half = size >> 1;
        const uint32_t step = N_FFT / size;
        for (uint32_t start = 0; start < N_FFT; start += size) {
            for (uint32_t k = 0; k < half; ++k) {
                const float wr = dsp->cos_tab[k * step];
                const float wi = sign * dsp->sin_tab[k * step];
                const uint32_t a = start + k;
                const uint32_t b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

static float eq_bell(float hz)
{
    if (hz <= EQ_LO_EDGE_HZ || hz >= EQ_HI_EDGE_HZ) {
        return 0.0f;
    }
    if (hz >= EQ_LO_TOP_HZ && hz <= EQ_HI_TOP_HZ) {
        return 1.0f;
    }
    float t;
    if (hz < EQ_LO_TOP_HZ) {
        t = logf(hz / EQ_LO_EDGE_HZ) / logf(EQ_LO_TOP_HZ / EQ_LO_EDGE_HZ);
    } else {
        t = logf(EQ_HI_EDGE_HZ / hz) / logf(EQ_HI_EDGE_HZ / EQ_HI_TOP_HZ);
    }
    return 0.5f - 0.5f * cosf(PI_F * t);
}

static void apply_config(air_voice_dsp_t *dsp, const air_voice_dsp_config_t *config)
{
    dsp->cfg = *config;
    air_voice_dsp_config_t *c = &dsp->cfg;
    c->hpf_hz = c->hpf_hz <= 0.0f ? 0.0f : clampf(c->hpf_hz, 20.0f, 1000.0f);
    c->ns_depth_db = clampf(c->ns_depth_db, 0.0f, 40.0f);
    c->eq_presence_db = clampf(c->eq_presence_db, -12.0f, 12.0f);
    c->agc_target_dbfs = clampf(c->agc_target_dbfs, -40.0f, -3.0f);
    c->agc_max_gain_db = clampf(c->agc_max_gain_db, 0.0f, 48.0f);
    c->manual_gain_db = clampf(c->manual_gain_db, -24.0f, 48.0f);
    c->limiter_ceiling_dbfs = clampf(c->limiter_ceiling_dbfs, -20.0f, 0.0f);
    c->vad_threshold_db = clampf(c->vad_threshold_db, 0.5f, 20.0f);
    c->gate_db = clampf(c->gate_db, 0.0f, 30.0f);
    c->airflow_depth_db = clampf(c->airflow_depth_db, 0.0f, 40.0f);
    dsp->air_floor = db_to_lin(-c->airflow_depth_db);
    dsp->air_floor_sq = dsp->air_floor * dsp->air_floor;
    dsp->gate_floor = db_to_lin(-c->gate_db);

    const bool was_on = dsp->hpf_on;
    dsp->hpf_on = c->hpf_hz > 0.0f;
    for (int i = 0; i < 2; ++i) {
        const float z1 = dsp->hpf[i].z1;
        const float z2 = dsp->hpf[i].z2;
        biquad_design(&dsp->hpf[i], true, c->hpf_hz > 0.0f ? c->hpf_hz : 100.0f,
                      i == 0 ? HPF_Q1 : HPF_Q2);
        /* Keep filter memory across live retuning to avoid clicks. */
        dsp->hpf[i].z1 = was_on ? z1 : 0.0f;
        dsp->hpf[i].z2 = was_on ? z2 : 0.0f;
    }

    dsp->gain_floor = db_to_lin(-c->ns_depth_db);
    dsp->xi_min = dsp->gain_floor * dsp->gain_floor;
    dsp->limiter_ceiling = db_to_lin(c->limiter_ceiling_dbfs);
    dsp->manual_gain = db_to_lin(c->manual_gain_db);
    for (uint32_t k = 0; k < N_BINS; ++k) {
        const float hz = (float)k * FS / (float)N_FFT;
        dsp->eq_curve[k] = c->eq_enable ? db_to_lin(c->eq_presence_db * eq_bell(hz)) : 1.0f;
    }
    if (dsp->agc_gain_db > c->agc_max_gain_db) {
        dsp->agc_gain_db = c->agc_max_gain_db;
    }
}

void air_voice_dsp_default_config(air_voice_dsp_config_t *config)
{
    if (!config) {
        return;
    }
    *config = (air_voice_dsp_config_t) {
        .hpf_hz = 120.0f,
        .ns_enable = true,
        .ns_depth_db = 12.0f,
        .eq_enable = true,
        .eq_presence_db = 4.0f,
        .agc_enable = true,
        .agc_target_dbfs = -20.0f,
        .agc_max_gain_db = 30.0f,
        .manual_gain_db = 0.0f,
        .limiter_enable = true,
        .limiter_ceiling_dbfs = -1.0f,
        .vad_threshold_db = 3.0f,
        .gate_db = 6.0f,
        .airflow_enable = true,
        .airflow_depth_db = 24.0f,
    };
}

static size_t align_up(size_t v)
{
    return (v + 15U) & ~(size_t)15U;
}

air_voice_dsp_t *air_voice_dsp_create(const air_voice_dsp_config_t *config,
                                      air_voice_dsp_alloc_fn alloc_fn,
                                      air_voice_dsp_free_fn free_fn)
{
    /* Single block: struct + all arrays, carved with 16-byte alignment. */
    const size_t f_n = align_up(N_FFT * sizeof(float));
    const size_t f_half = align_up((N_FFT / 2U) * sizeof(float));
    const size_t f_hop = align_up(HOP * sizeof(float));
    const size_t f_bins = align_up(N_BINS * sizeof(float));
    const size_t total = align_up(sizeof(air_voice_dsp_t)) +
        4U * f_n +                                   /* window re im analysis */
        2U * f_half +                                /* cos sin */
        align_up(N_FFT * sizeof(uint16_t)) +         /* bitrev */
        3U * f_hop +                                 /* ola hop_in hop_out */
        10U * f_bins +                               /* per-bin state */
        (NOISE_LONG_WINDOWS + 2U) * f_bins +         /* fast, fast_min, history */
        align_up(VOICING_BUF * sizeof(float));
    uint8_t *block = alloc_fn ? alloc_fn(total) : calloc(1, total);
    if (!block) {
        return NULL;
    }
    memset(block, 0, total);
    air_voice_dsp_t *dsp = (air_voice_dsp_t *)block;
    dsp->free_fn = free_fn;
    uint8_t *cursor = block + align_up(sizeof(air_voice_dsp_t));
#define CARVE(ptr, bytes) do { (ptr) = (void *)cursor; cursor += (bytes); } while (0)
    CARVE(dsp->window, f_n);
    CARVE(dsp->re, f_n);
    CARVE(dsp->im, f_n);
    CARVE(dsp->analysis, f_n);
    CARVE(dsp->cos_tab, f_half);
    CARVE(dsp->sin_tab, f_half);
    CARVE(dsp->bitrev, align_up(N_FFT * sizeof(uint16_t)));
    CARVE(dsp->ola, f_hop);
    CARVE(dsp->hop_in, f_hop);
    CARVE(dsp->hop_out, f_hop);
    CARVE(dsp->power, f_bins);
    CARVE(dsp->smooth, f_bins);
    CARVE(dsp->smin, f_bins);
    CARVE(dsp->stmp, f_bins);
    CARVE(dsp->presence, f_bins);
    CARVE(dsp->noise, f_bins);
    CARVE(dsp->prev_clean, f_bins);
    CARVE(dsp->gain, f_bins);
    CARVE(dsp->eq_curve, f_bins);
    CARVE(dsp->air_shape, f_bins);
    CARVE(dsp->fast, f_bins);
    CARVE(dsp->fast_min, f_bins);
    CARVE(dsp->min_hist, NOISE_LONG_WINDOWS * f_bins);
    CARVE(dsp->low_buf, align_up(VOICING_BUF * sizeof(float)));
#undef CARVE

    for (uint32_t n = 0; n < N_FFT; ++n) {
        /* sqrt of a periodic Hann: analysis * synthesis sums to 1 at 50 %. */
        dsp->window[n] = sqrtf(0.5f - 0.5f * cosf(2.0f * PI_F * (float)n / (float)N_FFT));
        uint32_t r = 0;
        for (uint32_t b = 0; b < LOG2_N_FFT; ++b) {
            r |= ((n >> b) & 1U) << (LOG2_N_FFT - 1U - b);
        }
        dsp->bitrev[n] = (uint16_t)r;
    }
    for (uint32_t k = 0; k < N_FFT / 2U; ++k) {
        dsp->cos_tab[k] = cosf(2.0f * PI_F * (float)k / (float)N_FFT);
        dsp->sin_tab[k] = sinf(2.0f * PI_F * (float)k / (float)N_FFT);
    }
    for (uint32_t k = 0; k < N_BINS; ++k) {
        const float hz = (float)k * FS / (float)N_FFT;
        float shape = 0.0f;
        if (k > 0 && hz < AIR_TAPER_HI_HZ) {
            shape = powf(AIR_REF_HZ / hz, AIR_SLOPE);
            if (shape > AIR_SHAPE_MAX) shape = AIR_SHAPE_MAX;
            if (hz > AIR_TAPER_LO_HZ) {
                const float t = (hz - AIR_TAPER_LO_HZ) / (AIR_TAPER_HI_HZ - AIR_TAPER_LO_HZ);
                shape *= 0.5f + 0.5f * cosf(PI_F * t);
            }
        }
        dsp->air_shape[k] = shape;
    }
    biquad_design(&dsp->voicing_lp, false, VOICING_LP_HZ, 0.70710678f);
    dsp->agc_attack = 1.0f - expf(-(float)HOP / (FS * AGC_ATTACK_S));
    dsp->agc_release = 1.0f - expf(-(float)HOP / (FS * AGC_RELEASE_S));
    dsp->limiter_release = 1.0f - expf(-1.0f / (FS * LIMITER_RELEASE_S));

    air_voice_dsp_config_t defaults;
    if (!config) {
        air_voice_dsp_default_config(&defaults);
        config = &defaults;
    }
    apply_config(dsp, config);
    air_voice_dsp_reset(dsp);
    return dsp;
}

void air_voice_dsp_destroy(air_voice_dsp_t *dsp)
{
    if (!dsp) {
        return;
    }
    if (dsp->free_fn) {
        dsp->free_fn(dsp);
    } else {
        free(dsp);
    }
}

void air_voice_dsp_set_config(air_voice_dsp_t *dsp, const air_voice_dsp_config_t *config)
{
    if (dsp && config) {
        apply_config(dsp, config);
    }
}

void air_voice_dsp_reset_counters(air_voice_dsp_t *dsp)
{
    if (!dsp) {
        return;
    }
    dsp->metrics.samples = 0;
    dsp->metrics.clipped = 0;
    dsp->metrics.speech_frames = 0;
    dsp->metrics.whisper_frames = 0;
    dsp->metrics.voiced_frames = 0;
    dsp->metrics.airflow_frames = 0;
}

void air_voice_dsp_reset(air_voice_dsp_t *dsp)
{
    if (!dsp) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        dsp->hpf[i].z1 = dsp->hpf[i].z2 = 0.0f;
    }
    dsp->voicing_lp.z1 = dsp->voicing_lp.z2 = 0.0f;
    memset(dsp->analysis, 0, N_FFT * sizeof(float));
    memset(dsp->ola, 0, HOP * sizeof(float));
    memset(dsp->hop_in, 0, HOP * sizeof(float));
    memset(dsp->hop_out, 0, HOP * sizeof(float));
    memset(dsp->presence, 0, N_BINS * sizeof(float));
    memset(dsp->prev_clean, 0, N_BINS * sizeof(float));
    for (uint32_t i = 0; i < NOISE_LONG_WINDOWS * N_BINS; ++i) {
        dsp->min_hist[i] = FLT_MAX;
    }
    dsp->min_slot = 0;
    for (uint32_t k = 0; k < N_BINS; ++k) {
        dsp->gain[k] = 1.0f;
    }
    memset(dsp->low_buf, 0, VOICING_BUF * sizeof(float));
    dsp->low_stage_count = 0;
    dsp->pos = 0;
    dsp->frames = 0;
    dsp->min_count = 0;
    dsp->hangover = 0;
    dsp->gate_hangover = 0;
    dsp->airflow_now = false;
    dsp->air_ref = 0.0f;
    dsp->vad = false;
    dsp->agc_gain_db = AGC_INITIAL_DB < dsp->cfg.agc_max_gain_db
        ? AGC_INITIAL_DB : dsp->cfg.agc_max_gain_db;
    dsp->applied_gain = dsp->cfg.agc_enable ? db_to_lin(dsp->agc_gain_db) : dsp->manual_gain;
    dsp->level_gain_db = amp_to_db(dsp->applied_gain);
    dsp->limiter_gain = 1.0f;
    dsp->gate_gain = 1.0f;
    dsp->speech_level_db = DB_FLOOR;
    dsp->noise_db = DB_FLOOR;
    dsp->low_count = 0;
    dsp->decim_phase = 0;
    dsp->voicing = 0.0f;
    dsp->in_acc = dsp->out_acc = 0.0;
    dsp->in_peak = dsp->out_peak = 0.0f;
    dsp->meter_count = 0;
    memset(&dsp->metrics, 0, sizeof(dsp->metrics));
    dsp->metrics.in_rms_dbfs = dsp->metrics.in_peak_dbfs = DB_FLOOR;
    dsp->metrics.out_rms_dbfs = dsp->metrics.out_peak_dbfs = DB_FLOOR;
    dsp->metrics.noise_dbfs = dsp->metrics.speech_dbfs = DB_FLOOR;
}

/* Broadband time-domain power of a one-sided spectrum (sqrt-Hann frame). */
static double spectrum_power(const float *bins)
{
    double sum = (double)bins[0] + (double)bins[N_BINS - 1U];
    for (uint32_t k = 1; k < N_BINS - 1U; ++k) {
        sum += 2.0 * (double)bins[k];
    }
    return 2.0 * sum / ((double)N_FFT * (double)N_FFT);
}

static void track_noise(air_voice_dsp_t *dsp)
{
    float *P = dsp->power;
    /* Frame 0 is half zero padding, so frame 1 is the first full frame. */
    if (dsp->frames <= 1U) {
        for (uint32_t k = 0; k < N_BINS; ++k) {
            const float p = P[k] + BIN_POWER_FLOOR;
            dsp->smooth[k] = dsp->smin[k] = dsp->stmp[k] = dsp->noise[k] = p;
            dsp->fast[k] = dsp->fast_min[k] = p;
        }
        return;
    }
    /* Warm-up: assume the first ~300 ms are noise and take a running mean,
     * which converges far faster than the MCRA recursion. */
    const bool warmup = dsp->frames < NOISE_WARMUP_FRAMES;
    const float warmup_weight = 1.0f / (float)dsp->frames;
    for (uint32_t k = 0; k < N_BINS; ++k) {
        const float pk = P[k] + BIN_POWER_FLOOR;
        /* Three-tap frequency smoothing, then first-order time smoothing. */
        const float left = P[k > 0 ? k - 1U : k + 1U];
        const float right = P[k + 1U < N_BINS ? k + 1U : k - 1U];
        const float sf = 0.25f * left + 0.5f * P[k] + 0.25f * right + BIN_POWER_FLOOR;
        const float s = MCRA_ALPHA_S * dsp->smooth[k] + (1.0f - MCRA_ALPHA_S) * sf;
        dsp->smooth[k] = s;
        const float f = NOISE_FAST_ALPHA * dsp->fast[k] + (1.0f - NOISE_FAST_ALPHA) * sf;
        dsp->fast[k] = f;
        if (f < dsp->fast_min[k]) dsp->fast_min[k] = f;
        if (s < dsp->smin[k]) dsp->smin[k] = s;
        if (s < dsp->stmp[k]) dsp->stmp[k] = s;
        const float indicator = s > MCRA_DELTA * dsp->smin[k] ? 1.0f : 0.0f;
        const float p = MCRA_ALPHA_P * dsp->presence[k] + (1.0f - MCRA_ALPHA_P) * indicator;
        dsp->presence[k] = p;
        if (warmup) {
            dsp->noise[k] += warmup_weight * (pk - dsp->noise[k]);
            continue;
        }
        const float alpha_d = MCRA_ALPHA_D + (1.0f - MCRA_ALPHA_D) * p;
        dsp->noise[k] = alpha_d * dsp->noise[k] + (1.0f - alpha_d) * pk;
        /* Whisper is noise-like, so weak speech bins slip under the presence
         * test and leak into the estimate during long utterances. Bound the
         * estimate by the bias-compensated minimum over the last few seconds
         * (minimum statistics), which only follows the true floor. */
        float long_min = dsp->fast_min[k];
        for (uint32_t w = 0; w < NOISE_LONG_WINDOWS; ++w) {
            const float m = dsp->min_hist[w * N_BINS + k];
            if (m < long_min) long_min = m;
        }
        const float cap = NOISE_MIN_BIAS * long_min;
        if (dsp->noise[k] > cap) {
            dsp->noise[k] = cap;
        }
    }
    if (++dsp->min_count >= MCRA_MIN_WINDOW) {
        dsp->min_count = 0;
        float *slot = dsp->min_hist + dsp->min_slot * N_BINS;
        dsp->min_slot = (dsp->min_slot + 1U) % NOISE_LONG_WINDOWS;
        for (uint32_t k = 0; k < N_BINS; ++k) {
            slot[k] = dsp->fast_min[k];
            dsp->fast_min[k] = dsp->fast[k];
            dsp->smin[k] = dsp->stmp[k] < dsp->smooth[k] ? dsp->stmp[k] : dsp->smooth[k];
            dsp->stmp[k] = dsp->smooth[k];
        }
    }
}

/* Per-bin airflow power at AIR_REF_HZ (0 when no airflow is present). */
static float estimate_airflow(air_voice_dsp_t *dsp)
{
    dsp->airflow_now = false;
    if (!dsp->cfg.airflow_enable || dsp->frames < 2U) {
        dsp->air_ref = 0.0f;
        return 0.0f;
    }
    const float *P = dsp->power;
    if (dsp->vad && dsp->voicing >= VOICED_THRESHOLD) {
        dsp->air_ref *= AIR_RELEASE; /* periodic speech, not breath */
        return dsp->air_ref;
    }
    float lo = 0.0f;
    float hi = 0.0f;
    for (uint32_t k = AIR_LO_K0; k <= AIR_LO_K1; ++k) {
        const float excess = P[k] - dsp->noise[k];
        if (excess > 0.0f) lo += excess;
    }
    for (uint32_t k = AIR_HI_K0; k <= AIR_HI_K1; ++k) {
        hi += P[k];
    }
    lo /= (float)(AIR_LO_K1 - AIR_LO_K0 + 1U);
    hi /= (float)(AIR_HI_K1 - AIR_HI_K0 + 1U);
    dsp->airflow_now = lo > AIR_DETECT_RATIO * hi;
    /* Low-band energy beyond what whisper itself produces is airflow. */
    float target = lo - AIR_WHISPER_RATIO * hi;
    if (target < 0.0f) target = 0.0f;
    dsp->air_ref = target >= dsp->air_ref
        ? target : AIR_RELEASE * dsp->air_ref + (1.0f - AIR_RELEASE) * target;
    return dsp->air_ref;
}

static void compute_gains(air_voice_dsp_t *dsp)
{
    const float *P = dsp->power;
    const float air = estimate_airflow(dsp);
    for (uint32_t k = 0; k < N_BINS; ++k) {
        float g = 1.0f;
        if (dsp->cfg.ns_enable && dsp->frames > 0) {
            const float lambda = dsp->noise[k];
            const float gamma = P[k] / lambda;
            float xi = DD_ALPHA * dsp->prev_clean[k] / lambda +
                (1.0f - DD_ALPHA) * (gamma > 1.0f ? gamma - 1.0f : 0.0f);
            if (xi < dsp->xi_min) xi = dsp->xi_min;
            g = xi / (1.0f + xi);
            if (g < dsp->gain_floor) g = dsp->gain_floor;
        }
        dsp->prev_clean[k] = g * g * P[k];
        float ga = 1.0f;
        if (air > 0.0f && dsp->air_shape[k] > 0.0f) {
            /* Power subtraction of the modelled airflow spectrum. */
            const float keep = 1.0f - AIR_OVERSUB * air * dsp->air_shape[k] /
                (P[k] + BIN_POWER_FLOOR);
            ga = keep <= dsp->air_floor_sq ? dsp->air_floor : sqrtf(keep);
        }
        dsp->gain[k] = g * ga * dsp->eq_curve[k];
    }
}

static void update_vad(air_voice_dsp_t *dsp)
{
    double sig = 0.0;
    double noi = 0.0;
    for (uint32_t k = VAD_BIN_LO; k <= VAD_BIN_HI; ++k) {
        sig += (double)dsp->power[k];
        noi += (double)dsp->noise[k];
    }
    const double ratio = sig / (noi > 1e-20 ? noi : 1e-20);
    const float band_snr = ratio > 1e-6 ? 10.0f * log10f((float)ratio) : DB_FLOOR;
    const bool active = dsp->frames > 2U && band_snr > dsp->cfg.vad_threshold_db;
    if (active) {
        dsp->hangover = VAD_HANGOVER;
        dsp->gate_hangover = GATE_HANGOVER;
    } else {
        if (dsp->hangover > 0) dsp->hangover--;
        if (dsp->gate_hangover > 0) dsp->gate_hangover--;
    }
    dsp->vad = active || dsp->hangover > 0;
}

static void update_voicing(air_voice_dsp_t *dsp)
{
    if (dsp->low_count < VOICING_WINDOW + VOICING_LAG_MAX) {
        dsp->voicing = 0.0f;
        return;
    }
    /* low_buf holds the newest sample at the end. */
    const float *newer = dsp->low_buf + VOICING_BUF - VOICING_WINDOW;
    float e_new = 0.0f;
    for (uint32_t n = 0; n < VOICING_WINDOW; ++n) {
        e_new += newer[n] * newer[n];
    }
    float best = 0.0f;
    if (e_new > 1e-9f) {
        for (uint32_t lag = VOICING_LAG_MIN; lag <= VOICING_LAG_MAX; ++lag) {
            const float *older = newer - lag;
            float xy = 0.0f;
            float e_old = 0.0f;
            for (uint32_t n = 0; n < VOICING_WINDOW; ++n) {
                xy += newer[n] * older[n];
                e_old += older[n] * older[n];
            }
            if (e_old > 1e-9f) {
                const float r = xy / sqrtf(e_new * e_old);
                if (r > best) best = r;
            }
        }
    }
    dsp->voicing = best;
}

static void run_agc_and_limiter(air_voice_dsp_t *dsp, float *block, float frame_clean_db)
{
    float target_gain;
    if (dsp->cfg.agc_enable) {
        if (dsp->vad && frame_clean_db > DB_FLOOR + 1.0f) {
            const float desired = clampf(dsp->cfg.agc_target_dbfs - frame_clean_db,
                                         AGC_MIN_GAIN_DB, dsp->cfg.agc_max_gain_db);
            const float coef = desired < dsp->agc_gain_db ? dsp->agc_attack : dsp->agc_release;
            dsp->agc_gain_db += (desired - dsp->agc_gain_db) * coef;
        }
        target_gain = db_to_lin(dsp->agc_gain_db);
    } else {
        target_gain = dsp->manual_gain;
    }
    dsp->level_gain_db = amp_to_db(target_gain);
    /* Expander: open quickly on speech, close slowly between utterances. */
    const float gate_target = dsp->gate_hangover > 0 ? 1.0f : dsp->gate_floor;
    const float gate_coef = gate_target > dsp->gate_gain ? GATE_OPEN_COEF : GATE_CLOSE_COEF;
    dsp->gate_gain += (gate_target - dsp->gate_gain) * gate_coef;
    target_gain *= dsp->gate_gain;

    /* Ramp linearly across the hop to avoid zipper noise. */
    const float start = dsp->applied_gain;
    const float step = (target_gain - start) / (float)HOP;
    for (uint32_t n = 0; n < HOP; ++n) {
        float y = block[n] * (start + step * (float)(n + 1U));
        if (dsp->cfg.limiter_enable) {
            const float a = fabsf(y);
            const float want = a > dsp->limiter_ceiling ? dsp->limiter_ceiling / a : 1.0f;
            if (want < dsp->limiter_gain) {
                dsp->limiter_gain = want;
            } else {
                dsp->limiter_gain += (want - dsp->limiter_gain) * dsp->limiter_release;
            }
            y *= dsp->limiter_gain;
        }
        block[n] = clampf(y, -1.0f, 32767.0f / INT16_SCALE);
    }
    dsp->applied_gain = target_gain;
}

static void append_low_band(air_voice_dsp_t *dsp)
{
    const size_t n = dsp->low_stage_count;
    if (n == 0) {
        return;
    }
    memmove(dsp->low_buf, dsp->low_buf + n, (VOICING_BUF - n) * sizeof(float));
    memcpy(dsp->low_buf + VOICING_BUF - n, dsp->low_stage, n * sizeof(float));
    dsp->low_count = dsp->low_count + n > VOICING_BUF ? VOICING_BUF : dsp->low_count + n;
    dsp->low_stage_count = 0;
}

static void process_hop(air_voice_dsp_t *dsp)
{
    append_low_band(dsp);
    memmove(dsp->analysis, dsp->analysis + HOP, (N_FFT - HOP) * sizeof(float));
    memcpy(dsp->analysis + (N_FFT - HOP), dsp->hop_in, HOP * sizeof(float));
    for (uint32_t n = 0; n < N_FFT; ++n) {
        dsp->re[n] = dsp->analysis[n] * dsp->window[n];
        dsp->im[n] = 0.0f;
    }
    fft(dsp, false);
    for (uint32_t k = 0; k < N_BINS; ++k) {
        dsp->power[k] = dsp->re[k] * dsp->re[k] + dsp->im[k] * dsp->im[k];
    }

    track_noise(dsp);
    update_vad(dsp);
    compute_gains(dsp);

    double clean = 0.0;
    for (uint32_t k = 0; k < N_BINS; ++k) {
        const float g = dsp->gain[k];
        dsp->re[k] *= g;
        dsp->im[k] *= g;
        const double w = (k == 0 || k == N_BINS - 1U) ? 1.0 : 2.0;
        clean += w * (double)(g * g * dsp->power[k]);
        if (k > 0 && k < N_BINS - 1U) {
            dsp->re[N_FFT - k] *= g;
            dsp->im[N_FFT - k] *= g;
        }
    }
    fft(dsp, true);
    const float inv_n = 1.0f / (float)N_FFT;
    for (uint32_t n = 0; n < HOP; ++n) {
        dsp->hop_out[n] = dsp->ola[n] + dsp->re[n] * dsp->window[n] * inv_n;
        dsp->ola[n] = dsp->re[n + HOP] * dsp->window[n + HOP] * inv_n;
    }

    const float frame_db = power_to_db(spectrum_power(dsp->power));
    const float clean_db = power_to_db(2.0 * clean / ((double)N_FFT * (double)N_FFT));
    dsp->noise_db = power_to_db(spectrum_power(dsp->noise));
    if (dsp->vad) {
        dsp->speech_level_db = dsp->speech_level_db <= DB_FLOOR
            ? frame_db
            : SPEECH_LEVEL_ALPHA * dsp->speech_level_db + (1.0f - SPEECH_LEVEL_ALPHA) * frame_db;
        update_voicing(dsp);
        dsp->metrics.speech_frames++;
        if (dsp->airflow_now) {
            dsp->metrics.airflow_frames++;
        }
        if (dsp->voicing >= VOICED_THRESHOLD) {
            dsp->metrics.voiced_frames++;
        } else if (dsp->voicing <= WHISPER_THRESHOLD) {
            dsp->metrics.whisper_frames++;
        }
    }
    run_agc_and_limiter(dsp, dsp->hop_out, clean_db);
    dsp->frames++;
}

static void publish_meters(air_voice_dsp_t *dsp)
{
    air_voice_dsp_metrics_t *m = &dsp->metrics;
    m->in_rms_dbfs = power_to_db(dsp->in_acc / (double)dsp->meter_count);
    m->out_rms_dbfs = power_to_db(dsp->out_acc / (double)dsp->meter_count);
    m->in_peak_dbfs = amp_to_db(dsp->in_peak);
    m->out_peak_dbfs = amp_to_db(dsp->out_peak);
    m->noise_dbfs = dsp->noise_db;
    m->speech_dbfs = dsp->speech_level_db;
    m->snr_db = dsp->speech_level_db > DB_FLOOR ? dsp->speech_level_db - dsp->noise_db : 0.0f;
    m->agc_gain_db = dsp->level_gain_db;
    m->voicing = dsp->voicing;
    m->vad = dsp->vad;
    m->airflow = dsp->airflow_now;
    dsp->in_acc = dsp->out_acc = 0.0;
    dsp->in_peak = dsp->out_peak = 0.0f;
    dsp->meter_count = 0;
}

void air_voice_dsp_process(air_voice_dsp_t *dsp, const int16_t *in, int16_t *out, size_t count)
{
    if (!dsp || !in || !out) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const int16_t raw = in[i];
        if (raw >= CLIP_LEVEL || raw <= -CLIP_LEVEL) {
            dsp->metrics.clipped++;
        }
        float x = (float)raw / INT16_SCALE;
        const float ax = fabsf(x);
        dsp->in_acc += (double)(x * x);
        if (ax > dsp->in_peak) dsp->in_peak = ax;
        if (dsp->hpf_on) {
            x = biquad_run(&dsp->hpf[1], biquad_run(&dsp->hpf[0], x));
        }
        const float low = biquad_run(&dsp->voicing_lp, x);
        if (++dsp->decim_phase >= VOICING_DECIM) {
            dsp->decim_phase = 0;
            if (dsp->low_stage_count < VOICING_STAGE) {
                dsp->low_stage[dsp->low_stage_count++] = low;
            }
        }

        dsp->hop_in[dsp->pos] = x;
        const float y = dsp->hop_out[dsp->pos];
        const float ay = fabsf(y);
        dsp->out_acc += (double)(y * y);
        if (ay > dsp->out_peak) dsp->out_peak = ay;
        const float scaled = y * INT16_SCALE;
        out[i] = (int16_t)(scaled >= 32767.0f ? 32767 : (scaled <= -32768.0f ? -32768 : lrintf(scaled)));

        if (++dsp->pos >= HOP) {
            dsp->pos = 0;
            process_hop(dsp);
        }
        if (++dsp->meter_count >= METER_WINDOW) {
            publish_meters(dsp);
        }
    }
    dsp->metrics.samples += count;
}

void air_voice_dsp_get_metrics(const air_voice_dsp_t *dsp, air_voice_dsp_metrics_t *out_metrics)
{
    if (dsp && out_metrics) {
        *out_metrics = dsp->metrics;
    }
}
