"""Per-vertex lighting sidecar (`<level>.unbake`) from the unbake fits, in engine vertex order.

The glb exporter writes each tree's vertex buffer straight from ``unpacked.vertices``, the
same array the engine loader uploads, so glb vertex i of a tree is engine vertex i. The fit
arrays only hold the vertices drawn by non-wind triangles, in ``cli.gather`` order; this module
recovers that mapping per tree and checks it on positions.

File layout (little-endian)::

    header   char[4] "SOVU", u32 version, u32 fr3_version, u32 tree_count,
             u32 slot_mask, u32 day_mask, u32 night_mask, u32 emitter_mask      32 B
    slots x8 f32 dir_fit[3], f32 pad, f32 lgt[3], f32 pad, f32 amb[3], f32 pad,
             f32 dir_table[3], f32 pad                                          64 B each
    trees    u32 kind (0 tfrag, 1 tie), u32 fr3_tree, u32 vertex_count, u32 pad,
             u64 pos_digest, u64 data_offset                                    32 B each
    records  per vertex, one RGBA32UI texel:
             x = a | k << 16 (u16 each, value * 16384), y = octahedral normal as
             two u16 ((o + 1) / 2 * 65535),
             z = v, 4 bits per palette slot (slot s at bits 4s..4s+3, 15 = 1.0),
             w = q (u8, 255 = full confidence) | FLAG_NORMAL | FLAG_WINDING
                 | E << 16 (u8, emissive 0..1) | E source << 24 (4 bits, see emissive.py)
             The normal is area-weighted and smoothed over position-welded vertices for every
             statically drawn vertex, fitted or not; fitted ones carry the fit's orientation.

Version 2 added E, ``night_mask`` (the slots E's night source is weighted by) and
``emitter_mask`` (effect slots that are light sources, see emissive.py). E is
computed from the glb colours alone, so a level without a fit still gets a sidecar (normals and
E, q = 0, empty slot table). Human verdicts (``materials/emissive-overrides.csv``, see
overrides.py) then force E to 1 or 0 per texture; the format and version are unchanged.

``a``, ``k``, ``lgt`` and ``amb`` are in the fit's units: palette u8 steps, 128 = 1.0.
``pos_digest`` is :func:`pos_digest` over the engine's float32 positions (game units).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pygltflib

from . import emissive, overrides
from .glb import SKIP_TREES, _read
from .levels import LEVELS

MAGIC = b"SOVU"
VERSION = 2
TFRAG3_VERSION = 43
KIND = {"tfrag": 0, "tie": 1}
_M1 = np.uint64(0x9E3779B97F4A7C15)
FLAG_NORMAL = 1 << 8  # w bit: normal defined
FLAG_WINDING = 1 << 9  # w bit: orientation from triangle winding, not chosen by the fit
AK_SCALE = 16384.0  # a and k as unorm16 fixed point, range [0, 4)


@dataclass
class GlbTree:
    kind: str  # tfrag | tie | shrub
    fr3_index: int
    name: str
    pos: np.ndarray  # (V,3) float32, glb units (game units / 4096)
    used: np.ndarray | None  # sorted vertex ids drawn by non-wind triangles; None if not fitted
    tris: np.ndarray | None = None  # (T,3) non-wind triangles
    lum: np.ndarray | None = None  # (8,V) palette luminance, u8 steps
    prims: list[tuple[str, np.ndarray]] | None = None  # (texture name, vertex ids) per primitive


def pos_digest(pos_game: np.ndarray) -> int:
    """Order-sensitive 64-bit digest of float32 positions: sum over j of (bits_j + 1) * (j + 1) * M."""
    bits = np.ascontiguousarray(pos_game, dtype=np.float32).view(np.uint32).ravel().astype(np.uint64)
    j = np.arange(1, len(bits) + 1, dtype=np.uint64)
    with np.errstate(over="ignore"):
        return int(((bits + np.uint64(1)) * (j * _M1)).sum(dtype=np.uint64))


def scan_glb(path: Path) -> list[GlbTree]:
    """Trees of a background glb in file order, with the vertex subset the fit used."""
    g = pygltflib.GLTF2().load(str(path))
    blob = g.binary_blob()
    out, n_tfrag = [], 0
    for root_idx in g.scenes[0].nodes:
        root = g.nodes[root_idx]
        if "-tfrag-" in root.name:
            kind, idx = "tfrag", n_tfrag
            n_tfrag += 1
        elif "-tie-" in root.name:
            kind, idx = "tie", int(root.name.rsplit("-", 1)[1])
        elif "-shrub-" in root.name:
            kind, idx = "shrub", int(root.name.rsplit("-", 1)[1])
        else:
            continue
        tris, attrs, prims = [], None, []
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None:
                continue
            for p in g.meshes[node.mesh].primitives:
                attrs = attrs or p.attributes
                if node.matrix is None:
                    tris.append(_read(g, blob, p.indices))
                tex = _texture_name(g, p.material)
                if tex and p.indices is not None:
                    prims.append((tex, np.unique(_read(g, blob, p.indices).astype(np.int64))))
        if attrs is None:
            continue
        pos = _read(g, blob, attrs.POSITION).astype(np.float32)
        lum = None
        if getattr(attrs, "COLOR_7", None) is not None:
            cols = np.stack([_read(g, blob, getattr(attrs, f"COLOR_{i}"))[:, :3] for i in range(8)])
            lum = emissive.luminance(np.rint(cols.astype(np.float64) * 255))
        fitted = tris and not any(s in root.name for s in SKIP_TREES)
        used = np.unique(np.concatenate(tris).astype(np.int64)) if fitted else None
        tri = np.concatenate(tris).astype(np.int64).reshape(-1, 3) if tris else None
        out.append(GlbTree(kind, idx, root.name, pos, used, tri, lum, prims))
    return out


def _texture_name(g: pygltflib.GLTF2, material: int | None) -> str | None:
    """A primitive's base-colour texture name (glTF texture name, else its image's)."""
    if material is None or not (0 <= material < len(g.materials)):
        return None
    pbr = g.materials[material].pbrMetallicRoughness
    ref = pbr.baseColorTexture if pbr else None
    if ref is None:
        return None
    tex = g.textures[ref.index]
    return tex.name or (g.images[tex.source].name if tex.source is not None else None) or None


def vertex_scale(tree: GlbTree, factors: dict[str, float]) -> np.ndarray | None:
    """Per-vertex E multiplier from per-texture factors; None when no primitive's texture has one.

    A vertex drawn by several textures takes the largest factor, an unlisted texture counting
    as 1, so a texture turned down never dims a vertex another texture shares.
    """
    if not factors or not tree.prims or not any(t in factors for t, _ in tree.prims):
        return None
    f = np.full(len(tree.pos), -1.0)  # -1: drawn by no primitive (a factor may be 0)
    for tex, idx in tree.prims:
        idx = idx[idx < len(f)]
        f[idx] = np.maximum(f[idx], factors.get(tex, 1.0))
    f[f < 0] = 1.0
    return f


def scan_glb_tree_names(gltf_json: dict) -> dict[str, tuple[int, int]]:
    """Root node name -> (kind, fr3_tree), JSON-only (no binary chunk needed).

    Mirrors :func:`scan_glb`'s kind/index assignment for tfrag/tie trees (the two kinds a
    sidecar keeps in glb vertex order, per this module's docstring), so a caller with only
    the glb's JSON chunk -- e.g. the viewer server's ``read_glb_json`` -- can resolve a
    clicked primitive's tree without opening the binary chunk. Shrub trees are omitted: the
    sidecar does not promise their vertex order matches the glb.
    """
    nodes = gltf_json.get("nodes", [])
    scenes = gltf_json.get("scenes", [])
    scene = scenes[gltf_json.get("scene", 0)] if scenes else {"nodes": []}
    out: dict[str, tuple[int, int]] = {}
    n_tfrag = 0
    for idx in scene.get("nodes", []):
        name = nodes[idx].get("name", "") if 0 <= idx < len(nodes) else ""
        if any(s in name for s in SKIP_TREES):
            continue
        if "-tfrag-" in name:
            out[name] = (KIND["tfrag"], n_tfrag)
            n_tfrag += 1
        elif "-tie-" in name:
            out[name] = (KIND["tie"], int(name.rsplit("-", 1)[1]))
    return out


def octahedral(n: np.ndarray) -> np.ndarray:
    """Unit normals (N,3) to octahedral coordinates in [-1, 1]^2."""
    n = n / np.maximum(np.abs(n).sum(1, keepdims=True), 1e-12)
    x, y, z = n[:, 0], n[:, 1], n[:, 2]
    ox = np.where(z >= 0, x, (1 - np.abs(y)) * np.where(x >= 0, 1, -1))
    oy = np.where(z >= 0, y, (1 - np.abs(x)) * np.where(y >= 0, 1, -1))
    return np.stack([ox, oy], 1)


def smooth_normals(pos: np.ndarray, tris: np.ndarray, weld: float = 1e-3) -> tuple[np.ndarray, np.ndarray]:
    """Area-weighted face normals summed over position-welded vertices (the fit's normals).

    Returns (normals, defined mask). Orientation follows the triangle winding, which the rip
    does not keep outward-consistent; the fit picks a sign per connected component instead.
    """
    wid = np.unique(np.round(pos.astype(np.float64) / weld).astype(np.int64), axis=0, return_inverse=True)[1].ravel()
    p = pos.astype(np.float64)
    fn = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    acc = np.zeros((wid.max() + 1, 3))
    for i in range(3):
        np.add.at(acc, wid[tris[:, i]], fn)
    n = acc[wid]
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    ok = ln[:, 0] > 1e-12
    return np.where(ok[:, None], n / np.maximum(ln, 1e-12), 0.0), ok


def confidence(baked: np.ndarray, fitted: np.ndarray, slots: np.ndarray, day_slots: list[int], lo: float,
               hi: float) -> np.ndarray:
    """q in [0,1] from the worst day-slot residual (mean over RGB): 1 at <= lo u8, 0 at >= hi u8.

    ``day_slots`` are the level's sun slots without its night slots (:func:`emissive.roles`)."""
    day = np.isin(slots, day_slots)
    r = np.abs(baked[:, day].astype(np.float32) - fitted[:, day]).mean(2).max(1)
    return np.clip((hi - r) / (hi - lo), 0.0, 1.0)


def day_slot_mask(slots, er: emissive.EmissiveRoles) -> int:
    """Header day_mask: the fitted slots the level's hour table gives the sun, never its night slots."""
    return sum(1 << int(s) for s in slots if int(s) in er.day)


def _u16(x: np.ndarray, scale: float) -> np.ndarray:
    return np.clip(np.rint(np.asarray(x, np.float64) * scale), 0, 65535).astype(np.uint32)


def pack_records(a, k, normal, v, slots, q) -> np.ndarray:
    """(N,4) uint32 texels."""
    rec = np.zeros((len(a), 4), np.uint32)
    rec[:, 0] = _u16(a, AK_SCALE) | (_u16(k, AK_SCALE) << 16)
    o = (np.clip(octahedral(normal.astype(np.float64)), -1, 1) + 1) * 0.5
    rec[:, 1] = _u16(o[:, 0], 65535) | (_u16(o[:, 1], 65535) << 16)
    vq = np.rint(np.clip(v.astype(np.float32), 0, 1) * 15).astype(np.uint32)
    for j, s in enumerate(slots):
        rec[:, 2] |= vq[:, j] << np.uint32(4 * int(s))
    rec[:, 3] = np.rint(np.clip(q, 0, 1) * 255).astype(np.uint32) | FLAG_NORMAL
    return rec


def pack_emissive(e: np.ndarray, src: np.ndarray) -> np.ndarray:
    """E (0..1) and its source index, for the upper bits of record.w."""
    eq = np.rint(np.clip(e, 0, 1) * 255).astype(np.uint32)
    return (eq << np.uint32(16)) | (np.where(eq > 0, src, 0).astype(np.uint32) << np.uint32(24))


def build(level: str, glb: Path, fit_dir: Path, lo: float, hi: float, k_max: float,
          e_factors: dict[str, float] | None = None,
          ov: dict[str, str] | None = None, raw: list[bytes] | None = None) -> tuple[bytes, dict]:
    """Sidecar bytes and report for one level. ``e_factors`` (texture name -> multiplier, from
    ``emissive-strengths.csv`` and the emitter score, see :func:`export_factors`) scales E per
    texture before packing; ``ov`` (texture name -> verdict, from ``emissive-overrides.csv``,
    see overrides.py) then forces E to 1 or 0. When ``raw`` is a list, the same sidecar with
    unscaled, unforced E is appended to it (what the emissive tools scan)."""
    trees = scan_glb(glb)
    if not any(t.lum is not None for t in trees if t.kind in KIND):
        raise ValueError(f"{level}: glb has no COLOR_7, rip with kMaxColor = 8")
    er = emissive.roles(level)
    er.night_ratio = emissive.night_ratio(np.concatenate([t.lum for t in trees if t.kind in KIND and t.lum is not None], 1), er)
    fit_path = fit_dir / f"{level}-fit.npz"
    fit = np.load(fit_path) if fit_path.exists() else None
    if fit is None:
        for t in trees:
            t.used = None
    summ = json.loads((fit_dir / f"{level}-summary.json").read_text()) if fit is not None else {"slots": []}
    slots = np.asarray(fit["slots"]) if fit is not None else np.zeros(0, np.int64)

    fitted_trees = [t for t in trees if t.used is not None]
    x = np.concatenate([t.pos[t.used] for t in fitted_trees]) if fitted_trees else np.zeros((0, 3), np.float32)
    q_all = np.zeros(0)
    if fit is not None:
        if x.shape != fit["x"].shape or not np.array_equal(x, fit["x"]):
            raise ValueError(f"{level}: glb vertex subset does not match the fit arrays")
        q_all = confidence(fit["baked"], fit["fitted"], slots, er.day, lo, hi)
        k_all = np.asarray(fit["k"], np.float64)
        q_all[(fit["kind"] == "shrub") | (k_all > k_max)] = 0.0
        k_all = np.minimum(k_all, k_max)

    report = {"level": level, "trees": [], "vertices": 0, "fitted_vertices": int(len(x)), "q_mean": 0.0,
              "has_fit": fit is not None,
              "emissive": {"effect_slots": er.effect, "emitter_slots": er.emitter, "night_slots": er.night,
                           "night_ratio": er.night_ratio,
                           "day_slots": er.day}}
    e_hist = np.zeros(4, np.int64)
    n_ov = 0
    scaled: set[str] = set()
    blocks, raw_blocks, table, off = [], [], [], 0
    for t in [t for t in trees if t.kind in KIND]:
        V = len(t.pos)
        rec = np.zeros((V, 4), np.uint32)
        if t.tris is not None and len(t.tris):
            n_s, ok = smooth_normals(t.pos, t.tris)
            o = (np.clip(octahedral(n_s[ok]), -1, 1) + 1) * 0.5
            rec[ok, 1] = _u16(o[:, 0], 65535) | (_u16(o[:, 1], 65535) << 16)
            rec[ok, 3] = FLAG_NORMAL | FLAG_WINDING
        if t.used is not None:
            s = off
            off += len(t.used)
            sl = slice(s, off)
            rec[t.used] = pack_records(fit["a"][sl], k_all[sl], fit["normal"][sl], fit["v"][sl], slots, q_all[sl])
        rec_raw = rec
        if t.lum is not None:
            e, src = emissive.compute(t.lum, er)
            if raw is not None:
                rec_raw = rec.copy()
                rec_raw[:, 3] |= pack_emissive(e, src)
            f = vertex_scale(t, e_factors or {})
            if f is not None:
                e = np.clip(e * f, 0.0, 1.0).astype(np.float32)
                scaled |= {tex for tex, _ in t.prims if tex in e_factors}
            rec[:, 3] |= pack_emissive(e, src)
            if ov and t.prims:
                w = rec[:, 3].copy()
                n_ov += overrides.apply(ov, w, overrides.texture_vertices(t.prims))
                rec[:, 3] = w
                e = ((w >> 16) & 0xFF) / 255.0
            e_hist += np.histogram(e, bins=[0, 0.5 / 255, 0.1, 0.4, 1.01])[0]
        table.append((KIND[t.kind], t.fr3_index, V, pos_digest(t.pos * np.float32(4096.0))))
        blocks.append(rec.tobytes())
        raw_blocks.append(rec_raw.tobytes())
        report["trees"].append({"name": t.name, "kind": t.kind, "fr3_tree": t.fr3_index, "vertices": V,
                                "fitted": 0 if t.used is None else int(len(t.used)),
                                "q_mean": float(((rec[:, 3] & 0xFF) / 255.0).mean()) if V else 0.0,
                                "e_nonzero": int((((rec[:, 3] >> 16) & 0xFF) > 0).sum()),
                                "digest": f"{table[-1][3]:016x}"})
        report["vertices"] += V
    shrub_n = int(sum(len(t.used) for t in fitted_trees if t.kind == "shrub"))
    if off + shrub_n != len(x):
        raise ValueError(f"{level}: fit rows {len(x)} != mapped {off} + shrub {shrub_n}")

    slot_mask = sum(1 << int(s) for s in slots)
    day_mask = day_slot_mask(slots, er)
    hdr = MAGIC + struct.pack("<7I", VERSION, TFRAG3_VERSION, len(table), slot_mask, day_mask, er.night_mask,
                               er.emitter_mask)
    glob = bytearray(64 * 8)
    for row in summ["slots"]:
        s = int(row["slot"])
        struct.pack_into("<16f", glob, 64 * s, *row["dir_fit"], 0.0, *row["lgt_fit_u8"], 0.0,
                         *row["amb_fit_u8"], 0.0, *row["dir_table"], 0.0)
    data_start = len(hdr) + len(glob) + 32 * len(table)
    tbl, pos = bytearray(), data_start
    for (kind, idx, V, dig), blk in zip(table, blocks):
        tbl += struct.pack("<4I2Q", kind, idx, V, 0, dig, pos)
        pos += len(blk)
    body = hdr + bytes(glob) + bytes(tbl) + b"".join(blocks)
    if raw is not None:
        raw.append(hdr + bytes(glob) + bytes(tbl) + b"".join(raw_blocks))
    q_fit = q_all[: off]
    report["q_mean"] = float(q_fit.mean()) if off else 0.0
    report["q_full_frac"] = float((q_fit >= 0.999).mean()) if off else 0.0
    report["bytes"] = len(body)
    report["emissive"]["scaled_textures"] = sorted(scaled)
    report["emissive"]["overridden"] = n_ov
    report["emissive"]["histogram"] = dict(zip(["0", "(0,0.1)", "[0.1,0.4)", "[0.4,1]"], map(int, e_hist)))
    return body, report


def read(path: Path) -> dict:
    """Parse a sidecar back (tests and checks)."""
    b = path.read_bytes()
    if b[:4] != MAGIC:
        raise ValueError("bad magic")
    ver, fr3v, n, slot_mask, day_mask, night_mask, emitter_mask = struct.unpack_from("<7I", b, 4)
    slots = [struct.unpack_from("<16f", b, 32 + 64 * s) for s in range(8)]
    trees = []
    for i in range(n):
        kind, idx, V, _, dig, off = struct.unpack_from("<4I2Q", b, 32 + 64 * 8 + 32 * i)
        rec = np.frombuffer(b, np.uint32, V * 4, off).reshape(V, 4)
        trees.append({"kind": kind, "fr3_tree": idx, "vertices": V, "digest": dig, "records": rec})
    return {"version": ver, "fr3_version": fr3v, "slot_mask": slot_mask, "day_mask": day_mask, "night_mask": night_mask,
            "emitter_mask": emitter_mask,
            "slots": slots, "trees": trees}


def load_e_factors(strengths_csv: Path, maps_index: Path) -> dict[str, float]:
    """Texture name -> human strength / default strength, from the reviewer's emissive ladder.

    ``strengths_csv`` is ``emissive-strengths.csv`` (last row per ``combo_id``, else ``name``,
    wins) and ``maps_index`` the ``emissive-maps/index.json`` holding each texture's default.
    Only ``human`` rows for textures with a positive default count; {} when a file is missing.
    """
    if not strengths_csv.is_file() or not maps_index.is_file():
        return {}
    with strengths_csv.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    textures = json.loads(maps_index.read_text(encoding="utf-8")).get("textures", {})
    last: dict[str, dict] = {}
    for r in rows:
        k = r.get("combo_id") or r.get("name", "")
        if k:
            last[k] = r
    out: dict[str, float] = {}
    for r in last.values():
        name = r.get("name", "")
        d = float(textures.get(name, {}).get("default_strength", 0.0))
        try:
            s = float(r.get("strength", ""))
        except ValueError:
            continue
        if r.get("source") == "human" and d > 0 and s > 0:
            out[name] = s / d
    return out


def human_strength_names(strengths_csv: Path) -> set[str]:
    """Texture names whose last ladder row is ``human`` (a step other than the default)."""
    if not strengths_csv.is_file():
        return set()
    with strengths_csv.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    last: dict[str, dict] = {}
    for r in rows:
        k = r.get("combo_id") or r.get("name", "")
        if k:
            last[k] = r
    return {r["name"] for r in last.values() if r.get("source") == "human" and r.get("name")}


def receiver_factors(scores: dict, scale: float, keep: set[str] = frozenset()) -> dict[str, float]:
    """Texture name -> ``scale`` for every receiver of ``emitter-score.json`` (``scores`` as
    loaded): below the threshold, not a ladder anchor, not a human emitter
    (``emissive_strengths.is_emitter``), and not in ``keep`` (textures with a human row)."""
    from sandover.materials.emissive_strengths import is_emitter

    thr = float(scores.get("threshold", 0.5))
    return {n: float(scale) for n, r in scores.get("textures", {}).items()
            if n not in keep and not is_emitter(r, thr)}


def export_factors(human: dict[str, float], receivers: dict[str, float]) -> dict[str, float]:
    """Per-texture E multipliers for the export: receiver scaling, with human ladder factors
    on top (a human row always wins). Overrides are applied after scaling, in :func:`build`."""
    return {**receivers, **human}


def raw_dir(lighting_dir: Path) -> Path:
    """Where the export keeps the unscaled sidecars the emissive tools scan."""
    return lighting_dir / "raw"


def scan_dir(lighting_dir: Path) -> Path:
    """The unscaled sidecars when the export wrote them, else ``lighting_dir`` itself."""
    r = raw_dir(lighting_dir)
    return r if r.is_dir() and any(r.glob("*.unbake")) else lighting_dir


def main(argv=None):
    ap = argparse.ArgumentParser(prog="sandover-unbake export-sidecar", description=__doc__.split("\n")[0])
    root = Path(os.environ.get("SANDOVER_ROOT", "."))
    data = Path(os.environ.get("SANDOVER_DATA", root / "sandover/data"))
    ap.add_argument("levels", nargs="*", choices=list(LEVELS), help="default: all 21")
    ap.add_argument("--levels-dir", type=Path, default=data / "unbake/riproot/decompiler_out/jak1/levels")
    ap.add_argument("--fit-dir", type=Path, default=data / "unbake")
    ap.add_argument("--out", type=Path, default=data / "lighting")
    ap.add_argument("--install", action="store_true", help="also copy into out/jak1/fr3 next to the level fr3")
    ap.add_argument("--q-lo", type=float, default=8.0, help="day residual (u8) at or below which q = 1")
    ap.add_argument("--q-hi", type=float, default=32.0, help="day residual (u8) at or above which q = 0")
    ap.add_argument("--k-max", type=float, default=3.99, help="sun gain clamp; vertices above it get q = 0")
    mats = data / "materials"
    ap.add_argument("--emissive-strengths", type=Path, default=mats / "emissive-strengths.csv",
                    help="reviewer ladder output; scales E per texture (default: used when present)")
    ap.add_argument("--emissive-maps-index", type=Path, default=mats / "emissive-maps" / "index.json",
                    help="default strengths the ladder values are relative to")
    ap.add_argument("--no-emissive-strengths", action="store_true", help="ignore the ladder, raw E")
    ap.add_argument("--overrides", type=Path, default=mats / "emissive-overrides.csv",
                    help="human emissive verdicts, forcing E to 1 or 0 (default: used when present)")
    ap.add_argument("--no-overrides", action="store_true", help="ignore the overrides file")
    ap.add_argument("--emitter-score", type=Path, default=mats / "emitter-score.json",
                    help="scales receivers' E by receiver_scale (default: used when present)")
    ap.add_argument("--emissive-preset", type=Path, default=None,
                    help="receiver_scale source, default sandover/presets/emissive.toml")
    ap.add_argument("--receiver-scale", type=float, default=None, help="override the preset's receiver_scale")
    ap.add_argument("--no-receiver-scale", action="store_true", help="leave receivers' E as baked")
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)
    human = {} if a.no_emissive_strengths else load_e_factors(a.emissive_strengths, a.emissive_maps_index)
    if human:
        print(f"E scaled for {len(human)} texture(s) from {a.emissive_strengths.name}", flush=True)
    ov = {} if a.no_overrides else overrides.load(a.overrides)
    if ov:
        print(f"E forced for {len(ov)} texture(s) from {a.overrides.name}", flush=True)
    receivers: dict[str, float] = {}
    if not a.no_receiver_scale and a.emitter_score.is_file():
        from sandover.materials import emitter_score

        scale = a.receiver_scale
        if scale is None:
            scale = emitter_score.load_params(a.emissive_preset).receiver_scale
        keep = set(ov) | (set() if a.no_emissive_strengths else human_strength_names(a.emissive_strengths))
        receivers = receiver_factors(emitter_score.load_scores(a.emitter_score), scale, keep)
        print(f"E x {scale:g} for {len(receivers)} receiver texture(s) from {a.emitter_score.name}", flush=True)
    factors = export_factors(human, receivers)
    raw_dir(a.out).mkdir(parents=True, exist_ok=True)
    fr3_dir = Path(os.environ.get("OPENGOAL_PROJ_PATH", root)) / "out/jak1/fr3"
    for lvl in a.levels or list(LEVELS):
        body, rep = build(lvl, a.levels_dir / lvl / f"{lvl}-background.glb", a.fit_dir, a.q_lo, a.q_hi, a.k_max,
                          factors, ov, raw := [])
        dst = a.out / f"{lvl}.unbake"
        dst.write_bytes(body)
        (raw_dir(a.out) / dst.name).write_bytes(raw[0])
        (a.out / f"{lvl}-sidecar.json").write_text(json.dumps(rep, indent=1))
        if a.install:
            shutil.copyfile(dst, fr3_dir / dst.name)
        print(lvl, f"{len(rep['trees'])} trees", f"{rep['vertices']} verts", f"{rep['fitted_vertices']} fitted",
              f"q_mean {rep['q_mean']:.2f}", f"E>0 {sum(t['e_nonzero'] for t in rep['trees'])}",
              f"night {rep['emissive']['night_slots']}", f"overridden {rep['emissive']['overridden']}",
              f"{rep['bytes'] / 1e6:.1f} MB", flush=True)


if __name__ == "__main__":
    main()
