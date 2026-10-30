"""``sandover-lights census``: every light source of each Jak 1 level, offline.

Five producers, one record shape (see ``kb/howto/lights.md``):

- ``entity``: actors whose type glows (eco, vents, orbs, cells, crystals, lamps, lava...),
  classified in ``entity_types.json`` (type -> colour, radius, state, source line);
- ``particle``: part-spawner actors (``*-part``) whose group has additive light parts;
- ``static``: clusters of emissive-lit tfrag/tie triangles on emitter-scored textures;
- ``vanilla-light``: positional lights the game itself uses (``vanilla_lights.json``);
- ``prop``: decorative props given a soft light by design (``prop_lights.json``, ``props.py``).

Positions are metres in game world space (the entity tables' unit; raw units / 4096).
Output: ``$SANDOVER_DATA/lights/<level>.json``, ``summary.md`` and ``sheets/<level>.png``.
"""

from __future__ import annotations

import json
import re
from collections import Counter
from pathlib import Path

import numpy as np

from sandover.lights.particles import Group

KINDS = ("entity", "particle", "static", "vanilla-light", "prop")
HERE = Path(__file__).resolve().parent
PICKUP = {1: "eco-yellow", 2: "eco-red", 3: "eco-blue", 4: "eco-green", 5: "money", 6: "fuel-cell",
          7: "eco-pill", 8: "buzzer", 9: "eco-pill-random"}  # fact-h.gc pickup-type


def rnd(v, n: int = 3) -> list[float]:
    return [round(float(x), n) for x in v]


def load_json(name: str, path: Path | None = None) -> dict:
    return json.loads((path or HERE / name).read_text(encoding="utf-8"))


def game_tasks(goal_src: Path) -> dict[int, str]:
    """game-task enum (engine/game/task/game-task-h.gc): id -> name."""
    text = (goal_src / "engine/game/task/game-task-h.gc").read_text(encoding="utf-8")
    body = text[text.index("(defenum game-task"):]
    body = body[: body.find("(def", 1)]
    return {int(i): n for n, i in re.findall(r"\((\S+) (\d+)\)", body)}


def load_actors(ent_dir: Path, level: str) -> list[dict]:
    p = ent_dir / f"{level}-actors.json"
    return (json.loads(p.read_text(encoding="utf-8")) or []) if p.is_file() else []


def record(lid, kind, name, pos, colour, radius, state, source, **extra) -> dict:
    r = {"id": lid, "kind": kind, "type": name, "pos": rnd(pos), "colour": rnd(colour) if colour is not None else None,
         "radius_hint_m": round(float(radius), 2), "state": state, "source": source}
    r.update({k: v for k, v in extra.items() if v is not None})
    return r


def task_state(pickup: int, amount: int, tasks: dict[int, str], via: str = "") -> dict:
    """State of a pickup: cells and scout flies by their game task, the rest by the entity."""
    if pickup == 6:
        t = tasks.get(amount, str(amount))
        return {"kind": "task", "var": f"{t} closed (cell collected){via}", "task": t}
    if pickup == 8:
        t = tasks.get(amount & 0xFFFF, str(amount & 0xFFFF))
        return {"kind": "task", "var": f"scout fly {amount >> 16} of {t} collected{via}", "task": t}
    return {"kind": "entity-flag", "var": f"pickup collected (entity perm dead){via}"}


VALVE_TASK = {3: "jungle-eggtop", 2: "red-eggtop", 1: "snow-eggtop"}  # collectables.gc vent init
NO_CONTENTS = ("'darkeco", "'bucket")  # crates.gc: these looks default to no pickup


def vent_state(lump: dict, pickup: int, names: dict[int, str]) -> dict:
    """Vent gate (collectables.gc vent init): valve task, alt-actor blocker, or a script."""
    opts = int(lump.get("options") or 0)
    alt = lump.get("alt-actor")
    alt = alt[0] if isinstance(alt, list) else alt
    if opts & 1 and opts & 4 and pickup in VALVE_TASK:
        return {"kind": "task", "var": f"open once {VALVE_TASK[pickup]} is complete (ecovalve cap)",
                "task": VALVE_TASK[pickup]}
    if alt is not None:
        return {"kind": "entity-flag", "var": f"open once alt-actor {names.get(int(alt), alt)} is perm complete",
                "entity": names.get(int(alt), str(alt))}
    if opts & 1:
        return {"kind": "entity-flag", "var": "blocked until a 'show event (script)"}
    return {"kind": "always", "var": "open vent, never used up"}


def pickup_table(table: dict) -> dict[int, dict]:
    raw = table.get("pickup_emit") or table.get("types", {}).get("crate", {}).get("contents_emit")         or table.get("pickup_colours", {})
    return {int(k): v for k, v in raw.items()}


