"""GPU cost of the SSAO passes per preset, from the engine's GL_TIMESTAMP scopes.

Boots one instance, stages each spot with the rig (camera fixed, noon), switches the
render target to the requested size and MSAA, then for ``off`` and every preset waits for
two 240-frame timer windows and keeps the second. Take the numbers solo: the command
refuses to start while another gk is running.

Usage::

    sandover-ssao-cost --spots village1-hut beach-start jungle-start --json cost.json
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from sandover import engine, locks, presets
from sandover.rig.capture import Game, Shot

TIMERS = ["buckets", "ssao-total", "ssao-copy56", "ssao-linearize-mips", "ssao-ao", "ssao-blur",
          "ssao-composite"]
RX = re.compile(r"SSAO-TIMING name=(\S+) avg_ms=([\d.]+) min_ms=([\d.]+) max_ms=([\d.]+)")


def other_games_running() -> bool:
    if sys.platform != "win32":
        return False
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gk.exe"], capture_output=True, text=True).stdout
    return "gk.exe" in out


def window(game: Game, want: list[str]) -> dict[str, dict]:
    """Second timer window of every timer in ``want`` after now (the first may straddle a switch)."""
    game.gk_log.skip_to_end()
    seen: dict[str, int] = {}
    got: dict[str, dict] = {}
    while set(want) - set(got):
        m = RX.search(game.gk_log.wait_for(r"SSAO-TIMING ", 60, alive=game.alive).string)
        name = m.group(1)
        seen[name] = seen.get(name, 0) + 1
        if name in want and seen[name] == 2:
            got[name] = {"avg": float(m.group(2)), "min": float(m.group(3)), "max": float(m.group(4))}
    return got


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-ssao-cost", description=__doc__.splitlines()[0])
    ap.add_argument("--spots", nargs="+", default=["village1-hut", "beach-start", "jungle-start"])
    ap.add_argument("--resolution", nargs=2, type=int, default=[2560, 1440])
    ap.add_argument("--msaa", type=int, default=4)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    if other_games_running():
        print("another gk is running; GPU costs must be measured solo", file=sys.stderr)
        return 2
    names = ["off", *presets.presets("ssao")]
    w, h = args.resolution
    rows = []
    bin_dir = engine.staged_bin()
    with locks.slot(purpose=f"sandover-ssao-cost bin {bin_dir.name}") as s, Game(s, bin_dir) as game:
        for k, spot in enumerate(args.spots):
            game.shoot(Shot(name=f"cost-{spot}", continue_point=spot, hour=12, index=k,
                            resolution=(w, h), msaa=args.msaa),
                       Path(game.cfg) / f"cost-{spot}.png")
            game.send(f"(begin (set! (-> *pc-settings* gfx-msaa) {args.msaa}) "
                      f"(set! (-> *pc-settings* width) {w}) (set! (-> *pc-settings* height) {h}) "
                      f"(set-window-size! *pc-settings* {w} {h}) 0)")
            game.send("(pc-set-ssao-param 11 1000000)")
            for name in names:
                for form in presets.goal_forms("ssao", name):
                    game.send(form)
                want = ["buckets"] if name == "off" else TIMERS
                t = window(game, want)
                rows.append({"spot": spot, "preset": name, **{k2: t[k2] for k2 in want}})
                print(f"{spot:14s} {name:9s} " + "  ".join(f"{k2}={t[k2]['avg']:.3f}" for k2 in want),
                      flush=True)
    if args.json:
        args.json.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
