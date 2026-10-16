"""Census of the game's fake light shafts: the particle launchers ``sp-light-shaft?`` hides.

The rule is the one in ``sparticle-launcher.gc`` (``sp-light-shaft?``): a ``defpart`` whose sprite
is anchored at one edge (``:rot-x 4``), at least 10 m long (``:scale-x``), living at least 4 s
(``:timer``), with no velocity or acceleration field. Everything else (eco and collectable glows,
lightning, boss beams, eco-room bursts) is short-lived or moving and stays.

For each match the census names its texture, the groups that launch it (``defpartgroup`` items
and direct ``launch-particles`` calls by part id) and the entities that spawn those groups per
level (``decompiler_out/jak1/entities/<level>-actors.json``, lump ``art-name``).

    uv run --no-sync python -m sandover.light_shafts [--json]
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from dataclasses import asdict, dataclass, field
from pathlib import Path

from sandover.paths import root as repo_root

METERS = 4096.0
TICKS_PER_SECOND = 300

_DEFPART = re.compile(r"\(defpart (\d+)\s*:init-specs\s*\((.*?)\)\)\s*(?=\n\s*\n|\n\(|\Z)", re.S)
_GROUP = re.compile(r"\(defpartgroup ([\w-]+)(.*?)(?=\n\(def|\Z)", re.S)
_MOVING = ("vel-x", "vel-y", "vel-z", "accel-x", "accel-y", "accel-z")


@dataclass
class Shaft:
    part: int
    file: str
    texture: str
    scale_x_m: tuple[float, float]
    timer_s: float
    groups: list[str] = field(default_factory=list)
    direct: list[str] = field(default_factory=list)  # states launching the part by id
    spawners: dict[str, int] = field(default_factory=dict)  # level -> entities


def _meters(tok: str) -> float | None:
    m = re.fullmatch(r"\(meters ([-\d.]+)\)", tok.strip())
    return float(m.group(1)) if m else None


def _spec(body: str, name: str) -> str | None:
    m = re.search(r"\(:" + re.escape(name) + r"((?:\s+(?:\([^()]*\)|[^\s()]+))+)\)", body)
    return m.group(1).strip() if m else None


def is_shaft(body: str) -> bool:
    """``sp-light-shaft?`` on a defpart's init-specs text."""
    if _spec(body, "rot-x") != "4":
        return False
    sx = _spec(body, "scale-x")
    sx0 = _meters(re.match(r"\(meters [-\d.]+\)", sx).group(0)) if sx and sx.startswith("(meters") else None
    tm = re.fullmatch(r"\(seconds ([\d.]+)\)", _spec(body, "timer") or "")
    if sx0 is None or sx0 < 10 or not tm or float(tm.group(1)) < 4:
        return False
    return not any(_spec(body, k) is not None for k in _MOVING)


def census(root: Path | None = None) -> list[Shaft]:
    root = Path(root or repo_root())
    src = root / "goal_src" / "jak1"
    texts = {f: f.read_text(encoding="utf-8", errors="ignore") for f in sorted(src.rglob("*.gc"))}
    shafts: list[Shaft] = []
    for f, t in texts.items():
        for m in _DEFPART.finditer(t):
            body = m.group(2)
            if not is_shaft(body):
                continue
            tex = re.search(r"\(:texture \((\S+) (\S+)\)\)", body)
            sx = re.findall(r"\(meters ([-\d.]+)\)", _spec(body, "scale-x") or "")
            tm = re.fullmatch(r"\(seconds ([\d.]+)\)", _spec(body, "timer") or "")
            shafts.append(Shaft(int(m.group(1)), f.relative_to(root).as_posix(),
                                f"{tex.group(2)}/{tex.group(1)}" if tex else "",
                                (float(sx[0]), float(sx[1]) if len(sx) > 1 else 0.0), float(tm.group(1))))
    for s in shafts:
        states: list[tuple[str, str]] = []
        for f, t in texts.items():
            for g in _GROUP.finditer(t):
                if re.search(rf"\(sp-item {s.part}\b", g.group(2)):
                    s.groups.append(g.group(1))
            for st in re.finditer(r"\(defstate ([\w-]+) \(([\w-]+)\)(.*?)(?=\n\(def|\Z)", t, re.S):
                if re.search(rf"\*part-id-table\* {s.part}\)", st.group(3)):
                    s.direct.append(st.group(1))
                    # part-spawner entities enter such a state by an art-name ending in its suffix
                    states.append((st.group(2), st.group(1).removeprefix(st.group(2) + "-")))
        ents = root / "decompiler_out" / "jak1" / "entities"
        for jf in sorted(ents.glob("*-actors.json")):
            n = sum(1 for a in (json.loads(jf.read_text()) or [])
                    if (art := str(a.get("lump", {}).get("art-name", ""))) in s.groups
                    or any(a.get("etype") == ty and art.endswith(sfx) for ty, sfx in states))
            if n:
                s.spawners[jf.name.removesuffix("-actors.json")] = n
    return shafts


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    rows = census()
    if a.json:
        print(json.dumps([asdict(r) for r in rows], indent=1))
        return 0
    for r in rows:
        print(f"part {r.part} ({r.file}) {r.texture} scale-x {r.scale_x_m[0]:g}+{r.scale_x_m[1]:g} m, "
              f"{r.timer_s:g} s; groups {r.groups or '-'}; launched directly in {r.direct or '-'}; "
              f"spawners {dict(Counter(r.spawners)) or '-'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
