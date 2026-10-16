"""Canopy shade map (`<level>.canopy`): top-down coverage and height of overhead foliage.

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
  so the projection toward the sun stays smooth.

File layout (little-endian)::

    char[4] "SOVC", u32 version, u32 width, u32 height,
    f32 x0, f32 z0, f32 texel, f32 y0, f32 y1, u32 pad[3]      48 B (metres, world)
    u16 cover, u16 height per texel, rows of increasing z, columns of increasing x;
    cover 65535 = 1 (no light through), height = y0 + h / 65535 * (y1 - y0)

Texel (i, j) covers x0 + i * texel .. x0 + (i + 1) * texel (likewise z). Game units are
metres * 4096 (the engine converts).
"""

from __future__ import annotations

import argparse
import base64
import csv
import io
import json
import os
import struct
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pygltflib
from PIL import Image
from scipy import ndimage

from .glb import _read

MAGIC = b"SOVC"
VERSION = 2
HEADER = struct.Struct("<4s3I5f3I")
SUB = 2  # samples per texel side
MAX_SIZE = 2048  # texels per side; the texel grows in 0.25 m steps past it
SKIP_TREES = ("tfrag-lowres",)


@dataclass
class Params:
    texel: float = 0.25  # metres
    opacity: float = 0.9  # light a leaf blocks where its alpha is full
    height_blur: float = 1.5  # Gaussian sigma of the height channel, metres (smooth projection)
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
class Mesh:
    """Triangles of one texture, world metres."""

    texture: str
    tri: np.ndarray  # (T,3,3)
    uv: np.ndarray  # (T,3,2)


@dataclass
class TextureAlpha:
    alpha: np.ndarray  # (H,W) float, 0..1 of the texture's own range
    wrap_s: bool = True
    wrap_t: bool = True
    cutout: float = 0.0  # share of texels below 0.5


@dataclass
class CanopyMap:
    x0: float
    z0: float
    texel: float
    y0: float
    y1: float
    coverage: np.ndarray  # (H,W) float 0..1: cover, 1 - transmittance
    height: np.ndarray  # (H,W) float metres
    report: dict = field(default_factory=dict)


# ---------------------------------------------------------------------------- glb


def _texture_index(g: pygltflib.GLTF2, material: int | None) -> int | None:
    if material is None or not (0 <= material < len(g.materials)):
        return None
    pbr = g.materials[material].pbrMetallicRoughness
    ref = pbr.baseColorTexture if pbr else None
    return None if ref is None else ref.index


def _texture_name(g: pygltflib.GLTF2, ti: int) -> str:
    tex = g.textures[ti]
    return tex.name or (g.images[tex.source].name if tex.source is not None else "") or f"tex{ti}"


def _image_alpha(g: pygltflib.GLTF2, blob: bytes, ti: int) -> TextureAlpha:
    tex = g.textures[ti]
    img = g.images[tex.source]
    if img.uri and img.uri.startswith("data:"):
        raw = base64.b64decode(img.uri.split(",", 1)[1])
    elif img.bufferView is not None:
        v = g.bufferViews[img.bufferView]
        raw = blob[(v.byteOffset or 0):(v.byteOffset or 0) + v.byteLength]
    else:
        raw = b""
    a = np.asarray(Image.open(io.BytesIO(raw)).convert("RGBA"), np.float32)[..., 3] if raw else np.ones((1, 1))
    top = max(float(a.max()), 1.0)
    a = a / top
    s = g.samplers[tex.sampler] if tex.sampler is not None and g.samplers else None
    return TextureAlpha(a, s is None or s.wrapS != 33071, s is None or s.wrapT != 33071, float((a < 0.5).mean()))


def _matrix(node) -> np.ndarray | None:
    if node.matrix is None:
        return None
    return np.asarray(node.matrix, np.float64).reshape(4, 4).T  # glTF is column-major


