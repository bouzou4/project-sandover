"""Per-level bounds and budget from the decompiler's ``<level>-background.glb`` exports.

The exporter writes LOD 0 only (tfrag tree 0, tie tree 0, all shrubs) with positions
already divided by 4096, i.e. in metres. Output: ``levels.json`` and ``levels.csv``
(one row per level and tree kind) under ``$SANDOVER_DATA/vista``.
"""

from __future__ import annotations

import argparse
import base64
import csv
import json
import re
import struct
from collections import defaultdict
from pathlib import Path

import numpy as np
import pygltflib

from sandover.census import sandover_data_dir

VERTEX_BYTES = 32  # tfrag PreloadedVertex, ShrubGpuVertex and tie Vertex are all 32 B
INDEX_BYTES = 4
MIP_FACTOR = 4.0 / 3.0
LORES = re.compile(r"lo-?res|lowres|medres|med-res")


def is_standin(kind: str, name: str) -> bool:
    return kind.startswith("tfrag-lowres") or bool(LORES.search(name))


def tree_kind(node_name: str, level: str) -> str:
    """``village1-tfrag-lowres`` -> ``tfrag-lowres``; ``village1-tie-2`` -> ``tie``."""
    rest = node_name[len(level) + 1 :] if node_name.startswith(level + "-") else node_name
    if rest.startswith("tfrag-"):
        return rest
    return rest.split("-")[0]


def node_matrix(node: pygltflib.Node) -> np.ndarray:
    if not node.matrix:
        return np.eye(4)
    m = np.array(node.matrix, dtype=np.float64).reshape(4, 4).T
    m[3] = [0, 0, 0, 1]  # wind matrices carry a non-affine w; only the 3x4 part is a transform
    return m


def png_size(uri: str) -> tuple[int, int]:
    head = base64.b64decode(uri.split(",", 1)[1][:44])
    return struct.unpack(">II", head[16:24])


def fr3_uncompressed_bytes(fr3: Path) -> int | None:
    """The fr3 container starts with the decompressed size as a little-endian u64."""
    if not fr3.exists():
        return None
    with fr3.open("rb") as f:
        return struct.unpack("<Q", f.read(8))[0]


class Reader:
    def __init__(self, g: pygltflib.GLTF2):
        self.g = g
        self.blob = g.binary_blob()
        self.cache: dict[int, np.ndarray] = {}

    def positions(self, acc_idx: int) -> np.ndarray:
        if acc_idx in self.cache:
            return self.cache[acc_idx]
        acc = self.g.accessors[acc_idx]
        bv = self.g.bufferViews[acc.bufferView]
        off = (bv.byteOffset or 0) + (acc.byteOffset or 0)
        stride = bv.byteStride or 12
        flat = np.frombuffer(self.blob, dtype=np.float32,
                             count=(stride * (acc.count - 1) + 12) // 4, offset=off)
        p = flat.reshape(-1, 3) if stride == 12 else np.lib.stride_tricks.as_strided(
            flat, shape=(acc.count, 3), strides=(stride, 4)).copy()
        self.cache[acc_idx] = p
        return p

    def indices(self, acc_idx: int) -> np.ndarray:
        acc = self.g.accessors[acc_idx]
        bv = self.g.bufferViews[acc.bufferView]
        dt = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32}[acc.componentType]
        off = (bv.byteOffset or 0) + (acc.byteOffset or 0)
        return np.frombuffer(self.blob, dtype=dt, count=acc.count, offset=off)


def bbox(p: np.ndarray) -> list[list[float]]:
    return [p.min(axis=0).round(2).tolist(), p.max(axis=0).round(2).tolist()]


