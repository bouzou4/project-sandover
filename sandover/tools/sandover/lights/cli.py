"""``sandover-lights``: offline light census per level (issue #191).

Usage::

    uv run --no-sync python -m sandover.lights census [--levels village1 jungle] [--out DIR]
        [--no-static] [--no-props] [--no-sheets]
    uv run --no-sync python -m sandover.lights bake [--levels ...] [--out FILE] [--csv FILE]
    uv run --no-sync python -m sandover.lights spots [--levels ...] [--yaml FILE]
    uv run --no-sync python -m sandover.lights inventory [--levels ...]
    uv run --no-sync python -m sandover.lights glowcheck FRAME.png --levels LEVEL [--textures ...]
    uv run --no-sync python -m sandover.lights eco [--out FILE]
    uv run --no-sync python -m sandover.lights flicker SHOT_DIR [--baseline DIR] [--control DIR] [--only NAME ...] [--no-strip]
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

from sandover import paths
from sandover.lights import census as C


def write_summary(out: Path, per_level: dict[str, list[dict]], notes: dict[str, Counter]) -> Path:
    lines = ["# Light census", "", "Counts per level and kind (`sandover-lights census`, issue #191).", "",
             "| level | " + " | ".join(C.KINDS) + " | total | state: always / entity-flag / task / time-of-day / player / transient |",
             "|---|" + "---|" * (len(C.KINDS) + 2)]
    tot = Counter()
    for lv, recs in per_level.items():
        k = Counter(r["kind"] for r in recs)
        s = Counter(r["state"]["kind"] for r in recs)
        tot.update(k)
        lines.append(f"| {lv} | " + " | ".join(str(k[x]) for x in C.KINDS) + f" | {len(recs)} | "
                     + " / ".join(str(s[x]) for x in ("always", "entity-flag", "task", "time-of-day", "player", "transient")) + " |")
    lines.append("| **all** | " + " | ".join(f"**{tot[x]}**" for x in C.KINDS) + f" | **{sum(tot.values())}** | |")
    types = Counter(r["type"] for recs in per_level.values() for r in recs if r["kind"] == "entity")
    lines += ["", "## Entity types", "", ", ".join(f"{t} {n}" for t, n in types.most_common()), ""]
    lines += ["## Not classified or not lights", ""]
    merged = Counter()
    for c in notes.values():
        merged.update(c)
    lines += [f"- {k}: {v}" for k, v in merged.most_common()] or ["- none"]
    p = out / "summary.md"
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return p


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-lights", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("census", help="write lights/<level>.json, summary.md and sheets")
    root, data = paths.root(), paths.data_dir()
    c.add_argument("--levels", nargs="*", default=None, help="default: every unbake level")
    c.add_argument("--out", type=Path, default=data / "lights")
    c.add_argument("--entities", type=Path, default=root / "decompiler_out/jak1/entities")
    c.add_argument("--goal-src", type=Path, default=root / "goal_src/jak1")
    c.add_argument("--textures", type=Path, default=root / "decompiler_out/jak1/textures")
    c.add_argument("--no-static", action="store_true")
    c.add_argument("--no-props", action="store_true", help="skip prop_lights.json props")
    c.add_argument("--no-sheets", action="store_true")
    b = sub.add_parser("bake", help="write the engine's world light table from the census")
    b.add_argument("--levels", nargs="*", default=None, help="default: every census level")
    b.add_argument("--census", type=Path, default=data / "lights")
    b.add_argument("--out", type=Path, default=None,
                   help="default: game/graphics/opengl_renderer/WorldLights.inc (committed)")
    b.add_argument("--csv", type=Path, default=None, help="also write the rows as CSV")
    sp = sub.add_parser("spots", help="rank lights by added over baked light and place shot cameras")
    sp.add_argument("--levels", nargs="*", default=None, help="default: every census level")
    sp.add_argument("--top", type=int, default=20, help="placed lights to list")
    sp.add_argument("--pick", type=int, default=3, help="shots to pick (best per kind, distinct levels)")
    sp.add_argument("--yaml", type=Path, default=None, help="write the picked shots as a manifest")
    inv = sub.add_parser("inventory", help="everything that looks like a light, lit or not")
    inv.add_argument("--levels", nargs="*", default=None, help="default: every census level")
    inv.add_argument("--census", type=Path, default=data / "lights")
    gc = sub.add_parser("glowcheck", help="texture vs surroundings brightness in a vanilla rig frame")
    gc.add_argument("frame", type=Path, help="<name>.png with its rig sidecar <name>.json")
    gc.add_argument("--levels", nargs="+", required=True, help="levels whose background is in view")
    gc.add_argument("--textures", nargs="*", default=None, help="default: every visible texture")
    fl = sub.add_parser("flicker", help="temporal stability of the lights in rig pan bursts")
    ec = sub.add_parser("eco", help="write EcoLights.inc: eco shot and blast groups and their colours")
    la = sub.add_parser("launch", help="write LaunchLights.inc: the particle groups that light their entity")
    la.add_argument("--out", type=Path, default=None,
                    help="default: game/graphics/opengl_renderer/LaunchLights.inc (committed)")
    ec.add_argument("--out", type=Path, default=None,
                    help="default: game/graphics/opengl_renderer/EcoLights.inc (committed)")
    fl.add_argument("shot_dir", type=Path, help="capture dir with <name>.json pan sidecars")
    fl.add_argument("--only", nargs="*", default=None, help="shot names")
    fl.add_argument("--gate", type=float, default=2.0, help="raw match distance (m)")
    fl.add_argument("--no-strip", action="store_true", help="skip <name>-strip.png")
    fl.add_argument("--baseline", type=Path, default=None,
                    help="the same shots captured with --preset emissive=unsmoothed")
    fl.add_argument("--control", type=Path, default=None,
                    help="the same shots with --preset emissive=off (lit-box steps without tile lights)")
    fl.add_argument("--no-boxes", action="store_true", help="skip the tracked lit-box metric")
    args = ap.parse_args(argv)
    if args.cmd == "launch":
        from sandover.lights import launch

        out, groups = launch.write(args.out)
        print(f"{len(groups)} launch light groups -> {out}")
        return 0
    if args.cmd == "eco":
        from sandover.lights import eco

        out, rows = eco.write(args.out)
        print(f"{len(rows)} eco shot and blast groups -> {out}")
        return 0
    if args.cmd == "flicker":
        from sandover.lights.flicker import run

        return run(args.shot_dir, args.only, args.gate, not args.no_strip, args.baseline,
                   args.control, not args.no_boxes)
    if args.cmd == "glowcheck":
        from sandover.lights.glowcheck import measure

        for r in measure(args.frame, args.levels, data, root, set(args.textures) if args.textures else None):
            print(json.dumps(r))
        return 0
    if args.cmd == "bake":
        return run_bake(args)
    if args.cmd == "inventory":
        from sandover.lights.inventory import run as run_inventory

        print(run_inventory(args.census, data, args.levels))
        return 0
    if args.cmd == "spots":
        from sandover.lights.spots import main_spots

        return main_spots(args)

    from sandover.lights.particles import load_groups
    from sandover.unbake.levels import LEVELS

    levels = args.levels or list(LEVELS)
    table = C.load_json("entity_types.json")
    vanilla = C.load_json("vanilla_lights.json")
    props = {} if args.no_props else C.load_json("prop_lights.json")
    tasks = C.game_tasks(args.goal_src)
    groups = load_groups(args.goal_src)
    static_ctx = None
    if not args.no_static:
        from sandover.materials.emissive_maps import index_texture_dir, maps_dir

        score = json.loads((data / "materials/emitter-score.json").read_text(encoding="utf-8"))
        emitters = {k: v for k, v in score["textures"].items() if v.get("emitter")}
        static_ctx = (emitters, float(score["params"].get("covered", 0.1)), maps_dir(), args.textures,
                      index_texture_dir(args.textures))
    args.out.mkdir(parents=True, exist_ok=True)
    from sandover.lights.sheet import footprint

    per_level: dict[str, list[dict]] = {}
    notes: dict[str, Counter] = {}
    fps = {}
    for lv in levels:
        fps[lv] = footprint(data / "unbake/riproot/decompiler_out/jak1/levels" / lv / f"{lv}-background.glb")
        actors = C.load_actors(args.entities, lv)
        src = f"decompiler_out/jak1/entities/{lv}-actors.json"
        ent, n1 = C.entity_lights(lv, actors, table, tasks, src)
        part, n2 = C.particle_lights(lv, actors, groups, src)
        stat = C.static_lights(lv, data, *static_ctx) if static_ctx else []
        prop = C.prop_lights(lv, data, props, args.textures) if props else []
        recs = ent + part + stat + C.vanilla_lights(lv, vanilla, fps[lv]) + prop
        per_level[lv] = recs
        notes[lv] = n1 + n2
        (args.out / f"{lv}.json").write_text(json.dumps(recs, indent=1), encoding="utf-8")
        print(f"{lv}: {len(ent)} entity, {len(part)} particle, {len(stat)} static, "
              f"{len(recs) - len(ent) - len(part) - len(stat) - len(prop)} vanilla, {len(prop)} prop")
    print(write_summary(args.out, per_level, notes))
    if not args.no_sheets:
        from sandover.lights.sheet import write_sheets

        for p in write_sheets(args.out / "sheets", per_level, fps):
            print(f"  sheet {p}")
    return 0


def run_bake(args) -> int:
    from sandover.lights.bake import bake, cpp_table_path

    out = args.out or cpp_table_path()
    stats = bake(args.census, out, args.levels, csv_out=args.csv)
    total = 0
    for lv, s in stats.items():
        total += s["rows"]
        cls = ", ".join(f"{k} {v}" for k, v in sorted(s["by_class"].items()))
        print(f"{lv}: {s['rows']} lights ({cls})")
    skipped = Counter()
    for s in stats.values():
        skipped.update(s["skipped"])
    for k, v in skipped.most_common():
        print(f"  skipped {k}: {v}")
    print(f"{total} lights -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
