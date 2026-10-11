#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Score a real close-talk whisper take and processed versions of it.

    python3 eval_takes.py TAKE_raw.wav [--aligned] [OUTPUT.wav ...]

The first line describes the raw microphone track: 32 ms frame levels after a
120 Hz high-pass (p90, p50, floor = p5, SNR = p90 - floor), the peak, and the
share of active frames whose 20-250 Hz energy exceeds the 0.5-4 kHz energy
(breath airflow hitting the mic port).

The raw track and every OUTPUT are then scored on frames labelled from the raw
track (speech: 1-4 kHz at least 12 dB over its floor; airflow: speech frames
whose 250-450 Hz level is 6 dB over 1.5-4 kHz; pause: within 3 dB of floor):
  contrast  1-4 kHz level of speech frames minus pauses, an SNR stand-in that
            is insensitive to the AGC gain
  airflow   150-600 Hz minus 1.5-4 kHz on airflow frames (lower is better)
  tilt      2-4 kHz minus 0.5-1.5 kHz on speech frames (negative = muffled)
Band levels are mean power per bin, so the tilt compares equal bandwidths.

Outputs of av_dsp_host lag the input by 512 samples and are re-aligned;
pass --aligned before tracks downloaded from the device, whose processed and
raw tracks are already sample-aligned.
"""
import sys

import numpy as np
from scipy import signal
from scipy.io import wavfile

FS = 16000
LATENCY = 512


def load(path):
    rate, x = wavfile.read(path)
    if rate != FS or x.ndim != 1:
        sys.exit(f"{path}: expected 16 kHz mono")
    return x.astype(np.float64) / 32768


def stft_power(x):
    f, _, S = signal.stft(x, fs=FS, nperseg=512, noverlap=256)
    return f, np.abs(S) ** 2


def band_db(f, P, lo, hi, reduce=np.mean):
    return 10 * np.log10(reduce(P[(f >= lo) & (f < hi)], axis=0) + 1e-16)


def describe_raw(x, f, P):
    hp = signal.butter(4, 120, "highpass", fs=FS, output="sos")
    y = signal.sosfilt(hp, x)
    n = len(y) // 512 * 512
    frames = 10 * np.log10(np.mean(y[:n].reshape(-1, 512) ** 2, axis=1) + 1e-16)
    p90, p50, floor = np.percentile(frames, [90, 50, 5])
    peak = 20 * np.log10(np.max(np.abs(x)) + 1e-16)
    total = band_db(f, P, 120, FS / 2, np.sum)
    active = total > np.percentile(total, 60)
    lf_over_mid = band_db(f, P, 20, 250, np.sum) - band_db(f, P, 500, 4000, np.sum)
    lf_share = 100 * np.mean(lf_over_mid[active] > 0)
    print(f"raw take: p90 {p90:6.1f}  p50 {p50:6.1f}  floor {floor:6.1f}  SNR {p90 - floor:5.1f} dB"
          f" | peak {peak:6.1f} dBFS | LF>mid in {lf_share:4.1f}% of active frames")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    raw = load(args[0])
    f, P_raw = stft_power(raw)
    describe_raw(raw, f, P_raw)

    whisper_band = band_db(f, P_raw, 1000, 4000)
    floor = np.percentile(whisper_band, 10)
    speech = whisper_band > floor + 12
    pause = whisper_band < floor + 3
    airflow = speech & (band_db(f, P_raw, 250, 450) - band_db(f, P_raw, 1500, 4000) > 6)
    print(f"labels: speech {speech.sum()}  airflow {airflow.sum()}"
          f" ({100 * airflow.sum() / max(1, speech.sum()):.0f}% of speech)  pause {pause.sum()}")

    aligned = False
    tracks = [(args[0], raw, True)]
    for path in args[1:]:
        if path == "--aligned":
            aligned = True
            continue
        tracks.append((path, load(path), aligned))
    for path, y, is_aligned in tracks:
        if not is_aligned:
            y = np.concatenate([y[LATENCY:], np.zeros(LATENCY)])
        _, P = stft_power(y)
        n = min(P.shape[1], P_raw.shape[1])
        P, s, a, z = P[:, :n], speech[:n], airflow[:n], pause[:n]
        wb = band_db(f, P, 1000, 4000)
        contrast = 10 * np.log10(np.mean(10 ** (wb[s] / 10))) - 10 * np.log10(np.mean(10 ** (wb[z] / 10)))
        residue = (np.median((band_db(f, P, 150, 600) - band_db(f, P, 1500, 4000))[a])
                   if a.any() else float("nan"))
        tilt = np.median((band_db(f, P, 2000, 4000) - band_db(f, P, 500, 1500))[s])
        level = 10 * np.log10(np.mean(y ** 2) + 1e-16)
        name = path.rsplit("/", 1)[-1]
        print(f"  {name:34s} contrast {contrast:5.1f} dB | airflow {residue:5.1f} dB"
              f" | tilt {tilt:5.1f} dB | rms {level:6.1f} dBFS")


if __name__ == "__main__":
    main()
