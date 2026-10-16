"""Bake the light census into the engine's world light table (issue #191 phase 2).

Reads ``$SANDOVER_DATA/lights/<level>.json`` (``sandover-lights census``) and writes the C++
table ``game/graphics/opengl_renderer/WorldLights.inc`` (committed, compiled into gk like
``NeighboursMood.inc``), so a copied project root cannot lose it. ``--csv`` also writes the rows
as CSV for inspection. One row per light:

``level, id, x, y, z, r, g, b, radius_m, cls, gain, gate, param, aid``

- ``x y z``: metres, game world space; ``r g b``: 0..1; ``radius_m``: see below.
- ``cls``: which preset intensity scales the light (``CLASSES``).
- ``gain``: per-light factor on top (particle group strength, static energy share).
- ``gate``: what turns it on (``GATES``); ``param``: the gate's argument (hour mask, E source).
- ``aid``: the entity's actor id, for the gates GOAL answers.

Filter: entities whose type (or crate contents) ``entity_types.json`` marks ``emits: false``, so
a type decision takes effect at the next bake without a census rerun (precursor orbs, design
decision 2026-09-26), ``maybe`` entities (unless their type has since been marked ``emits: true``;
a type's ``bake`` entry can set its class and gate), vanilla lights (the game's own region kits, not emitters), particle
spawners with no light part, crates whose drops go to the entity pool (no entity to follow),
statics on non-emitter slots (lightning, caustics, fades, and the cave walls the crystals light,
``fade-lights``), and all but the ``static_cap`` biggest static clusters per level (by energy =
E x lit area) are left out; lava clusters (role ``lava``) have their own ``lava_cap``, so the
lava lights from the table rather than from screen tiles, and lamps (role ``night``) their own
``night_cap``, so a level's big lit floors cannot crowd its lamps out, and statics whose texture
is named as a light (``LIGHT_NAME``: rolling's ``rol-light`` posts, finalboss ``fin-light``) their
own ``named_cap``, and citadel lamp glass (``GLASS_NAME``: the ``cit-glass-*`` shield lamps; other
levels' glass stays a plain static) its own ``glass_cap``. Props (census kind
``prop``, ``prop_lights.json``) bake as class ``glow``, always on, up to ``prop_cap`` per level, when
the game draws them as lights (``props.glows_in_vanilla``). Radius: the census hint raised to the class floor (``radius_min``:
lamps hang a few metres over the street) and capped at ``radius_cap_m``. Settings:
``[emissive_lights.bake]`` in ``presets/emissive.toml``.
"""

from __future__ import annotations

import csv
import json
import math
import re
from collections import Counter
from pathlib import Path

from sandover.paths import root

CPP_TABLE_REL = "game/graphics/opengl_renderer/WorldLights.inc"

CLASSES = {"strong": 0, "weak": 1, "arc": 2, "particle": 3, "lamp": 4, "glow": 5}

# Must match WorldLights.h (Gate).
GATES = {
    "always": 0,
    "hour": 1,        # param: 24-bit mask of the hours the part spawner is off
    "slot": 2,        # param: E source (0-7 palette slot, 8 static, 9 night); the background's weight
    "alive": 10,      # entity process up, not dead/complete, not in an idle-dark state
    "crate": 11,      # crate broken and its contents not all collected
    "crystal": 12,    # cavecrystal lit (level follows its emissive colour)
    "task-open": 13,  # alive and the entity's task not complete
    "task-done": 14,  # alive and the entity's task complete
    "dark-crystal": 15,  # the model's emissive (dark at rest, full on each flash), the explosion's starflash
}

WEAK_TYPES = {"eco", "race-ring"}
CRATE_TYPES = {"crate/money", "crate/eco-blue", "crate/eco-yellow", "crate/eco-red", "crate/fuel-cell"}
ENTITY_GATE = {
    "cavecrystal": "crystal",
    "eggtop": "task-open", "snow-eggtop": "task-open", "oracle": "task-open",
    "energyhub": "task-open", "citb-robotboss": "task-open",
    "green-sagecage": "task-done", "red-sagecage": "task-done", "blue-sagecage": "task-done",
    "yellow-sagecage": "task-done",
    "lavabase": "always", "lavafall": "always", "lavafallsewera": "always", "lavafallsewerb": "always",
    "lavashortcut": "always", "lavatube-lava": "always", "ogre-lava": "always", "villagec-lava": "always",
    "dark-eco-pool": "always", "caveflamepots": "always", "minershort": "always",
}
PROP_CRYSTAL = 1  # glow row param bit, WorldLights.h kPropCrystal: not capped with the other props

