"""Static emitters: clusters of emissive-lit triangles on emitter-scored tfrag/tie textures.

One light per spatial cluster of a level's lit triangles (corner sidecar E >= ``covered``) over
every emitter texture: triangle centres are binned on a ``cell_m`` grid, 26-connected cells
form a cluster, and a cluster wider than ``split_m`` is cut into ``split_m`` tiles so a lava
floor gives several lights instead of one in the middle. Position = E x area weighted centre;
colour = the energy-weighted mean of its textures' colours (mean albedo over the texture's
emissive texels, E map >= ``covered``).
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pygltflib
from scipy import ndimage

from sandover.unbake import sidecar
from sandover.unbake.glb import SKIP_TREES, _read


@dataclass
class Cluster:
    texture: str  # the texture with the most energy
    tree: str
    textures: dict[str, float]  # texture -> energy share
    pos: np.ndarray  # (3,) metres
    extent_m: float
    energy: float  # sum of E x area (m^2)
    tris: int
    source: int = -1  # sidecar E source with the most energy (0-7 palette slot, 8 static, 9 night)


def cluster_points(pts: np.ndarray, w: np.ndarray, cell_m: float = 2.0,
                   split_m: float = 12.0) -> list[tuple[np.ndarray, float, float, np.ndarray]]:
    """Group (N,3) points: [(weighted centre, extent m, weight, member indices)]."""
    if not len(pts):
        return []
    cell = np.floor(pts / cell_m).astype(np.int64)
    lo = cell.min(0)
    shape = cell.max(0) - lo + 1
    if np.prod(shape.astype(np.float64)) > 5e7:  # sparse fallback: split tiles only
        labels = np.zeros(len(pts), np.int64)
    else:
        grid = np.zeros(shape, bool)
        c = cell - lo
        grid[c[:, 0], c[:, 1], c[:, 2]] = True
        lab, _ = ndimage.label(grid, structure=np.ones((3, 3, 3)))
        labels = lab[c[:, 0], c[:, 1], c[:, 2]].astype(np.int64)
    out = []
    for lb in np.unique(labels):
        idx = np.nonzero(labels == lb)[0]
        p = pts[idx]
        ext = float(np.max(p.max(0) - p.min(0)))
        if ext > split_m:
            tile = np.floor(p / split_m).astype(np.int64)
            _, tid = np.unique(tile, axis=0, return_inverse=True)
            groups = [idx[tid.ravel() == t] for t in range(tid.max() + 1)]
        else:
            groups = [idx]
        for g in groups:
            ww = w[g]
            s = float(ww.sum())
            ctr = (pts[g] * ww[:, None]).sum(0) / s if s > 0 else pts[g].mean(0)
            gp = pts[g]
            out.append((ctr, float(np.max(gp.max(0) - gp.min(0))), s, g))
    return out


def level_clusters(glb_path: Path, sc_path: Path, emitters: set[str], covered: float = 0.1,
                   cell_m: float = 2.0, split_m: float = 12.0) -> list[Cluster]:
    from sandover.materials.emissive_table import _material_texture_name

    g = pygltflib.GLTF2().load(str(glb_path))
    blob = g.binary_blob()
    tree_rec = {(t["kind"], t["fr3_tree"]): t["records"] for t in sidecar.read(sc_path)["trees"]}
    per_tex: dict[tuple[str, str], list[tuple[np.ndarray, np.ndarray]]] = {}
    n_tfrag = 0
    for root_idx in g.scenes[g.scene or 0].nodes:
        root = g.nodes[root_idx]
        name = root.name or ""
        if any(s in name for s in SKIP_TREES):
            continue
        if "-tfrag-" in name:
            key = (0, n_tfrag)
            n_tfrag += 1
        elif "-tie-" in name:
            key = (1, int(name.rsplit("-", 1)[1]))
        else:
            continue
        rec = tree_rec.get(key)
        if rec is None:
            continue
        e_all = ((rec[:, 3] >> 16) & 0xFF) / 255.0
        src_all = (rec[:, 3] >> 24) & 0xF
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None or node.matrix is not None:
                continue
            for p in g.meshes[node.mesh].primitives:
                tex = _material_texture_name(g, p.material)
                if p.indices is None or tex not in emitters:
                    continue
                pos = _read(g, blob, p.attributes.POSITION).astype(np.float64)
                idx = _read(g, blob, p.indices).astype(np.int64).ravel()
                idx = idx[: len(idx) // 3 * 3].reshape(-1, 3)
                idx = idx[(idx < min(len(e_all), len(pos))).all(1)]
                if not len(idx):
                    continue
                e = e_all[idx]
                lit = e.max(1) >= covered
                if not lit.any():
                    continue
                t = idx[lit]
                a, b, cc = pos[t[:, 0]], pos[t[:, 1]], pos[t[:, 2]]
                area = 0.5 * np.linalg.norm(np.cross(b - a, cc - a), axis=1)
                per_tex.setdefault((tex, name), []).append(((a + b + cc) / 3.0, e[lit].mean(1) * np.maximum(area, 1e-4),
                                                            src_all[t[:, 0]]))
    if not per_tex:
        return []
    keys = sorted(per_tex)
    pts = np.concatenate([c[0] for k in keys for c in per_tex[k]])
    w = np.concatenate([c[1] for k in keys for c in per_tex[k]])
    key_of = np.concatenate([np.full(len(c[0]), i) for i, k in enumerate(keys) for c in per_tex[k]])
    src = np.concatenate([c[2] for k in keys for c in per_tex[k]])
    out = []
    for ctr, ext, s, members in cluster_points(pts, w, cell_m, split_m):
        share: dict[str, float] = {}
        tree_e: dict[str, float] = {}
        for ki, wi in zip(key_of[members], w[members]):
            tex, tree = keys[ki]
            share[tex] = share.get(tex, 0.0) + float(wi)
            tree_e[tree] = tree_e.get(tree, 0.0) + float(wi)
        tot = sum(share.values()) or 1.0
        share = {t: round(e / tot, 3) for t, e in sorted(share.items(), key=lambda x: -x[1])}
        by_src = np.bincount(src[members].astype(np.int64), weights=w[members], minlength=16)
        out.append(Cluster(next(iter(share)), max(tree_e, key=tree_e.get), share, ctr, ext, s, len(members),
                           int(np.argmax(by_src))))
    return out


def texture_colour(emap: np.ndarray | None, rgba: np.ndarray | None, covered: float = 0.1) -> list[float] | None:
    """Mean albedo RGB over texels with E >= ``covered`` (all opaque texels if none)."""
    if rgba is None:
        return None
    if emap is not None and emap.shape == rgba.shape[:2]:
        m = emap >= covered
    else:
        m = rgba[..., 3] > 0.05
    if not m.any():
        m = np.ones(rgba.shape[:2], bool)
    return [round(float(v), 3) for v in rgba[..., :3][m].mean(0)]
