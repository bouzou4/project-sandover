"""Parse the Jak 1 mood tables (mood-lights and palette-interp) out of mood-tables.gc."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np

MOOD_TABLES = Path("goal_src/jak1/engine/gfx/mood/mood-tables.gc")
LEVEL_INFO = Path("goal_src/jak1/engine/level/level-info.gc")


@dataclass
class MoodLights:
    direction: np.ndarray  # (3,) unit vector toward the light
    lgt_color: np.ndarray  # (3,)
    amb_color: np.ndarray  # (3,)


@dataclass
class HourEntry:
    snapshot1: int
    snapshot2: int
    morph_start: float
    morph_end: float


def _vec(block: str, key: str) -> np.ndarray:
    m = re.search(rf":{key}\s*\(new 'static 'vector([^)]*)\)", block)
    out = np.zeros(4)
    if m:
        for i, c in enumerate("xyzw"):
            f = re.search(rf":{c}\s+(-?[\d.e+-]+)", m.group(1))
            if f:
                out[i] = float(f.group(1))
    return out[:3]


def _define_blocks(text: str) -> dict[str, str]:
    starts = [(m.group(1), m.start()) for m in re.finditer(r"^\(define \*([\w-]+)\*", text, re.MULTILINE)]
    ends = [s for _, s in starts[1:]] + [len(text)]
    return {name: text[s:e] for (name, s), e in zip(starts, ends)}


def parse(root: Path) -> tuple[dict[str, list[MoodLights]], dict[str, list[HourEntry]], dict[str, dict[str, str]]]:
    """Return (lights tables, hour tables, mood-context -> {lights, interp}) from the source tree."""
    text = (root / MOOD_TABLES).read_text(encoding="utf-8")
    blocks = _define_blocks(text)
    lights: dict[str, list[MoodLights]] = {}
    hours: dict[str, list[HourEntry]] = {}
    for name, block in blocks.items():
        if name.endswith("mood-lights-table"):
            parts = re.split(r"\(new 'static\s+'mood-lights(?![-\w])", block)[1:]
            lights[name] = [
                MoodLights(_vec(p, "direction"), _vec(p, "lgt-color"), _vec(p, "amb-color")) for p in parts
            ]
        elif "'sky-color-day" in block:
            ents = []
            for m in re.finditer(r"\(new 'static 'sky-color-hour([^)]*)\)", block):
                body = m.group(1)

                def g(k: str, d: float = 0.0) -> float:
                    f = re.search(rf":{k}\s+(-?[\d.]+)", body)
                    return float(f.group(1)) if f else d

                ents.append(HourEntry(int(g("snapshot1")), int(g("snapshot2")), g("morph-start"), g("morph-end")))
            hours[name] = ents
    contexts: dict[str, dict[str, str]] = {}
    for m in re.finditer(r"\(set! \(-> \*([\w-]+)\* (mood-lights-table|palette-interp)\) \*([\w-]+)\*\)", text):
        contexts.setdefault(m.group(1), {})[m.group(2)] = m.group(3)
    return lights, hours, contexts


def level_moods(root: Path) -> dict[str, str]:
    """Map level name -> mood-context name from level-info.gc."""
    text = (root / LEVEL_INFO).read_text(encoding="utf-8")
    out = {}
    for m in re.finditer(r":name '([\w-]+).*?:mood '\*([\w-]+)\*", text, re.DOTALL):
        out.setdefault(m.group(1), m.group(2))
    return out


def sun_slots(hour_table: list[HourEntry]) -> dict[int, list[int]]:
    """Slots the hour table references, with the hours (0-23) at which each is live."""
    used: dict[int, list[int]] = {}
    for h, e in enumerate(hour_table):
        for s in {e.snapshot1, e.snapshot2}:
            used.setdefault(s, []).append(h)
    return dict(sorted(used.items()))
