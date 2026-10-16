"""Post-fit diagnostics: how much of the bake the directional term carries, vs an ambient-only model."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import numpy as np

from .levels import LEVELS


def ambient_only(C: np.ndarray, iters: int = 20) -> np.ndarray:
    """Rank-1 fit C[i,s,c] = a[i] * A[s,c] by alternating least squares."""
    A = C.mean(0)
    for _ in range(iters):
        a = np.maximum((C * A[None]).sum((1, 2)) / max((A * A).sum(), 1e-9), 0)
        A = (a[:, None, None] * C).sum(0) / max((a * a).sum(), 1e-9)
    return np.minimum(a[:, None, None] * A[None], 255.0)


def level_diag(out: Path, level: str) -> dict:
    f = np.load(out / f"{level}-fit.npz")
    s = json.loads((out / f"{level}-summary.json").read_text())
    m = np.isin(f["kind"], ["tfrag", "tie"])
    C = f["baked"][m].astype(np.float64)
    d = np.array([r["dir_fit"] for r in s["slots"]])
    L = np.array([r["lgt_fit_u8"] for r in s["slots"]])
    nd = np.maximum(f["normal"][m] @ d.T, 0)
    direct = (f["k"][m][:, None] * nd * f["v"][m].astype(np.float64))[..., None] * L[None]
    amb = ambient_only(C)
    e = np.abs(amb - C)
    return {"level": level,
            "directional_share": float(direct.sum() / max(f["fitted"][m].sum(), 1e-9)),
            "ambient_only_median": float(np.median(e)), "ambient_only_p90": float(np.percentile(e, 90))}


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else Path(os.environ.get("SANDOVER_DATA", "sandover/data")) / "unbake")
    rows = [level_diag(out, lvl) for lvl in LEVELS if (out / f"{lvl}-fit.npz").exists()]
    print("| level | directional share of fitted light | ambient-only median / p90 u8 |")
    print("|---|---|---|")
    for r in rows:
        print(f"| {r['level']} | {r['directional_share']:.0%} | {r['ambient_only_median']:.1f} / {r['ambient_only_p90']:.0f} |")
    (out / "diag.json").write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
