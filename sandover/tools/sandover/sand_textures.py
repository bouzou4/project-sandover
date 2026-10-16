"""Sand textures: which background textures are sand, from the level's own collision data.

Writes ``game/graphics/opengl_renderer/SandTextures.inc`` (committed, compiled into gk like
``WorldLights.inc``). The background class shading reads it as bit 0x20 of the class byte outside
the swamp (``Materials.cpp``): wet sand stays matte where wet rock and wood take a smoother
roughness (``pbr_bg_frag.glsl``, wet-to-PBR flag 16 of ``sw_flags``). Nothing is tagged by hand:

- Join: every ground-facing background triangle (|normal y| > 0.7) near the sea (centroid y in
  -1..3 m, where the shoreline is wet) is matched to the nearest collision triangle (3D centroid within 1.5 m) of the level's
  collide mesh in ``out/jak1/fr3/<level>.fr3``; the collision triangle's pat material
  (``sand``, ``stone``, ``wood``...) is what Jak walks on there.
- A texture is sand when at least ``MIN_SHARE`` of its matched area, and ``MIN_AREA_M2`` of it,
  lies on ``sand`` pat.
- A texture with the same derived-map statistics (size, roughness, slope percentiles: the same
  source art under another name) as a sand texture is sand too; the jungle's beach is the
  village beach art on ``stone`` pat.

The texture's derived roughness cannot separate them: sand measures smoother than the rock
(``rough_mean`` 0.879 against 0.93-0.96), so roughness alone would make sand the glossiest.

Run (needs ``zstandard`` for the .fr3)::

    uv run --no-sync --with zstandard python -m sandover.sand_textures write
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

from sandover import paths

CPP_TABLE_REL = "game/graphics/opengl_renderer/SandTextures.inc"
LEVELS = ("beach", "training", "village1", "village2", "misty", "jungle")
PAT_MATERIALS = ["stone", "ice", "quicksand", "waterbottom", "tar", "sand", "wood", "grass",
                 "pcmetal", "snow", "deepsnow", "hotcoals", "lava", "crwood", "gravel", "dirt",
                 "metal", "straw", "tube", "swamp", "stopproj", "rotate", "neutral"]
BAND_Y = (-1.0, 3.0)
MATCH_M = 1.5
MIN_UP = 0.7  # ground-facing triangles only: a trunk or wall standing on sand is not sand
MIN_SHARE = 0.8
MIN_AREA_M2 = 50.0
ART_KEYS = ("width", "height", "rough_mean", "slope_p50_deg", "slope_p95_deg")


def is_sand(pat_area: Counter, min_share: float = MIN_SHARE, min_area: float = MIN_AREA_M2) -> bool:
    """True if ``pat_area`` (pat material -> matched area, m2) is mostly sand."""
    total = sum(v for k, v in pat_area.items() if k != "none")
    return total >= min_area and pat_area.get("sand", 0.0) >= min_share * total


def same_art(index: dict[str, dict], sand: set[str]) -> set[str]:
    """Texture names whose derived-map statistics equal those of a texture in ``sand``."""
    key = lambda v: tuple(v.get(k) for k in ART_KEYS)  # noqa: E731
    sand_keys = {key(v) for v in index.values() if v.get("name") in sand}
    return {v["name"] for v in index.values() if key(v) in sand_keys and v.get("name")}


def load_collide(level: str) -> tuple[np.ndarray, np.ndarray]:
    """Collision triangles (n, 3, 3) in metres and their pat material index."""
    tris, pat = load_collide_pat(paths.root() / "out/jak1/fr3" / f"{level}.fr3")
    return tris, (pat >> 6) & 0x3F


def load_collide_pat(fr3: Path) -> tuple[np.ndarray, np.ndarray]:
    """Collision triangles (n, 3, 3) in metres and their raw pat-surface words, from a level's
    .fr3: the CollisionMesh vector is found as the longest run of 32-byte vertex records (float
    xyz, zero flags, unit int16 normal) preceded by its u64 count."""
    import zstandard

    raw = Path(fr3).read_bytes()
    size = struct.unpack_from("<Q", raw, 0)[0]
    buf = zstandard.ZstdDecompressor().decompress(raw[8:], max_output_size=size)
    best = None
    for ph in range(32):
        n = (len(buf) - ph) // 32
        rec = np.frombuffer(buf, dtype=np.uint8, count=n * 32, offset=ph).reshape(n, 32)
        flags = rec[:, 12:16].view(np.uint32)[:, 0]
        nrm = rec[:, 16:22].view(np.int16).astype(np.float64)
        pad = rec[:, 22:24].view(np.uint16)[:, 0]
        ok = (flags == 0) & (pad == 0) & (np.abs(np.sqrt((nrm ** 2).sum(1)) - 32767) < 40)
        d = np.diff(np.r_[0, ok.astype(np.int8), 0])
        starts, ends = np.where(d == 1)[0], np.where(d == -1)[0]
        if len(starts):
            i = np.argmax(ends - starts)
            if best is None or ends[i] - starts[i] > best[2]:
                best = (ph, starts[i], ends[i] - starts[i])
    if best is None:
        raise RuntimeError(f"{Path(fr3).stem}: no collision mesh found")
    ph, s, run = best
    off = ph + s * 32
    for k in range(2_000_000):
        o = off - 32 * k
        cnt = struct.unpack_from("<Q", buf, o - 8)[0]
        if k + run <= cnt <= 50_000_000 and o + cnt * 32 <= len(buf):
            off = o
            break
    else:
        raise RuntimeError(f"{Path(fr3).stem}: no collision vertex count")
    if cnt % 3:
        raise RuntimeError(f"{Path(fr3).stem}: collision vertex count {cnt} is not triangles")
    rec = np.frombuffer(buf, dtype=np.uint8, count=cnt * 32, offset=off).reshape(cnt, 32)
    pos = rec[:, 0:12].copy().view(np.float32).reshape(-1, 3).astype(np.float64) / 4096.0
    pat = rec[:, 24:28].copy().view(np.uint32)[:, 0]
    return pos.reshape(-1, 3, 3), pat.reshape(-1, 3)[:, 0]


def background_tris(level: str):
    """(texture name, a, b, c) per primitive of the level's background glb, metres."""
    from pygltflib import GLTF2

    path = paths.data_dir() / f"unbake/riproot/decompiler_out/jak1/levels/{level}/{level}-background.glb"
    g = GLTF2().load(str(path))
    blob = g.binary_blob()

    def acc(i):
        a = g.accessors[i]
        bv = g.bufferViews[a.bufferView]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a.type]
        dt = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[a.componentType]
        off = (bv.byteOffset or 0) + (a.byteOffset or 0)
        isz = np.dtype(dt).itemsize
        if bv.byteStride and bv.byteStride != n * isz:
            raw = np.frombuffer(blob, np.uint8, count=bv.byteStride * a.count, offset=off)
            return raw.reshape(a.count, bv.byteStride)[:, :n * isz].copy().view(dt).reshape(a.count, n)
        x = np.frombuffer(blob, dt, count=a.count * n, offset=off)
        return x.reshape(a.count, n) if n > 1 else x

    for node in g.nodes:
        if node.mesh is None:
            continue
        for pr in g.meshes[node.mesh].primitives:
            if pr.material is None or pr.mode not in (None, 4):
                continue
            tex = (g.materials[pr.material].name or "?").split(".")[0]
            P = acc(pr.attributes.POSITION).astype(np.float64)
            idx = acc(pr.indices).astype(np.int64).reshape(-1, 3)
            yield tex, P[idx[:, 0]], P[idx[:, 1]], P[idx[:, 2]]


