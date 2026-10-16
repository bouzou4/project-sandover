"""``sandover-materials emissive-maps``: per-texture emissive maps rasterized from the sidecars.

For every texture drawn by a level's tfrag/tie trees, the per-vertex E of the level's `.unbake`
sidecar (`sandover.unbake.sidecar`; glb vertex order = sidecar order for those trees) is
interpolated across each triangle through the mesh UVs into texture space, taking the max over
every draw of the texture in every level. UVs outside [0, 1) wrap (the texture tiles), so a
triangle spanning several repeats writes every repeat into the one map. Only triangles with
some E > 0 are rasterized; each vertex is also splatted into its nearest texel so a triangle
smaller than a texel still marks it.

Output under ``$SANDOVER_DATA/materials/emissive-maps/``:

- ``<tpage>/<name>.png``: 8-bit greyscale, E x 255, the texture's own size. Written for every
  texture with bake coverage > 0 (some texel at E >= ``COVERED``), and for every texture a human marked ``emissive``
  (``emissive-overrides.csv``) with no bake coverage, from its texel luminance instead.
- ``index.json``: ``{"reference": {...}, "anchors": {...}, "textures": {name: stats}}`` with
  per-texture ``tpage``, ``width``, ``height``, ``max``, ``mean`` (over covered texels),
  ``coverage`` (fraction of texels with E >= ``COVERED``, the engine bloom ramp's start and the
  Browse tab's threshold), ``peak`` (95th percentile over covered texels), ``role``
  (the mood slot role that contributed most E: flames, light, lava, fade-lights, lightning,
  caustics, fade, night, static), ``levels``, ``default_strength`` and ``source``
  (``bake`` or ``luminance``).

Default strengths are relative to the torch anchor, which sits at 1.0 on the reviewer's
ladder: ``bake`` = peak / torch peak; ``luminance`` = the texture's bright-texel luminance
(95th percentile of Rec.601 luma over opaque texels) / the torch texture's, times
``LUMINANCE_DISCOUNT`` because a texture with no baked glow is less certain to be a light.

Anchors (`ANCHOR_PATTERNS`, :func:`find_anchors`): torch, lamp glass, eco, crystal, lava. Each
is the median-peak texture among those whose name matches the role's pattern and whose dominant
E source has one of the role's mood slot roles; lava, which no sidecar sees as a lit surface,
falls back to named lava textures scored by luminance (``ANCHOR_LUMINANCE_FALLBACK``). See
kb/howto/materials.md "Emissive review" for the list found on the current rip.

``--reindex`` keeps the rasterized maps and redoes only the luminance entries, anchors and
defaults (after new overrides), in seconds instead of the full scan's ~20 minutes.

Keyed by texture name (the glTF texture/image name = PNG stem), like ``emissive_table``; the
tpage is the texture-set directory holding that stem, preferring one named after a level the
texture is drawn in when the stem exists under several tpages.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pygltflib
from PIL import Image

from sandover.materials.emissive_table import EMISSIVE_THRESHOLD, _material_texture_name
from sandover.materials.overrides import latest_verdicts, override_key, overrides_path, read_overrides
from sandover.materials.sheets import materials_dir, textures_dir
from sandover.unbake import sidecar
from sandover.unbake.emissive import SRC_NIGHT, SRC_STATIC
from sandover.unbake.glb import SKIP_TREES, _read
from sandover.unbake.levels import LEVELS

E_EPS = 0.5 / 255
COVERED = EMISSIVE_THRESHOLD  # a texel counts as covered (reads as emissive) at E >= this
MAX_TILES = 4  # a triangle spanning more UV repeats than this is clipped to this many
LUMINANCE_DISCOUNT = 0.5
LUMA = np.array([0.299, 0.587, 0.114])

# role -> (texture name regex, accepted dominant E roles or None for any)
ANCHOR_PATTERNS: dict[str, tuple[str, set[str] | None]] = {
    "torch": (r"torch|flame|fire", {"flames"}),
    "lamp glass": (r"lamp|lantern|light|bulb|glass", {"night", "static", "light", "flames"}),
    "eco": (r"eco-valve-inside|blueeco|eco-glow", None),
    "crystal": (r"crystal", None),
    "lava": (r"lava|magma", {"lava"}),
}
# Roles whose emitter is not a lit tfrag/tie surface in the sidecar (the lava itself): taken from
# the texture set by name, with a luminance map and default.
ANCHOR_LUMINANCE_FALLBACK: dict[str, str] = {"lava": r"^lava-(flow-01|ground-hot|floor-deadly)$"}
MIN_ANCHOR_COVERAGE = 0.25


def maps_dir() -> Path:
    return materials_dir() / "emissive-maps"


def index_path() -> Path:
    return maps_dir() / "index.json"


# ---- rasterizer -----------------------------------------------------------------------------

def rasterize(uv: np.ndarray, e: np.ndarray, out: np.ndarray) -> None:
    """Max-splat triangles into ``out`` (H, W float32) in place.

    ``uv`` is (T, 3, 2) glTF UVs (origin top-left, v down), ``e`` (T, 3) per-corner values.
    Texel (x, y) is sampled at its centre ((x + .5) / W, (y + .5) / H); coordinates outside
    the texture wrap, so repeated UVs tile. Every corner is also written to its nearest texel.
    """
    h, w = out.shape
    px = uv[..., 0] * w - 0.5
    py = uv[..., 1] * h - 0.5
    for t in range(len(uv)):
        x0, y0 = px[t], py[t]
        ex = e[t]
        xmin, xmax = int(np.ceil(x0.min())), int(np.floor(x0.max()))
        ymin, ymax = int(np.ceil(y0.min())), int(np.floor(y0.max()))
        xmax = min(xmax, xmin + MAX_TILES * w - 1)
        ymax = min(ymax, ymin + MAX_TILES * h - 1)
        if xmax >= xmin and ymax >= ymin:
            gx, gy = np.meshgrid(np.arange(xmin, xmax + 1), np.arange(ymin, ymax + 1))
            d = (y0[1] - y0[2]) * (x0[0] - x0[2]) + (x0[2] - x0[1]) * (y0[0] - y0[2])
            if abs(d) > 1e-12:
                l0 = ((y0[1] - y0[2]) * (gx - x0[2]) + (x0[2] - x0[1]) * (gy - y0[2])) / d
                l1 = ((y0[2] - y0[0]) * (gx - x0[2]) + (x0[0] - x0[2]) * (gy - y0[2])) / d
                l2 = 1.0 - l0 - l1
                inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
                if inside.any():
                    val = (l0 * ex[0] + l1 * ex[1] + l2 * ex[2])[inside]
                    np.maximum.at(out, (gy[inside] % h, gx[inside] % w), val.astype(out.dtype))
        cx = np.rint(x0).astype(np.int64) % w
        cy = np.rint(y0).astype(np.int64) % h
        np.maximum.at(out, (cy, cx), ex.astype(out.dtype))


# ---- level scan -------------------------------------------------------------------------

@dataclass
class TexAccum:
    uv: list[np.ndarray] = field(default_factory=list)  # (T,3,2) chunks
    e: list[np.ndarray] = field(default_factory=list)  # (T,3) chunks
    roles: dict[str, float] = field(default_factory=dict)
    levels: set[str] = field(default_factory=set)
    img_size: tuple[int, int] | None = None


def _source_role(level: str, src: int) -> str:
    if src == SRC_NIGHT:
        return "night"
    if src == SRC_STATIC:
        return "static"
    return LEVELS[level].effects.get(src, "static")


def _image_size(g: pygltflib.GLTF2, material_idx: int | None) -> tuple[int, int] | None:
    """(width, height) of the material's embedded PNG, from its IHDR; None if not embedded."""
    try:
        tex = g.textures[g.materials[material_idx].pbrMetallicRoughness.baseColorTexture.index]
        uri = g.images[tex.source].uri or ""
    except (AttributeError, IndexError, TypeError):
        return None
    if not uri.startswith("data:"):
        return None
    head = base64.b64decode(uri.split(",", 1)[1][:44])
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")


