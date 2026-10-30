"""Normalize ESRGAN pack texture alpha to the PS2 opaque-at-128 convention (ADR-0016).

The 4x pack keeps 128-opaque on average but noisy (~113-129), and some real-alpha
textures exceed 128; the engine's replace_texture() copies bytes unclamped. This reads
each pack PNG, classifies it against the matching vanilla PNG's alpha histogram, and
writes a normalized copy plus a CSV audit. The source pack is never modified.

    uv run sandover-alpha-normalize --src <pack dir> --dst <out dir> [--dry-run]
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from dataclasses import asdict, dataclass, fields
from pathlib import Path

import numpy as np
from PIL import Image

from sandover.paths import data_dir, root

OPAQUE = 128


def vanilla_dir() -> Path:
    # decompiler_out is gitignored game data; only the main checkout (or $SANDOVER_ROOT) has it.
    return root() / "decompiler_out" / "jak1" / "textures"


@dataclass
class Row:
    tpage: str
    name: str
    vanilla_found: bool
    vanilla_min: int | None
    vanilla_max: int | None
    vanilla_has_alpha: bool | None
    pack_min_before: int
    pack_max_before: int
    pack_min_after: int
    pack_max_after: int
    action: str
    note: str


def vanilla_alpha_stats(tpage: str, name: str) -> tuple[int, int] | None:
    """(min, max) alpha of the matching vanilla PNG, or None if missing / no alpha channel."""
    path = vanilla_dir() / tpage / f"{name}.png"
    if not path.exists():
        return None
    with Image.open(path) as im:
        if im.mode not in ("RGBA", "LA", "PA"):
            return None
        return im.getchannel("A").getextrema()


def normalize(alpha: np.ndarray, vanilla_stats: tuple[int, int] | None) -> tuple[np.ndarray, str, str]:
    """Map ``alpha`` to the 128-opaque convention.

    - vanilla is flat (min==max, e.g. plain-opaque 128 or a constant translucent plane):
      the pack's variance around that constant is upscale noise, not real alpha data, so
      it's snapped back to the flat value.
    - vanilla has a real transparency gradient: the pack is rescaled so its own peak lands
      on the vanilla peak (128 for mixed opaque/translucent), preserving the gradient's shape.
    - no matching vanilla texture: clamp to <=128 (ADR-0016 fallback), flagged for review.
    """
    pack_max = int(alpha.max()) if alpha.size else 0

    if vanilla_stats is None:
        return np.clip(alpha, 0, OPAQUE), "clamped_no_vanilla", "no matching vanilla texture; clamped to <=128"

    v_min, v_max = vanilla_stats
    if v_min == v_max:
        return np.full_like(alpha, v_max), "flattened", f"vanilla is flat at {v_max}; pack noise snapped to it"

    if pack_max == 0:
        return alpha.copy(), "unchanged", "fully transparent texture; nothing to scale"

    target = min(v_max, OPAQUE)
    if pack_max == target:
        return alpha.copy(), "unchanged", f"pack peak already at {target}"

    scale = target / pack_max
    out = np.clip(np.round(alpha.astype(np.float64) * scale), 0, target).astype(np.uint8)
    return out, "rescaled", f"proportional rescale, peak {pack_max}->{target}"


def _load(path: Path) -> tuple[np.ndarray, np.ndarray, bool]:
    with Image.open(path) as im:
        pack_had_alpha = im.mode in ("RGBA", "LA", "PA")
        rgba = np.array(im.convert("RGBA"))
    return rgba, rgba[..., 3], pack_had_alpha


def iter_pack_pngs(src: Path):
    for tpage_dir in sorted(p for p in src.iterdir() if p.is_dir()):
        for png in sorted(tpage_dir.glob("*.png")):
            yield tpage_dir.name, png.stem, png


def run(src: Path, dst: Path, dry_run: bool) -> list[Row]:
    rows: list[Row] = []
    for tpage, name, png in iter_pack_pngs(src):
        rgba, alpha, pack_had_alpha = _load(png)
        before_min, before_max = (int(alpha.min()), int(alpha.max())) if alpha.size else (0, 0)
        v_stats = vanilla_alpha_stats(tpage, name)
        out_alpha, action, note = normalize(alpha, v_stats)
        if not pack_had_alpha:
            note = f"{note}; pack PNG had no alpha channel (Pillow defaulted it opaque)"

        rows.append(Row(
            tpage=tpage, name=name,
            vanilla_found=v_stats is not None,
            vanilla_min=v_stats[0] if v_stats else None,
            vanilla_max=v_stats[1] if v_stats else None,
            # PS2 GS convention: "has alpha" means not uniformly opaque (128), matching census.py.
            vanilla_has_alpha=(not (v_stats[0] == v_stats[1] == OPAQUE)) if v_stats else None,
            pack_min_before=before_min, pack_max_before=before_max,
            pack_min_after=int(out_alpha.min()) if out_alpha.size else 0,
            pack_max_after=int(out_alpha.max()) if out_alpha.size else 0,
            action=action, note=note,
        ))

        if not dry_run:
            rgba[..., 3] = out_alpha
            out_path = dst / tpage / f"{name}.png"
            out_path.parent.mkdir(parents=True, exist_ok=True)
            Image.fromarray(rgba, "RGBA").save(out_path)
    return rows


def write_csv(rows: list[Row], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=[fld.name for fld in fields(Row)])
        w.writeheader()
        for row in rows:
            w.writerow(asdict(row))


def print_summary(rows: list[Row], csv_path: Path) -> None:
    total = len(rows)
    actions = Counter(r.action for r in rows)
    exceeded = [r for r in rows if r.pack_max_before > OPAQUE]
    no_vanilla = actions["clamped_no_vanilla"]
    print(f"textures processed: {total}", file=sys.stderr)
    print(f"audit: {csv_path}", file=sys.stderr)
    for action, count in sorted(actions.items()):
        print(f"  {action}: {count}", file=sys.stderr)
    print(f"needed clamping (no matching vanilla): {no_vanilla}", file=sys.stderr)
    print(f"exceeded 128 before normalization: {len(exceeded)}", file=sys.stderr)
    if exceeded:
        peaks = [r.pack_max_before for r in exceeded]
        print(f"  peak alpha range: {min(peaks)}-{max(peaks)}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", required=True, type=Path, help="pack dir, laid out <tpage>/<name>.png")
    ap.add_argument("--dst", required=True, type=Path, help="output dir, same layout; should live under $SANDOVER_DATA")
    ap.add_argument("--dry-run", action="store_true", help="compute the audit only; write no PNGs")
    args = ap.parse_args()

    src, dst = args.src.resolve(), args.dst.resolve()
    if dst == src or dst.is_relative_to(src) or src.is_relative_to(dst):
        sys.exit("--dst must not overlap --src; the source pack is read-only")
    if not dst.is_relative_to(data_dir().resolve()):
        print(f"warning: --dst is outside $SANDOVER_DATA ({data_dir()})", file=sys.stderr)

    rows = run(args.src, args.dst, args.dry_run)
    csv_path = args.dst / "alpha_audit.csv"
    write_csv(rows, csv_path)
    print_summary(rows, csv_path)


if __name__ == "__main__":
    main()
