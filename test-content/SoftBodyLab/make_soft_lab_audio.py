#!/usr/bin/env python3
"""Writes the SoftBodyLab break sounds into Content/Audio/: Thud, WoodCrack, GlassBreak, MetalClang.

Procedural and deterministic (a fixed-seed LCG for noise, no third-party samples and no libraries beyond the standard
library): mono, 16-bit PCM, 44.1 kHz, short. They are test content for the soft-body break effects
(docs/SOFTBODY.md "Break effects"), not sound design; replace them with real assets for a game. Run from anywhere:

    python make_soft_lab_audio.py
"""
import math
import os
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "Content", "Audio")
RATE = 44100


class Lcg:
    """Numerical Recipes LCG: the same noise every run."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return (self.s / 0x7FFFFFFF) - 1.0   # [-1, 1)


def write(name, samples):
    os.makedirs(OUT, exist_ok=True)
    peak = max(1e-9, max(abs(x) for x in samples))
    scale = 0.89 / peak
    path = os.path.join(OUT, name + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, x * scale)) * 32767)) for x in samples))
    print("wrote", name, "%.2f s" % (len(samples) / RATE))


def env(t, attack, decay):
    """Linear attack, exponential decay."""
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / decay)


def thud():
    """A dull impact: a falling sine around 80 Hz with a short noise tick."""
    n = int(0.45 * RATE)
    rng = Lcg(1)
    out = []
    for i in range(n):
        t = i / RATE
        f = 80.0 * math.exp(-t * 6.0) + 35.0
        body = math.sin(2 * math.pi * f * t) * env(t, 0.002, 0.09)
        tick = rng.next() * env(t, 0.0005, 0.012) * 0.5
        out.append(body + tick)
    return out


def wood_crack():
    """A dry crack: a burst of band-limited noise with a woody ring at ~420 Hz."""
    n = int(0.6 * RATE)
    rng = Lcg(2)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / RATE
        lp += (rng.next() - lp) * 0.35   # a one-pole low-pass: duller than white
        crack = lp * env(t, 0.0003, 0.02)
        ring = math.sin(2 * math.pi * 420.0 * t) * env(t, 0.001, 0.05) * 0.35 + math.sin(2 * math.pi * 760.0 * t) * env(t, 0.001, 0.03) * 0.15
        out.append(crack + ring)
    return out


def glass_break():
    """A shatter: a bright noise burst, then a scatter of high pings that thins out."""
    n = int(1.1 * RATE)
    rng = Lcg(3)
    pings = []
    for k in range(34):
        start = (rng.next() * 0.5 + 0.5) * 0.7
        freq = 2600.0 + (rng.next() * 0.5 + 0.5) * 4600.0
        amp = 0.15 + (rng.next() * 0.5 + 0.5) * 0.35
        pings.append((start, freq, amp))
    out = []
    hp_prev = 0.0
    raw_prev = 0.0
    for i in range(n):
        t = i / RATE
        raw = rng.next()
        hp = 0.92 * (hp_prev + raw - raw_prev)   # a one-pole high-pass keeps it bright
        hp_prev, raw_prev = hp, raw
        s = hp * env(t, 0.0004, 0.05) * 0.8
        for start, freq, amp in pings:
            if t >= start:
                s += math.sin(2 * math.pi * freq * (t - start)) * amp * math.exp(-(t - start) * 14.0)
        out.append(s)
    return out


def metal_clang():
    """A struck sheet: inharmonic partials with different decays."""
    n = int(1.0 * RATE)
    rng = Lcg(4)
    partials = [(233.0, 1.0, 0.45), (371.0, 0.8, 0.35), (588.0, 0.7, 0.3), (941.0, 0.5, 0.22), (1523.0, 0.4, 0.16), (2410.0, 0.25, 0.1)]
    out = []
    for i in range(n):
        t = i / RATE
        s = rng.next() * env(t, 0.0002, 0.008) * 0.6
        for freq, amp, decay in partials:
            s += math.sin(2 * math.pi * freq * t) * amp * env(t, 0.0008, decay)
        out.append(s)
    return out


if __name__ == "__main__":
    write("Thud", thud())
    write("WoodCrack", wood_crack())
    write("GlassBreak", glass_break())
    write("MetalClang", metal_clang())
