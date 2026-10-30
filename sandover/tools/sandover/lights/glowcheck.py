"""Is a background texture drawn brighter than its surroundings in a vanilla frame?

A prop light is only right where the game draws its source visibly brighter than what is around
it. For a rig capture (``<frame>.png`` + sidecar ``.json`` with ``camera_trans``/``camera_rot``,
16:9, the rig's 80 degree horizontal field of view) the level background is z-buffered at quarter
resolution into a texture-id map. Per visible texture:

- ``contrast``: median sRGB luma of its pixels over the median of the other textures' pixels
  whose surface lies within ``radius_m`` of it (world space);
- ``saturation``: (max - min) / max of its mean sRGB colour (a self-lit surface reads vivid, a pale
  matte one does not);
- ``ceiling``: share of its pixels, and of those surroundings, with a channel at 250 or more (the
  SDR ceiling: drawn brighter than the display shows).

Jak and the actors near him are masked with boxes. ``props.glows_in_vanilla`` turns the
numbers recorded in ``prop_lights.json`` into the bake's decision.

    uv run --no-sync python -m sandover.lights glowcheck FRAME.png --levels maincave [--textures ...]
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from sandover.unbake.glb import SKIP_TREES, _read

Y = np.array([0.299, 0.587, 0.114])
HALF_FOV_X = np.radians(40.0)
SCALE = 4
CEILING = 250 / 255


def _draws(glb: Path) -> list[dict]:
    """Background draws: texture name, world positions (m) and triangles, instanced ties placed."""
    import pygltflib

    from sandover.materials.emissive_table import _material_texture_name

    g = pygltflib.GLTF2().load(str(glb))
    blob = g.binary_blob()
    cache: dict = {}
    out = []
    for ri in g.scenes[g.scene or 0].nodes:
        root = g.nodes[ri]
        if any(s in (root.name or "") for s in SKIP_TREES):
            continue
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None:
                continue
            for p in g.meshes[node.mesh].primitives:
                if p.indices is None:
                    continue
                k = p.attributes.POSITION
                if k not in cache:
                    cache[k] = _read(g, blob, k).astype(np.float64)
                pos = cache[k]
                if node.matrix is not None:
                    m = np.array(node.matrix, np.float64).reshape(4, 4).T
                    pos = pos @ m[:3, :3].T + m[:3, 3]
                out.append({"tex": _material_texture_name(g, p.material) or "", "pos": pos,
                            "tris": _read(g, blob, p.indices).astype(np.int64).reshape(-1, 3)})
    return out


def zbuffer(draws: list[dict], cam: np.ndarray, rot: np.ndarray, size: tuple[int, int],
            reach_m: float = 80.0) -> tuple[np.ndarray, np.ndarray]:
    """Per pixel of a ``size`` (w, h) image: the draw index (-1 = none) and the world centre of
    the nearest triangle."""
    w, h = size
    tx = np.tan(HALF_FOV_X)
    ty = tx * h / w
    zb = np.full((h, w), np.inf)
    tid = np.full((h, w), -1, np.int64)
    wp = np.zeros((h, w, 3))
    for di, d in enumerate(draws):
        P = d["pos"][d["tris"]]
        c = P.mean(1)
        m = np.linalg.norm(c - cam, axis=1) < reach_m
        v = np.einsum("ij,nkj->nki", rot, P[m] - cam)
        ok = (v[:, :, 2] > 0.1).all(1)
        v, c = v[ok], c[m][ok]
        x = w / 2 - v[:, :, 0] / v[:, :, 2] / tx * w / 2
        y = h / 2 - v[:, :, 1] / v[:, :, 2] / ty * h / 2
        z = v[:, :, 2]
        for k in range(len(v)):
            x0, x1 = int(max(0, np.floor(x[k].min()))), int(min(w - 1, np.ceil(x[k].max())))
            y0, y1 = int(max(0, np.floor(y[k].min()))), int(min(h - 1, np.ceil(y[k].max())))
            if x0 > x1 or y0 > y1:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            (ax, bx, cx), (ay, by, cy) = x[k], y[k]
            den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            if abs(den) < 1e-9:
                continue
            l1 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
            l2 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / den
            l3 = 1 - l1 - l2
            inside = (l1 >= 0) & (l2 >= 0) & (l3 >= 0)
            zz = 1.0 / (l1 / z[k, 0] + l2 / z[k, 1] + l3 / z[k, 2])
            sub = zb[y0:y1 + 1, x0:x1 + 1]
            upd = inside & (zz < sub)
            sub[upd] = zz[upd]
            tid[y0:y1 + 1, x0:x1 + 1][upd] = di
            wp[y0:y1 + 1, x0:x1 + 1][upd] = c[k]
    return tid, wp


def actor_mask(meta: dict, actors: list[dict], cam: np.ndarray, rot: np.ndarray,
               size: tuple[int, int]) -> np.ndarray:
    """Screen boxes over Jak (3.4 m tall) and the actors within 25 m of him (2 m)."""
    w, h = size
    tx = np.tan(HALF_FOV_X)
    ty = tx * h / w
    m = np.zeros((h, w), bool)
    if not meta.get("jak_pos"):
        return m
    jak = np.array(meta["jak_pos"][:3]) / 4096
    pts = [(jak, 1.2, 3.4)] + [(np.array(a["trans"][:3]), 1.0, 2.0) for a in actors
                               if np.linalg.norm(np.array(a["trans"][:3]) - jak) < 25]
    for p, r, hgt in pts:
        box = np.array([p + [dx, dy, dz] for dx in (-r, r) for dy in (-0.2, hgt) for dz in (-r, r)])
        v = (rot @ (box - cam).T).T
        if (v[:, 2] <= 0.1).any():
            continue
        x = w / 2 - v[:, 0] / v[:, 2] / tx * w / 2
        y = h / 2 - v[:, 1] / v[:, 2] / ty * h / 2
        m[int(max(0, y.min())):int(min(h, y.max() + 1)), int(max(0, x.min())):int(min(w, x.max() + 1))] = True
    return m


def measure(png: Path, levels: list[str], data: Path, repo: Path, textures: set[str] | None = None,
            radius_m: float = 5.0, min_px: int = 150) -> list[dict]:
    """Per visible texture of ``levels`` (all, or ``textures``): contrast and ceiling shares."""
    from PIL import Image
    from scipy.spatial import cKDTree

    meta = json.loads(png.with_suffix(".json").read_text(encoding="utf-8"))
    W, H = meta["resolution"]
    size = (W // SCALE, H // SCALE)
    cam = np.array(meta["camera_trans"][:3]) / 4096
    rot = np.array(meta["camera_rot"]).reshape(3, 3)
    draws, actors = [], []
    for lv in levels:
        draws += _draws(data / "unbake/riproot/decompiler_out/jak1/levels" / lv / f"{lv}-background.glb")
        ap = repo / "decompiler_out/jak1/entities" / f"{lv}-actors.json"
        if ap.is_file():
            actors += json.loads(ap.read_text(encoding="utf-8"))
    tid, wp = zbuffer(draws, cam, rot, size)
    valid = (tid >= 0) & ~actor_mask(meta, actors, cam, rot, size)
    img = np.asarray(Image.open(png).convert("RGB").resize(size, Image.BOX), np.float64) / 255.0
    lum = img @ Y
    top = img.max(2) >= CEILING
    names = np.array([d["tex"] for d in draws] + [""])[tid]
    out = []
    for t in sorted(set(names[valid]) - {""}):
        if textures is not None and t not in textures:
            continue
        em = valid & (names == t)
        if em.sum() < min_px:
            continue
        pts = wp[em]
        dd, _ = cKDTree(pts[:: max(1, len(pts) // 3000)]).query(wp.reshape(-1, 3))
        gr = valid & (names != t) & (dd.reshape(names.shape) < radius_m)
        if gr.sum() < min_px:
            continue
        e, g = float(np.median(lum[em])), float(np.median(lum[gr]))
        mean = img[em].mean(0)
        sat = float((mean.max() - mean.min()) / max(mean.max(), 1e-3))
        out.append({"texture": t, "px": int(em.sum()), "contrast": round(e / max(g, 1e-3), 2),
                    "saturation": round(sat, 2),
                    "ceiling": [round(float(top[em].mean()), 2), round(float(top[gr].mean()), 2)],
                    "rgb": [(img[em].mean(0) * 255).round().tolist(), (img[gr].mean(0) * 255).round().tolist()]})
    return out
