"""Play-session snapshots: the play session's F9 sidecars, and re-shooting one with the rig.

gk (with ``SANDOVER_RUN_DIR`` set) saves ``<run dir>/NNNN.png`` on F9; ``snap.gc`` prints a
``SNAP:STATE`` line with the game state for it, and :class:`Watcher` turns that line into
``<run dir>/NNNN.json`` (schema: ``sandover/kb/howto/critic-run.md``). F10/F8 print
``SNAP:WARP 1``/``SNAP:WARP -1``, which the play script maps to its next/previous level.
"""

from __future__ import annotations

import json
import re
import struct
import threading
import time
from collections.abc import Callable
from datetime import UTC, datetime
from pathlib import Path

from sandover.paths import data_dir

HERE = Path(__file__).resolve().parent
SNAP_GC = HERE / "snap.gc"
DEGREES = 65536.0 / 360.0  # GOAL `degrees` units per degree
STATE_RX = re.compile(r'SNAP:STATE (\d+) "?([^"\s]+)"? "?([^"\s]+)"?((?: [0-9A-Fa-f]+){25})')
WARP_RX = re.compile(r"SNAP:WARP (-?1)")


def new_run_dir(slug: str = "play") -> Path:
    """Create ``$SANDOVER_DATA/runs/<YYYYMMDD-HHMM>-<slug>/`` (local time) and return it."""
    d = data_dir() / "runs" / f"{datetime.now():%Y%m%d-%H%M}-{slug}"
    d.mkdir(parents=True, exist_ok=True)
    return d


def _f32(h: str) -> float:
    return struct.unpack("<f", struct.pack("<I", int(h, 16) & 0xFFFFFFFF))[0]


def parse_state(line: str) -> dict | None:
    """The game-state fields of a sidecar from one ``SNAP:STATE`` log line, or None."""
    m = STATE_RX.search(line)
    if not m:
        return None
    v = [_f32(h) for h in m.group(4).split()]
    cont = m.group(3)
    return {
        "index": int(m.group(1)),
        "level": None if m.group(2) == "none" else m.group(2),
        "continue": None if cont == "none" else cont,
        "jak_pos": v[0:3],
        "cam_pos": v[3:6],
        "cam_rows": [v[6:9], v[9:12], v[12:15]],
        "fov": v[15] / DEGREES,
        "hour": v[16],
        "mood_weights": v[17:25],
    }


def sidecar(state: dict, toggles: list[str], build_sha: str, when: datetime | None = None) -> dict:
    """The full sidecar, in schema order. ``status``/``note``/``tags``/``annotations`` belong to
    the triage tool; capture only writes their defaults."""
    when = when or datetime.now(UTC)
    return {
        "index": state["index"],
        "time_utc": when.isoformat(timespec="seconds").replace("+00:00", "Z"),
        "level": state["level"],
        "continue": state["continue"],
        "jak_pos": state["jak_pos"],
        "cam_pos": state["cam_pos"],
        "cam_rows": state["cam_rows"],
        "fov": state["fov"],
        "hour": state["hour"],
        "mood_weights": state["mood_weights"],
        "toggles": list(toggles),
        "build_sha": build_sha,
        "status": "kept",
        "note": None,
        "tags": [],
        "annotations": [],
    }


def write_sidecar(run_dir: Path, data: dict, png_timeout: float = 20.0) -> Path:
    """Wait for ``NNNN.png`` to be complete, then write ``NNNN.json`` atomically."""
    stem = f"{data['index']:04d}"
    png = run_dir / f"{stem}.png"
    deadline, size = time.monotonic() + png_timeout, -1
    while time.monotonic() < deadline:
        if png.exists() and png.stat().st_size == size and size > 0:
            break
        size = png.stat().st_size if png.exists() else -1
        time.sleep(0.25)
    else:
        raise TimeoutError(f"{png} was not written")
    # gk writes NNNN.exr (FP16 target on) before NNNN.png
    data = {**data, "hdr": (run_dir / f"{stem}.exr").exists()}
    out = run_dir / f"{stem}.json"
    if out.exists():
        raise FileExistsError(f"{out} exists; capture never rewrites a sidecar")
    tmp = out.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    tmp.replace(out)
    return out


class Watcher(threading.Thread):
    """Tails the gk log from ``pos``: writes a sidecar per ``SNAP:STATE`` and calls ``on_warp``
    with +1/-1 per ``SNAP:WARP``. ``toggles()`` returns the forms currently applied."""

    def __init__(self, gk_log: Path, pos: int, run_dir: Path, toggles: Callable[[], list[str]],
                 build_sha: str, on_warp: Callable[[int], None],
                 say: Callable[[str], None] = print):
        super().__init__(daemon=True)
        self.log, self.pos, self.run_dir = gk_log, pos, run_dir
        self.toggles, self.sha, self.on_warp, self.say = toggles, build_sha, on_warp, say

    def run(self) -> None:
        while True:
            try:
                with open(self.log, "rb") as f:
                    f.seek(self.pos)
                    data = f.read()
            except OSError:
                data = b""
            lines = data.split(b"\n")[:-1]
            for raw in lines:
                self.pos += len(raw) + 1
                self.handle(raw.decode("utf-8", "replace"))
            time.sleep(0.1)

    def handle(self, line: str) -> None:
        w = WARP_RX.search(line)
        if w:
            self.on_warp(int(w.group(1)))
            return
        st = parse_state(line)
        if st is None:
            return
        data = sidecar(st, self.toggles(), self.sha)

        def save() -> None:
            try:
                write_sidecar(self.run_dir, data)
                self.say(f"snap #{st['index']} saved ({st['level']}, hour {st['hour']:.1f})")
            except Exception as exc:  # noqa: BLE001
                self.say(f"snap #{st['index']} FAILED: {exc}")

        threading.Thread(target=save, daemon=True).start()


def shot_from_snapshot(path: Path, name: str | None = None):
    """A rig :class:`~sandover.rig.capture.Shot` at the snapshot's camera, Jak position, hour and
    level, gameplay look (Jak shown, wind, particles, time-of-day effects), at the PNG's size.
    Returns ``(shot, toggles)``; the toggles are the forms that were applied in the session."""
    from sandover.rig.capture import Shot, continue_points

    snap = json.loads(path.read_text(encoding="utf-8"))
    conts = continue_points()
    level_of = dict(conts)
    cont = snap.get("continue")
    if snap.get("level") and level_of.get(cont) != snap["level"]:
        cont = next((c for c, lv in conts if lv == snap["level"]), cont)
    if cont not in level_of:
        raise ValueError(f"{path}: no continue point for level {snap.get('level')!r}")
    res = (2560, 1440)
    png = path.with_suffix(".png")
    if png.exists():
        from PIL import Image

        with Image.open(png) as im:
            res = im.size
    rows = snap["cam_rows"]
    shot = Shot(
        name=name or f"{path.parent.name}-{path.stem}", continue_point=cont, hour=float(snap["hour"]),
        camera={"pos": list(snap["cam_pos"]), "rot": [x for r in rows for x in r]},
        effects=True, wind=True, jak=True, particles=True, resolution=tuple(res),
        jak_pos=tuple(snap["jak_pos"]))
    return shot, list(snap.get("toggles") or [])


def stage_hook(forms: list[str]) -> str:
    """A ``*rig-stage-hook*`` that re-sends ``forms`` after the settle frames (a level load can
    reset some toggles)."""
    return f"(set! *rig-stage-hook* (lambda () {' '.join(forms)} (none)))"