def pat_areas(level: str) -> dict[str, Counter]:
    """Texture -> pat material -> background area (m2) near the sea standing on it."""
    from scipy.spatial import cKDTree

    ctris, mats = load_collide(level)
    tree = cKDTree(ctris.mean(1))
    out: dict[str, Counter] = defaultdict(Counter)
    for tex, a, b, c in background_tris(level):
        cen = (a + b + c) / 3
        cr = np.cross(b - a, c - a)
        ln = np.linalg.norm(cr, axis=1)
        up = np.abs(cr[:, 1]) > MIN_UP * np.maximum(ln, 1e-12)
        m = (cen[:, 1] > BAND_Y[0]) & (cen[:, 1] < BAND_Y[1]) & up
        if not m.any():
            continue
        area = 0.5 * ln[m]
        dist, idx = tree.query(cen[m])
        for d, i, w in zip(dist, idx, area):
            k = PAT_MATERIALS[mats[i]] if d < MATCH_M and mats[i] < len(PAT_MATERIALS) else "none"
            out[tex][k] += float(w)
    return out


def derive(levels=LEVELS) -> tuple[list[str], dict[str, dict]]:
    """Sand texture names (sorted) and the per-texture evidence."""
    report: dict[str, dict] = {}
    sand: set[str] = set()
    for lv in levels:
        for tex, cnt in pat_areas(lv).items():
            total = sum(v for k, v in cnt.items() if k != "none")
            if total < MIN_AREA_M2:
                continue
            share = cnt.get("sand", 0.0) / total
            report.setdefault(tex, {})[lv] = {"matched_m2": round(total), "sand_share": round(share, 3)}
            if is_sand(cnt):
                sand.add(tex)
    index = json.loads((paths.data_dir() / "materials/pbr-maps/index.json").read_text())["textures"]
    for name in sorted(same_art(index, sand) - sand):
        report.setdefault(name, {})["same_art"] = True
        sand.add(name)
    return sorted(sand), report


def cpp_table(names: list[str]) -> str:
    head = ("// Generated by `python -m sandover.sand_textures write` from the levels' collision pat\n"
            "// materials (sandover/tools/sandover/sand_textures.py). Do not edit by hand.\n")
    return head + "".join(f'"{n}",\n' for n in names)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-sand", description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=["show", "write"])
    ap.add_argument("--out", type=Path, help=f"table path (default $SANDOVER_ROOT/{CPP_TABLE_REL})")
    a = ap.parse_args(argv)
    names, report = derive()
    for tex in sorted(report, key=lambda t: (t not in names, t)):
        print(("SAND " if tex in names else "     ") + tex, json.dumps(report[tex]))
    if a.cmd == "write":
        out = a.out or paths.root() / CPP_TABLE_REL
        out.write_text(cpp_table(names), encoding="utf-8", newline="\n")
        print(f"wrote {len(names)} textures to {out.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
