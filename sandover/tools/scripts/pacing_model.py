"""Timing model for Jak 1 game speed vs render rate (issue: HDR play feels sped up).

Two parts:
  1. game speed for a given sequence of render intervals, under the old
     display-frame-start rule (round up, reset below 1.3) and the new accumulator rule;
  2. render intervals produced by the HDR DXGI present path (limiter, then Present(1) on a
     DWM-composed flip chain at 143.99 Hz) vs the new pacing (Present(0), limiter paces).

Run: python pacing_model.py
"""

from __future__ import annotations

import math
import random

TICK = 1 / 60  # one game frame at target fps 60
VBLANK = 1 / 143.99


def old_step(ratio: float) -> int:
    """display-frame-start before the fix: (timer / ticks-per-frame) + 1, reset to 1 below 1.3."""
    if ratio < 1.3:
        return 1
    return min(4, math.floor(ratio) + 1)


class NewStep:
    """display-frame-start after the fix: whole game frames owed, carried across frames."""

    def __init__(self) -> None:
        self.owed = 0.0
        self.steps: list[int] = []

    def __call__(self, ratio: float) -> int:
        r = min(max(ratio, 0.0), 4.0)
        if abs(r - 1.0) < 0.02:
            self.owed = 0.9 * self.owed + 1.0
        else:
            self.owed = min(self.owed + r, 4.0)
        n = max(1, min(4, math.floor(self.owed + 0.5)))
        self.owed = max(self.owed - n, -1.0)
        self.steps.append(n)
        return n


def game_speed(intervals: list[float], rule) -> float:
    game = sum(rule(dt / TICK) for dt in intervals) * TICK
    return game / sum(intervals)


def jitter(period: float, n: int = 6000, sd: float = 0.0007, seed: int = 1) -> list[float]:
    """Loop intervals with Gaussian jitter whose sum stays n * period."""
    rng = random.Random(seed)
    t = [i * period + rng.gauss(0, sd) for i in range(n + 1)]
    t.sort()
    return [b - a for a, b in zip(t, t[1:])]


def dxgi_old(work: float, n: int = 4000, compose_vblanks: int = 1) -> list[float]:
    """Limiter (1/60 from its last exit) then Present(1) on a composed chain with max latency 1.

    Present returns once its frame is on screen: latched at the first vblank after the call,
    shown `compose_vblanks` later. The render thread then works `work` seconds before the limiter.
    """
    lim = 0.0
    out = []
    for _ in range(n):
        latch = math.ceil(lim / VBLANK + 1e-9) * VBLANK
        ret = latch + compose_vblanks * VBLANK
        nxt = max(lim + TICK, ret + work)
        out.append(nxt - lim)
        lim = nxt
    return out


def dxgi_new(work: float, n: int = 4000) -> list[float]:
    """Limiter then Present(0) without tearing: nothing blocks, the limiter paces the loop."""
    return [max(TICK, work)] * n


def main() -> None:
    print("1. game speed (game seconds per wall second) vs steady render fps, with and without jitter")
    print(f"{'fps':>6} {'old':>6} {'new':>6}")
    print("   60 and 59.94 fps must use only 1-steps (no hitches); 45-59 fps steps are uneven (1,1,2)")
    for fps in (20, 24, 30, 36, 40, 45, 48, 50, 55, 58, 59.94, 60):
        for sd in (0.0007, 0.0):
            iv = jitter(1 / fps, n=60000, sd=sd)
            rule = NewStep()
            sp = game_speed(iv, rule)
            steps = {k: rule.steps.count(k) for k in sorted(set(rule.steps))}
            head = "".join(map(str, rule.steps[100:112]))
            print(f"{fps:>6} {game_speed(iv, old_step):6.3f} {sp:6.3f}  jitter {sd * 1e3:.1f} ms,"
                  f" step counts {steps}, sequence {head}")

    print("\n   20.8/27.8 ms mix averaging 40 fps (3 and 4 vblanks at 143.99 Hz)")
    rng = random.Random(2)
    mix = [(4 if rng.random() < 0.6 else 3) * VBLANK for _ in range(6000)]
    fps = len(mix) / sum(mix)
    print(f"   fps {fps:.1f}: old {game_speed(mix, old_step):.3f}, new {game_speed(mix, NewStep()):.3f}")

    print("\n2. render fps on the HDR DXGI path, limiter 60, 143.99 Hz, vs render work per frame")
    print(f"{'work ms':>8} {'old +1':>7} {'old +0':>7} {'new':>6}  old game speed (+1)")
    for w_ms in (2, 4, 6, 8, 9, 10, 11, 12, 14):
        w = w_ms / 1000
        a = dxgi_old(w, compose_vblanks=1)
        b = dxgi_old(w, compose_vblanks=0)
        c = dxgi_new(w)
        f = lambda iv: len(iv) / sum(iv)
        print(f"{w_ms:>8} {f(a):7.1f} {f(b):7.1f} {f(c):6.1f}  {game_speed(a, old_step):.3f}")

    print("\n3. target fps above 60 (custom video mode), rendering at target: seconds per frame")
    print("   old adds (int time-factor) = int(300 / fps) ticks; new carries the fraction")
    for fps in (60, 75, 90, 100, 120, 144, 165):
        old = int(300 / fps) * fps / 300
        print(f"{fps:>6} old {old:.3f} new 1.000")


if __name__ == "__main__":
    main()
