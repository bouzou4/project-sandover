"""Directional-sun + ambient model fitted to baked palette colors.

Per vertex i and sun slot s (colors in u8 steps, 128 = 1.0):
    C[i,s] = a[i] * Amb[s] + v[i,s] * k[i] * max(0, n[i] . d[s]) * Lgt[s]
Globals (d, Lgt, Amb per slot) come from scipy least_squares; the per-vertex (a, k) are
solved in closed form inside each evaluation (variable projection); v is fitted afterwards.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.optimize import least_squares

U8 = 128.0  # mood-table colors are in units where 128 u8 = 1.0


def to_angles(d: np.ndarray) -> np.ndarray:
    d = d / np.linalg.norm(d, axis=-1, keepdims=True)
    return np.stack([np.arccos(np.clip(d[..., 1], -1, 1)), np.arctan2(d[..., 2], d[..., 0])], -1)


def to_dir(ang: np.ndarray) -> np.ndarray:
    th, ph = ang[..., 0], ang[..., 1]
    return np.stack([np.sin(th) * np.cos(ph), np.cos(th), np.sin(th) * np.sin(ph)], -1)


def angle_deg(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    a = a / np.linalg.norm(a, axis=-1, keepdims=True)
    b = b / np.linalg.norm(b, axis=-1, keepdims=True)
    return np.degrees(np.arccos(np.clip((a * b).sum(-1), -1, 1)))


@dataclass
class Globals:
    d: np.ndarray  # (S,3)
    lgt: np.ndarray  # (S,3) u8 units
    amb: np.ndarray  # (S,3) u8 units

    def pack(self) -> np.ndarray:
        return np.concatenate([to_angles(self.d).ravel(), self.lgt.ravel(), self.amb.ravel()])

    @classmethod
    def unpack(cls, x: np.ndarray, S: int) -> Globals:
        return cls(to_dir(x[: 2 * S].reshape(S, 2)), x[2 * S : 5 * S].reshape(S, 3), x[5 * S :].reshape(S, 3))


def solve_ak(C: np.ndarray, lam: np.ndarray, g: Globals) -> tuple[np.ndarray, np.ndarray]:
    """Non-negative least squares for (a, k) per vertex. C (N,S,3), lam (N,S) = v * max(0, n.d)."""
    U = g.amb[None]  # (1,S,3)
    W = lam[..., None] * g.lgt[None]  # (N,S,3)
    uu = (U * U).sum((1, 2))
    uw = (U * W).sum((1, 2))
    ww = (W * W).sum((1, 2))
    uc = (U * C).sum((1, 2))
    wc = (W * C).sum((1, 2))
    det = uu * ww - uw * uw
    ok = det > 1e-9
    a = np.where(ok, (ww * uc - uw * wc) / np.where(ok, det, 1), uc / uu)
    k = np.where(ok, (uu * wc - uw * uc) / np.where(ok, det, 1), 0.0)
    a_only = np.maximum(uc / uu, 0)
    k_only = np.where(ww > 1e-9, np.maximum(wc / np.maximum(ww, 1e-9), 0), 0)
    bad_a, bad_k = a < 0, k < 0
    a, k = np.where(bad_a, 0, a), np.where(bad_a, k_only, k)
    a, k = np.where(bad_k, a_only, a), np.where(bad_k, 0, k)
    return a, k


def predict(a, k, lam, g: Globals) -> np.ndarray:
    return np.minimum(a[:, None, None] * g.amb[None] + (k[:, None] * lam)[..., None] * g.lgt[None], 255.0)


def lambert(n: np.ndarray, d: np.ndarray) -> np.ndarray:
    return np.maximum(n @ d.T, 0.0)


@dataclass
class FitResult:
    g: Globals
    sign: np.ndarray  # per component, +-1
    a: np.ndarray | None = None
    k: np.ndarray | None = None
    v: np.ndarray | None = None
    cost: float = 0.0
    nfev: int = 0


def fit_globals(C, n, g0: Globals, f_scale=8.0, max_nfev=60) -> tuple[Globals, float, int]:
    S = C.shape[1]

    def resid(x):
        g = Globals.unpack(x, S)
        lam = lambert(n, g.d)
        a, k = solve_ak(C, lam, g)
        return (predict(a, k, lam, g) - C).ravel()

    x0 = g0.pack()
    lo = np.concatenate([np.full(2 * S, -np.inf), np.zeros(6 * S)])
    hi = np.concatenate([np.full(2 * S, np.inf), np.full(6 * S, 1024.0)])
    r = least_squares(resid, np.clip(x0, lo + 1e-6, hi - 1e-6), bounds=(lo, hi), loss="soft_l1",
                      f_scale=f_scale, x_scale="jac", max_nfev=max_nfev, diff_step=1e-4)
    return Globals.unpack(r.x, S), float(r.cost), int(r.nfev)


def pick_signs(C, n, comp, g: Globals) -> np.ndarray:
    """Per connected component, keep the normal orientation that fits better."""
    err = []
    for s in (1.0, -1.0):
        lam = lambert(s * n, g.d)
        a, k = solve_ak(C, lam, g)
        e = np.abs(predict(a, k, lam, g) - C).sum((1, 2))
        err.append(np.bincount(comp, e, minlength=comp.max() + 1))
    return np.where(err[1] < err[0], -1.0, 1.0)


def fit_visibility(C, n, a, k, g: Globals, iters=3):
    """Per-vertex, per-slot occlusion v in [0,1], alternating with (a, k)."""
    nd = lambert(n, g.d)
    v = np.ones_like(nd)
    for _ in range(iters):
        W = (k[:, None] * nd)[..., None] * g.lgt[None]
        r = C - a[:, None, None] * g.amb[None]
        ww = (W * W).sum(-1)
        v = np.where(ww > 1e-6, np.clip((W * r).sum(-1) / np.maximum(ww, 1e-6), 0, 1), 1.0)
        a, k = solve_ak(C, v * nd, g)
    return a, k, v


def hemisphere(n=400) -> np.ndarray:
    """Fibonacci directions over the whole sphere (suns below the horizon are allowed)."""
    i = np.arange(n) + 0.5
    y = 1 - 2 * i / n
    r = np.sqrt(1 - y * y)
    ph = np.pi * (1 + 5**0.5) * i
    return np.stack([r * np.cos(ph), y, r * np.sin(ph)], -1)


def scan_directions(C, n, g: Globals, sweeps=2, f_scale=8.0) -> Globals:
    """Prior-free init: coordinate-wise brute-force scan of each slot's direction.

    Normal orientation is unknown at this point, so each vertex takes the better of +n / -n.
    """
    cand = hemisphere()
    d = g.d.copy()

    def vcost(nd):
        a, k = solve_ak(C, nd, g)
        r = predict(a, k, nd, g) - C
        return (2 * f_scale**2 * (np.sqrt(1 + (r / f_scale) ** 2) - 1)).sum((1, 2))

    for _ in range(sweeps):
        for j in range(len(d)):
            best, arg = np.inf, d[j]
            ndp, ndm = lambert(n, d), lambert(-n, d)
            for c in cand:
                ndp[:, j], ndm[:, j] = np.maximum(n @ c, 0), np.maximum(-n @ c, 0)
                cost = np.minimum(vcost(ndp), vcost(ndm)).sum()
                if cost < best:
                    best, arg = cost, c
            d[j] = arg
    return Globals(d, g.lgt.copy(), g.amb.copy())


def fit_level(C, n, comp, g0: Globals, rounds=2, sample=None, rng=None, blind=False) -> FitResult:
    """C (N,S,3) u8, n (N,3) geometric normals, comp (N,) component ids (0..K-1)."""
    rng = rng or np.random.default_rng(0)
    idx = np.arange(len(C)) if sample is None or sample >= len(C) else rng.choice(len(C), sample, replace=False)
    g = g0
    if blind:
        sub = idx[: min(len(idx), 5000)]
        g = scan_directions(C[sub], n[sub], g)
    cost, nfev = 0.0, 0
    for _ in range(rounds):
        sign = pick_signs(C, n, comp, g)
        ns = n * sign[comp][:, None]
        g, cost, nf = fit_globals(C[idx], ns[idx], g)
        nfev += nf
    sign = pick_signs(C, n, comp, g)
    ns = n * sign[comp][:, None]
    lam = lambert(ns, g.d)
    a, k = solve_ak(C, lam, g)
    return FitResult(g, sign, a, k, np.ones_like(lam), cost, nfev)


def errors(C, pred) -> dict:
    e = np.abs(pred - C)
    per_vtx = np.sqrt((e**2).mean((1, 2)))
    return {
        "median": float(np.median(e)),
        "p90": float(np.percentile(e, 90)),
        "max": float(e.max()),
        "explained_frac": float((per_vtx <= 4.0).mean()),
        "r2": float(1 - ((pred - C) ** 2).sum() / ((C - C.mean((0,), keepdims=True)) ** 2).sum()),
    }
