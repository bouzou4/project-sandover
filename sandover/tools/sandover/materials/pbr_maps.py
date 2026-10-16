"""``sandover-materials derive-maps``: per-texture normal and roughness maps from the albedo.

Algorithmic, not generative: every map is a filter of the texture's own albedo, chosen by its
material class (``materials.csv``) through the recipes in ``sandover/presets/pbr-recipes.toml``
(:func:`load_recipes`, :func:`resolve`). Per texture (:func:`derive`):

1. Albedo: the alpha-normalized 4x pack (``$SANDOVER_DATA/assets/esrgan-v1.0.1-alpha-normalized``)
   when it holds the texture, else the vanilla rip. Alpha follows the PS2 convention (128 =
   opaque); texels under the engine's alpha-test cutoff are masked out.
2. Height: Rec.601 luminance, masked texels filled by normalized convolution so cut-out edges
   do not ring, band-passed as G(band_lo) - G(band_hi) plus weighted ``extra_bands`` (a small
   Materialize-style pyramid; optionally inverted, and blurred along
   the dominant structure-tensor orientation for wood). Filters run in the Fourier domain, so
   every texture is treated as tiling (wraps at the borders).
3. Normal: Sobel slope of the height with wrapped borders, times ``normal_strength``,
   soft-clipped at ``max_slope``; glTF / OpenGL convention (+X right, +Y up in the image).
   Masked texels and ``flat`` recipes get (0, 0, 1).
4. Roughness: ``rough_base`` + ``rough_contrast`` x local contrast of the height + ``rough_luma``
   x (luminance - mean), clipped.
5. Metallic: the recipe's scalar, written to the index only.
6. Parallax height (:func:`parallax_qualifies`): for masonry textures only (a class in
   ``[parallax] classes`` whose name carries one of ``cues``, or a ``parallax`` opt-in in
   ``materials.csv``; ``parallax = no`` opts out), the band-passed height stretched to 0..1
   between its 1st and 99th percentiles (1 = top; masked texels are top). Mostly cut-out
   textures (coverage under ``min_coverage``) never qualify; the engine keeps the unshifted
   alpha, so alpha silhouettes do not move. The engine packs the height into the map's alpha
   and moves the roughness into blue.

Output under ``$SANDOVER_DATA/materials/pbr-maps/``: ``<tpage>/<name>.normal.png`` (RGB),
``<tpage>/<name>.rough.png`` (greyscale), ``<name>.parallax.png`` (greyscale, qualifying textures
only), optionally ``<name>.height.png`` (``--height``), and
``index.json`` keyed ``<tpage>/<name>`` with the per-texture parameters and the recipe version.
Partial runs (``--class``, ``--only``) update their entries in the existing index.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import time
import tomllib
from concurrent.futures import ProcessPoolExecutor
from dataclasses import asdict, dataclass, fields
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from sandover import paths
from sandover.materials.sheets import materials_dir, textures_dir

LUMA = np.array([0.299, 0.587, 0.114])
ALPHA_CUTOFF = 0.128  # engine alpha test on the doubled PS2 alpha
EPS = 1e-6


@dataclass(frozen=True)
class Recipe:
    flat: bool = False
    band_lo: float = 0.5
    band_hi: float = 4.0
    extra_bands: tuple = ()
    invert: bool = False
    gain: float = 1.0
    normal_strength: float = 1.0
    max_slope: float = 0.6
    grain: float = 0.0
    grain_coherence: float = 0.15
    rough_base: float = 0.8
    rough_contrast: float = 0.1
    rough_luma: float = 0.0
    contrast_ref: float = 0.08
    rough_min: float = 0.04
    rough_max: float = 1.0
    metallic: float = 0.0
    min_size: int = 16


RECIPE_FIELDS = {f.name for f in fields(Recipe)}


def recipes_path() -> Path:
    return paths.root() / "sandover" / "presets" / "pbr-recipes.toml"


def maps_dir() -> Path:
    return materials_dir() / "pbr-maps"


def pack_dir() -> Path:
    return paths.data_dir() / "assets" / "esrgan-v1.0.1-alpha-normalized"


def load_recipes(path: Path | None = None) -> dict:
    """Parse ``pbr-recipes.toml``: ``{"version", "defaults", "class": {...}, "tag": {...}}``.

    Rejects keys that are not :class:`Recipe` fields, so a typo does not silently do nothing.
    """
    data = tomllib.loads((path or recipes_path()).read_text(encoding="utf-8"))
    tables = [("defaults", data.get("defaults", {}))]
    tables += [(f"class.{k}", v) for k, v in data.get("class", {}).items()]
    tables += [(f"tag.{k}", v) for k, v in data.get("tag", {}).items()]
    for where, t in tables:
        bad = set(t) - RECIPE_FIELDS
        if bad:
            raise ValueError(f"pbr-recipes.toml [{where}]: unknown key(s) {sorted(bad)}")
    px = {**PARALLAX_DEFAULTS, **data.get("parallax", {})}
    bad = set(px) - set(PARALLAX_DEFAULTS)
    if bad:
        raise ValueError(f"pbr-recipes.toml [parallax]: unknown key(s) {sorted(bad)}")
    return {"version": int(data.get("version", 0)), "defaults": data.get("defaults", {}),
            "class": data.get("class", {}), "tag": data.get("tag", {}), "parallax": px}


PARALLAX_DEFAULTS = {"classes": ["stone"], "cues": ["brick", "stone", "block", "wall", "tile", "cobble"],
                     "min_coverage": 0.4}
OPT_IN = {"1", "yes", "true", "on"}
OPT_OUT = {"0", "no", "false", "off"}


def parallax_qualifies(cfg: dict, cls: str, name: str, flag: str = "") -> bool:
    """Masonry gate for the parallax height: the ``parallax`` column wins (yes/no), else a
    class in ``cfg["classes"]`` with a masonry cue in the texture name."""
    f = (flag or "").strip().lower()
    if f in OPT_OUT:
        return False
    if f in OPT_IN:
        return True
    n = name.lower()
    return cls in cfg["classes"] and any(c in n for c in cfg["cues"])


def resolve(recipes: dict, cls: str, tags: list[str] | tuple[str, ...] = ()) -> tuple[Recipe, str]:
    """Recipe for a class and tags: defaults, then the class table, then each tag's table.
    Returns (recipe, key) where key names what applied, e.g. ``metal+precursor``."""
    merged = dict(recipes["defaults"])
    key = cls if cls in recipes["class"] else "defaults"
    merged.update(recipes["class"].get(cls, {}))
    for t in tags:
        if t in recipes["tag"]:
            merged.update(recipes["tag"][t])
            key += f"+{t}"
    merged["extra_bands"] = tuple(tuple(float(x) for x in b) for b in merged.get("extra_bands", ()))
    return Recipe(**merged), key


# ---- filters (periodic, via FFT) --------------------------------------------------------

def _freqs(shape: tuple[int, int]) -> tuple[np.ndarray, np.ndarray]:
    fy = np.fft.fftfreq(shape[0])[:, None]
    fx = np.fft.rfftfreq(shape[1])[None, :]
    return fy, fx


def gaussian_tf(shape: tuple[int, int], sigma: float) -> np.ndarray:
    """rfft2 transfer function of an isotropic Gaussian of ``sigma`` pixels."""
    fy, fx = _freqs(shape)
    return np.exp(-2.0 * np.pi ** 2 * sigma ** 2 * (fx ** 2 + fy ** 2))


def directional_tf(shape: tuple[int, int], sigma: float, angle: float) -> np.ndarray:
    """1-D Gaussian of ``sigma`` pixels along direction ``angle`` (radians, x right, y down)."""
    fy, fx = _freqs(shape)
    f_along = fx * np.cos(angle) + fy * np.sin(angle)
    return np.exp(-2.0 * np.pi ** 2 * sigma ** 2 * f_along ** 2)


def blur(img: np.ndarray, tf: np.ndarray) -> np.ndarray:
    return np.fft.irfft2(np.fft.rfft2(img) * tf, s=img.shape)


def fill_masked(lum: np.ndarray, mask: np.ndarray, sigma: float) -> np.ndarray:
    """Replace texels outside ``mask`` by a normalized-convolution average of the opaque ones,
    so filters see a smooth continuation instead of a step at cut-out edges."""
    if mask.all():
        return lum
    if not mask.any():
        return np.full_like(lum, float(lum.mean()))
    m = mask.astype(np.float64)
    tf = gaussian_tf(lum.shape, sigma)
    num, den = blur(lum * m, tf), blur(m, tf)
    fallback = float(lum[mask].mean())
    filled = np.where(den > 1e-3, num / np.maximum(den, 1e-3), fallback)
    return np.where(mask, lum, filled)


def structure_orientation(lum: np.ndarray, mask: np.ndarray) -> tuple[float, float]:
    """Dominant *grain* direction (radians, x right, y down) and coherence in [0, 1] from the
    texture-wide structure tensor. The grain runs across the dominant gradient."""
    gy, gx = np.gradient(lum)
    w = mask.astype(np.float64)
    jxx, jyy, jxy = (w * gx * gx).sum(), (w * gy * gy).sum(), (w * gx * gy).sum()
    tr = jxx + jyy
    if tr < EPS:
        return 0.0, 0.0
    coherence = float(np.sqrt((jxx - jyy) ** 2 + 4 * jxy ** 2) / tr)
    grad_angle = 0.5 * np.arctan2(2 * jxy, jxx - jyy)
    return float(grad_angle + np.pi / 2), coherence


def sobel(h: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(d/dx, d/drow) of ``h`` per pixel, Sobel with wrapped borders."""
    def r(a, dy, dx):
        return np.roll(np.roll(a, dy, 0), dx, 1)
    gx = ((r(h, 1, -1) + 2 * r(h, 0, -1) + r(h, -1, -1)) - (r(h, 1, 1) + 2 * r(h, 0, 1) + r(h, -1, 1))) / 8
    gy = ((r(h, -1, 1) + 2 * r(h, -1, 0) + r(h, -1, -1)) - (r(h, 1, 1) + 2 * r(h, 1, 0) + r(h, 1, -1))) / 8
    return gx, gy


