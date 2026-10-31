#!/usr/bin/env python3
"""
Studio Engine - reference matcher.

Analyses mastered reference acapellas, then works out Studio Engine settings
that make your raw vocal sound as close to them as possible: tonal balance
(Match EQ), dynamics (compressor), sibilance (de-esser), noise clean-up,
loudness/peak (limiter) and, when instrumentals are supplied, how loud the
vocal sits against the beat (Mix Fit).

When the command-line renderer (studio_engine_cli) is available the matcher
works in a closed loop: it renders your vocal through the real DSP, measures
the result against the reference and refines the settings until they converge.

Commands
--------
  analyze   Build a reference target from one or more acapellas.
            studio_match.py analyze ref1.wav ref2.wav -o refs/artist.json
            [--beats ref1_beat.wav ref2_beat.wav]   (vocal-vs-beat level)

  match     Produce a Studio Engine profile for your vocal.
            studio_match.py match --reference refs/artist.json --vocal raw.wav
                 -o profiles/my_song.json [--beat my_beat.wav] [--iterations 6]

  compare   Print how two files differ on every matched dimension.
            studio_match.py compare processed.wav reference.wav

  report    Print the analysis of a single file.

Only numpy is required. Input files: WAV (PCM 16/24/32-bit or float).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np

# Must match Source/dsp/EQ.h
MATCH_BAND_HZ = [31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]
MATCH_BAND_IDS = ["match_31", "match_62", "match_125", "match_250", "match_500",
                  "match_1k", "match_2k", "match_4k", "match_8k", "match_16k"]
MATCH_Q = 1.41
THIRD_OCT_HZ = [round(25 * 2 ** (i / 3), 2) for i in range(0, 30)]  # 25 Hz .. 20.3 kHz

PARAM_RANGES = {  # mirrors Source/dsp/Params.h (only what the matcher sets)
    "hpf_freq": (20, 400), "dn_amount": (0, 40), "dn_sensitivity": (-10, 10),
    "gate_threshold": (-90, 0), "gate_range": (0, 80),
    "deess_freq": (3000, 12000), "deess_threshold": (-60, 0), "deess_range": (0, 24),
    "comp_threshold": (-60, 0), "comp_ratio": (1, 20), "comp_attack": (0.1, 100),
    "comp_release": (10, 1000), "comp_makeup": (-12, 24),
    "mix_offset": (-12, 12), "sat_drive": (0, 24), "sat_mix": (0, 100),
    "lim_drive": (0, 24), "lim_ceiling": (-12, 0), "output_gain": (-24, 24),
    "match_amount": (0, 150),
}
for _id in MATCH_BAND_IDS:
    PARAM_RANGES[_id] = (-12, 12)


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def db(x, floor=-150.0):
    return np.maximum(10.0 * np.log10(np.maximum(x, 1e-30)), floor)


# --------------------------------------------------------------------------
# WAV I/O
# --------------------------------------------------------------------------
def read_wav(path):
    """Returns (samples[frames, channels] float64, sample_rate)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file (convert MP3/FLAC to WAV first)")
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            raw = body
            break
        pos += 8 + size + (size & 1)
    if fmt is None or raw is None:
        raise ValueError(f"{path}: missing fmt/data chunk")
    tag, ch, rate, bits = fmt
    bps = bits // 8
    n = len(raw) // (bps * ch)
    raw = raw[: n * bps * ch]
    if tag == 3 and bits == 32:
        x = np.frombuffer(raw, "<f4").astype(np.float64)
    elif tag == 3 and bits == 64:
        x = np.frombuffer(raw, "<f8").copy()
    elif tag == 1 and bits == 16:
        x = np.frombuffer(raw, "<i2") / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v / float(1 << 23)
    elif tag == 1 and bits == 32:
        x = np.frombuffer(raw, "<i4") / 2147483648.0
    else:
        raise ValueError(f"{path}: unsupported WAV encoding (format {tag}, {bits} bit)")
    return x.reshape(-1, ch), float(rate)


# --------------------------------------------------------------------------
# Filters evaluated in the frequency domain (no scipy needed)
# --------------------------------------------------------------------------
def biquad_response(b, a, freqs, fs):
    w = 2 * np.pi * np.asarray(freqs, dtype=np.float64) / fs
    z1 = np.exp(-1j * w)
    z2 = z1 * z1
    return (b[0] + b[1] * z1 + b[2] * z2) / (1.0 + a[0] * z1 + a[1] * z2)


