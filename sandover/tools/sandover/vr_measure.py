"""Geometry tests for the synthetic VR eye images (``sandover-vr-synth measure``).

The mono shot at the same camera is warped through each eye's logged rotation and asymmetric
window (far content; the eye offset is ignored) and registered against the swapchain image: a
correct eye leaves no residual rotation, scale or shift. Disparity rows compare the two halves
of the side-by-side shot, which share one frustum.
"""

from __future__ import annotations

import json
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image

FAKE_EYE_RE = re.compile(
    r"\[xr\] fake eye (\d): rot (\S+) (\S+) (\S+) / (\S+) (\S+) (\S+) / (\S+) (\S+) (\S+) "
    r"pos_m (\S+) (\S+) (\S+) head_x_m (\S+) tan (\S+) (\S+) (\S+) (\S+)")
FAKE_HEAD_RE = re.compile(r"\[xr\] fake head: yaw (\S+) pitch (\S+) roll (\S+) deg")
GAME_TAN_RE = re.compile(r"game frustum half-tangents x (\S+) y (\S+)")
POSES = (("nojak", (0.0, 0.0, 0.0)), ("yaw10", (10.0, 0.0, 0.0)), ("pitch10", (0.0, 10.0, 0.0)),
         ("roll10", (0.0, 0.0, 10.0)))


def fake_eyes_by_pose(log_text: str) -> dict[tuple, list[dict]]:
    """Per scripted head pose (yaw, pitch, roll): both eyes' rot (game camera -> eye), pos, tan."""
    out: dict[tuple, list] = {}
    pose = None
    for line in log_text.splitlines():
        m = FAKE_HEAD_RE.search(line)
        if m:
            pose = tuple(round(float(x), 3) for x in m.groups())
            out[pose] = [None, None]
            continue
        m = FAKE_EYE_RE.search(line)
        if m and pose is not None:
            g = [float(x) for x in m.groups()[1:]]
            out[pose][int(m.group(1))] = {"rot": np.array(g[0:9]).reshape(3, 3), "pos_m": g[9:12],
                                          "head_x_m": g[12], "tan": g[13:17]}
    return out


