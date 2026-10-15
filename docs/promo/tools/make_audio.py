#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# make_audio.py - synthesizes the Vita JP Overlay promo music and UI sound
# effects from code. No samples, soundfonts, downloaded audio or models: every
# sound is built here from sine waves and seeded pseudo-random noise with numpy,
# then ffmpeg normalises loudness and encodes.
#
# Usage:  .venv/bin/python make_audio.py            # build everything
#         .venv/bin/python make_audio.py --verify   # measure the outputs
#
# Environment: FFMPEG / FFPROBE override the ffmpeg binaries.
import json
import os
import re
import shutil
import struct
import subprocess
import sys

import numpy as np

# Outputs go to docs/promo/assets/audio; intermediates to docs/promo/.cache.
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "audio")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")
WORK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".cache", "audio-work")

SR = 48000
BPM = 120
BEAT = 60.0 / BPM            # 0.5 s
BAR = 4 * BEAT               # 2.0 s
NBARS = 22
DUR = NBARS * BAR            # 44.0 s
N = int(round(DUR * SR))     # 2,112,000 samples
SWING = 0.025                # off-beat 8ths land 25 ms late

MUSIC_LUFS, MUSIC_TP, MUSIC_LRA = -16.0, -1.5, 20.0
SFX_LUFS_M, SFX_TP = -18.0, -1.5


