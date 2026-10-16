"""Labelled screening grids for capture sets: one row per shot, one column per set (Pillow).

Internal screening only (set names are shown); blind review sheets are `sandover-sheets`.

Usage::

    sandover-grid $SANDOVER_DATA/shots/ssao/{off,faithful,contact} --out sheets/ --per-sheet 4
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def render(sets: list[Path], out_dir: Path, thumb_w: int = 640, per_sheet: int = 4,
           captions: dict[str, str] | None = None) -> list[Path]:
    """Write sheet-N.png files, ``per_sheet`` shots each, columns labelled with the set names."""
    shots = sorted(p.stem for p in sets[0].glob("*.png") if all((s / p.name).exists() for s in sets))
    font = ImageFont.load_default(size=22)
    small = ImageFont.load_default(size=17)
    captions = captions or {}
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for k in range(0, len(shots), per_sheet):
        chunk = shots[k:k + per_sheet]
        first = Image.open(sets[0] / f"{chunk[0]}.png")
        th = round(first.height * thumb_w / first.width)
        head, label_w, pad = (64 if captions else 36), 260, 4
        sheet = Image.new("RGB", (label_w + len(sets) * (thumb_w + pad), head + len(chunk) * (th + pad)),
                          (24, 24, 24))
        d = ImageDraw.Draw(sheet)
        for c, s in enumerate(sets):
            d.text((label_w + c * (thumb_w + pad) + 8, 6), s.name, fill=(240, 240, 240), font=font)
            if s.name in captions:
                d.text((label_w + c * (thumb_w + pad) + 8, 36), captions[s.name], fill=(190, 190, 190),
                       font=small)
        for r, name in enumerate(chunk):
            y = head + r * (th + pad)
            d.text((8, y + th // 2 - 11), name, fill=(240, 240, 240), font=font)
            for c, s in enumerate(sets):
                im = Image.open(s / f"{name}.png").convert("RGB").resize((thumb_w, th), Image.LANCZOS)
                sheet.paste(im, (label_w + c * (thumb_w + pad), y))
        path = out_dir / f"grid-{k // per_sheet + 1}.png"
        sheet.save(path)
        written.append(path)
    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-grid", description=__doc__.splitlines()[0])
    ap.add_argument("sets", nargs="+", type=Path, help="capture dirs, one column each")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--thumb-width", type=int, default=640)
    ap.add_argument("--per-sheet", type=int, default=4)
    ap.add_argument("--captions", metavar="PHASE",
                    help="caption each column from sandover/presets/<PHASE>.toml (set dir name = preset)")
    args = ap.parse_args(argv)
    captions = {"off": "vanilla, no AO"}
    if args.captions:
        from sandover import presets

        captions |= {k: v["caption"] for k, v in presets.presets(args.captions).items() if "caption" in v}
    for p in render(args.sets, args.out, args.thumb_width, args.per_sheet,
                    captions if args.captions else None):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
