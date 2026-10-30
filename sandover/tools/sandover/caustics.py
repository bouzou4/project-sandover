"""Sunken caustics: the procedural pattern of `pbr_bg_frag.glsl` (`pbr_caustic`) and its energy.

The original lights the sunken levels' caustics through palette slots 4-7: four phases of one
looping pattern, cross-faded by `update-mood-caustics` (32 frames, weights summing to 1.25). The
engine freezes those four weights at their time mean (exact mean light, `caustic_itimes`) and adds
a zero-mean procedural term scaled, per palette entry, by the bake's caustic envelope (the mean of
slots 4-7: the caustic light), faded out where the original barely animates it (`strength` under
MASK_STRENGTH). The look's `amp` is the share of that light the web gathers into its lines (1 /
web_mean: all of it). Its spatial scale is a multiple of the
original's, per level and geometry kind (`level_cells`). This module is a bit-exact port of that
pattern (integer hash, gradient noise) so its statistics can be measured on the 8-slot rips:

    uv run sandover-caustics stats                 # both levels, every preset in caustics.toml
    uv run sandover-caustics stats --level sunken --look web
    uv run sandover-caustics moods --write         # levels, slots and cells -> CausticMoods.inc

`stats` samples points uniformly by area on the caustic-lit triangles (Gouraud envelope, as the
rasteriser sees it) and compares the original's mean and swing over its 32-frame loop with the
procedural term's over the same 0.53 s.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

import numpy as np

from sandover import presets
from sandover.paths import data_dir

LUM = np.array([0.299, 0.587, 0.114])
# 1 / standard deviation of `noise` (unit-variance field before the look's shaping)
NOISE_INV_STD = 5.236
MOOD_GC = Path("goal_src/jak1/engine/gfx/mood/mood.gc")
LEVEL_INFO_GC = Path("goal_src/jak1/engine/level/level-info.gc")
CPP_TABLE = Path("game/graphics/opengl_renderer/background/CausticMoods.inc")
# distance at which `noise` decorrelates to 0.5, in cells (`noise_half_corr`)
NOISE_HALF_CORR = 0.4375
KINDS = ("tfrag", "tie", "shrub")
# the strength byte is a / STRENGTH_RANGE x 255 (Caustics.cpp kStrengthRange)
STRENGTH_RANGE = 2.0
# relative swing under which the original counts as unanimated (pbr_bg_vert.glsl pbr_caustic_env)
MASK_STRENGTH = 0.1
# palette steps per unit vertex colour (the shaders' fragment_color: 2 x texel / 255)
PALETTE_ONE = 127.5
# texture brightness of bright stone, an assumption for `stats`' clipped share (not measured)
CLIP_T0 = 0.75
# soft limit on an 8-bit target (pbr_caustic_lit): the peak eases into 1 from LIMIT_KNEE
LIMIT_KNEE = 0.8


# ---- the pattern (mirror of pbr_bg_frag.glsl) ----

def _pcg3d(v: np.ndarray) -> np.ndarray:
    v = v.astype(np.uint32) * np.uint32(1664525) + np.uint32(1013904223)
    x, y, z = v[..., 0].copy(), v[..., 1].copy(), v[..., 2].copy()
    x += y * z
    y += z * x
    z += x * y
    x ^= x >> np.uint32(16)
    y ^= y >> np.uint32(16)
    z ^= z >> np.uint32(16)
    x += y * z
    y += z * x
    z += x * y
    return np.stack([x, y, z], axis=-1)


def noise(p: np.ndarray, seed: int) -> np.ndarray:
    """Quintic gradient noise at points p (N,3); gradients from an integer hash of the cell."""
    with np.errstate(over="ignore"):
        i = np.floor(p).astype(np.int64)
        f = p - i
        u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0)
        out = np.zeros(len(p))
        s = np.array([seed, seed * 7, seed * 13], dtype=np.int64)
        for dz in (0, 1):
            for dy in (0, 1):
                for dx in (0, 1):
                    c = np.array([dx, dy, dz])
                    h = _pcg3d(((i + c + s) & 0xFFFFFFFF).astype(np.uint32))
                    g = (h & np.uint32(0xFFFF)).astype(np.float64) / 32767.5 - 1.0
                    d = np.einsum("nc,nc->n", g, f - c)
                    w = np.prod(np.where(c == 1, u, 1.0 - u), axis=1)
                    out += w * d
    return out


def field(p_m: np.ndarray, t: float, look: dict, cell_m) -> np.ndarray:
    """Zero-mean caustic term (multiplies the masked envelope) at world points p_m (N,3, metres),
    time t, for the level cell `cell_m` (scalar or per point) times the look's `cell_scale`."""
    cell = np.asarray(cell_m, dtype=np.float64) * look["cell_scale"]
    cell = cell[:, None] if cell.ndim else cell
    q = p_m / cell
    drift = np.array([0.6, 0.3, 0.75]) * (look["drift_mps"] * t / cell)
    # the web is even in n, so its visible loop is half the phase loop
    ph = (np.pi if look["web"] > 0.0 else 2.0 * np.pi) * look["loop_hz"] * t
    n = (noise(q + drift, 0) * np.cos(ph) + noise(q - drift + 17.0, 1) * np.sin(ph)) * NOISE_INV_STD
    if look["web"] > 0.0:
        c = np.exp(-(n / look["web"]) ** 2) - look["web_mean"]
    else:
        c = n
    return look["amp"] * c


