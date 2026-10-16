"""Temporal stability of the emissive lights from a rig pan burst (issue #191, snapshot run
2026-09-25: lava and orb light flickered while the camera moved).

A shot with ``pan: {frames, yaw_deg, move_m}`` (``sandover-capture``) saves ``<name>-fNN.png``
for N consecutive frames while the camera turns, and its sidecar ``<name>.json`` carries the
engine's per-frame light lists (``pan.lights``, world metres):

- ``raw``: the candidate tiles the select pass ranked this frame (what the lights were before
  temporal tracking): ``[x, y, z, luma, fill]``;
- ``out``: the tracked tile lights the frame was lit with: ``[slot, x, y, z, luma]``;
- ``world``: the world lights drawn: ``[row, x, y, z, luma]``.

For each consecutive pair of frames this reports, luma-weighted:

- ``raw_move_m`` / ``out_move_m``: how far a light moved (raw: nearest candidate within
  ``--gate`` metres; out: the same slot);
- ``raw_churn`` / ``out_churn`` / ``world_churn``: share of the light (luma) that appeared,
  vanished or changed in power between the two frames (world: by table row): 0 = the same
  lights at the same strength;
- ``drawn_step``: relative change of the total drawn power (out + world luma), the closest
  single number to the brightness flicker on screen;
- ``pos_var_m2``: over the whole burst, the luma-weighted variance of each tracked slot's
  position (the per-light position jitter; raw candidates have no identity).

On the frames themselves (the lit geometry, independent of the light lists): the frame is cut
into a grid of boxes; each box is followed from frame to frame by normalised cross-correlation
of luma (the pan moves the content), and the luma and blue-minus-red steps of the tracked patch
are reported. ``box_step`` is the largest such luma step (0-255) over the burst and ``box_hue``
the largest blue-minus-red step: a light switching on or changing hue on a surface shows here
even when every light in the lists changes smoothly. A capture with ``--preset emissive=off``
as ``--control`` says whether a step comes from the tile lights at all.

The comparison that counts is against the same shots captured with ``--preset
emissive=unsmoothed`` (``--baseline DIR``): ``raw`` only shows the candidates the tracker was
fed, not what an untracked build drew. The true pre-change baseline is a master build.
Frames are written side by side to ``<name>-strip.png`` unless ``--no-strip``.

Usage::

    uv run --no-sync python -m sandover.lights flicker $SANDOVER_DATA/shots/light-stable \
        --baseline $SANDOVER_DATA/shots/light-stable-unsmoothed
"""

from __future__ import annotations

import json
import math
from pathlib import Path


def _d(a, b) -> float:
    return math.dist(a[:3], b[:3])


def _raw_pair(a: list, b: list, gate: float) -> tuple[float, float]:
    """(luma-weighted move of matched candidates, churn) from frame ``a`` to ``b``."""
    used, move, wsum, churn = set(), 0.0, 0.0, 0.0
    total = sum(x[3] for x in a) + sum(x[3] for x in b)
    for x in sorted(a, key=lambda r: -r[3]):
        best, bd = None, gate
        for j, y in enumerate(b):
            if j not in used and (d := _d(x, y)) < bd:
                best, bd = j, d
        if best is None:
            churn += x[3]
            continue
        used.add(best)
        w = min(x[3], b[best][3])
        move += bd * w
        wsum += w
        churn += abs(x[3] - b[best][3])
    churn += sum(y[3] for j, y in enumerate(b) if j not in used)
    return (move / wsum if wsum else 0.0), (churn / total if total else 0.0)


def _out_pair(a: list, b: list) -> tuple[float, float]:
    ia, ib = {r[0]: r[1:] for r in a}, {r[0]: r[1:] for r in b}
    total = sum(r[3] for r in ia.values()) + sum(r[3] for r in ib.values())
    move = wsum = churn = 0.0
    for k in ia.keys() | ib.keys():
        x, y = ia.get(k), ib.get(k)
        if x is None or y is None:
            churn += (x or y)[3]
            continue
        w = min(x[3], y[3])
        move += _d(x, y) * w
        wsum += w
        churn += abs(x[3] - y[3])
    return (move / wsum if wsum else 0.0), (churn / total if total else 0.0)


def _pos_var(frames: list[dict]) -> float:
    by_slot: dict[int, list[list[float]]] = {}
    for f in frames:
        for r in f["out"]:
            by_slot.setdefault(r[0], []).append(r[1:])
    num = den = 0.0
    for rows in by_slot.values():
        if len(rows) < 2:
            continue
        w = sum(r[3] for r in rows)
        if w <= 0:
            continue
        mean = [sum(r[i] * r[3] for r in rows) / w for i in range(3)]
        var = sum(r[3] * sum((r[i] - mean[i]) ** 2 for i in range(3)) for r in rows) / w
        num += var * w
        den += w
    return num / den if den else 0.0


