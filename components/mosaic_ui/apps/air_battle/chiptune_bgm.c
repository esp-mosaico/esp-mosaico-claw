/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Air Battle BGM: 4-bar loop in A minor at 140 BPM.  Each voice line spans
 * exactly 64 sixteenth-note ticks (ppq=4) so all four voices realign at the
 * loop point.
 *
 * MIDI reference:
 *   E2=40  F2=41  G2=43  A2=45
 *   E5=76  F5=77  G5=79  A5=81  B5=83
 *   C6=84  D6=86  E6=88
 */

#include "chiptune_bgm.h"

#define REST      { 0, 2, 0, CHIPTUNE_WAVE_SQUARE }
#define REST_D(d) { 0, (d), 0, CHIPTUNE_WAVE_SQUARE }

/* Voice 0 -- lead melody (square) */
static const chiptune_event_t bgm_melody[] = {
    /* Bar 1: A5 A5 C6 E6 E6- D6 C6 */
    { 81, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 81, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 84, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 88, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 88, 4, 235, CHIPTUNE_WAVE_SQUARE },
    { 86, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 84, 2, 220, CHIPTUNE_WAVE_SQUARE },
    /* Bar 2: B5 A5 G5 A5 A5- . E5 */
    { 83, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 81, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 79, 2, 200, CHIPTUNE_WAVE_SQUARE },
    { 81, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 81, 4, 235, CHIPTUNE_WAVE_SQUARE },
    REST_D(2),
    { 76, 2, 200, CHIPTUNE_WAVE_SQUARE },
    /* Bar 3: F5 G5 A5 C6 E6- D6- */
    { 77, 2, 210, CHIPTUNE_WAVE_SQUARE },
    { 79, 2, 210, CHIPTUNE_WAVE_SQUARE },
    { 81, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 84, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 88, 4, 240, CHIPTUNE_WAVE_SQUARE },
    { 86, 4, 230, CHIPTUNE_WAVE_SQUARE },
    /* Bar 4: C6 B5 A5 G5 A5== */
    { 84, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 83, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 81, 2, 220, CHIPTUNE_WAVE_SQUARE },
    { 79, 2, 200, CHIPTUNE_WAVE_SQUARE },
    { 81, 8, 240, CHIPTUNE_WAVE_SQUARE },
};
static const chiptune_pattern_t pattern_melody = {
    .events      = bgm_melody,
    .event_count = sizeof(bgm_melody) / sizeof(bgm_melody[0]),
    .loop_from   = 0,
};

/* Voice 1 -- counter melody on the offbeats (25% pulse) */
static const chiptune_event_t bgm_counter[] = {
    /* Bar 1: . E5 . A5 . E5 . A5 */
    REST, { 76, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 81, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 76, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 81, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    /* Bar 2: . E5 . A5 . E5 . B5 */
    REST, { 76, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 81, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 76, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 83, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    /* Bar 3: . F5 . A5 . G5 . B5 */
    REST, { 77, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 81, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 79, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 83, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    /* Bar 4: . E5 . A5 rest(4) E5(4) */
    REST, { 76, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST, { 81, 2, 140, CHIPTUNE_WAVE_PULSE_25 },
    REST_D(4),
    { 76, 4, 150, CHIPTUNE_WAVE_PULSE_25 },
};
static const chiptune_pattern_t pattern_counter = {
    .events      = bgm_counter,
    .event_count = sizeof(bgm_counter) / sizeof(bgm_counter[0]),
    .loop_from   = 0,
};

/* Voice 2 -- bass line (triangle) */
static const chiptune_event_t bgm_bass[] = {
    { 45, 16, 200, CHIPTUNE_WAVE_TRIANGLE },        /* Bar 1: A2 */
    { 40, 16, 200, CHIPTUNE_WAVE_TRIANGLE },        /* Bar 2: E2 */
    { 41,  8, 200, CHIPTUNE_WAVE_TRIANGLE },        /* Bar 3: F2 */
    { 43,  8, 200, CHIPTUNE_WAVE_TRIANGLE },        /*        G2 */
    { 45,  8, 210, CHIPTUNE_WAVE_TRIANGLE },        /* Bar 4: A2 */
    { 40,  8, 210, CHIPTUNE_WAVE_TRIANGLE },        /*        E2 */
};
static const chiptune_pattern_t pattern_bass = {
    .events      = bgm_bass,
    .event_count = sizeof(bgm_bass) / sizeof(bgm_bass[0]),
    .loop_from   = 0,
};

/* Voice 3 -- drums (noise).  Kick uses a very low "note" so the LFSR clocks
 * slowly and lands as a thump; snare uses a mid note for a hissier attack.
 * Each hit lasts one tick followed by three rests -- the amp slew turns the
 * short duration into a percussive envelope. */
#define KICK   { 30, 1, 220, CHIPTUNE_WAVE_NOISE }
#define SNARE  { 60, 1, 200, CHIPTUNE_WAVE_NOISE }
#define REST3  { 0, 3, 0,   CHIPTUNE_WAVE_NOISE }

static const chiptune_event_t bgm_drums[] = {
    KICK, REST3, SNARE, REST3, KICK, REST3, SNARE, REST3,
    KICK, REST3, SNARE, REST3, KICK, REST3, SNARE, REST3,
    KICK, REST3, SNARE, REST3, KICK, REST3, SNARE, REST3,
    KICK, REST3, SNARE, REST3, KICK, REST3, SNARE, REST3,
};
static const chiptune_pattern_t pattern_drums = {
    .events      = bgm_drums,
    .event_count = sizeof(bgm_drums) / sizeof(bgm_drums[0]),
    .loop_from   = 0,
};

static const chiptune_pattern_t air_battle_patterns[] = {
    pattern_melody,
    pattern_counter,
    pattern_bass,
    pattern_drums,
};

const chiptune_song_t chiptune_bgm_air_battle = {
    .patterns      = air_battle_patterns,
    .pattern_count = sizeof(air_battle_patterns) / sizeof(air_battle_patterns[0]),
    .tempo_bpm     = 140,
    .ppq           = 4,
};