def soft_limit(p0: np.ndarray, p1: np.ndarray) -> np.ndarray:
    """Output peak of pbr_caustic_lit on an 8-bit target: without the term `p0`, with it `p1`."""
    k, r = LIMIT_KNEE, 1.0 - LIMIT_KNEE
    p = k + r * (1.0 - np.exp(-(p1 - k) / r))
    s = np.clip((p - p0) / np.where(p1 > p0, p1 - p0, 1.0), 0.0, 1.0)
    return np.where((p1 <= k) | (p1 <= p0), p1, p0 + s * (p1 - p0))


def looks() -> dict[str, dict]:
    """caustics.toml presets that turn the pattern on (mode 1)."""
    return {k: v for k, v in presets.presets("caustics").items() if v.get("mode", 1)}


# ---- the original (update-mood-caustics) ----

def vanilla_weights() -> np.ndarray:
    """Slot 4-7 weights over one 32-frame loop in steady state, (32, 4)."""
    w = np.zeros(4)
    out = []
    for s in range(64):
        v = (s >> 3) & 3
        f = 0.125 * (s & 7)
        w[(v - 1) & 3] = 1.0 - f
        w[v] = f
        if s >= 32:
            out.append(w.copy())
    return np.array(out)


def weight_cov() -> np.ndarray:
    """Covariance of the four loop weights over the loop (4, 4)."""
    return np.cov(vanilla_weights().T, bias=True)


def strength(slot_rgb: np.ndarray) -> np.ndarray:
    """Per palette entry, from the four caustic slots (4, N, 3) as bytes: the original's temporal
    RMS over its loop relative to its mean, quantised as the engine stores it (alpha byte)."""
    lum = np.einsum("snc,c->sn", slot_rgb.astype(np.float64), LUM)
    mean = lum.mean(0)
    d = lum - mean
    rms = np.sqrt(np.maximum(np.einsum("in,ij,jn->n", d, weight_cov(), d), 0.0))
    a = np.where(mean > 0.0, rms / (1.25 * np.maximum(mean, 1e-9)), 0.0)
    return np.round(np.clip(a / STRENGTH_RANGE, 0.0, 1.0) * 255.0) / 255.0 * STRENGTH_RANGE


# ---- data-derived level list ----

def caustic_moods(root: Path | None = None) -> list[tuple[str, int]]:
    """(level, first slot) for every level whose mood function calls update-mood-caustics."""
    root = root or presets.root()
    mood = (root / MOOD_GC).read_text(encoding="utf-8")
    funcs = {}
    for m in re.finditer(r"\(defun (update-mood-[\w-]+) ", mood):
        end = mood.find("\n(def", m.end())
        body = mood[m.end(): end if end > 0 else len(mood)]
        c = re.search(r"\(update-mood-caustics arg0 (\d+) ", body)
        if c and m.group(1) != "update-mood-caustics":
            funcs[m.group(1)] = int(c.group(1))
    info = (root / LEVEL_INFO_GC).read_text(encoding="utf-8")
    out = []
    for m in re.finditer(r"\(define [\w-]+\s+\(new 'static\s+'level-load-info(.*?)(?=\n\(define)", info + "\n(define",
                         re.S):
        body = m.group(1)
        name = re.search(r":name '([\w-]+)", body)
        fn = re.search(r":mood-func '([\w-]+)", body)
        if name and fn and fn.group(1) in funcs:
            out.append((name.group(1), funcs[fn.group(1)]))
    return out