MEAN_KEYS = ["raw_move_m", "raw_churn", "out_move_m", "out_churn", "world_churn", "drawn_step"]


def _drawn(f: dict) -> float:
    return sum(r[4] for r in f["out"]) + sum(r[4] for r in f["world"])


def analyse(frames: list[dict], gate: float = 2.0) -> dict:
    """Per-pair rows and burst means (see the module docstring)."""
    frames = sorted(frames, key=lambda f: f["frame"])
    rows = []
    for a, b in zip(frames, frames[1:]):
        rm, rc = _raw_pair(a["raw"], b["raw"], gate)
        om, oc = _out_pair(a["out"], b["out"])
        _, wc = _out_pair(a["world"], b["world"])
        da, db = _drawn(a), _drawn(b)
        rows.append({"frames": [a["frame"], b["frame"]], "raw_move_m": rm, "raw_churn": rc,
                     "out_move_m": om, "out_churn": oc, "world_churn": wc,
                     "drawn_step": abs(db - da) / max(da, db, 1e-9),
                     "raw_n": len(b["raw"]), "out_n": len(b["out"]), "world_n": len(b["world"]),
                     "drawn_luma": db})
    keys = MEAN_KEYS
    mean = {k: (sum(r[k] for r in rows) / len(rows) if rows else 0.0) for k in keys}
    return {"pairs": rows, "mean": mean, "pos_var_m2": _pos_var(frames)}


def strip(pngs: list[Path], out: Path, width: int = 480) -> Path:
    from PIL import Image

    ims = [Image.open(p).convert("RGB") for p in pngs]
    h = round(ims[0].height * width / ims[0].width)
    sheet = Image.new("RGB", (width * len(ims), h))
    for i, im in enumerate(ims):
        sheet.paste(im.resize((width, h), Image.LANCZOS), (i * width, 0))
    sheet.save(out)
    return out


BOX_GRID = (4, 3)
BOX_SEARCH = 60  # px (full resolution) each way, horizontally; a third of that vertically


def _frames(shot_dir: Path, name: str) -> list:
    import numpy as np
    from PIL import Image

    return [np.asarray(Image.open(p).convert("RGB"), np.float32)
            for p in sorted(shot_dir.glob(f"{name}-f[0-9][0-9].png"))]