def entity_lights(level: str, actors: list[dict], table: dict, tasks: dict[int, str],
                  src: str) -> tuple[list[dict], Counter]:
    """Records for actors whose type emits (``maybe`` ones flagged), plus a count of the rest."""
    types = table["types"]
    pick = pickup_table(table)
    names = {a.get("aid"): a.get("lump", {}).get("name", "") for a in actors}
    out, notes = [], Counter()
    for row, a in enumerate(actors):
        et, lump = a["etype"], a.get("lump", {})
        if et.endswith("-part"):
            continue
        t = types.get(et)
        if t is None:
            notes[f"{et} (unclassified)"] += 1
            continue
        ei = lump.get("eco-info")
        ei = [int(x) for x in ei] if isinstance(ei, list) and ei else None
        name = lump.get("name", f"{et}-{a.get('aid')}")
        base = {"level": level, "aid": a.get("aid"), "moves": t.get("moves", False)}
        if et == "crate":
            if not ei or ei[0] == 0:
                if str(lump.get("crate-type")) in NO_CONTENTS or ei:
                    continue
                ei = [9, 1]  # crates.gc: no eco-info drops eco-pill-random
            p = pick.get(ei[0])
            if not p or p.get("emits", True) is False:
                continue
            amount = ei[1] if len(ei) > 1 else 1
            out.append(record(f"{level}:{name}", "entity", f"crate/{PICKUP.get(ei[0], ei[0])}", a["trans"][:3],
                              p.get("colour"), p.get("radius_hint_m", 1.0),
                              task_state(ei[0], amount, tasks, "; hidden until the crate breaks"),
                              f"{src}#{row}", contents={"pickup": PICKUP.get(ei[0]), "amount": amount},
                              type_source=t.get("source"), maybe=True if p.get("emits") == "maybe" else None, **base))
            continue
        if t["emits"] is False:
            continue
        if t["emits"] == "maybe":
            notes[f"{et} (maybe, listed with maybe=true)"] += 1
        colour = t.get("colour")
        state = {k: v for k, v in (t.get("state") or {"kind": "always", "var": ""}).items() if k in ("kind", "var", "task")}
        if t.get("colour_from") == "eco-info" and ei and ei[0] in pick:
            if pick[ei[0]].get("emits", True) is False:
                continue
            colour = pick[ei[0]].get("colour", colour)
        if et in ("ecovent", "ventblue"):
            state = vent_state(lump, ei[0] if ei else 3, names)
        elif ei and ei[0] in (6, 8) and len(ei) > 1:
            state = task_state(ei[0], ei[1], tasks)
        out.append(record(f"{level}:{name}", "entity", et, a["trans"][:3], colour, t.get("radius_hint_m", 2.0),
                          state, f"{src}#{row}", type_source=t.get("source"),
                          maybe=True if t["emits"] == "maybe" else None, **base))
    return out, notes


def hours(mask: str) -> str:
    """Hour list of a 24-bit ``:hour-mask`` (bit i = hour i, written MSB first). The launcher
    skips an item at the hours whose bit is set (sparticle-launcher.gc:746), so these are the
    hours it is off."""
    return ",".join(str(h) for h in sorted(23 - i for i, c in enumerate(mask.zfill(24)) if c == "1"))


def particle_lights(level: str, actors: list[dict], groups: dict[str, Group],
                    src: str) -> tuple[list[dict], Counter]:
    out, notes = [], Counter()
    for row, a in enumerate(actors):
        if not a["etype"].endswith("-part"):
            continue
        lump = a.get("lump", {})
        art = lump.get("art-name")
        gname = art[0] if isinstance(art, list) else art
        g = groups.get(gname) if isinstance(gname, str) else None
        if gname is None:
            notes["part spawner without art-name (spawns nothing)"] += 1
            continue
        if g is None:
            notes[f"{gname} (group not in goal_src)"] += 1
            continue
        if not g.emits:
            notes["no additive light parts"] += 1
            continue
        mask = g.hour_mask()
        state = ({"kind": "time-of-day", "var": f"off at hours {hours(mask)}"} if mask else
                 {"kind": "always", "var": "part-spawner (off once its entity is perm complete)"})
        out.append(record(f"{level}:{lump.get('name', gname)}", "particle", g.name, a["trans"][:3], g.colour(),
                          min(max(g.radius_m, 2.0), 10.0), state, f"{src}#{row}", level=level,
                          aid=a.get("aid"), group_source=g.source, intensity=round(g.strength(), 3),
                          textures=sorted({p.texture for p in g.additive_parts})))
    return out, notes


