"""Contact sheets: one row per shot, N candidates side by side, lettered A-E.

No preset names, no scores -- generalizes ``pilot.py``'s texture sheet to round
candidates. The round directory it reads is described in ``rounds.py``.

Usage::

    uv run sandover-sheets --round $SANDOVER_DATA/rounds/sample/round-001
    uv run sandover-sheets --round <dir> --crop 0.4,0.4,0.2,0.2 --out-dir <dir>
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

CELL_W = 480
IMG_H = 270
LABEL_H = 28
PAD = 8
BG = (40, 40, 40)
FG = (230, 230, 230)
ANCHOR_OUTLINE = (255, 200, 60)
PLAIN_OUTLINE = (90, 90, 90)


def _font(size: int = 18):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


@dataclass
class SheetLayout:
    """Pure layout math for a row of ``n_cols`` cells -- no image I/O, unit-testable."""

    n_cols: int
    cell_w: int
    img_h: int
    label_h: int
    pad: int

    @property
    def cell_h(self) -> int:
        return self.img_h + self.label_h + 2 * self.pad

    @property
    def width(self) -> int:
        return self.n_cols * self.cell_w

    @property
    def height(self) -> int:
        return self.cell_h

    def cell_x(self, i: int) -> int:
        if not 0 <= i < self.n_cols:
            raise IndexError(i)
        return i * self.cell_w


def layout(n_cols: int, cell_w: int = CELL_W, img_h: int = IMG_H,
           label_h: int = LABEL_H, pad: int = PAD) -> SheetLayout:
    if n_cols < 1:
        raise ValueError("a sheet needs at least one column")
    return SheetLayout(n_cols, cell_w, img_h, label_h, pad)


def _fit(im: Image.Image, max_w: int, max_h: int) -> Image.Image:
    fitted = im.copy()
    fitted.thumbnail((max_w, max_h))
    return fitted


def _crop_inset(im: Image.Image, crop: tuple[float, float, float, float], size: int) -> Image.Image:
    x, y, w, h = crop
    width, height = im.size
    box = (int(x * width), int(y * height), int((x + w) * width), int((y + h) * height))
    return im.crop(box).resize((size, size))


def render_shot_sheet(
    images: dict[str, Path],
    out_path: Path,
    *,
    anchor_letter: str | None = None,
    crop: tuple[float, float, float, float] | None = None,
    cell_w: int = CELL_W,
    img_h: int = IMG_H,
) -> Path:
    """Render one shot's contact-sheet row: a cell per key in ``images``, in order.

    Keys are letters (``"A".."E"``, or ``"vanilla"``) -- never preset names or scores.
    ``crop`` is an optional normalized ``(x, y, w, h)`` inset drawn in each cell's corner.
    """
    letters = list(images.keys())
    lay = layout(len(letters), cell_w=cell_w, img_h=img_h)
    sheet = Image.new("RGB", (lay.width, lay.height), BG)
    draw = ImageDraw.Draw(sheet)
    font = _font(18)

    for i, letter in enumerate(letters):
        x0 = lay.cell_x(i)
        with Image.open(images[letter]) as raw:
            im = raw.convert("RGB")
            fitted = _fit(im, lay.cell_w - 2 * lay.pad, lay.img_h)
            ox = x0 + (lay.cell_w - fitted.width) // 2
            oy = lay.pad
            sheet.paste(fitted, (ox, oy))
            if crop is not None:
                inset = _crop_inset(im, crop, size=min(lay.cell_w, lay.img_h) // 3)
                sheet.paste(inset, (x0 + lay.cell_w - inset.width - lay.pad, oy + lay.pad))

        outline = ANCHOR_OUTLINE if letter == anchor_letter else PLAIN_OUTLINE
        draw.rectangle([x0 + 1, 0, x0 + lay.cell_w - 2, lay.cell_h - 1], outline=outline, width=2)
        label = letter if letter != anchor_letter else f"{letter} (faithful)"
        draw.text((x0 + lay.pad, lay.img_h + 2 * lay.pad - 4), label, fill=FG, font=font)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path, quality=90)
    return out_path


def render_round(manifest, out_dir: Path, *,
                  crop: tuple[float, float, float, float] | None = None,
                  shots: list[str] | None = None) -> list[Path]:
    """Render one sheet per shot in a round manifest, A-E only (no vanilla column)."""
    out = []
    for shot in shots or manifest.shots:
        images = {letter: manifest.image_path(letter, shot) for letter in manifest.candidates}
        out.append(render_shot_sheet(images, out_dir / f"{shot}.jpg",
                                      anchor_letter=manifest.anchor_letter, crop=crop))
    return out


def _parse_crop(spec: str | None) -> tuple[float, float, float, float] | None:
    if not spec:
        return None
    parts = [float(v) for v in spec.split(",")]
    if len(parts) != 4:
        raise ValueError("--crop needs x,y,w,h")
    return tuple(parts)  # type: ignore[return-value]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-sheets", description=__doc__.splitlines()[0])
    ap.add_argument("--round", required=True, type=Path, help="round directory (manifest.json inside)")
    ap.add_argument("--out-dir", type=Path, help="default: <round>/sheets")
    ap.add_argument("--shots", nargs="+", help="only these shot names (default: all in the round)")
    ap.add_argument("--crop", help="normalized x,y,w,h inset, e.g. 0.4,0.4,0.2,0.2")
    args = ap.parse_args(argv)

    from sandover.rounds import RoundManifest

    manifest = RoundManifest.load(args.round)
    out_dir = args.out_dir or args.round / "sheets"
    paths = render_round(manifest, out_dir, crop=_parse_crop(args.crop), shots=args.shots)
    for p in paths:
        print(f"wrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