def _ncc_shift(a, b, box, search: int) -> tuple[float, int, int]:
    """Best (score, dx, dy) moving ``box`` of luma image ``a`` onto ``b``."""
    import numpy as np

    x0, y0, x1, y1 = box
    p = a[y0:y1, x0:x1] - a[y0:y1, x0:x1].mean()
    pn = np.sqrt((p * p).sum()) + 1e-6
    best = (-2.0, 0, 0)
    for dy in range(-search // 3, search // 3 + 1):
        for dx in range(-search, search + 1):
            X0, Y0 = x0 + dx, y0 + dy
            if X0 < 0 or Y0 < 0 or X0 + (x1 - x0) > b.shape[1] or Y0 + (y1 - y0) > b.shape[0]:
                continue
            q = b[Y0:Y0 + y1 - y0, X0:X0 + x1 - x0]
            q = q - q.mean()
            c = float((p * q).sum() / (pn * (np.sqrt((q * q).sum()) + 1e-6)))
            if c > best[0]:
                best = (c, dx, dy)
    return best


def box_steps(frames: list, grid: tuple[int, int] = BOX_GRID, search: int = BOX_SEARCH,
              min_ncc: float = 0.5) -> list[dict]:
    """Per grid box: the tracked patch's luma and blue-minus-red per frame and their steps.
    Tracking runs on half-resolution luma; a box that leaves the frame or loses the match stops."""
    import numpy as np

    if len(frames) < 2:
        return []
    wts = np.array([0.2126, 0.7152, 0.0722], np.float32)
    lum = [f @ wts for f in frames]
    small = [l[::2, ::2] for l in lum]
    h, w = lum[0].shape
    bw, bh = w // (grid[0] + 1), h // (grid[1] + 1)
    out = []
    for gy in range(grid[1]):
        for gx in range(grid[0]):
            x0 = (gx * w) // grid[0] + (w // grid[0] - bw) // 2
            y0 = (gy * h) // grid[1] + (h // grid[1] - bh) // 2
            box = [x0, y0, x0 + bw, y0 + bh]
            lm, hue = [], []
            for i, f in enumerate(frames):
                if i > 0:
                    c, dx, dy = _ncc_shift(small[i - 1], small[i], [v // 2 for v in box], search // 2)
                    nb = [box[0] + 2 * dx, box[1] + 2 * dy, box[2] + 2 * dx, box[3] + 2 * dy]
                    if c < min_ncc or nb[0] < 0 or nb[1] < 0 or nb[2] > w or nb[3] > h:
                        break
                    box = nb
                patch = f[box[1]:box[3], box[0]:box[2]]
                lm.append(float(lum[i][box[1]:box[3], box[0]:box[2]].mean()))
                hue.append(float((patch[..., 2] - patch[..., 0]).mean()))
            dl = [b - a for a, b in zip(lm, lm[1:])]
            dh = [b - a for a, b in zip(hue, hue[1:])]
            out.append({"box": [x0, y0, x0 + bw, y0 + bh], "frames": len(lm), "luma": lm,
                        "luma_step": dl, "hue_step": dh,
                        "max_luma_step": max((abs(v) for v in dl), default=0.0),
                        "max_hue_step": max((abs(v) for v in dh), default=0.0)})
    return out


def box_summary(boxes: list[dict]) -> dict:
    if not boxes:
        return {"box_step": 0.0, "box_hue": 0.0, "worst_box": None}
    worst = max(boxes, key=lambda b: b["max_luma_step"])
    return {"box_step": worst["max_luma_step"],
            "box_hue": max(b["max_hue_step"] for b in boxes), "worst_box": worst["box"]}


def _load(shot_dir: Path, names: list[str] | None) -> dict[str, tuple[dict, dict]]:
    out = {}
    for side in sorted(shot_dir.glob("*.json")):
        try:
            meta = json.loads(side.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        pan = meta.get("pan") if isinstance(meta, dict) else None
        if pan and (not names or meta.get("name") in names):
            out[meta["name"]] = (meta, pan)
    return out


def run(shot_dir: Path, names: list[str] | None, gate: float, make_strip: bool,
        baseline: Path | None = None, control: Path | None = None, boxes: bool = True) -> int:
    shots = _load(shot_dir, names)
    base = _load(baseline, names) if baseline else {}
    for name, (_meta, pan) in shots.items():
        res = analyse(pan.get("lights") or [], gate)
        res.update({"name": name, "yaw_deg": pan.get("yaw_deg"), "move_m": pan.get("move_m")})
        if name in base:
            res["baseline"] = analyse(base[name][1].get("lights") or [], gate)["mean"]
        box_rows = []
        if boxes:
            for label, d in (("this", shot_dir), ("baseline", baseline), ("control", control)):
                if d is None:
                    continue
                bx = box_steps(_frames(d, name))
                if bx:
                    res.setdefault("boxes", {})[label] = {**box_summary(bx), "grid": bx}
                    box_rows.append((label, d.name, box_summary(bx), bx))
        if make_strip:
            pngs = sorted(shot_dir.glob(f"{name}-f[0-9][0-9].png"))
            if pngs:
                res["strip"] = strip(pngs, shot_dir / f"{name}-strip.png").name
        (shot_dir / f"{name}-flicker.json").write_text(json.dumps(res, indent=2) + "\n",
                                                       encoding="utf-8")
        print(f"{name}: yaw {pan.get('yaw_deg')} deg/frame, move {pan.get('move_m')} m/frame, "
              f"{len(pan.get('lights') or [])} frames logged")
        cols = "  ".join(f"{k:>11}" for k in MEAN_KEYS)
        print(f"  pair   {cols}  raw n  out n  world n  drawn luma")
        for r in res["pairs"]:
            vals = "  ".join(f"{r[k]:11.3f}" for k in MEAN_KEYS)
            print(f"  {r['frames'][0]:02d}-{r['frames'][1]:02d}  {vals}  {r['raw_n']:5d}  "
                  f"{r['out_n']:5d}  {r['world_n']:7d}  {r['drawn_luma']:10.4g}")
        print("  mean   " + "  ".join(f"{res['mean'][k]:11.3f}" for k in MEAN_KEYS)
              + f"   tracked position variance {res['pos_var_m2']:.4f} m^2")
        if "baseline" in res:
            print("  basel. " + "  ".join(f"{res['baseline'][k]:11.3f}" for k in MEAN_KEYS)
                  + f"   ({baseline.name})")
        for label, dname, summ, bx in box_rows:
            print(f"  lit boxes ({label}, {dname}): largest luma step {summ['box_step']:.1f}, "
                  f"largest blue-red step {summ['box_hue']:.1f}, worst box {summ['worst_box']}")
            worst = max(bx, key=lambda b: b["max_luma_step"])
            print("    worst box luma steps " + " ".join(f"{v:+.1f}" for v in worst["luma_step"]))
        if not pan.get("lights"):
            why = "; ".join(pan.get("off") or []) or "no EMLIGHTS lines at all"
            print(f"  no light lists: tile lights were not running ({why})")
    if not shots:
        print(f"no pan shots in {shot_dir}")
        return 1
    return 0
