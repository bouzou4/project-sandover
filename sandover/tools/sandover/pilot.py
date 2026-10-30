"""Stratified 100-texture pilot sample + contact sheets for manual classification.

Usage:
    uv run python -m sandover.pilot sample     # pick 100 textures, write manifest.csv
    uv run python -m sandover.pilot sheets      # render sheet-1..5.png from the manifest
"""

from __future__ import annotations

import argparse
import csv
import os
import random
import sys
from pathlib import Path

from sandover.census import DATASET_NAME, _configure_fiftyone_env, sandover_data_dir

PILOT_DIR_NAME = "census/pilot"
BATCH_SIZE = 20
SAMPLE_SIZE = 100
SEED = 51  # fixed for reproducibility


def pilot_dir() -> Path:
    return sandover_data_dir() / PILOT_DIR_NAME


def manifest_path() -> Path:
    return pilot_dir() / "manifest.csv"


def stratified_sample(target_total: int = SAMPLE_SIZE) -> list[dict]:
    """Pick target_total samples spread proportionally across tpages."""
    _configure_fiftyone_env()
    import fiftyone as fo

    dataset = fo.load_dataset(DATASET_NAME)

    by_tpage: dict[str, list] = {}
    for s in dataset:
        by_tpage.setdefault(s.tpage_name, []).append(s)

    rng = random.Random(SEED)
    tpages = sorted(by_tpage.keys())
    total_textures = sum(len(v) for v in by_tpage.values())

    # Largest-remainder (Hamilton) apportionment: no per-tpage floor, so a
    # tpage's slot count is proportional to its true share of the rip.
    shares = {name: len(by_tpage[name]) / total_textures * target_total for name in tpages}
    alloc = {name: min(int(shares[name]), len(by_tpage[name])) for name in tpages}
    remaining = target_total - sum(alloc.values())

    by_largest_frac = sorted(
        (name for name in tpages if alloc[name] < len(by_tpage[name])),
        key=lambda name: shares[name] - int(shares[name]),
        reverse=True,
    )
    for name in by_largest_frac:
        if remaining <= 0:
            break
        if alloc[name] < len(by_tpage[name]):
            alloc[name] += 1
            remaining -= 1

    picked: list = []
    for name in tpages:
        pool = by_tpage[name][:]
        rng.shuffle(pool)
        picked.extend(pool[: alloc[name]])

    rng.shuffle(picked)

    rows = []
    for i, s in enumerate(picked):
        rows.append(
            {
                "index": i,
                "name": s.name,
                "tpage": s.tpage_name,
                "width": s.width,
                "height": s.height,
                "has_alpha": s.has_alpha,
                "thumbnail_path": s.thumbnail_path,
                "filepath": s.filepath,
            }
        )
    return rows


def write_manifest(rows: list[dict]) -> None:
    pilot_dir().mkdir(parents=True, exist_ok=True)
    with manifest_path().open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {manifest_path()} ({len(rows)} rows)")


def read_manifest() -> list[dict]:
    with manifest_path().open(encoding="utf-8") as f:
        return list(csv.DictReader(f))


def render_sheets() -> None:
    from PIL import Image, ImageDraw, ImageFont

    rows = read_manifest()
    cols, sheet_rows = 4, 5
    cell = 220
    img_area = 160
    header_h = 30

    try:
        font = ImageFont.load_default(size=14)
    except TypeError:
        font = ImageFont.load_default()

    n_batches = (len(rows) + BATCH_SIZE - 1) // BATCH_SIZE
    for b in range(n_batches):
        batch = rows[b * BATCH_SIZE : (b + 1) * BATCH_SIZE]
        sheet = Image.new("RGB", (cols * cell, sheet_rows * cell), "white")
        draw = ImageDraw.Draw(sheet)

        for i, row in enumerate(batch):
            gx, gy = i % cols, i // cols
            x0, y0 = gx * cell, gy * cell
            draw.rectangle([x0, y0, x0 + cell - 1, y0 + cell - 1], outline="black")

            thumb_path = Path(row["thumbnail_path"])
            if thumb_path.exists():
                with Image.open(thumb_path) as t:
                    t = t.convert("RGBA")
                    t.thumbnail((img_area, img_area))
                    bg = Image.new("RGBA", (img_area, img_area), (200, 200, 200, 255))
                    ox = (img_area - t.width) // 2
                    oy = (img_area - t.height) // 2
                    bg.paste(t, (ox, oy), t)
                    sheet.paste(bg.convert("RGB"), (x0 + 10, y0 + header_h))

            idx = int(row["index"])
            caption = f"#{idx} {row['tpage']}/{row['name']}"
            draw.text((x0 + 4, y0 + 2), caption[:34], fill="black", font=font)

        out_path = pilot_dir() / f"sheet-{b + 1}.png"
        sheet.save(out_path)
        print(f"wrote {out_path} ({len(batch)} textures)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="sandover-pilot")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("sample")
    sub.add_parser("sheets")
    args = parser.parse_args(argv)

    if args.command == "sample":
        rows = stratified_sample()
        write_manifest(rows)
    elif args.command == "sheets":
        render_sheets()
    return 0


if __name__ == "__main__":
    sys.exit(main())
