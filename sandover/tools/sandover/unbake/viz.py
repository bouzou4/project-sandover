"""Rerun recordings and residual-colored glb copies of a level fit."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pygltflib
import rerun as rr


def heat(x: np.ndarray, vmax: float) -> np.ndarray:
    """Scalar -> u8 RGB ramp (black, red, yellow, white)."""
    t = np.clip(x / vmax, 0, 1)[:, None]
    ramp = np.array([[0, 0, 0], [200, 0, 0], [255, 220, 0], [255, 255, 255]], float)
    pos = t * 3
    i = np.minimum(pos.astype(int), 2)[:, 0]
    f = pos[:, 0] - i
    return (ramp[i] * (1 - f)[:, None] + ramp[i + 1] * f[:, None]).astype(np.uint8)


def to_rgb8(c: np.ndarray) -> np.ndarray:
    """Baked u8 (128 = 1.0) to displayable 8-bit, matching the shader's x2."""
    return np.clip(c * 2, 0, 255).astype(np.uint8)


def record(path: Path, level: str, x, tris, slots, baked, fitted, vis, dirs, table_dirs, points=None):
    """baked/fitted (N,S,3), vis (N,S), dirs/table_dirs (S,3); points: {slot: (pos, rgb, baked, pred)}."""
    rr.init(f"sandover-unbake-{level}", spawn=False)
    rr.save(str(path))
    center = x.mean(0)
    span = float(np.ptp(x, 0).max()) * 0.25
    views = ("baked", "fitted", "residual", "visibility")
    for v in views:
        rr.log(f"views/{v}", rr.Mesh3D(vertex_positions=x.astype(np.float32), triangle_indices=tris.astype(np.uint32)),
               static=True)

    def colors(name, col):
        rr.log(f"views/{name}", rr.Mesh3D.from_fields(vertex_colors=col))

    for j, s in enumerate(slots):
        rr.set_time("slot", sequence=int(s))
        colors("baked", to_rgb8(baked[:, j]))
        colors("fitted", to_rgb8(fitted[:, j]))
        colors("residual", heat(np.abs(fitted[:, j] - baked[:, j]).mean(1), 32.0))
        colors("visibility", heat(vis[:, j], 1.0))
        rr.log("lights/sun_fit", rr.Arrows3D(origins=[center], vectors=[dirs[j] * span], colors=[[255, 200, 0]]))
        rr.log("lights/sun_table", rr.Arrows3D(origins=[center], vectors=[table_dirs[j] * span], colors=[[0, 160, 255]]))
        rr.log("metrics/median_abs_err", rr.Scalars(float(np.median(np.abs(fitted[:, j] - baked[:, j])))))
    for s, (pos, rgb, b, p) in (points or {}).items():
        rr.set_time("slot", sequence=int(s))
        colors("baked", to_rgb8(b))
        colors("fitted", to_rgb8(p))
        colors("residual", heat(np.abs(p - b).mean(1), 32.0))
        rr.log("metrics/median_abs_err", rr.Scalars(float(np.median(np.abs(p - b)))))
        if len(pos):
            col = np.clip(rgb / np.maximum(rgb.max(1, keepdims=True), 1e-6) * 255, 0, 255).astype(np.uint8)
            rr.log("lights/points", rr.Points3D(pos, colors=col, radii=0.6))
    rr.disconnect()


def residual_glb(path: Path, x, tris, colors: dict[str, np.ndarray]):
    """Write a minimal glb: one mesh, COLOR_0.. = the given u8 RGB layers (names go in extras)."""
    x = x.astype(np.float32)
    idx = tris.astype(np.uint32).ravel()
    blobs = [x.tobytes(), idx.tobytes()] + [
        (np.concatenate([c, np.full((len(c), 1), 255, np.uint8)], 1)).tobytes() for c in colors.values()
    ]
    offsets = np.cumsum([0] + [len(b) for b in blobs])
    g = pygltflib.GLTF2(scene=0, scenes=[pygltflib.Scene(nodes=[0])], nodes=[pygltflib.Node(mesh=0)])
    g.buffers = [pygltflib.Buffer(byteLength=int(offsets[-1]))]
    for i, b in enumerate(blobs):
        g.bufferViews.append(pygltflib.BufferView(buffer=0, byteOffset=int(offsets[i]), byteLength=len(b)))
    g.accessors.append(pygltflib.Accessor(bufferView=0, componentType=5126, count=len(x), type="VEC3",
                                          min=x.min(0).tolist(), max=x.max(0).tolist()))
    g.accessors.append(pygltflib.Accessor(bufferView=1, componentType=5125, count=len(idx), type="SCALAR"))
    attrs = {"POSITION": 0}
    for i, _ in enumerate(colors):
        g.accessors.append(pygltflib.Accessor(bufferView=2 + i, componentType=5121, normalized=True,
                                              count=len(x), type="VEC4"))
        attrs[f"COLOR_{i}"] = 2 + i
    g.meshes = [pygltflib.Mesh(primitives=[pygltflib.Primitive(attributes=pygltflib.Attributes(**attrs), indices=1)],
                               extras={"color_layers": list(colors)})]
    g.set_binary_blob(b"".join(blobs))
    g.save_binary(str(path))
