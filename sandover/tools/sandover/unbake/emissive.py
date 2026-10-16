"""Per-vertex emissive value E from the eight baked palette slots (issue #135).

E marks light that the bake put on a vertex from something other than the sun and ambient:
lamps, flames, lava, crystals. It has three sources, summed in palette u8 steps (128 = 1.0):

- effect: the luminance of every effect slot of the level (``levels.LEVELS[...].effects``);
  its runtime weight is that slot's palette weight.
- night: in night slots (sun slots the level's hour table has live at midnight and not at
  10:00-14:00), the part of the luminance above what moonlight alone would give, i.e. above
  ``night_ratio`` x the vertex's brightest day slot + ``NIGHT_MARGIN``. ``night_ratio`` is the
  level's median night/day ratio. On the ``village1`` table the lamps are baked into slots 6-7,
  not into an effect slot; this finds them. Runtime weight: the summed weight of the night slots.
- static (levels with two or more sun slots): the part of the dimmest sun slot above
  ``STATIC_T``, light on at every hour (lanterns lit by day too). Runtime weight 1.

The stored value is ``min(sum, 255) / 255`` with the index of the largest source:
0-7 a palette slot, ``SRC_STATIC`` or ``SRC_NIGHT``. ``emitter_mask`` marks the effect slots
whose role is a light source (flames, lamps, lava, crystal fades); lightning, caustics and
camera fades light surfaces from outside and are kept in E but left out of that mask.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .levels import LEVELS

LUMA = np.array([0.299, 0.587, 0.114])
SRC_STATIC = 8
SRC_NIGHT = 9
EMITTER_ROLES = {"flames", "light", "lava", "fade-lights"}
# per palette-interp table, from facts/mood.md Part 4: live at 0:00, not live 10:00-14:00
NIGHT_BY_TABLE = {"village1": [6, 7], "misty": [1], "rolling": [2, 3], "firecanyon": [3]}
HOUR_TABLE = {
    **dict.fromkeys(["village1", "beach", "jungle", "training", "finalboss"], "village1"),
    **dict.fromkeys(["misty", "village2", "swamp", "sunken", "sunkenb"], "misty"),
    **dict.fromkeys(["rolling", "ogre"], "rolling"),
    **dict.fromkeys(["firecanyon", "snow", "village3"], "firecanyon"),
}
STATIC_T = 96.0  # u8 steps: dimmest-slot light above this is on all day
NIGHT_MARGIN = 32.0  # u8 steps of night light above the moonlight prediction that count as zero


@dataclass
class EmissiveRoles:
    effect: list[int]
    night: list[int]
    day: list[int]
    emitter: list[int]
    night_ratio: float = 0.0

    @property
    def night_mask(self) -> int:
        return sum(1 << s for s in self.night)

    @property
    def emitter_mask(self) -> int:
        return sum(1 << s for s in self.emitter)


def luminance(colors: np.ndarray) -> np.ndarray:
    """(8,V,3) u8 palette colours -> (8,V) luminance in u8 steps."""
    return (np.asarray(colors, np.float64)[..., :3] * LUMA).sum(-1)


def roles(level: str) -> EmissiveRoles:
    """Slot roles for E from the role table and the level's hour table."""
    r = LEVELS[level]
    effect = sorted(r.effects)
    sun = [s for s in r.sun if s not in r.effects]
    table_night = NIGHT_BY_TABLE.get(HOUR_TABLE.get(level, ""), []) if r.category != "quick" else []
    night = [s for s in sun if s in table_night]
    day = [s for s in sun if s not in night]
    emitter = [s for s in effect if r.effects[s] in EMITTER_ROLES]
    return EmissiveRoles(effect, night, day, emitter)


def night_ratio(lum: np.ndarray, er: EmissiveRoles) -> float:
    """Median over lit vertices of (brightest night slot) / (brightest day slot)."""
    if not er.night or not er.day:
        return 0.0
    day = lum[er.day].max(0)
    lit = day > 32
    return float(np.median(lum[er.night].max(0)[lit] / day[lit])) if lit.any() else 0.0


def compute(lum: np.ndarray, er: EmissiveRoles) -> tuple[np.ndarray, np.ndarray]:
    """(8,V) luminance -> (E in [0,1] float32, source index uint8), both (V,)."""
    V = lum.shape[1]
    src = np.zeros((10, V))
    for s in er.effect:
        src[s] = lum[s]
    if er.night and er.day:
        moon = er.night_ratio * lum[er.day].max(0) + NIGHT_MARGIN
        src[SRC_NIGHT] = np.maximum(lum[er.night].max(0) - moon, 0.0)
    sun = er.day + er.night
    if len(sun) >= 2:  # one slot cannot tell "lit at every hour" from "lit"
        src[SRC_STATIC] = np.maximum(lum[sun].min(0) - STATIC_T, 0.0)
    total = src.sum(0)
    e = np.clip(total, 0, 255) / 255.0
    dom = np.where(total > 0, src.argmax(0), 0).astype(np.uint8)
    return e.astype(np.float32), dom
