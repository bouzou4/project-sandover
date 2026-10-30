"""Moving-sun experiments: capture variants with the rig, FLIP them, build captioned sheets.

    uv run python -m sandover.unbake.experiments run [--only EXP...] [--parallel N] [--bin NAME]
    uv run python -m sandover.unbake.experiments flip
    uv run python -m sandover.unbake.experiments sheets

Captures land in ``$SANDOVER_DATA/experiments/sun-experiments/<experiment>/<variant>/``; sheets and
``flip.json`` next to them. The sun controller is set once per game instance with
``pc-set-sun-option`` (see ``goal_src/jak1/kernel-defs.gc``).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from sandover.paths import data_dir, root

M = 1_000_000


def sun(enable=1, mode=1, offset=0.0, night=0, view=0) -> list[str]:
    opts = {0: enable, 1: mode, 2: offset, 3: night, 5: view}
    return [f"(pc-set-sun-option {k} {int(round(v * M))})" for k, v in opts.items()]


@dataclass
class Variant:
    name: str
    caption: str
    goal: list[str]
    only: list[str] = field(default_factory=list)


@dataclass
class Experiment:
    name: str
    manifest: str
    variants: list[Variant]
    reference: str = "vanilla"


NOON = ["village1-hut-h12", "beach-start-h12"]
EXPERIMENTS = [
    Experiment("day", "sun-day.yaml", [
        Variant("vanilla", "Vanilla: the game cross-fades two baked sun slots", sun(enable=0)),
        Variant("arc", "Moving sun: one sun follows the fitted arc with the hour", sun(mode=1)),
        Variant("identity", "Sidecar on, sun = fitted slots (must equal vanilla)", sun(mode=0)),
    ]),
    Experiment("noon-offset", "sun-day.yaml", [
        Variant("vanilla", "Vanilla noon", sun(enable=0), NOON),
        Variant("m30", "Sun 30 deg earlier along the arc", sun(offset=-30), NOON),
        Variant("m15", "Sun 15 deg earlier along the arc", sun(offset=-15), NOON),
        Variant("p15", "Sun 15 deg later along the arc", sun(offset=15), NOON),
        Variant("p30", "Sun 30 deg later along the arc", sun(offset=30), NOON),
    ]),
    Experiment("night", "sun-night.yaml", [
        Variant("fitted", "Fitted moon: the bake as it is (vanilla)", sun(enable=0)),
        Variant("table", "Table moon: background relit toward the mood table's moon", sun(mode=0, night=1)),
        Variant("ambient", "Ambient only: the moon's direct light removed", sun(mode=0, night=2)),
    ], reference="fitted"),
    Experiment("mapping", "sun-day.yaml", [
        Variant("vanilla", "Vanilla noon", sun(enable=0), NOON),
        Variant("model", "Debug: fitted model in place of the bake (mapping check)", sun(mode=0, view=1), NOON),
        Variant("normal", "Debug: sidecar normals", sun(mode=0, view=3), NOON),
    ]),
]


def out_root() -> Path:
    return data_dir() / "experiments" / "sun-experiments"


def cmd_run(args) -> int:
    rc = 0
    for exp in EXPERIMENTS:
        if args.only and exp.name not in args.only:
            continue
        for v in exp.variants:
            if args.variant and v.name not in args.variant:
                continue
            dst = out_root() / exp.name / v.name
            cmd = ["sandover-capture", "run", str(root() / "sandover/shots" / exp.manifest),
                   "--out-dir", str(dst), "--parallel", str(args.parallel)]
            if v.only:
                cmd += ["--only", *v.only]
            for g in v.goal:
                cmd += ["--goal", g]
            if args.bin:
                cmd += ["--bin", args.bin]
            print(f"== {exp.name}/{v.name}", flush=True)
            rc |= subprocess.run(cmd).returncode
    return rc


def _shots(d: Path) -> list[str]:
    return sorted(p.stem for p in d.glob("*.png"))


def cmd_flip(args) -> int:
    from sandover.parity.metrics import flip, load

    report = {}
    for exp in EXPERIMENTS:
        ref = out_root() / exp.name / exp.reference
        if not ref.exists():
            continue
        for v in exp.variants:
            if v.name == exp.reference:
                continue
            d = out_root() / exp.name / v.name
            for s in _shots(d):
                if (ref / f"{s}.png").exists():
                    report.setdefault(f"{exp.name}/{v.name}", {})[s] = flip(load(ref / f"{s}.png"), load(d / f"{s}.png"))[0]
    for k, per in report.items():
        vals = list(per.values())
        print(f"{k:28s} mean {sum(vals) / len(vals):.5f} max {max(vals):.5f} ({len(vals)} shots)")
    (out_root() / "flip.json").write_text(json.dumps(report, indent=1))
    return 0


def _font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def sheet(rows: list[str], cols: list[tuple[str, Path]], out: Path, cell_w=720, crop=None) -> None:
    """One row per shot, one column per variant, the variant caption above each column."""
    cell_h = cell_w * 9 // 16
    head, left, pad = 64, 170, 6
    img = Image.new("RGB", (left + len(cols) * (cell_w + pad), head + len(rows) * (cell_h + pad)), (32, 32, 32))
    dr = ImageDraw.Draw(img)
    for j, (cap, _) in enumerate(cols):
        x = left + j * (cell_w + pad)
        words, line, lines = cap.split(), "", []
        for w in words:
            if dr.textlength(f"{line} {w}".strip(), font=_font(18)) > cell_w - 8:
                lines.append(line)
                line = w
            else:
                line = f"{line} {w}".strip()
        lines.append(line)
        for i, ln in enumerate(lines[:3]):
            dr.text((x + 4, 4 + 20 * i), ln, fill=(235, 235, 235), font=_font(18))
    for i, r in enumerate(rows):
        y = head + i * (cell_h + pad)
        dr.text((6, y + cell_h // 2 - 10), r, fill=(235, 235, 235), font=_font(16))
        for j, (_, d) in enumerate(cols):
            p = d / f"{r}.png"
            if not p.exists():
                continue
            im = Image.open(p).convert("RGB")
            if crop:
                W, H = im.size
                im = im.crop((int(crop[0] * W), int(crop[1] * H), int((crop[0] + crop[2]) * W),
                              int((crop[1] + crop[3]) * H)))
            img.paste(im.resize((cell_w, cell_h), Image.LANCZOS), (left + j * (cell_w + pad), y))
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(out)


def cmd_sheets(args) -> int:
    for exp in EXPERIMENTS:
        base = out_root() / exp.name
        cols = [(v.caption, base / v.name) for v in exp.variants if (base / v.name).exists()]
        if not cols:
            continue
        shots = _shots(cols[0][1])
        levels = sorted({s.rsplit("-", 1)[0] for s in shots})
        for lvl in levels:
            rows = [s for s in shots if s.rsplit("-", 1)[0] == lvl]
            sheet(rows, cols, out_root() / "sheets" / f"{exp.name}-{lvl}.png")
            if args.crop:
                sheet(rows, cols, out_root() / "sheets" / f"{exp.name}-{lvl}-crop.png",
                      crop=[float(c) for c in args.crop.split(",")])
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--only", nargs="+", help="experiment names")
    r.add_argument("--variant", nargs="+", help="variant names")
    r.add_argument("--parallel", type=int, default=2)
    r.add_argument("--bin", help="staged binary copy name (default: newest staged)")
    sub.add_parser("flip")
    s = sub.add_parser("sheets")
    s.add_argument("--crop", help="also write sheets of a normalized x,y,w,h inset")
    a = ap.parse_args(argv)
    return {"run": cmd_run, "flip": cmd_flip, "sheets": cmd_sheets}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
