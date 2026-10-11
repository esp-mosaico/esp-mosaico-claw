#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Generate labelled synthetic test signals for the air-voice DSP chain.

Speech-like "syllables" are produced by exciting three formant resonators
either with white noise (whisper: aperiodic, no F0) or with a jittered
glottal pulse train (voiced). They are mixed with pink-ish background noise
at a chosen SNR. A JSON file next to each WAV lists the speech segments so an
evaluation script can compare levels inside and outside speech.

    python3 gen_test_signals.py OUT_DIR
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy import signal

FS = 16000
RNG = np.random.default_rng(1234)


def resonator(x, freq, bw):
    r = np.exp(-np.pi * bw / FS)
    theta = 2 * np.pi * freq / FS
    a = [1.0, -2 * r * np.cos(theta), r * r]
    return signal.lfilter([1 - r], a, x)


def vocal_tract(excitation, formants):
    y = np.zeros_like(excitation)
    for freq, bw, gain in formants:
        y += gain * resonator(excitation, freq, bw)
    return y


def pink_noise(n):
    white = RNG.standard_normal(n)
    b = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
    a = [1, -2.494956002, 2.017265875, -0.522189400]
    return signal.lfilter(b, a, white)


def rms_db(x):
    return 10 * np.log10(np.mean(x ** 2) + 1e-20)


def scale_to_db(x, db):
    return x * 10 ** ((db - rms_db(x)) / 20)


VOWELS = [  # (F1, F2, F3) Hz; whisper F1 is shifted up (~+150 Hz)
    (730, 1090, 2440), (270, 2290, 3010), (530, 1840, 2480),
    (570, 840, 2410), (440, 1020, 2240), (660, 1720, 2410),
]


def syllable(n, voiced):
    f1, f2, f3 = VOWELS[RNG.integers(len(VOWELS))]
    if voiced:
        f0 = RNG.uniform(105, 145)
        exc = np.zeros(n)
        t = 0.0
        while t < n:
            exc[int(t)] = 1.0
            t += FS / (f0 * (1 + 0.01 * RNG.standard_normal()))
        exc = signal.lfilter([1], [1, -0.95], exc)  # glottal tilt
        bw = (80, 100, 140)
    else:
        # Turbulent excitation; real whispered spectra roll off below
        # ~500 Hz, so the noise source is high-passed accordingly.
        exc = signal.lfilter(*signal.butter(2, 400, "highpass", fs=FS),
                             RNG.standard_normal(n))
        f1 += 150
        bw = (200, 220, 260)
    y = vocal_tract(exc, [(f1, bw[0], 1.0), (f2, bw[1], 0.7), (f3, bw[2], 0.5)])
    # Add a fricative-like onset to half of the syllables.
    if RNG.random() < 0.5:
        k = n // 4
        fric = signal.lfilter(*signal.butter(2, [3000, 6500], btype="band", fs=FS),
                              RNG.standard_normal(k))
        y[:k] += 0.6 * np.std(y) / (np.std(fric) + 1e-9) * fric
    env = np.sin(np.linspace(0, np.pi, n)) ** 0.6
    return y * env


def make(duration_s, voiced, speech_db, noise_db, breath_pops=False):
    n = int(duration_s * FS)
    speech = np.zeros(n)
    segments = []
    t = int(1.5 * FS)  # leading silence lets the noise tracker converge
    while t < n - FS:
        phrase_end = min(n - FS, t + int(RNG.uniform(1.2, 2.2) * FS))
        start = t
        while t < phrase_end:
            ln = int(RNG.uniform(0.14, 0.28) * FS)
            speech[t:t + ln] += syllable(ln, voiced)
            t += ln + int(RNG.uniform(0.01, 0.06) * FS)
        segments.append([start / FS, t / FS])
        t += int(RNG.uniform(0.8, 1.5) * FS)
    mask = np.zeros(n, bool)
    for a, b in segments:
        mask[int(a * FS):int(b * FS)] = True
    speech = speech * 10 ** ((speech_db - rms_db(speech[mask])) / 20)
    noise = scale_to_db(pink_noise(n), noise_db)
    mix = speech + noise
    if breath_pops:
        for _ in range(4):
            p = int(RNG.uniform(1, duration_s - 1) * FS)
            pop = signal.lfilter(*signal.butter(2, 120, fs=FS), RNG.standard_normal(1600))
            pop *= np.hanning(1600) * 10 ** (-12 / 20) / (np.max(np.abs(pop)) + 1e-9)
            mix[p:p + 1600] += pop
    return mix, speech, segments


def save(path, x):
    from scipy.io import wavfile
    wavfile.write(path, FS, np.clip(np.round(x * 32768), -32768, 32767).astype(np.int16))


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    cases = {
        # Close whisper (~10 cm): speech -42 dBFS over -60 dBFS noise.
        "whisper_near": dict(voiced=False, speech_db=-42, noise_db=-60),
        # Far whisper: only ~6 dB above the noise.
        "whisper_far": dict(voiced=False, speech_db=-52, noise_db=-58),
        # Normal voice at moderate level for the voicing detector.
        "voiced": dict(voiced=True, speech_db=-30, noise_db=-60),
        # Whisper with low-frequency breath pops hitting the port.
        "whisper_pops": dict(voiced=False, speech_db=-42, noise_db=-60, breath_pops=True),
    }
    for name, kw in cases.items():
        mix, speech, segments = make(14.0, **kw)
        save(out / f"{name}.wav", mix)
        save(out / f"{name}_clean.wav", speech)
        (out / f"{name}.json").write_text(json.dumps({"segments": segments, **kw}))
        print("wrote", name, "segments", len(segments))


if __name__ == "__main__":
    main()
