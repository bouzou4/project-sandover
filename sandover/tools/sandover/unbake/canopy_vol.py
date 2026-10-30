"""Canopy volume map v3 (`lighting/canopy/<level>.canopy`): the crown's leaf volume for the backlit
leaves' sun transmittance. The canopy shade keeps its own v2 map (canopy.py, unchanged).

Canopy is chosen from data, never from a list of names (see :func:`classify`): foliage held up
by a tree. A candidate texture has material class ``foliage`` (``materials.csv``) and an alpha
with cut-out holes. One of its triangles is canopy when it faces up or down, sits at least
``clearance`` metres above the ground beneath and beside it (up-facing triangles of every other
texture), and a wood surface (trunk, branch) nearby rises to just below it. A texture counts
when most of its clear area is held up that way; only its canopy triangles are drawn.

The map is rasterised from the level's background glb (tfrag, tie including instanced wind
ties, shrub; LOD 0) at ``texel`` metres (coarser past ``MAX_SIZE`` texels a side) with
``SUB`` x ``SUB`` samples per texel:

- cover = 1 - transmittance: each sample's light is multiplied, per canopy layer above it, by
  ``1 - opacity * alpha`` of that layer's leaf texture there (two-sided duplicates dropped), so
  gaps keep their structure and stacked layers darken without saturating; texel = sample mean;
- height = the lowest opaque (alpha >= 0.5) canopy sample in the texel (metres, world y);
  texels without canopy take the nearest covered texel's height, then the channel is blurred
  so the projection toward the sun stays smooth;
- top = the highest opaque leaf sample, filled and blurred the same way;
- volume = cover as above but from every card of the crown, any facing (the leaf density the
  backlight's sun transmittance marches through, between height and top).

Levels whose level-info has ``:sky #f`` get no map.

File layout (little-endian)::

    char[4] "SOVC", u32 version, u32 width, u32 height,
    f32 x0, f32 z0, f32 texel, f32 y0, f32 y1, u32 pad[3]      48 B (metres, world)
    u16 cover, u16 height, u16 top, u16 volume per texel, rows of increasing z, columns of
    increasing x; pad[0] = flags (1: the level has a sky); cover and volume 65535 = 1 (no light
    through), height and top = y0 + h / 65535 * (y1 - y0)

Texel (i, j) covers x0 + i * texel .. x0 + (i + 1) * texel (likewise z). Game units are
metres * 4096 (the engine converts).
"""

from __future__ import annotations

import argparse
import json
import os
import struct
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

from .canopy import (DGO, MAX_SIZE, SUB, GroundIndex, Mesh, TextureAlpha, WoodIndex, _alpha_at,
                     load_classes, load_level, tri_normals)

MAGIC = b"SOVC"
VERSION = 3
FLAG_SKY = 1
HEADER = struct.Struct("<4s3I5f3I")


@dataclass
class Params:
    texel: float = 0.25  # metres
    opacity: float = 0.9  # light a leaf blocks where its alpha is full
    height_blur: float = 0.0  # Gaussian sigma of the height channels, metres (the shader filters)
    clearance: float = 4.0  # metres above the ground beneath
    facing: float = 0.7  # |n.y| of a drawn canopy triangle (steep hanging skirts draw thin lines)
    min_share: float = 0.8  # share of a texture's clear area that a tree must hold up
    min_area: float = 25.0  # m2 of held area for a texture to count
    trunk_reach: float = 10.0  # metres sideways from a wood surface
    trunk_gap: float = 6.0  # metres the wood top may sit below the card
    cutout: float = 0.05  # share of texels below the alpha threshold for "has holes"
    blur: float = 0.0  # Gaussian sigma of the cover channel, metres (the shader filters)
    ground_cell: float = 1.0  # metres, ground height lookup cell
    near: float = 2.0  # metres: ground this close sideways counts as beneath (cliff edges)
    min_level_area: float = 100.0  # m2 of coverage below which a level gets no map



@dataclass
class CanopyMap:
    x0: float
    z0: float
    texel: float
    y0: float
    y1: float
    coverage: np.ndarray  # (H,W) float 0..1: cover, 1 - transmittance (shade cards)
    height: np.ndarray  # (H,W) float metres: lowest leaf
    top: np.ndarray | None = None  # (H,W) float metres: highest leaf
    volume: np.ndarray | None = None  # (H,W) float 0..1: cover of every canopy card (any facing)
    sky: bool = True
    report: dict = field(default_factory=dict)



