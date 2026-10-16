"""Game speed vs wall clock at forced render rates, on one rig instance (issue #279).

Boots one gk + goalc of a staged binary copy (the rig's Game), warps to a continue point,
turns capture mode off (*fixed-frame-step* #f), then for each rate forces the C++ frame
limiter to it and measures game seconds (real-frame-counter / 300) against the EE timer
(300 MHz). The game's own 60 Hz clock is untouched: only the limiter target changes.

    cd sandover/tools
    uv run --no-sync python scripts/speed_test.py --bin <staged dir> [--hotload <drawable.gc>]

--hotload sends the pacing forms (*pc-frames-owed*, *pc-seconds-owed*, display-frame-start)
from the given drawable.gc after a first pass, then measures again, so one instance gives the
loaded code ("loaded") and the hot-loaded code ("hotload"). For a worktree build, set
SANDOVER_GK_PROJ_PATH / SANDOVER_DATA as in kb/howto/rig-capture.md "Worktree builds".
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

from sandover import locks
from sandover.paths import data_dir
from sandover.rig import capture

HOOK = """
(define *speed-fps* 0)
(define *speed-set-fps* (the-as (function int none) pc-set-frame-rate))
(set! pc-set-frame-rate
      (the-as (function int none)
              (lambda ((rate int)) (*speed-set-fps* (if (> *speed-fps* 0) *speed-fps* rate)) (none))))
(format 0 "SPEED:HOOKED~%")
"""


def goal_forms(path: Path, heads: tuple[str, ...]) -> list[str]:
    """Top-level forms of a GOAL file that start with one of ``heads``."""
    src = path.read_text(encoding="utf-8")
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == ";":
            i = src.find("\n", i)
            i = n if i < 0 else i
        elif src.startswith("#|", i):
            i = src.find("|#", i) + 2
        elif c == '"':
            i = _skip_string(src, i)
        elif c == "(":
            j = _form_end(src, i)
            form = src[i:j]
            if form.startswith(heads):
                out.append(form)
            i = j
        else:
            i += 1
    return out


def _skip_string(src: str, i: int) -> int:
    i += 1
    while src[i] != '"':
        i += 2 if src[i] == "\\" else 1
    return i + 1


def _form_end(src: str, i: int) -> int:
    depth = 0
    while True:
        c = src[i]
        if c == ";":
            i = src.find("\n", i)
        elif src.startswith("#|", i):
            i = src.find("|#", i) + 2
            continue
        elif c == '"':
            i = _skip_string(src, i)
            continue
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1


class Probe:
    def __init__(self, game: capture.Game):
        self.game, self.k = game, 0

    def sample(self) -> tuple[int, int, int]:
        """(real-frame-counter, real-actual-frame-counter, EE timer) read through the gk log."""
        self.k += 1
        tag = f"SPEED:S{self.k}"
        self.game.send(f'(format 0 "{tag} ~D ~D ~D~%" (-> *display* real-frame-counter) '
                       f"(-> *display* real-actual-frame-counter) (__read-ee-timer))")
        m = self.game.gk_log.wait_for(rf"{tag} (-?\d+) (-?\d+) (\d+)", 20, alive=self.game.alive)
        return int(m.group(1)), int(m.group(2)), int(m.group(3))

    def expect(self, form: str, pattern: str, timeout: float = 30) -> re.Match:
        self.game.send(form)
        return self.game.gk_log.wait_for(pattern, timeout, alive=self.game.alive)


def measure(p: Probe, rates: list[int], settle: float, window: float, label: str) -> None:
    for rate in rates:
        p.game.send(f"(set! *speed-fps* {rate}) (pc-set-frame-rate {rate})")
        time.sleep(settle)
        c0, f0, t0 = p.sample()
        time.sleep(window)
        c1, f1, t1 = p.sample()
        wall = (t1 - t0) / 3e8
        speed = ((c1 - c0) / 300.0) / wall
        print(f"SPEED {label:8s} target {rate:3d} fps: render {(f1 - f0) / wall:6.2f} fps, "
              f"game speed {speed:.4f} over {wall:.1f} s", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="staged binary dir under $SANDOVER_DATA/bin, or a path")
    ap.add_argument("--continue", dest="cont", default="village1-hut")
    ap.add_argument("--rates", default="40,60")
    ap.add_argument("--settle", type=float, default=1.0)
    ap.add_argument("--window", type=float, default=5.0)
    ap.add_argument("--vsync", action="store_true", help="keep vsync on (default off: the limiter alone paces)")
    ap.add_argument("--hotload", type=Path, help="drawable.gc whose pacing forms to send for a second pass")
    ap.add_argument("--slot", type=int)
    a = ap.parse_args()

    bin_dir = Path(a.bin) if Path(a.bin).is_dir() else data_dir() / "bin" / a.bin
    if not (bin_dir / f"gk{capture.EXE}").exists():
        print(f"no gk in {bin_dir}", file=sys.stderr)
        return 2
    rates = [int(r) for r in a.rates.split(",")]
    hot = []
    if a.hotload:
        hot = goal_forms(a.hotload, ("(define *pc-frames-owed*", "(define *pc-seconds-owed*",
                                     "(defun display-frame-start"))
        if len(hot) != 3:
            print(f"expected 3 pacing forms in {a.hotload}, found {len(hot)}", file=sys.stderr)
            return 2
    level = dict(capture.continue_points()).get(a.cont)
    if not level:
        print(f"unknown continue point {a.cont}", file=sys.stderr)
        return 2

    purpose = f"speed_test bin {bin_dir.name}"
    with locks.slot(a.slot, purpose) as slot, \
            capture.Game(slot, bin_dir, write=not capture.outputs_fresh()) as game:
        p = Probe(game)
        game.gk_log.skip_to_end()
        game.send(f"(start 'play (get-continue-by-name *game-info* \"{a.cont}\"))")
        try:
            game.gk_log.wait_for(rf"\bmaterials: {re.escape(level)} ", 8, alive=game.alive)
        except TimeoutError:
            capture.stamp(f"no 'materials: {level}' line; continuing")
        time.sleep(8)
        p.expect("(rig-mode #f)", r"RIG:MODE #f")
        m = p.expect('(format 0 "SPEED:FFS ~A~%" *fixed-frame-step*)', r"SPEED:FFS (\S+)")
        if m.group(1) != "#f":
            raise RuntimeError(f"*fixed-frame-step* is {m.group(1)}")
        if not a.vsync:
            game.send("(set! (-> *pc-settings* vsync?) #f) (pc-set-vsync #f)")
        p.expect(HOOK, r"SPEED:HOOKED")
        measure(p, rates, a.settle, a.window, "loaded")
        if hot:
            game.goalc_log.skip_to_end()
            p.expect(" ".join(hot) + ' (format 0 "SPEED:HOTLOADED~%")', r"SPEED:HOTLOADED", 60)
            measure(p, rates, a.settle, a.window, "hotload")
        game.send("(set! *speed-fps* 0) (pc-set-frame-rate 60)")
        time.sleep(1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
