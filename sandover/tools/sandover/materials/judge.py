"""Batches and prompt for the vision judge: two passes, every sheet seen by two judges."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from sandover.materials import CLASS_LIST, CLASSES
from sandover.materials.sheets import PER_SHEET, materials_dir, sheet_name

N_BATCHES = 20
PASSES = ("a", "b")


def class_bullets() -> str:
    """One prompt bullet per class, in ``materials.toml`` order."""
    return "\n".join(f"- {c.name}: {c.description}" for c in CLASS_LIST)


PROMPT = """\
You are the vision judge for a texture material classification of the game Jak and Daxter
(PS2, 2001). Open each contact sheet below with the Read tool and look at it. Each sheet
shows 20 textures; every cell is captioned `#<index> <width>x<height>`, then the tpage
(texture page, names the level and draw kind: tfrag = level geometry, pris = characters and
props, shrub = foliage and small props, alpha = translucent, water, sky), then the texture
name. Transparent pixels show a grey checkerboard; textures with translucency also get a
small opaque colour inset (white border) in the top-right corner. Tiny textures are upscaled.

{guidance}Assign each texture exactly one class, using pixels first and the name as a hint:
{class_bullets}

Confidence is your probability (0 to 1) that the class is right. Use below 0.5 when you are
guessing (flat swatch, unreadable, name and pixels disagree). Keep notes to a few words.

Sheets (all under {sheet_dir}):
{sheet_list}

Write a single JSON array to {out_path} with the Write tool, one object per texture on
every sheet ({n_items} objects, indices {index_ranges}):
  {{"index": 123, "class": "stone", "confidence": 0.8, "note": "cracked cliff rock"}}
`class` must be one of: {classes}. Do not skip any index. Do not read any other files.
When done, reply with only: the output path, the object count, and your class counts.
"""


def batches(pass_id: str, n_sheets: int) -> list[list[int]]:
    """Sheet numbers (1-based) per batch; pass b is shifted half a batch so its judges differ."""
    order = np.arange(1, n_sheets + 1)
    if pass_id == "b":
        order = np.roll(order, -(n_sheets // N_BATCHES // 2))
    return [c.tolist() for c in np.array_split(order, N_BATCHES)]


def judgement_path(pass_id: str, batch: int) -> Path:
    return materials_dir() / "judgements" / pass_id / f"batch-{batch:02d}.json"


def _ranges(sheets: list[int], per_sheet: int, n_items: int) -> list[tuple[int, int]]:
    return [((s - 1) * per_sheet, min(s * per_sheet, n_items) - 1) for s in sheets]


def prompt(pass_id: str, batch: int, n_items: int, per_sheet: int = 20) -> str:
    n_sheets = -(-n_items // per_sheet)
    sheets = batches(pass_id, n_sheets)[batch]
    rng = _ranges(sheets, per_sheet, n_items)
    sheet_dir = materials_dir() / "sheets"
    return PROMPT.format(
        guidance="",
        class_bullets=class_bullets(),
        sheet_dir=sheet_dir.as_posix(),
        sheet_list="\n".join(f"- {(sheet_dir / sheet_name(s)).as_posix()}" for s in sheets),
        out_path=judgement_path(pass_id, batch).as_posix(),
        n_items=sum(b - a + 1 for a, b in rng),
        index_ranges=", ".join(f"{a}-{b}" for a, b in rng),
        classes=", ".join(CLASSES),
    )


def rejudge_prompt(out_dir: Path, n_items: int, guidance: str = "") -> str:
    """Prompt for a rejudge batch: its own self-contained sheet set under ``out_dir``.

    Unlike ``prompt()``, sheets and indices are local to this batch (0..n_items-1), not the
    global manifest -- ``table.prepare_rejudge`` writes ``out_dir/manifest.csv`` to map the
    judge's local indices back to ``combo_id``.
    """
    n_sheets = -(-n_items // PER_SHEET) if n_items else 0
    sheet_list = "\n".join(f"- {(out_dir / sheet_name(k + 1)).as_posix()}" for k in range(n_sheets))
    return PROMPT.format(
        guidance=(guidance + "\n\n") if guidance else "",
        class_bullets=class_bullets(),
        sheet_dir=out_dir.as_posix(),
        sheet_list=sheet_list,
        out_path=(out_dir / "judgement.json").as_posix(),
        n_items=n_items,
        index_ranges=f"0-{n_items - 1}" if n_items else "(none)",
        classes=", ".join(CLASSES),
    )


def load_pass(pass_id: str) -> dict[int, dict]:
    """All judgements of one pass, keyed by texture index; invalid entries are dropped."""
    out: dict[int, dict] = {}
    for f in sorted((materials_dir() / "judgements" / pass_id).glob("batch-*.json")):
        for j in json.loads(f.read_text(encoding="utf-8")):
            cls = str(j.get("class", "")).strip().lower()
            if cls not in CLASSES:
                continue
            conf = min(max(float(j.get("confidence", 0.0)), 0.0), 1.0)
            out[int(j["index"])] = {"class": cls, "confidence": conf,
                                    "note": str(j.get("note", "")), "batch": f.stem}
    return out
