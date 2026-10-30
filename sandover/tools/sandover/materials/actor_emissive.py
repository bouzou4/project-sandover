"""Actor emissive table: E for sprite, particle and merc textures, which have no sidecar E.

Eco collectables, vents, power cells, precursor orbs, fire and beams are drawn by the sprite
renderer (particles) and merc (models), so the bake never flags them. They get E
from explicit override rows keyed by texture, the same rows the reviewer's ``E`` key writes
(``emissive-overrides.csv``, :mod:`sandover.materials.overrides`), with a ladder step in
``emissive-strengths.csv`` (:mod:`sandover.materials.emissive_strengths`).

``sandover-materials actor-emissive`` does two things:

- ``--seed`` appends those rows for the textures listed in ``sandover/presets/emissive.toml``
  ``[actor_emissive]``, with the ladder step chosen by :func:`ladder_step` from the element's
  baked peak colour (or the texture's explicit ``step``), raised by ``octaves``
  (:func:`default_step`). Rows already current are not appended again.
- Always: exports ``$SANDOVER_DATA/materials/emissive-actors.csv`` (``combo_id, name, e,
  min_chroma, lights``) from every texture whose latest override is ``emissive``, which the
  engine reads (``game/graphics/opengl_renderer/EmissiveTable.h``). ``e`` is the ladder strength
  times the torch's peak E, capped at 1. ``lights`` (0..1, default 1) is the share of that E
  that feeds the screen-space tile lights; bloom and the SSAO fade always take the full E.

The brightness rule (``kb/findings/2026-09-24-eco-brightness.md``): eco effects carry their
brightness in the vertex colour (x2) and in additive stacking, so their pixels already sit near
1.0 before bloom. Bloom adds about ``strength x w x (c - threshold)`` to a pixel of colour ``c``
inside a large element, ``w`` being the bloom weight ``smoothstep(lo, hi, E)``. Under the clamp
tonemap, the element's baked peak ``c`` must stay under ``cap`` after that add, which bounds
``w`` and so the ladder step. Under the FP16 target nothing clamps, so HDR output can raise the
step later without changing the rule's inputs.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import tomllib
from pathlib import Path

from sandover import paths
from sandover.materials import emissive_strengths as es
from sandover.materials import overrides as ov
from sandover.materials.sheets import materials_dir

FIELDS = ["combo_id", "name", "e", "min_chroma", "lights"]
SEED_NOTE = "actor emissive seed"


def table_path() -> Path:
    return materials_dir() / "emissive-actors.csv"


def preset_path() -> Path:
    return paths.root() / "sandover" / "presets" / "emissive.toml"


def load_config(path: Path | None = None) -> dict:
    """``[actor_emissive]`` of ``emissive.toml`` with defaults filled in."""
    raw = tomllib.loads((path or preset_path()).read_text(encoding="utf-8")).get("actor_emissive", {})
    cfg = {
        "ramp": tuple(raw.get("ramp", (0.1, 0.4))),
        "threshold": float(raw.get("threshold", 0.3)),
        "strength": float(raw.get("strength", 1.0)),
        "cap": float(raw.get("cap", 0.98)),
        "octaves": float(raw.get("octaves", 0.0)),
        "textures": [],
    }
    for t in raw.get("textures", []):
        if "name" not in t or "combo_id" not in t:
            raise ValueError(f"actor_emissive texture needs name and combo_id: {t}")
        cfg["textures"].append({
            "name": t["name"], "combo_id": t["combo_id"], "tpage": t.get("tpage", ""),
            "element": t.get("element", ""), "peak": float(t.get("peak", 0.0)),
            "min_chroma": float(t.get("min_chroma", 0.0)), "note": t.get("note", ""),
            "lights": min(1.0, max(0.0, float(t.get("lights", 1.0)))),
            "step": float(t["step"]) if "step" in t else None,
        })
    return cfg


# ---- the brightness rule --------------------------------------------------------------------

def bloom_weight(e: float, lo: float, hi: float) -> float:
    """The engine's bloom and SSAO gate: ``smoothstep(lo, hi, e)``."""
    x = min(max((e - lo) / max(hi - lo, 1e-6), 0.0), 1.0)
    return x * x * (3 - 2 * x)


