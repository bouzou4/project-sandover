"""Reprojection check of a motion-vector dump (``pc-motion-vectors-dump``, MotionVectors.h).

Each dumped frame N is ``mv_NNN_colour.exr`` (scene colour before FSR 3), ``mv_NNN_vel.exr`` (r, g:
velocity in render-target pixels, ``p_prev - p_cur`` with +y up; b: source 0 camera, 1 object,
2 sky, 3 invalid frame), ``mv_NNN_aux.exr`` (r, g: camera-only velocity; b: clip w / 4096) and
``mv_NNN.json``. For every pair of consecutive game frames the previous colour is warped by the
velocity (bilinear) and compared with the current colour, once with the full vectors and once with
camera-only vectors, over the same pixels: inside the frame after the warp under both, not
disoccluded (the previous w at the warped position within ``--w-tol`` of the current w, under both
warps) and on a valid frame. Errors are the mean absolute difference of the display-encoded
colour, x 255.

``python -m sandover.rig.mvcheck <dump dir> [--json out.json] [--png-dir dir] [--mask mask.png]``

``--mask`` limits the classes to the white pixels of a PNG the size of the frame (e.g. a box
around an actor), so a figure can be read for one actor. ``--edge N`` also drops pixels within N
pixels of a source-class boundary of the current frame and pixels whose previous-frame source (at
the warped position, nearest) differs from the current one: silhouette edges and disocclusion, so
the object class reads its interior.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from sandover.hdr import read_exr, to_png


def load(d: Path, i: int) -> dict:
    base = d / f"mv_{i:03d}"
    colour, _ = read_exr(base.with_name(base.name + "_colour.exr"))
    vel, attrs = read_exr(base.with_name(base.name + "_vel.exr"))
    aux, _ = read_exr(base.with_name(base.name + "_aux.exr"))
    meta = json.loads(base.with_suffix(".json").read_text(encoding="utf-8"))
    return {"colour": colour, "vel": vel[..., :2], "src": np.rint(vel[..., 2]).astype(np.int8),
            "cam": aux[..., :2], "w": aux[..., 2] * 4096.0, "meta": meta, "attrs": attrs}


def bilinear(img: np.ndarray, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    """Sample ``img`` (H, W[, C]) at float pixel centres ``x`` (column), ``y`` (row)."""
    h, w = img.shape[:2]
    x = np.clip(x, 0, w - 1)
    y = np.clip(y, 0, h - 1)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)
    fx = (x - x0)
    fy = (y - y0)
    if img.ndim == 3:
        fx = fx[..., None]
        fy = fy[..., None]
    a = img[y0, x0] * (1 - fx) + img[y0, x1] * fx
    b = img[y1, x0] * (1 - fx) + img[y1, x1] * fx
    return a * (1 - fy) + b * fy


def warp(prev: dict, v: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Previous colour and w at each pixel's previous position, and the in-frame mask."""
    h, w = v.shape[:2]
    rows, cols = np.mgrid[0:h, 0:w].astype(np.float64)
    x = cols + v[..., 0]
    y = rows - v[..., 1]  # rows run top-down, +y is up
    inside = (x >= 0) & (x <= w - 1) & (y >= 0) & (y <= h - 1)
    return bilinear(prev["colour"], x, y), bilinear(prev["w"], x, y), inside


def edge_mask(prev: dict, cur: dict, radius: int) -> np.ndarray:
    """True where a pixel is at least ``radius`` px from a source boundary and its warped
    previous source matches."""
    import cv2

    src = cur["src"]
    k = np.ones((3, 3), np.uint8)
    s8 = src.astype(np.uint8)
    boundary = (cv2.dilate(s8, k) != cv2.erode(s8, k)).astype(np.uint8)
    near = cv2.dilate(boundary, np.ones((2 * radius + 1, 2 * radius + 1), np.uint8)) > 0
    h, w = src.shape
    rows, cols = np.mgrid[0:h, 0:w]
    x = np.clip(np.rint(cols + cur["vel"][..., 0]), 0, w - 1).astype(np.int64)
    y = np.clip(np.rint(rows - cur["vel"][..., 1]), 0, h - 1).astype(np.int64)
    return ~near & (prev["src"][y, x] == src)