# --------------------------------------------------------------- utilities
def hz(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


def tt(n):
    return np.arange(n) / SR


def fades(sig, fin=0.003, fout=0.010):
    """Raised-cosine fade in/out so a note starts and ends at exactly zero."""
    n = sig.shape[-1]
    a = min(int(fin * SR), n // 2)
    b = min(int(fout * SR), n // 2)
    env = np.ones(n)
    if a:
        env[:a] = 0.5 - 0.5 * np.cos(np.pi * np.arange(a) / a)
    if b:
        env[n - b:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(1, b + 1) / b)
    return sig * env


def pan2(mono, p):
    """Equal-power pan, p in [-1, 1]."""
    th = (p + 1.0) * np.pi / 4.0
    return np.vstack([mono * np.cos(th), mono * np.sin(th)])


def place(bus, sig, t0):
    """Mix a (2, n) signal into a (2, N) bus at time t0 (seconds)."""
    s = int(round(t0 * SR))
    n = sig.shape[-1]
    e = min(s + n, bus.shape[-1])
    if s < 0 or e <= s:
        return
    bus[:, s:e] += sig[:, : e - s]


def lp(fc, order=2):
    return lambda f: 1.0 / np.sqrt(1.0 + (f / fc) ** (2 * order))


def hp(fc, order=2):
    return lambda f: (f > 0) / np.sqrt(1.0 + (fc / np.maximum(f, 1e-9)) ** (2 * order))


def fft_filt(x, *resps):
    """Zero-phase magnitude filtering by FFT (zero-padded, no wrap-around)."""
    n = x.shape[-1]
    m = 1 << int(np.ceil(np.log2(n + 8192)))
    f = np.fft.rfftfreq(m, 1.0 / SR)
    H = np.ones_like(f)
    for r in resps:
        H = H * r(f)
    return np.fft.irfft(np.fft.rfft(x, n=m) * H, n=m)[..., :n]


def moving_avg(x, win):
    w = max(1, int(win * SR))
    c = np.cumsum(np.concatenate([np.zeros(w), x, np.zeros(w)]))
    y = (c[w:] - c[:-w]) / w
    off = w // 2
    return y[off: off + len(x)]


def comb(x, d, g):
    y = np.zeros_like(x)
    for s in range(0, len(x), d):
        e = min(s + d, len(x))
        y[s:e] = x[s:e]
        if s >= d:
            y[s:e] += g * y[s - d: e - d]
    return y


def allpass(x, d, g):
    y = np.zeros_like(x)
    for s in range(0, len(x), d):
        e = min(s + d, len(x))
        y[s:e] = -g * x[s:e]
        if s >= d:
            y[s:e] += x[s - d: e - d] + g * y[s - d: e - d]
    return y


def schroeder(x, rt60, combs_ms, aps):
    out = np.zeros_like(x)
    for ms in combs_ms:
        d = int(ms * SR / 1000.0)
        g = 10.0 ** (-3.0 * d / (rt60 * SR))
        out += comb(x, d, g)
    out /= len(combs_ms)
    for ms, g in aps:
        out = allpass(out, int(ms * SR / 1000.0), g)
    return out


def write_wav_float(path, x):
    """(2, n) float -> 32-bit float WAV (intermediate for ffmpeg)."""
    data = np.ascontiguousarray(x.T.astype("<f4")).tobytes()
    ch = x.shape[0]
    with open(path, "wb") as fh:
        fh.write(b"RIFF" + struct.pack("<I", 4 + 26 + 12 + 8 + len(data)) + b"WAVE")
        fh.write(b"fmt " + struct.pack("<IHHIIHHH", 18, 3, ch, SR, SR * ch * 4, ch * 4, 32, 0))
        fh.write(b"fact" + struct.pack("<II", 4, x.shape[1]))
        fh.write(b"data" + struct.pack("<I", len(data)) + data)


def read_wav16(path):
    with open(path, "rb") as fh:
        b = fh.read()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", path
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = b[pos + 8: pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            data = body
        pos += 8 + size + (size & 1)
    tag, ch, sr, _, _, bits = fmt
    assert bits == 16 and tag in (1, 0xFFFE), (path, fmt)
    return sr, ch, np.frombuffer(data, "<i2").reshape(-1, ch).T


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit("command failed: " + " ".join(cmd))
    return r.stderr


# ------------------------------------------------------------- instruments
def ep_voice(m, hold, vel, seed_phase=0.0):
    """Warm electric piano: 2-operator phase modulation, detuned L/R pair,
    soft 45 ms attack, slow release, gentle tremolo. Returns (2, n)."""
    rel = 0.9
    n = int((hold + rel) * SR)
    t = tt(n)
    f = hz(m)
    att = 0.045
    env = np.where(t < att, 0.5 - 0.5 * np.cos(np.pi * t / att), 1.0)
    env *= 0.55 + 0.45 * np.exp(-t / 0.7)
    env *= np.where(t > hold, np.exp(-(t - hold) / 0.22), 1.0)
    idx = 0.85 * np.exp(-t / 0.35) + 0.22          # brightness falls as it rings
    out = []
    for det, ph in ((+0.0022, 0.0), (-0.0022, np.pi / 2)):
        fc = f * (1.0 + det)
        mod = idx * np.sin(2 * np.pi * fc * t)
        car = np.sin(2 * np.pi * fc * t + mod + seed_phase)
        car += 0.035 * np.exp(-t / 0.03) * np.sin(2 * np.pi * 7.0 * fc * t)  # tine
        trem = 1.0 + 0.07 * np.sin(2 * np.pi * 4.2 * t + ph)
        out.append(car * trem * env)
    return fades(np.vstack(out) * vel, 0.004, 0.03)


def pluck(m, vel, dur=0.5):
    """Plucky arp: sine + soft square-ish additive partials, higher partials
    decaying faster (an implicit, gentle lowpass)."""
    n = int(dur * SR)
    t = tt(n)
    f = hz(m)
    sig = np.zeros(n)
    for k in range(1, 9):
        if k * f > 11000:
            break
        a = (1.0 if k == 1 else 0.0) + (0.32 / k if k % 2 else 0.0) + 0.06 / k
        tau = 0.32 / (1.0 + 0.9 * (k - 1))
        sig += a * np.exp(-t / tau) * np.sin(2 * np.pi * k * f * t)
    return fades(sig * vel / 1.4, 0.002, 0.06)


def bass_note(m, dur, vel):
    n = int((dur + 0.05) * SR)
    t = tt(n)
    f = hz(m)
    env = np.minimum(1.0, t / 0.008) * (0.75 + 0.25 * np.exp(-t / 0.25))
    s = np.sin(2 * np.pi * f * t) + 0.10 * np.sin(4 * np.pi * f * t)
    s = np.tanh(1.3 * s) / np.tanh(1.3)            # round, slightly warm
    return fades(s * env * vel, 0.006, 0.05)


def kick(vel):
    n = int(0.42 * SR)
    t = tt(n)
    f = 47.0 + 75.0 * np.exp(-t / 0.028)
    ph = 2 * np.pi * np.cumsum(f) / SR
    s = np.sin(ph) * np.exp(-t / 0.22)
    return fades(s * vel, 0.001, 0.05)


def snap(rng, vel):
    n = int(0.26 * SR)
    t = tt(n)
    noise = fft_filt(rng.standard_normal(n), hp(1300, 2), lp(4800, 2))
    noise /= np.max(np.abs(noise)) + 1e-12
    body = 0.35 * np.sin(2 * np.pi * 190 * t) * np.exp(-t / 0.04)
    s = noise * np.exp(-t / 0.055) + body
    return fades(s * vel, 0.001, 0.05)


def shaker(rng, vel):
    n = int(0.09 * SR)
    t = tt(n)
    noise = fft_filt(rng.standard_normal(n), hp(5500, 3), lp(13000, 2))
    noise /= np.max(np.abs(noise)) + 1e-12
    env = np.where(t < 0.008, t / 0.008, np.exp(-(t - 0.008) / 0.026))
    return fades(noise * env * vel, 0.001, 0.02)


def bell(m, vel, dur=1.3):
    """Soft marimba/bell: fundamental + marimba partials (x4, x10) + a little
    phase-modulated shimmer, all decaying."""
    n = int(dur * SR)
    t = tt(n)
    f = hz(m)
    s = np.exp(-t / 0.5) * np.sin(2 * np.pi * f * t
                                  + 0.5 * np.exp(-t / 0.15) * np.sin(2 * np.pi * 3.5 * f * t))
    s += 0.22 * np.exp(-t / 0.09) * np.sin(2 * np.pi * 3.99 * f * t)
    if 9.9 * f < 16000:
        s += 0.05 * np.exp(-t / 0.03) * np.sin(2 * np.pi * 9.9 * f * t)
    return fades(s * vel, 0.0015, 0.08)


# ----------------------------------------------------------------- the song
CHORDS = {                       # pad voicing (MIDI), bass root (MIDI)
    "Fmaj7": ([53, 57, 60, 64], 41),
    "Am7": ([52, 55, 57, 60], 33),
    "Dm7": ([53, 57, 60, 62], 38),
    "Bbmaj7": ([53, 57, 58, 62], 34),
    "C": ([52, 55, 60, 62], 36),           # C(add9)
    "Fmaj9": ([53, 57, 60, 64, 67], 41),
}
PROG = ["Fmaj7", "Am7", "Dm7", "Bbmaj7"]
SPLIT = {7, 15, 19}                        # bars 8, 16, 20: Bbmaj7 | C


def segments(i):
    """Chord segments of 0-based bar i: (start beat, beats, chord)."""
    if i == 21:
        return [(0, 4, "Fmaj9")]
    if i == 20:
        return [(0, 2, "Fmaj7"), (2, 2, "C")]   # turnaround into the outro
    if i in SPLIT:
        return [(0, 2, "Bbmaj7"), (2, 2, "C")]
    return [(0, 4, PROG[i % 4])]


def section(i):
    if i < 2:
        return "intro"
    if i < 10:
        return "A"
    if i < 18:
        return "B"
    if i < 21:
        return "A2"
    return "outro"


# bell motif: (beat, MIDI, beats). F-major pentatonic F G A C D.
X = [(0, 86, 1), (1, 84, .5), (1.5, 81, .5), (2, 84, 1), (3, 86, .5), (3.5, 84, .5),
     (4, 81, 1.5), (5.5, 79, .5), (6, 77, 1), (7, 79, 1)]            # over Dm7 | Bbmaj7
Y = [(0, 81, 1), (1, 84, .5), (1.5, 86, .5), (2, 84, 1), (3, 81, .5), (3.5, 79, .5),
     (4, 81, 1.5), (5.5, 79, .5), (6, 84, 1), (7, 81, 1)]            # over Fmaj7 | Am7
Y_END = Y[:6] + [(4, 81, 2), (6, 77, 2)]                            # resolves to F
MOTIF = {10: X, 12: Y, 14: X, 16: Y_END}


def swing(beat):
    """Off-beat 8ths are late by SWING seconds."""
    frac = (beat * 2) % 2
    return beat * BEAT + (SWING if abs(frac - 1.0) < 1e-6 else 0.0)


def make_music():
    rng = np.random.default_rng(20261015)
    hum = np.random.default_rng(7)               # humanisation, separate stream
    pad = np.zeros((2, N)); arp = np.zeros((2, N)); bass = np.zeros((2, N))
    kik = np.zeros((2, N)); snr = np.zeros((2, N)); shk = np.zeros((2, N))
    bel = np.zeros((2, N))
    kicks = []

    for i in range(NBARS):
        t0 = i * BAR
        sec = section(i)
        for (b0, nb, ch) in segments(i):
            notes, root = CHORDS[ch]
            ts = t0 + b0 * BEAT
            # --- electric-piano pad
            if sec == "outro":
                hold, v = 1.6, 0.11
            else:
                hold, v = nb * BEAT - 0.02, (0.10 if sec in ("intro", "A", "A2") else 0.115)
            for k, m in enumerate(notes):
                place(pad, ep_voice(m, hold, v, seed_phase=0.3 * k), ts + 0.006 * k)
            if sec == "B" and nb == 4:          # soft comp re-strike, "and" of 2
                for k, m in enumerate(notes):
                    place(pad, ep_voice(m, 1.9, 0.05), ts + 1.5 * BEAT + SWING + 0.006 * k)
            # --- arpeggio, 8ths
            tones = [m + 12 for m in notes]
            if sec == "outro":
                for k, m in enumerate(tones):          # one slow up-roll, then ring
                    place(arp, pan2(pluck(m, 0.20, 1.6), -0.25), ts + k * 0.12)
            else:
                pat = [0, 1, 2, 3, 2, 1, 2, 3]
                for e in range(int(nb * 2)):
                    beat = b0 + e * 0.5
                    m = tones[pat[(e + int(b0 * 2)) % 8] % len(tones)]
                    v = 0.22 if e % 2 == 0 else 0.16
                    if sec == "intro":
                        v *= 0.65 + 0.35 * (i * 8 + e) / 16.0
                    v *= 1.0 + 0.06 * (hum.random() - 0.5)
                    dt = 0.004 * (hum.random() - 0.5)
                    place(arp, pan2(pluck(m, v), -0.25), t0 + swing(beat) + dt)
            # --- sub bass
            if sec == "outro":
                place(bass, pan2(bass_note(root, 1.7, 0.30), 0), ts)
            elif sec != "intro":
                if nb == 4:
                    pattern = [(0, 1.7, 0), (2.5, 0.4, 12), (3, 0.9, 0)]
                else:
                    pattern = [(0, 1.7, 0)]
                for (bb, dd, oc) in pattern:
                    place(bass, pan2(bass_note(root + oc, dd * BEAT, 0.30 if oc == 0 else 0.18), 0),
                          ts + swing(bb))
        # --- drums: bars 3-21
        if sec in ("A", "B", "A2"):
            for beat in (0, 2):
                place(kik, pan2(kick(0.55), 0), t0 + beat * BEAT)
                kicks.append(t0 + beat * BEAT)
            for beat in (1, 3):
                place(snr, pan2(snap(rng, 0.45), 0.05), t0 + beat * BEAT)
            for e in range(8):
                beat = e * 0.5
                v = (0.11 if e % 2 == 0 else 0.17) * (1.25 if sec == "B" else 1.0)
                v *= 1.0 + 0.15 * (hum.random() - 0.5)
                place(shk, pan2(shaker(rng, v), 0.3), t0 + swing(beat))
        # --- bell motif: groove B, two-bar phrases
        if i in MOTIF:
            for (bb, m, dd) in MOTIF[i]:
                place(bel, pan2(bell(m, 0.22, max(0.9, dd * BEAT + 0.8)), 0.3), t0 + swing(bb))
        if sec == "outro":
            place(bel, pan2(bell(81, 0.10, 1.9), 0.3), t0 + 0.5)

    # gentle kick ducking of pad and arp (lo-fi pump)
    duck = np.ones(N)
    tn = tt(N)
    for tk in kicks:
        s = int(tk * SR)
        e = min(N, s + int(0.5 * SR))
        duck[s:e] *= 1.0 - 0.18 * np.exp(-(tn[s:e] - tk) / 0.12)
    pad *= duck
    arp *= 0.5 + 0.5 * duck

    pad = fft_filt(pad, lp(3500, 1), hp(90, 1))
    buses = {"pad": pad, "arp": arp, "bass": bass, "kick": kik,
             "snap": snr, "shaker": shk, "bells": bel}

    # ping-pong dotted-8th delay on arp + bells, darkened
    send = fft_filt(0.35 * arp + 0.6 * bel, hp(300, 2), lp(3200, 2)).sum(axis=0) * 0.5
    dly = np.zeros((2, N))
    d = int(0.75 * BEAT * SR)
    for k in range(1, 5):
        g = 0.38 ** k
        ch = k % 2
        dly[ch, k * d:] += g * send[: N - k * d]
    # Schroeder reverb on a mix send
    rsend = (0.7 * pad + 0.5 * arp + 0.6 * bel + 0.25 * snr + 0.1 * shk).sum(axis=0)
    rsend = fft_filt(rsend, hp(220, 2), lp(5500, 2))
    pre = int(0.02 * SR)
    rsend = np.concatenate([np.zeros(pre), rsend[:-pre]])
    aps = [(5.0, 0.7), (1.7, 0.7)]
    revL = schroeder(rsend, 1.9, [29.7, 37.1, 41.1, 43.7], aps)
    revR = schroeder(rsend, 1.9, [30.4, 36.3, 42.2, 44.9], aps)
    rev = fft_filt(np.vstack([revL, revR]), lp(6000, 1))

    mix = (1.0 * pad + 0.9 * arp + 1.0 * bass + 0.9 * kik + 0.8 * snr
           + 0.8 * shk + 1.0 * bel + 0.30 * dly + 0.35 * rev)

    # master: DC/subsonic cut, gentle 2:1 bus compression, soft knee clip
    mix = fft_filt(mix, hp(28, 2))
    mix -= mix.mean(axis=1, keepdims=True)
    mix /= np.max(np.abs(mix))
    lvl = 10 * np.log10(moving_avg((mix ** 2).mean(axis=0), 0.03) + 1e-12)
    gdb = -np.maximum(lvl - (-20.0), 0.0) * (1.0 - 1.0 / 2.0)
    gdb = moving_avg(gdb, 0.08)
    mix *= 10 ** (gdb / 20.0)
    mix *= 0.9 / np.max(np.abs(mix))
    a = np.abs(mix)
    knee = 0.6
    mix = np.where(a > knee, np.sign(mix) * (knee + (1 - knee) * np.tanh((a - knee) / (1 - knee))), mix)

    # final ring-out: fade 42.4 s -> 43.92 s, silence after; tiny fade-in
    env = np.ones(N)
    f0, f1 = int(42.4 * SR), int(43.92 * SR)
    x = np.arange(f1 - f0) / (f1 - f0)
    env[f0:f1] = (0.5 + 0.5 * np.cos(np.pi * x)) ** 1.5
    env[f1:] = 0.0
    env[: int(0.005 * SR)] = np.linspace(0, 1, int(0.005 * SR))
    mix *= env
    mix *= 0.89 / np.max(np.abs(mix))

    for k, v in buses.items():
        print(f"  bus {k:7s} rms {20*np.log10(np.sqrt(np.mean(v**2))+1e-12):6.1f} dBFS (pre-mix)")
    assert np.isfinite(mix).all()
    return mix


# --------------------------------------------------------------------- SFX
def sfx_tap(rng):
    n = int(0.070 * SR); t = tt(n)
    f = 1150 + 650 * np.exp(-t / 0.006)
    s = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t / 0.014)
    s += 0.25 * np.sin(2 * np.pi * 420 * t) * np.exp(-t / 0.02)
    tr = fft_filt(rng.standard_normal(n), lp(3000, 2)) * np.exp(-t / 0.002) * 0.3
    s = fft_filt(s + tr, lp(6000, 1))
    return pan2(fades(s, 0.0005, 0.02), 0.0)


def sfx_click(rng):
    n = int(0.045 * SR); t = tt(n)
    def one(t0, v):
        u = np.maximum(t - t0, 0); on = t >= t0
        noise = fft_filt(rng.standard_normal(n), hp(2200, 2), lp(5500, 2))
        noise /= np.max(np.abs(noise))
        body = np.sin(2 * np.pi * 950 * u) * np.exp(-u / 0.006)
        return on * (0.6 * noise * np.exp(-u / 0.0018) + body) * v
    s = one(0.0, 1.0) + one(0.020, 0.45)            # press, then release
    return pan2(fades(s, 0.0003, 0.012), 0.0)


def bandsweep(noise, centres, bw_oct, win=1024, hop=256):
    """Time-varying band-pass by STFT overlap-add; centres(t) in Hz."""
    n = len(noise)
    w = np.hanning(win)
    f = np.fft.rfftfreq(win, 1.0 / SR)
    out = np.zeros(n + win); norm = np.zeros(n + win)
    x = np.concatenate([noise, np.zeros(win)])
    for s in range(0, n, hop):
        c = centres((s + win / 2) / SR)
        g = np.exp(-0.5 * (np.log2(np.maximum(f, 1.0) / c) / bw_oct) ** 2)
        seg = np.fft.irfft(np.fft.rfft(x[s:s + win] * w) * g, n=win)
        out[s:s + win] += seg * w
        norm[s:s + win] += w * w
    return out[:n] / np.maximum(norm[:n], 1e-3)


def sfx_whoosh(rng):
    d = 0.45; n = int(d * SR); t = tt(n)
    cen = lambda x: 600 * (3200 / 600) ** np.sin(np.pi * min(x / d, 1.0)) if x < d else 600
    nz = np.vstack([bandsweep(rng.standard_normal(n), cen, 0.9),
                    bandsweep(rng.standard_normal(n), cen, 0.9)])
    u = t / d
    env = np.sin(np.pi * u ** 0.8) ** 2
    p = -0.6 + 1.2 * u                                   # drifts left -> right
    th = (p + 1) * np.pi / 4
    s = np.vstack([nz[0] * env * np.cos(th), nz[1] * env * np.sin(th)])
    return fades(s, 0.005, 0.04)


def sfx_pop(rng):
    n = int(0.160 * SR); t = tt(n)
    f = 330 + 620 * (1 - np.exp(-t / 0.012))
    ph = 2 * np.pi * np.cumsum(f) / SR
    env = np.minimum(1, t / 0.002) * np.exp(-t / 0.035)
    s = (np.sin(ph) + 0.18 * np.sin(2 * ph)) * env
    s += 0.12 * np.sin(2 * np.pi * 1900 * t) * np.exp(-t / 0.008)
    return pan2(fades(s, 0.0005, 0.03), 0.0)


def sfx_chime(rng):
    n = int(0.600 * SR)
    out = np.zeros((2, n))
    for t0, m, p in ((0.0, 84, -0.15), (0.11, 91, 0.15)):   # C6 -> G6
        k = n - int(t0 * SR); t = tt(k); f = hz(m)
        s = np.exp(-t / 0.17) * np.sin(2 * np.pi * f * t)
        s += 0.25 * np.exp(-t / 0.08) * np.sin(2 * np.pi * 2.0 * f * t)
        s += 0.08 * np.exp(-t / 0.04) * np.sin(2 * np.pi * 3.01 * f * t)
        s *= np.minimum(1, t / 0.002)
        out[:, int(t0 * SR):] += pan2(s, p)
    return fades(out, 0.0005, 0.08)


def sfx_home(rng):
    d = 0.38; n = int(d * SR); t = tt(n)
    cen = lambda x: 450 * (4200 / 450) ** min(x / d, 1.0)
    nz = np.vstack([bandsweep(rng.standard_normal(n), cen, 0.7),
                    bandsweep(rng.standard_normal(n), cen, 0.7)])
    u = t / d
    env = np.where(u < 0.7, np.sin(np.pi / 2 * u / 0.7) ** 2,
                   np.cos(np.pi / 2 * (u - 0.7) / 0.3) ** 2)
    f = 300 * (900 / 300) ** u
    tone = 0.25 * np.sin(2 * np.pi * np.cumsum(f) / SR) * env
    s = nz * env + tone
    return fades(s, 0.004, 0.03)


SFX = {"sfx-tap": sfx_tap, "sfx-click": sfx_click, "sfx-whoosh": sfx_whoosh,
       "sfx-pop": sfx_pop, "sfx-chime": sfx_chime, "sfx-home": sfx_home}


# ------------------------------------------------------- ffmpeg processing
def loudnorm_json(stderr):
    j = stderr[stderr.rindex("{"): stderr.rindex("}") + 1]
    return json.loads(j)


def master_music(raw, out_wav):
    ln = f"loudnorm=I={MUSIC_LUFS}:TP={MUSIC_TP}:LRA={MUSIC_LRA}"
    m1 = loudnorm_json(run([FFMPEG, "-hide_banner", "-nostats", "-i", raw,
                            "-af", ln + ":print_format=json", "-f", "null", "-"]))
    ln2 = (ln + f":measured_I={m1['input_i']}:measured_TP={m1['input_tp']}"
           f":measured_LRA={m1['input_lra']}:measured_thresh={m1['input_thresh']}"
           f":offset={m1['target_offset']}:linear=true:print_format=json")
    af = ln2 + f",aresample={SR},apad=whole_len={N},atrim=end_sample={N}"
    m2 = loudnorm_json(run([FFMPEG, "-hide_banner", "-nostats", "-y", "-i", raw,
                            "-af", af, "-ar", str(SR), "-c:a", "pcm_s16le",
                            "-map_metadata", "-1", "-fflags", "+bitexact",
                            "-flags:a", "+bitexact", out_wav]))
    if m2["normalization_type"] != "linear":
        raise SystemExit("loudnorm fell back to dynamic mode: " + json.dumps(m2))
    return m1, m2


def ebur128(path, pad=0.0):
    af = (f"apad=pad_dur={pad}," if pad else "") + "ebur128=peak=true:framelog=verbose"
    err = run([FFMPEG, "-hide_banner", "-nostats", "-v", "verbose", "-i", path, "-af", af, "-f", "null", "-"])
    ms = [float(v) for v in re.findall(r"\bM:\s*(-?[\d.]+)", err)]
    ss = [float(v) for v in re.findall(r"\bS:\s*(-?[\d.]+)", err)]
    summ = err[err.rindex("Summary:"):]
    I = float(re.search(r"I:\s*(-?[\d.]+) LUFS", summ).group(1))
    tp = float(re.search(r"True peak:\s*Peak:\s*(-?[\d.inf]+)", summ).group(1))
    return {"I": I, "M_max": max(ms), "S_max": max(ss), "TP": tp}


def master_sfx(raw, out_wav):
    m = ebur128(raw, pad=1.0)
    gain = SFX_LUFS_M - m["M_max"]
    capped = False
    if m["TP"] + gain > SFX_TP:
        gain = SFX_TP - m["TP"] - 0.1
        capped = True
    run([FFMPEG, "-hide_banner", "-nostats", "-y", "-i", raw,
         "-af", f"volume={gain:.3f}dB:precision=double", "-ar", str(SR),
         "-c:a", "pcm_s16le", "-map_metadata", "-1", "-fflags", "+bitexact",
         "-flags:a", "+bitexact", out_wav])
    return gain, capped


def build():
    os.makedirs(WORK, exist_ok=True)
    print("synthesising music ...")
    mix = make_music()
    raw = os.path.join(WORK, "music-raw.wav")
    write_wav_float(raw, mix)
    m1, m2 = master_music(raw, os.path.join(HERE, "promo-music.wav"))
    print("  loudnorm pass 1 (input):  I=%s LUFS TP=%s dBTP LRA=%s LU" %
          (m1["input_i"], m1["input_tp"], m1["input_lra"]))
    print("  loudnorm pass 2 (output): I=%s LUFS TP=%s dBTP LRA=%s LU type=%s" %
          (m2["output_i"], m2["output_tp"], m2["output_lra"], m2["normalization_type"]))
    run([FFMPEG, "-hide_banner", "-nostats", "-y", "-i", os.path.join(HERE, "promo-music.wav"),
         "-c:a", "aac", "-b:a", "192k", "-map_metadata", "-1", "-fflags", "+bitexact",
         "-flags:a", "+bitexact", "-movflags", "+faststart", os.path.join(HERE, "promo-music.m4a")])
    for i, (name, fn) in enumerate(SFX.items()):
        rng = np.random.default_rng(1000 + i)
        s = fn(rng)
        s = fades(fft_filt(s, hp(40, 2)), 0.0005, 0.008)   # no DC, ends at zero
        s /= np.max(np.abs(s))
        s *= 0.5
        assert np.isfinite(s).all()
        r = os.path.join(WORK, name + "-raw.wav")
        write_wav_float(r, s)
        g, capped = master_sfx(r, os.path.join(HERE, name + ".wav"))
        print(f"  {name}: gain {g:+.2f} dB" + (" (capped by true peak)" if capped else ""))
    shutil.rmtree(WORK)


def verify():
    files = ["promo-music.wav"] + [k + ".wav" for k in SFX] + ["promo-music.m4a"]
    rows = []
    for f in files:
        p = os.path.join(HERE, f)
        info = json.loads(subprocess.run(
            [FFPROBE, "-v", "error", "-show_entries",
             "stream=sample_rate,channels,codec_name,duration_ts,duration:format=duration,size",
             "-of", "json", p], capture_output=True, text=True).stdout)
        st = info["streams"][0]
        e = ebur128(p, pad=(3.5 if f.startswith("sfx") else 0.0))
        err = run([FFMPEG, "-hide_banner", "-nostats", "-i", p, "-af",
                   "astats=measure_overall=Peak_level+DC_offset+Number_of_NaNs:measure_perchannel=none",
                   "-f", "null", "-"])
        pk = float(re.search(r"Peak level dB:\s*(-?[\d.inf]+)", err).group(1))
        dc = float(re.search(r"DC offset:\s*(-?[\d.e+-]+)", err).group(1))
        row = dict(file=f, codec=st["codec_name"], sr=int(st["sample_rate"]), ch=st["channels"],
                   dur=float(info["format"]["duration"]), peak_dBFS=pk, dc=dc, **e)
        if f.endswith(".wav"):
            sr, ch, x = read_wav16(p)
            row["samples"] = x.shape[1]
            row["clipped"] = int(np.sum((x == 32767) | (x == -32768)))
            row["nan"] = 0  # integer PCM cannot hold NaN; raw floats were asserted finite
            row["first_last"] = (int(np.abs(x[:, 0]).max()), int(np.abs(x[:, -1]).max()))
        rows.append(row)
    for r in rows:
        print(json.dumps(r))
    with open(os.path.join(HERE, "measurements.json"), "w") as fh:
        json.dump(rows, fh, indent=1)


if __name__ == "__main__":
    if "--verify" in sys.argv:
        verify()
    else:
        build()
