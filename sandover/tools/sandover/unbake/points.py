"""Point-light clusters for effect slots (flames, lamps, lava).

C[i] = a[i] * Amb + sum_j I_j * max(0, n.l) / (r^2 + R0^2) * w(r / R_j), with w = 1 for the plain
inverse-square model and w = max(0, 1 - (r/R)^2)^2 for the windowed one.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.optimize import least_squares
from sklearn.cluster import DBSCAN

R0 = 0.5  # softening radius (m) so r^2 never reaches 0


@dataclass
class PointFit:
    pos: np.ndarray  # (J,3)
    rgb: np.ndarray  # (J,3) intensity, u8 * m^2
    radius: np.ndarray  # (J,) window radius (inf for plain inverse square)
    amb: np.ndarray  # (3,)
    pred: np.ndarray  # (N,3)


def one_light(x, n, p, c, rad):
    dv = p[None] - x
    r2 = (dv * dv).sum(1)
    ndl = np.maximum((n * dv).sum(1) / np.sqrt(r2 + 1e-9), 0)
    w = np.maximum(0, 1 - r2 / rad**2) ** 2 if np.isfinite(rad) else 1.0
    return (ndl * w / (r2 + R0 * R0))[:, None] * c[None]


def seed_clusters(x, Y, thr=12.0, eps=2.0, min_samples=6, max_lights=64):
    """DBSCAN on vertices brighter than thr; returns seed positions (J,3), brightest clusters first."""
    hot = np.flatnonzero(Y > thr)
    if len(hot) < min_samples:
        return np.zeros((0, 3))
    lab = DBSCAN(eps=eps, min_samples=min_samples).fit_predict(x[hot])
    seeds = []
    for l in np.unique(lab[lab >= 0]):
        m = hot[lab == l]
        w = Y[m] - thr
        seeds.append(((x[m] * w[:, None]).sum(0) / w.sum(), w.sum()))
    seeds.sort(key=lambda s: -s[1])
    return np.array([s[0] for s in seeds[:max_lights]]).reshape(-1, 3)


def fit_points(x, n, C, a, seeds, windowed=True, passes=2, reach=20.0, f_scale=8.0) -> PointFit:
    """Fit each light locally (block coordinate descent) against the residual of the others."""
    J = len(seeds)
    amb = np.maximum(np.median(C / np.maximum(a[:, None], 0.05), 0), 0)
    full = np.minimum(a[:, None] * amb[None], 255.0)
    keep = np.zeros(len(x), bool)
    for sd in seeds:
        keep |= ((x - sd) ** 2).sum(1) < reach**2
    ki = np.flatnonzero(keep)
    x, n, C, base = x[ki], n[ki], C[ki], full[ki]
    pos = seeds + np.array([0, 0.5, 0])
    rgb = np.tile(np.maximum(C.max(0), 16) * 4.0, (J, 1))
    radius = np.full(J, 8.0 if windowed else np.inf)
    contrib = [one_light(x, n, pos[j], rgb[j], radius[j]) for j in range(J)]
    total = base + sum(contrib) if J else base
    for _ in range(passes):
        for j in range(J):
            near = np.flatnonzero(((x - seeds[j]) ** 2).sum(1) < reach**2)
            if len(near) < 8:
                continue
            xs, ns = x[near], n[near]
            other = total[near] - contrib[j][near]
            target = C[near]

            def resid(p):
                rad = p[6] if windowed else np.inf
                return (np.minimum(other + one_light(xs, ns, p[:3], p[3:6], rad), 255.0) - target).ravel()

            p0 = np.concatenate([pos[j], rgb[j], [radius[j]] if windowed else []])
            lo = np.concatenate([seeds[j] - 6, np.zeros(3), [1.0] if windowed else []])
            hi = np.concatenate([seeds[j] + 6, np.full(3, 1e5), [reach] if windowed else []])
            r = least_squares(resid, np.clip(p0, lo + 1e-6, hi - 1e-6), bounds=(lo, hi), loss="soft_l1",
                              f_scale=f_scale, x_scale="jac", max_nfev=60)
            pos[j], rgb[j] = r.x[:3], r.x[3:6]
            if windowed:
                radius[j] = r.x[6]
            total = total - contrib[j]
            contrib[j] = one_light(x, n, pos[j], rgb[j], radius[j])
            total = total + contrib[j]
    full[ki] = np.minimum(total, 255.0)
    return PointFit(pos, rgb, radius, amb, full)
