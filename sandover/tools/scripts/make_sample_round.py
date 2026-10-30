"""Build a sample round under $SANDOVER_DATA/rounds/sample/round-001/.

No real presets exist yet for any feature, so this synthesizes five "candidates" per shot
from a real rig capture (brightness/contrast/tint tweaks), clearly labelled as sample data,
so sheets.py/report.py have something real (real pixels, real camera, real
resolution) to run against. Source: SOURCE_DIR (a local capture folder).

Usage: uv run python scripts/make_sample_round.py
"""

from __future__ import annotations

import json
from pathlib import Path

from PIL import Image, ImageEnhance, ImageFilter

from sandover.paths import data_dir

SOURCE_DIR = data_dir() / "shots" / "e5" / "base"
SHOTS = ["village1-hut-mono", "beach-start-mono", "jungle-start-mono"]
SCALE_WIDTH = 960  # downscaled from the 2560x1440 source, so the sample stays small


def _load_scaled(path: Path) -> Image.Image:
    with Image.open(path) as im:
        im = im.convert("RGB")
        h = int(im.height * SCALE_WIDTH / im.width)
        return im.resize((SCALE_WIDTH, h))


def _candidate(vanilla: Image.Image, letter: str) -> Image.Image:
    if letter == "A":
        return vanilla.copy()
    if letter == "B":
        return ImageEnhance.Contrast(ImageEnhance.Brightness(vanilla).enhance(1.08)).enhance(1.12)
    if letter == "C":
        return vanilla.filter(ImageFilter.GaussianBlur(0.6))
    if letter == "D":
        return ImageEnhance.Brightness(vanilla).enhance(0.82)
    if letter == "E":
        return ImageEnhance.Color(vanilla).enhance(1.35)
    raise ValueError(letter)


CANDIDATES = {
    "A": {"preset": "sample-faithful", "role": "anchor", "label": "Faithful (sample data)"},
    "B": {"preset": "sample-contact", "role": "candidate", "label": "Contact (sample data)"},
    "C": {"preset": "sample-soft", "role": "candidate", "label": "Soft (sample data)"},
    "D": {"preset": "sample-grounded", "role": "candidate", "label": "Grounded (sample data)"},
    "E": {"preset": "sample-deep", "role": "candidate", "label": "Deep (sample data)"},
}


def build() -> Path:
    round_dir = data_dir() / "rounds" / "sample" / "round-001"
    round_dir.mkdir(parents=True, exist_ok=True)

    images: dict[str, dict[str, str]] = {letter: {} for letter in CANDIDATES}
    vanilla: dict[str, str] = {}

    for shot in SHOTS:
        src = SOURCE_DIR / f"{shot}.png"
        if not src.exists():
            print(f"skip {shot}: {src} not found")
            continue
        base = _load_scaled(src)

        vpath = round_dir / "vanilla" / f"{shot}.png"
        vpath.parent.mkdir(parents=True, exist_ok=True)
        base.save(vpath)
        vanilla[shot] = str(vpath.relative_to(round_dir)).replace("\\", "/")

        for letter in CANDIDATES:
            cpath = round_dir / "candidates" / letter / f"{shot}.png"
            cpath.parent.mkdir(parents=True, exist_ok=True)
            _candidate(base, letter).save(cpath)
            images[letter][shot] = str(cpath.relative_to(round_dir)).replace("\\", "/")

    manifest = {
        "schema": 1,
        "phase": "sample",
        "round": 1,
        "pick_set": "sample",
        "shots": [s for s in SHOTS if s in vanilla],
        "candidates": CANDIDATES,
        "images": images,
        "vanilla": vanilla,
    }
    manifest_path = round_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {manifest_path} ({len(manifest['shots'])} shots)")
    return round_dir


if __name__ == "__main__":
    build()