def cpp_table(cells: dict[str, dict[str, float]], root: Path | None = None) -> str:
    lines = ["// Generated by sandover-caustics moods from mood.gc, level-info.gc and the 8-slot rips.",
             "// {level, first palette slot of the update-mood-caustics loop, noise cell (m) for",
             "//  tfrag, tie, shrub: the original's phase pattern decorrelates at the same distance}"]
    for lv, slot in caustic_moods(root):
        c = cells[lv]
        lines.append(f'{{"{lv}", {slot}, {{{", ".join(f"{c[k]:.1f}f" for k in KINDS)}}}}},')
    return "\n".join(lines) + "\n"


def read_cpp_cells(root: Path | None = None) -> dict[str, dict[str, float]]:
    """Cells per level and kind as written in CausticMoods.inc."""
    text = ((root or presets.root()) / CPP_TABLE).read_text(encoding="utf-8")
    out = {}
    for m in re.finditer(r'\{"([\w-]+)", (\d+), \{([^}]*)\}\}', text):
        out[m.group(1)] = dict(zip(KINDS, (float(x.strip().rstrip("f")) for x in m.group(3).split(","))))
    return out


def _half_corr(pos: np.ndarray, vals: list[np.ndarray], rng: np.random.Generator, anchors: int = 5000) -> float:
    """Distance (m) at which the values' correlation between surface points falls to 0.5."""
    from scipy.spatial import cKDTree

    edges = np.arange(0.0, 8.01, 0.25)
    mid = 0.5 * (edges[:-1] + edges[1:])
    tree = cKDTree(pos)
    a = rng.choice(len(pos), min(anchors, len(pos)), replace=False)
    nb = tree.query_ball_point(pos[a], r=edges[-1])
    i = np.concatenate([np.full(len(n), k) for k, n in zip(a, nb)])
    j = np.concatenate(nb)
    b = np.digitize(np.linalg.norm(pos[i] - pos[j], axis=1), edges) - 1
    cv = np.array([[np.corrcoef(v[i[b == k]], v[j[b == k]])[0, 1] for k in range(len(mid))]
                   for v in vals]).mean(0)
    k = int(np.argmax(cv < 0.5))
    if k == 0:
        return float(edges[-1])
    return float(mid[k - 1] + (cv[k - 1] - 0.5) / (cv[k - 1] - cv[k]) * (mid[k] - mid[k - 1]))


def noise_half_corr(n: int = 200000, seed: int = 6) -> float:
    """Distance, in cells, at which `noise` decorrelates to 0.5 (NOISE_HALF_CORR)."""
    rng = np.random.default_rng(seed)
    ds = np.arange(0.3, 0.6, 0.0125)
    cv = []
    for d in ds:
        q = rng.random((n, 3)) * 300.0
        u = rng.normal(size=q.shape)
        u /= np.linalg.norm(u, axis=1)[:, None]
        cv.append(np.corrcoef(noise(q, 0), noise(q + u * d, 0))[0, 1])
    cv = np.array(cv)
    k = int(np.argmax(cv < 0.5))
    return float(ds[k - 1] + (cv[k - 1] - 0.5) / (cv[k - 1] - cv[k]) * (ds[k] - ds[k - 1]))


