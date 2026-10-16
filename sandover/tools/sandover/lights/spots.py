"""Rank world lights by how much they would add where the baked lighting is dark (issue #191).

For every light in the baked table (``sandover-lights bake``, all census levels), the level
geometry within its radius gets the light the engine would add (the tile-light model: class
intensity x gain x colour, wrapped N.L, ``(1 - d / r)^falloff``, soft limit) and the baked
light it already has (the vertex palette at the shot hour: night slots on levels that have
them, else the noon blend; the quick slot on cave levels). Score = sum of added luma over sum of
baked luma on the lit vertices: a strong light over dark geometry ranks first.

Each ranked light gets a camera: 3-5 m from the lit patch, looking at it, turned so the emitter
sits a few degrees outside the frame (its sprite and glow stay off screen), clear of geometry and
with an unobstructed line to the patch. Particle, arc and weak lights are not candidates (their
sprites fill the frame, or they are too faint to show).

    uv run --no-sync python -m sandover.lights spots [--levels ...] [--top 20] [--yaml FILE]
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from sandover.lights.bake import CLASSES, GATES, bake_rows

HALF_FOV_X = np.radians(32.0)
ASPECT = 16.0 / 9.0
HALF_FOV_Y = np.arctan(np.tan(HALF_FOV_X) / ASPECT)
WRAP = 0.5
MIN_DIST = 0.2
MAX_ADD = 0.35
BAKED_EPS = 0.05
CANDIDATE_CLASSES = {CLASSES["strong"], CLASSES["lamp"]}
KIND_OF = {"cavecrystal": "crystal", "ecovent": "vent", "ventblue": "vent"}


def luma(c: np.ndarray) -> np.ndarray:
    return c[..., 0] * 0.299 + c[..., 1] * 0.587 + c[..., 2] * 0.114


@dataclass
class Level:
    name: str
    pos: np.ndarray  # (V,3) m
    normal: np.ndarray  # (V,3)
    baked: np.ndarray  # (V,) luma, 1.0 = texture as drawn unlit
    tri: np.ndarray  # (T,3,3) m
    hour: int
    em_w: np.ndarray  # (10,) weight of each E source at ``hour`` (Unbake.cpp unbake_emissive_weights)
    continues: list[tuple[str, np.ndarray]] = field(default_factory=list)


def shot_hour(mood: dict, sidecar_night: int) -> int:
    return 23 if sidecar_night else 12


def palette_weights(mood: dict, hour: int) -> np.ndarray:
    """Per-slot weight of the level's palette at ``hour`` (mid-hour morph)."""
    w = np.zeros(8)
    if mood.get("mode") == "quick" or not mood.get("palette_interp"):
        w[int((mood.get("quick_slot") or {}).get("lights", 0))] = 1.0
        return w
    e = mood["palette_interp"][hour]
    t = 0.5 * (e["morph_start"] + e["morph_end"])
    w[e["snapshot1"]] += 1.0 - t
    w[e["snapshot2"]] += t
    return w


def load_level(name: str, data: Path, repo: Path) -> Level | None:
    from sandover.unbake import sidecar
    from sandover.unbake.glb import load_background

    glb = data / "unbake/riproot/decompiler_out/jak1/levels" / name / f"{name}-background.glb"
    if not glb.is_file():
        return None
    mood_p = repo / "sandover/tools/data/mood" / f"{name}.json"
    mood = json.loads(mood_p.read_text(encoding="utf-8")) if mood_p.is_file() else {}
    sc = sidecar.scan_dir(data / "lighting") / f"{name}.unbake"
    hdr = sidecar.read(sc) if sc.is_file() else {}
    night, emitters = int(hdr.get("night_mask", 0)), int(hdr.get("emitter_mask", 0))
    hour = shot_hour(mood, night)
    w = palette_weights(mood, hour)
    em_w = np.zeros(10)
    for k in range(8):
        em_w[k] = w[k] if emitters & (1 << k) else 0.0
    em_w[8] = 1.0
    em_w[9] = min(1.0, sum(w[k] for k in range(8) if night & (1 << k)))
    pos, nrm, baked, tris = [], [], [], []
    for t in load_background(glb):
        cols = t.colors[:, t.color_index, :] if t.colors.shape[1] != len(t.pos) else t.colors
        c = np.tensordot(w, cols, axes=1) / 128.0
        pos.append(t.pos)
        nrm.append(t.normal)
        baked.append(luma(c))
        tris.append(t.pos[t.tris])
    if not pos:
        return None
    return Level(name, np.concatenate(pos), np.concatenate(nrm), np.concatenate(baked),
                 np.concatenate(tris), hour, em_w)


