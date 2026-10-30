"""Prop lights: decorative background props given a soft light by design.

Not emitters in vanilla, so neither the emitter score nor the baked E finds them: the textures
are listed in ``prop_lights.json`` (texture -> levels, radius, gain). Every tfrag/tie/shrub
draw of a listed texture is clustered on a ``cell_m`` grid (``statics.cluster_points``), one
light per prop. Colour: the texture's alpha-weighted mean times the mean vertex colour of its
draws (brightest palette slot), in linear light, max channel 1: the cave mushrooms get their
green from the vertex colours, not the pale texture.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pygltflib

from sandover.lights.statics import cluster_points
from sandover.unbake.glb import SKIP_TREES, _read

GLOW_CONTRAST = 1.3  # median luma over the surroundings' in vanilla frames (glowcheck)
GLOW_SATURATION = 0.6  # and a vivid colour: pale matte props are brighter by albedo alone
GLOW_CEILING = 0.5  # or this share of its pixels at the SDR ceiling, twice the surroundings'


@dataclass
class Prop:
    texture: str
    tree: str
    pos: np.ndarray  # (3,) metres
    extent_m: float
    verts: int
    vtx_colour: np.ndarray | None = None  # (3,) mean vertex colour of the texture's draws, 0..1


def texture_points(glb_path: Path, textures: set[str],
                   colours: dict[str, list[np.ndarray]] | None = None) -> dict[str, list[tuple[str, np.ndarray]]]:
    """World-space vertices (metres) drawn with each of ``textures``: {texture: [(tree, (N,3))]}.
    With ``colours``, also each draw's vertex colours (brightest palette slot per vertex)."""
    from sandover.materials.emissive_table import _material_texture_name

    g = pygltflib.GLTF2().load(str(glb_path))
    blob = g.binary_blob()
    out: dict[str, list[tuple[str, np.ndarray]]] = {}
    for root_idx in g.scenes[g.scene or 0].nodes:
        root = g.nodes[root_idx]
        name = root.name or ""
        if any(s in name for s in SKIP_TREES):
            continue
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None:
                continue
            for p in g.meshes[node.mesh].primitives:
                tex = _material_texture_name(g, p.material)
                if tex not in textures or p.indices is None:
                    continue
                pos = _read(g, blob, p.attributes.POSITION).astype(np.float64)
                idx = _read(g, blob, p.indices).astype(np.int64).ravel()
                used = np.unique(idx[idx < len(pos)])
                pts = pos[used]
                if colours is not None:
                    cols = [getattr(p.attributes, f"COLOR_{i}", None) for i in range(8)]
                    cols = np.stack([_read(g, blob, c)[:, :3] for c in cols if c is not None]) if any(
                        c is not None for c in cols) else None
                    if cols is not None:
                        v = cols[:, used[used < cols.shape[1]]].astype(np.float64)
                        best = (v @ np.array([0.299, 0.587, 0.114])).argmax(0)
                        colours.setdefault(tex, []).append(v[best, np.arange(v.shape[1])])
                if node.matrix is not None:  # instanced (wind) ties
                    m = np.array(node.matrix, np.float64).reshape(4, 4).T
                    pts = pts @ m[:3, :3].T + m[:3, 3]
                if len(pts):
                    out.setdefault(tex, []).append((name, pts))
    return out


def level_props(glb_path: Path, table: dict[str, dict], level: str) -> list[Prop]:
    """One ``Prop`` per cluster of each listed texture used on ``level``."""
    want = {t for t, v in table.items() if level in v.get("levels", ())}
    if not want or not glb_path.is_file():
        return []
    out = []
    vcols: dict[str, list[np.ndarray]] = {}
    for tex, parts in sorted(texture_points(glb_path, want, vcols).items()):
        cfg = table[tex]
        vc = np.concatenate(vcols[tex]).mean(0) if vcols.get(tex) else None
        pts = np.concatenate([p for _, p in parts])
        tree_of = np.concatenate([np.full(len(p), i) for i, (_, p) in enumerate(parts)])
        for ctr, ext, _, members in cluster_points(pts, np.ones(len(pts)), float(cfg.get("cell_m", 1.0)),
                                                   float(cfg.get("split_m", 8.0))):
            tree = parts[int(np.bincount(tree_of[members]).argmax())][0]
            # the member vertex nearest the centroid: a patch's centroid can sit inside rock, and
            # the tile lights have no world occlusion
            on_mesh = pts[members][np.argmin(np.linalg.norm(pts[members] - ctr, axis=1))]
            out.append(Prop(tex, tree, on_mesh, ext, len(members), vc))
    return out


def texture_linear_colour(png: Path) -> list[float] | None:
    """Alpha-weighted mean of a texture in linear light (sRGB decode), max channel 1."""
    from PIL import Image

    if not png.is_file():
        return None
    a = np.asarray(Image.open(png).convert("RGBA"), np.float64) / 255.0
    w = a[..., 3].ravel()
    if w.sum() <= 0:
        return None
    lin = (a[..., :3].reshape(-1, 3)) ** 2.2
    m = (lin * w[:, None]).sum(0) / w.sum()
    return list(m / m.max()) if m.max() > 0 else None


def prop_colour(tex_lin: list[float] | None, vtx: np.ndarray | None) -> list[float] | None:
    """Texture colour (linear) times the vertex colour (sRGB-like 0..1, decoded), max channel 1."""
    if tex_lin is None:
        return None
    c = np.array(tex_lin, np.float64)
    if vtx is not None:
        c = c * np.clip(np.asarray(vtx, np.float64), 0.0, None) ** 2.2
    return list(c / c.max()) if c.max() > 0 else None


def glows_in_vanilla(cfg: dict, emitter: bool = False) -> tuple[bool, str]:
    """Does the game draw this prop as a light? From the ``vanilla`` frames measured with
    ``glowcheck`` and recorded in ``prop_lights.json`` (medians over the frames): brighter than
    its surroundings (``GLOW_CONTRAST``) in a vivid colour (``GLOW_SATURATION``), or at the SDR
    ceiling (``GLOW_CEILING``, and twice the surroundings' share). ``emitter``: the texture also
    carries non-zero baked E on the level (a census static), so the game treats it as an emitter
    (at some hour; the Mountain Pass crystals' E is night-only) and the contrast suffices: the
    saturation guard is for props with no E at all, whose brightness may be albedo alone."""
    frames = cfg.get("vanilla") or []
    if not frames:
        return False, "prop with no measured vanilla frame"
    med = lambda k: float(np.median([f[k] for f in frames]))
    own = float(np.median([f["ceiling"][0] for f in frames]))
    around = float(np.median([f["ceiling"][1] for f in frames]))
    if own >= GLOW_CEILING and own >= 2.0 * around:
        return True, ""
    if med("contrast") >= GLOW_CONTRAST and (emitter or med("saturation") >= GLOW_SATURATION):
        return True, ""
    return False, "prop not drawn as a light in vanilla (contrast, colour)"