def e_for_weight(w: float, lo: float, hi: float) -> float:
    """Inverse of :func:`bloom_weight` on ``[lo, hi]`` (bisection; the smoothstep is monotone)."""
    w = min(max(w, 0.0), 1.0)
    a, b = 0.0, 1.0
    for _ in range(40):
        m = (a + b) / 2
        if m * m * (3 - 2 * m) < w:
            a = m
        else:
            b = m
    return lo + (a + b) / 2 * (hi - lo)


def max_weight(peak: float, threshold: float, strength: float = 1.0, cap: float = 0.98) -> float:
    """Largest bloom weight that keeps a baked colour ``peak`` at or under ``cap`` after bloom.

    Bloom adds ``strength x w x (peak - threshold)`` inside a large element (a small one keeps
    less of the kernel, so this is conservative). A peak at or above ``cap`` already clips
    without bloom: 0. A peak at or below the threshold gets nothing added: 1.
    """
    if peak >= cap:
        return 0.0
    if peak <= threshold or strength <= 0:
        return 1.0
    return min(1.0, (cap - peak) / (strength * (peak - threshold)))


def ladder_step(peak: float, cfg: dict, torch_peak: float = 1.0) -> float:
    """Highest quarter-octave ladder strength whose E keeps ``peak`` under the cap."""
    lo, hi = cfg["ramp"]
    w = max_weight(peak, cfg["threshold"], cfg["strength"], cfg["cap"])
    e = e_for_weight(w, lo, hi) if w > 0 else es.LADDER_MIN * torch_peak
    s = e / max(torch_peak, 1e-6)
    k = math.floor(math.log(max(s, es.LADDER_MIN)) / math.log(es.NUDGE) + 1e-9)
    return es.clamp_strength(es.NUDGE ** k)


def default_step(t: dict, cfg: dict, torch_peak: float = 1.0) -> float:
    """Seeded ladder step of a configured texture: its explicit ``step`` or :func:`ladder_step`,
    raised by ``cfg["octaves"]`` and snapped to a quarter octave.

    ``step`` is for elements whose peak already clips in vanilla, where the rule has no safe step
    above the ramp. ``octaves`` is the look on top of the rule (1 = the eco-001 pick, D).
    """
    s = t["step"] if t.get("step") is not None else ladder_step(t["peak"], cfg, torch_peak)
    s *= 2.0 ** cfg.get("octaves", 0.0)
    k = round(math.log(max(s, es.LADDER_MIN)) / math.log(es.NUDGE))
    return es.clamp_strength(es.NUDGE ** k)


def torch_peak(index: Path | None = None) -> float:
    """Peak E of the torch anchor (``emissive-maps/index.json`` ``reference``), else 1.0."""
    p = index or materials_dir() / "emissive-maps" / "index.json"
    if p.is_file():
        ref = json.loads(p.read_text(encoding="utf-8")).get("reference", {})
        if ref.get("peak"):
            return float(ref["peak"])
    return 1.0


# ---- rows -----------------------------------------------------------------------------------

