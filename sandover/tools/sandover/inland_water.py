"""Generate the inland water table (``InlandWater.inc``) and the list of ``water-anim`` surfaces
from game data.

Nothing here is typed by hand: which surfaces are clear water, which are mud, and which are left
alone (dark eco, lava) all come from the decompiled game.

- **Which entities.** Every entity whose type is ``water-anim`` or a subclass of it
  (``goal_src/jak1`` ``deftype`` forms), with its ``look`` lump, from
  ``decompiler_out/jak1/entities/<level>-actors.json``.
- **Per look** (``water-anim.gc``): the skeleton group, its art group and lod0 merc model.
- **Per art** (``decompiler_out/jak1/raw_obj/<art group>-ag.go``): the ``texture-bucket`` res
  (tfrag, pris or water sink, so the merc or generic bucket) and whether the effect has an
  envmap; the textures and their alpha mode from the rip (``*.glb`` of the model).
- **Per type**: the draw path. A type whose ``water-vol-method-22`` allocates a
  ``ripple-merc-query`` draws through Generic2 (``bones.gc`` sends a draw with a pending query to
  mercneric); every other ripple surface draws through Merc2 with the game's vertex update. The
  game's tint is the type's ``color-mult`` (``set-vector!`` in the same method, per look where
  the method has a ``case``).
- **Kind.** ``dark-eco`` when the type is ``dark-eco-pool`` or a subclass, or the base texture is
  a ``*darkecowater``; ``mud`` when the type sets ``(water-flag mud)``; ``clear`` when the base
  texture is alpha blended and the effect has an envmap (translucent water); ``other`` (lava)
  for the rest. Only clear and mud go in the engine table.
- **Textures** (``decompiler_out/jak1/import/textures.gc``, ``tpages.gc``): combo ids of the clear
  and mud base textures on the tpages of the levels that place them, and whether that level has
  a sky (``level-info.gc`` ``:sky``).

Usage::

    uv run --no-sync python -m sandover.inland_water [--check] [--json FILE] [--report]

Writes ``game/graphics/opengl_renderer/InlandWater.inc`` in this checkout (``--check`` only
compares). Decompiler output and the rip are read from the primary checkout when this one has
none.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from sandover import paths
from sandover.lights import goal_object as GO

M = 4096.0
INC_REL = "game/graphics/opengl_renderer/InlandWater.inc"
BASE_TYPE = "water-anim"
POOL_TYPE = "dark-eco-pool"
# texture-bucket res -> foreground sink (process-drawable.gc initialize-skeleton: default 1, 4 -> 2)
SINKS = {0: "tfrag", 1: "pris", 2: "water"}
BUCKETS = {
    ("merc2", "tfrag"): "MERC_TFRAG_TEX_LEVEL0/1",
    ("merc2", "pris"): "MERC_PRIS_LEVEL0/1",
    ("merc2", "water"): "MERC_WATER_LEVEL0/1",
    ("generic2", "tfrag"): "GENERIC_TFRAG_TEX_LEVEL0/1",
    ("generic2", "pris"): "GENERIC_PRIS_LEVEL0/1",
    ("generic2", "water"): "GENERIC_WATER_LEVEL0/1",
}
KIND_CODE = {"clear": 1, "mud": 2}


def checkout() -> Path:
    """This tool's own checkout (the worktree when run from one)."""
    return Path(__file__).resolve().parents[3]


def game_dir() -> Path:
    """``decompiler_out/jak1`` of this checkout, else of the primary checkout."""
    for base in (paths.root(), paths.data_dir().parents[1]):
        d = base / "decompiler_out" / "jak1"
        if (d / "entities").is_dir():
            return d
    raise FileNotFoundError("no decompiler_out/jak1 in this checkout or the primary one")


# ---- goal_src parsers ----