def normal_from_height(h: np.ndarray, strength: float, max_slope: float) -> np.ndarray:
    """Unit normals (H, W, 3), glTF convention: +X right, +Y up the image, +Z out."""
    gx, gy = sobel(h)
    sx, sy = -gx * strength, gy * strength
    if max_slope > 0:
        sx, sy = max_slope * np.tanh(sx / max_slope), max_slope * np.tanh(sy / max_slope)
    n = np.stack([sx, sy, np.ones_like(h)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


# ---- one texture --------------------------------------------------------------------------

@dataclass
class Derived:
    normal: np.ndarray  # (H, W, 3) unit
    rough: np.ndarray  # (H, W) 0..1
    height: np.ndarray  # (H, W), band-passed, 0 at masked texels
    stats: dict


def alpha_mask(alpha_ps2: np.ndarray) -> np.ndarray:
    """Texels that survive the engine alpha test (PS2 alpha 0..128 doubled)."""
    return np.minimum(alpha_ps2.astype(np.float64) * 2.0 / 255.0, 1.0) >= ALPHA_CUTOFF


def derive(rgba: np.ndarray, recipe: Recipe, scale: float = 1.0) -> Derived:
    """Maps for one texture. ``rgba`` is uint8 (H, W, 4) with PS2 alpha; ``scale`` is pixels per
    vanilla texel (4 for the pack), applied to every recipe distance and to the slope."""
    h_, w_ = rgba.shape[:2]
    rgb = rgba[..., :3].astype(np.float64) / 255.0
    mask = alpha_mask(rgba[..., 3])
    lum = (rgb * LUMA).sum(-1)
    opaque = lum[mask] if mask.any() else lum.ravel()
    mean_lum = float(opaque.mean())
    small = min(h_, w_) / scale < recipe.min_size
    flat = recipe.flat or small or not mask.any()
    stats: dict = {"coverage": round(float(mask.mean()), 4), "flat": bool(flat)}

    height = np.zeros((h_, w_))
    if not flat:
        widest = max([recipe.band_hi, 1.0] + [b[1] for b in recipe.extra_bands])
        filled = fill_masked(lum, mask, widest * scale)
        sig = -filled if recipe.invert else filled
        band = np.zeros_like(sig)
        for lo, hi, weight in ((recipe.band_lo, recipe.band_hi, 1.0), *recipe.extra_bands):
            band += weight * (blur(sig, gaussian_tf(sig.shape, lo * scale))
                              - blur(sig, gaussian_tf(sig.shape, hi * scale)))
        if recipe.grain > 0:
            angle, coh = structure_orientation(filled, mask)
            stats.update(grain_deg=round(float(np.degrees(angle)) % 180, 1), coherence=round(coh, 3))
            if coh >= recipe.grain_coherence:
                band = blur(band, directional_tf(band.shape, recipe.grain * scale, angle))
        height = band * recipe.gain

    if flat:
        normal = np.zeros((h_, w_, 3))
        normal[..., 2] = 1.0
    else:
        normal = normal_from_height(height, recipe.normal_strength * scale, recipe.max_slope)
        normal[~mask] = (0.0, 0.0, 1.0)

    if flat or recipe.rough_contrast == 0:
        contrast = np.zeros((h_, w_))
    else:
        tf = gaussian_tf(height.shape, max(recipe.band_hi, 1.0) * scale)
        var = blur(height ** 2, tf) - blur(height, tf) ** 2
        contrast = np.clip(np.sqrt(np.maximum(var, 0)) / (recipe.contrast_ref * recipe.gain), 0, 1)
    rough = recipe.rough_base + recipe.rough_contrast * contrast + recipe.rough_luma * (lum - mean_lum)
    rough = np.clip(rough, recipe.rough_min, recipe.rough_max)
    rough[~mask] = np.clip(recipe.rough_base, recipe.rough_min, recipe.rough_max)
    height[~mask] = 0.0

    slope = np.degrees(np.arccos(np.clip(normal[..., 2][mask], -1, 1))) if mask.any() else np.zeros(1)
    stats.update(slope_p50_deg=round(float(np.median(slope)), 2),
                 slope_p95_deg=round(float(np.percentile(slope, 95)), 2),
                 rough_mean=round(float(rough[mask].mean()) if mask.any() else float(rough.mean()), 3))
    return Derived(normal=normal, rough=rough, height=height, stats=stats)


def parallax_height(height: np.ndarray, alpha_ps2: np.ndarray) -> np.ndarray:
    """Band-passed height stretched to 0..1 between its 1st and 99th percentiles of the opaque
    texels (1 = top surface, no offset); masked texels are top."""
    mask = alpha_mask(alpha_ps2)
    vals = height[mask] if mask.any() else height.ravel()
    lo, hi = np.percentile(vals, [1, 99])
    out = np.clip((height - lo) / max(hi - lo, EPS), 0, 1)
    out[~mask] = 1.0
    return out


def encode_normal(n: np.ndarray) -> np.ndarray:
    return np.clip(np.rint((n * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8)


def encode_unit(a: np.ndarray) -> np.ndarray:
    return np.clip(np.rint(a * 255), 0, 255).astype(np.uint8)


# ---- inputs -----------------------------------------------------------------------------

@dataclass(frozen=True)
class Job:
    tpage: str
    name: str
    cls: str
    tags: tuple[str, ...]
    envmap_swatch: bool
    albedo: str  # path
    source: str  # pack | vanilla
    vanilla_w: int
    recipe: Recipe
    recipe_key: str
    parallax: bool = False
    min_coverage: float = 0.4


def read_materials(path: Path | None = None) -> list[dict]:
    p = path or materials_dir() / "materials.csv"
    with p.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def plan(rows: list[dict], recipes: dict, vanilla_root: Path, pack_root: Path,
         only_class: str | None = None, only: set[str] | None = None,
         prefer: str = "pack", parallax_only: bool = False) -> list[Job]:
    """One job per ``materials.csv`` row with a readable albedo, after the filters.
    ``prefer="vanilla"`` derives from the vanilla rip even where the pack has the texture;
    ``parallax_only`` keeps the rows that pass :func:`parallax_qualifies`."""
    px = recipes.get("parallax", PARALLAX_DEFAULTS)
    jobs: list[Job] = []
    for r in rows:
        tpage, name, cls = r["tpage"], r["name"], r.get("class", "")
        tags = tuple(t for t in (r.get("tags") or "").replace(";", ",").split(",") if t.strip())
        tags = tuple(t.strip() for t in tags)
        if only_class and cls != only_class and only_class not in tags:
            continue
        if only and f"{tpage}/{name}" not in only:
            continue
        qualifies = parallax_qualifies(px, cls, name, r.get("parallax", ""))
        if parallax_only and not qualifies:
            continue
        vanilla = vanilla_root / tpage / f"{name}.png"
        if not vanilla.is_file():
            continue
        pack = pack_root / tpage / f"{name}.png"
        src = pack if prefer == "pack" and pack.is_file() else vanilla
        with Image.open(vanilla) as im:
            vw = im.size[0]
        recipe, key = resolve(recipes, cls, tags)
        swatch = str(r.get("envmap_swatch", "")).lower() == "true"
        jobs.append(Job(tpage, name, cls, tags, swatch, str(src), "pack" if src == pack else "vanilla",
                        vw, recipe, key, qualifies, float(px["min_coverage"])))
    return jobs


def run_job(job: Job, out_dir: str, write_height: bool = False) -> tuple[str, dict]:
    with Image.open(job.albedo) as im:
        rgba = np.asarray(im.convert("RGBA"))
    scale = rgba.shape[1] / max(job.vanilla_w, 1)
    recipe = job.recipe
    if job.envmap_swatch and not recipe.flat:
        recipe = Recipe(**{**asdict(recipe), "flat": True})
    d = derive(rgba, recipe, scale)
    folder = Path(out_dir) / job.tpage
    folder.mkdir(parents=True, exist_ok=True)
    Image.fromarray(encode_normal(d.normal), "RGB").save(folder / f"{job.name}.normal.png")
    Image.fromarray(encode_unit(d.rough), "L").save(folder / f"{job.name}.rough.png")
    if write_height:
        hs = d.height / (np.abs(d.height).max() + EPS) * 0.5 + 0.5
        Image.fromarray(encode_unit(hs), "L").save(folder / f"{job.name}.height.png")
    parallax = job.parallax and not d.stats["flat"] and d.stats["coverage"] >= job.min_coverage
    px_path = folder / f"{job.name}.parallax.png"
    if parallax:
        Image.fromarray(encode_unit(parallax_height(d.height, rgba[..., 3])), "L").save(px_path)
    elif px_path.exists():
        px_path.unlink()
    rec = {"tpage": job.tpage, "name": job.name, "class": job.cls, "tags": list(job.tags),
           "recipe": job.recipe_key, "source": job.source, "width": rgba.shape[1],
           "height": rgba.shape[0], "scale": round(scale, 3), "metallic": recipe.metallic,
           "normal_strength": recipe.normal_strength, "envmap_swatch": job.envmap_swatch,
           "height_map": write_height, "parallax": parallax, **d.stats}
    return f"{job.tpage}/{job.name}", rec


def _run_star(args):
    return run_job(*args)


def build(jobs: list[Job], out_dir: Path, version: int, write_height: bool = False,
          workers: int | None = None, log=print) -> dict:
    """Run every job (in parallel), merge the records into ``index.json`` and return it."""
    out_dir.mkdir(parents=True, exist_ok=True)
    workers = workers or max(1, (os.cpu_count() or 2) - 2)
    records: dict[str, dict] = {}
    t0 = time.perf_counter()
    args = [(j, str(out_dir), write_height) for j in jobs]
    if workers == 1 or len(jobs) < 8:
        results = map(_run_star, args)
    else:
        pool = ProcessPoolExecutor(workers)
        results = pool.map(_run_star, args, chunksize=8)
    for i, (key, rec) in enumerate(results, 1):
        rec["recipe_version"] = version
        records[key] = rec
        if i % 500 == 0:
            log(f"  {i}/{len(jobs)} ({time.perf_counter() - t0:.0f} s)")
    if not (workers == 1 or len(jobs) < 8):
        pool.shutdown()
    index = load_index(out_dir / "index.json")
    index["textures"].update(records)
    index["recipe_version"] = version
    index["generated"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    index["last_run"] = {"textures": len(jobs), "seconds": round(time.perf_counter() - t0, 1),
                         "workers": workers}
    (out_dir / "index.json").write_text(json.dumps(index, indent=1, sort_keys=True), encoding="utf-8")
    return index


def load_index(path: Path | None = None) -> dict:
    p = path or maps_dir() / "index.json"
    if not p.is_file():
        return {"recipe_version": 0, "textures": {}}
    return json.loads(p.read_text(encoding="utf-8"))


# ---- contact sheets -----------------------------------------------------------------------

SHEET_N = 12
SHEET_CELL = 160
SHEET_COLS = 2  # triplets per row


def pick_for_sheet(records: list[dict], n: int = SHEET_N) -> list[dict]:
    """``n`` textures spread evenly over the sorted list, skipping flat and tiny ones when possible."""
    good = [r for r in records if not r.get("flat")] or records
    good = sorted(good, key=lambda r: (r["tpage"], r["name"]))
    if len(good) <= n:
        return good
    return [good[round(i * (len(good) - 1) / (n - 1))] for i in range(n)]


def _cell(img: Image.Image) -> Image.Image:
    img = img.copy()
    img.thumbnail((SHEET_CELL, SHEET_CELL), Image.NEAREST if max(img.size) < SHEET_CELL // 2 else Image.LANCZOS)
    if max(img.size) < SHEET_CELL // 2:
        k = SHEET_CELL // max(img.size)
        img = img.resize((img.size[0] * k, img.size[1] * k), Image.NEAREST)
    return img


def render_sheet(title: str, picks: list[dict], albedo_of, out_dir: Path, path: Path) -> None:
    """albedo | normal | roughness per texture, ``SHEET_COLS`` triplets per row."""
    pad, cap = 6, 30
    tw = 3 * SHEET_CELL + 2 * pad
    rows = (len(picks) + SHEET_COLS - 1) // SHEET_COLS
    W = SHEET_COLS * (tw + 3 * pad) + pad
    H = 40 + rows * (SHEET_CELL + cap + pad)
    sheet = Image.new("RGB", (W, H), (28, 30, 32))
    dr = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 13)
        big = ImageFont.truetype("DejaVuSans.ttf", 18)
    except OSError:
        font = big = ImageFont.load_default()
    dr.text((pad, 10), f"{title}: albedo | normal | roughness ({len(picks)} textures)", fill=(230, 230, 230), font=big)
    for i, r in enumerate(picks):
        x0 = pad + (i % SHEET_COLS) * (tw + 3 * pad)
        y0 = 40 + (i // SHEET_COLS) * (SHEET_CELL + cap + pad)
        with Image.open(albedo_of(r)) as im:
            a = np.asarray(im.convert("RGBA")).astype(np.float64)
        alpha = np.minimum(a[..., 3:4] * 2 / 255, 1)
        rgb = a[..., :3] * alpha + 96 * (1 - alpha)
        tiles = [Image.fromarray(rgb.astype(np.uint8), "RGB")]
        folder = out_dir / r["tpage"]
        for suffix in ("normal", "rough"):
            with Image.open(folder / f"{r['name']}.{suffix}.png") as im:
                tiles.append(im.convert("RGB"))
        for j, t in enumerate(tiles):
            c = _cell(t)
            sheet.paste(c, (x0 + j * (SHEET_CELL + pad), y0))
        label = f"{r['tpage']}/{r['name']}"[:60]
        extra = f"slope p95 {r.get('slope_p95_deg', 0):.0f} deg, rough {r.get('rough_mean', 0):.2f}"
        dr.text((x0, y0 + SHEET_CELL + 2), label, fill=(210, 210, 210), font=font)
        dr.text((x0, y0 + SHEET_CELL + 15), extra, fill=(150, 150, 150), font=font)
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


def render_sheets(index: dict, out_dir: Path, vanilla_root: Path | None = None,
                  pack_root: Path | None = None) -> list[Path]:
    """One sheet per class present (and per tag), under ``<out_dir>/sheets/``."""
    by_group: dict[str, list[dict]] = {}
    for r in index["textures"].values():
        by_group.setdefault(r.get("class") or "unclassified", []).append(r)
        for t in r.get("tags", []):
            by_group.setdefault(f"tag-{t}", []).append(r)
    written = []
    for group, recs in sorted(by_group.items()):
        picks = pick_for_sheet(recs)
        p = out_dir / "sheets" / f"{group}.png"
        render_sheet(group, picks, lambda r: _albedo_path(r, vanilla_root, pack_root), out_dir, p)
        written.append(p)
    return written


def _albedo_path(r: dict, vanilla_root: Path | None, pack_root: Path | None) -> Path:
    pack = (pack_root or pack_dir()) / r["tpage"] / f"{r['name']}.png"
    if r.get("source") == "pack" and pack.is_file():
        return pack
    return (vanilla_root or textures_dir()) / r["tpage"] / f"{r['name']}.png"


# ---- CLI ------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-materials derive-maps", description=__doc__.splitlines()[0])
    ap.add_argument("--class", dest="cls", default=None, help="only this class (or tag, e.g. precursor)")
    ap.add_argument("--only", action="append", default=None, metavar="TPAGE/NAME",
                    help="only this texture; repeatable")
    ap.add_argument("--height", action="store_true", help="also write <name>.height.png")
    ap.add_argument("--parallax", action="store_true",
                    help="only the textures that qualify for the parallax height ([parallax] in the recipes)")
    ap.add_argument("--recipes", type=Path, default=None, help="default sandover/presets/pbr-recipes.toml")
    ap.add_argument("--materials", type=Path, default=None, help="default $SANDOVER_DATA/materials/materials.csv")
    ap.add_argument("--textures", type=Path, default=None, help="vanilla rip, default decompiler_out/jak1/textures")
    ap.add_argument("--pack", type=Path, default=None,
                    help="alpha-normalized pack, default $SANDOVER_DATA/assets/esrgan-v1.0.1-alpha-normalized")
    ap.add_argument("--source", choices=("pack", "vanilla"), default="pack",
                    help="albedo to derive from where the pack has the texture (default pack)")
    ap.add_argument("--out", type=Path, default=None, help="default $SANDOVER_DATA/materials/pbr-maps")
    ap.add_argument("--workers", type=int, default=None, help="default logical threads - 2")
    ap.add_argument("--no-sheets", action="store_true", help="skip the per-class contact sheets")
    ap.add_argument("--sheets-only", action="store_true", help="redraw the sheets from the existing index")
    args = ap.parse_args(argv)
    out = args.out or maps_dir()
    if not args.sheets_only:
        recipes = load_recipes(args.recipes)
        jobs = plan(read_materials(args.materials), recipes, args.textures or textures_dir(),
                    args.pack or pack_dir(), args.cls, set(args.only) if args.only else None,
                    args.source, args.parallax)
        n_pack = sum(1 for j in jobs if j.source == "pack")
        print(f"{len(jobs)} textures ({n_pack} from the pack, {len(jobs) - n_pack} vanilla), "
              f"recipes v{recipes['version']} -> {out}")
        t0 = time.perf_counter()
        index = build(jobs, out, recipes["version"], args.height, args.workers)
        print(f"derived {len(jobs)} textures in {time.perf_counter() - t0:.1f} s; "
              f"index has {len(index['textures'])}")
    if not args.no_sheets:
        sheets = render_sheets(load_index(out / "index.json"), out, args.textures, args.pack)
        print(f"{len(sheets)} contact sheets -> {out / 'sheets'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