def load_level(glb: Path) -> tuple[list[Mesh], dict[str, TextureAlpha]]:
    """Every LOD 0 background triangle grouped by texture, instanced wind ties placed."""
    g = pygltflib.GLTF2().load(str(glb))
    blob = g.binary_blob()
    parts: dict[str, list[tuple[np.ndarray, np.ndarray]]] = defaultdict(list)
    alphas: dict[str, TextureAlpha] = {}
    for ri in g.scenes[0].nodes:
        root = g.nodes[ri]
        if any(s in root.name for s in SKIP_TREES):
            continue
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None:
                continue
            m = _matrix(node)
            for p in g.meshes[node.mesh].primitives:
                ti = _texture_index(g, p.material)
                if ti is None or p.indices is None or p.attributes.TEXCOORD_0 is None:
                    continue
                name = _texture_name(g, ti)
                if name not in alphas:
                    alphas[name] = _image_alpha(g, blob, ti)
                pos = _read(g, blob, p.attributes.POSITION).astype(np.float64)
                if m is not None:
                    pos = pos @ m[:3, :3].T + m[:3, 3]
                uv = _read(g, blob, p.attributes.TEXCOORD_0).astype(np.float64)
                idx = _read(g, blob, p.indices).astype(np.int64).reshape(-1, 3)
                parts[name].append((pos[idx], uv[idx]))
    meshes = [Mesh(k, np.concatenate([a for a, _ in v]), np.concatenate([b for _, b in v])) for k, v in parts.items()]
    return meshes, alphas


# ---------------------------------------------------------------------- materials


