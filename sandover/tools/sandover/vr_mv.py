"""Per-eye motion vectors against the geometric prediction of a scripted head step
(``sandover-vr-synth mvstep``).

The dump (``pc-motion-vectors-dump`` during an XR frame) writes ``mv_NNN_e<eye>`` per eye: the
velocity (``v = p_prev - p_cur`` in render pixels, GL texel grid, +y up) and the aux image whose
blue channel is the clip w / 4096 (0 on the sky); the JSON's ``w_per_unit`` turns clip w into
view depth in game units. For the frame whose head
pose changed, each pixel's previous position is predicted from its own depth and the two logged
eye poses (``[xr] fake eye`` lines: game camera -> eye rotation, eye offset in metres), without
the engine's matrices, and compared with the dumped vector.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import numpy as np

EYE_RE = re.compile(r"\[xr\] fake eye (\d): rot (\S+) (\S+) (\S+) / (\S+) (\S+) (\S+) / (\S+) (\S+) (\S+) "
                    r"pos_m (\S+) (\S+) (\S+) head_x_m (\S+) tan (\S+) (\S+) (\S+) (\S+)")
HEAD_RE = re.compile(r"\[xr\] fake head: yaw (\S+) pitch (\S+) roll (\S+) deg")
TAN_RE = re.compile(r"game frustum half-tangents x (\S+) y (\S+) -> eye scale x (\S+) y (\S+)")
UNITS_PER_M = 4096.0


def eye_poses(log: str) -> dict[tuple, list]:
    """Per scripted head pose (yaw, pitch, roll), in log order: (rot, offset in game units) per eye."""
    out: dict[tuple, list] = {}
    cur = None
    for line in log.splitlines():
        m = HEAD_RE.search(line)
        if m:
            cur = tuple(round(float(x), 3) for x in m.groups())
            out.setdefault(cur, [None, None])
            continue
        m = EYE_RE.search(line)
        if m and cur is not None:
            g = [float(x) for x in m.groups()[1:]]
            out[cur][int(m.group(1))] = (np.array(g[0:9]).reshape(3, 3), np.array(g[9:12]) * UNITS_PER_M)
    return out


def render_tans(log: str) -> tuple[float, float]:
    """Half-tangents of the drawn (symmetric) eye frame: the game's over the eye scale."""
    gtx, gty, fx, fy = map(float, TAN_RE.findall(log)[-1])
    return gtx / fx, gty / fy


def predict(w: int, h: int, rtx: float, rty: float, depth: np.ndarray, before, after) -> np.ndarray:
    """Predicted ``p_prev - p_cur`` (px) for each pixel of the frame drawn with eye pose ``after``
    when the previous frame used ``before``. ``depth``: view depth (game units), <= 0 for the sky
    (a direction: rotation only). Rows in the GL texel grid (row 0 at the bottom)."""
    rot_a, t_a = before
    rot_b, t_b = after
    px = ((np.arange(w) + 0.5) / w * 2 - 1) * rtx
    py = ((np.arange(h) + 0.5) / h * 2 - 1) * rty
    tx, ty = np.meshgrid(px, py)
    sky = depth <= 0
    d = np.where(sky, 1.0, depth)
    e = np.stack([-tx * d, ty * d, d], -1)  # eye axes: +x left, +y up, +z forward
    c = e @ rot_b + np.where(sky[..., None], 0.0, t_b)  # game camera space
    q = (c - np.where(sky[..., None], 0.0, t_a)) @ rot_a.T  # the previous eye's space
    pxa = (-q[..., 0] / q[..., 2] / rtx + 1) / 2 * w - 0.5
    pya = (q[..., 1] / q[..., 2] / rty + 1) / 2 * h - 0.5
    gx, gy = np.meshgrid(np.arange(w), np.arange(h))
    return np.stack([pxa - gx, pya - gy], -1)


def _read(path: Path) -> np.ndarray:
    from sandover.hdr import read_exr
    return read_exr(path)[0][::-1].astype(np.float64)  # GL row order


def check_dump(dump: Path, log: str, step: tuple[tuple, tuple], centre: int = 32,
               min_speed: float = 1.0) -> list[dict]:
    """One row per dumped eye frame; the step frame (median speed above ``min_speed`` px) gets the
    error against the prediction from the step's two head poses ((yaw, pitch, roll) before, after)."""
    poses = eye_poses(log)
    keys = [tuple(round(float(x), 3) for x in k) for k in step]
    rtx, rty = render_tans(log)
    rows = []
    for js in sorted(Path(dump).glob("mv_*_e*.json")):
        meta = json.loads(js.read_text())
        stem = str(js)[:-5]
        vel = _read(Path(stem + "_vel.exr"))
        h, w = vel.shape[:2]
        eye = meta["eye"]
        speed = float(np.median(np.hypot(vel[..., 0], vel[..., 1])))
        row = {"file": js.name, "eye": eye, "tick": meta.get("tick"), "valid": meta["valid"],
               "median_speed_px": speed}
        if speed > min_speed and meta["valid"]:
            # aux b is clip w / 4096; clip w is view depth times w_per_unit
            depth = _read(Path(stem + "_aux.exr"))[..., 2] * 4096.0 / meta["w_per_unit"]
            pred = predict(w, h, rtx, rty, depth, poses[keys[-2]][eye], poses[keys[-1]][eye])
            err = np.hypot(vel[..., 0] - pred[..., 0], vel[..., 1] - pred[..., 1])
            cy, cx = h // 2, w // 2
            box = err[cy - centre:cy + centre, cx - centre:cx + centre]
            ok = np.isfinite(err)
            row.update({
                "poses": [keys[-2], keys[-1]],
                "pred_centre_px": [round(float(pred[cy, cx, 0]), 3), round(float(pred[cy, cx, 1]), 3)],
                "got_centre_px": [round(float(vel[cy, cx, 0]), 3), round(float(vel[cy, cx, 1]), 3)],
                "centre_err_mean_px": float(box.mean()), "centre_err_max_px": float(box.max()),
                "frame_err_median_px": float(np.median(err[ok])),
                "frame_err_p95_px": float(np.percentile(err[ok], 95)),
                "sky_share": float((depth <= 0).mean()),
            })
        rows.append(row)
    return rows


def cmd_mvstep(a) -> int:
    log = Path(a.log).read_text(encoding="utf-8", errors="replace")
    step = (tuple(map(float, a.before.split(","))), tuple(map(float, a.after.split(","))))
    rows = check_dump(Path(a.dump), log, step, centre=a.centre)
    for r in rows:
        print(json.dumps(r))
    if a.out:
        Path(a.out).write_text(json.dumps(rows, indent=1), encoding="utf-8")
    steps = [r for r in rows if "centre_err_max_px" in r]
    return 0 if steps and all(r["centre_err_max_px"] < 0.5 for r in steps) else 1