def top_forms(text: str) -> list[str]:
    """Top-level forms of a GOAL source file (each starts with ``(`` in column 0)."""
    starts = [m.start() for m in re.finditer(r"^\(", text, re.M)]
    return [text[a:b] for a, b in zip(starts, starts[1:] + [len(text)])]


def parse_look_table(water_anim: str) -> list[str]:
    """Skeleton group of each look, in ``*water-anim-look*`` order (the look index)."""
    form = next(f for f in top_forms(water_anim) if f.startswith("(define *water-anim-look*"))
    return re.findall(r":skel-group\s+'(\*[\w-]+\*)", form)


@dataclass
class SkelGroup:
    art_group: str
    model: str  # merc-ctrl name (the lod0 mesh)


def parse_skelgroups(water_anim: str) -> dict[str, SkelGroup]:
    out = {}
    for f in top_forms(water_anim):
        m = re.match(r"\(defskelgroup\s+(\*[\w-]+\*)\s+([\w-]+)\s+[\w-]+\s+\S+\s+\(\(([\w-]+)-mg", f)
        if m:
            out[m.group(1)] = SkelGroup(m.group(2), m.group(3))
    return out


def subtypes(goal_files: dict[str, str], base: str) -> set[str]:
    """``base`` and every type deriving from it, transitively."""
    parent = {}
    for text in goal_files.values():
        for name, par in re.findall(r"^\(deftype ([\w-]+) \(([\w-]+)\)", text, re.M):
            parent[name] = par
    out = set()
    for name in parent:
        t = name
        while t in parent and t != base:
            t = parent[t]
        if t == base:
            out.add(name)
    return out | {base}, parent


@dataclass
class TypeInfo:
    query: bool  # allocates a ripple-merc-query: drawn through Generic2
    mud: bool  # sets (water-flag mud)
    tint: dict  # look -> color-mult (r g b a); key None = every look


def parse_type_info(goal_files: dict[str, str], parent: dict[str, str], name: str) -> TypeInfo:
    """``water-vol-method-22`` of ``name`` (falling back to its parents): query, mud flag, tint."""
    t = name
    while t:
        for text in goal_files.values():
            for f in top_forms(text):
                if not f.startswith(f"(defmethod water-vol-method-22 ((this {t}))"):
                    continue
                tint = {}
                for keys, vec in re.findall(
                        r"\(\(([\d\s]+)\)\s+\(set-vector! \(-> this draw color-mult\)\s+([-\d.\s]+)\)\)", f):
                    for k in keys.split():
                        tint[int(k)] = tuple(float(v) for v in vec.split())
                vec = r"\s+([-\d.\s]+)\)"
                # sunken-water sets its safe tint in water-vol-method-25 of the same file
                m = (re.search(r"\(set-vector! \(-> this draw color-mult\)" + vec, f)
                     or re.search(r"\(set-vector! \(-> this safe-color-mult\)" + vec, text))
                if not tint and m:
                    tint[None] = tuple(float(v) for v in m.group(1).split())
                return TypeInfo("ripple-merc-query" in f, "(water-flag mud)" in f, tint)
        t = parent.get(t)
    return TypeInfo(False, False, {})


def parse_level_sky(level_info: str) -> dict[str, bool]:
    out = {}
    for block in re.split(r"\(new 'static\s+'level-load-info", level_info)[1:]:
        n = re.search(r":name '([\w-]+)", block)
        s = re.search(r":sky (#t|#f)", block)
        if n and s:
            out[n.group(1)] = s.group(1) == "#t"
    return out


def parse_tex_ids(textures_gc: str, tpages_gc: str) -> dict[tuple[str, str], int]:
    """``(texture, tpage) -> combo id``."""
    pages = {n: int(v) for n, v in re.findall(r"\(defconstant ([\w-]+) (\d+)\)", tpages_gc)}
    out = {}
    for tex, page, idx in re.findall(r"\(def-tex ([\w-]+) ([\w-]+) (\d+)\)", textures_gc):
        if page in pages:
            out[(tex, page)] = (pages[page] << 16) | int(idx)
    return out