def seed_rows(cfg: dict, override_rows: list[dict], strength_rows: list[dict],
              peak_e: float = 1.0) -> tuple[list[dict], list[dict]]:
    """Override and strength rows to append for the configured textures.

    A texture gets an ``emissive`` override unless its latest verdict already is one (a human
    ``not-emissive`` is left alone), and a ladder row from :func:`ladder_step` unless its latest
    strength row already has that value or is a human's.
    """
    verdicts = ov.latest_verdicts(override_rows)
    latest = {}
    for r in strength_rows:
        k = es.strength_key(r)
        if k:
            latest[k] = r
    new_ov, new_st = [], []
    for t in cfg["textures"]:
        key = t["combo_id"]
        base = {"combo_id": t["combo_id"], "tpage": t["tpage"], "name": t["name"]}
        v = verdicts.get(key) or verdicts.get(t["name"])
        if v == "not-emissive":
            continue
        if v != "emissive":
            new_ov.append({**base, "level": "", "draw_id": t["element"], "verdict": "emissive",
                           "note": SEED_NOTE})
        s = default_step(t, cfg, peak_e)
        cur = latest.get(key) or latest.get(t["name"])
        if cur and (cur.get("source") == "human" or abs(float(cur["strength"]) - s) < 1e-3 * s):
            continue
        oct_note = f" +{cfg.get('octaves', 0.0):g} octave" if cfg.get("octaves") else ""
        new_st.append({**base, "strength": s, "source": "default",
                       "note": f"{SEED_NOTE}: peak {t['peak']:.2f}{oct_note}"})
    return new_ov, new_st


def export_rows(cfg: dict, override_rows: list[dict], strength_rows: list[dict],
                peak_e: float = 1.0) -> list[dict]:
    """Engine table rows: every texture whose latest verdict is ``emissive`` and has a combo_id.

    ``e`` = latest ladder strength x ``peak_e`` (capped at 1; 1.0 x ``peak_e`` without a ladder
    row, the torch); ``min_chroma`` and ``lights`` from the config, 0 and 1 for textures it does
    not list.
    """
    verdicts = ov.latest_verdicts(override_rows)
    names = {}
    for r in override_rows:
        k = ov.override_key(r)
        if k:
            names[k] = r
    strengths = {}
    for r in strength_rows:
        k = es.strength_key(r)
        if k:
            strengths[k] = r
    chroma = {t["combo_id"]: t["min_chroma"] for t in cfg["textures"]}
    lights = {t["combo_id"]: t.get("lights", 1.0) for t in cfg["textures"]}
    out = []
    for key, verdict in sorted(verdicts.items()):
        row = names[key]
        combo = row.get("combo_id", "")
        if verdict != "emissive" or not combo:
            continue
        st = strengths.get(combo) or strengths.get(row.get("name", ""))
        s = float(st["strength"]) if st else 1.0
        out.append({"combo_id": combo, "name": row.get("name", ""),
                    "e": f"{min(1.0, s * peak_e):.4f}", "min_chroma": f"{chroma.get(combo, 0.0):.2f}",
                    "lights": f"{lights.get(combo, 1.0):.2f}"})
    return out


def write_table(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-materials actor-emissive", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", action="store_true",
                    help="append override and ladder rows for the [actor_emissive] textures first")
    ap.add_argument("--preset", type=Path, default=None, help="default sandover/presets/emissive.toml")
    ap.add_argument("--overrides", type=Path, default=None, help="default emissive-overrides.csv")
    ap.add_argument("--strengths", type=Path, default=None, help="default emissive-strengths.csv")
    ap.add_argument("--out", type=Path, default=None, help="default emissive-actors.csv")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args(argv)

    cfg = load_config(args.preset)
    ov_path = args.overrides or ov.overrides_path()
    st_path = args.strengths or es.strengths_path()
    peak_e = torch_peak()
    if args.seed:
        new_ov, new_st = seed_rows(cfg, ov.read_overrides(ov_path), es.read_strengths(st_path), peak_e)
        for r in new_ov:
            print(f"override  {r['name']:<28} emissive")
            if not args.dry_run:
                ov.append_override(ov_path, r)
        for r in new_st:
            print(f"ladder    {r['name']:<28} {r['strength']:.4g}")
            if not args.dry_run:
                es.append_strength(st_path, r)
    rows = export_rows(cfg, ov.read_overrides(ov_path), es.read_strengths(st_path), peak_e)
    for r in rows:
        print(f"table     {r['name']:<28} e {r['e']}  min_chroma {r['min_chroma']}  lights {r['lights']}")
    if not args.dry_run:
        out = args.out or table_path()
        write_table(out, rows)
        print(f"{len(rows)} textures -> {out}")
    return 0