def level_stats(glb: Path, level: str, outlier_m: float, points_out: Path | None = None) -> dict:
    g = pygltflib.GLTF2().load(str(glb))
    r = Reader(g)
    kinds: dict[str, dict] = defaultdict(
        lambda: {"tris": 0, "verts": 0, "pts": [], "accs": set(), "placed": set()})
    objects: list[dict] = []
    detail_pts: list[np.ndarray] = []

    def walk(idx: int, parent: np.ndarray, kind: str, top: str) -> None:
        node = g.nodes[idx]
        world = parent @ node_matrix(node)
        identity = np.allclose(world, np.eye(4))
        if node.mesh is not None:
            k = kinds[kind]
            pts_node = []
            for prim in g.meshes[node.mesh].primitives:
                pa = prim.attributes.POSITION
                if prim.indices is not None:
                    k["tris"] += g.accessors[prim.indices].count // 3
                p = r.positions(pa)
                if not identity:
                    p = p @ world[:3, :3].T + world[:3, 3]
                if pa not in k["accs"]:
                    k["accs"].add(pa)
                    k["verts"] += len(p)
                used = p[np.unique(r.indices(prim.indices))] if prim.indices is not None else p
                placed = (prim.indices, pa, None if identity else world.tobytes())
                if placed not in k["placed"]:
                    k["placed"].add(placed)
                    k["pts"].append(used)  # unreferenced vertices hold junk far outside the level
                pts_node.append(used)
            if pts_node:
                pn = np.concatenate(pts_node)
                if not is_standin(kind, node.name):
                    detail_pts.append(pn)
                objects.append({"kind": kind, "tree": top, "name": node.name,
                                "center": pn.mean(axis=0), "bbox": bbox(pn), "pts": pn})
        for c in node.children or []:
            walk(c, world, kind, top)

    for idx in g.scenes[0].nodes:
        name = g.nodes[idx].name
        walk(idx, np.eye(4), tree_kind(name, level), name)

    out: dict = {"level": level, "kinds": {}}
    all_pts = []
    for kind, k in sorted(kinds.items()):
        pts = np.concatenate(k["pts"]) if k["pts"] else np.zeros((0, 3), np.float32)
        all_pts.append(pts)
        out["kinds"][kind] = {"tris": k["tris"], "verts": k["verts"],
                              "bbox_m": bbox(pts) if len(pts) else None}
    pts = np.concatenate(all_pts)
    lo, hi = np.percentile(pts, 1, axis=0), np.percentile(pts, 99, axis=0)
    out["bbox_m"] = bbox(pts)
    out["core_bbox_m"] = [lo.round(2).tolist(), hi.round(2).tolist()]
    out["tris"] = sum(v["tris"] for v in out["kinds"].values())
    out["verts"] = sum(v["verts"] for v in out["kinds"].values())

    if points_out is not None and detail_pts:
        # 0.25 m voxel grid keeps the seam test exact enough and the file small
        q = np.unique(np.round(np.concatenate(detail_pts) * 4).astype(np.int32), axis=0)
        np.save(points_out / f"{level}.npy", q.astype(np.float32) / 4)
    if points_out is not None:
        sample = {f"{i}": o["pts"][:: max(1, len(o["pts"]) // 2000)]
                  for i, o in enumerate(x for x in objects if is_standin(x["kind"], x["name"]))}
        np.savez(points_out / f"{level}-standins.npz", **sample)
    detail = [o for o in objects if not is_standin(o["kind"], o["name"])]
    if detail:
        mins = np.array([o["bbox"][0] for o in detail]).min(axis=0)
        maxs = np.array([o["bbox"][1] for o in detail]).max(axis=0)
        out["detail_bbox_m"] = [mins.round(2).tolist(), maxs.round(2).tolist()]
    out["standins"] = [{"kind": o["kind"], "tree": o["tree"], "name": o["name"],
                        "center_m": o["center"].round(1).tolist(), "bbox_m": o["bbox"]}
                       for o in objects if is_standin(o["kind"], o["name"])]

    far = []
    for o in objects:
        d = np.maximum(0, np.maximum(lo - o["center"], o["center"] - hi))
        dist = float(np.linalg.norm(d))
        if dist > outlier_m:
            far.append({"kind": o["kind"], "tree": o["tree"], "name": o["name"],
                        "center_m": o["center"].round(1).tolist(), "bbox_m": o["bbox"],
                        "outside_core_m": round(dist, 1)})
    out["outliers"] = sorted(far, key=lambda x: -x["outside_core_m"])

    tex = {}
    for im in g.images:
        if im.uri and im.uri.startswith("data:") and not (im.name or "").endswith("-envmap-strength"):
            tex[im.name] = png_size(im.uri)
    tex_bytes = sum(w * h * 4 for w, h in tex.values())
    geo = out["verts"] * VERTEX_BYTES + out["tris"] * 3 * INDEX_BYTES
    out["textures"] = {"count": len(tex), "rgba8_bytes": tex_bytes,
                       "names": sorted(tex)}
    out["vram_estimate_bytes"] = {"geometry_lod0": geo, "textures_mip": int(tex_bytes * MIP_FACTOR),
                                  "total": geo + int(tex_bytes * MIP_FACTOR)}
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-vista-stats", description=__doc__)
    rip = sandover_data_dir() / "unbake" / "riproot"
    ap.add_argument("--levels-dir", type=Path, default=rip / "decompiler_out" / "jak1" / "levels")
    ap.add_argument("--fr3-dir", type=Path, default=rip / "out" / "jak1" / "fr3")
    ap.add_argument("--out", type=Path, default=sandover_data_dir() / "vista")
    ap.add_argument("--outlier-m", type=float, default=50.0)
    ap.add_argument("levels", nargs="*")
    a = ap.parse_args(argv)
    glbs = sorted(a.levels_dir.glob("*/*-background.glb"))
    if a.levels:
        glbs = [p for p in glbs if p.parent.name in a.levels]
    if not glbs:
        raise SystemExit(f"no *-background.glb under {a.levels_dir}")
    (a.out / "points").mkdir(parents=True, exist_ok=True)
    results = []
    for glb in glbs:
        level = glb.parent.name
        s = level_stats(glb, level, a.outlier_m, a.out / "points")
        s["fr3_uncompressed_bytes"] = fr3_uncompressed_bytes(a.fr3_dir / f"{level}.fr3")
        results.append(s)
        print(f"{level:12s} tris={s['tris']:>9,} verts={s['verts']:>9,} "
              f"tex={s['textures']['rgba8_bytes'] / 2**20:7.1f} MiB outliers={len(s['outliers'])}")
    (a.out / "levels.json").write_text(json.dumps(results, indent=1))
    with (a.out / "levels.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["level", "kind", "tris", "verts", "min_x", "min_y", "min_z",
                    "max_x", "max_y", "max_z"])
        for s in results:
            for kind, k in list(s["kinds"].items()) + [("all", s)]:
                bb = k["bbox_m"] or [[None] * 3, [None] * 3]
                w.writerow([s["level"], kind, k["tris"], k["verts"], *bb[0], *bb[1]])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
