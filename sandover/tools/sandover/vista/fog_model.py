"""Offline model of the game's fog against Sandover's unified fog and haze.

The game fogs linearly in view depth: the mood gives start, end, max and min per time-of-day
slot (``update-mood-fog`` lerps two slots by the hour), and ``update-math-camera`` turns them
into ``fog = hvdf.w + pzw * depth`` clamped to [min, max]; the fog amount is
``(255 - fog) / 255`` toward the fog colour. Sandover's unified fog is
``(255 - min) * (1 - exp(-od))`` with ``od`` from density, start and height falloff, and the
haze mixes toward the fog colour times a tint afterwards. ``master`` (vista option 13 off)
replaces the game's fog with the unified one; ``combine`` (option 13 on, the default) takes
max(game fog, unified + haze), with the game's part in the game's colour (``combine``).

    uv run python -m sandover.vista.fog_model curves finalboss:3@6 village1@12 jungle@12/training
    uv run python -m sandover.vista.fog_model measure A.png B.png C.png --level finalboss --slot 3

Not modelled: the per-frame overrides in mood.gc (swamp raises the minimum with camera pitch,
citadel moves the end near the silo, ogre blends in ogre3, jungleb rewrites slot 0); the
engine takes them from the live frame, this model reads the static tables.
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path

from sandover import presets
from sandover.mood_tables import OUT_DIR, evaluate_palette_hour

DISTANCES_M = [50, 100, 200, 400, 800, 1600, 3200, 6400]


@dataclass
class VanillaFog:
    start_m: float
    end_m: float
    fog_max: float
    fog_min: float
    color: tuple[float, float, float]

    def amount(self, depth_m: float) -> float:
        """0-1 fog toward the fog colour at a view depth (tfrag3.vert: 255 - clamp(...))."""
        if self.end_m <= self.start_m:
            f = self.fog_min if depth_m >= self.end_m else self.fog_max
        else:
            t = (depth_m - self.start_m) / (self.end_m - self.start_m)
            f = self.fog_max + t * (self.fog_min - self.fog_max)
        return (255.0 - min(max(f, self.fog_min), self.fog_max)) / 255.0


def vanilla(level: str, hour: float, slot: int | None = None) -> VanillaFog:
    """``slot`` forces one fog slot, as update-mood-quick does (finalboss before its movies:
    slot 3, mood.gc update-mood-finalboss)."""
    d = json.loads((OUT_DIR / f"{level}.json").read_text())
    if slot is not None or d["mode"] == "quick":
        a = b = d["quick_slot"]["fog"] if slot is None else slot
        t = 0.0
    else:
        a, b, t = evaluate_palette_hour(d["fog_interp"], hour)
    fa, fb = d["fog"][a], d["fog"][b]
    lerp = lambda x, y: x + t * (y - x)  # noqa: E731
    dists = [lerp(x, y) for x, y in zip(fa["fog_dists"], fb["fog_dists"])]
    color = tuple(lerp(x, y) for x, y in zip(fa["fog_color"][:3], fb["fog_color"][:3]))
    return VanillaFog(dists[0] / 4096, dists[1] / 4096, dists[2], dists[3], color)


@dataclass
class Unified:
    start_m: float
    density_per_km: float
    height_m: float
    haze_per_km: float
    haze_start_m: float
    haze_max: float
    haze_tint: tuple[float, float, float]

    @classmethod
    def preset(cls, name: str = "haze") -> "Unified":
        p = presets.resolve("vista", name)
        return cls(float(p["fog_start_m"]), float(p["fog_density"]), float(p["fog_height_m"]),
                   float(p.get("haze_density", 0.0)), float(p.get("haze_start_m", 200.0)),
                   float(p.get("haze_max", 1.0)), tuple(p.get("haze_tint", [1.0, 1.0, 1.0])))

    def fog(self, dist_m: float, mid_y_m: float, fog_min: float, scale: float = 1.0) -> float:
        h = math.exp(-max(mid_y_m, 0.0) / self.height_m) if self.height_m > 0 else 1.0
        od = self.density_per_km * scale / 1000 * max(dist_m - self.start_m, 0.0) * h
        return (255.0 - fog_min) / 255.0 * (1.0 - math.exp(-od))

    def haze(self, dist_m: float, scale: float = 1.0) -> float:
        k = self.haze_per_km * scale / 1000
        return self.haze_max * (1.0 - math.exp(-k * max(dist_m - self.haze_start_m, 0.0)))


def combine(van: float, uni: float, hz: float) -> tuple[float, float, float]:
    """The shaders' atmo_combine: (fog amount, haze amount, tint share of the fog layer).
    The total is max(game fog, unified + haze); the game's part keeps the game's colour and
    only the excess takes the tint and the haze colour."""
    te = 1 - (1 - uni) * (1 - hz)
    a1 = max(van, uni)
    if te <= van:
        return a1, 0.0, 0.0
    return a1, max(1 - (1 - te) / max(1 - a1, 1e-4), 0.0), (te - van) / te


def shade(base, fog_rgb, haze_rgb, f: float, h: float, tint=(1.0, 1.0, 1.0), tw: float = 1.0):
    """tfrag3.frag: mix toward the (partly tinted) fog colour, then toward the haze colour."""
    fc = [g * (1 + tw * (t - 1)) for g, t in zip(fog_rgb, tint)]
    c = [b + f * (g - b) for b, g in zip(base, fc)]
    return tuple(x + h * (y - x) for x, y in zip(c, haze_rgb))


def neighbour(name: str, level: str | None) -> dict:
    """[presets.<name>.neighbour_fog.<level>] with defaults."""
    nb = presets.resolve("vista", name).get("neighbour_fog", {}).get(level or "", {})
    return {"density": float(nb.get("density_scale", 1.0)), "tint": tuple(nb.get("tint", [1.0] * 3)),
            "haze": float(nb.get("haze_scale", 1.0))}


def curves(level: str, hour: float, mid_y_m: float = 20.0, u: Unified | None = None,
           distances: list[float] = DISTANCES_M, slot: int | None = None,
           nb: str | None = None, base=(100.0, 100.0, 80.0), preset: str = "haze") -> list[dict]:
    """Per distance (as view depth, where the two agree): vanilla fog; master (unified replaces
    it); add (first cut: floor under the unified fog, haze on top); combine (the fix).
    Each has the total fog amount and the pixel distance from vanilla's colour for ``base``.
    ``nb``: a neighbour drawn from ``level``'s camera, with its preset density, tint, haze."""
    u = u or Unified.preset(preset)
    v = vanilla(level, hour, slot)
    n = neighbour(preset, nb)
    g = v.color
    hc = tuple(min(c * t, 255.0) for c, t in zip(g, u.haze_tint))
    rows = []
    for d in distances:
        van = v.amount(d)
        uni = u.fog(d, mid_y_m, v.fog_min, n["density"])
        hz = u.haze(d, n["haze"])
        ref = shade(base, g, hc, van, 0.0)
        r = {"m": d, "vanilla": van, "unified": uni, "haze": hz}
        for mode, (f, h, tw) in {"master": (uni, hz, 1.0), "add": (max(van, uni), hz, 1.0),
                                 "combine": combine(van, uni, hz)}.items():
            px = shade(base, g, hc, f, h, n["tint"], tw)
            r[mode] = 1 - (1 - f) * (1 - h)
            r[mode + "_de"] = math.dist(px, ref)
        rows.append(r)
    return rows