def load_classes(materials_csv: Path, dgo: str | None = None) -> dict[str, str]:
    """Texture name -> class; rows of the level's DGO win, else the most common class."""
    votes: dict[str, Counter] = defaultdict(Counter)
    local: dict[str, Counter] = defaultdict(Counter)
    with open(materials_csv, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            votes[row["name"]][row["class"]] += 1
            if dgo and dgo in (row.get("levels") or ""):
                local[row["name"]][row["class"]] += 1
    return {n: (local.get(n) or c).most_common(1)[0][0] for n, c in votes.items()}


# ------------------------------------------------------------------------ geometry


def tri_normals(tri: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Unit normals (T,3) and areas (T,)."""
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    ln = np.linalg.norm(n, axis=1)
    return n / np.maximum(ln, 1e-12)[:, None], 0.5 * ln


_SPAN = 1.0e4  # metres per cell rank in the combined sort key
_OFF = 5.0e3  # world y offset, keeps y + _OFF in [0, _SPAN)


class GroundIndex:
    """Highest ground point below a query, per xz cell (ground sampled at triangle points)."""

    def __init__(self, tris: np.ndarray, cell: float):
        self.cell = cell
        pts = [tris.mean(1)] + [tris[:, i] for i in range(3)] + [(tris[:, i] + tris[:, (i + 1) % 3]) / 2 for i in range(3)]
        big = tris[np.ptp(tris[:, :, [0, 2]], axis=1).max(1) > cell]
        for tri in big:  # dense samples on large ground triangles
            k = int(np.ptp(tri[:, [0, 2]], axis=0).max() / cell * 2) + 2
            u, v = np.meshgrid(np.linspace(0, 1, k), np.linspace(0, 1, k))
            m = (u + v) <= 1
            pts.append(tri[0] + u[m][:, None] * (tri[1] - tri[0]) + v[m][:, None] * (tri[2] - tri[0]))
        p = np.concatenate(pts) if pts else np.zeros((0, 3))
        key = self._key(p[:, 0], p[:, 2])
        self.cells, rank = np.unique(key, return_inverse=True)
        self.comb = np.sort(rank.ravel() * _SPAN + (p[:, 1] + _OFF))

    def _key(self, x, z):
        ix = np.floor(np.asarray(x) / self.cell).astype(np.int64) + (1 << 20)
        iz = np.floor(np.asarray(z) / self.cell).astype(np.int64) + (1 << 20)
        return ix * (1 << 21) + iz

    def below(self, x, y, z) -> np.ndarray:
        """Highest ground y strictly below y at (x, z); -inf where none."""
        out = np.full(len(np.atleast_1d(x)), -np.inf)
        if not len(self.cells):
            return out
        k = self._key(x, z)
        r = np.clip(np.searchsorted(self.cells, k), 0, len(self.cells) - 1)
        has = self.cells[r] == k
        q = r * _SPAN + (np.asarray(y, np.float64) - 1e-3 + _OFF)
        i = np.searchsorted(self.comb, q) - 1
        ok = has & (i >= 0)
        ok[ok] &= np.floor(self.comb[i[ok]] / _SPAN) == r[ok]
        out[ok] = self.comb[i[ok]] - r[ok] * _SPAN - _OFF
        return out

    def below_near(self, x, y, z, radius: float) -> np.ndarray:
        """Highest ground below y within ``radius`` (square, whole cells) of (x, z)."""
        n = int(np.ceil(radius / self.cell))
        out = np.full(len(np.atleast_1d(x)), -np.inf)
        for dx in range(-n, n + 1):
            for dz in range(-n, n + 1):
                out = np.maximum(out, self.below(x + dx * self.cell, y, z + dz * self.cell))
        return out


class WoodIndex:
    """Highest point of steep wood surfaces (trunks, branches) per xz cell, max-filtered sideways."""

    def __init__(self, tris: np.ndarray, cell: float, reach: float):
        self.cell = cell
        if len(tris):
            n, _ = tri_normals(tris)
            pts = tris[np.abs(n[:, 1]) < 0.7].reshape(-1, 3)
        else:
            pts = np.zeros((0, 3))
        self.ok = len(pts) > 0
        if not self.ok:
            return
        self.x0, self.z0 = pts[:, 0].min() - reach, pts[:, 2].min() - reach
        nx = int((pts[:, 0].max() + reach - self.x0) / cell) + 2
        nz = int((pts[:, 2].max() + reach - self.z0) / cell) + 2
        top = np.full((nz, nx), -np.inf)
        np.maximum.at(top, (((pts[:, 2] - self.z0) / cell).astype(int), ((pts[:, 0] - self.x0) / cell).astype(int)),
                      pts[:, 1])
        r = int(np.ceil(reach / cell))
        self.top = ndimage.maximum_filter(top, size=2 * r + 1, mode="constant", cval=-np.inf)

    def top_near(self, x, z) -> np.ndarray:
        out = np.full(len(x), -np.inf)
        if not self.ok:
            return out
        i = ((np.asarray(x) - self.x0) / self.cell).astype(np.int64)
        j = ((np.asarray(z) - self.z0) / self.cell).astype(np.int64)
        ok = (i >= 0) & (j >= 0) & (i < self.top.shape[1]) & (j < self.top.shape[0])
        out[ok] = self.top[j[ok], i[ok]]
        return out


def classify(meshes: list[Mesh], alphas: dict[str, TextureAlpha], classes: dict[str, str],
             p: Params) -> tuple[dict[str, np.ndarray], dict]:
    """Canopy triangles per texture (boolean mask over the mesh) and a per-texture report.

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
        held = clear & (wi.top_near(c[:, 0], c[:, 2]) >= low - p.trunk_gap)
        ca, ha = float(a[clear].sum()), float(a[held].sum())
        share = ha / ca if ca > 0 else 0.0
        ok = ha >= p.min_area and share >= p.min_share
        rep[m.texture] = {"area_m2": round(float(a.sum()), 1), "clear_m2": round(ca, 1), "held_m2": round(ha, 1),
                          "held_share": round(share, 3), "cutout": round(alphas[m.texture].cutout, 3),
                          "canopy": bool(ok)}
        if ok:
            keep[m.texture] = held
    return keep, rep


# --------------------------------------------------------------------- rasterise


def _alpha_at(t: TextureAlpha, uv: np.ndarray) -> np.ndarray:
    h, w = t.alpha.shape
    u = uv[:, 0] * w - 0.5
    v = uv[:, 1] * h - 0.5
    iu = np.floor(u + 0.5).astype(np.int64)
    iv = np.floor(v + 0.5).astype(np.int64)
    iu = np.mod(iu, w) if t.wrap_s else np.clip(iu, 0, w - 1)
    iv = np.mod(iv, h) if t.wrap_t else np.clip(iv, 0, h - 1)
    return t.alpha[iv, iu]


def dedupe(tri: np.ndarray, uv: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Drop triangles that repeat another's corners (two-sided cards drawn twice)."""
    if not len(tri):
        return tri, uv
    c = np.round(tri * 100).astype(np.int64)  # cm
    o = np.lexsort((c[:, :, 2], c[:, :, 1], c[:, :, 0]), axis=-1)
    key = np.take_along_axis(c, o[:, :, None], axis=1).reshape(len(tri), 9)
    _, first = np.unique(key, axis=0, return_index=True)
    first = np.sort(first)
    return tri[first], uv[first]


def rasterize(tris: list[tuple[np.ndarray, np.ndarray, TextureAlpha]], p: Params) -> CanopyMap | None:
    """Cover (1 - transmittance) and lowest height on a grid around all triangles (xz projection)."""
    if not tris:
        return None
    allp = np.concatenate([t.reshape(-1, 3) for t, _, _ in tris])
    pad = 2.0
    ext = max(np.ptp(allp[:, 0]), np.ptp(allp[:, 2])) + 2 * pad
    texel = max(p.texel, float(np.ceil(ext / MAX_SIZE / 0.25)) * 0.25)
    x0 = np.floor((allp[:, 0].min() - pad) / texel) * texel
    z0 = np.floor((allp[:, 2].min() - pad) / texel) * texel
    W = int(np.ceil((allp[:, 0].max() + pad - x0) / texel))
    H = int(np.ceil((allp[:, 2].max() + pad - z0) / texel))
    trans = np.ones((H * SUB, W * SUB), np.float32)
    low = np.full((H, W), np.inf, np.float32)
    for tri, uv, ta in tris:
        tri, uv = dedupe(tri, uv)
        for k in range(len(tri)):
            _raster_one(tri[k], uv[k], ta, x0, z0, texel, p.opacity, trans, low)
    cov = 1.0 - trans.reshape(H, SUB, W, SUB).mean((1, 3), dtype=np.float64)
    if p.blur > 0:
        cov = ndimage.gaussian_filter(cov, p.blur / texel, mode="constant")
    height = low.astype(np.float64)
    empty = ~np.isfinite(height)
    if empty.all():
        return None
    idx = ndimage.distance_transform_edt(empty, return_distances=False, return_indices=True)
    height = height[idx[0], idx[1]]
    if p.height_blur > 0:
        height = ndimage.gaussian_filter(height, p.height_blur / texel, mode="nearest")
    return CanopyMap(float(x0), float(z0), texel, float(height.min()), float(height.max()), cov, height)


def _raster_one(tri, uv, ta, x0, z0, texel, opacity, trans, low):
    P = tri[:, [0, 2]]
    d = (P[1, 0] - P[0, 0]) * (P[2, 1] - P[0, 1]) - (P[2, 0] - P[0, 0]) * (P[1, 1] - P[0, 1])
    if abs(d) < 1e-10:
        return
    s = texel / SUB
    HS, WS = trans.shape
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
    trans[J, I] *= (1.0 - opacity * a).astype(np.float32)  # each sample once per triangle
    on = a >= 0.5
    if on.any():
        np.minimum.at(low, (J[on] // SUB, I[on] // SUB), (B[on] @ tri[:, 1]).astype(np.float32))


# ---------------------------------------------------------------------- build + io


def build(glb: Path, materials_csv: Path, dgo: str | None, p: Params | None = None) -> tuple[CanopyMap | None, dict]:
    p = p or Params()
    meshes, alphas = load_level(glb)
    classes = load_classes(materials_csv, dgo)
    keep, rep = classify(meshes, alphas, classes, p)
    tris = [(m.tri[keep[m.texture]], m.uv[keep[m.texture]], alphas[m.texture]) for m in meshes if m.texture in keep]
    cm = rasterize(tris, p)
    report = {"params": p.__dict__, "textures": rep, "canopy_textures": sorted(keep)}
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
    cov = np.clip(np.rint(cm.coverage * 65535), 0, 65535).astype("<u2")
    hgt = np.clip(np.rint((cm.height - cm.y0) / span * 65535), 0, 65535).astype("<u2")
    body = np.stack([cov, hgt], -1).tobytes()
    path.write_bytes(HEADER.pack(MAGIC, VERSION, W, H, cm.x0, cm.z0, cm.texel, cm.y0, cm.y0 + span, 0, 0, 0) + body)


def read(path: Path) -> CanopyMap:
    b = path.read_bytes()
    magic, ver, W, H, x0, z0, texel, y0, y1, *_ = HEADER.unpack_from(b, 0)
    if magic != MAGIC or ver != VERSION:
        raise ValueError(f"{path.name}: not a canopy map v{VERSION}")
    d = np.frombuffer(b, "<u2", W * H * 2, HEADER.size).reshape(H, W, 2).astype(np.float64)
    return CanopyMap(x0, z0, texel, y0, y1, d[..., 0] / 65535, y0 + d[..., 1] / 65535 * (y1 - y0))


def preview(path: Path, cm: CanopyMap) -> None:
    """Coverage as grey, height as hue tint (low = warm), north up."""
    h = (cm.height - cm.y0) / max(cm.y1 - cm.y0, 1e-3)
    c = cm.coverage
    rgb = np.stack([c * (1 - 0.5 * h), c * (0.6 + 0.4 * h), c * (0.3 + 0.7 * h)], -1)
    Image.fromarray(np.clip(rgb[::-1] * 255, 0, 255).astype(np.uint8)).save(path)


DGO = {"village1": "VI1", "beach": "BEA", "jungle": "JUN", "jungleb": "JUB", "misty": "MIS", "firecanyon": "FIC",
       "village2": "VI2", "sunken": "SUN", "sunkenb": "SUB", "swamp": "SWA", "rolling": "ROL", "ogre": "OGR",
       "village3": "VI3", "snow": "SNO", "maincave": "MAI", "darkcave": "DAR", "robocave": "ROB", "lavatube": "LAV",
       "citadel": "CIT", "finalboss": "FIN", "training": "TRA"}


def main(argv=None):
    ap = argparse.ArgumentParser(prog="sandover-unbake canopy", description=__doc__.split("\n")[0])
    root = Path(os.environ.get("SANDOVER_ROOT", "."))
    data = Path(os.environ.get("SANDOVER_DATA", root / "sandover/data"))
    ap.add_argument("levels", nargs="*", help="default: every level with a background glb")
    ap.add_argument("--levels-dir", type=Path, default=data / "unbake/riproot/decompiler_out/jak1/levels")
    ap.add_argument("--materials", type=Path, default=data / "materials/materials.csv")
    ap.add_argument("--out", type=Path, default=data / "lighting")
    for k, v in Params().__dict__.items():
        ap.add_argument(f"--{k.replace('_', '-')}", type=float, default=v)
    a = ap.parse_args(argv)
    p = Params(**{k: getattr(a, k) for k in Params().__dict__})
    levels = a.levels or sorted(d.name for d in a.levels_dir.iterdir() if (d / f"{d.name}-background.glb").exists())
    a.out.mkdir(parents=True, exist_ok=True)
    for lvl in levels:
        glb = a.levels_dir / lvl / f"{lvl}-background.glb"
        cm, rep = build(glb, a.materials, f"{DGO[lvl]}.DGO" if lvl in DGO else None, p)
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