# mood-slot roles of static E (emissive-maps index)
LAMP_ROLES = {"night", "flames"}
NON_EMITTER_ROLES = {"lightning", "caustics", "fade", "fade-lights"}
NIGHT_SOURCE = 9

LIGHT_NAME = re.compile(r"light|lamp|glow")
GLASS_NAME = re.compile(r"^cit-glass")

DEFAULTS = {"static_cap": 48, "lava_cap": 256, "night_cap": 64, "named_cap": 128, "glass_cap": 512, "glass_gain": 4608.0,
            "prop_cap": 256,
            "static_energy_ref": 1.0,
            "radius_cap_m": 12.0, "radius_min": {"lamp": 7.0}}

_HOURS = re.compile(r"hours\s+([\d,\s]+)")


def cpp_table_path() -> Path:
    return root() / CPP_TABLE_REL


def hour_mask(var: str) -> int:
    """Bit i set = the spawner is off at hour i (census ``time-of-day`` var)."""
    m = _HOURS.search(var)
    if not m:
        return 0
    return sum(1 << int(h) for h in m.group(1).replace(" ", "").split(",") if h != "")


def dark_types(table: dict | None = None) -> set[str]:
    """Census entity types that give no light: ``emits: false`` in ``entity_types.json``, and
    ``crate/<pickup>`` for crate contents marked so."""
    from sandover.lights.census import PICKUP, load_json

    t = table if table is not None else load_json("entity_types.json")
    out = {k for k, v in t.get("types", {}).items() if v.get("emits") is False}
    for k, v in t.get("types", {}).get("crate", {}).get("contents_emit", {}).items():
        if v.get("emits") is False:
            out.add(f"crate/{PICKUP.get(int(k), k)}")
    return out


def entity_bake(table: dict | None = None) -> tuple[set[str], dict[str, dict]]:
    """Types marked ``emits: true`` (their census ``maybe`` rows are kept) and each type's
    ``bake`` override ``{class, gate, param, gain, colour, radius_m, offset_m}`` from
    ``entity_types.json``; crate contents' overrides (``contents_emit``) as ``crate/<pickup>``."""
    from sandover.lights.census import load_json

    from sandover.lights.census import PICKUP

    t = (table if table is not None else load_json("entity_types.json")).get("types", {})
    bright = {k for k, v in t.items() if v.get("emits") is True}
    over = {k: v["bake"] for k, v in t.items() if isinstance(v.get("bake"), dict)}
    for k, v in t.get("crate", {}).get("contents_emit", {}).items():
        if isinstance(v.get("bake"), dict):
            over[f"crate/{PICKUP.get(int(k), k)}"] = v["bake"]
    return bright, over


def crystal_props() -> set[str]:
    """``prop_lights.json`` textures marked ``crystal``: each is its own light for a character."""
    from sandover.lights.census import load_json

    return {k for k, v in load_json("prop_lights.json").get("textures", {}).items() if v.get("crystal")}


def glow_verdicts(table: dict | None = None) -> dict[str, str]:
    """``prop_lights.json`` texture -> "" if it glows in vanilla, else the reason it gives no light."""
    from sandover.lights.census import load_json
    from sandover.lights.props import glows_in_vanilla

    t = (table if table is not None else load_json("prop_lights.json")).get("textures", {})
    return {k: glows_in_vanilla(v)[1] for k, v in t.items()}


def settings() -> dict:
    from sandover.presets import load

    try:
        cfg = load("emissive")["emissive_lights"].get("bake", {})
    except (FileNotFoundError, KeyError):
        cfg = {}
    return {**DEFAULTS, **cfg}