def scan_level(level: str, levels_dir: Path, lighting_dir: Path, acc: dict[str, TexAccum]) -> set[str]:
    """Add this level's emissive triangles to ``acc`` (texture name -> TexAccum); returns the
    names of every texture its tfrag/tie trees draw (emissive or not), empty if a file is missing."""
    glb_path = levels_dir / level / f"{level}-background.glb"
    sc_path = lighting_dir / f"{level}.unbake"
    if not glb_path.is_file() or not sc_path.is_file():
        return set()
    g = pygltflib.GLTF2().load(str(glb_path))
    blob = g.binary_blob()
    tree_rec = {(t["kind"], t["fr3_tree"]): t["records"] for t in sidecar.read(sc_path)["trees"]}
    seen: set[str] = set()
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
        uv_all = None
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None:
                continue
            for p in g.meshes[node.mesh].primitives:
                tex_name = _material_texture_name(g, p.material)
                if p.indices is None or not tex_name or p.attributes.TEXCOORD_0 is None:
                    continue
                seen.add(tex_name)
                idx = _read(g, blob, p.indices).astype(np.int64).ravel()
                idx = idx[: len(idx) // 3 * 3].reshape(-1, 3)
                idx = idx[(idx < len(e_all)).all(1)]
                et = e_all[idx]
                hot = et.max(1) > E_EPS
                if not hot.any():
                    continue
                if uv_all is None:
                    uv_all = _read(g, blob, p.attributes.TEXCOORD_0).astype(np.float64)
                a = acc.setdefault(tex_name, TexAccum())
                a.uv.append(uv_all[idx[hot]])
                a.e.append(et[hot])
                a.levels.add(level)
                a.img_size = a.img_size or _image_size(g, p.material)
                v = np.unique(idx[hot])
                for s in np.unique(src_all[v]):
                    r = _source_role(level, int(s))
                    a.roles[r] = a.roles.get(r, 0.0) + float(e_all[v][src_all[v] == s].sum())
    return seen


# ---- textures and stats -----------------------------------------------------------------

def index_texture_dir(tex_root: Path) -> dict[str, list[str]]:
    """PNG stem -> every tpage directory holding it (sorted)."""
    out: dict[str, list[str]] = {}
    if tex_root.is_dir():
        for p in sorted(tex_root.glob("*/*.png")):
            out.setdefault(p.stem, []).append(p.parent.name)
    return out


def pick_tpage(tpages: list[str], levels: set[str]) -> str:
    """The tpage named after a level the texture is drawn in, else the first."""
    for tp in tpages:
        if any(tp.startswith(f"{lvl}-") for lvl in levels):
            return tp
    return tpages[0]


def map_stats(m: np.ndarray) -> dict:
    cov = m >= COVERED
    n = int(cov.sum())
    return {
        "max": round(float(m.max()) if m.size else 0.0, 4),
        "mean": round(float(m[cov].mean()) if n else 0.0, 4),
        "peak": round(float(np.percentile(m[cov], 95)) if n else 0.0, 4),
        "coverage": round(n / m.size if m.size else 0.0, 5),
    }


def texture_luma(png: Path) -> tuple[np.ndarray, float]:
    """(per-texel luma 0..1, 95th percentile over opaque texels) of a ripped texture."""
    im = np.asarray(Image.open(png).convert("RGBA"), np.float64) / 255.0
    luma = (im[..., :3] * LUMA).sum(-1)
    opaque = im[..., 3] > 0.05
    vals = luma[opaque] if opaque.any() else luma.ravel()
    return luma, float(np.percentile(vals, 95)) if vals.size else 0.0


def luminance_map(luma: np.ndarray, lo: float = 0.5, hi: float = 0.9) -> np.ndarray:
    """Texture-space map for a texture with no baked glow: smoothstep of its luma."""
    t = np.clip((luma - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def find_anchors(textures: dict[str, dict]) -> dict[str, str]:
    """role -> texture name, see ANCHOR_PATTERNS.

    Candidates: bake-sourced textures whose name matches the role's pattern, whose dominant E
    role is accepted, with coverage >= ``MIN_ANCHOR_COVERAGE``. The anchor is the candidate at
    the median peak (lower median; ties by coverage then name): E saturates at 1.0 on whole
    levels (firecanyon's lava slots), so the max would pick a lit wall as often as a lamp.
    Luminance-sourced textures only anchor the roles in ``ANCHOR_LUMINANCE_FALLBACK``.
    """
    out: dict[str, str] = {}
    for role, (pattern, roles) in ANCHOR_PATTERNS.items():
        rx = re.compile(pattern, re.I)
        cands = [(t["peak"], -t["coverage"], name) for name, t in textures.items()
                 if t["source"] == "bake" and name not in out.values() and rx.search(name)
                 and (roles is None or t.get("role") in roles) and t["coverage"] >= MIN_ANCHOR_COVERAGE]
        if not cands and role in ANCHOR_LUMINANCE_FALLBACK:
            fb = re.compile(ANCHOR_LUMINANCE_FALLBACK[role], re.I)
            cands = [(t.get("luma_p95", 0.0), -t["coverage"], name) for name, t in textures.items()
                     if t["source"] == "luminance" and fb.search(name)]
        if cands:
            cands.sort()
            out[role] = cands[(len(cands) - 1) // 2][2]
    return out


def default_strengths(textures: dict[str, dict], torch: str | None, certain: set[str] = frozenset()) -> dict:
    """Fill ``default_strength`` in place; returns the reference record. ``certain`` names
    luminance textures known to be lights (the fallback anchors), which skip the discount."""
    ref_peak = textures[torch]["peak"] if torch else 0.0
    ref_luma = textures[torch].get("luma_p95", 0.0) if torch else 0.0
    ref_peak = ref_peak or max((t["peak"] for t in textures.values()), default=1.0) or 1.0
    ref_luma = ref_luma or 1.0
    for name, t in textures.items():
        if t["source"] == "bake":
            t["default_strength"] = round(t["peak"] / ref_peak, 4)
        else:
            k = 1.0 if name in certain else LUMINANCE_DISCOUNT
            t["default_strength"] = round(k * t.get("luma_p95", 0.0) / ref_luma, 4)
    return {"texture": torch, "peak": ref_peak, "luma_p95": ref_luma}


def build(levels_dir: Path, lighting_dir: Path, out_dir: Path, tex_root: Path,
          levels: list[str] | None = None, overrides_csv: Path | None = None,
          log=print) -> dict:
    acc: dict[str, TexAccum] = {}
    drawn: set[str] = set()
    for lvl in levels or list(LEVELS):
        seen = scan_level(lvl, levels_dir, lighting_dir, acc)
        drawn |= seen
        log(f"{lvl}: {len(seen)} textures drawn, {sum(1 for a in acc.values() if lvl in a.levels)} with E > 0")
    tex_index = index_texture_dir(tex_root)
    textures: dict[str, dict] = {}
    for name, a in sorted(acc.items()):
        tpages = tex_index.get(name)
        tpage = pick_tpage(tpages, a.levels) if tpages else ""
        png = tex_root / tpage / f"{name}.png" if tpage else None
        if png is not None and png.is_file():
            with Image.open(png) as im:
                w, h = im.size
        elif a.img_size:
            w, h = a.img_size
        else:
            continue
        m = np.zeros((h, w), np.float32)
        rasterize(np.concatenate(a.uv), np.concatenate(a.e), m)
        st = map_stats(m)
        if st["coverage"] <= 0:
            continue
        rec = {"tpage": tpage or "_", "width": w, "height": h, **st,
               "role": max(a.roles, key=a.roles.get) if a.roles else "",
               "levels": sorted(a.levels), "source": "bake"}
        if png is not None and png.is_file():
            rec["luma_p95"] = round(texture_luma(png)[1], 4)
        _write_map(out_dir, rec["tpage"], name, m)
        textures[name] = rec
    return finalize(textures, len(drawn), out_dir, tex_root, tex_index, overrides_csv)


def finalize(textures: dict[str, dict], drawn: int, out_dir: Path, tex_root: Path,
             tex_index: dict[str, list[str]], overrides_csv: Path | None = None) -> dict:
    """Add the luminance-sourced textures (human ``emissive`` overrides with no bake coverage,
    and the ``ANCHOR_LUMINANCE_FALLBACK`` textures), pick anchors, fill default strengths and
    write ``index.json``. Only the bake-sourced entries of ``textures`` are kept; returns the index."""
    textures = {n: t for n, t in textures.items() if t.get("source") == "bake"}
    verdicts = override_names(read_overrides(overrides_csv or overrides_path()))
    lum_names = {n for n, v in verdicts.items() if v == "emissive"}
    for pattern in ANCHOR_LUMINANCE_FALLBACK.values():
        rx = re.compile(pattern, re.I)
        lum_names |= {n for n in tex_index if rx.search(n)}
    for name in sorted(lum_names):
        if name in textures or name not in tex_index or verdicts.get(name) == "not-emissive":
            continue
        tpage = tex_index[name][0]
        luma, p95 = texture_luma(tex_root / tpage / f"{name}.png")
        m = luminance_map(luma).astype(np.float32)
        textures[name] = {"tpage": tpage, "width": m.shape[1], "height": m.shape[0], **map_stats(m),
                          "role": "", "levels": [], "source": "luminance", "luma_p95": round(p95, 4)}
        _write_map(out_dir, tpage, name, m)
    anchors = find_anchors(textures)
    fallback = {n for r, n in anchors.items() if textures[n]["source"] == "luminance"}
    ref = default_strengths(textures, anchors.get("torch"), fallback)
    index = {"reference": ref,
             "anchors": {r: {"name": n, "tpage": textures[n]["tpage"], "source": textures[n]["source"],
                             "strength": textures[n]["default_strength"]} for r, n in anchors.items()},
             "drawn_textures": drawn, "textures": textures}
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "index.json").write_text(json.dumps(index, indent=1, sort_keys=True), encoding="utf-8")
    return index


def reindex(out_dir: Path, tex_root: Path, overrides_csv: Path | None = None) -> dict:
    """Re-run :func:`finalize` on an existing ``index.json`` without re-rasterizing."""
    old = load_index(out_dir / "index.json")
    return finalize(old.get("textures", {}), old.get("drawn_textures", 0), out_dir, tex_root,
                    index_texture_dir(tex_root), overrides_csv)


def _write_map(out_dir: Path, tpage: str, name: str, m: np.ndarray) -> None:
    d = out_dir / tpage
    d.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.clip(np.rint(m * 255), 0, 255).astype(np.uint8), "L").save(d / f"{name}.png")


def override_names(rows: list[dict]) -> dict[str, str]:
    """Latest override verdict per texture name (overrides are keyed by combo_id or name)."""
    names = {override_key(r): r.get("name", "") for r in rows}
    return {names.get(k) or k: v for k, v in latest_verdicts(rows).items()}


def load_index(path: Path | None = None) -> dict:
    p = path or index_path()
    if not p.is_file():
        return {"reference": {}, "anchors": {}, "textures": {}}
    return json.loads(p.read_text(encoding="utf-8"))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-materials emissive-maps",
                                 description=__doc__.splitlines()[0])
    root = Path(os.environ.get("SANDOVER_ROOT", "."))
    data = Path(os.environ.get("SANDOVER_DATA", root / "sandover/data"))
    ap.add_argument("levels", nargs="*", choices=list(LEVELS), help="default: all 21")
    ap.add_argument("--levels-dir", type=Path, default=data / "unbake/riproot/decompiler_out/jak1/levels")
    ap.add_argument("--lighting-dir", type=Path, default=sidecar.scan_dir(data / "lighting"),
                    help="default $SANDOVER_DATA/lighting/raw when the export wrote it, else lighting")
    ap.add_argument("--textures", type=Path, default=None, help="default $SANDOVER_ROOT/decompiler_out/jak1/textures")
    ap.add_argument("--out", type=Path, default=None, help="default $SANDOVER_DATA/materials/emissive-maps")
    ap.add_argument("--reindex", action="store_true",
                    help="keep the rasterized bake maps; redo overrides, anchors and defaults only")
    args = ap.parse_args(argv)
    out = args.out or maps_dir()
    tex_root = args.textures or textures_dir()
    if args.reindex:
        index = reindex(out, tex_root)
    else:
        index = build(args.levels_dir, args.lighting_dir, out, tex_root, args.levels or None)
    tex = index["textures"]
    n_bake = sum(1 for t in tex.values() if t["source"] == "bake")
    print(f"{len(tex)} emissive maps ({n_bake} bake, {len(tex) - n_bake} luminance) of "
          f"{index['drawn_textures']} textures drawn by tfrag/tie -> {out}")
    for role, a in index["anchors"].items():
        print(f"  anchor {role}: {a['tpage']}/{a['name']} strength {a['strength']:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