def continue_points(repo: Path) -> dict[str, list[tuple[str, np.ndarray]]]:
    src = (repo / "goal_src/jak1/engine/level/level-info.gc").read_text(encoding="utf-8")
    rx = re.compile(r"'continue-point\s+:name\s+\"([^\"]+)\"\s+:level\s+'([\w-]+)\s+:trans\s+"
                    r"\(new 'static 'vector :x ([-\d.]+) :y ([-\d.]+) :z ([-\d.]+)")
    out: dict[str, list] = {}
    for name, lv, x, y, z in rx.findall(src):
        out.setdefault(lv, []).append((name, np.array([float(x), float(y), float(z)]) / 4096.0))
    return out


def added_light(lv: Level, idx: np.ndarray, L: np.ndarray, power: np.ndarray, radius: float,
                falloff: float, intensity: float) -> np.ndarray:
    v = L - lv.pos[idx]
    d = np.linalg.norm(v, axis=1)
    n = lv.normal[idx]
    ndl = np.abs(np.einsum("ij,ij->i", n, v / np.maximum(d, 1e-6)[:, None]))  # winding normals: either side
    ndl = np.maximum((ndl + WRAP) / (1.0 + WRAP), 0.0)
    att = np.clip(1.0 - d / radius, 0.0, 1.0) ** falloff * np.clip(d / MIN_DIST, 0.0, 1.0)
    a = luma(power[None, :]) * ndl * att * intensity
    return a * MAX_ADD / (MAX_ADD + a)


def seg_blocked(tri: np.ndarray, a: np.ndarray, b: np.ndarray, skip_end: float = 0.3) -> bool:
    """Does segment a->b (minus its last ``skip_end`` m) cross any triangle?"""
    d = b - a
    ln = float(np.linalg.norm(d))
    if ln <= skip_end:
        return False
    lo, hi = np.minimum(a, b) - 0.01, np.maximum(a, b) + 0.01
    m = ~((tri.max(1) < lo).any(1) | (tri.min(1) > hi).any(1))
    t = tri[m]
    if not len(t):
        return False
    d = d / ln
    e1, e2 = t[:, 1] - t[:, 0], t[:, 2] - t[:, 0]
    p = np.cross(d, e2)
    det = np.einsum("ij,ij->i", e1, p)
    ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    s = a - t[:, 0]
    u = np.einsum("ij,ij->i", s, p) * inv
    q = np.cross(s, e1)
    v = (q @ d) * inv
    tt = np.einsum("ij,ij->i", e2, q) * inv
    hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (tt > 0.05) & (tt < ln - skip_end)
    return bool(hit.any())


def look_rot(f: np.ndarray) -> np.ndarray:
    """``inv-camera-rot`` rows (right, up, forward) for forward ``f`` with world up +y."""
    f = f / np.linalg.norm(f)
    r = np.cross([0.0, 1.0, 0.0], f)
    r /= np.linalg.norm(r)
    return np.vstack([r, np.cross(f, r), f])


def yaw(R: np.ndarray, a: float) -> np.ndarray:
    c, s = np.cos(a), np.sin(a)
    return R @ np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def angles(R: np.ndarray, cam: np.ndarray, p: np.ndarray) -> tuple[float, float, float]:
    v = R @ (p - cam)
    return float(np.arctan2(v[0], v[2])), float(np.arctan2(v[1], v[2])), float(v[2])