class Baker:
    def __init__(self, cfg: dict):
        self.cap = int(cfg["static_cap"])
        self.lava_cap = int(cfg.get("lava_cap", DEFAULTS["lava_cap"]))
        self.night_cap = int(cfg.get("night_cap", DEFAULTS["night_cap"]))
        self.prop_cap = int(cfg.get("prop_cap", DEFAULTS["prop_cap"]))
        self.named_cap = int(cfg.get("named_cap", DEFAULTS["named_cap"]))
        self.glass_cap = int(cfg.get("glass_cap", DEFAULTS["glass_cap"]))
        self.glass_gain = float(cfg.get("glass_gain", DEFAULTS["glass_gain"]))
        self.crystal_props = set(cfg["crystal_props"]) if "crystal_props" in cfg else crystal_props()
        self.prop_why = cfg["prop_verdicts"] if "prop_verdicts" in cfg else glow_verdicts()
        self.energy_ref = float(cfg["static_energy_ref"])
        self.radius_cap = float(cfg["radius_cap_m"])
        self.radius_min = {k: float(v) for k, v in (cfg.get("radius_min") or {}).items()}
        self.dark = set(cfg["dark_types"]) if "dark_types" in cfg else dark_types()
        if "bright_types" in cfg:
            self.bright = set(cfg["bright_types"])
            self.overrides = cfg.get("bake_overrides", {})
        else:
            self.bright, self.overrides = entity_bake()

    @staticmethod
    def pool(rec: dict) -> str:
        """The per-level cap a static counts against."""
        if rec.get("role") in ("lava", "night"):
            return rec["role"]
        tex = str(rec.get("type", ""))
        return "named" if LIGHT_NAME.search(tex) else "glass" if GLASS_NAME.search(tex) else "static"

    def row(self, rec: dict, cls: str, gain: float, gate: str, param: int = 0) -> dict:
        r = max(float(rec.get("radius_hint_m") or 3.0), self.radius_min.get(cls, 0.0))
        return {"level": rec["level"], "id": rec["id"], "pos": rec["pos"], "colour": rec["colour"],
                "radius_m": min(r, self.radius_cap), "cls": CLASSES[cls],
                "gain": round(float(gain), 4), "gate": GATES[gate], "param": int(param),
                "aid": int(rec.get("aid") or 0)}

    def entity(self, rec: dict) -> tuple[dict | None, str]:
        t = rec["type"]
        if t in self.dark:
            return None, "type gives no light (entity_types.json emits false)"
        if t.startswith("crate/"):
            if t not in CRATE_TYPES:
                return None, "crate dropping into the entity pool"
            if t not in self.overrides:
                return self.row(rec, "weak", 1.0, "crate"), ""
        if t in self.overrides:
            o = self.overrides[t]
            row = self.row(rec, o["class"], float(o.get("gain", 1.0)), o["gate"], int(o.get("param", 0)))
            if "colour" in o:
                row["colour"] = [float(c) for c in o["colour"]]
            if "radius_m" in o:
                row["radius_m"] = min(float(o["radius_m"]), self.radius_cap)
            if "offset_m" in o:  # entity origin at the base: light from the body's centre
                row["pos"] = [round(float(p) + float(d), 3) for p, d in zip(row["pos"], o["offset_m"])]
            return row, ""
        gate = ENTITY_GATE.get(t, "alive")
        return self.row(rec, "weak" if t in WEAK_TYPES else "strong", 1.0, gate), ""

    def particle(self, rec: dict) -> tuple[dict | None, str]:
        tex = rec.get("textures") or []
        strength = float(rec.get("intensity") or 0.0)
        if not tex or strength <= 0.0:
            return None, "particle spawner without a light part"
        st = rec["state"]
        gate, param = ("hour", hour_mask(st.get("var", ""))) if st["kind"] == "time-of-day" else ("always", 0)
        if any(x.startswith("lightning") for x in tex):
            return self.row(rec, "arc", 1.0, gate, param), ""
        return self.row(rec, "particle", strength, gate, param), ""

    def prop(self, rec: dict) -> dict:
        param = PROP_CRYSTAL if rec.get("type") in self.crystal_props else 0
        return self.row(rec, "glow", float(rec.get("gain", 1.0)), "always", param)

    def static(self, rec: dict) -> tuple[dict | None, str]:
        role = rec.get("role")
        if role in NON_EMITTER_ROLES:
            return None, f"static on a {role} slot"
        gain = min(1.0, float(rec.get("energy") or 0.0) / max(self.energy_ref, 1e-6))
        cls = "lamp" if role in LAMP_ROLES else "strong"
        if self.pool(rec) == "glass":
            # takes over the shield lamps' tiles at their power; weak: no actor light, as before
            cls, gain = "weak", gain * self.glass_gain
        src = rec.get("e_source")
        if src is None or not 0 <= int(src) <= NIGHT_SOURCE:
            src = NIGHT_SOURCE if role == "night" else None
        if src is None:
            return self.row(rec, cls, gain, "always"), ""
        return self.row(rec, cls, gain, "slot", int(src)), ""

    def level(self, recs: list[dict]) -> tuple[list[dict], Counter]:
        out, skipped, statics, props = [], Counter(), [], []
        for r in recs:
            if r.get("maybe") and r.get("type") not in self.bright:
                skipped["maybe"] += 1
                continue
            if r.get("colour") is None:
                skipped["no colour"] += 1
                continue
            vals = [*r["pos"], *r["colour"], r.get("radius_hint_m") or 3.0]
            if not all(isinstance(v, (int, float)) and math.isfinite(v) for v in vals):
                skipped["non-finite value"] += 1
                continue
            kind = r["kind"]
            if kind == "vanilla-light":
                skipped["vanilla light (the game's own region kit)"] += 1
                continue
            if kind == "static":
                statics.append(r)
                continue
            if kind == "prop":
                why = self.prop_why.get(r.get("type"), "prop not in prop_lights.json")
                if why:
                    skipped[why] += 1
                else:
                    props.append(r)
                continue
            got, why = self.entity(r) if kind == "entity" else self.particle(r)
            if got is None:
                skipped[why] += 1
            else:
                out.append(got)
        statics.sort(key=lambda r: -float(r.get("energy") or 0.0))
        caps = {"lava": self.lava_cap, "night": self.night_cap, "named": self.named_cap,
                "glass": self.glass_cap, "static": self.cap}
        kept = dict.fromkeys(caps, 0)
        for r in statics:
            got, why = self.static(r)
            pool = self.pool(r)
            if got is None:
                skipped[why] += 1
            elif kept[pool] >= caps[pool]:
                skipped[f"{pool} beyond the per-level cap"] += 1
            else:
                out.append(got)
                kept[pool] += 1
        for i, r in enumerate(props):
            if i >= self.prop_cap:
                skipped["prop beyond the per-level cap"] += len(props) - i
                break
            out.append(self.prop(r))
        return out, skipped


