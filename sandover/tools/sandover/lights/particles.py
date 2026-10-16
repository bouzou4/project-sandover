"""Particle groups (``defpartgroup``/``defpart`` in goal_src) and their light colour.

A group counts as a light when at least one of its parts is additive. Additive is read from
the part's ``(:flags (... bit3 ...))``: inferred, bit3 marks the glow textures (hotdot,
starflash, flare, lightning) and is absent on smoke, rock and water textures.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

_GROUP = re.compile(r"^\(defpartgroup\s+(\S+)", re.M)
_PART = re.compile(r"^\(defpart\s+(\d+)", re.M)
_ITEM = re.compile(r"\(sp-item\s+(\d+)([^()]*(?:\([^()]*\)[^()]*)*)\)")
_HOURS = re.compile(r":hour-mask\s+#b([01]+)")
_BIRTH = re.compile(r"\(:birth-func\s+'(\S+)\)")
# additive but not a light: water spray, drops and ripples
NOT_LIGHT_TEXTURES = ("lakedrop", "water-ring", "water-wave", "water-splash", "falls-particle",
                      "butterfly-wing", "buzzerwing")
_BOUNDS = re.compile(r":bounds\s+\(static-bspherem\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\)")
_TEX = re.compile(r"\(:texture\s+\((\S+)\s+\S+\)")
_FLAGS = re.compile(r"\(:flags\s+\(([^)]*)\)\)")
_COPY = re.compile(r"\(:([rgba])\s+:copy\s+([rgba])\)")
_NUM = r"-?\d+(?:\.\d+)?"
_CHAN = re.compile(rf"\(:([rgba])\s+({_NUM})(?:\s+({_NUM}))?(?:\s+({_NUM}))?\)")


@dataclass
class Part:
    id: int
    texture: str | None
    rgba: dict[str, float]  # mean 0..255, PS2 scale (128 = 1.0); a missing channel is 0
    additive: bool
    source: str
    birth: str | None = None

    @property
    def light(self) -> bool:
        return self.additive and bool(self.texture) and not self.texture.startswith(NOT_LIGHT_TEXTURES)


@dataclass
class Group:
    name: str
    items: list[int]
    radius_m: float
    source: str
    hours: dict[int, str] = field(default_factory=dict)  # part id -> 24-bit hour mask
    parts: list[Part] = field(default_factory=list)

    @property
    def additive_parts(self) -> list[Part]:
        return [p for p in self.parts if p.light]

    def hour_mask(self) -> str | None:
        """The ``#b`` hour mask (bit i = hour i) shared by every masked item, None when unmasked."""
        masks = set(self.hours.values())
        return masks.pop() if len(masks) == 1 else None

    def strength(self) -> float:
        """Brightest light part: max channel / 128 x alpha / 128 (alpha < 16 fades in, read as 64)."""
        best = 0.0
        for p in self.additive_parts:
            a = p.rgba.get("a", 128.0)
            a = 64.0 if a < 16.0 else a
            best = max(best, max(p.rgba.get(c, 0.0) for c in "rgb") / 128.0 * a / 128.0)
        return min(best, 2.0)

    @property
    def emits(self) -> bool:
        return bool(self.additive_parts)

    def colour(self) -> list[float] | None:
        """Alpha-weighted mean colour of the additive parts, 0..1 with 128 = 1.0."""
        ps = [p for p in self.additive_parts if not (p.birth and "color" in p.birth)]
        if not ps:
            return None
        w = [max(p.rgba.get("a", 128.0), 1.0) for p in ps]
        out = []
        for ch in "rgb":
            v = sum(p.rgba.get(ch, 0.0) * wi for p, wi in zip(ps, w)) / sum(w)
            out.append(round(min(v / 128.0, 1.0), 3))
        return out


def _chan_mean(m: re.Match) -> float:
    base = float(m.group(2))
    if m.group(4) is not None:  # float-int-rand: base, count, mult
        return base + float(m.group(3)) * float(m.group(4)) / 2.0
    if m.group(3) is not None:
        return base + float(m.group(3)) / 2.0
    return base


def _forms(text: str, start_re: re.Pattern) -> list[tuple[re.Match, str, int]]:
    """Top-level forms matching ``start_re``: (match, form text, line)."""
    out = []
    for m in start_re.finditer(text):
        depth, i = 0, m.start()
        for i in range(m.start(), len(text)):
            c = text[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            elif c == ";":
                j = text.find("\n", i)
                i = j if j >= 0 else len(text)
        out.append((m, text[m.start(): i + 1], text.count("\n", 0, m.start()) + 1))
    return out


def parse_text(text: str, rel: str) -> tuple[dict[str, Group], dict[int, Part]]:
    groups: dict[str, Group] = {}
    parts: dict[int, Part] = {}
    for m, form, line in _forms(text, _GROUP):
        b = _BOUNDS.search(form)
        items, hours = [], {}
        for it in _ITEM.finditer(form):
            items.append(int(it.group(1)))
            h = _HOURS.search(it.group(2))
            if h:
                hours[int(it.group(1))] = h.group(1)
        groups[m.group(1)] = Group(m.group(1), items, float(b.group(4)) if b else 4.0, f"{rel}:{line}", hours)
    for m, form, line in _forms(text, _PART):
        rgba = {c.group(1): _chan_mean(c) for c in _CHAN.finditer(form)}
        for c in _COPY.finditer(form):
            if c.group(2) in rgba:
                rgba[c.group(1)] = rgba[c.group(2)]
        tex = _TEX.search(form)
        fl = _FLAGS.search(form)
        bf = _BIRTH.search(form)
        parts[int(m.group(1))] = Part(int(m.group(1)), tex.group(1) if tex else None, rgba,
                                      bool(fl and "bit3" in fl.group(1).split()), f"{rel}:{line}",
                                      bf.group(1) if bf else None)
    return groups, parts


def load_groups(goal_src: Path) -> dict[str, Group]:
    """Every particle group under ``goal_src`` (the jak1 tree, ``old/`` skipped) with its parts."""
    groups: dict[str, Group] = {}
    parts: dict[int, Part] = {}
    for f in sorted(goal_src.rglob("*.gc")):
        if "old" in f.relative_to(goal_src).parts:
            continue
        text = f.read_text(encoding="utf-8", errors="ignore")
        if "defpart" not in text:
            continue
        rel = "goal_src/jak1/" + f.relative_to(goal_src).as_posix()
        g, p = parse_text(text, rel)
        groups.update(g)
        parts.update(p)
    for g in groups.values():
        g.parts = [parts[i] for i in dict.fromkeys(g.items) if i in parts]
    return groups
