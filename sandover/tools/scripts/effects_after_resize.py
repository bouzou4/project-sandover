"""Do AO, bloom and tile lights survive a resize, a fullscreen toggle, a render-scale change, an
HDR output toggle, an FP16 toggle and the play launcher's per-warp forms? One rig instance with the
play look (ultra AO); after each event a window frame and an AO-view frame are saved through the bug
report path (each one also rebuilds the render target, as F9 does).

``uv run --no-sync python scripts/effects_after_resize.py --bin <staged bin> --out <dir>``

Checks per probe: the AO view (blurred AO, ``pc-set-ssao-param 10``) is drawn (most pixels above
0.9; with AO off the lit frame shows instead) and has occluded texels (share below 0.8), and the
window frame matches the first frame at that window size (mean absolute difference, 0..255). The last probe runs after the vanilla switch off and on, the user's
workaround. Exit 1 when any event probe fails.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

from sandover import locks
from sandover.paths import data_dir
from sandover.rig import capture

# share of AO-view pixels below 0.8 that counts as "AO present"; dead AO reads ~0
AO_OCCLUDED_MIN = 0.02
# share of AO-view pixels above 0.9: the view is mostly unoccluded white; a lit frame (AO off,
# the view not drawn) is not
AO_WHITE_MIN = 0.5
# mean absolute difference to the baseline window frame (0..255) that counts as changed
FRAME_DIFF_MAX = 10.0  # live actors, water and clouds alone give about 7

EVENTS = {
    "fullscreen": ["(pc-set-display-mode! 'fullscreen 0 0)"],
    "windowed": ["(pc-set-display-mode! 'windowed {w} {h})"],
    "scale-59": ["(set! (-> *pc-settings* sandover render-scale) 3)"],
    "scale-back": ["(set! (-> *pc-settings* sandover render-scale) {scale})"],
    "hdr-out-off": ["(set! (-> *pc-settings* sandover hdr-output) 2)"],
    "hdr-out-back": ["(set! (-> *pc-settings* sandover hdr-output) {hdr_out})"],
    "fp16-off": ["(set! (-> *pc-settings* sandover hdr-target?) #f)"],
    "fp16-on": ["(set! (-> *pc-settings* sandover hdr-target?) #t)"],
    "resize": ["(pc-set-window-size! {w2} {h2})"],
    "resize-back": ["(pc-set-window-size! {w} {h})"],
    "vanilla-cycle": ["(set! (-> *pc-settings* sandover vanilla?) #t)", "WAIT",
                      "(set! (-> *pc-settings* sandover vanilla?) #f)"],
}
# the play launcher's per-warp apply (local play.py): raw effect forms, then the menu reapplied
PLAY_FORMS = ["(pc-set-bloom 0 1000000)", "(pc-set-bloom 4 1000000)",
              "(pc-set-emissive-lights 0 1000000)", "(pc-set-shadows 1 1.0)",
              "(pc-set-shadow-option 5 300000)", "(pc-set-fur 0 1000000)",
              "(pc-set-water 0 1000000)", "(pc-set-pbr-param 0 1)", "VISTA",
              "(pc-set-hdr-target #t)", "(pc-set-tonemap 0 2000000)",
              "(pc-set-hdr-output 1 1000000)", "(pc-set-hdr-output 0 1000000)"]
EVENTS["play-apply"] = [" ".join(PLAY_FORMS) + " (sandover-settings-reapply)"]
EVENTS["play-apply-raw"] = [" ".join(PLAY_FORMS)]
# the launcher's AO form before the tiers became the menu's: index 4 is past the four presets
EVENTS["old-launcher-ao"] = ["(pc-set-ssao-preset 4)"]
# events after which the HDR swapchain may restart (slow on some drivers)
HDR_RESTART = {"play-apply", "play-apply-raw", "fullscreen", "windowed", "hdr-out-off", "hdr-out-back", "resize", "resize-back"}
DEFAULT = ["fullscreen", "windowed", "fullscreen", "windowed", "scale-59", "scale-back",
           "hdr-out-off", "hdr-out-back", "fp16-off", "fp16-on", "resize", "resize-back",
           "play-apply", "old-launcher-ao", "fullscreen"]


def gk_count() -> int:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gk.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    return sum(1 for line in out.splitlines() if line.lower().startswith('"gk.exe"'))


def wait_gk_room(cap: int = 2) -> None:
    delay = 30.0
    while (n := gk_count()) >= cap:
        print(f"ED: {n} gk running, waiting {delay:.0f}s", flush=True)
        time.sleep(delay)
        delay = min(delay * 2, 3600.0)


class Probe:
    def __init__(self, game: capture.Game, run_dir: Path):
        self.g, self.reports = game, run_dir / "bug-reports"

    def report(self, timeout: float = 90.0) -> Path:
        """Save a bug report and return its screenshot once the zip is written (a request while
        one is being written is ignored, so the next one waits for the zip; resent once)."""
        before = {p.name for p in self.reports.glob("*")} if self.reports.exists() else set()
        self.g.send("(sandover-bug-report)")
        start = time.monotonic()
        resent = False
        while time.monotonic() - start < timeout:
            time.sleep(0.5)
            new = [d for d in self.reports.iterdir() if d.is_dir() and d.name not in before]                 if self.reports.exists() else []
            for d in new:
                if (self.reports / f"sandover-bug-{d.name}.zip").exists():
                    return d / "screenshot.png"
            if not new and not resent and time.monotonic() - start > 20:
                self.g.send("(sandover-bug-report)")
                resent = True
        raise TimeoutError("no bug report screenshot")


def lum(png: Path) -> np.ndarray:
    a = np.asarray(Image.open(png).convert("RGB"), dtype=np.float32) / 255.0
    return a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="staged binary copy under $SANDOVER_DATA/bin")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--events", nargs="*", default=DEFAULT, choices=sorted(EVENTS))
    ap.add_argument("--settle", type=float, default=4.0, help="seconds after each event")
    ap.add_argument("--continue-point", default="village1-hut")
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run_dir = out / "run"
    os.environ["SANDOVER_RUN_DIR"] = str(run_dir)
    bin_dir = data_dir() / "bin" / a.bin
    wait_gk_room()
    results: list[dict] = []
    with locks.slot(purpose=f"effects after resize, bin {bin_dir.name}") as s:
        with capture.Game(s, bin_dir, write=not capture.outputs_fresh()) as g:
            shot = capture.Shot(name="ed-setup", continue_point=a.continue_point, hour=12.0,
                                resolution=(1280, 720), msaa=4, actors="freeze")
            g.shoot(shot, out / "setup.png")
            # the play look after the shot: the HDR output start can take most of a minute
            g.gk_log.skip_to_end()
            # the reporting player's AO tier (ultra)
            g.send(capture.PLAY_LOOK + " (set! (-> *pc-settings* sandover ssao-quality) 3)"
                   + ' (format 0 "ED:LOOK~%")')
            g.gk_log.wait_for(r"ED:LOOK", 60)
            try:
                g.gk_log.wait_for(r"hdr: started|hdr: .*SDR", 120)
            except TimeoutError:
                pass
            time.sleep(a.settle * 2)
            probe = Probe(g, run_dir)
            vista = subprocess.run([sys.executable, "-m", "sandover.presets", "apply", "vista", "haze"],
                                   capture_output=True, text=True).stdout
            fmt = {"vista": " ".join(l.strip() for l in vista.splitlines() if l.strip().startswith("(")),
                   "w": 1280, "h": 720, "w2": 1600, "h2": 900, "scale": 0, "hdr_out": 1}
            base: dict[tuple, tuple[np.ndarray, np.ndarray]] = {}

            def take(tag: str) -> dict:
                frame = probe.report()
                g.send("(pc-set-ssao-param 10 2000000)")
                time.sleep(1.5)
                ao_png = probe.report()
                g.send("(pc-set-ssao-param 10 0)")
                time.sleep(1.0)
                f, ao = lum(frame), lum(ao_png)
                # baselines per window size: the first probe at each size
                base.setdefault(f.shape, (f, ao))
                bf, bao = base[f.shape]
                diff = None if bf is f else float(np.abs(f - bf).mean() * 255.0)
                ao_diff = None if bao is ao else float(np.abs(ao - bao).mean() * 255.0)
                r = {"probe": tag, "frame": str(frame), "ao_view": str(ao_png),
                     "size": list(f.shape[::-1]),
                     "ao_occluded": float((ao < 0.8).mean()), "ao_std": float(ao.std()),
                     "frame_mean": float(f.mean() * 255.0), "frame_diff": diff,
                     "ao_diff": ao_diff}
                r["ao_white"] = float((ao > 0.9).mean())
                r["ao_ok"] = r["ao_occluded"] >= AO_OCCLUDED_MIN and r["ao_white"] >= AO_WHITE_MIN
                r["frame_ok"] = diff is None or diff <= FRAME_DIFF_MAX
                print("ED: " + json.dumps(r), flush=True)
                return r

            results.append(take("baseline"))
            for ev in a.events:
                g.gk_log.skip_to_end()
                for form in EVENTS[ev]:
                    if form == "WAIT":
                        time.sleep(a.settle)
                    else:
                        g.send(form.replace("VISTA", fmt["vista"]).format(**fmt))
                if ev in HDR_RESTART:
                    try:
                        g.gk_log.wait_for(r"hdr: (settled|started|stopped|off)", 120)
                    except TimeoutError:
                        print(f"ED: {ev}: no hdr start line", flush=True)
                time.sleep(a.settle)
                results.append(take(ev))
            for form in EVENTS["vanilla-cycle"]:
                time.sleep(a.settle) if form == "WAIT" else g.send(form)
            time.sleep(a.settle)
            results.append(take("after-vanilla-cycle"))
    failed = [r["probe"] for r in results[1:-1] if not (r["ao_ok"] and r["frame_ok"])]
    (out / "result.json").write_text(json.dumps({"bin": a.bin, "probes": results,
                                                 "failed": failed}, indent=1), encoding="utf-8")
    print(f"ED: {'FAIL ' + ' '.join(failed) if failed else 'PASS'}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