def glb_materials(path: Path) -> list[tuple[str, str]]:
    """``(image name, alphaMode)`` of each material with a base colour texture."""
    d = path.read_bytes()
    j = json.loads(d[20:20 + struct.unpack_from("<I", d, 12)[0]])
    images = [i.get("name") or i.get("uri") for i in j.get("images", [])]
    textures = j.get("textures", [])
    out = []
    for m in j.get("materials", []):
        bc = m.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if bc is None:
            continue
        src = textures[bc["index"]].get("source")
        if src is not None:
            out.append((images[src], m.get("alphaMode", "OPAQUE")))
    return out


# ---- assembly ----

@dataclass
class Sources:
    entities: Path
    raw_obj: Path
    import_dir: Path
    rip: Path
    goal_src: Path


def default_sources() -> Sources:
    g = game_dir()
    return Sources(entities=g / "entities", raw_obj=g / "raw_obj", import_dir=g / "import",
                   rip=paths.data_dir() / "unbake" / "riproot" / "decompiler_out" / "jak1" / "levels",
                   goal_src=checkout() / "goal_src" / "jak1")


def read_goal(src: Sources) -> dict[str, str]:
    return {str(p.relative_to(src.goal_src)).replace("\\", "/"): p.read_text(encoding="utf-8")
            for p in src.goal_src.rglob("*.gc")}


def base_texture(mats: list[tuple[str, str]]) -> tuple[str, str]:
    """The surface's own texture: the first material that is not the envmap strength image."""
    for name, mode in mats:
        if not name.endswith("-envmap-strength") and not name.startswith(("environment-", "pal-environment")):
            return name, mode
    return (mats[0] if mats else ("", "OPAQUE"))


def build(src: Sources) -> dict:
    goal = read_goal(src)
    water_anim = goal["engine/common-obs/water-anim.gc"]
    looks = parse_look_table(water_anim)
    groups = parse_skelgroups(water_anim)
    types, parent = subtypes(goal, BASE_TYPE)
    pools, _ = subtypes(goal, POOL_TYPE)
    sky = parse_level_sky(goal["engine/level/level-info.gc"])
    tex_ids = parse_tex_ids((src.import_dir / "textures.gc").read_text(encoding="utf-8"),
                            (src.import_dir / "tpages.gc").read_text(encoding="utf-8"))
    arts: dict[str, dict] = {}

    def art(group: SkelGroup) -> dict:
        if group.model not in arts:
            obj = GO.load(src.raw_obj / f"{group.art_group}-ag.go")
            geo = next(t for t in obj.objects("art-joint-geo") if GO.art_name(obj, t) == group.model)
            merc = next(t for t in obj.objects("merc-ctrl") if GO.art_name(obj, t) == group.model)
            fx = GO.merc_effects(obj, merc)
            bucket = int(GO.res_tags(obj, geo).get("texture-bucket", 1))
            mats = []
            for glb in sorted(src.rip.glob(f"*/{group.model}.glb")):
                mats += glb_materials(glb)
            tex, mode = base_texture(mats)
            arts[group.model] = {"sink": SINKS.get(2 if bucket == 4 else bucket, "pris"),
                                 "envmap": any(e.envmap for e in fx), "texture": tex, "alpha": mode}
        return arts[group.model]

    surfaces = []
    tinfo: dict[str, TypeInfo] = {}
    for f in sorted(src.entities.glob("*-actors.json")):
        level = f.name[:-len("-actors.json")]
        for a in json.loads(f.read_text(encoding="utf-8")) or []:
            et = a["etype"]
            if et not in types or "look" not in a["lump"]:
                continue
            lump = a["lump"]
            look = int(lump["look"])
            group = groups[looks[look]]
            info = art(group)
            ti = tinfo.setdefault(et, parse_type_info(goal, parent, et))
            if et in pools or "darkecowater" in info["texture"]:
                kind = "dark-eco"
            elif ti.mud:
                kind = "mud"
            elif info["alpha"] == "BLEND" and info["envmap"]:
                kind = "clear"
            else:
                kind = "other"
            path = "generic2" if ti.query else "merc2"
            wh = lump.get("water-height", [])
            tint = ti.tint.get(look, ti.tint.get(None))
            surfaces.append({
                "level": level, "name": lump.get("name"), "etype": et, "look": look,
                "model": group.model, "kind": kind, "renderer": path, "sink": info["sink"],
                "bucket": BUCKETS[(path, info["sink"])], "texture": info["texture"],
                "alpha": info["alpha"], "envmap": info["envmap"],
                "color_mult": list(tint) if tint else None,
                "surface_m": round(float(wh[0]) / M, 2) if wh else None,
                "sky": bool(sky.get(level, False)),
            })
    combos = {}
    for s in surfaces:
        if s["kind"] not in KIND_CODE:
            continue
        for (tex, page), cid in tex_ids.items():
            if tex == s["texture"] and page.startswith(s["level"] + "-vis-"):
                row = combos.setdefault(cid, {"combo": cid, "texture": tex, "tpage": page,
                                              "kind": s["kind"], "sky": s["sky"],
                                              "generic": s["renderer"] == "generic2"})
                if row["kind"] != s["kind"] or row["sky"] != s["sky"]:
                    raise ValueError(f"texture {tex} {page} is used by two kinds of surface")
    return {"surfaces": surfaces, "textures": sorted(combos.values(), key=lambda r: r["combo"])}