def level_cells(level: str, n: int = 1500000, seed: int = 4) -> dict[str, float]:
    """Noise cell (m) per geometry kind whose half-correlation distance is the original's: the
    phase deviation (slot - envelope) / envelope of the four slots, on caustic-lit surface. A kind
    with little lit area takes the level's value."""
    from sandover.unbake.glb import load_background

    trees = load_background(_rip(level))
    rng = np.random.default_rng(seed)

    def half(sub):
        if not sub:
            return None
        pos, lum, _, _ = _samples(sub, n, rng)
        env = lum[4:8].mean(0)
        keep = env > 8.0
        if keep.sum() < 20000:
            return None
        return _half_corr(pos[keep], [(lum[4 + k][keep] - env[keep]) / env[keep] for k in range(4)], rng)

    every = half(trees)
    return {k: round((half([t for t in trees if t.kind == k]) or every) / NOISE_HALF_CORR, 1) for k in KINDS}


# ---- energy on the rips ----

def _rip(level: str) -> Path:
    return data_dir() / "unbake/riproot/decompiler_out/jak1/levels" / level / f"{level}-background.glb"


def _samples(trees, n: int, rng: np.random.Generator):
    """Area-uniform points on caustic-lit triangles: position, per-slot luminance (8,N), the
    Gouraud-interpolated masked envelope (luminance, N) with the geometry kind, lit area."""
    P, A = [], []
    for t in trees:
        lum = np.einsum("svc,c->sv", t.colors, LUM)
        es = lum[4:8].mean(0) * np.minimum(strength(t.colors[4:8]) / MASK_STRENGTH, 1.0)
        tri = t.tris
        lit = (lum[4:8][:, tri].max(axis=(0, 2)) > 1.0)
        tri = tri[lit]
        a, b, c = (t.pos[tri[:, i]] for i in range(3))
        area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
        P.append((tri, t.pos, lum, es, KINDS.index(t.kind)))
        A.append(area)
    area = np.concatenate(A)
    pick = rng.choice(len(area), size=n, p=area / area.sum())
    offs = np.cumsum([0] + [len(x) for x in A])
    pos = np.zeros((n, 3))
    lum = np.zeros((8, n))
    es = np.zeros(n)
    kind = np.zeros(n, dtype=np.int64)
    r1, r2 = rng.random(n), rng.random(n)
    s = np.sqrt(r1)
    bary = np.stack([1 - s, s * (1 - r2), s * r2], axis=1)
    for k, (tri, vp, vl, ve, kd) in enumerate(P):
        m = (pick >= offs[k]) & (pick < offs[k + 1])
        idx = tri[pick[m] - offs[k]]
        pos[m] = np.einsum("nk,nkc->nc", bary[m], vp[idx])
        lum[:, m] = np.einsum("nk,snk->sn", bary[m], vl[:, idx])
        es[m] = np.einsum("nk,nk->n", bary[m], ve[idx])
        kind[m] = kd
    return pos, lum, (es, kind), area.sum()


def stats(level: str, look_names: list[str], n: int = 60000, seed: int = 1) -> dict:
    from sandover.unbake.glb import load_background

    trees = load_background(_rip(level))
    pos, lum, (es, kind), area = _samples(trees, n, np.random.default_rng(seed))
    cells = read_cpp_cells()[level]
    cell = np.array([cells[k] for k in KINDS])[kind]
    rng = np.random.default_rng(seed + 1)
    base = lum[0] + 0.875 * lum[2]  # day slot plus the room light at its mean (check script)
    W = vanilla_weights()
    van = W @ lum[4:8]  # (32, N)
    env = lum[4:8].mean(0)
    times = np.arange(32) / 60.0
    out = {"level": level, "lit_area_m2": area, "samples": n}

    def summarise(c, limited=False):
        tot = base + c.mean(0)
        swing = c.max(0) - c.min(0)
        p1 = (base + c) / PALETTE_ONE * CLIP_T0
        if limited:
            p1 = soft_limit((base + 1.25 * env) / PALETTE_ONE * CLIP_T0, p1)
        return {"clip_share": float((p1.max(0) > 0.999).mean()),
                "clip_share_raw": float((((base + c) / PALETTE_ONE * CLIP_T0).max(0) > 0.999).mean()),
                "mean_total": float(tot.mean()), "mean_caustic": float(c.mean()),
                "caustic_share": float((c.mean(0) / np.maximum(tot, 1)).mean()),
                "swing_median": float(np.median(swing)), "swing_p90": float(np.percentile(swing, 90)),
                "swing_share_median": float(np.median(swing / np.maximum(tot, 1))),
                "swing_share_p90": float(np.percentile(swing / np.maximum(tot, 1), 90))}

    out["original"] = summarise(van)
    out["original"]["half_corr_m"] = _half_corr(pos, [van[t] - van[t + 16] for t in (0, 8)], rng)
    for name in look_names:
        look = looks()[name]
        c = np.stack([1.25 * env + 1.25 * es * np.maximum(field(pos, t, look, cell), -1.0)
                      for t in times])
        out[name] = summarise(c, limited=True)
        out[name]["half_corr_m"] = _half_corr(pos, [c[t] - c[t + 16] for t in (0, 8)], rng)
    return out