def _spec(s: str) -> tuple[str, float, int | None, str | None]:
    """LEVEL[:SLOT]@HOUR[/NEIGHBOUR]"""
    s, _, nb = s.partition("/")
    level, _, hour = s.partition("@")
    level, _, slot = level.partition(":")
    return level, float(hour or 12), int(slot) if slot else None, nb or None


def cmd_curves(args) -> int:
    u = Unified.preset(args.preset)
    for spec in args.specs:
        level, hour, slot, nb = _spec(spec)
        v = vanilla(level, hour, slot)
        print(f"{level} {hour:g}h{' neighbour ' + nb if nb else ''}: game fog "
              f"{v.start_m:.0f}-{v.end_m:.0f} m, cap {(255 - v.fog_min) / 255:.2f}, "
              f"colour {tuple(round(c) for c in v.color)}")
        print("     m  vanilla | master  dE | add     dE | combine dE   (unified, haze)")
        for r in curves(level, hour, args.mid_y, u, slot=slot, nb=nb, preset=args.preset):
            print(f"{r['m']:6.0f}   {r['vanilla']:.2f}  |  {r['master']:.2f} {r['master_de']:4.0f}"
                  f" | {r['add']:.2f} {r['add_de']:4.0f} | {r['combine']:.2f} {r['combine_de']:4.0f}"
                  f"   ({r['unified']:.2f}, {r['haze']:.2f})")
    return 0


