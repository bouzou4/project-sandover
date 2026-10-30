"""Does the game clock follow the wall clock in an XR session? One rig instance, frame rate 120.

``uv run --no-sync python scripts/xr_game_rate.py --bin <staged bin> --out <dir> [--patch]``

The XR session runs with the fake head pose (kb/howto/vr-synthetic.md: SteamVR on). Each window
marks the game clock (``real-frame-counter``, 300 per second), the drawn frames, the game steps
(``real-integral-frame-counter``: a lag frame steps 2) and the wall clock (``__read-ee-timer``),
waits ``--seconds`` and marks again. Windows: flat screen, then XR, each with the fixed frame step
off (play) and on (the rig). ``--patch`` then sends this checkout's ``pc-game-fps`` and
``set-time-ratios`` over the REPL (the fix's GOAL side, compiled into the running game) and
measures XR again. Writes ``rate.json``; exit 1 when a patched XR window is off by more than 5 %.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path

from sandover import locks
from sandover.paths import data_dir
from sandover.rig import capture

REPO = Path(__file__).resolve().parents[3]
MARK = ('(defun xrrate-mark ((tag string)) (format 0 "XRRATE ~A ~D ~D ~D ~D~%" tag '
        "(__read-ee-timer) (-> *display* real-frame-counter) "
        "(-> *display* real-actual-frame-counter) (-> *display* real-integral-frame-counter)) (none))")
LINE = re.compile(r"XRRATE (\S+) (\d+) (\d+) (\d+) (\d+)")
XR_ON = ["(define-extern pc-set-xr-fake (function symbol none))",
         "(define-extern pc-set-xr-fake-option (function int int none))",
         *[f"(pc-set-xr-fake-option {i} 0)" for i in range(6)],
         "(pc-set-xr #t)", "(pc-set-xr-fake #t)"]
COMPILE_CHECK = ["goal_src/jak1/pc/pckernel-h.gc", "goal_src/jak1/engine/gfx/hw/display.gc",
                 "goal_src/jak1/engine/ps2/pad.gc", "goal_src/jak1/levels/swamp/swamp-obs.gc"]


def gk_count() -> int:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gk.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    return sum(1 for line in out.splitlines() if line.lower().startswith('"gk.exe"'))


def wait_gk_room(cap: int = 2) -> None:
    delay = 30.0
    while (n := gk_count()) >= cap:
        print(f"XRRATE: {n} gk running, waiting {delay:.0f}s", flush=True)
        time.sleep(delay)
        delay = min(delay * 2, 3600.0)


def top_form(text: str, head: str) -> str:
    """The form of ``text`` (comments removed) that starts with ``head``, by paren matching."""
    text = re.sub(r";[^\n]*", "", text)
    i = text.index(head)
    depth = 0
    for j in range(i, len(text)):
        depth += {"(": 1, ")": -1}.get(text[j], 0)
        if depth == 0:
            return text[i:j + 1]
    raise ValueError(head)


def patch_forms() -> list[str]:
    h = (REPO / "goal_src" / "jak1" / "pc" / "pckernel-h.gc").read_text(encoding="utf-8")
    d = (REPO / "goal_src" / "jak1" / "engine" / "gfx" / "hw" / "display.gc").read_text(encoding="utf-8")
    return ["(define-extern pc-get-xr-game-rate (function int))",
            top_form(h, "(defmacro pc-game-fps ()"),
            top_form(d, "(defmethod set-time-ratios ((this display) (slowdown float))")]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="staged binary copy under $SANDOVER_DATA/bin")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--fps", type=int, default=120)
    ap.add_argument("--patch", action="store_true")
    ap.add_argument("--no-xr", dest="xr", action="store_false", help="flat windows only (no headset)")
    ap.add_argument("--continue-point", default="village1-hut")
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    os.environ["SANDOVER_RUN_DIR"] = str(out / "run")
    bin_dir = data_dir() / "bin" / a.bin
    wait_gk_room()
    rows: list[dict] = []
    compile_errors: list[str] = []
    with locks.slot(purpose=f"xr game rate, bin {bin_dir.name}") as s:
        with capture.Game(s, bin_dir, write=not capture.outputs_fresh()) as g:
            shot = capture.Shot(name="xr-rate-setup", continue_point=a.continue_point, hour=12.0,
                                resolution=(1280, 720), msaa=2, actors="live")
            g.shoot(shot, out / "setup.png")
            g.gk_log.skip_to_end()
            g.send("(rig-mode #f)")
            g.send(MARK)
            g.send(f"(set-frame-rate! *pc-settings* {a.fps} #t)")
            time.sleep(2.0)

            def window(tag: str, fixed: bool) -> dict:
                g.send(f"(set! *fixed-frame-step* {'#t' if fixed else '#f'})")
                time.sleep(1.0)
                g.gk_log.skip_to_end()
                g.send(f'(xrrate-mark "{tag}-a")')
                m0 = g.gk_log.wait_for(rf'XRRATE "?{tag}-a"? ', 20)
                time.sleep(a.seconds)
                g.send(f'(xrrate-mark "{tag}-b")')
                m1 = g.gk_log.wait_for(rf'XRRATE "?{tag}-b"? ', 20)
                v0 = [int(x) for x in LINE.search(m0.string).groups()[1:]]
                v1 = [int(x) for x in LINE.search(m1.string).groups()[1:]]
                wall = (v1[0] - v0[0]) / 3e8  # __read-ee-timer: 300 MHz
                game = (v1[1] - v0[1]) / 300.0
                frames, steps = v1[2] - v0[2], v1[3] - v0[3]
                r = {"window": tag, "fixed_step": fixed, "wall_s": round(wall, 3), "game_s": round(game, 3),
                     "game_per_wall": round(game / wall, 3), "frames": frames, "steps": steps,
                     "steps_per_frame": round(steps / max(frames, 1), 3)}
                print("XRRATE:", json.dumps(r), flush=True)
                rows.append(r)
                return r

            def patch() -> None:
                g.goalc_log.skip_to_end()
                for f in patch_forms():
                    g.send(f)
                # the changed files compile against this goalc's types (nothing written or loaded)
                for f in COMPILE_CHECK:
                    g.send(f'(asm-file "{(REPO / f).as_posix()}")')
                g.send('(format 0 "XRRATE:PATCHED~%")')
                g.gk_log.wait_for(r"XRRATE:PATCHED", 120)
                time.sleep(2.0)
                errs = [line for line in g.goalc_log._lines() if re.search(r"(?i)compilation error|\berror\b", line)]
                compile_errors.extend(errs)

            window("flat", False)
            window("flat-fixed", True)
            if a.xr:
                for f in XR_ON:
                    g.send(f)
                g.gk_log.wait_for(r"\[xr\] state \S+ -> RUNNING", 90)
                time.sleep(5.0)
                window("xr", False)
                window("xr-fixed", True)
            if a.patch:
                patch()
                if a.xr:
                    window("xr-patched", False)
                    window("xr-patched-fixed", True)
                    g.send("(pc-set-xr #f)")
                    time.sleep(3.0)
                window("flat-patched", False)
                window("flat-patched-fixed", True)
            if a.xr:
                g.send("(pc-set-xr #f)")
                time.sleep(1.0)
    (out / "rate.json").write_text(json.dumps({"windows": rows, "compile_errors": compile_errors}, indent=1),
                                   encoding="utf-8")
    bad = [r for r in rows if r["window"].startswith("xr-patched") and abs(r["game_per_wall"] - 1.0) > 0.05]
    return 1 if bad or compile_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
