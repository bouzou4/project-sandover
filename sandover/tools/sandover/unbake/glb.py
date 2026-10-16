"""Load ripped level backgrounds (tfrag/tie/shrub) with all 8 time-of-day palettes."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pygltflib
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

_DTYPES = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}
_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
SKIP_TREES = ("tfrag-lowres",)  # lower LOD copy of tfrag-normal


@dataclass
class Tree:
    """One background tree: shared vertex buffer and the triangles drawn from it."""

    name: str
    kind: str  # tfrag | tie | shrub
    pos: np.ndarray  # (V,3) world units / 4096
    colors: np.ndarray  # (8,V,3) u8 palette colors, one per slot
    color_index: np.ndarray  # (V,) palette entry
    tris: np.ndarray  # (T,3)
    normal: np.ndarray | None = None  # (V,3) area-weighted geometric normal
    component: np.ndarray | None = None  # (V,) connected component id


def _read(g: pygltflib.GLTF2, blob: bytes, idx: int) -> np.ndarray:
    acc = g.accessors[idx]
    view = g.bufferViews[acc.bufferView]
    dt = _DTYPES[acc.componentType]
    w = _WIDTH[acc.type]
    start = (view.byteOffset or 0) + (acc.byteOffset or 0)
    arr = np.frombuffer(blob, dtype=dt, count=acc.count * w, offset=start)
    return arr.reshape(acc.count, w) if w > 1 else arr


def load_background(path: Path) -> list[Tree]:
    g = pygltflib.GLTF2().load(str(path))
    blob = g.binary_blob()
    trees = []
    for root_idx in g.scenes[0].nodes:
        root = g.nodes[root_idx]
        if any(s in root.name for s in SKIP_TREES):
            continue
        kind = "tfrag" if "-tfrag-" in root.name else "tie" if "-tie-" in root.name else "shrub"
        tris, attrs = [], None
        for c in root.children:
            node = g.nodes[c]
            if node.matrix is not None or node.mesh is None:
                continue  # instanced wind ties
            for p in g.meshes[node.mesh].primitives:
                if p.attributes.COLOR_7 is None:
                    raise ValueError(f"{path.name}: no COLOR_7, rip with kMaxColor = 8")
                attrs = p.attributes
                tris.append(_read(g, blob, p.indices).reshape(-1, 3))
        if attrs is None:
            continue
        pos = _read(g, blob, attrs.POSITION).astype(np.float64)
        cols = np.stack([_read(g, blob, getattr(attrs, f"COLOR_{i}"))[:, :3] for i in range(8)])
        cidx = _read(g, blob, attrs._COLOR_INDEX).astype(np.int64)
        trees.append(
            Tree(root.name, kind, pos, np.rint(cols * 255).astype(np.float64), cidx, np.concatenate(tris).astype(np.int64))
        )
    for t in trees:
        compute_normals(t)
    return trees


def compute_normals(t: Tree, weld: float = 1e-3) -> None:
    """Area-weighted face normals accumulated on position-welded vertices."""
    q = np.round(t.pos / weld).astype(np.int64)
    _, wid = np.unique(q, axis=0, return_inverse=True)
    wid = wid.ravel()
    a, b, c = (t.pos[t.tris[:, i]] for i in range(3))
    fn = np.cross(b - a, c - a)  # length = 2 * area
    acc = np.zeros((wid.max() + 1, 3))
    for i in range(3):
        np.add.at(acc, wid[t.tris[:, i]], fn)
    n = acc[wid]
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    t.normal = np.where(ln > 0, n / np.maximum(ln, 1e-12), 0.0)
    e = np.concatenate([wid[t.tris[:, [0, 1]]], wid[t.tris[:, [1, 2]]]])
    m = coo_matrix((np.ones(len(e)), (e[:, 0], e[:, 1])), shape=(len(acc), len(acc)))
    _, comp = connected_components(m, directed=False)
    t.component = comp[wid]