def pair(prev: dict, cur: dict, w_tol: float, w_abs: float, mask: np.ndarray | None,
         edge: int = 0) -> dict:
    out = {"frame": cur["meta"]["frame"], "prev_frame": prev["meta"]["frame"],
           "valid": cur["meta"]["valid"], "cut": cur["meta"].get("cut", ""),
           "merc": {k: cur["meta"].get(f"merc_{k}") for k in ("matched", "missed", "unkeyed")}}
    if not cur["meta"]["valid"] or cur["meta"]["frame"] != prev["meta"]["frame"] + 1:
        out["skipped"] = "invalid frame" if not cur["meta"]["valid"] else "not consecutive"
        return out
    c = cur["colour"]
    keep = np.ones(c.shape[:2], bool) if mask is None else mask.copy()
    # FP16 targets can hold inf/NaN (lava overflow): such pixels have no error to measure
    keep &= np.isfinite(c).all(axis=2) & np.isfinite(prev["colour"]).all(axis=2)
    if edge > 0:
        keep &= edge_mask(prev, cur, edge)
    warped = {}
    for name, v in (("vectors", cur["vel"]), ("camera", cur["cam"])):
        col, wp, inside = warp(prev, v)
        occl = np.abs(wp - cur["w"]) > np.maximum(w_tol * cur["w"], w_abs)
        warped[name] = col
        keep &= inside & ~occl
    zero = prev["colour"]
    with np.errstate(invalid="ignore"):
        err = {k: np.abs(v - c).mean(axis=2) * 255.0 for k, v in warped.items()}
        err["none"] = np.abs(zero - c).mean(axis=2) * 255.0
    for e in err.values():
        keep &= np.isfinite(e)
    out["max_velocity_px"] = float(np.abs(cur["vel"]).max())
    classes = {"all": np.ones_like(keep), "static": cur["src"] == 0, "object": cur["src"] == 1,
               "sky": cur["src"] == 2}
    for cname, cm in classes.items():
        m = keep & cm
        n = int(m.sum())
        row = {"pixels": n, "total": int(cm.sum())}
        if n:
            for k, e in err.items():
                row[f"mae_{k}"] = round(float(e[m].mean()), 4)
        out[cname] = row
    out["_err"] = err
    out["_keep"] = keep
    return out


def heat(e: np.ndarray, gain: float = 4.0) -> np.ndarray:
    v = np.clip(e * gain / 255.0, 0, 1)
    return np.stack([v, v, v], axis=-1)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump", type=Path)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--png-dir", type=Path, help="error images per pair (vectors, camera-only)")
    ap.add_argument("--mask", type=Path, help="PNG: only its white pixels count")
    ap.add_argument("--edge", type=int, default=0,
                    help="drop pixels this close to a source boundary or with a changed source")
    ap.add_argument("--w-tol", type=float, default=0.05, help="relative w change = disocclusion")
    ap.add_argument("--w-abs", type=float, default=0.25 * 4096,
                    help="absolute w change floor (clip w units, game units for pzw 1)")
    a = ap.parse_args(argv)
    frames = sorted(int(p.name[3:6]) for p in a.dump.glob("mv_???.json"))
    mask = None
    if a.mask:
        import cv2
        m = cv2.imread(str(a.mask), cv2.IMREAD_GRAYSCALE)
        mask = m > 127
    rows = []
    prev = None
    for i in frames:
        cur = load(a.dump, i)
        if prev is not None:
            r = pair(prev, cur, a.w_tol, a.w_abs, mask, a.edge)
            if a.png_dir and "_err" in r:
                a.png_dir.mkdir(parents=True, exist_ok=True)
                for k in ("vectors", "camera"):
                    to_png(a.png_dir / f"err_{i:03d}_{k}.png", heat(r["_err"][k]))
            r.pop("_err", None)
            r.pop("_keep", None)
            rows.append(r)
            line = f"frame {r['frame']}: "
            if "skipped" in r:
                line += f"skipped ({r['skipped']} {r['cut']})"
            else:
                parts = []
                for cname in ("static", "object", "sky", "all"):
                    c = r[cname]
                    if c["pixels"]:
                        parts.append(f"{cname} {c['pixels']}px vec {c['mae_vectors']:.3f} "
                                     f"cam {c['mae_camera']:.3f} none {c['mae_none']:.3f}")
                line += "; ".join(parts) + f"; merc {r['merc']}"
            print(line)
        prev = cur
    if a.json:
        a.json.write_text(json.dumps(rows, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
