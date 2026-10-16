"""Manifest and numbered contact sheets for the vision judge and the maintainer spot-check."""

from __future__ import annotations

import csv
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from sandover import paths

PER_SHEET = 20
COLS, ROWS = 4, 5
CELL_W, IMG = 256, 208
CAPTION_H = 52
INSET = 64
CELL_H = IMG + CAPTION_H + 8
MANIFEST_FIELDS = ["index", "combo_id", "tpage", "name", "width", "height",
                   "has_alpha", "envmap_swatch", "png"]


def materials_dir() -> Path:
    return paths.data_dir() / "materials"


def manifest_path() -> Path:
    return materials_dir() / "manifest.csv"


def textures_dir() -> Path:
    return paths.root() / "decompiler_out" / "jak1" / "textures"


def hex_id(combo_id: int | None) -> str:
    return "" if combo_id is None else f"0x{combo_id:08x}"


def load_rgba(png: Path) -> Image.Image:
    """Texture as display RGBA: PS2 alpha (128 = opaque) doubled to 8-bit."""
    with Image.open(png) as im:
        rgba = np.asarray(im.convert("RGBA"), dtype=np.uint16).copy()
    rgba[..., 3] = np.minimum(rgba[..., 3] * 2, 255)
    return Image.fromarray(rgba.astype(np.uint8), "RGBA")


def is_envmap_swatch(im: Image.Image) -> bool:
    """Heuristic: tiny (<=16 px) and nearly flat, i.e. shaded by an envmap at runtime."""
    if max(im.size) > 16:
        return False
    rgb = np.asarray(im.convert("RGB"), dtype=np.float32)
    return float(rgb.reshape(-1, 3).std(axis=0).max()) < 12.0


def build_manifest() -> list[dict]:
    """One row per texture in the FiftyOne dataset, ordered by tpage then name."""
    from sandover.census import DATASET_NAME, _configure_fiftyone_env
    _configure_fiftyone_env()
    import fiftyone as fo

    ds = fo.load_dataset(DATASET_NAME)
    tpages, names, w, h, alpha, combo = ds.values(
        ["tpage_name", "name", "width", "height", "has_alpha", "combo_id"])
    order = sorted(range(len(names)), key=lambda i: (tpages[i], names[i]))
    rows = []
    for idx, i in enumerate(order):
        png = f"{tpages[i]}/{names[i]}.png"
        rows.append({
            "index": idx, "combo_id": hex_id(combo[i]), "tpage": tpages[i], "name": names[i],
            "width": w[i], "height": h[i], "has_alpha": alpha[i],
            "envmap_swatch": is_envmap_swatch(load_rgba(textures_dir() / png)), "png": png,
        })
    return rows


def write_csv(path: Path, rows: list[dict], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def read_csv(path: Path) -> list[dict]:
    with path.open(encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def _font(size: int):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def _checker(size: int, tile: int = 8) -> Image.Image:
    yy, xx = np.mgrid[0:size, 0:size]
    v = np.where(((xx // tile) + (yy // tile)) % 2 == 0, 170, 110).astype(np.uint8)
    return Image.fromarray(np.stack([v, v, v, np.full_like(v, 255)], -1), "RGBA")


def _fit_nearest(im: Image.Image, box: int) -> Image.Image:
    # Integer upscale keeps texels crisp; large textures shrink smoothly.
    w, h = im.size
    k = max(1, box // max(w, h))
    if k > 1:
        return im.resize((w * k, h * k), Image.NEAREST)
    im = im.copy()
    im.thumbnail((box, box), Image.LANCZOS)
    return im


def render_sheet(rows: list[dict], out: Path, title: str) -> Path:
    """Render up to 20 textures on a 4x5 grid with index, size, tpage and name captions."""
    header = 28
    sheet = Image.new("RGB", (COLS * CELL_W, header + ROWS * CELL_H), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    big, small = _font(18), _font(13)
    draw.text((8, 5), title, fill=(240, 240, 240), font=big)
    bg = _checker(IMG)
    for i, row in enumerate(rows):
        x0, y0 = (i % COLS) * CELL_W, header + (i // COLS) * CELL_H
        tile = bg.copy()
        tex = _fit_nearest(load_rgba(textures_dir() / row["png"]), IMG)
        tile.alpha_composite(tex, ((IMG - tex.width) // 2, (IMG - tex.height) // 2))
        ix = x0 + (CELL_W - IMG) // 2
        sheet.paste(tile.convert("RGB"), (ix, y0 + 4))
        if str(row["has_alpha"]) == "True":
            # Opaque RGB inset so translucent texels still show their colour.
            inset = _fit_nearest(tex.convert("RGB"), INSET)
            draw.rectangle([ix + IMG - inset.width - 3, y0 + 3, ix + IMG, y0 + inset.height + 6],
                           fill=(255, 255, 255))
            sheet.paste(inset, (ix + IMG - inset.width - 1, y0 + 5))
        draw.rectangle([x0, y0, x0 + CELL_W - 1, y0 + CELL_H - 1], outline=(90, 90, 90))
        ty = y0 + IMG + 6
        head = f"#{row['index']}  {row['width']}x{row['height']}  {row.get('label', '')}"
        draw.text((x0 + 6, ty), head.rstrip(), fill=(255, 220, 90), font=big)
        draw.text((x0 + 6, ty + 20), str(row["tpage"])[:36], fill=(200, 200, 200), font=small)
        draw.text((x0 + 6, ty + 35), str(row["name"])[:36], fill=(240, 240, 240), font=small)
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    return out


def sheet_name(n: int) -> str:
    return f"sheet-{n:03d}.png"


def render_all(rows: list[dict], out_dir: Path, prefix: str = "sheet") -> list[Path]:
    out = []
    for n in range(0, len(rows), PER_SHEET):
        k = n // PER_SHEET + 1
        chunk = rows[n:n + PER_SHEET]
        title = f"{prefix} {k}  (rows {n}..{n + len(chunk) - 1})"
        out.append(render_sheet(chunk, out_dir / sheet_name(k), title))
    return out


# ---- single-texture rendering (reviewer, Browse tab) ---------------------------------------

MIN_BOX = 512


def fit_for_review(im: Image.Image, min_box: int = MIN_BOX) -> Image.Image:
    """Nearest-neighbour upscale so the shown image is at least ``min_box`` px; leaves big ones."""
    w, h = im.size
    if max(w, h) >= min_box:
        return im.copy()
    k = max(1, -(-min_box // max(w, h)))
    return im.resize((w * k, h * k), Image.NEAREST)


def compose_over_checker(rgba: Image.Image) -> Image.Image:
    w, h = rgba.size
    bg = _checker(max(w, h)).crop((0, 0, w, h))
    out = bg.copy()
    out.alpha_composite(rgba)
    return out.convert("RGB")


def alpha_thumbnail(rgba: Image.Image) -> Image.Image:
    return Image.fromarray(np.asarray(rgba)[..., 3], "L")


def render_texture(row: dict) -> tuple[Image.Image, Image.Image, tuple[int, int]]:
    png = textures_dir() / row["tpage"] / f"{row['name']}.png"
    raw = load_rgba(png)
    true_size = raw.size
    disp = fit_for_review(raw)
    return compose_over_checker(disp), alpha_thumbnail(disp), true_size