def place_camera(lv: Level, L: np.ndarray, patch: np.ndarray, pts: np.ndarray,
                 outside_deg: float = 6.0) -> tuple[np.ndarray, np.ndarray, int] | None:
    """Camera 3-5 m from ``patch`` looking at it, emitter ``L`` just outside the frame.
    Returns (position m, rot 3x3, lit points in view) or None."""
    best = None
    near = lv.pos[np.linalg.norm(lv.pos - patch, axis=1) < 8.0]
    for dist in (4.0, 3.0, 5.0):
        for elev in (np.radians(25), np.radians(40), np.radians(12)):
            for az in np.radians(np.arange(0, 360, 20)):
                off = np.array([np.cos(az) * np.cos(elev), np.sin(elev), np.sin(az) * np.cos(elev)])
                cam = patch + dist * off
                if len(near) and np.min(np.linalg.norm(near - cam, axis=1)) < 0.8:
                    continue
                R0 = look_rot(patch - cam)
                ax, _, _ = angles(R0, cam, L)
                target = (1.0 if ax >= 0 else -1.0) * (HALF_FOV_X + np.radians(outside_deg))
                R = min((yaw(R0, s * (target - ax)) for s in (1.0, -1.0)),
                        key=lambda m: abs(angles(m, cam, L)[0] - target))
                ax2, ay2, z2 = angles(R, cam, L)
                if z2 > 0 and abs(ax2) < HALF_FOV_X + np.radians(2.0) and abs(ay2) < HALF_FOV_Y + np.radians(2.0):
                    continue
                px, py, pz = angles(R, cam, patch)
                if pz <= 0 or abs(px) > HALF_FOV_X * 0.85 or abs(py) > HALF_FOV_Y * 0.85:
                    continue
                if seg_blocked(lv.tri, cam, patch):
                    continue
                v = (R @ (pts - cam).T).T
                inview = (v[:, 2] > 0.3) & (np.abs(np.arctan2(v[:, 0], v[:, 2])) < HALF_FOV_X) & \
                         (np.abs(np.arctan2(v[:, 1], v[:, 2])) < HALF_FOV_Y)
                n = int(inview.sum())
                if best is None or n > best[2]:
                    best = (cam, R, n)
        if best is not None and best[2] > 0:
            break
    return best


def rank(levels: dict[str, Level], rows: list[dict], cls_int: dict[int, float], falloff: float,
         intensity: float, radius_max: float, min_verts: int = 40) -> list[dict]:
    out = []
    for r in rows:
        lv = levels.get(r["level"])
        if lv is None or r["cls"] not in CANDIDATE_CLASSES:
            continue
        L = np.array(r["pos"], dtype=np.float64)
        radius = min(float(r["radius_m"]), radius_max)
        d = np.linalg.norm(lv.pos - L, axis=1)
        idx = np.nonzero((d < radius) & (d > 0.3))[0]
        if len(idx) < min_verts:
            continue
        state = 1.0
        if r["gate"] == GATES["slot"]:
            state = float(lv.em_w[r["param"]]) if 0 <= r["param"] < 10 else 0.0
        elif r["gate"] == GATES["hour"]:
            state = 0.0 if (r["param"] >> lv.hour) & 1 else 1.0
        if state < 0.5:
            continue
        power = np.array(r["colour"]) * cls_int[r["cls"]] * r["gain"] * state
        a = added_light(lv, idx, L, power, radius, falloff, intensity)
        lit = a > 0.02
        if lit.sum() < min_verts:
            continue
        b = np.maximum(lv.baked[idx][lit], 0.0)
        score = float(a[lit].sum() / (b + BAKED_EPS).sum())
        w = a[lit]
        patch = (lv.pos[idx][lit] * w[:, None]).sum(0) / w.sum()
        out.append({"row": r, "score": score, "added": float(w.mean()), "baked": float(b.mean()),
                    "lit": int(lit.sum()), "patch": patch, "pts": lv.pos[idx][lit]})
    out.sort(key=lambda x: -x["score"])
    return out


def kind(row: dict, census: dict[str, dict]) -> str:
    rec = census.get(row["id"], {})
    if row["cls"] == CLASSES["lamp"]:
        return "lamp"
    return KIND_OF.get(rec.get("type", ""), rec.get("kind", "other"))


def pick(ranked: list[dict], census: dict[str, dict], n: int = 3) -> list[dict]:
    """Best per (kind), then distinct levels, in score order."""
    chosen, kinds, lvls = [], set(), set()
    for x in ranked:
        k, lv = kind(x["row"], census), x["row"]["level"]
        if k in kinds or lv in lvls:
            continue
        chosen.append(x)
        kinds.add(k)
        lvls.add(lv)
        if len(chosen) == n:
            break
    return chosen


TEMPLE_TOP = ("  - {name: jungle-top-cell-offscreen-night, continue: jungle-start, hour: 23, camera: "
              "{pos: [1462272.0, 462848.0, -974848.0], rot: [0.0327, 0.0, -0.9994, 0.2089, 0.9779, "
              "0.0068, 0.9774, -0.209, 0.032]}}")

