"""Light-source inventory: everything that looks like a light, and whether it lights.

Per level, one row per candidate kind with its count and status against the baked world light
table (``bake_rows``):

- ``entity``: census entity types (``entity_types.json`` emits true or maybe): rows baked vs
  census rows, and the bake's reason for the rest (``maybe``, ``emits: false``);
- ``static``: census static clusters per texture: baked, or dropped by a cap or a non-emitter slot;
- ``particle``: census part spawners per group: baked, or no light part;
- ``prop``: census props (``prop_lights.json``);
- ``texture``: background textures that look like lights by name (``GLOW_NAME``) but have no
  baked E, so no census producer sees them: clusters of their geometry, and the median distance
  to the nearest baked light (a lamp's glass next to its baked lamp row is already lit).

Writes ``$SANDOVER_DATA/lights/inventory/inventory.json`` and ``inventory.md`` (a subfolder: the
bake reads every ``lights/*.json`` as a level); the classification (add /
already lit / should not light) is the finding that reads it, not this tool.
"""

from __future__ import annotations

import json
import re
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

GLOW_NAME = re.compile(r"glow|light|lamp|lantern|torch|flame|fire|candle|crystal|coil|energy|beam|"
                       r"mushroom|shroom|lava|bluelight|eco-valve-inside|hot-", re.I)
NOT_LIGHT = re.compile(r"sky|cloud|water|envmap|environment-|black|shadow|lightning", re.I)


def _baked_index(rows: list[dict]) -> dict[str, dict]:
    by: dict[str, dict] = defaultdict(lambda: {"ids": set(), "pos": []})
    for r in rows:
        by[r["level"]]["ids"].add(r["id"])
        by[r["level"]]["pos"].append(r["pos"])
    return by


def _skip_reason(baker, rec: dict) -> str:
    if rec.get("maybe") and rec.get("type") not in baker.bright:
        return "maybe (unconfirmed)"
    kind = rec["kind"]
    if kind == "entity":
        got, why = baker.entity(rec)
    elif kind == "particle":
        got, why = baker.particle(rec)
    elif kind == "static":
        got, why = baker.static(rec)
        why = why or "beyond the per-level cap"
    else:
        return "not baked"
    return why or "not baked"


def census_rows(census: dict[str, list[dict]], rows: list[dict], baker) -> list[dict]:
    baked = _baked_index(rows)
    agg: dict[tuple, dict] = {}
    for lv, recs in census.items():
        ids = baked[lv]["ids"]
        for r in recs:
            if r["kind"] == "vanilla-light":
                continue
            key = (lv, r["kind"], r["type"])
            a = agg.setdefault(key, {"level": lv, "kind": r["kind"], "name": r["type"], "count": 0,
                                     "lit": 0, "unlit_reasons": Counter(), "colour": r.get("colour")})
            a["count"] += 1
            if r["id"] in ids:
                a["lit"] += 1
            else:
                a["unlit_reasons"][_skip_reason(baker, r)] += 1
    out = []
    for a in agg.values():
        a["unlit_reasons"] = dict(a["unlit_reasons"])
        out.append(a)
    return out


def texture_rows(levels: list[str], data: Path, emitters: set[str], props: set[str],
                 rows: list[dict], names: dict[str, set[str]]) -> list[dict]:
    """Glow-named background textures without baked E, per level (``names``: level -> textures)."""
    from sandover.lights.props import texture_points
    from sandover.lights.statics import cluster_points

    baked = _baked_index(rows)
    out = []
    for lv in levels:
        want = {t for t in names.get(lv, ()) if t not in emitters and t not in props}
        glb = data / "unbake/riproot/decompiler_out/jak1/levels" / lv / f"{lv}-background.glb"
        if not want or not glb.is_file():
            continue
        lit = np.array(baked[lv]["pos"], np.float64).reshape(-1, 3)
        for tex, parts in sorted(texture_points(glb, want).items()):
            pts = np.concatenate([p for _, p in parts])
            cl = cluster_points(pts, np.ones(len(pts)), 1.0, 8.0)
            ctrs = np.array([c[0] for c in cl])
            near = (np.sqrt(((ctrs[:, None, :] - lit[None]) ** 2).sum(-1)).min(1) if len(lit)
                    else np.full(len(ctrs), np.inf))
            out.append({"level": lv, "kind": "texture", "name": tex, "count": len(cl), "lit": 0,
                        "trees": sorted({t for t, _ in parts}),
                        "near_lit_median_m": round(float(np.median(near)), 1),
                        "near_lit_share_4m": round(float((near < 4.0).mean()), 2),
                        "centre": [round(float(x), 1) for x in ctrs[0]], "unlit_reasons": {"no baked E": len(cl)}})
    return out


def glow_named_textures(materials_csv: Path) -> dict[str, set[str]]:
    """Background textures by level whose name looks like a light (materials.csv tpage prefix)."""
    import csv

    out: dict[str, set[str]] = defaultdict(set)
    with materials_csv.open(encoding="utf-8") as f:
        for r in csv.DictReader(f):
            tp, name = r["tpage"], r["name"]
            m = re.match(r"(\w+)-vis-(tfrag|shrub|alpha)", tp)
            bare = re.sub(r"^lava-", "", name)  # lavatube's texture prefix, not lava
            if m and GLOW_NAME.search(bare) and not NOT_LIGHT.search(name):
                out[m.group(1)].add(name)
    return out


def write_md(path: Path, items: list[dict]) -> None:
    lines = ["# Light-source inventory", "",
             "Everything that looks like a light, per level, and whether the world light table lights it "
             "(`sandover-lights inventory`). `lit` = rows in the table; `near lit` = median "
             "distance from a texture cluster to the nearest table light.", "",
             "| level | kind | name | count | lit | unlit (why) | near lit m | colour |", "|---|---|---|---|---|---|---|---|"]
    for a in sorted(items, key=lambda a: (a["level"], a["kind"], a["name"])):
        why = ", ".join(f"{k} {v}" for k, v in a["unlit_reasons"].items())
        col = ", ".join(f"{c:.2f}" for c in a["colour"]) if a.get("colour") else ""
        lines.append(f"| {a['level']} | {a['kind']} | {a['name']} | {a['count']} | {a['lit']} | {why} | "
                     f"{a.get('near_lit_median_m', '')} | {col} |")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def run(census_dir: Path, data: Path, levels: list[str] | None) -> Path:
    from sandover.lights.bake import Baker, DEFAULTS, bake_rows, settings
    from sandover.lights.census import load_json

    census = {f.stem: json.loads(f.read_text(encoding="utf-8")) for f in sorted(census_dir.glob("*.json"))
              if not levels or f.stem in levels}
    census = {k: v for k, v in census.items() if isinstance(v, list)}
    rows, _ = bake_rows(census_dir, list(census))
    baker = Baker({**DEFAULTS, **settings()})
    items = census_rows(census, rows, baker)
    score = json.loads((data / "materials/emitter-score.json").read_text(encoding="utf-8"))
    emitters = {k for k, v in score["textures"].items() if v.get("emitter")}
    props = set(load_json("prop_lights.json").get("textures", {}))
    items += texture_rows(list(census), data, emitters, props, rows,
                          glow_named_textures(data / "materials/materials.csv"))
    for a in items:
        a.pop("trees", None)
    d = census_dir / "inventory"
    d.mkdir(exist_ok=True)
    out = d / "inventory.md"
    (d / "inventory.json").write_text(json.dumps(items, indent=1), encoding="utf-8")
    write_md(out, items)
    return out