def k_weighting_power(freqs, fs):
    """|H(f)|^2 of the BS.1770 K-weighting pair (libebur128 formulation)."""
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = math.tan(math.pi * f0 / fs)
    Vh = 10 ** (G / 20)
    Vb = Vh ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    b1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0]
    a1 = [2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = math.tan(math.pi * f0 / fs)
    a0 = 1 + K / Q + K * K
    b2 = [1.0, -2.0, 1.0]
    a2 = [2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    h = biquad_response(b1, a1, freqs, fs) * biquad_response(b2, a2, freqs, fs)
    return np.abs(h) ** 2


def peak_eq_db(freqs, f0, gain_db, q, fs):
    """RBJ peaking filter magnitude in dB (identical to Source/dsp/Biquad.h)."""
    A = 10 ** (gain_db / 40)
    w0 = 2 * math.pi * min(f0, fs * 0.49) / fs
    alpha = math.sin(w0) / (2 * q)
    cw = math.cos(w0)
    a0 = 1 + alpha / A
    b = [(1 + alpha * A) / a0, -2 * cw / a0, (1 - alpha * A) / a0]
    a = [-2 * cw / a0, (1 - alpha / A) / a0]
    return 20 * np.log10(np.abs(biquad_response(b, a, freqs, fs)) + 1e-30)


def match_eq_response(gains, freqs, fs):
    total = np.zeros(len(freqs))
    for f0, g in zip(MATCH_BAND_HZ, gains):
        if f0 < fs * 0.45 and abs(g) > 0.01:
            total += peak_eq_db(freqs, f0, g, MATCH_Q, fs)
    return total


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------
def frame_view(x, size, hop):
    n = 1 + max(0, (len(x) - size) // hop)
    if len(x) < size:
        x = np.pad(x, (0, size - len(x)))
        n = 1
    return np.lib.stride_tricks.as_strided(x, shape=(n, size), strides=(x.strides[0] * hop, x.strides[0]))


def integrated_lufs(x, fs):
    """BS.1770-4 integrated loudness with absolute and relative gating."""
    size, hop = int(0.4 * fs), int(0.1 * fs)
    freqs = np.fft.rfftfreq(size, 1 / fs)
    wk = k_weighting_power(freqs, fs)
    powers = []
    for c in range(x.shape[1]):
        frames = frame_view(np.ascontiguousarray(x[:, c]), size, hop)
        ch_pow = []
        for i in range(0, len(frames), 256):
            spec = np.fft.rfft(frames[i:i + 256], axis=1)
            p = np.abs(spec) ** 2 * wk
            # Parseval for rfft: double the non-DC/non-Nyquist bins.
            p[:, 1:-1] *= 2
            ch_pow.append(p.sum(axis=1) / (size * size))
        powers.append(np.concatenate(ch_pow))
    z = np.sum(powers, axis=0)
    l = -0.691 + 10 * np.log10(np.maximum(z, 1e-30))
    gated = z[l > -70]
    if len(gated) == 0:
        return -100.0, l
    rel = -0.691 + 10 * np.log10(gated.mean()) - 10
    gated2 = z[(l > -70) & (l > rel)]
    if len(gated2) == 0:
        return -100.0, l
    return float(-0.691 + 10 * np.log10(gated2.mean())), l


def true_peak_db(x, oversample=4):
    """Approximate true peak via FFT up-sampling of the loudest regions."""
    mono_abs = np.max(np.abs(x), axis=1)
    peak = float(mono_abs.max()) if len(mono_abs) else 0.0
    if peak <= 0:
        return -150.0
    # Up-sample windows around the 32 loudest samples.
    idx = np.argsort(mono_abs)[-32:]
    best = peak
    for i in idx:
        a, b = max(0, i - 256), min(len(x), i + 256)
        for c in range(x.shape[1]):
            seg = x[a:b, c]
            if len(seg) < 16:
                continue
            up = np.fft.irfft(np.fft.rfft(seg), len(seg) * oversample) * oversample
            # Ignore the segment edges where the circular FFT wraps around.
            inner = up[oversample * 16: -oversample * 16]
            if len(inner):
                best = max(best, float(np.max(np.abs(inner))))
    return float(20 * np.log10(best))


def band_edges(centres):
    edges = []
    for f in centres:
        edges.append((f / 2 ** (1 / 6), f * 2 ** (1 / 6)))
    return edges


def analyze_audio(x, fs, label=""):
    """Measures everything the matcher needs. x: [frames, channels]."""
    mono = x.mean(axis=1)
    out = {"file": label, "sample_rate": fs, "duration_s": round(len(mono) / fs, 2), "channels": x.shape[1]}

    lufs, _ = integrated_lufs(x, fs)
    out["lufs"] = round(lufs, 2)
    out["peak_db"] = round(float(20 * np.log10(max(np.max(np.abs(x)), 1e-12))), 2)
    out["true_peak_db"] = round(true_peak_db(x), 2)

    # 50 ms frame levels -> noise floor, voiced frames, dynamics.
    fsz = int(0.05 * fs)
    frames = frame_view(np.ascontiguousarray(mono), fsz, fsz)
    # Digital silence (gated acapellas) counts as a -100 dB floor.
    rms = np.maximum(db(np.mean(frames ** 2, axis=1)), -100.0)
    if len(rms) < 10 or np.max(rms) < -90:
        raise ValueError(f"{label}: file is silent or too short")
    noise_floor = float(np.percentile(rms, 5))
    p95 = float(np.percentile(rms, 95))
    voiced_thr = max(noise_floor + 12.0, p95 - 35.0)
    voiced = rms > voiced_thr
    v = rms[voiced]
    out["noise_floor_db"] = round(noise_floor, 2)
    out["voiced_ratio"] = round(float(voiced.mean()), 3)
    out["level_p10"] = round(float(np.percentile(v, 10)), 2)
    out["level_p50"] = round(float(np.percentile(v, 50)), 2)
    out["level_p95"] = round(float(np.percentile(v, 95)), 2)
    out["dynamic_spread_db"] = round(out["level_p95"] - out["level_p10"], 2)
    out["snr_db"] = round(out["level_p50"] - noise_floor, 2)
    voiced_rms = 10 * np.log10(np.mean(10 ** (v / 10)))
    out["crest_db"] = round(out["peak_db"] - float(voiced_rms), 2)

    # Spectral analysis on voiced vs quiet frames.
    n = 8192 if fs > 50000 else 4096
    hop = n // 2
    win = np.hanning(n)
    sframes = frame_view(np.ascontiguousarray(mono), n, hop)
    srms = db(np.mean(sframes ** 2, axis=1))
    freqs = np.fft.rfftfreq(n, 1 / fs)
    v_sel = np.where(srms > voiced_thr + 3)[0]
    q_sel = np.where((srms > -100) & (srms < noise_floor + 4))[0]
    edges = band_edges(THIRD_OCT_HZ)
    band_idx = [np.where((freqs >= lo) & (freqs < hi))[0] for lo, hi in edges]

    def mean_spectrum(sel):
        acc = np.zeros(len(freqs))
        count = 0
        for i in range(0, len(sel), 128):
            chunk = sframes[sel[i:i + 128]] * win
            acc += np.sum(np.abs(np.fft.rfft(chunk, axis=1)) ** 2, axis=0)
            count += len(chunk)
        return acc / max(1, count), count

    vspec, vcount = mean_spectrum(v_sel)
    qspec, qcount = mean_spectrum(q_sel) if len(q_sel) else (np.zeros(len(freqs)), 0)
    norm = (n * np.mean(win ** 2)) * n / 2

    def bands(spec):
        return np.array([db(spec[ix].sum() / norm) if len(ix) else -150.0 for ix in band_idx], dtype=float)

    vb = bands(vspec)
    ref_sel = [i for i, f in enumerate(THIRD_OCT_HZ) if 250 <= f <= 4000]
    ref_level = float(np.mean(vb[ref_sel]))
    out["third_octave_hz"] = THIRD_OCT_HZ
    out["spectrum_rel_db"] = [round(float(b - ref_level), 2) for b in vb]
    out["spectrum_abs_db"] = [round(float(b), 2) for b in vb]
    out["noise_spectrum_abs_db"] = [round(float(b), 2) for b in bands(qspec)] if qcount else None
    nyq_ok = [f < fs * 0.45 for f in THIRD_OCT_HZ]
    out["band_valid"] = nyq_ok

    # Tilt: slope (dB/oct) between 200 Hz and 10 kHz.
    sel = [i for i, f in enumerate(THIRD_OCT_HZ) if 200 <= f <= 10000 and nyq_ok[i]]
    octs = np.log2(np.array([THIRD_OCT_HZ[i] for i in sel]) / 1000.0)
    out["tilt_db_per_oct"] = round(float(np.polyfit(octs, vb[sel], 1)[0]), 2)

    # Sibilance: high band (5-10 kHz) share in the most sibilant 10% of frames.
    hi_ix = np.where((freqs >= 5000) & (freqs < min(10000, fs * 0.45)))[0]
    if len(v_sel):
        ratios = []
        for i in range(0, len(v_sel), 128):
            chunk = sframes[v_sel[i:i + 128]] * win
            p = np.abs(np.fft.rfft(chunk, axis=1)) ** 2
            ratios.append(db(p[:, hi_ix].sum(axis=1) / np.maximum(p.sum(axis=1), 1e-30)))
        ratios = np.concatenate(ratios)
        out["sibilance_db"] = round(float(np.mean(np.sort(ratios)[-max(1, len(ratios) // 10):])), 2)
        out["sibilance_mean_db"] = round(float(np.mean(ratios)), 2)
    else:
        out["sibilance_db"] = out["sibilance_mean_db"] = -60.0

    if x.shape[1] >= 2:
        mid = (x[:, 0] + x[:, 1]) / 2
        side = (x[:, 0] - x[:, 1]) / 2
        out["stereo_width_db"] = round(float(db(np.mean(side ** 2)) - db(np.mean(mid ** 2))), 2)
    return out


def analyze_file(path):
    x, fs = read_wav(path)
    return analyze_audio(x, fs, os.path.basename(path)), x, fs


# --------------------------------------------------------------------------
# Reference building
# --------------------------------------------------------------------------
AVG_KEYS = ["lufs", "peak_db", "true_peak_db", "noise_floor_db", "level_p10", "level_p50", "level_p95",
            "dynamic_spread_db", "snr_db", "crest_db", "tilt_db_per_oct", "sibilance_db", "sibilance_mean_db"]


def build_reference(paths, beats=None):
    analyses = []
    for p in paths:
        a, x, fs = analyze_file(p)
        analyses.append(a)
        print(f"  analysed {a['file']}: {a['lufs']:.1f} LUFS, spread {a['dynamic_spread_db']:.1f} dB, "
              f"sibilance {a['sibilance_db']:.1f} dB, noise floor {a['noise_floor_db']:.1f} dB")
    ref = {"type": "studio_engine_reference", "version": 1, "sources": [a["file"] for a in analyses],
           "metrics": {}, "third_octave_hz": THIRD_OCT_HZ}
    for k in AVG_KEYS:
        ref["metrics"][k] = round(float(np.mean([a[k] for a in analyses])), 2)
    # Average spectra in dB, only over bands each file can represent.
    spec = []
    for i in range(len(THIRD_OCT_HZ)):
        vals = [a["spectrum_rel_db"][i] for a in analyses if a["band_valid"][i]]
        spec.append(round(float(np.mean(vals)), 2) if vals else None)
    ref["spectrum_rel_db"] = spec
    if len(analyses) > 1:
        ref["spectrum_spread_db"] = [round(float(np.std([a["spectrum_rel_db"][i] for a in analyses])), 2)
                                     for i in range(len(THIRD_OCT_HZ))]

    if beats:
        offsets = []
        for vp, bp in zip(paths, beats):
            xv, fv = read_wav(vp)
            xb, fb = read_wav(bp)
            lv, _ = integrated_lufs(xv, fv)
            lb, _ = integrated_lufs(xb, fb)
            offsets.append(lv - lb)
            print(f"  {os.path.basename(vp)} vs {os.path.basename(bp)}: vocal sits {lv - lb:+.1f} LU vs beat")
        ref["metrics"]["vocal_vs_beat_lu"] = round(float(np.mean(offsets)), 2)
    ref["files"] = analyses
    return ref


# --------------------------------------------------------------------------
# Matching
# --------------------------------------------------------------------------
def fit_match_eq(target_diff, freqs, weights, fs, max_boost, start=None, iters=60):
    """Least-squares fit of the 10 octave peaking bands to a dB curve."""
    g = np.zeros(10) if start is None else np.array(start, dtype=float)
    freqs = np.asarray(freqs)
    w = np.asarray(weights)
    lam = 0.05
    for _ in range(iters):
        r = match_eq_response(g, freqs, fs) - target_diff
        J = np.zeros((len(freqs), 10))
        for k in range(10):
            dg = np.zeros(10)
            dg[k] = 0.25
            J[:, k] = (match_eq_response(g + dg, freqs, fs) - match_eq_response(g - dg, freqs, fs)) / 0.5
        Jw = J * w[:, None]
        step = np.linalg.solve(Jw.T @ J + lam * np.eye(10), -(Jw.T @ r) - lam * g * 0.1)
        g = np.clip(g + step, -12.0, np.minimum(max_boost, 9.0))
        if np.max(np.abs(step)) < 0.01:
            break
    return g


def spectral_targets(ref, src, denoise_db=0.0):
    """Returns (freqs, diff_db, weights, max_boost_per_band) for the EQ fit."""
    freqs, diff, weights = [], [], []
    for i, f in enumerate(THIRD_OCT_HZ):
        r = ref["spectrum_rel_db"][i]
        if r is None or not src["band_valid"][i]:
            continue
        freqs.append(f)
        diff.append(r - src["spectrum_rel_db"][i])
        # Voice-critical range counts most; extremes count less.
        w = 1.0 if 100 <= f <= 12000 else 0.35
        weights.append(w)
    diff = np.clip(np.array(diff), -14, 14)

    # Never boost a band past the source's own noise floor (would add hiss).
    max_boost = np.full(10, 9.0)
    nspec = src.get("noise_spectrum_abs_db")
    if nspec:
        for k, f0 in enumerate(MATCH_BAND_HZ):
            near = [i for i, f in enumerate(THIRD_OCT_HZ) if f0 / 1.5 <= f <= f0 * 1.5]
            if near:
                headroom = np.mean([src["spectrum_abs_db"][i] - nspec[i] for i in near])
                # The denoiser lowers the floor, which buys some extra headroom.
                max_boost[k] = clamp(headroom + 0.5 * denoise_db - 12.0, 0.0, 9.0)
    return np.array(freqs), diff, np.array(weights), max_boost


def initial_settings(ref, src, have_beat):
    m = ref["metrics"]
    p = {}
    # --- Noise clean-up ------------------------------------------------------
    snr = src["snr_db"]
    ref_snr = m["snr_db"]
    p["dn_on"] = 1
    p["dn_amount"] = round(clamp(ref_snr - snr + 8, 8, 30), 1)
    p["dn_adaptive"] = 1
    p["dn_sensitivity"] = 0.0
    post_floor = src["noise_floor_db"] - p["dn_amount"] * 0.7
    p["gate_on"] = 1
    p["gate_threshold"] = round(clamp(min(post_floor + 14, src["level_p10"] - 8), -85, -25), 1)
    p["gate_range"] = 12.0

    # --- Rumble ----------------------------------------------------------------
    low = [i for i, f in enumerate(THIRD_OCT_HZ) if f <= 63]
    rumble = np.mean([src["spectrum_rel_db"][i] - (ref["spectrum_rel_db"][i] or 0) for i in low])
    p["hpf_on"] = 1
    p["hpf_freq"] = 100.0 if rumble > 6 else 80.0

    # --- Dynamics --------------------------------------------------------------
    ss, rs = src["dynamic_spread_db"], m["dynamic_spread_db"]
    thr = src["level_p50"] - 2.0  # compress the upper half of the performance
    upper = max(1.0, src["level_p95"] - thr)
    wanted_cut = max(0.0, ss - rs)
    ratio = upper / max(0.75, upper - wanted_cut) if wanted_cut > 0 else 1.5
    p["comp_on"] = 1
    p["comp_threshold"] = round(clamp(thr, -50, -6), 1)
    p["comp_ratio"] = round(clamp(ratio, 1.5, 8.0), 2)
    p["comp_attack"] = 8.0 if ratio < 4 else 4.0
    p["comp_release"] = 120.0
    p["comp_makeup"] = 0.0

    # --- Sibilance ------------------------------------------------------------
    p["deess_on"] = 1
    p["deess_freq"] = 6500.0
    p["deess_range"] = 10.0
    sib_excess = src["sibilance_db"] - m["sibilance_db"]
    p["deess_threshold"] = round(clamp(src["level_p95"] - 14 - max(0, sib_excess), -55, -10), 1)

    # --- Tone & colour ---------------------------------------------------------
    p["match_on"] = 1
    p["match_amount"] = 100.0
    p["eq_on"] = 1
    p["sat_on"] = 1
    p["sat_drive"] = 4.0
    p["sat_mix"] = 20.0

    # --- Mix Fit -----------------------------------------------------------------
    p["mix_on"] = 1 if have_beat else 0
    if "vocal_vs_beat_lu" in m:
        p["mix_offset"] = round(clamp(m["vocal_vs_beat_lu"], -12, 12), 1)

    # --- Loudness ----------------------------------------------------------------
    p["lim_on"] = 1
    p["lim_ceiling"] = round(clamp(m["true_peak_db"], -6, -0.1), 2)
    p["lim_drive"] = 0.0
    p["output_gain"] = 0.0
    return p


def apply_eq(p, gains):
    for pid, g in zip(MATCH_BAND_IDS, gains):
        p[pid] = round(float(g), 2)


def score(ref, out):
    """Weighted distance between a rendered result and the reference."""
    m = ref["metrics"]
    freqs, diff, w, _ = spectral_targets(ref, out)
    spec_err = float(np.sqrt(np.sum(w * diff ** 2) / np.sum(w)))
    return {
        "spectrum_rms_db": round(spec_err, 2),
        "lufs_err": round(out["lufs"] - m["lufs"], 2),
        "spread_err": round(out["dynamic_spread_db"] - m["dynamic_spread_db"], 2),
        "sibilance_err": round(out["sibilance_db"] - m["sibilance_db"], 2),
        "true_peak_err": round(out["true_peak_db"] - m["true_peak_db"], 2),
        "total": round(spec_err + 0.5 * abs(out["lufs"] - m["lufs"])
                       + 0.3 * abs(out["dynamic_spread_db"] - m["dynamic_spread_db"])
                       + 0.3 * max(0.0, out["sibilance_db"] - m["sibilance_db"]), 3),
    }


def find_cli(explicit=None):
    candidates = [explicit] if explicit else []
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", ".."))
    exe = "studio_engine_cli" + (".exe" if os.name == "nt" else "")
    candidates += [os.path.join(root, "build", exe), os.path.join(root, "build", "Release", exe),
                   os.path.join(root, "build", "tools", "cli", exe), shutil.which(exe)]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def render(cli, vocal, params, beat, workdir, tag):
    prof = os.path.join(workdir, f"{tag}.json")
    out = os.path.join(workdir, f"{tag}.wav")
    with open(prof, "w") as f:
        json.dump({"name": tag, "params": params}, f)
    cmd = [cli, vocal, out, "--profile", prof, "--auto-noise"]
    if beat:
        cmd += ["--sidechain", beat]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"renderer failed: {r.stderr.strip()}")
    return out


def match(ref, vocal_path, beat_path=None, cli=None, iterations=6, verbose=True):
    src, xs, fs = analyze_file(vocal_path)
    if verbose:
        print(f"source {src['file']}: {src['lufs']:.1f} LUFS, spread {src['dynamic_spread_db']:.1f} dB, "
              f"SNR {src['snr_db']:.1f} dB, sibilance {src['sibilance_db']:.1f} dB")
    ref = json.loads(json.dumps(ref))  # private copy; targets may be adjusted
    if beat_path:
        xb, fb = read_wav(beat_path)
        beat_lufs, _ = integrated_lufs(xb, fb)
        m = ref["metrics"]
        if "vocal_vs_beat_lu" in m:
            # Pairing wins: the vocal should sit where the reference vocal sat
            # against its own beat, so the loudness target follows this beat.
            m["lufs"] = round(beat_lufs + m["vocal_vs_beat_lu"], 2)
        else:
            m["vocal_vs_beat_lu"] = round(m["lufs"] - beat_lufs, 2)
        if verbose:
            print(f"instrumental {os.path.basename(beat_path)}: {beat_lufs:.1f} LUFS -> vocal target "
                  f"{m['lufs']:.1f} LUFS ({m['vocal_vs_beat_lu']:+.1f} LU vs beat)")
    p = initial_settings(ref, src, beat_path is not None)
    freqs, diff, w, max_boost = spectral_targets(ref, src, p["dn_amount"])
    gains = fit_match_eq(diff, freqs, w, fs, max_boost)
    apply_eq(p, gains)
    # Open-loop loudness estimate: compressor + EQ change level only modestly.
    need = ref["metrics"]["lufs"] - src["lufs"]
    p["lim_drive"] = round(clamp(need, 0, 24), 2)
    p["output_gain"] = round(clamp(min(0.0, need), -24, 0), 2)

    history = []
    if cli is None:
        if verbose:
            print("note: studio_engine_cli not found - settings are an open-loop estimate.\n"
                  "      Build the project (see README) for closed-loop matching.")
        return p, src, None, history

    best = None
    with tempfile.TemporaryDirectory() as wd:
        for it in range(iterations):
            out_path = render(cli, vocal_path, p, beat_path, wd, f"iter{it}")
            out, _, _ = analyze_file(out_path)
            s = score(ref, out)
            history.append({"iteration": it, **s})
            if verbose:
                print(f"  iter {it}: spectrum {s['spectrum_rms_db']:.2f} dB rms | loudness {s['lufs_err']:+.2f} LU | "
                      f"dynamics {s['spread_err']:+.2f} dB | sibilance {s['sibilance_err']:+.2f} dB | "
                      f"peak {s['true_peak_err']:+.2f} dB")
            if os.environ.get("STUDIO_MATCH_DEBUG"):
                print("     ", {k: p[k] for k in ("comp_threshold", "comp_ratio", "deess_threshold", "deess_range", "lim_drive")})
            if best is None or s["total"] < best[0]["total"]:
                best = (s, dict(p), out)
            if it == iterations - 1:
                break

            # --- Refine EQ on the measured residual --------------------------
            f2, resid, w2, _ = spectral_targets(ref, out)
            cur = np.array([p[k] for k in MATCH_BAND_IDS])
            gains = fit_match_eq(resid, f2, w2, fs, np.full(10, 12.0), iters=30)
            new = np.clip(cur + 0.8 * gains, -12, np.minimum(max_boost, 9))
            apply_eq(p, new)

            # --- Dynamics: ratio from spread error ------------------------------
            # Ratio first; once it is firm (4:1) move the threshold down so the
            # quieter phrases are controlled too (that is what narrows spread).
            se_ = s["spread_err"]
            if se_ > 0.5:
                if p["comp_ratio"] < 4.0:
                    p["comp_ratio"] = round(clamp(p["comp_ratio"] * (1 + 0.2 * se_), 1.2, 4.0), 2)
                else:
                    p["comp_threshold"] = round(clamp(p["comp_threshold"] - 1.5 * se_, -55, -6), 1)
            elif se_ < -0.5:
                if p["comp_threshold"] < -12 and p["comp_ratio"] <= 3.0:
                    p["comp_threshold"] = round(clamp(p["comp_threshold"] - 1.5 * se_, -55, -6), 1)
                else:
                    p["comp_ratio"] = round(clamp(p["comp_ratio"] * (1 + 0.15 * se_), 1.2, 12.0), 2)

            # --- Sibilance -------------------------------------------------------
            sib = s["sibilance_err"]
            if sib > 0.5:
                p["deess_threshold"] = round(clamp(p["deess_threshold"] - 0.8 * sib, -60, 0), 1)
                p["deess_range"] = round(clamp(p["deess_range"] + 1.5 * sib, 0, 20), 1)
            elif sib < -1.5:
                p["deess_threshold"] = round(clamp(p["deess_threshold"] - 0.8 * sib, -60, 0), 1)
                p["deess_range"] = round(clamp(p["deess_range"] + 1.0 * sib, 3, 20), 1)

            # --- Loudness ---------------------------------------------------------
            gain_needed = -s["lufs_err"]
            total = p["lim_drive"] + p["output_gain"] + gain_needed
            p["lim_drive"] = round(clamp(total, 0, 24), 2)
            p["output_gain"] = round(clamp(min(0.0, total), -24, 0), 2)
    s, p, _ = best
    return p, src, s, history


def make_profile(name, params, ref, src, final_score, history):
    return {
        "name": name,
        "format": "studio_engine_profile",
        "version": 1,
        "params": params,
        "analysis": {
            "reference_sources": ref.get("sources"),
            "reference_metrics": ref["metrics"],
            "source": {k: src[k] for k in AVG_KEYS if k in src},
            "final_match": final_score,
            "iterations": history,
        },
    }


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def print_report(a):
    keys = ["lufs", "true_peak_db", "noise_floor_db", "snr_db", "level_p10", "level_p50", "level_p95",
            "dynamic_spread_db", "crest_db", "tilt_db_per_oct", "sibilance_db", "stereo_width_db"]
    print(f"== {a['file']} ({a['sample_rate']:.0f} Hz, {a['channels']} ch, {a['duration_s']} s)")
    for k in keys:
        if k in a:
            print(f"  {k:<20} {a[k]:>8.2f}")
    print("  spectrum (dB rel. 250 Hz-4 kHz average):")
    row = []
    for f, v, ok in zip(THIRD_OCT_HZ, a["spectrum_rel_db"], a["band_valid"]):
        if ok:
            row.append(f"{f:>7.0f}Hz {v:+6.1f}")
    for i in range(0, len(row), 4):
        print("   " + "   ".join(row[i:i + 4]))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Studio Engine reference matcher")
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("analyze", help="build a reference target from acapellas")
    a.add_argument("files", nargs="+")
    a.add_argument("--beats", nargs="*", help="instrumentals matching each acapella (optional)")
    a.add_argument("-o", "--output", required=True)

    m = sub.add_parser("match", help="compute Studio Engine settings for a vocal")
    m.add_argument("--reference", required=True, help="reference JSON from 'analyze' (or a WAV acapella)")
    m.add_argument("--vocal", required=True)
    m.add_argument("--beat", help="instrumental for this vocal (enables Mix Fit)")
    m.add_argument("-o", "--output", required=True)
    m.add_argument("--name")
    m.add_argument("--cli", help="path to studio_engine_cli")
    m.add_argument("--iterations", type=int, default=6)
    m.add_argument("--render", help="also render the matched vocal to this WAV path")

    c = sub.add_parser("compare", help="compare a processed file with a reference")
    c.add_argument("processed")
    c.add_argument("reference")

    r = sub.add_parser("report", help="analyse a single file")
    r.add_argument("file")

    args = ap.parse_args(argv)

    if args.cmd == "analyze":
        if args.beats and len(args.beats) != len(args.files):
            ap.error("--beats needs one instrumental per acapella")
        print(f"analysing {len(args.files)} reference(s)")
        ref = build_reference(args.files, args.beats)
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w") as f:
            json.dump(ref, f, indent=2)
        print(f"wrote reference target {args.output}")
        return 0

    if args.cmd == "match":
        if args.reference.lower().endswith(".wav"):
            ref = build_reference([args.reference])
        else:
            with open(args.reference) as f:
                ref = json.load(f)
        cli = find_cli(args.cli)
        params, src, final, hist = match(ref, args.vocal, args.beat, cli, args.iterations)
        name = args.name or os.path.splitext(os.path.basename(args.output))[0]
        prof = make_profile(name, params, ref, src, final, hist)
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w") as f:
            json.dump(prof, f, indent=2)
        print(f"wrote Studio Engine profile {args.output}  (load it with 'Load Profile' in the plugin)")
        if final:
            print(f"final match: spectrum within {final['spectrum_rms_db']:.2f} dB rms, loudness "
                  f"{final['lufs_err']:+.2f} LU, dynamics {final['spread_err']:+.2f} dB")
        if args.render:
            if not cli:
                print("cannot render: studio_engine_cli not found")
                return 1
            cmd = [cli, args.vocal, args.render, "--profile", args.output, "--auto-noise", "--pcm24"]
            if args.beat:
                cmd += ["--sidechain", args.beat]
            subprocess.run(cmd, check=True)
        return 0

    if args.cmd == "compare":
        pa, _, _ = analyze_file(args.processed)
        ra = build_reference([args.reference])
        s = score(ra, pa)
        print_report(pa)
        print(f"\nvs reference {args.reference}:")
        for k, v in s.items():
            print(f"  {k:<18} {v:+.2f}")
        return 0

    if args.cmd == "report":
        a_, _, _ = analyze_file(args.file)
        print_report(a_)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