YAML_HEAD = """# World light list (issue #191 phase 2): spots where the baked lighting is dark but an emitter is
# clearly there. Picked by `sandover-lights spots` (score = light the world list adds / baked
# light on the geometry within the light's radius), best per kind on distinct levels; cameras
# 3-5 m from the lit patch with the emitter just outside the frame (64 degree horizontal field of
# view). The temple top keeps the power cell 18 degrees outside. Tile lights on in both runs;
# world count 0 vs 16. See kb/findings/2026-09-25-world-light-list.md.
set: world-lights
defaults:
  resolution: [2560, 1440]
  msaa: 4
  settle_frames: 90
  camera: from_continue
  effects: on
  wind: off
  particles: on
  jak: hide
shots:
"""


def shot_line(x: dict, cont: str, hour: int, cam: np.ndarray, R: np.ndarray, census: dict) -> str:
    r = x["row"]
    name = f"{r['level']}-{kind(r, census)}-{r['id'].split(':', 1)[1].replace(':', '-')}"
    name = re.sub(r"[^a-z0-9-]+", "-", name.lower()).strip("-")
    pos = ", ".join(f"{v * 4096.0:.1f}" for v in cam)
    rot = ", ".join(f"{v:.4f}" for v in R.ravel())
    note = (f"  # {r['id']} ({census.get(r['id'], {}).get('type', '?')}): score {x['score']:.2f}, "
            f"added {x['added']:.3f} over baked {x['baked']:.3f} on {x['lit']} vertices")
    return note + "\n" + f"  - {{name: {name}, continue: {cont}, hour: {hour}, camera: {{pos: [{pos}], rot: [{rot}]}}}}"


def main_spots(args) -> int:
    from sandover import paths
    from sandover.presets import emissive_light_params

    data, repo = paths.data_dir(), paths.root()
    p = emissive_light_params("default")
    cls_int = {CLASSES["strong"]: float(p["world_strong"]), CLASSES["lamp"]: float(p["world_lamp"])}
    rows, _ = bake_rows(data / "lights", args.levels)
    census = {}
    for f in sorted((data / "lights").glob("*.json")):
        if args.levels and f.stem not in args.levels:
            continue
        recs = json.loads(f.read_text(encoding="utf-8"))
        if isinstance(recs, list):
            census.update({r["id"]: r for r in recs})
    conts = continue_points(repo)
    levels = {}
    for name in sorted({r["level"] for r in rows}):
        lv = load_level(name, data, repo)
        if lv is not None:
            levels[name] = lv
            print(f"{name}: {len(lv.pos)} vertices, hour {lv.hour}", flush=True)
    ranked = rank(levels, rows, cls_int, float(p["falloff"]), float(p["intensity"]),
                  float(p["world_radius_max_m"]))
    placed = []
    for x in ranked:
        if len(placed) >= args.top:
            break
        lv = levels[x["row"]["level"]]
        got = place_camera(lv, np.array(x["row"]["pos"], dtype=np.float64), x["patch"], x["pts"])
        if got is None:
            continue
        x["cam"], x["rot"], x["inview"] = got
        placed.append(x)
    for i, x in enumerate(placed):
        r = x["row"]
        print(f"{i + 1:2d} {x['score']:6.2f} {r['id']:<40} {kind(r, census):<8} added {x['added']:.3f} "
              f"baked {x['baked']:.3f} lit {x['lit']:5d} in view {x['inview']}")
    chosen = pick(placed, census, args.pick)
    summary = [{"id": x["row"]["id"], "score": x["score"], "added": x["added"], "baked": x["baked"],
                "lit": x["lit"], "kind": kind(x["row"], census), "cam_m": x["cam"].tolist(),
                "rot": x["rot"].ravel().tolist()} for x in placed]
    out_dir = data / "lights" / "spots"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "ranking.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    lines = [YAML_HEAD.rstrip("\n")]
    hooks = []
    for x in chosen:
        lv = x["row"]["level"]
        cont = min(conts.get(lv, [("?", x["patch"])]), key=lambda c: np.linalg.norm(c[1] - x["patch"]))[0]
        lines.append(shot_line(x, cont, levels[lv].hour, x["cam"], x["rot"], census))
        if census.get(x["row"]["id"], {}).get("type") == "cavecrystal":
            hooks.append(x["row"]["id"].split(":", 1)[1])
    lines.append("  # the tower-top power cell 10.7 m away, 50 degrees off the view axis")
    lines.append(TEMPLE_TOP)
    text = "\n".join(lines) + "\n"
    print("picked:", ", ".join(x["row"]["id"] for x in chosen))
    if hooks:
        print("crystal stage hook enames:", " ".join(hooks))
    if args.yaml:
        Path(args.yaml).write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {args.yaml}")
    return 0