def web_mean(web: float, n: int = 400000, seed: int = 2) -> float:
    """Spatial mean of exp(-(n / web)^2) for the unit-variance noise (the look's web_mean)."""
    p = np.random.default_rng(seed).random((n, 3)) * 200.0
    v = noise(p, 0) * NOISE_INV_STD
    return float(np.exp(-(v / web) ** 2).mean())


def web_amp(web: float, n: int = 400000, seed: int = 2) -> float:
    """Amplitude at which the web gathers all the caustic light into its lines (1 / web_mean)."""
    return 1.0 / web_mean(web, n, seed)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stats", help="original vs procedural mean and swing on the rips")
    s.add_argument("--level", action="append", help="default: every caustic level")
    s.add_argument("--look", action="append", help="default: every look in caustics.toml")
    s.add_argument("--samples", type=int, default=60000)
    m = sub.add_parser("moods", help="levels, slots and cells (from the rips); --write the C++ table")
    m.add_argument("--write", action="store_true")
    sub.add_parser("noise", help="noise standard deviation and the web means of the looks")
    args = ap.parse_args()
    if args.cmd == "moods":
        table = cpp_table({lv: level_cells(lv) for lv, _ in caustic_moods()})
        print(table, end="")
        if args.write:
            (presets.root() / CPP_TABLE).write_text(table, newline="\n")
        return
    if args.cmd == "noise":
        p = np.random.default_rng(3).random((400000, 3)) * 200.0
        print(f"noise std {noise(p, 0).std():.5f} (NOISE_INV_STD {NOISE_INV_STD}, "
              f"field std {(noise(p, 0) * NOISE_INV_STD).std():.4f})")
        for name, look in looks().items():
            if look["web"] > 0:
                print(f"{name}: web {look['web']} mean {web_mean(look['web']):.4f} (preset {look['web_mean']}), "
                      f"full-gather amp {web_amp(look['web']):.3f} (preset {look['amp']})")
        print(f"half-correlation {noise_half_corr():.4f} cells (NOISE_HALF_CORR {NOISE_HALF_CORR})")
        return
    levels = args.level or [lv for lv, _ in caustic_moods()]
    names = args.look or list(looks())
    for lv in levels:
        r = stats(lv, names, args.samples)
        print(f"# {lv}: caustic-lit area {r['lit_area_m2']:,.0f} m2, {r['samples']} area samples")
        print(f"{'':10s} {'mean total':>10s} {'mean caus':>9s} {'share':>6s} {'swing med':>9s} "
              f"{'p90':>6s} {'swing/tot med':>13s} {'p90':>6s} {'half-corr m':>11s} "
              f"{'clip raw':>8s} {'8-bit':>6s}")
        for k in ["original", *names]:
            x = r[k]
            print(f"{k:10s} {x['mean_total']:10.2f} {x['mean_caustic']:9.2f} {x['caustic_share']:6.1%} "
                  f"{x['swing_median']:9.2f} {x['swing_p90']:6.2f} {x['swing_share_median']:13.1%} "
                  f"{x['swing_share_p90']:6.1%} {x['half_corr_m']:11.2f} {x['clip_share_raw']:8.2%} "
                  f"{x['clip_share']:6.2%}")
        print(f"(clip: share of lit area whose loop peak reaches white at texture {CLIP_T0} "
              f"(assumed), without and with the 8-bit soft limit)")


if __name__ == "__main__":
    main()