def bake_level(recs: list[dict], static_cap: int, energy_ref: float,
               cfg: dict | None = None) -> tuple[list[dict], Counter]:
    c = {**DEFAULTS, **(cfg or {}), "static_cap": static_cap, "static_energy_ref": energy_ref}
    return Baker(c).level(recs)


FIELDS = ["level", "id", "x", "y", "z", "r", "g", "b", "radius_m", "cls", "gain", "gate", "param", "aid"]

INC_HEADER = [
    "// Generated by `sandover-lights bake` from the light census ($SANDOVER_DATA/lights/*.json).",
    "// Do not edit; regenerate. See WorldLights.h and sandover/kb/howto/lights.md.",
    "// {level, x, y, z (m), r, g, b, radius (m), gain, class, gate, param, aid}",
]


def _f(v: float) -> str:
    s = f"{float(v):.3f}".rstrip("0").rstrip(".")
    if s in ("-0", ""):
        s = "0"
    return (s + ".f") if "." not in s else (s + "f")


def cpp_table(rows: list[dict]) -> str:
    lines = list(INC_HEADER)
    for r in rows:
        nums = ", ".join(_f(v) for v in [*r["pos"], *r["colour"], r["radius_m"], r["gain"]])
        lines.append(f'{{"{r["level"]}", {nums}, {r["cls"]}, {r["gate"]}, {r["param"]}u, {r["aid"]}u}},')
    return "\n".join(lines) + "\n"


def write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(FIELDS)
        for r in rows:
            w.writerow([r["level"], r["id"], *(f"{v:.3f}" for v in r["pos"]),
                        *(f"{v:.3f}" for v in r["colour"]), f"{r['radius_m']:.2f}", r["cls"],
                        f"{r['gain']:.4f}", r["gate"], r["param"], r["aid"]])


def bake_rows(lights_dir: Path, levels: list[str] | None = None,
              cfg: dict | None = None) -> tuple[list[dict], dict[str, dict]]:
    """All levels' rows (level name order, then census order) and per-level counts."""
    baker = Baker({**DEFAULTS, **(cfg if cfg is not None else settings())})
    rows, stats = [], {}
    inv = {v: k for k, v in CLASSES.items()}
    for f in sorted(lights_dir.glob("*.json")):
        lv = f.stem
        if levels and lv not in levels:
            continue
        recs = json.loads(f.read_text(encoding="utf-8"))
        if not isinstance(recs, list):
            continue
        got, skipped = baker.level(recs)
        rows += got
        stats[lv] = {"rows": len(got), "by_class": dict(Counter(inv[r["cls"]] for r in got)),
                     "skipped": dict(skipped)}
    return rows, stats


def bake(lights_dir: Path, out: Path, levels: list[str] | None = None,
         cfg: dict | None = None, csv_out: Path | None = None) -> dict[str, dict]:
    """Write the C++ table to ``out`` (and the CSV to ``csv_out``); returns per-level counts
    ``{level: {"rows", "by_class", "skipped"}}``."""
    rows, stats = bake_rows(lights_dir, levels, cfg)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(cpp_table(rows), encoding="utf-8", newline="\n")
    if csv_out is not None:
        write_csv(csv_out, rows)
    return stats
