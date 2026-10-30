"""Build and inspect a FiftyOne dataset of ripped Jak 1 textures.

Usage:
    uv run sandover-census build
    uv run sandover-census stats [--markdown PATH]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path


def repo_root() -> Path:
    # sandover/tools/sandover/census.py -> repo root is three parents up.
    return Path(__file__).resolve().parents[3]


def sandover_data_dir() -> Path:
    env = os.environ.get("SANDOVER_DATA")
    if env:
        return Path(env)
    return repo_root() / "sandover" / "data"


def textures_dir() -> Path:
    return repo_root() / "decompiler_out" / "jak1" / "textures"


def import_dir() -> Path:
    return repo_root() / "decompiler_out" / "jak1" / "import"


# --- write_tpage_imports parsing -------------------------------------------------
# tpages.gc: (defconstant <name> <id>); textures.gc: (def-tex <name> <tpage> <idx>).
# Both written by the decompiler with write_tpage_imports:true.

_TPAGE_RE = re.compile(r"^\(defconstant\s+(\S+)\s+(\d+)\)\s*$")
_TEX_RE = re.compile(r"^\(def-tex\s+(\S+)\s+(\S+)\s+(\d+)\)\s*$")


def parse_tpage_ids(path: Path) -> dict[str, int]:
    ids: dict[str, int] = {}
    if not path.exists():
        return ids
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _TPAGE_RE.match(line)
        if m:
            ids[m.group(1)] = int(m.group(2))
    return ids


def parse_tex_idx(path: Path) -> dict[tuple[str, str], int]:
    idx: dict[tuple[str, str], int] = {}
    if not path.exists():
        return idx
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _TEX_RE.match(line)
        if m:
            tex_name, tpage_name, tex_idx = m.group(1), m.group(2), int(m.group(3))
            idx[(tpage_name, tex_name)] = tex_idx
    return idx


# --- write_texture_levels parsing ------------------------------------------------
# texture-levels.json: {"<tpage>/<name>": ["level", ...]}, written by the decompiler
# with write_texture_levels:true.


def texture_levels_path() -> Path:
    return import_dir() / "texture-levels.json"


def load_texture_levels(path: Path) -> dict[tuple[str, str], list[str]]:
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {tuple(k.split("/", 1)): v for k, v in data.items()}


# --- scanning ----------------------------------------------------------------


def iter_texture_pngs(tex_dir: Path):
    for tpage_dir in sorted(p for p in tex_dir.iterdir() if p.is_dir()):
        for png in sorted(tpage_dir.glob("*.png")):
            yield tpage_dir.name, png.stem, png


def image_has_alpha(img) -> bool:
    # PS2 GS alpha: opaque is 128, not 255. "Has alpha" means not uniformly 128.
    if img.mode not in ("RGBA", "LA", "PA"):
        return False
    alpha = img.getchannel("A")
    lo, hi = alpha.getextrema()
    return not (lo == hi == 128)


def make_thumbnail(src: Path, dst: Path, max_size: int = 128) -> None:
    from PIL import Image

    dst.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(src) as img:
        img = img.convert("RGBA")
        # Rescale PS2 GS alpha (opaque=128) to 8-bit (opaque=255) for display.
        r, g, b, a = img.split()
        a = a.point(lambda v: min(255, v * 2))
        img = Image.merge("RGBA", (r, g, b, a))
        img.thumbnail((max_size, max_size))
        img.save(dst)


# --- fiftyone dataset ----------------------------------------------------------

DATASET_NAME = "jak1-textures"


def _configure_fiftyone_env() -> None:
    os.environ.setdefault("FIFTYONE_DATABASE_DIR", str(sandover_data_dir() / "fiftyone"))


def build_dataset() -> None:
    """(Re)build the dataset from scratch - always replaces any existing one."""
    _configure_fiftyone_env()
    import fiftyone as fo
    from PIL import Image

    tex_dir = textures_dir()
    if not tex_dir.exists():
        raise SystemExit(f"no textures found at {tex_dir} - run `task rip-textures` first")

    tpage_ids = parse_tpage_ids(import_dir() / "tpages.gc")
    tex_idx = parse_tex_idx(import_dir() / "textures.gc")
    levels_path = texture_levels_path()
    levels_by_key = load_texture_levels(levels_path)

    if fo.dataset_exists(DATASET_NAME):
        fo.delete_dataset(DATASET_NAME)

    dataset = fo.Dataset(DATASET_NAME)
    dataset.persistent = True

    thumb_dir = sandover_data_dir() / "census" / "thumbnails"

    samples = []
    n = 0
    for tpage_name, name, png_path in iter_texture_pngs(tex_dir):
        with Image.open(png_path) as img:
            width, height = img.size
            has_alpha = image_has_alpha(img)

        thumb_path = thumb_dir / tpage_name / f"{name}.png"
        if not thumb_path.exists():
            make_thumbnail(png_path, thumb_path)

        page_id = tpage_ids.get(tpage_name)
        local_idx = tex_idx.get((tpage_name, name))
        combo_id = (page_id << 16) | local_idx if page_id is not None and local_idx is not None else None

        sample = fo.Sample(filepath=str(png_path))
        sample["tpage"] = page_id
        sample["tpage_name"] = tpage_name
        sample["name"] = name
        sample["width"] = width
        sample["height"] = height
        sample["has_alpha"] = has_alpha
        # Jak 1 has no animated texture slots (see finding note).
        sample["animated"] = False
        # None if texture-levels.json wasn't generated; [] if generated but unused anywhere.
        sample["levels"] = levels_by_key.get((tpage_name, name), []) if levels_path.exists() else None
        sample["combo_id"] = combo_id
        sample["thumbnail_path"] = str(thumb_path)
        samples.append(sample)
        n += 1

    dataset.add_samples(samples)
    print(f"added {n} samples to fiftyone dataset {DATASET_NAME!r}")
    print(f"database dir: {os.environ['FIFTYONE_DATABASE_DIR']}")


# --- stats -----------------------------------------------------------------


def compute_stats() -> dict:
    _configure_fiftyone_env()
    import fiftyone as fo

    dataset = fo.load_dataset(DATASET_NAME)

    by_tpage: dict[str, int] = {}
    size_hist: dict[str, int] = {}
    alpha_count = 0
    total = 0
    with_levels = 0
    without_levels = 0
    by_level: dict[str, int] = {}

    for s in dataset:
        total += 1
        by_tpage[s.tpage_name] = by_tpage.get(s.tpage_name, 0) + 1
        bucket = f"{s.width}x{s.height}"
        size_hist[bucket] = size_hist.get(bucket, 0) + 1
        if s.has_alpha:
            alpha_count += 1
        if s.levels:
            with_levels += 1
            for lvl in s.levels:
                by_level[lvl] = by_level.get(lvl, 0) + 1
        else:
            without_levels += 1

    return {
        "total": total,
        "by_tpage": dict(sorted(by_tpage.items(), key=lambda kv: -kv[1])),
        "size_hist": dict(sorted(size_hist.items(), key=lambda kv: -kv[1])),
        "alpha_count": alpha_count,
        "with_levels": with_levels,
        "without_levels": without_levels,
        "by_level": dict(sorted(by_level.items(), key=lambda kv: -kv[1])),
    }


def render_markdown(stats: dict) -> str:
    lines = []
    lines.append(f"Total textures: **{stats['total']}**")
    lines.append(f"With alpha channel in use: **{stats['alpha_count']}** "
                 f"({100.0 * stats['alpha_count'] / max(stats['total'], 1):.1f}%)")
    lines.append(f"With a known level: **{stats['with_levels']}**; without: "
                 f"**{stats['without_levels']}**")
    lines.append("")
    lines.append("### Counts by tpage")
    lines.append("")
    lines.append("| tpage | textures |")
    lines.append("|---|---|")
    for name, count in stats["by_tpage"].items():
        lines.append(f"| {name} | {count} |")
    lines.append("")
    lines.append("### Size histogram (top 20)")
    lines.append("")
    lines.append("| width x height | textures |")
    lines.append("|---|---|")
    for i, (bucket, count) in enumerate(stats["size_hist"].items()):
        if i >= 20:
            break
        lines.append(f"| {bucket} | {count} |")
    lines.append("")
    lines.append("### Textures per level (top 20)")
    lines.append("")
    lines.append("| level | textures |")
    lines.append("|---|---|")
    for i, (name, count) in enumerate(stats["by_level"].items()):
        if i >= 20:
            break
        lines.append(f"| {name} | {count} |")
    return "\n".join(lines) + "\n"


def print_stats(stats: dict) -> None:
    print(f"total textures: {stats['total']}")
    print(f"with alpha: {stats['alpha_count']} "
          f"({100.0 * stats['alpha_count'] / max(stats['total'], 1):.1f}%)")
    print(f"with a known level: {stats['with_levels']}; without: {stats['without_levels']}")
    print("by tpage (top 20):")
    for i, (name, count) in enumerate(stats["by_tpage"].items()):
        if i >= 20:
            break
        print(f"  {name}: {count}")
    print("size histogram (top 20):")
    for i, (bucket, count) in enumerate(stats["size_hist"].items()):
        if i >= 20:
            break
        print(f"  {bucket}: {count}")
    print("textures per level (top 20):")
    for i, (name, count) in enumerate(stats["by_level"].items()):
        if i >= 20:
            break
        print(f"  {name}: {count}")


# --- cli ---------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="sandover-census")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("build", help="(re)build the jak1-textures fiftyone dataset")

    stats_p = sub.add_parser("stats", help="print/write summary stats for the dataset")
    stats_p.add_argument("--markdown", type=Path, default=None)

    args = parser.parse_args(argv)

    if args.command == "build":
        build_dataset()
    elif args.command == "stats":
        stats = compute_stats()
        print_stats(stats)
        if args.markdown:
            args.markdown.parent.mkdir(parents=True, exist_ok=True)
            args.markdown.write_text(render_markdown(stats), encoding="utf-8")
            print(f"wrote {args.markdown}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