def static_lights(level: str, data: Path, emitters: dict[str, dict], covered: float, maps: Path,
                  tex_root: Path, tex_index: dict) -> list[dict]:
    from sandover.lights.statics import level_clusters, texture_colour
    from sandover.materials.emitter_score import load_albedo, load_map
    from sandover.unbake import sidecar

    glb = data / "unbake/riproot/decompiler_out/jak1/levels" / level / f"{level}-background.glb"
    sc = sidecar.scan_dir(data / "lighting") / f"{level}.unbake"
    if not glb.is_file() or not sc.is_file():
        return []
    colours: dict[str, list[float] | None] = {}
    out = []
    for i, c in enumerate(level_clusters(glb, sc, set(emitters), covered)):
        mix, wsum = np.zeros(3), 0.0
        for tex, share in c.textures.items():
            if tex not in colours:
                tp = emitters[tex].get("tpage", "")
                colours[tex] = texture_colour(load_map(maps, tp, tex), load_albedo(tex_root, tex_index, tp, tex), covered)
            if colours[tex] is not None:
                mix += share * np.array(colours[tex])
                wsum += share
        out.append(record(f"{level}:static:{i}", "static", c.texture, c.pos, list(mix / wsum) if wsum else None,
                          min(max(3.0, c.extent_m), 12.0), {"kind": "always", "var": "baked vertex E (mood slot)"},
                          f"{glb.name}:{c.tree}", level=level, energy=round(c.energy, 3), tris=c.tris,
                          extent_m=round(c.extent_m, 2), textures=c.textures, role=emitters[c.texture].get("role"),
                          e_source=c.source))
    return out


def prop_lights(level: str, data: Path, table: dict, tex_root: Path) -> list[dict]:
    """Props of ``prop_lights.json`` on ``level``: one always-on record per cluster. No
    ``textures``/``intensity`` fields, so an older bake skips the kind."""
    from sandover.lights.props import level_props, prop_colour, texture_linear_colour

    glb = data / "unbake/riproot/decompiler_out/jak1/levels" / level / f"{level}-background.glb"
    tex_cfg = table.get("textures", {})
    colours: dict[str, list[float] | None] = {}
    out = []
    for i, p in enumerate(level_props(glb, tex_cfg, level)):
        cfg = tex_cfg[p.texture]
        if p.texture not in colours:
            pngs = sorted(tex_root.glob(f"{level}-*/{p.texture}.png")) or sorted(tex_root.glob(f"*/{p.texture}.png"))
            colours[p.texture] = texture_linear_colour(pngs[0]) if pngs else None
        vtx = None if cfg.get("colour_from") == "texture" else p.vtx_colour
        out.append(record(f"{level}:prop:{p.texture}:{i}", "prop", p.texture, p.pos,
                          cfg.get("colour") or prop_colour(colours[p.texture], vtx),
                          float(cfg.get("radius_hint_m", 4.0)),
                          {"kind": "always", "var": "decorative prop lit by design (prop_lights.json)"},
                          f"{glb.name}:{p.tree}", level=level, gain=float(cfg.get("gain", 1.0)),
                          extent_m=round(p.extent_m, 2), verts=p.verts))
    return out


def ground_y(fp, x: float, z: float, r: float = 6.0) -> float | None:
    """Median height of footprint vertices within ``r`` m (xz) of the point."""
    if fp is None or not len(fp):
        return None
    d2 = (fp[:, 0] - x) ** 2 + (fp[:, 2] - z) ** 2
    near = fp[d2 < r * r]
    return float(np.median(near[:, 1])) if len(near) else None


def vanilla_lights(level: str, table: dict, fp=None) -> list[dict]:
    """Positional entries of ``vanilla_lights.json`` for ``level``; directional ones have no pos.

    Entries measured in xz only get y from the level footprint (``y_from_ground``). Colours
    above 1 (finalboss under-glow) are divided by their max, which is kept as ``gain``.
    """
    out = []
    for i, v in enumerate(table.get("lights", [])):
        if v.get("level") != level or v.get("pos") is None:
            continue
        drv = str(v.get("driver", "always"))
        head = drv.split(":")[0].strip()
        kind = "task" if "task" in head else head if head in ("always", "time-of-day") else "entity-flag"
        pos = list(v["pos"])
        est = None
        if pos[1] is None:
            pos[1] = ground_y(fp, pos[0], pos[2])
            est = True
            if pos[1] is None:
                pos[1] = 0.0
        col, gain = v.get("colour"), None
        if col and max(col) > 1.0:
            gain = max(col)
            col = [c / gain for c in col]
        out.append(record(f"{level}:vanilla:{i}", "vanilla-light", v.get("name", ""), pos, col,
                          v.get("radius_hint_m") or 6.0, {"kind": kind, "var": drv}, v.get("source", ""),
                          level=level, vanilla_kind=v.get("kind"), affects=v.get("affects"),
                          light_group=v.get("light_group"), y_from_ground=est, gain=gain))
    return out