def gray(path: Path, scale: int) -> np.ndarray:
    im = Image.open(path).convert("L")
    if scale > 1:
        im = im.resize((im.width // scale, im.height // scale), Image.BOX)
    return np.asarray(im, dtype=np.float32) / 255.0


def warp_mono_to_eye(mono: np.ndarray, game_tan, eye: dict, shape) -> np.ndarray:
    """The mono image as the eye would see it (NaN where the mono frame has no data)."""
    from scipy.ndimage import map_coordinates
    h, w = shape
    tl, tr, tu, td = eye["tan"]
    tx = tl + (np.arange(w) + 0.5) / w * (tr - tl)  # screen right
    ty = tu - (np.arange(h) + 0.5) / h * (tu - td)  # screen up
    TX, TY = np.meshgrid(tx, ty)
    # eye ray in game camera axes (+x left) is (-tx, ty, 1); the game ray is rot^T times it
    e = np.stack([-TX, TY, np.ones_like(TX)], -1)
    c = e @ eye["rot"]
    with np.errstate(divide="ignore", invalid="ignore"):
        gx, gy = -c[..., 0] / c[..., 2], c[..., 1] / c[..., 2]
    mh, mw = mono.shape
    mu = (gx / game_tan[0] * 0.5 + 0.5) * mw - 0.5
    mv = (0.5 - gy / game_tan[1] * 0.5) * mh - 0.5
    ok = (c[..., 2] > 0) & (mu >= 0) & (mu <= mw - 1) & (mv >= 0) & (mv <= mh - 1)
    out = map_coordinates(mono, [np.where(ok, mv, 0), np.where(ok, mu, 0)], order=1)
    return np.where(ok, out, np.nan)


def edges(a: np.ndarray) -> np.ndarray:
    from scipy.ndimage import sobel
    return np.hypot(sobel(a, 0), sobel(a, 1))


def residual(pred: np.ndarray, got: np.ndarray) -> dict:
    """Best similarity (angle, scale about the centre, then shift) from predicted to rendered."""
    from scipy.ndimage import affine_transform
    mask = ~np.isnan(pred)
    p = edges(np.where(mask, pred, 0)) * mask
    g = edges(got) * mask
    h, w = p.shape
    win = np.outer(np.hanning(h), np.hanning(w))
    G = np.fft.fft2(g * win)
    best = None
    ctr = np.array([h / 2, w / 2])
    for ang in (-2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0):
        for sc in (0.98, 0.99, 1.0, 1.01, 1.02):
            a = math.radians(ang)
            m = np.array([[math.cos(a), -math.sin(a)], [math.sin(a), math.cos(a)]]) / sc
            q = affine_transform(p, m, offset=ctr - m @ ctr, order=1)
            X = G * np.conj(np.fft.fft2(q * win))
            r = np.fft.ifft2(X / (np.abs(X) + 1e-9)).real
            k = np.unravel_index(np.argmax(r), r.shape)
            dy, dx = [(k[i] + s // 2) % s - s // 2 for i, s in enumerate(r.shape)]
            if best is None or r[k] > best["score"]:
                best = {"angle_deg": float(ang), "scale": sc, "dx": int(dx), "dy": int(dy),
                        "score": float(r[k])}
    return best


def row_disparity(left: np.ndarray, right: np.ndarray, band: int = 16, maxd: int = 40) -> np.ndarray:
    """Per row band: xL - xR (px) maximising edge correlation; NaN where flat (sky)."""
    n = min(left.shape[1], right.shape[1])
    el, er = edges(left[:, :n]), edges(right[:, :n])
    out = []
    for y0 in range(0, left.shape[0] - band, band):
        a, b = el[y0:y0 + band], er[y0:y0 + band]
        if a.mean() < 0.02:
            out.append(np.nan)
            continue
        sc = [np.sum(a[:, maxd:-maxd] * np.roll(b, dd, axis=1)[:, maxd:-maxd])
              for dd in range(-maxd, maxd + 1)]
        # roll(b, dd) moves the right image by dd: the best dd is xL - xR
        out.append(float(np.argmax(sc) - maxd))
    return np.array(out)


def measure(d: Path, log_text: str, cams: list[str], scale: int) -> list[dict]:
    poses = fake_eyes_by_pose(log_text)
    gt = GAME_TAN_RE.findall(log_text)
    game_tan = (float(gt[-1][0]), float(gt[-1][1])) if gt else (0.833, 0.469)
    results = []
    for cam in cams:
        mono = gray(d / f"{cam}-mono.png", 1)
        for tag, pose in POSES:
            shot = f"{cam}-xr-{tag}"
            if not (d / f"{shot}-left.png").exists() or pose not in poses:
                continue
            for i, side in enumerate(("left", "right")):
                got = gray(d / f"{shot}-{side}.png", scale)
                r = residual(warp_mono_to_eye(mono, game_tan, poses[pose][i], got.shape), got)
                r.update({"shot": shot, "eye": side, "dx_px": r["dx"] * scale, "dy_px": r["dy"] * scale})
                results.append(r)
                print(f"{shot:28s} {side:5s} residual angle {r['angle_deg']:+.1f} deg scale "
                      f"{r['scale']:.2f} shift ({r['dx_px']:+d}, {r['dy_px']:+d}) px score {r['score']:.3f}")
        sbs = d / f"{cam}-xr-nojak.png"
        if sbs.exists():
            im = gray(sbs, scale)
            half = im.shape[1] // 2
            disp = row_disparity(im[:, :half], im[:, half:]) * scale
            n = len(disp)
            thirds = [float(np.nanmedian(disp[k * n // 3:(k + 1) * n // 3])) for k in range(3)]
            results.append({"shot": f"{cam}-xr-nojak", "row_disparity_px": [None if np.isnan(x) else x for x in disp],
                            "thirds": thirds})
            print(f"{cam}: disparity xL-xR median top/mid/bottom third "
                  + " ".join(f"{t:+.1f}" for t in thirds) + " px (shared frustum)")
    return results


def control(d: Path, log_text: str, cam: str, scale: int) -> dict:
    """Negative control: the identity prediction against the pitch-10 image must not register."""
    poses = fake_eyes_by_pose(log_text)
    gt = GAME_TAN_RE.findall(log_text)
    game_tan = (float(gt[-1][0]), float(gt[-1][1])) if gt else (0.833, 0.469)
    mono = gray(d / f"{cam}-mono.png", 1)
    got = gray(d / f"{cam}-xr-pitch10-left.png", scale)
    r = residual(warp_mono_to_eye(mono, game_tan, poses[(0.0, 0.0, 0.0)][0], got.shape), got)
    r.update({"control": "identity prediction vs pitch10 left", "dy_px": r["dy"] * scale})
    print(f"control (identity prediction vs pitch10 image): shift ({r['dx'] * scale:+d}, {r['dy'] * scale:+d}) px "
          f"score {r['score']:.3f}")
    return r


def cmd_measure(a) -> int:
    text = Path(a.log).read_text(errors="replace")
    res = measure(Path(a.dir), text, a.cam, a.downscale)
    if (Path(a.dir) / f"{a.cam[0]}-xr-pitch10-left.png").exists():
        res.append(control(Path(a.dir), text, a.cam[0], a.downscale))
    Path(a.out).write_text(json.dumps(res, indent=1))
    return 0


def rgb(path: Path, scale: int) -> np.ndarray:
    im = Image.open(path).convert("RGB")
    if scale > 1:
        im = im.resize((im.width // scale, im.height // scale), Image.BOX)
    return np.asarray(im, dtype=np.float32)


def eye_rays(eye: dict, shape) -> np.ndarray:
    """Per pixel of the eye image: the view ray in game camera axes (+x left, +y up, +z fwd)."""
    h, w = shape
    tl, tr, tu, td = eye["tan"]
    tx = tl + (np.arange(w) + 0.5) / w * (tr - tl)
    ty = tu - (np.arange(h) + 0.5) / h * (tu - td)
    TX, TY = np.meshgrid(tx, ty)
    return np.stack([-TX, TY, np.ones_like(TX)], -1) @ eye["rot"]


def sky_metrics(d: Path, log_text: str, cam: str, shot: str, scale: int) -> list[dict]:
    """Identity head: the mono shot warped into each eye, compared above the horizon.

    coverage: share of pixels that are not black (max channel > 16), mono vs eye, inside the
    mono frustum; outside_cov: the same for the eye where the mono frame has no data; agree: share
    within 48 levels of the mono pixel; far_agree: the same in the 6 degrees above the horizon.
    """
    poses = fake_eyes_by_pose(log_text)
    gt = GAME_TAN_RE.findall(log_text)
    game_tan = (float(gt[-1][0]), float(gt[-1][1])) if gt else (0.833, 0.469)
    rot = json.loads((d / f"{cam}-mono.json").read_text())["camera_requested"]["rot"]
    up_y, fwd_y, left_y = rot[4], rot[7], rot[1]
    mono = rgb(d / f"{cam}-mono.png", 1)
    out = []
    for i, side in enumerate(("left", "right")):
        eye = poses[(0.0, 0.0, 0.0)][i]
        got = rgb(d / f"{shot}-{side}.png", scale)
        pred = np.stack([warp_mono_to_eye(mono[..., k], game_tan, eye, got.shape[:2])
                         for k in range(3)], -1)
        c = eye_rays(eye, got.shape[:2])
        c = c / np.linalg.norm(c, axis=-1, keepdims=True)
        elev = np.arcsin(np.clip(c[..., 0] * left_y + c[..., 1] * up_y + c[..., 2] * fwd_y, -1, 1))
        valid = ~np.isnan(pred[..., 0])
        above = elev > 0.005
        inside, outside = valid & above, ~valid & above & (c[..., 2] > 0)
        far = inside & (elev < math.radians(6.0))
        lit_m = np.nan_to_num(pred).max(-1) > 16
        lit_e = got.max(-1) > 16
        close = np.abs(np.nan_to_num(pred) - got).max(-1) < 48
        r = {"shot": shot, "eye": side,
             "coverage_mono": float(lit_m[inside].mean()), "coverage_eye": float(lit_e[inside].mean()),
             "outside_cov": float(lit_e[outside].mean()) if outside.any() else None,
             "agree": float(close[inside].mean()), "far_agree": float(close[far].mean())}
        r["coverage_diff"] = r["coverage_eye"] - r["coverage_mono"]
        out.append(r)
        print(f"{shot:24s} {side:5s} coverage mono {r['coverage_mono']:.4f} eye {r['coverage_eye']:.4f} "
              f"(diff {r['coverage_diff']:+.4f}) outside {r['outside_cov'] if r['outside_cov'] is None else round(r['outside_cov'], 4)} "
              f"agree {r['agree']:.3f} far {r['far_agree']:.3f}")
    return out
