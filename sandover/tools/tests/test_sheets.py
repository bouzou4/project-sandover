from pathlib import Path

import pytest
from PIL import Image

from sandover.sheets import ANCHOR_OUTLINE, PLAIN_OUTLINE, layout, render_shot_sheet


def test_layout_math():
    lay = layout(5, cell_w=100, img_h=60, label_h=20, pad=5)
    assert lay.width == 500
    assert lay.height == lay.cell_h == 60 + 20 + 10
    assert lay.cell_x(0) == 0
    assert lay.cell_x(4) == 400


def test_layout_rejects_zero_columns():
    with pytest.raises(ValueError):
        layout(0)


def test_layout_cell_x_out_of_range():
    lay = layout(3)
    with pytest.raises(IndexError):
        lay.cell_x(3)


def _solid(path: Path, color: tuple[int, int, int], size=(64, 48)) -> None:
    Image.new("RGB", size, color).save(path)


def test_render_shot_sheet_size_and_columns(tmp_path: Path):
    images = {}
    for letter, color in [("A", (255, 0, 0)), ("B", (0, 255, 0)), ("C", (0, 0, 255))]:
        p = tmp_path / f"{letter}.png"
        _solid(p, color)
        images[letter] = p

    out = render_shot_sheet(images, tmp_path / "sheet.jpg", anchor_letter="A",
                             cell_w=200, img_h=100)
    assert out.exists()
    with Image.open(out) as sheet:
        assert sheet.size == (600, 100 + 28 + 16)  # 3 cols x cell_w, default label_h/pad


def _close(a: tuple[int, int, int], b: tuple[int, int, int], tol: int = 30) -> bool:
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def test_render_shot_sheet_anchor_outline(tmp_path: Path):
    images = {}
    for letter, color in [("A", (255, 0, 0)), ("B", (0, 255, 0))]:
        p = tmp_path / f"{letter}.png"
        _solid(p, color)
        images[letter] = p

    # PNG output, so outline pixels aren't perturbed by JPEG quantization.
    out = render_shot_sheet(images, tmp_path / "sheet.png", anchor_letter="A", cell_w=150, img_h=80)
    with Image.open(out) as sheet:
        # top edge of the anchor cell (letter A, first column) is the anchor outline color
        assert _close(sheet.getpixel((10, 0)), ANCHOR_OUTLINE)
        # top edge of the non-anchor cell (letter B, second column) is the plain outline
        assert _close(sheet.getpixel((160, 0)), PLAIN_OUTLINE)


def test_render_shot_sheet_with_crop_inset(tmp_path: Path):
    images = {"A": tmp_path / "A.png"}
    _solid(images["A"], (10, 20, 30), size=(200, 200))
    out = render_shot_sheet(images, tmp_path / "sheet.jpg", crop=(0.25, 0.25, 0.5, 0.5))
    assert out.exists()
