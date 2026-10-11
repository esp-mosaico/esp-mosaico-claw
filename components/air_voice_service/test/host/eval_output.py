#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Compare speech vs. non-speech levels before and after the DSP chain.

    python3 eval_output.py input.wav output.wav labels.json

The chain has 32 ms of latency; the output is re-aligned before measuring.
"Contrast" is the level difference between labelled speech regions and the
remaining (noise-only) regions, a simple stand-in for SNR that is insensitive
to the AGC gain.
"""
import json
import sys

import numpy as np
from scipy.io import wavfile

LATENCY = 512


def level_db(x):
    return 10 * np.log10(np.mean(x.astype(np.float64) ** 2) + 1e-20) - 20 * np.log10(32768)


def main():
    fs, x = wavfile.read(sys.argv[1])
    _, y = wavfile.read(sys.argv[2])
    labels = json.loads(open(sys.argv[3]).read())
    y = np.concatenate([y[LATENCY:], np.zeros(LATENCY, y.dtype)])
    mask = np.zeros(len(x), bool)
    for a, b in labels["segments"]:
        mask[int(a * fs):int(b * fs)] = True
    guard = np.convolve(mask, np.ones(int(0.3 * fs)), "same") > 0  # exclude edges
    quiet = ~guard
    quiet[: int(1.5 * fs)] = False  # tracker warm-up
    for name, sig in (("input ", x), ("output", y)):
        sp, nz = level_db(sig[mask]), level_db(sig[quiet])
        clip = np.mean(np.abs(sig) >= 32700) * 100
        print(f"{name}: speech {sp:6.1f} dBFS  noise {nz:6.1f} dBFS  contrast {sp - nz:5.1f} dB  clip {clip:.2f}%")


if __name__ == "__main__":
    main()
