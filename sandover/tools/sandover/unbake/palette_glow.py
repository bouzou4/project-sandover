"""Reference for the engine's palette glow split (game/.../background/PaletteGlow.cpp).

Per palette entry over the fitted day and night slots s, with the level's fitted ambient A_s and
sun L_s (u8 units, 128 = 1.0): C_s = a A_s + sigma_s L_s + e, a, sigma_s >= 0, e >= 0 rgb constant.
Returns the gated glow luminance above the floor, as PaletteGlow.cpp does; the values in
test/test_palette_glow.cpp come from ``python -m sandover.unbake.palette_glow``.
"""
from __future__ import annotations

import itertools

import numpy as np

Y = np.array([0.299, 0.587, 0.114])
FIT_RMS, FLOOR, STEADY, RIDGE, ITERS, SUNLESS = 12.0, 16.0, 0.1, 1e-3, 60, 0.05


def entry(C: np.ndarray, A: np.ndarray, L: np.ndarray) -> float:
    """C, A, L: (S, 3). Glow (u8 luminance above FLOOR) of one entry."""
    C, A, L = (np.asarray(v, np.float64) for v in (C, A, L))
    S = len(C)
    if S < 2 or S > 8:
        return 0.0
    W = (C < 254).astype(np.float64)
    rng = -1.0
    for c in range(3):
        v = C[W[:, c] > 0, c]
        if len(v) >= 2:
            rng = max(rng, (v.max() - v.min()) / v.max() if v.max() > 1e-3 else 0.0)
    steady = 0.0 if rng < 0 else float(np.clip((2 * STEADY - rng) / STEADY, 0, 1))
    LL = (L * L * W).sum(1)

    def sigma(a, e):
        r = C - a * A - e
        return np.where(LL > 1e-6, np.maximum((r * L * W).sum(1) / np.maximum(LL, 1e-6), 0), 0)

    if steady <= 0:
        return 0.0
    a, sig, AA = 0.0, np.zeros(S), (A * A * W).sum()
    for _ in range(ITERS):
        a = max(((C - sig[:, None] * L) * A * W).sum() / AA, 0.0) if AA > 1e-9 else 0.0
        sig = sigma(a, 0.0)
    r = (C - a * A - sig[:, None] * L) * W
    if np.sqrt((r * r).sum() / max(W.sum(), 1.0)) <= FIT_RMS:
        return 0.0
    G = np.zeros((4, 4))
    G[0, 0] = (A * A * W).sum()
    for c in range(3):
        G[0, 1 + c] = G[1 + c, 0] = (A[:, c] * W[:, c]).sum()
        G[1 + c, 1 + c] = W[:, c].sum() + RIDGE
    sig, x = np.zeros(S), np.zeros(4)
    for _ in range(ITERS):
        R = C - sig[:, None] * L
        b = np.array([(R * A * W).sum()] + [(R[:, c] * W[:, c]).sum() for c in range(3)])
        best, cost_best = np.zeros(4), np.inf
        for k in range(1, 5):
            for sb in map(list, itertools.combinations(range(4), k)):
                try:
                    xs = np.linalg.solve(G[np.ix_(sb, sb)], b[sb])
                except np.linalg.LinAlgError:
                    continue
                if (xs < -1e-9).any():
                    continue
                cand = np.zeros(4)
                cand[sb] = xs
                rr = (R - cand[0] * A - cand[1:]) * W
                cost = (rr ** 2).sum() + RIDGE * (cand[1:] ** 2).sum()
                if cost < cost_best:
                    cost_best, best = cost, cand
        x = np.maximum(best, 0)
        sig = sigma(x[0], x[1:])
    e = x[1:].copy()
    for c in range(3):
        if W[:, c].sum() == 0:
            e[c] = max((C[:, c] - x[0] * A[:, c]).min(), 0.0)
    return float(steady * max(e @ Y - FLOOR, 0.0))


def level_splits(L: np.ndarray) -> bool:
    """The level gate: at least two slots, one of them sunless (sun under SUNLESS of the max)."""
    lum = np.asarray(L, np.float64) @ Y
    return len(lum) >= 2 and bool(lum.min() < SUNLESS * lum.max())


# Mountain Pass (ogre) fitted slots: day 0-1 with sun, night 2-3 without (test_palette_glow.cpp)
OGRE_A = [[305.4, 307.6, 311.9], [360.0, 354.2, 359.7], [193.9, 201.4, 281.1], [188.8, 222.2, 273.3]]
OGRE_L = [[1017.3, 1023.9, 1008.5], [500.6, 527.6, 518.5], [0, 0, 0], [0, 0, 0]]

if __name__ == "__main__":
    for name, c in [("red", [[208, 107, 107]] * 4), ("grey", [[132, 122, 122]] * 4),
                    ("red-clipped", [[255, 90, 90]] * 4)]:
        print(f"{name}: {entry(c, OGRE_A, OGRE_L):.2f}")