def cmd_measure(args) -> int:
    """Distance of each frame's pixels to the fog colour over the vanilla frame's fogged far
    field (C within ``--near`` of the fog colour, sky excluded as unchanged between A and C)."""
    import numpy as np
    from PIL import Image

    imgs = [np.asarray(Image.open(p).convert("RGB"), dtype=np.float32) for p in args.frames]
    a, b, c = imgs
    fog = np.array(args.fog if args.fog else vanilla(args.level, args.hour, args.slot).color,
                   np.float32)
    dist = [np.linalg.norm(x - fog, axis=-1) for x in imgs]
    if args.box:
        x0, y0, x1, y1 = args.box
        mask = np.zeros(a.shape[:2], bool)
        mask[y0:y1, x0:x1] = True
    else:
        mask = (dist[2] < args.near) & (np.abs(a - c).max(axis=-1) > args.sky_tol)
    n = int(mask.sum())
    out = {"fog_rgb": fog.tolist(), "pixels": n}
    if n:
        for name, dd, img in zip("ABC", dist, imgs):
            out[name] = {"mean_rgb": img[mask].mean(axis=0).round(1).tolist(),
                         "fog_distance": round(float(dd[mask].mean()), 2)}
        for name, dd in (("A", dist[0]), ("B", dist[1])):
            out[name]["thinner_than_C"] = round(float((dd[mask] > dist[2][mask] + args.tol).mean()), 3)
    print(json.dumps(out, indent=1))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("curves", help="fog amount vs distance: vanilla, master, add, combine")
    c.add_argument("specs", nargs="+", metavar="LEVEL[:SLOT]@HOUR[/NEIGHBOUR]")
    c.add_argument("--preset", default="haze", help="vista preset (default: the play look)")
    c.add_argument("--mid-y", type=float, default=20.0, help="mid height of the ray, metres")
    c.set_defaults(fn=cmd_curves)
    m = sub.add_parser("measure", help="far-field distance to the fog colour in A/B/C frames")
    m.add_argument("frames", nargs=3, type=Path, metavar="PNG", help="A master, B fix, C vanilla")
    m.add_argument("--level", default="village1")
    m.add_argument("--hour", type=float, default=12.0)
    m.add_argument("--slot", type=int, help="forced fog slot (finalboss fight: 3)")
    m.add_argument("--fog", type=float, nargs=3, help="fog colour override (0-255)")
    m.add_argument("--box", type=int, nargs=4, metavar=("X0", "Y0", "X1", "Y1"))
    m.add_argument("--near", type=float, default=60.0, help="C pixels this close to the fog count")
    m.add_argument("--sky-tol", type=float, default=3.0, help="A-C difference below this is sky")
    m.add_argument("--tol", type=float, default=4.0, help="RGB slack for 'thinner than C'")
    m.set_defaults(fn=cmd_measure)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
