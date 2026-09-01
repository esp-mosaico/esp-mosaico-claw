/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lightweight 8-bit / chiptune audio engine.  Songs are stored as compact
 * event lists (typically <1 KB in .rodata); the engine renders PCM in real
 * time and feeds the shared audio mixer through a single producer task, so
 * no PCM/WAV/MP3 assets ever land in flash.
 *
 * Voice layout in the current song bank:
 *   channel 0 (BGM) - square    -> lead melody
 *   channel 1 (BGM) - 25% pulse -> counter-melody / arpeggio
 *   channel 2 (BGM) - triangle  -> bass
 *   channel 3 (BGM) - noise     -> percussion
 *   channels 4..5   - overlay   -> one-shot SFX (never steal BGM voices)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHIPTUNE_WAVE_SQUARE    = 0,   /* 50% duty pulse */
    CHIPTUNE_WAVE_TRIANGLE  = 1,   /* mellow bass */
    CHIPTUNE_WAVE_SAWTOOTH  = 2,   /* harsher lead */
    CHIPTUNE_WAVE_NOISE     = 3,   /* LFSR noise for drums / SFX */
    CHIPTUNE_WAVE_PULSE_25  = 4,   /* 25% duty pulse (thin) */
    CHIPTUNE_WAVE_PULSE_12  = 5,   /* 12.5% duty pulse (very thin) */
} chiptune_wave_t;

/* One monophonic note or rest; note == 0 encodes a rest for `duration`
 * ticks and waveform/volume are ignored. */
typedef struct {
    uint8_t note;      /* MIDI note number */
    uint8_t duration;  /* sequencer ticks */
    uint8_t volume;    /* 0..255 */
    uint8_t waveform;  /* chiptune_wave_t */
} chiptune_event_t;

/* One voice line.  loop_from == event_count disables looping (one-shot SFX). */
typedef struct {
    const chiptune_event_t *events;
    uint16_t event_count;
    uint16_t loop_from;
} chiptune_pattern_t;

#define CHIPTUNE_BGM_CHANNELS_MAX 4

/* Patterns beyond CHIPTUNE_BGM_CHANNELS_MAX are ignored. */
typedef struct {
    const chiptune_pattern_t *patterns;
    uint8_t  pattern_count;
    uint16_t tempo_bpm;
    uint8_t  ppq;              /* ticks per quarter note (>=1) */
} chiptune_song_t;

typedef enum {
    CHIPTUNE_SFX_LASER = 0,
    CHIPTUNE_SFX_HIT,
    CHIPTUNE_SFX_EXPLOSION,
    CHIPTUNE_SFX_POWERUP,
    CHIPTUNE_SFX_ENEMY_SHOT,
    CHIPTUNE_SFX_COUNT,
} chiptune_sfx_t;

/* Open a SYSTEM-role mixer track and start the producer task.  Idempotent. */
esp_err_t chiptune_engine_start(void);

/* Stop the producer, close the track and clear all voices. */
void chiptune_engine_stop(void);

/* Install the BGM; NULL silences BGM voices while leaving SFX playable. */
void chiptune_engine_set_song(const chiptune_song_t *song);

/* Freeze / resume the sequencer clock without tearing the mixer track down. */
void chiptune_engine_pause(void);
void chiptune_engine_resume(void);

/* One-shot SFX on the overlay voices; never touches BGM. */
void chiptune_engine_trigger_sfx(chiptune_sfx_t sfx);

#ifdef __cplusplus
}
#endif
