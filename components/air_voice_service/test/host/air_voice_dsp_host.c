/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* Host harness: run the on-device whisper chain over a 16 kHz mono WAV.
 *
 *   cc -O2 -std=c11 -Wall -Wextra -I../.. -o av_dsp_host \
 *      air_voice_dsp_host.c ../../air_voice_dsp.c -lm
 *   ./av_dsp_host in.wav out.wav [key=value ...]
 *
 * Keys mirror the device settings: hpf_hz ns ns_depth_db eq eq_presence_db
 * agc agc_target_dbfs agc_max_gain_db manual_gain_db limiter vad_threshold_db
 * gate_db airflow airflow_depth_db
 * pre_gain_db (simulates a different codec gain before the chain).
 * Unset keys keep air_voice_dsp_default_config(), i.e. the whisper_near preset.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_voice_dsp.h"

#define CHUNK 320U

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int16_t *read_wav(const char *path, size_t *out_count)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path); fclose(f); return NULL;
    }
    uint16_t channels = 0, bits = 0; uint32_t rate = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) { fprintf(stderr, "no data chunk\n"); fclose(f); return NULL; }
        uint32_t size = rd32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[40] = {0};
            if (size > sizeof(fmt) || fread(fmt, 1, size, f) != size) { fclose(f); return NULL; }
            channels = rd16(fmt + 2); rate = rd32(fmt + 4); bits = rd16(fmt + 14);
        } else if (!memcmp(ch, "data", 4)) {
            if (channels != 1 || bits != 16 || rate != AIR_VOICE_DSP_SAMPLE_RATE) {
                fprintf(stderr, "need 16 kHz mono 16-bit (got %u Hz %u ch %u bit)\n", rate, channels, bits);
                fclose(f); return NULL;
            }
            int16_t *pcm = malloc(size);
            size_t got = pcm ? fread(pcm, 1, size, f) : 0;
            fclose(f);
            *out_count = got / 2U;
            return pcm;
        } else {
            fseek(f, (long)(size + (size & 1U)), SEEK_CUR);
        }
    }
}

static int write_wav(const char *path, const int16_t *pcm, size_t count)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    uint8_t h[44] = {0};
    const uint32_t bytes = (uint32_t)(count * 2U);
    memcpy(h, "RIFF", 4); wr32(h + 4, 36U + bytes); memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16); wr16(h + 20, 1); wr16(h + 22, 1); wr32(h + 24, AIR_VOICE_DSP_SAMPLE_RATE);
    wr32(h + 28, AIR_VOICE_DSP_SAMPLE_RATE * 2U); wr16(h + 32, 2); wr16(h + 34, 16);
    memcpy(h + 36, "data", 4); wr32(h + 40, bytes);
    fwrite(h, 1, 44, f); fwrite(pcm, 2, count, f); fclose(f);
    return 0;
}

static void apply_kv(air_voice_dsp_config_t *c, float *pre_gain_db, const char *kv)
{
    char key[64]; float v;
    if (sscanf(kv, "%63[^=]=%f", key, &v) != 2) { fprintf(stderr, "bad arg %s\n", kv); exit(2); }
    if (!strcmp(key, "hpf_hz")) c->hpf_hz = v;
    else if (!strcmp(key, "ns")) c->ns_enable = v != 0;
    else if (!strcmp(key, "ns_depth_db")) c->ns_depth_db = v;
    else if (!strcmp(key, "eq")) c->eq_enable = v != 0;
    else if (!strcmp(key, "eq_presence_db")) c->eq_presence_db = v;
    else if (!strcmp(key, "agc")) c->agc_enable = v != 0;
    else if (!strcmp(key, "agc_target_dbfs")) c->agc_target_dbfs = v;
    else if (!strcmp(key, "agc_max_gain_db")) c->agc_max_gain_db = v;
    else if (!strcmp(key, "manual_gain_db")) c->manual_gain_db = v;
    else if (!strcmp(key, "limiter")) c->limiter_enable = v != 0;
    else if (!strcmp(key, "vad_threshold_db")) c->vad_threshold_db = v;
    else if (!strcmp(key, "gate_db")) c->gate_db = v;
    else if (!strcmp(key, "airflow")) c->airflow_enable = v != 0;
    else if (!strcmp(key, "airflow_depth_db")) c->airflow_depth_db = v;
    else if (!strcmp(key, "pre_gain_db")) *pre_gain_db = v;
    else { fprintf(stderr, "unknown key %s\n", key); exit(2); }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.wav out.wav [key=value ...]\n", argv[0]);
        return 2;
    }
    size_t count = 0;
    int16_t *pcm = read_wav(argv[1], &count);
    if (!pcm) return 1;

    air_voice_dsp_config_t cfg;
    air_voice_dsp_default_config(&cfg);
    float pre_gain_db = 0.0f;
    for (int i = 3; i < argc; ++i) apply_kv(&cfg, &pre_gain_db, argv[i]);
    if (pre_gain_db != 0.0f) {
        const float g = powf(10.0f, pre_gain_db / 20.0f);
        for (size_t i = 0; i < count; ++i) {
            float s = pcm[i] * g;
            pcm[i] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
        }
    }

    air_voice_dsp_t *dsp = air_voice_dsp_create(&cfg, NULL, NULL);
    if (!dsp) { fprintf(stderr, "alloc failed\n"); return 1; }
    int16_t *out = malloc(count * sizeof(int16_t));
    printf("  time   in_rms in_pk  out_rms out_pk  noise  speech  snr   agc   voic vad\n");
    uint32_t next_print = 0;
    for (size_t off = 0; off < count; off += CHUNK) {
        const size_t n = count - off < CHUNK ? count - off : CHUNK;
        air_voice_dsp_process(dsp, pcm + off, out + off, n);
        if (off >= next_print) {
            next_print += AIR_VOICE_DSP_SAMPLE_RATE / 2U;
            air_voice_dsp_metrics_t m;
            air_voice_dsp_get_metrics(dsp, &m);
            printf("%6.2fs %6.1f %6.1f %7.1f %6.1f %6.1f %7.1f %5.1f %5.1f  %4.2f  %d\n",
                   (double)off / AIR_VOICE_DSP_SAMPLE_RATE, (double)m.in_rms_dbfs, (double)m.in_peak_dbfs,
                   (double)m.out_rms_dbfs, (double)m.out_peak_dbfs, (double)m.noise_dbfs,
                   (double)m.speech_dbfs, (double)m.snr_db,
                   (double)m.agc_gain_db, (double)m.voicing, m.vad);
        }
    }
    air_voice_dsp_metrics_t m;
    air_voice_dsp_get_metrics(dsp, &m);
    printf("summary: samples=%llu clipped=%llu speech_frames=%u whisper=%u voiced=%u airflow=%u\n",
           (unsigned long long)m.samples, (unsigned long long)m.clipped,
           m.speech_frames, m.whisper_frames, m.voiced_frames, m.airflow_frames);
    write_wav(argv[2], out, count);
    air_voice_dsp_destroy(dsp);
    free(out);
    free(pcm);
    return 0;
}
