"""World-space consistency and stand-in check from ``levels.json`` + ``level_info.json``.

Writes ``adjacency.csv`` (gap or overlap per adjacent pair), ``continues.csv`` (does each
continue point sit in its own level's geometry box) and ``standins.csv`` (low-res objects
matched to the real level whose box holds them) under ``$SANDOVER_DATA/vista``.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

from sandover.census import sandover_data_dir


def box_relation(a: list, b: list) -> tuple[str, float, list[float]]:
    """Return ("gap", euclidean gap m, per-axis gaps) or ("overlap", min overlap depth, depths)."""
    alo, ahi, blo, bhi = map(np.asarray, (a[0], a[1], b[0], b[1]))
    sep = np.maximum(blo - ahi, alo - bhi)  # >0: separated along that axis
    if (sep > 0).any():
        g = np.maximum(sep, 0)
        return "gap", float(np.linalg.norm(g)), g.round(1).tolist()
    depth = -sep
    return "overlap", float(depth.min()), depth.round(1).tolist()


def point_box_distance(p, box) -> float:
    lo, hi, p = np.asarray(box[0]), np.asarray(box[1]), np.asarray(p)
    return float(np.linalg.norm(np.maximum(0, np.maximum(lo - p, p - hi))))


def seam(pa: np.ndarray, pb: np.ndarray, touch_m: float) -> tuple[float, int]:
    """Closest approach between two point clouds and how many points of ``pa`` touch ``pb``."""
    d, _ = cKDTree(pb).query(pa, k=1, distance_upper_bound=np.inf)
    return float(d.min()), int((d <= touch_m).sum())


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-vista-adjacency", description=__doc__)
    ap.add_argument("--data", type=Path, default=sandover_data_dir() / "vista")
    ap.add_argument("--touch-m", type=float, default=0.5)
    ap.add_argument("--box", default="detail_bbox_m",
                    choices=["detail_bbox_m", "core_bbox_m", "bbox_m"])
    a = ap.parse_args(argv)
    stats = {s["level"]: s for s in json.loads((a.data / "levels.json").read_text())}
    info = json.loads((a.data / "level_info.json").read_text())

    with (a.data / "adjacency.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["a", "b", "box_relation", "box_metres", "axis_x", "axis_y", "axis_z",
                    "closest_approach_m", "touching_points", "source"])
        for la, lb, src in info["adjacent_pairs"]:
            if la in stats and lb in stats:
                rel, m, axes = box_relation(stats[la][a.box], stats[lb][a.box])
                fa, fb = a.data / "points" / f"{la}.npy", a.data / "points" / f"{lb}.npy"
                gap, touch = seam(np.load(fa), np.load(fb), a.touch_m) if (
                    fa.exists() and fb.exists()) else (None, None)
                w.writerow([la, lb, rel, round(m, 1), *axes,
                            None if gap is None else round(gap, 2), touch, ";".join(src)])

    with (a.data / "continues.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["level", "continue", "x", "y", "z", "outside_own_box_m",
                    "outside_bsphere_m"])
        for name, lv in info["levels"].items():
            if name not in stats:
                continue
            bs = lv["bsphere_m"]
            for c in lv["continues"]:
                p = c["trans_m"]
                d_bs = max(0.0, float(np.linalg.norm(np.subtract(p, bs[:3]))) - bs[3])
                w.writerow([name, c["name"], *p,
                            round(point_box_distance(p, stats[name][a.box]), 1), round(d_bs, 1)])

    with (a.data / "standins.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["host", "kind", "name", "cx", "cy", "cz", "size_x", "size_y", "size_z",
                    "nearest_real_level", "dist_to_its_box_m", "median_dist_to_real_m",
                    "real_level_at_median"])
        clouds = {n: np.load(p) for n in stats if (p := a.data / "points" / f"{n}.npy").exists()}
        for host, s in stats.items():
            sp = a.data / "points" / f"{host}-standins.npz"
            samples = np.load(sp) if sp.exists() else {}
            others = [n for n in clouds if n != host]
            trees = {n: cKDTree(clouds[n]) for n in others} if samples else {}
            for i, o in enumerate(s.get("standins", [])):
                med, where = None, None
                if f"{i}" in samples:
                    # per stand-in vertex: distance to the closest real geometry of any other level
                    d = np.stack([trees[n].query(samples[f"{i}"])[0] for n in others])
                    med = round(float(np.median(d.min(axis=0))), 1)
                    where = others[int(np.bincount(d.argmin(axis=0)).argmax())]
                c = o["center_m"]
                size = np.subtract(o["bbox_m"][1], o["bbox_m"][0]).round(1).tolist()
                best = min(((point_box_distance(c, t[a.box]), n) for n, t in stats.items()
                            if n != host and a.box in t), default=(None, None))
                w.writerow([host, o["kind"], o["name"], *c, *size, best[1], round(best[0], 1),
                            med, where])
    print(f"wrote adjacency.csv, continues.csv, standins.csv to {a.data}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
