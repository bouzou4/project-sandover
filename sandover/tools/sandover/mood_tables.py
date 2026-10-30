"""Mirror a level's time-of-day tables from mood-tables.gc into JSON for lightbench.

The engine evaluates these tables in GOAL (update-mood-palette / update-mood-fog). lightbench
runs without GOAL, so `--hour` reads this JSON instead. The vista renderer's neighbour moods
read the palette part from a C++ table built into gk (`NeighboursMood.inc`), written by the
same command. Regenerate both after editing the GOAL file:

    uv run sandover-mood-tables            # all 21 levels
    uv run sandover-mood-tables village1    # one level
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
MOOD_TABLES = REPO / "goal_src/jak1/engine/gfx/mood/mood-tables.gc"
OUT_DIR = REPO / "sandover/tools/data/mood"
CPP_TABLE = REPO / "game/graphics/opengl_renderer/NeighboursMood.inc"

# All 21 playable levels (kb/facts/mood.md Part 3/4). beach and sunkenb have no
# update-mood-* function of their own; they reuse village1's and sunken's.
LEVELS = [
    "village1", "beach", "jungle", "training", "finalboss",
    "misty", "village2", "swamp", "sunken", "sunkenb",
    "rolling", "ogre", "firecanyon", "snow", "village3",
    "maincave", "robocave", "darkcave", "jungleb", "lavatube", "citadel",
]

# These call update-mood-quick(0, 0, 0) instead of update-mood-palette (mood.gc);
# their palette-interp / fog-interp bindings are dead data.
QUICK_LEVELS = {"maincave", "robocave", "darkcave", "jungleb", "lavatube", "citadel"}
QUICK_SLOT = {"fog": 0, "sun": 0, "lights": 0}

# update-mood-village3 overwrites mood-lights-table slots 0-3 every frame after
# update-mood-palette, so the static data for those slots never reaches the renderer.
INDIRECT_LEVELS = {
    "village3": (
        "mood-lights-table slots 0-3 are overwritten every frame by "
        "update-mood-village3 (mood.gc:1366-1393); the static table above is not "
        "what the renderer actually uses for those slots."
    ),
}


def _define_body(src: str, name: str) -> str:
    start = src.index(f"(define {name}")
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                return src[start : i + 1]
    raise ValueError(name)


def _kw(text: str, key: str, default: float = 0.0) -> float:
    m = re.search(rf":{key}\s+(-?[\d.]+)", text)
    return float(m.group(1)) if m else default


def _vector(text: str, key: str) -> list[float]:
    m = re.search(rf":{key}\s*\(new 'static 'vector([^)]*)\)", text)
    body = m.group(1) if m else ""
    return [_kw(body, c) for c in "xyzw"]


def parse_interp(src: str, name: str) -> list[dict]:
    body = _define_body(src, name)
    hours = re.findall(r"\(new 'static 'sky-color-hour([^)]*)\)", body)
    assert len(hours) == 24, (name, len(hours))
    return [
        {
            "snapshot1": int(_kw(h, "snapshot1")),
            "snapshot2": int(_kw(h, "snapshot2")),
            "morph_start": _kw(h, "morph-start"),
            "morph_end": _kw(h, "morph-end"),
        }
        for h in hours
    ]


def parse_fog(src: str, name: str) -> list[dict]:
    body = _define_body(src, name)
    entries = re.split(r"\(new 'static\s+'mood-fog(?![-\w])", body)[1:]
    assert len(entries) == 8, (name, len(entries))
    return [
        {"fog_color": _vector(e, "fog-color"), "fog_dists": _vector(e, "fog-dists")}
        for e in entries
    ]


def binding(src: str, level: str, slot: str) -> str:
    m = re.search(rf"\(set! \(-> \*{level}-mood\* {slot}\) (\*[\w-]+\*)\)", src)
    if not m:
        raise ValueError(f"no {slot} binding for {level}")
    return m.group(1)


def evaluate_palette_hour(entries: list[dict], hour: float) -> tuple[int, int, float]:
    """(snapshot1, snapshot2, morph) at ``hour``, matching update-mood-palette's
    morph-start/morph-end lerp (mood.gc:146-204)."""
    idx = int(hour) % 24
    frac = hour - int(hour)
    e = entries[idx]
    morph = e["morph_start"] * (1.0 - frac) + e["morph_end"] * frac
    return e["snapshot1"], e["snapshot2"], morph


def extract(level: str) -> dict:
    src = MOOD_TABLES.read_text()
    fog_table = binding(src, level, "mood-fog-table")
    quick = level in QUICK_LEVELS
    indirect_note = INDIRECT_LEVELS.get(level)

    out = {
        "level": level,
        "source": "goal_src/jak1/engine/gfx/mood/mood-tables.gc",
        "mode": "quick" if quick else "cycle",
        "fog": parse_fog(src, fog_table),
        "quick_slot": QUICK_SLOT if quick else None,
        "indirect_binding": indirect_note is not None,
        "indirect_note": indirect_note,
    }

    palette = binding(src, level, "palette-interp")
    fog_interp = binding(src, level, "fog-interp")
    out["tables"] = {"palette_interp": palette, "fog_interp": fog_interp, "fog": fog_table}
    if quick:
        # Bindings exist (e.g. *default-interp-table*) but update-mood-quick never
        # reads them; the level's actual current-hour behavior is quick_slot above.
        out["palette_interp"] = None
        out["fog_interp"] = None
    else:
        out["palette_interp"] = parse_interp(src, palette)
        out["fog_interp"] = parse_interp(src, fog_interp)
    return out


def cpp_table() -> str:
    """Palette-interp rows of every level as C++ initializers for neighbours::MoodTable."""
    lines = [
        "// Generated by sandover-mood-tables from goal_src/jak1/engine/gfx/mood/mood-tables.gc.",
        "// {level, quick slot (-1: cycle), {snapshot1, snapshot2, morph start, morph end} x 24}",
    ]
    for level in LEVELS:
        data = extract(level)
        quick = data["quick_slot"]["lights"] if data["quick_slot"] else -1
        rows = data["palette_interp"] or [
            {"snapshot1": 0, "snapshot2": 0, "morph_start": 0.0, "morph_end": 0.0}
        ] * 24
        assert len(rows) == 24, level
        cells = ", ".join(
            f"{{{r['snapshot1']}, {r['snapshot2']}, {float(r['morph_start'])!r}f, {float(r['morph_end'])!r}f}}"
            for r in rows
        )
        lines.append(f'{{"{level}", {quick}, {{{cells}}}}},')
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("levels", nargs="*", help="defaults to all 21 levels")
    args = ap.parse_args()
    levels = args.levels or LEVELS
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for level in levels:
        out = OUT_DIR / f"{level}.json"
        out.write_text(json.dumps(extract(level), indent=1) + "\n", newline="\n")
        print(out.relative_to(REPO))
    if not args.levels:
        CPP_TABLE.write_text(cpp_table(), newline="\n")
        print(CPP_TABLE.relative_to(REPO))


if __name__ == "__main__":
    main()
