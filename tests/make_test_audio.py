#!/usr/bin/env python3
"""Synthesises vocal-like test material (numpy only) for the matcher tests.

Additive synthesis of a sung/rapped voice: glottal harmonics shaped by moving
formants, pitch drift + vibrato, phrase envelopes, and sibilant noise bursts.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "reference_match"))


def write_wav(path, x, fs):
    x = np.clip(np.asarray(x, dtype=np.float32), -1, 1)
    if x.ndim == 1:
        x = x[:, None]
    ch = x.shape[1]
    data = x.astype("<f4").tobytes()
    import struct
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 3, ch, int(fs), int(fs) * ch * 4, ch * 4, 32))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def shape_noise(n, fs, lo, hi, rng, tilt=0.0):
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / fs)
    mask = ((f >= lo) & (f <= hi)).astype(float)
    if tilt:
        mask *= (np.maximum(f, 20) / 1000.0) ** (tilt / 6.02)
    y = np.fft.irfft(spec * mask, n)
    return y / (np.std(y) + 1e-12)


def voice(seconds, fs, seed, brightness=0.0, dynamics=1.0):
    rng = np.random.default_rng(seed)
    n = int(seconds * fs)
    t = np.arange(n) / fs
    # Phrases: 1.6-3 s sung, 0.4-0.9 s gaps.
    env = np.zeros(n)
    pos = 0.3
    while pos < seconds - 0.5:
        length = rng.uniform(1.6, 3.0)
        a, b = int(pos * fs), int(min(seconds, pos + length) * fs)
        seg = np.arange(b - a) / fs
        # syllables inside the phrase
        syl = 0.55 + 0.45 * np.abs(np.sin(np.pi * seg * rng.uniform(2.5, 4.0)))
        level = rng.uniform(0.35, 1.0) ** dynamics
        fade = np.minimum(1, np.minimum(seg / 0.05, (seg[-1] - seg) / 0.08 + 1e-9))
        env[a:b] = level * syl * fade
        pos += length + rng.uniform(0.4, 0.9)
    f0 = 150 + 40 * np.sin(2 * np.pi * 0.21 * t + seed) + 25 * np.sin(2 * np.pi * 0.07 * t)
    f0 *= 1 + 0.012 * np.sin(2 * np.pi * 5.5 * t)
    phase = 2 * np.pi * np.cumsum(f0) / fs
    # Formants drift between vowels.
    vowel = 0.5 + 0.5 * np.sin(2 * np.pi * 0.9 * t + seed)
    F = [(500 + 300 * vowel, 90), (1500 - 500 * vowel + 600 * (1 - vowel), 110), (2600, 160), (3400, 250)]
    y = np.zeros(n)
    for h in range(1, 60):
        fh = h * f0
        amp = np.zeros(n)
        for fc, bw in F:
            amp += 1.0 / (1 + ((fh - fc) / bw) ** 2)
        amp = (amp + 0.02) * (h ** -0.9) * (fh / 1000.0) ** (brightness / 6.02)
        amp *= fh < fs * 0.45
        y += amp * np.sin(h * phase)
    y *= env
    # Sibilants at some syllable onsets.
    sib = shape_noise(n, fs, 4500, 11000, rng)
    gate = np.zeros(n)
    for _ in range(int(seconds * 1.2)):
        c = int(rng.uniform(0.3, seconds - 0.3) * fs)
        if env[c] > 0:
            L = int(0.09 * fs)
            gate[c:c + L] = np.hanning(L)[: len(gate[c:c + L])] * rng.uniform(0.3, 0.9)
    y += 0.5 * sib * gate * np.max(np.abs(y)) * 0.5
    return y / (np.max(np.abs(y)) + 1e-12) * 0.5


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    fs = 48000
    rng = np.random.default_rng(99)
    # "Raw" home recording: dull, very dynamic, noisy (hiss + hum + room rumble).
    raw = voice(24, fs, seed=1, brightness=-3.0, dynamics=1.8) * 0.4
    n = len(raw)
    t = np.arange(n) / fs
    noise = 10 ** (-58 / 20) * shape_noise(n, fs, 30, 20000, rng, tilt=-1.5)
    hum = 10 ** (-62 / 20) * (np.sin(2 * np.pi * 60 * t) + 0.5 * np.sin(2 * np.pi * 180 * t))
    write_wav(os.path.join(out_dir, "raw_vocal.wav"), raw + noise + hum, fs)
    # A clean source performance for the reference (different take/voice).
    write_wav(os.path.join(out_dir, "ref_dry.wav"), voice(20, fs, seed=5, brightness=0.0, dynamics=1.0) * 0.4, fs)
    # Instrumental: kick-ish thumps + bass + hats.
    beat = 0.35 * np.sin(2 * np.pi * 55 * t) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.5 * t))
    hats = shape_noise(n, fs, 6000, 16000, rng) * (np.mod(t, 0.25) < 0.03) * 0.15
    mids = shape_noise(n, fs, 300, 4000, rng) * 0.08
    write_wav(os.path.join(out_dir, "beat.wav"), np.stack([beat + hats + mids] * 2, axis=1) * 0.7, fs)
    print("wrote test audio to", out_dir)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "test_audio")