def classify(meshes: list[Mesh], alphas: dict[str, TextureAlpha], classes: dict[str, str],
             p: Params) -> tuple[dict[str, np.ndarray], dict, dict[str, np.ndarray]]:
    """Canopy triangles per texture (boolean masks over the mesh): the shade cards, a report,
    and the crown body (the shade cards plus the steeper cards held the same way).

    A triangle is canopy when it faces up or down (|n.y| >= facing), sits ``clearance`` metres
    above the ground within ``near`` metres sideways, and a wood surface within ``trunk_reach``
    rises to within ``trunk_gap`` metres below it (foliage held up by a tree). A texture is
    canopy when such triangles cover ``min_area`` and are ``min_share`` of its clear area.
    """
    cand = {m.texture for m in meshes
            if classes.get(m.texture) == "foliage" and alphas[m.texture].cutout >= p.cutout}
    ground, wood = [], []
    for m in meshes:
        if classes.get(m.texture) == "wood":
            wood.append(m.tri)
        if m.texture in cand:
            continue
        n, a = tri_normals(m.tri)
        ground.append(m.tri[(np.abs(n[:, 1]) > 0.7) & (a > 1e-6)])  # winding is arbitrary
    gi = GroundIndex(np.concatenate(ground) if ground else np.zeros((0, 3, 3)), p.ground_cell)
    wi = WoodIndex(np.concatenate(wood) if wood else np.zeros((0, 3, 3)), p.ground_cell * 2, p.trunk_reach)
    keep: dict[str, np.ndarray] = {}
    body_of: dict[str, np.ndarray] = {}
    rep: dict = {}
    for m in meshes:
        if m.texture not in cand:
            continue
        n, a = tri_normals(m.tri)
        face = np.abs(n[:, 1]) >= p.facing
        c = m.tri.mean(1)
        low = m.tri[:, :, 1].min(1)
        g = gi.below_near(c[:, 0], low, c[:, 2], p.near)
        clear = face & np.isfinite(g) & ((low - g) >= p.clearance)
        near_wood = wi.top_near(c[:, 0], c[:, 2]) >= low - p.trunk_gap
        held = clear & near_wood
        body = np.isfinite(g) & ((low - g) >= p.clearance) & near_wood  # any facing
        ca, ha = float(a[clear].sum()), float(a[held].sum())
        share = ha / ca if ca > 0 else 0.0
        ok = ha >= p.min_area and share >= p.min_share
        rep[m.texture] = {"area_m2": round(float(a.sum()), 1), "clear_m2": round(ca, 1), "held_m2": round(ha, 1),
                          "held_share": round(share, 3), "cutout": round(alphas[m.texture].cutout, 3),
                          "canopy": bool(ok)}
        if ok:
            keep[m.texture] = held
            body_of[m.texture] = body | held
    return keep, rep, body_of


def dedupe_index(tri: np.ndarray) -> np.ndarray:
    """Indices of the triangles to keep: one of each set that repeats the same corners."""
    if not len(tri):
        return np.zeros(0, np.int64)
    c = np.round(tri * 100).astype(np.int64)  # cm
    o = np.lexsort((c[:, :, 2], c[:, :, 1], c[:, :, 0]), axis=-1)
    key = np.take_along_axis(c, o[:, :, None], axis=1).reshape(len(tri), 9)
    _, first = np.unique(key, axis=0, return_index=True)
    return np.sort(first)


def rasterize(tris: list[tuple], p: Params) -> CanopyMap | None:
    """Cover, leaf heights and volume cover on a grid around all triangles (xz projection).

    ``tris``: (triangles, uvs, alpha, shade) per texture; ``shade`` (bool per triangle, default
    all) marks the cards that also draw the shade cover; every card draws the volume channels.
    """
    if not tris:
        return None
    allp = np.concatenate([t[0].reshape(-1, 3) for t in tris])
    pad = 2.0
    ext = max(np.ptp(allp[:, 0]), np.ptp(allp[:, 2])) + 2 * pad
    texel = max(p.texel, float(np.ceil(ext / MAX_SIZE / 0.25)) * 0.25)
    x0 = np.floor((allp[:, 0].min() - pad) / texel) * texel
    z0 = np.floor((allp[:, 2].min() - pad) / texel) * texel
    W = int(np.ceil((allp[:, 0].max() + pad - x0) / texel))
    H = int(np.ceil((allp[:, 2].max() + pad - z0) / texel))
    trans = np.ones((H * SUB, W * SUB), np.float32)
    trans_v = np.ones((H * SUB, W * SUB), np.float32)
    low = np.full((H, W), np.inf, np.float32)
    top = np.full((H, W), -np.inf, np.float32)
    for t in tris:
        tri, uv, ta = t[0], t[1], t[2]
        shade = t[3] if len(t) > 3 else np.ones(len(tri), bool)
        keep = dedupe_index(tri)
        for k in keep:
            _raster_one(tri[k], uv[k], ta, x0, z0, texel, p.opacity, trans if shade[k] else None, trans_v, low, top)
    cov = 1.0 - trans.reshape(H, SUB, W, SUB).mean((1, 3), dtype=np.float64)
    vol = 1.0 - trans_v.reshape(H, SUB, W, SUB).mean((1, 3), dtype=np.float64)
    if p.blur > 0:
        cov = ndimage.gaussian_filter(cov, p.blur / texel, mode="constant")
    empty = ~np.isfinite(low)
    if empty.all():
        return None
    idx = ndimage.distance_transform_edt(empty, return_distances=False, return_indices=True)
    height = low.astype(np.float64)[idx[0], idx[1]]
    hi = top.astype(np.float64)[idx[0], idx[1]]
    if p.height_blur > 0:
        height = ndimage.gaussian_filter(height, p.height_blur / texel, mode="nearest")
        hi = ndimage.gaussian_filter(hi, p.height_blur / texel, mode="nearest")
    hi = np.maximum(hi, height)
    return CanopyMap(float(x0), float(z0), texel, float(height.min()), float(hi.max()), cov, height, hi, vol)


