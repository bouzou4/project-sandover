"""GPU cost of the fur shells per shell count, from the engine's GPU timers.

Boots one instance, stages one manifest shot with the rig (camera fixed), switches the
render target to the requested size and MSAA, then for fur off and every shell count waits
for two 240-frame windows and keeps the second: ``FUR-TIMING`` (GPU time of the instanced
shell draws alone, summed per frame) and the ``buckets`` frame timer. The other fur parameters come from ``--preset``. Take the numbers solo: the command
refuses to start while another gk is running.

Usage::

    sandover-fur-cost sandover/shots/fur.yaml --shot daxter-close --shells 4 8 16 24 32
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from sandover import engine, locks, presets
from sandover.paths import data_dir
from sandover.rig.capture import Game, load_manifest
from sandover.rig.cost import other_games_running, window

FUR_RX = re.compile(r"FUR: (\d+) shelled draws in the last (\d+) frames")
FUR_T = re.compile(r"FUR-TIMING shells=(\d+) avg_ms=([\d.]+)")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-fur-cost", description=__doc__.splitlines()[0])
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--shot", required=True, help="shot name in the manifest")
    ap.add_argument("--shells", nargs="+", type=int, default=[4, 8, 16, 24, 32])
    ap.add_argument("--preset", default="medium", help="fur preset for the other parameters")
    ap.add_argument("--goal", action="append", default=[], help="extra GOAL form sent after staging")
    ap.add_argument("--bin", help="staged binary copy under $SANDOVER_DATA/bin")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    if other_games_running():
        print("another gk is running; GPU costs must be measured solo", file=sys.stderr)
        return 2
    _, shots = load_manifest(args.manifest)
    shot = next((s for s in shots if s.name == args.shot), None)
    if shot is None:
        ap.error(f"no shot {args.shot!r} in {args.manifest}")
    w, h = shot.resolution
    bin_dir = data_dir() / "bin" / args.bin if args.bin else engine.staged_bin()
    rows = []
    with locks.slot(purpose=f"sandover-fur-cost bin {bin_dir.name}") as s, Game(s, bin_dir) as game:
        game.shoot(shot, Path(game.cfg) / f"cost-{shot.name}.png")
        game.send(f"(begin (set! (-> *pc-settings* gfx-msaa) {shot.msaa}) "
                  f"(set! (-> *pc-settings* width) {w}) (set! (-> *pc-settings* height) {h}) "
                  f"(set-window-size! *pc-settings* {w} {h}) 0)")
        for form in args.goal:
            game.send(form)
        game.send("(pc-set-ssao-param 11 1000000)")
        for n in [0, *args.shells]:
            if n == 0:
                forms = presets.goal_forms("fur", "off")
            else:
                forms = presets.goal_forms("fur", args.preset) + [f"(pc-set-fur 1 {n * 1_000_000})"]
            for form in forms:
                game.send(form)
            t = window(game, ["buckets"])["buckets"]
            draws = shells_ms = None
            if n:
                game.gk_log.skip_to_end()
                for _ in range(2):
                    mt = FUR_T.search(game.gk_log.wait_for(r"FUR-TIMING ", 60, alive=game.alive).string)
                shells_ms = float(mt.group(2))
                m = FUR_RX.search(game.gk_log.wait_for(r"FUR: ", 30, alive=game.alive).string)
                draws = round(int(m.group(1)) / max(int(m.group(2)), 1), 2)
            rows.append({"shot": shot.name, "shells": n, "buckets_ms": t, "fur_ms": shells_ms,
                         "fur_draws_per_frame": draws})
            print(f"{shot.name:14s} shells={n:3d} buckets avg={t['avg']:.3f} min={t['min']:.3f} "
                  f"max={t['max']:.3f} fur shells ms={shells_ms} fur draws/frame={draws}", flush=True)
    if args.json:
        args.json.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