INC_HEADER = [
    "// Generated by `python -m sandover.inland_water` from the decompiled game (entities,",
    "// water-anim.gc, the water art, the rip). Do not edit; regenerate. See InlandWater.h.",
]


def cpp_table(t: dict) -> str:
    lines = list(INC_HEADER)
    for c in t["textures"]:
        lines.append(f'{{0x{c["combo"]:08x}u, {KIND_CODE[c["kind"]]}, {"true" if c["sky"] else "false"}}},'
                     f'  // {c["texture"]} {c["tpage"]}{" (generic2)" if c["generic"] else ""}')
    return "\n".join(lines) + "\n"


def report(t: dict) -> str:
    rows = ["| level | entity | type | look | kind | renderer | bucket | texture | color-mult | surface y (m) |",
            "|---|---|---|---|---|---|---|---|---|---|"]
    for s in t["surfaces"]:
        cm = " ".join(f"{v:g}" for v in s["color_mult"]) if s["color_mult"] else "1 1 1 1 (default)"
        rows.append(f"| {s['level']} | {s['name']} | {s['etype']} | {s['look']} | {s['kind']} | {s['renderer']} "
                    f"| {s['bucket']} | {s['texture']} | {cm} | {s['surface_m']} |")
    return "\n".join(rows) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover.inland_water", description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=checkout() / INC_REL)
    ap.add_argument("--json", type=Path, default=None, help="also write the table as JSON")
    ap.add_argument("--report", action="store_true", help="print the surface list as a Markdown table")
    ap.add_argument("--check", action="store_true", help="compare with --out, write nothing")
    a = ap.parse_args(argv)
    t = build(default_sources())
    text = cpp_table(t)
    if a.json:
        a.json.write_text(json.dumps(t, indent=1) + "\n", encoding="utf-8")
    if a.report:
        print(report(t))
    if a.check:
        same = a.out.exists() and a.out.read_text(encoding="utf-8") == text
        print(f"{a.out}: {'up to date' if same else 'STALE'}")
        return 0 if same else 1
    a.out.write_text(text, encoding="utf-8", newline="\n")
    kinds = {}
    for s in t["surfaces"]:
        kinds[s["kind"]] = kinds.get(s["kind"], 0) + 1
    print(f"{len(t['surfaces'])} water-anim surfaces {kinds}, {len(t['textures'])} texture combos -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
