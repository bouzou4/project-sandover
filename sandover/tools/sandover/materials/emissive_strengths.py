"""Relative emissive strengths: the reviewer's ladder math and ``emissive-strengths.csv``.

Strengths are relative to the torch anchor at 1.0 (``emissive_maps``) and live on a log rail
from ``LADDER_MIN`` to ``LADDER_MAX``; a nudge is a quarter octave (``NUDGE``).

``emissive-strengths.csv`` (``$SANDOVER_DATA/materials/``): ``combo_id, tpage, name, strength,
source (default|human), note, timestamp``. Append-only, one row per change, so closing the page
never loses a verdict; the last row per key (``combo_id``, or ``name`` when blank) wins.
``sandover-unbake export-sidecar`` reads it (``sandover.unbake.sidecar.load_e_factors``).
"""

from __future__ import annotations

import csv
import math
from datetime import datetime, timezone
from pathlib import Path

from sandover.materials.sheets import materials_dir, read_csv

FIELDS = ["combo_id", "tpage", "name", "strength", "source", "note", "timestamp"]
SOURCES = {"default", "human"}
LADDER_MIN = 1 / 64
LADDER_MAX = 16.0
NUDGE = 2 ** 0.25


def strengths_path() -> Path:
    return materials_dir() / "emissive-strengths.csv"


# ---- ladder math ----------------------------------------------------------------------------

def clamp_strength(s: float) -> float:
    return min(max(float(s), LADDER_MIN), LADDER_MAX)


def rail_pos(s: float) -> float:
    """Strength -> position on the rail, 0 at ``LADDER_MIN`` and 1 at ``LADDER_MAX`` (log scale)."""
    lo, hi = math.log(LADDER_MIN), math.log(LADDER_MAX)
    return (math.log(clamp_strength(s)) - lo) / (hi - lo)


def rail_strength(pos: float) -> float:
    """Inverse of :func:`rail_pos`, clamped to the rail."""
    lo, hi = math.log(LADDER_MIN), math.log(LADDER_MAX)
    return math.exp(lo + min(max(float(pos), 0.0), 1.0) * (hi - lo))


def nudge(s: float, steps: int) -> float:
    """``steps`` quarter-octaves up (positive) or down, clamped to the rail."""
    return clamp_strength(clamp_strength(s) * NUDGE ** steps)


def neighbours(strengths: dict[str, float], name: str, s: float, k: int = 3) -> list[tuple[str, float]]:
    """Up to ``k`` textures just below and ``k`` just above ``s`` (log distance), ``name`` excluded,
    sorted by strength."""
    others = [(n, v) for n, v in strengths.items() if n != name and v > 0]
    below = sorted((x for x in others if x[1] <= s), key=lambda x: -x[1])[:k]
    above = sorted((x for x in others if x[1] > s), key=lambda x: x[1])[:k]
    return sorted(below + above, key=lambda x: x[1])


# ---- CSV ------------------------------------------------------------------------------------

def strength_key(row: dict) -> str:
    return row.get("combo_id") or row.get("name", "")


def append_strength(path: Path, row: dict) -> dict:
    """Append one row (header written on first use); returns the row as written.

    ``source`` must be ``default`` or ``human`` and ``strength`` a positive number, else
    ``ValueError``. The timestamp is filled in here.
    """
    source = str(row.get("source", ""))
    if source not in SOURCES:
        raise ValueError(f"bad source {source!r}, expected one of {sorted(SOURCES)}")
    strength = float(row.get("strength", "nan"))
    if not strength > 0 or math.isinf(strength):
        raise ValueError(f"bad strength {row.get('strength')!r}")
    out = {k: str(row.get(k, "") or "") for k in FIELDS}
    out["strength"] = f"{strength:.4g}"
    out["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    path.parent.mkdir(parents=True, exist_ok=True)
    is_new = not path.exists()
    with path.open("a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if is_new:
            w.writeheader()
        w.writerow(out)
    return out


def read_strengths(path: Path | None = None) -> list[dict]:
    p = path or strengths_path()
    return read_csv(p) if p.is_file() else []


def latest_strengths(rows: list[dict]) -> dict[str, dict]:
    """Texture name -> its last row (file order); rows keyed by combo_id resolve through ``name``."""
    by_key: dict[str, dict] = {}
    for r in rows:
        k = strength_key(r)
        if k:
            by_key[k] = r
    return {r.get("name", ""): r for r in by_key.values() if r.get("name")}


# ---- emitter score (issue #161) -------------------------------------------------------------

def is_emitter(rec: dict, threshold: float) -> bool:
    """An ``emitter-score.json`` record counts as an emitter at or above ``threshold``, or when
    it is a ladder anchor or a human marked it one."""
    return (float(rec.get("score", 0.0)) >= threshold or bool(rec.get("anchor"))
            or rec.get("human") == "emitter")


def scored_queue(queue: list[dict], scores: dict) -> list[dict]:
    """The ladder queue reordered and re-defaulted by ``emitter-score.json`` (``scores`` as
    loaded, ``{"threshold", "textures": {name: record}}``).

    Each row gains ``score``, ``emitter`` and ``bake_default`` (its previous ``default``).
    Receivers (:func:`is_emitter` false) default to their
    ``ladder_default`` (the rail bottom) and sort last; emitters sort first by descending score;
    unscored rows sit between, in their original order. Returns new row dicts.
    """
    tex = scores.get("textures", {})
    thr = float(scores.get("threshold", 0.5))
    out = []
    for i, row in enumerate(queue):
        r = dict(row, bake_default=row.get("default", LADDER_MIN))
        rec = tex.get(row.get("name", ""))
        if rec is None:
            r.update(score=None, emitter=None)
            out.append((1, 0.0, i, r))
            continue
        s = float(rec.get("score", 0.0))
        emitter = is_emitter(rec, thr)
        r.update(score=s, emitter=emitter)
        if not emitter:
            r["default"] = clamp_strength(float(rec.get("ladder_default", LADDER_MIN)))
        out.append((0 if emitter else 2, -s, i, r))
    out.sort(key=lambda x: x[:3])
    return [x[3] for x in out]