def _raster_one(tri, uv, ta, x0, z0, texel, opacity, trans, trans_v, low, top):
    P = tri[:, [0, 2]]
    d = (P[1, 0] - P[0, 0]) * (P[2, 1] - P[0, 1]) - (P[2, 0] - P[0, 0]) * (P[1, 1] - P[0, 1])
    if abs(d) < 1e-10:
        return
    s = texel / SUB
    HS, WS = trans_v.shape
    i0 = max(int(np.floor((P[:, 0].min() - x0) / s)), 0)
    i1 = min(int(np.ceil((P[:, 0].max() - x0) / s)), WS)
    j0 = max(int(np.floor((P[:, 1].min() - z0) / s)), 0)
    j1 = min(int(np.ceil((P[:, 1].max() - z0) / s)), HS)
    if i1 <= i0 or j1 <= j0:
        return
    I, J = np.meshgrid(np.arange(i0, i1), np.arange(j0, j1))
    X, Z = x0 + (I + 0.5) * s + 1.37e-4, z0 + (J + 0.5) * s + 2.91e-4  # off shared edges
    b1 = ((X - P[0, 0]) * (P[2, 1] - P[0, 1]) - (P[2, 0] - P[0, 0]) * (Z - P[0, 1])) / d
    b2 = ((P[1, 0] - P[0, 0]) * (Z - P[0, 1]) - (X - P[0, 0]) * (P[1, 1] - P[0, 1])) / d
    b0 = 1 - b1 - b2
    inside = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
    if not inside.any():
        return
    B = np.stack([b0[inside], b1[inside], b2[inside]], 1)
    a = _alpha_at(ta, B @ uv)
    I, J = I[inside], J[inside]
    t = (1.0 - opacity * a).astype(np.float32)  # each sample once per triangle
    trans_v[J, I] *= t
    if trans is not None:
        trans[J, I] *= t
    on = a >= 0.5
    if on.any():
        y = (B[on] @ tri[:, 1]).astype(np.float32)
        np.minimum.at(low, (J[on] // SUB, I[on] // SUB), y)
        np.maximum.at(top, (J[on] // SUB, I[on] // SUB), y)


def build(glb: Path, materials_csv: Path, dgo: str | None, p: Params | None = None,
          sky: bool = True) -> tuple[CanopyMap | None, dict]:
    p = p or Params()
    meshes, alphas = load_level(glb)
    classes = load_classes(materials_csv, dgo)
    keep, rep, body = classify(meshes, alphas, classes, p)
    tris = [(m.tri[body[m.texture]], m.uv[body[m.texture]], alphas[m.texture], keep[m.texture][body[m.texture]])
            for m in meshes if m.texture in keep]
    cm = rasterize(tris, p)
    report = {"params": p.__dict__, "textures": rep, "canopy_textures": sorted(keep), "sky": sky}
    if cm is not None:
        cm.sky = sky
    if cm is not None and cm.coverage.sum() * cm.texel ** 2 < p.min_level_area:
        report["skipped"] = f"coverage {cm.coverage.sum() * cm.texel ** 2:.0f} m2 < min_level_area"
        cm = None
    if cm is not None:
        report.update({"size": [int(cm.coverage.shape[1]), int(cm.coverage.shape[0])],
                       "origin_m": [cm.x0, cm.z0], "y_range_m": [cm.y0, cm.y1],
                       "texel_m": cm.texel, "covered_m2": round(float(cm.coverage.sum() * cm.texel ** 2), 1)})
        cm.report = report
    return cm, report


def write(path: Path, cm: CanopyMap) -> None:
    H, W = cm.coverage.shape
    span = max(cm.y1 - cm.y0, 1e-3)
    top = cm.height if cm.top is None else cm.top
    vol = cm.coverage if cm.volume is None else cm.volume
    u16 = lambda x: np.clip(np.rint(x * 65535), 0, 65535).astype("<u2")
    body = np.stack([u16(cm.coverage), u16((cm.height - cm.y0) / span), u16((top - cm.y0) / span), u16(vol)], -1)
    flags = FLAG_SKY if cm.sky else 0
    path.write_bytes(HEADER.pack(MAGIC, VERSION, W, H, cm.x0, cm.z0, cm.texel, cm.y0, cm.y0 + span, flags, 0, 0)
                     + body.tobytes())


def read(path: Path) -> CanopyMap:
    b = path.read_bytes()
    magic, ver, W, H, x0, z0, texel, y0, y1, flags, *_ = HEADER.unpack_from(b, 0)
    if magic != MAGIC or ver != VERSION:
        raise ValueError(f"{path.name}: not a canopy map v{VERSION}")
    d = np.frombuffer(b, "<u2", W * H * 4, HEADER.size).reshape(H, W, 4).astype(np.float64) / 65535
    return CanopyMap(x0, z0, texel, y0, y1, d[..., 0], y0 + d[..., 1] * (y1 - y0), y0 + d[..., 2] * (y1 - y0),
                     d[..., 3], bool(flags & FLAG_SKY))


def preview(path: Path, cm: CanopyMap) -> None:
    """Coverage as grey, height as hue tint (low = warm), north up."""
    h = (cm.height - cm.y0) / max(cm.y1 - cm.y0, 1e-3)
    c = cm.coverage
    rgb = np.stack([c * (1 - 0.5 * h), c * (0.6 + 0.4 * h), c * (0.3 + 0.7 * h)], -1)
    Image.fromarray(np.clip(rgb[::-1] * 255, 0, 255).astype(np.uint8)).save(path)


def level_skies(level_info: Path) -> dict[str, bool]:
    """Level name -> its `:sky` flag, from level-info.gc."""
    out, name = {}, None
    for line in level_info.read_text(encoding="utf-8").splitlines():
        t = line.strip()
        if t.startswith(":name '"):
            name = t.split("'", 1)[1].split()[0]
        elif t.startswith(":sky ") and name:
            out[name] = t.split()[1] == "#t"
            name = None
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(prog="sandover-unbake canopy-vol", description=__doc__.split("\n")[0])
    root = Path(os.environ.get("SANDOVER_ROOT", "."))
    data = Path(os.environ.get("SANDOVER_DATA", root / "sandover/data"))
    ap.add_argument("levels", nargs="*", help="default: every level with a background glb")
    ap.add_argument("--levels-dir", type=Path, default=data / "unbake/riproot/decompiler_out/jak1/levels")
    ap.add_argument("--materials", type=Path, default=data / "materials/materials.csv")
    ap.add_argument("--out", type=Path, default=data / "lighting" / "canopy")
    ap.add_argument("--level-info", type=Path, default=root / "goal_src/jak1/engine/level/level-info.gc",
                    help="level-info.gc: levels with :sky #f get no map")
    for k, v in Params().__dict__.items():
        ap.add_argument(f"--{k.replace('_', '-')}", type=float, default=v)
    a = ap.parse_args(argv)
    p = Params(**{k: getattr(a, k) for k in Params().__dict__})
    levels = a.levels or sorted(d.name for d in a.levels_dir.iterdir() if (d / f"{d.name}-background.glb").exists())
    a.out.mkdir(parents=True, exist_ok=True)
    skies = level_skies(a.level_info) if a.level_info.is_file() else {}
    for lvl in levels:
        glb = a.levels_dir / lvl / f"{lvl}-background.glb"
        sky = skies.get(lvl, True)
        if not sky:
            (a.out / f"{lvl}-canopy.json").write_text(json.dumps({"skipped": "no sky (level-info :sky #f)"}, indent=1))
            print(lvl, "no sky: skipped", flush=True)
            continue
        cm, rep = build(glb, a.materials, f"{DGO[lvl]}.DGO" if lvl in DGO else None, p, sky)
        dst = a.out / f"{lvl}.canopy"
        (a.out / f"{lvl}-canopy.json").write_text(json.dumps(rep, indent=1))
        if cm is None:
            print(lvl, "no canopy" + (f" (stale {dst.name} left in place)" if dst.exists() else ""), flush=True)
            continue
        write(dst, cm)
        preview(a.out / f"{lvl}-canopy.png", cm)
        print(lvl, "canopy:", ", ".join(rep["canopy_textures"]), f"{rep['size'][0]}x{rep['size'][1]}",
              f"{rep['covered_m2']:.0f} m2", flush=True)


if __name__ == "__main__":
    main()
