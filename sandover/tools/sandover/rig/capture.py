"""Unattended, deterministic Jak 1 captures driven by a shot manifest.

Boots goalc and gk once from ``$SANDOVER_ROOT/out/build/Release/bin``, loads ``rig.gc``
over the nREPL, turns on capture mode (fixed game-time step per frame) and runs each shot
inside the game: respawn at the continue point, wait for the ``RIG:READY`` marker, reset
clocks, random seeds, actors and particles, fix the camera, set and freeze the hour,
settle a fixed number of frames, pause, screenshot. PNGs and JSON sidecars go to
``$SANDOVER_DATA/shots/<set>/``; each finished shot prints ``RIG:DONE <name>``.

Usage::

    sandover-capture run ../shots/auto-v0.yaml
    sandover-capture run ../shots/auto-v0.yaml --only village1-hut-noon --out-dir /tmp/a
    sandover-capture run ../shots/auto-v0.yaml --actors live --max-seconds 1800
    sandover-capture run ../shots/auto-v0.yaml --while-playing fail   # default: wait for play to end
    sandover-capture run ../shots/neighbours.yaml --goal-after-load '(pc-set-vista "neighbours")' --wait-vista
    sandover-capture one --continue-point village1-hut --hour 12 --out hut.png
    sandover-capture from-snapshot $SANDOVER_DATA/runs/<run>/0003.json --bin <build>
    sandover-capture compare runA/ runB/          # FLIP per shot
    sandover-capture continues                    # continue points from level-info.gc
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import threading
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from dataclasses import dataclass
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path

from sandover import engine, locks, presets, settings, vanilla
from sandover.paths import data_dir, root
from sandover.rig import nrepl, playguard
from sandover.rig.playguard import PlayGuard, PlaySessionActive

HERE = Path(__file__).resolve().parent
EXE = ".exe" if os.name == "nt" else ""
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
HOUR_LABELS = {6: "dawn", 12: "noon", 18: "dusk", 23: "night"}
EPOCH_STRIDE = 10_000_000  # game ticks between shot clocks; one shot uses < 200k
PAN_MAX_FRAMES = 27  # rig.gc RIG_PAN_MAX - 5
METER = 4096.0
ACTORS = ("live", "freeze")
READY_TIMEOUT = 300.0
#: environment overrides gk reads; recorded in every sidecar
ENV_KEYS = ("SANDOVER_MATERIALS", "SANDOVER_PBR_MAPS", "SANDOVER_PBR_PRESET", "SANDOVER_GK_PROJ_PATH")
#: ``--play-look``: the game's own defaults after the load, pushed in the same message so a
#: later frame's settings update has nothing left to re-send over the shot's param forms
PLAY_LOOK = ("(sandover-settings-reset! (-> *pc-settings* sandover)) (sandover-settings-reapply) "
             "(sandover-settings-update)")


T0 = time.monotonic()


def stamp(msg: str) -> None:
    print(f"[{time.monotonic() - T0:7.1f}s] {msg}", flush=True)


class Crashed(RuntimeError):
    """gk or goalc exited while a shot was running."""


class Hung(TimeoutError):
    """No ``RIG:READY`` within the ready timeout: the instance is stuck (usually the IOP
    sound-RPC deadlock on a level load, #312) and must be restarted."""


ACTORS_HELD = re.compile(r"RIG:ACTORS freeze after=(\d+)")
CUTSCENE = re.compile(r"RIG:CUTSCENE \S+ (.*)$")


class Log:
    """Incremental reader over a growing log file. Every line read (or skipped) is passed to
    each callable in ``taps`` first."""

    def __init__(self, path: Path):
        self.path, self.pos = path, 0
        self.taps: list[Callable[[str], None]] = []

    def _lines(self):
        if not self.path.exists():
            return
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            data = f.read()
        for raw in data.split(b"\n")[:-1]:
            self.pos += len(raw) + 1
            line = ANSI.sub("", raw.decode("utf-8", "replace"))
            for tap in self.taps:
                tap(line)
            yield line

    def skip_to_end(self) -> None:
        if self.taps:
            for _ in self._lines():
                pass
        self.pos = self.path.stat().st_size if self.path.exists() else 0

    def wait_until(self, test: Callable[[str], object], timeout: float, what: str,
                   alive: Callable[[], bool] | None = None,
                   tick: Callable[[], object] | None = None):
        """Read lines until ``test(line)`` returns something truthy (returned), or ``tick()``,
        called once per poll, does."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if alive and not alive():
                raise Crashed(f"process died while waiting for {what} in {self.path}")
            for line in self._lines():
                r = test(line)
                if r:
                    return r
            if tick:
                r = tick()
                if r:
                    return r
            time.sleep(0.1)
        raise TimeoutError(f"no {what} in {self.path.name} after {timeout:.0f}s")

    def wait_for(self, pattern: str, timeout: float, fail: str | None = None,
                 alive: Callable[[], bool] | None = None) -> re.Match:
        rx, frx = re.compile(pattern), re.compile(fail) if fail else None

        def test(line: str) -> re.Match | None:
            if frx and frx.search(line):
                raise RuntimeError(f"{self.path.name}: {line.strip()}")
            return rx.search(line)

        return self.wait_until(test, timeout, f"/{pattern}/", alive)


MATERIALS = re.compile(r"\bmaterials: ([\w-]+) (?:\d+ textures|maps )")
VISTA = re.compile(r"VISTA: drawing(.*?) from (\S+)'s camera(?:; waiting for the loader:(.*?))?\s*(?:\||$)")
VISTA_OFF = re.compile(r"VISTA: (\S+) is not an outdoor vista level, neighbours off")


#: the renderer's once-per-process table lines: class rows and the files it loaded (or missed)
MATERIALS_TABLE = re.compile(r"\bmaterials: (\d+ rows .*|map index .*|preset .*|.* not found.*|.* unreadable.*"
                             r"|.* has no combo_id.*)$")
MATERIALS_LEVEL = re.compile(r"\bmaterials: ([\w-]+ \d+ textures, .*)$")


def materials_summary(line: str) -> tuple[str, str] | None:
    """``("table", text)`` for a materials table line (rows per class and file path, map index,
    PBR preset, missing files), ``("level", text)`` for a level's texture classes, else None."""
    m = MATERIALS_TABLE.search(line)
    if m:
        return "table", m.group(1).strip()
    m = MATERIALS_LEVEL.search(line)
    return ("level", m.group(1).strip()) if m else None


def loaded_level(line: str) -> str | None:
    """The level a renderer ``materials: <level> ...`` line reports as loaded (the line
    play.py waits for before applying forms), else None."""
    m = MATERIALS.search(line)
    return m.group(1) if m else None


def parse_vista(line: str) -> dict | None:
    """One ``VISTA: drawing <levels> from <level>'s camera[; waiting for the loader: <levels>]
    | ...`` line (VistaRenderer, logged when it changes): drawn and still-loading neighbours.
    ``VISTA: <level> is not an outdoor vista level, neighbours off`` (an indoor viewer) reads as done."""
    m = VISTA.search(line)
    if not m:
        off = VISTA_OFF.search(line)
        return {"drawing": [], "camera": off.group(1), "waiting": []} if off else None
    drawn = m.group(1).split()
    return {"drawing": [] if drawn == ["nothing"] else drawn, "camera": m.group(2),
            "waiting": (m.group(3) or "").split()}


class VistaWait:
    """``--wait-vista``: done when a VISTA line read after the wait starts has an empty
    loader list, or when none arrives for ``quiet`` s and the last one before was complete
    (the renderer logs only changes)."""

    def __init__(self, last: dict | None, quiet: float = 8.0,
                 clock: Callable[[], float] = time.monotonic):
        self.last, self.quiet, self.clock = last, quiet, clock
        self.t0, self.fresh = clock(), False

    def feed(self, line: str) -> bool:
        v = parse_vista(line)
        if v is None:
            return False
        self.last, self.fresh = v, True
        return not v["waiting"]

    def idle(self) -> bool:
        return (not self.fresh and self.last is not None and not self.last["waiting"]
                and self.clock() - self.t0 >= self.quiet)


@dataclass
class Shot:
    """One capture. ``camera`` is "from_continue", "follow" or {pos: [3], rot: [9]}.
    ``jak_pos`` (game units) moves Jak there once the continue point is ready."""

    name: str
    continue_point: str
    hour: float
    camera: str | dict = "from_continue"
    effects: bool = False
    wind: bool = False
    jak: bool = True
    particles: bool = False
    settle_frames: int = 60
    resolution: tuple[int, int] = (2560, 1440)
    msaa: int = 4
    index: int = 0
    jak_pos: tuple[float, float, float] | None = None
    pan: dict | None = None
    goals_after_load: tuple[str, ...] = ()
    wait_vista: bool = False
    wait_log: str | None = None
    wait_timeout: float = 120.0
    actors: str = "live"
    render_scale: float | None = None  # scene size / output size; None: left as it is
    upscaler: str | None = None  # "bilinear" or "fsr" (FidelityFX FSR 1)
    cutscenes: bool = False  # let cutscenes play; default: the rig ends each as it starts

    @property
    def hold(self) -> bool:
        """The shot stops after the level loads for forms and log waits (``*rig-hold*``)."""
        return bool(self.goals_after_load or self.wait_vista or self.wait_log
                    or self.render_scale is not None or self.upscaler is not None)

    def render_scale_forms(self) -> tuple[str, ...]:
        """``pc-set-render-scale`` forms for ``render_scale`` / ``upscaler``, sent after every
        other after-load form so a settings reset in an arm does not undo them."""
        out = []
        if self.render_scale is not None:
            out.append(f"(pc-set-render-scale 0 {round(self.render_scale * 1000000)})")
        if self.upscaler is not None:
            out.append(f"(pc-set-render-scale 1 {UPSCALERS.index(self.upscaler) * 1000000})")
        return tuple(out)


#: manifest ``upscaler`` values, in pc-set-render-scale order
UPSCALERS = ("bilinear", "fsr")


def _render_scale(m: dict) -> dict:
    rs, up = m.get("render_scale"), m.get("upscaler")
    if rs is not None and not 0.25 <= float(rs) <= 1.0:
        raise ValueError(f"{m['name']}: render_scale must be in 0.25..1, not {rs!r}")
    if up is not None and up not in UPSCALERS:
        raise ValueError(f"{m['name']}: upscaler must be one of {UPSCALERS}, not {up!r}")
    return {"render_scale": None if rs is None else float(rs), "upscaler": up}


def _bool(v, default: bool) -> bool:
    if v is None:
        return default
    if isinstance(v, str):
        return v.lower() in ("on", "true", "yes", "show")
    return bool(v)


def actors_mode(v, default: str, name: str = "") -> str:
    """``actors: live | freeze`` of a manifest entry or the CLI; ``default`` when unset."""
    if v is None:
        return default
    v = str(v).lower()
    if v not in ACTORS:
        raise ValueError(f"{name + ': ' if name else ''}actors must be one of {ACTORS}, not {v!r}")
    return v


def load_manifest(path: Path, actors: str = "live") -> tuple[str, list[Shot]]:
    """Expand a manifest (see sandover/shots/auto-v0.yaml) into an ordered shot list.
    ``actors`` is the mode of shots that set none (neither per shot nor in ``defaults``)."""
    import yaml

    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    set_name = doc.get("set") or path.stem
    d = doc.get("defaults", {})
    raw: list[dict] = []
    gen = doc.get("generate")
    if gen:
        if gen.get("from") != "continue_points":
            raise ValueError(f"unsupported generate.from: {gen.get('from')}")
        hours = gen["hours"]
        hours = hours if isinstance(hours, dict) else {HOUR_LABELS.get(h, f"h{h:02d}"): h for h in hours}
        conts = gen.get("continues") or [c for c, _ in continue_points()]
        for c in conts:
            for label, h in hours.items():
                raw.append({"name": f"{c}-{label}", "continue": c, "hour": h})
    raw += doc.get("shots") or []
    known = {c for c, _ in continue_points()}
    shots, seen = [], set()
    for i, s in enumerate(raw):
        m = {**d, **s}
        if m["continue"] not in known:
            raise ValueError(f"{m['name']}: unknown continue point {m['continue']!r}")
        if m["name"] in seen:
            raise ValueError(f"duplicate shot name {m['name']!r}")
        seen.add(m["name"])
        cam = m.get("camera", "from_continue")
        shots.append(Shot(
            name=m["name"], continue_point=m["continue"], hour=float(m["hour"]),
            camera=cam, effects=_bool(m.get("effects"), False), wind=_bool(m.get("wind"), False),
            jak=_bool(m.get("jak"), True), particles=_bool(m.get("particles"), False),
            settle_frames=int(m.get("settle_frames", 60)),
            resolution=tuple(m.get("resolution", (2560, 1440))), msaa=int(m.get("msaa", 4)), index=i,
            jak_pos=tuple(float(v) for v in m["jak_pos"]) if m.get("jak_pos") else None,
            pan=_pan(m), actors=actors_mode(m.get("actors"), actors, m["name"]), **_post_load(m),
            **_render_scale(m), cutscenes=_bool(m.get("cutscenes"), False)))
    return set_name, shots


def _post_load(m: dict) -> dict:
    """``goals_after_load`` (a form or a list), ``wait_vista``, ``wait_log`` (regex) and
    ``wait_log_timeout`` (s) of a merged manifest entry, as :class:`Shot` fields."""
    forms = m.get("goals_after_load") or ()
    forms = (forms,) if isinstance(forms, str) else tuple(str(f) for f in forms)
    rx = m.get("wait_log")
    if rx:
        try:
            re.compile(rx)
        except re.error as exc:
            raise ValueError(f"{m['name']}: bad wait_log regex {rx!r}: {exc}") from None
    return {"goals_after_load": forms, "wait_vista": _bool(m.get("wait_vista"), False),
            "wait_log": rx or None, "wait_timeout": float(m.get("wait_log_timeout", 120.0))}


def apply_play_look(shots: list[Shot]) -> None:
    """``--play-look``: :data:`PLAY_LOOK` goes first in every shot's after-load forms."""
    for s in shots:
        s.goals_after_load = (PLAY_LOOK, *s.goals_after_load)


def apply_post_load_args(shots: list[Shot], forms: list[str], wait_log: str | None,
                         wait_vista: bool, timeout: float | None) -> None:
    """Command-line ``--goal-after-load`` forms go after each shot's own; ``--wait-log`` and
    ``--wait-log-timeout`` replace the shot's; ``--wait-vista`` turns the vista wait on."""
    for s in shots:
        s.goals_after_load = tuple(s.goals_after_load) + tuple(forms)
        s.wait_vista = s.wait_vista or wait_vista
        s.wait_log = wait_log or s.wait_log
        if timeout is not None:
            s.wait_timeout = float(timeout)


def _pan(m: dict) -> dict | None:
    """``pan: {frames, yaw_deg, move_m}``: a burst of ``frames`` consecutive frames while the
    explicit camera turns ``yaw_deg`` about the world up axis and moves ``move_m`` (world
    metres, [x, y, z]) per frame."""
    pan = m.get("pan")
    if not pan:
        return None
    if not isinstance(m.get("camera"), dict):
        raise ValueError(f"{m['name']}: pan needs an explicit camera {{pos, rot}}")
    frames = int(pan.get("frames", 8))
    if not 1 <= frames <= PAN_MAX_FRAMES:
        raise ValueError(f"{m['name']}: pan frames must be 1..{PAN_MAX_FRAMES}")
    return {"frames": frames, "yaw_deg": float(pan.get("yaw_deg", 0.0)),
            "move_m": [float(v) for v in pan.get("move_m", [0.0, 0.0, 0.0])]}


def pan_cameras(camera: dict, pan: dict) -> list[tuple[list[float], list[float]]]:
    """(pos, rot rows) for pan cameras 0..frames+4: camera k is the explicit camera turned
    k x ``yaw_deg`` about the world up axis (each row rotated) and moved k x ``move_m``."""
    out = []
    for k in range(pan["frames"] + 5):
        a = math.radians(pan["yaw_deg"] * k)
        c, s = math.cos(a), math.sin(a)
        r = camera["rot"]
        rows = []
        for i in range(3):
            x, y, z = r[3 * i:3 * i + 3]
            rows += [x * c + z * s, y, -x * s + z * c]
        pos = [p + k * d * METER for p, d in zip(camera["pos"], pan["move_m"])]
        out.append((pos, rows))
    return out


EMLIGHTS_OFF = re.compile(r"EMLIGHTS f=\d+ off (.*)$")

CAMERA_TOL_M = 0.05
CAMERA_TOL_DEG = 0.25
HEX12 = r"((?:[0-9A-Fa-f]+ ?){12})"
CAMFIX = re.compile(r'RIG:CAMFIX "?[^"\s]+"? region="?([^"\s]*)"? state=(\S+) slaves=(\d+) ' + HEX12)


def camera_bits(hexes: str) -> tuple[list[float], list[float]] | None:
    """(trans, rot) from the 12 raw float bit words of a RIG:CAMERA/CAMREQ/CAMFIX line, or None
    when the line does not hold exactly 12 words (another thread's output cut into it: HEX12
    lets a cut line still match by splitting words)."""
    words = hexes.split()
    if len(words) != 12:
        return None
    v = [struct.unpack("<f", struct.pack("<I", int(h, 16) & 0xFFFFFFFF))[0] for h in words]
    return v[:3], v[3:12]


def camera_error(req_trans, req_rot, trans, rot) -> tuple[float, float]:
    """(metres, degrees) between two cameras: the position distance and the angle of the
    rotation taking one inverse-camera-rot 3x3 (row-major) onto the other."""
    if len(req_trans) != 3 or len(trans) != 3 or len(req_rot) != 9 or len(rot) != 9:
        raise ValueError(f"malformed camera: trans {len(req_trans)}/{len(trans)}, "
                         f"rot {len(req_rot)}/{len(rot)} values")
    dist = math.dist(req_trans, trans) / METER
    tr = sum(a * b for a, b in zip(req_rot, rot, strict=True))
    # continue-point matrices are stored to 4 decimals: scale out their row-norm error
    tr *= 3.0 / math.sqrt(sum(a * a for a in req_rot) * sum(b * b for b in rot))
    return dist, math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1.0) / 2.0))))


def check_camera(requested: tuple, actual: tuple, tol_m: float = CAMERA_TOL_M,
                 tol_deg: float = CAMERA_TOL_DEG) -> dict:
    """``camera_check`` sidecar field: the captured (trans, rot) against the requested one.
    ``ok`` is None (not False) when either camera line was malformed: unknown, not a mismatch."""
    if requested is None or actual is None:
        return {"ok": None, "error": "malformed RIG:CAMERA/CAMREQ line", "tol_m": tol_m,
                "tol_deg": tol_deg}
    dist, deg = camera_error(*requested, *actual)
    return {"ok": dist <= tol_m and deg <= tol_deg, "dist_m": round(dist, 4),
            "angle_deg": round(deg, 3), "tol_m": tol_m, "tol_deg": tol_deg}


def parse_camfix(line: str) -> dict | None:
    """One ``RIG:CAMFIX`` line: by the end of the settle frames the camera master had moved the
    rig's camera (Jak in a camera region), so the rig fixed it again."""
    m = CAMFIX.search(line)
    if not m:
        return None
    bits = camera_bits(m.group(4))
    if bits is None:
        return None
    trans, rot = bits
    return {"region": None if m.group(1) == "none" else m.group(1), "state": m.group(2),
            "slaves": int(m.group(3)), "trans": trans, "rot": rot}


def camera_mismatch_message(name: str, check: dict) -> str:
    return (f"RIG:CAMERA-MISMATCH {name}: the captured camera is {check['dist_m']:.3f} m and "
            f"{check['angle_deg']:.2f} deg from the requested one (limits {check['tol_m']} m, "
            f"{check['tol_deg']} deg)")


def parse_emlights(line: str) -> dict | None:
    """One ``EMLIGHTS f=N raw=... out=... world=...`` log line (EmissiveLights::log_frame):
    ``raw`` [x, y, z, luma, fill, r, g, b], ``out`` and ``world`` [id, x, y, z, luma, r, g, b]
    (older builds stop at luma); world metres."""
    m = re.search(r"EMLIGHTS f=(\d+) raw=(\S*) out=(\S*) world=(\S*)", line)
    if not m:
        return None

    def rows(txt: str, id_first: bool) -> list[list[float]]:
        out = []
        for item in filter(None, txt.split(";")):
            v = [float(x) for x in item.split(",")]
            if id_first:
                v[0] = int(v[0])
            out.append(v)
        return out

    return {"frame": int(m.group(1)), "raw": rows(m.group(2), False),
            "out": rows(m.group(3), True), "world": rows(m.group(4), True)}


class LogWaiter:
    """``--wait-log`` and/or ``--wait-vista`` over a stream of gk log lines: ``feed`` each
    line, poll ``idle``; both return True once every requested condition has held."""

    def __init__(self, regex: str | None, vista: VistaWait | None):
        self.rx, self.regex, self.vista = (re.compile(regex) if regex else None), regex, vista
        self.log_ok, self.vista_ok = self.rx is None, vista is None
        self.line: str | None = None
        self.what = " and ".join(w for w, on in (("an empty vista loader list", vista),
                                                 (f"/{regex}/", regex)) if on)

    def done(self) -> bool:
        return self.log_ok and self.vista_ok

    def feed(self, line: str) -> bool:
        if not self.vista_ok and self.vista.feed(line):
            self.vista_ok = True
        if not self.log_ok and self.rx.search(line):
            self.log_ok, self.line = True, line.strip()
        return self.done()

    def idle(self) -> bool:
        if not self.vista_ok and self.vista.idle():
            self.vista_ok = True
        return self.done()

    def result(self) -> dict:
        out: dict = {"ok": self.done()}
        if self.regex:
            out["log"] = self.regex
            if self.line:
                out["line"] = self.line
        if self.vista:
            out["vista"] = self.vista.last
        return out


def continue_points() -> list[tuple[str, str]]:
    """(name, level) for every continue point in level-info.gc, in file order."""
    src = (root() / "goal_src" / "jak1" / "engine" / "level" / "level-info.gc").read_text(encoding="utf-8")
    rx = re.compile(r"'continue-point\s+:name\s+\"([^\"]+)\"\s+:level\s+'([\w-]+)")
    return rx.findall(src)


def git_sha() -> str:
    try:
        sha = subprocess.run(["git", "-C", str(root()), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(root()), "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True).stdout.strip()
        return sha + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"



def env_overrides() -> dict[str, str | None]:
    """The :data:`ENV_KEYS` as this tool passes them to gk (None when unset)."""
    return {k: os.environ.get(k) or None for k in ENV_KEYS}


def _mute_env() -> dict:
    """Rig instances render silently: OpenAL Soft's null backend unless SANDOVER_RIG_AUDIO=1."""
    return {} if os.environ.get("SANDOVER_RIG_AUDIO") == "1" else {"ALSOFT_DRIVERS": "null"}


#: SDL hints for every rig gk (environment values beat the game's own SDL_SetHint calls). The
#: allow-list holds one VID/PID no pad has, so SDL opens no gamepad at all; see rig-capture.md
RIG_INPUT_ENV = {
    # VID 0 is unassigned; not 0/0, which XInput reports for a pad it cannot identify
    "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x0000/0x0001",
    "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "0",
    "SDL_WINDOW_ACTIVATE_WHEN_SHOWN": "0",
    "SDL_WINDOW_ACTIVATE_WHEN_RAISED": "0",
}


def bin_root(bin_dir: Path) -> Path | None:
    """The project root a staged bin was built against (``root`` in its build.json), if any."""
    try:
        rel = json.loads((bin_dir / "build.json").read_text(encoding="utf-8")).get("root")
    except (OSError, ValueError):
        return None
    proj = data_dir() / rel if rel else None
    return proj if proj is not None and proj.is_dir() else None


def gk_env(slot: locks.Slot, gk_root: str) -> dict[str, str]:
    """Environment for a rig gk: :func:`engine_env` with ``gk_root`` as the project root, muted
    audio and no controller input (:data:`RIG_INPUT_ENV`)."""
    return dict(engine_env(slot), OPENGOAL_PROJ_PATH=gk_root, **_mute_env(), **RIG_INPUT_ENV)


def engine_env(slot: locks.Slot) -> dict[str, str]:
    """Environment for gk and goalc on ``slot``: the project root, and the engine's own
    rotating logs in the slot's log dir, so no other instance can hold the oldest one open."""
    return dict(os.environ, OPENGOAL_PROJ_PATH=str(root()),
                OPENGOAL_LOG_DIR=str(slot.logs_dir / "engine"))


def goalc_env_for(slot: locks.Slot, write: bool = False) -> dict[str, str]:
    """:func:`engine_env` plus goalc's REPL history in the slot's config dir and, unless
    ``write``, no rewriting of out/ by (mi)."""
    env = engine_env(slot)
    # goalc has no --config-path; APPDATA redirects its REPL history off the system drive
    if os.name == "nt":
        env["APPDATA"] = str(slot.config_dir)
    if not write:
        env["OPENGOAL_MAKE_NO_WRITE"] = "1"
    return env


def goal_float(v: float) -> str:
    """Positional literal that round-trips the value; the GOAL reader takes no exponent (2.4e-07)
    and needs a '.' to read a float."""
    v = float(v)
    if not math.isfinite(v):
        raise ValueError(f"not a finite float: {v}")
    s = format(Decimal(repr(v)), "f")
    return s if "." in s else s + ".0"


def goal_bool(v: bool) -> str:
    return "#t" if v else "#f"


@dataclass(frozen=True)
class AbSpec:
    """Paused-frame A/B (``capture run --ab-mask A B``): each shot is rendered with perf-opt mask
    A, then the same paused frame with mask B, then with mask A again, into ``<out>/A/``,
    ``<out>/B/`` and ``<out>/A2/``. A against A2 is the shot's own floor: state that still moves
    while the game is paused (renderer-clock animation, fades) shows there, not only in A/B.
    ``water_clock_s`` pins the wave clock (water option 90) so both runs see the same waves;
    None leaves it live."""

    a: int
    b: int
    water_clock_s: float | None = 100.0

    def forms(self) -> str:
        out = f"(set! *rig-ab* #t) (set! *rig-ab-a* {self.a}) (set! *rig-ab-mask* {self.b})"
        if self.water_clock_s is not None:
            out += f" (pc-set-water 90 {round(self.water_clock_s * 1_000_000)})"
        return out


def ab_dirs(out_dir: Path) -> tuple[Path, Path, Path]:
    return out_dir / "A", out_dir / "B", out_dir / "A2"


def ab_verdicts(ab_rows: list[dict], floor_rows: list[dict], threshold: int = 2) -> list[dict]:
    """Per shot, A/B and the A/A2 floor side by side. ``ok``: A/B within the threshold.
    ``floor``: A/B over it, but so is the floor, and A/B's share of pixels over the threshold is
    at most 1.5x the floor's plus 0.001% (state moving while paused, not the mask). ``fail``:
    anything else."""
    floors = {r["name"]: r for r in floor_rows}
    out = []
    for r in ab_rows:
        f = floors.get(r["name"], {})
        v = {"name": r["name"], "ab_max": r.get("max"), "ab_share": r.get("share_over"),
             "floor_max": f.get("max"), "floor_share": f.get("share_over")}
        if "error" in r or r.get("max") is None:
            v["verdict"] = "error"
        elif r["max"] <= threshold:
            v["verdict"] = "ok"
        elif (f.get("max") is not None and f["max"] > threshold
              and r["share_over"] <= 1.5 * f["share_over"] + 1e-5):
            v["verdict"] = "floor"
        else:
            v["verdict"] = "fail"
        out.append(v)
    return out


def ab_report(out_dir: Path, ab: AbSpec, threshold: int = 2) -> list[dict]:
    """pixdiff of A against B and of A against A2 (the floor); writes ``pixdiff.json``,
    ``pixdiff-floor.json`` and ``ab-verdicts.json`` and prints one line per shot."""
    from sandover.rig import pixdiff

    a_dir, b_dir, a2_dir = ab_dirs(out_dir)
    ab_rows = pixdiff.compare(a_dir, b_dir, threshold)
    floor_rows = pixdiff.compare(a_dir, a2_dir, threshold) if a2_dir.exists() else []
    for name, rows in (("pixdiff.json", ab_rows), ("pixdiff-floor.json", floor_rows)):
        doc = {"threshold": threshold, "shots": rows}
        (out_dir / name).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    verdicts = ab_verdicts(ab_rows, floor_rows, threshold)
    (out_dir / "ab-verdicts.json").write_text(json.dumps(verdicts, indent=2) + "\n", encoding="utf-8")

    def pct(x):
        return "-" if x is None else f"{100 * x:.4f}%"

    stamp(f"A/B masks {ab.a:#x} / {ab.b:#x} (floor = A again):")
    for v in verdicts:
        stamp(f"  {v['name']:24s} {v['verdict']:5s} A/B max {v['ab_max']} over {pct(v['ab_share'])}  "
              f"floor max {v['floor_max']} over {pct(v['floor_share'])}")
    return verdicts


def wait_png(png: Path, before: float, timeout: float = 30) -> None:
    """Until ``png`` is newer than ``before`` and its size has stopped changing."""
    deadline = time.monotonic() + timeout
    while not (png.exists() and png.stat().st_mtime > before):
        if time.monotonic() > deadline:
            raise TimeoutError(f"{png} was not written")
        time.sleep(0.1)
    size = -1
    while size != png.stat().st_size:
        size = png.stat().st_size
        time.sleep(0.3)


class Game:
    """One goalc + gk session on a game instance slot, with rig.gc loaded and capture mode on."""

    def __init__(self, slot: locks.Slot, bin_dir: Path, write: bool = False,
                 compiled: threading.Event | None = None):
        """``write``: this goalc's (mi) rewrites out/ (under the compile lock); otherwise it
        only compiles for type info, after ``compiled`` is set."""
        self.slot, self.bin, self.write, self.compiled = slot, bin_dir, write, compiled
        self.logs, self.cfg = slot.logs_dir, slot.config_dir
        for d in (self.logs, self.cfg):
            d.mkdir(parents=True, exist_ok=True)
        # the game's own Sandover defaults are all on; the rig boots vanilla and sends forms
        settings.seed_off(self.cfg)
        self.shots_dir = self.cfg / "OpenGOAL" / "jak1" / "screenshots"
        self.goalc = self.gk = None
        self.goalc_log = Log(self.logs / "goalc.out.log")
        self.gk_log = Log(self.logs / "gk.out.log")
        self.gk_log.taps.append(self._tap)
        self.vista: dict | None = None  # last VISTA line
        self.loads: list[str] = []  # levels loaded since the current shot started
        self.level: str | None = None  # level of the previous held shot
        self.camfix: dict | None = None  # RIG:CAMFIX of the current shot
        self.ab: AbSpec | None = None  # paused-frame A/B per shot
        self.held_after: int | None = None  # RIG:ACTORS freeze of the current shot
        self.materials: list[str] = []  # table lines of this gk process
        self.level_classes: dict[str, str] = {}  # level -> its texture class line
        self.cuts: list[str] = []  # RIG:CUTSCENE lines of the current shot

    def _tap(self, line: str) -> None:
        lv = loaded_level(line)
        if lv:
            self.loads.append(lv)
        mat = materials_summary(line)
        if mat:
            kind, text = mat
            if kind == "level":
                self.level_classes[text.split()[0]] = text
            elif text not in self.materials:
                self.materials.append(text)
        held = ACTORS_HELD.search(line)
        if held:
            self.held_after = int(held.group(1))
        v = parse_vista(line)
        if v:
            self.vista = v
        fix = parse_camfix(line)
        if fix:
            self.camfix = fix
        cut = CUTSCENE.search(line)
        if cut:
            self.cuts.append(cut.group(1).strip())

    def send(self, text: str) -> None:
        nrepl.send(text, port=self.slot.nrepl_port)

    def __enter__(self) -> Game:
        s = self.slot
        goalc_env = goalc_env_for(s, self.write)
        # stdin at EOF keeps goalc's stdout flushed and (e) honoured (see kb/howto/rig-capture.md)
        empty = self.logs / "empty-stdin.txt"
        empty.write_bytes(b"")
        self.goalc = subprocess.Popen(
            [self.bin / f"goalc{EXE}", "--user-auto", "--game", "jak1",
             "--port", str(s.nrepl_port), "--debug-port", str(s.debug_port)],
            cwd=root(), env=goalc_env,
            stdin=open(empty, "rb"), stdout=open(self.goalc_log.path, "wb"), stderr=subprocess.STDOUT)
        try:
            self.goalc_log.wait_for(r"nREPL: Listening on", 60)
            if self.write:
                with locks.hold(locks.COMPILE, f"(mi) for slot{s.index}"):
                    self.make()
            else:
                if self.compiled is not None:
                    self.compiled.wait()
                while locks.holder(locks.COMPILE):
                    time.sleep(1)
                self.make()
            stamp(f"slot{s.index}: (mi) done ({'writing out/' if self.write else 'types only'})")
            # a worktree build reads its own shaders from a project root that links the main data
            gk_root = os.environ.get("SANDOVER_GK_PROJ_PATH") or str(root())
            self.gk = subprocess.Popen(
                [self.bin / f"gk{EXE}", "-v", "--game", "jak1", "--config-path", self.cfg,
                 "--port", str(s.game_port), "--", "-boot", "-fakeiso", "-debug"],
                cwd=gk_root, env=gk_env(s, gk_root),
                stdout=open(self.gk_log.path, "wb"), stderr=subprocess.STDOUT)
            self.gk_log.wait_for(r"InitListenerConnect", 120)
            for _ in range(20):
                self.send(f"(lt {s.game_port})")
                try:
                    self.goalc_log.wait_for(r"Got version .* OK", 3)
                    break
                except TimeoutError:
                    pass
            else:
                raise TimeoutError("(lt) never connected")
            self.goalc_log.skip_to_end()
            self.send((HERE / "rig.gc").read_text(encoding="utf-8"))
            try:
                self.gk_log.wait_for(r"RIG:LOADED", 60)
            except TimeoutError:
                self.goalc_log.wait_for(r"Compilation Error", 1)
                raise RuntimeError(f"rig.gc did not compile; see {self.goalc_log.path}") from None
            self.send("(rig-mode #t)")
            self.gk_log.wait_for(r"RIG:MODE #t", 10)
            stamp(f"slot{s.index}: game up on port {s.game_port}, capture mode on")
        except BaseException:
            self.close()
            raise
        return self

    def make(self) -> None:
        self.send("(mi)")
        self.goalc_log.wait_for(r"Successfully built all \d+ targets", 600,
                                fail=r"Compilation Error|REPL Error", alive=self.alive_goalc)

    def alive_goalc(self) -> bool:
        return bool(self.goalc and self.goalc.poll() is None)

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        try:
            if self.gk and self.gk.poll() is None:
                self.send("(shutdown-target)")
                self.gk.wait(30)
            if self.goalc and self.goalc.poll() is None:
                self.send("(e)")
                self.goalc.wait(30)
        except Exception as exc:  # noqa: BLE001
            print(f"clean shutdown failed ({exc}); terminating", file=sys.stderr)
        for p in (self.gk, self.goalc):
            if p and p.poll() is None:
                p.kill()

    def kill(self) -> None:
        """Kill this instance's gk and goalc (the processes this object started) without the
        clean shutdown a stuck game would not answer."""
        for p in (self.gk, self.goalc):
            if p and p.poll() is None:
                p.kill()
                try:
                    p.wait(30)
                except subprocess.TimeoutExpired:
                    print(f"pid {p.pid} did not exit after kill", file=sys.stderr)

    def alive(self) -> bool:
        return bool(self.gk and self.gk.poll() is None and self.goalc and self.goalc.poll() is None)

    def shoot(self, shot: Shot, out_png: Path, ready_timeout: float = READY_TIMEOUT) -> dict:
        """Run one shot in the game and copy its PNG to ``out_png``. Returns the sidecar dict."""
        w, h = shot.resolution
        png = self.shots_dir / f"{shot.name}.png"
        png_b = self.shots_dir / f"{shot.name}--ab-b.png"
        png_a2 = self.shots_dir / f"{shot.name}--ab-a2.png"
        before = png.stat().st_mtime if png.exists() else 0
        before_b = png_b.stat().st_mtime if png_b.exists() else 0
        before_a2 = png_a2.stat().st_mtime if png_a2.exists() else 0
        self.gk_log.skip_to_end()
        self.loads = []
        self.camfix = None
        self.held_after = None
        self.cuts = []
        t0 = time.monotonic()
        self.send(f'(rig-shot-setup {w} {h} {shot.msaa} "{shot.name}")')
        self.gk_log.wait_for(r"RIG:SHOTCFG", 10)
        cam_mode = "continue"
        if shot.camera == "follow":
            cam_mode = "follow"
        elif isinstance(shot.camera, dict):
            cam_mode = "explicit"
            x, y, z = shot.camera["pos"]
            r = shot.camera["rot"]
            self.send(f"(rig-set-camera-pos {goal_float(x)} {goal_float(y)} {goal_float(z)})")
            for i in range(3):
                self.send(f"(rig-set-camera-row {i} " + " ".join(goal_float(v) for v in r[3 * i:3 * i + 3]) + ")")
        epoch = (shot.index + 1) * EPOCH_STRIDE
        self.send(f"(rig-set-options {goal_bool(not shot.jak)} {goal_bool(shot.wind)} "
                  f"{goal_bool(shot.effects)} {goal_bool(shot.particles)})")
        if shot.actors != "live":
            self.send(f"(rig-set-actors '{shot.actors})")
        if shot.cutscenes:
            self.send("(rig-set-cutscenes #t)")
        if shot.jak_pos:
            self.send("(rig-set-jak-pos {} {} {})".format(*map(goal_float, shot.jak_pos)))
        if shot.pan:
            for i, (pos, rows) in enumerate(pan_cameras(shot.camera, shot.pan)):
                self.send(f"(rig-set-pan-pos {i} " + " ".join(map(goal_float, pos)) + ")")
                for j in range(3):
                    self.send(f"(rig-set-pan-row {i} {j} "
                              + " ".join(map(goal_float, rows[3 * j:3 * j + 3])) + ")")
            self.send(f"(rig-set-pan-frames {shot.pan['frames']})")
        # no console/hint debug text in frames (#283 flag; play hides it too)
        self.send("(set! *display-debug-text* #f)")
        self.send(f"(set! *rig-hold* {goal_bool(shot.hold)})")
        self.send(self.ab.forms() if self.ab else "(set! *rig-ab* #f)")
        log_start = self.gk_log.pos
        self.send(f"(rig-shot \"{shot.continue_point}\" {shot.hour:.4f} '{cam_mode} {shot.settle_frames} {epoch})")
        self.gk_log.wait_for(r"RIG:SEQUENCE", 10)
        try:
            ready = self.gk_log.wait_for(r"RIG:READY \S+ load=(-?\d+)", ready_timeout, alive=self.alive)
            ready_s = time.monotonic() - t0
        except TimeoutError as exc:
            raise Hung(str(exc)) from None
        post = self.post_load(shot) if shot.hold else None
        cam = self.gk_log.wait_for(r"RIG:CAMERA " + HEX12, 60, alive=self.alive)
        req = self.gk_log.wait_for(r"RIG:CAMREQ " + HEX12, 10, alive=self.alive)
        self.gk_log.wait_for(r"RIG:SHOT", 30 + (90 if self.ab else 0), alive=self.alive)
        shot_s = time.monotonic() - t0
        wait_png(png, before)
        out_png.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(png, out_png)
        hdr_meta = copy_hdr(png, out_png, before)
        if self.ab:
            wait_png(png_b, before_b)
            out_b = out_png.parent.with_name("B") / out_png.name
            out_b.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(png_b, out_b)
            b_hdr = copy_hdr(png_b, out_b, before_b).get("hdr", False)
            wait_png(png_a2, before_a2)
            out_a2 = out_png.parent.with_name("A2") / out_png.name
            out_a2.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(png_a2, out_a2)
            copy_hdr(png_a2, out_a2, before_a2)
            hdr_meta["ab"] = {"a": self.ab.a, "b": self.ab.b, "water_clock_s": self.ab.water_clock_s,
                              "b_hdr": b_hdr}
        pan_meta = self.collect_pan(shot, out_png, log_start) if shot.pan else None
        actual, requested = camera_bits(cam.group(1)), camera_bits(req.group(1))
        for label, bits, m in (("CAMERA", actual, cam), ("CAMREQ", requested, req)):
            if bits is None:
                print(f"RIG:CAMERA-MALFORMED {shot.name} {label}: {m.string.strip()[:300]}", flush=True)
        trans, rot = actual if actual else (None, None)
        extra = {"jak_pos": list(shot.jak_pos)} if shot.jak_pos else {}
        if cam_mode != "follow":
            extra.update(camera_fields(requested, actual, self.camfix))
        if pan_meta:
            extra["pan"] = pan_meta
        if post:
            extra["post_load"] = post
        if shot.actors == "freeze":
            # settle frame at which the actor groups were held (RIG:ACTORS), None if never seen
            extra["actors_held_after"] = self.held_after
        level = dict(continue_points()).get(shot.continue_point)
        extra["materials_log"] = {"table": list(self.materials),
                                  "level": self.level_classes.get(level)}
        return {
            **extra,
            "name": shot.name, "continue": shot.continue_point, "hour": shot.hour,
            "camera": cam_mode,
            # *math-camera* at the capture frame; lightbench --cam-trans/--cam-rot take these as is
            "camera_trans": trans, "camera_rot": rot,
            "effects": shot.effects, "wind": shot.wind, "jak": shot.jak, "particles": shot.particles,
            "actors": shot.actors, "env": env_overrides(),
            "settle_frames": shot.settle_frames, "resolution": list(shot.resolution), "msaa": shot.msaa,
            "render_scale": shot.render_scale, "upscaler": shot.upscaler,
            "slot": self.slot.index, "epoch_ticks": epoch, "load_ticks": int(ready.group(1)),
            "cutscenes": "allow" if shot.cutscenes else "skip", "cutscenes_ended": list(self.cuts),
            "seconds": {"ready": round(ready_s, 1), "shot": round(shot_s, 1)},
            "sha": git_sha(), "timestamp": datetime.now(UTC).isoformat(timespec="seconds"),
            **hdr_meta,
        }

    def post_load(self, shot: Shot) -> dict:
        """At ``RIG:HOLD`` (level loaded, camera fixed, before the actor reset and the settle
        frames): send the shot's after-load forms, run its log waits, release the hold."""
        try:
            self.gk_log.wait_for(r"RIG:HOLD", 60, alive=self.alive)
            level = dict(continue_points())[shot.continue_point]
            if level not in self.loads and level != self.level:
                # RIG:READY already implies the load; this only guards the log order
                try:
                    self.gk_log.wait_for(rf"\bmaterials: {re.escape(level)} ", 10, alive=self.alive)
                except TimeoutError:
                    stamp(f"{shot.name}: no 'materials: {level}' line (level already resident?)")
            info: dict = {"level": level, "level_loaded": level in self.loads}
            self.level = level
            forms = (*shot.goals_after_load, *shot.render_scale_forms())
            if forms:
                self.goalc_log.skip_to_end()
                self.send(" ".join(forms) + ' (format 0 "RIG:APPLIED~%")')
                try:
                    self.gk_log.wait_for(r"RIG:APPLIED", 60, alive=self.alive)
                except TimeoutError:
                    try:
                        err = self.goalc_log.wait_for(r"Compilation Error|REPL Error", 1)
                    except TimeoutError:
                        raise TimeoutError("after-load forms never printed RIG:APPLIED") from None
                    raise RuntimeError(f"after-load forms failed: {err.string.strip()}") from None
                info["applied"] = list(forms)
            if shot.wait_vista or shot.wait_log:
                info["wait"] = self.wait_ready(shot)
        except BaseException:
            # let the held sequence finish so it cannot overlap the next shot
            self.send("(set! *rig-hold* #f)")
            try:
                self.gk_log.wait_for(r"RIG:SHOT", 120, alive=self.alive)
            except Exception:  # noqa: BLE001
                pass
            raise
        self.send("(set! *rig-hold* #f)")
        self.gk_log.wait_for(r"RIG:RELEASED", 30, alive=self.alive)
        return info

    def wait_ready(self, shot: Shot) -> dict:
        """Tail gk's log until ``shot.wait_log`` matches and/or the vista loader list is empty,
        at most ``shot.wait_timeout`` s. A timeout is reported and recorded, not fatal."""
        waiter = LogWaiter(shot.wait_log, VistaWait(self.vista) if shot.wait_vista else None)
        t0 = time.monotonic()
        try:
            self.gk_log.wait_until(waiter.feed, shot.wait_timeout, waiter.what, self.alive, waiter.idle)
        except TimeoutError:
            print(f"RIG:WAIT-TIMEOUT {shot.name} no {waiter.what} after {shot.wait_timeout:.0f}s",
                  flush=True)
        return {**waiter.result(), "seconds": round(time.monotonic() - t0, 1)}

    def collect_pan(self, shot: Shot, out_png: Path, log_start: int) -> dict:
        """Copy the burst frames next to ``out_png`` as ``<name>-fNN.png`` and parse this
        shot's EMLIGHTS log lines (none when the tile lights are off)."""
        n = shot.pan["frames"]
        for k in range(n):
            src = self.shots_dir / f"{shot.name}-f{k:02d}.png"
            if not src.exists():
                raise RuntimeError(f"pan frame {src.name} was not written")
            shutil.copy2(src, out_png.with_name(f"{out_png.stem}-f{k:02d}.png"))
        with open(self.gk_log.path, "rb") as f:
            f.seek(log_start)
            text = f.read(self.gk_log.pos - log_start).decode("utf-8", "replace")
        lines = [ANSI.sub("", ln) for ln in text.splitlines()]
        frames = [p for p in map(parse_emlights, lines) if p]
        off = sorted({m.group(1) for m in map(EMLIGHTS_OFF.search, lines) if m})
        if not frames:
            stamp(f"{shot.name}: no tile light lists in the burst"
                  + (f" ({'; '.join(off)})" if off else " (no EMLIGHTS lines: gk without #239?)"))
        return {**shot.pan, "lights": frames, **({"off": off} if off else {})}


def camera_fields(requested: tuple | None, actual: tuple | None, camfix: dict | None) -> dict:
    """Sidecar fields ``camera_requested``, ``camera_check`` and, when the rig had to fix the
    camera again after the settle frames, ``camera_refixed`` (what the game had drawn instead).
    A malformed camera line (None) is reported in ``camera_check``, never raised."""
    out = {"camera_requested": {"trans": requested[0], "rot": requested[1]} if requested else None,
           "camera_check": check_camera(requested, actual)}
    if camfix and requested:
        dist, deg = camera_error(*requested, camfix["trans"], camfix["rot"])
        out["camera_refixed"] = {**camfix, "dist_m": round(dist, 4), "angle_deg": round(deg, 3)}
    return out


def outputs_fresh() -> bool:
    """True when the last writing (mi) (KERNEL.CGO is its first output) is newer than every
    GOAL source, so no goalc needs to rewrite out/."""
    kernel = root() / "out" / "jak1" / "iso" / "KERNEL.CGO"
    if not kernel.exists():
        return False
    src = root() / "goal_src"
    newest = max(p.stat().st_mtime for p in src.rglob("*") if p.suffix in (".gc", ".gd", ".gp", ".gs"))
    return kernel.stat().st_mtime > newest


def partition(shots: list[Shot], n: int) -> list[list[Shot]]:
    """Split shots into at most ``n`` lists, keeping each level's shots together where the
    count allows, in manifest order, balanced by shot count."""
    level = dict(continue_points())
    groups: dict[str, list[Shot]] = {}
    for sh in shots:
        groups.setdefault(level[sh.continue_point], []).append(sh)
    chunks = list(groups.values())
    # more instances than levels: split the biggest level (each half loads it once)
    while len(chunks) < n and max(len(c) for c in chunks) > 1:
        big = max(chunks, key=len)
        chunks.remove(big)
        chunks += [big[: len(big) // 2], big[len(big) // 2 :]]
    parts: list[list[Shot]] = [[] for _ in range(min(n, len(chunks)))]
    for g in sorted(chunks, key=len, reverse=True):
        min(parts, key=len).extend(g)
    for p in parts:
        p.sort(key=lambda sh: sh.index)
    return [p for p in parts if p]


def copy_hdr(png: Path, out_png: Path, before: float) -> dict:
    """Copy the EXR gk wrote with ``png`` (FP16 target on) next to ``out_png``; sidecar fields.

    gk writes the EXR before the PNG, so a complete PNG means a complete EXR."""
    from sandover import hdr

    exr = png.with_suffix(".exr")
    if not (exr.exists() and exr.stat().st_mtime >= before):
        return {"hdr": False}
    out = out_png.with_suffix(".exr")
    shutil.copy2(exr, out)
    rgb, attrs = hdr.read_exr(out)
    return {
        "hdr": True,
        "hdr_tonemap": {k.removeprefix("sandover"): v for k, v in attrs.items()},
        "hdr_stats": hdr.stats(rgb)["max"],
    }


def run_worker(slot: locks.Slot, bin_dir: Path, shots: list[Shot], out_dir: Path, write: bool,
               compiled: threading.Event | None, stagger: int = 0,
               setup: list[str] | None = None,
               toggles: dict[str, str] | None = None,
               camera_strict: bool = True, deadline: float | None = None,
               skipped: list[Shot] | None = None,
               ab: AbSpec | None = None,
               guard: PlayGuard | None = None) -> tuple[list[Shot], list[Shot]]:
    """Capture ``shots`` on one instance. Returns (failed, not captured because it crashed).
    A shot whose captured camera is off the requested one keeps its files and counts as failed,
    or with ``camera_strict`` off is only reported. A shot that hangs before ``RIG:READY``
    (:class:`Hung`) kills this instance's gk and goalc, boots a fresh pair on the same slot and
    is retried once there. Past ``deadline`` (``time.monotonic()``) no new shot starts; the
    rest go to ``skipped``. ``guard`` is consulted before each instance start and each shot
    (:class:`PlayGuard`); when it gives up, the rest go to ``skipped`` too."""
    failed: list[Shot] = []
    hung: set[str] = set()
    # binaries without OPENGOAL_LOG_DIR rotate logs in one shared dir; simultaneous starts race
    time.sleep(2.0 * stagger)
    k, game = 0, None

    def boot(first: bool):
        if guard and guard.wait(f"slot{slot.index} start", deadline)["detected"] and guard.mode == "wait":
            # only a wall cap ends a wait with the session still running: start nothing
            raise PlaySessionActive(f"wall cap reached waiting for the play session before slot{slot.index} start")
        g = Game(slot, bin_dir, write and first, compiled if first else None)
        g.__enter__()
        if first and write and compiled is not None:
            compiled.set()
        for form in setup or []:
            g.send(form)
        g.ab = ab
        return g

    def stop_idle():
        nonlocal game
        stamp(f"RIG:PLAY-WAIT slot{slot.index}: stopping the idle instance until the session ends")
        game.__exit__(None, None, None)
        game = None

    try:
        game = boot(True)
        png_dir = ab_dirs(out_dir)[0] if ab else out_dir
        while k < len(shots):
            shot = shots[k]
            before = guard.wait(f"slot{slot.index} {shot.name}", deadline, stop_idle) if guard else None
            if deadline is not None and time.monotonic() >= deadline:
                print(f"RIG:WALL-CAP slot{slot.index} skipping {len(shots) - k} shots from {shot.name}",
                      flush=True)
                if skipped is not None:
                    skipped.extend(shots[k:])
                break
            if game is None:
                game = boot(False)
            if not game.alive():
                _crash(slot, f"before {shot.name}")
                return failed, shots[k:]
            try:
                meta = game.shoot(shot, png_dir / f"{shot.name}.png")
            except Hung as exc:
                print(f"RIG:FAIL {shot.name} {exc}", flush=True)
                retry = shot.name not in hung
                hung.add(shot.name)
                if not retry:
                    failed.append(shot)
                    k += 1
                print(f"RIG:RESTART slot{slot.index} after {shot.name} hung"
                      + ("; retrying it once" if retry else ""), flush=True)
                game.kill()
                game.__exit__(None, None, None)
                game = None
                if k < len(shots):
                    game = boot(False)
                continue
            except Crashed as exc:
                _crash(slot, f"{shot.name} {exc}")
                return failed, shots[k:]
            except Exception as exc:  # noqa: BLE001
                print(f"RIG:FAIL {shot.name} {exc}", flush=True)
                failed.append(shot)
                k += 1
                continue
            k += 1
            if before is not None:
                meta["play_session"] = playguard.sidecar(before, guard.check())
                if meta["play_session"]["detected"]:
                    print(f"RIG:PLAYING {shot.name}: a play session was running at capture time", flush=True)
            meta["toggles"] = dict(toggles or {})
            if shot.goals_after_load:
                meta["toggles"]["goal_after_load"] = list(shot.goals_after_load)
            if shot.name in hung:
                meta["restarted_after_hang"] = True
            (out_dir / f"{shot.name}.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
            if not ab and vanilla.vanilla_run(toggles, setup, shot):
                try:
                    vanilla.register(out_dir / f"{shot.name}.png", stamp)
                except Exception as exc:  # noqa: BLE001
                    print(f"RIG:VANILLA-REGISTER-FAILED {shot.name} {exc}", flush=True)
            check = meta.get("camera_check")
            if check and check.get("ok") is False:
                print(camera_mismatch_message(shot.name, check), flush=True)
                if camera_strict:
                    failed.append(shot)
                    continue
            stamp(f"RIG:DONE {shot.name} slot{slot.index} load={meta['load_ticks']}")
    except PlaySessionActive as exc:
        print(f"RIG:PLAYING slot{slot.index} skipping {len(shots) - k} shots: {exc}", flush=True)
        if skipped is not None:
            skipped.extend(shots[k:])
        if compiled is not None:
            compiled.set()
        return failed, []
    except Exception as exc:  # noqa: BLE001
        _crash(slot, f"boot: {exc}")
        if compiled is not None:
            compiled.set()
        return failed, shots[k:]
    finally:
        if game is not None:
            game.__exit__(None, None, None)
    return failed, []


def _free_slot(order: list[int], purpose: str, stack: ExitStack) -> locks.Slot | None:
    for i in order:
        try:
            return stack.enter_context(locks.slot(i, purpose, timeout=0, quiet=True))
        except TimeoutError:
            pass
    return None


CRASH_WHY: dict[int, str] = {}
RETRY_WAIT = 600.0


def _crash(slot: locks.Slot, why: str) -> None:
    CRASH_WHY[slot.index] = why
    print(f"RIG:CRASH slot{slot.index} {why}", flush=True)


def retry_slot(crashed: set[int]) -> int:
    """The crashed slot a retry runs on (the lowest, if several)."""
    return min(crashed)


def await_clear(s: locks.Slot, timeout: float, poll: float = 1.0) -> None:
    """Wait until nothing listens on the held slot's ports (the crashed gk is gone)."""
    end = time.monotonic() + timeout
    while s.busy_ports():
        if locks.reclaim_orphans(s):
            continue
        if time.monotonic() > end:
            raise TimeoutError(f"slot{s.index} ports {s.busy_ports()} still in use after {timeout:.0f}s")
        time.sleep(poll)


def reuse_vanilla(shots: list[Shot], out_dir: Path, toggles: dict | None, setup: list[str] | None,
                  force: bool = False) -> list[Shot]:
    """Copy an indexed vanilla frame to ``out_dir`` for each vanilla-look shot that has one at its
    size; returns the shots still to capture. ``force`` (--force-vanilla) reuses nothing."""
    if force:
        return shots
    idx, todo = vanilla.load_index(), []
    for sh in shots:
        meta = vanilla.shot_meta(sh) if vanilla.vanilla_run(toggles, setup, sh) else None
        hit = vanilla.lookup(meta, idx) if meta else None
        if not (hit and vanilla.reuse(hit, out_dir / f"{sh.name}.png", sh.name, stamp)):
            todo.append(sh)
    return todo


def run_shots(set_name: str, shots: list[Shot], out_dir: Path, parallel: int = 4,
              slot: int | None = None, bin_dir: Path | None = None,
              setup: list[str] | None = None, toggles: dict[str, str] | None = None,
              camera_strict: bool = True, max_seconds: float | None = None,
              ab: AbSpec | None = None, guard: PlayGuard | None = None,
              force_vanilla: bool = False) -> int:
    """Capture ``shots`` on up to ``parallel`` concurrent instances (capped by free slots),
    each booting once and taking whole levels. A crashed instance's remaining shots are
    retried once on the crashed slot. ``bin_dir``: binaries to run (default: stage the current
    build). ``setup``: GOAL forms sent once per instance before its first shot;
    ``toggles`` is recorded in every sidecar. ``camera_strict``: see :func:`run_worker`.
    ``max_seconds``: wall cap for the whole run; no shot starts after it (the one running
    finishes) and the summary lists the shots skipped. ``guard``: what to do while a play
    session runs (default: wait for it to end, bounded by the wall cap)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    t0 = time.monotonic()
    deadline = None if max_seconds is None else t0 + max_seconds
    skipped: list[Shot] = []
    if not ab:
        shots = reuse_vanilla(shots, out_dir, toggles, setup, force_vanilla)
        if not shots:
            return 0
    guard = guard or PlayGuard(say=stamp)
    try:
        guard.wait("the run", deadline)
        if deadline is not None and time.monotonic() >= deadline:
            raise PlaySessionActive("the wall cap passed while waiting for the play session")
    except PlaySessionActive as exc:
        print(f"RIG:PLAYING not starting: {exc}", flush=True)
        print(summary(shots, [], shots, out_dir, time.monotonic() - t0), flush=True)
        return 1
    bin_dir = bin_dir or engine.staged_bin()
    for d in engine.prune(also_keep=bin_dir):
        stamp(f"pruned staged binaries {d.name}")
    purpose = f"sandover-capture {set_name} ({len(shots)} shots) bin {bin_dir.name}"
    fresh = outputs_fresh()
    with ExitStack() as stack:
        slots: list[locks.Slot] = []
        for i in [slot] if slot is not None else range(locks.SLOTS):
            if len(slots) >= parallel:
                break
            s = _free_slot([i], purpose, stack)
            if s is not None:
                slots.append(s)
        if not slots:
            try:
                slots.append(stack.enter_context(locks.slot(slot, purpose, timeout=_left(deadline))))
            except TimeoutError as exc:
                print(f"RIG:SLOT-BUSY {exc}", flush=True)
                print(summary(shots, [], shots, out_dir, time.monotonic() - t0, "slot busy"), flush=True)
                return 1
        parts = partition(shots, len(slots))
        slots = slots[:len(parts)]
        compiled = None if fresh else threading.Event()
        stamp(f"{len(shots)} shots on slots {[s.index for s in slots]}, binaries {bin_dir.name}, "
              + ("out/ is current" if fresh else f"slot{slots[0].index} rewrites out/ first"))
        with ThreadPoolExecutor(len(parts)) as pool:
            futs = [pool.submit(run_worker, s, bin_dir, p, out_dir, not fresh and k == 0, compiled, k,
                                setup, toggles, camera_strict, deadline, skipped, ab, guard)
                    for k, (s, p) in enumerate(zip(slots, parts, strict=True))]
            results = [f.result() for f in futs]
        failed = [sh for f, _ in results for sh in f]
        retry = [sh for _, r in results for sh in r]
        crashed = {s.index for s, (_, r) in zip(slots, results, strict=True) if r}
        why = "wall cap or play session"
        if retry and deadline is not None and time.monotonic() > deadline:
            print(f"RIG:WALL-CAP not retrying {len(retry)} shots", flush=True)
            skipped += retry
            retry = []
        if retry:
            retry.sort(key=lambda sh: sh.index)
            stamp(f"retrying {len(retry)} shots once: {[sh.name for sh in retry]}")
            rs = retry_slot(crashed)
            held = next(s for s in slots if s.index == rs)
            other = None
            if slot is None:
                other = _free_slot([i for i in range(locks.SLOTS) if i not in {s.index for s in slots}],
                                   purpose + " retry", stack)
            target = other or held
            reason = CRASH_WHY.get(rs, "gk crash or failed boot")
            print(f"RIG:RETRY slot{target.index}: {len(retry)} shots after {reason}", flush=True)
            left = _left(deadline)
            wait = RETRY_WAIT if left is None else min(left, RETRY_WAIT)
            try:
                await_clear(target, wait)
            except TimeoutError as exc:
                print(f"RIG:SLOT-BUSY not retrying {len(retry)} shots: {exc}", flush=True)
                skipped += retry
                why = "wall cap, play session or slot busy"
            else:
                f2, r2 = run_worker(target, bin_dir, retry, out_dir, False, None, 0, setup, toggles,
                                    camera_strict, deadline, skipped, ab, guard)
                failed += f2 + r2
    print(summary(shots, failed, skipped, out_dir, time.monotonic() - t0, why), flush=True)
    if ab:
        ab_report(out_dir, ab)
    return 1 if failed or skipped else 0


def _left(deadline: float | None) -> float | None:
    """Seconds to the wall cap (None: no cap); waiting on a slot counts towards it."""
    return None if deadline is None else max(0.0, deadline - time.monotonic())


def summary(shots: list[Shot], failed: list[Shot], skipped: list[Shot], out_dir: Path,
            seconds: float, why: str = "wall cap or play session") -> str:
    """The run's last line: shots captured, failed and skipped (and ``why``)."""
    done = len(shots) - len(failed) - len(skipped)
    out = f"[{time.monotonic() - T0:7.1f}s] {done}/{len(shots)} shots in {out_dir} in {seconds:.0f}s"
    if failed:
        out += f"; failed: {[sh.name for sh in failed]}"
    if skipped:
        out += f"; skipped ({why}): {[sh.name for sh in skipped]}"
    return out


def compare(a: Path, b: Path, out: Path | None) -> int:
    """Mean and max FLIP (LDR) per shot for PNGs present in both dirs."""
    import flip_evaluator as flip
    import numpy as np

    rows = []
    for pa in sorted(a.glob("*.png")):
        pb = b / pa.name
        if not pb.exists():
            continue
        fmap, mean, _ = flip.evaluate(str(pa), str(pb), "LDR", applyMagma=False)
        fmap = np.asarray(fmap)
        rows.append({"shot": pa.stem, "mean": float(mean), "max": float(fmap.max()),
                     "px_over_0.1": float((fmap > 0.1).mean())})
    if not rows:
        print("no common PNGs")
        return 1
    print("| shot | mean FLIP | max FLIP | px > 0.1 |\n|---|---|---|---|")
    for r in rows:
        print(f"| {r['shot']} | {r['mean']:.6f} | {r['max']:.4f} | {100 * r['px_over_0.1']:.3f}% |")
    means = [r["mean"] for r in rows]
    print(f"\n{len(rows)} shots: mean of means {sum(means) / len(means):.6f}, "
          f"worst mean {max(means):.6f}, worst max {max(r['max'] for r in rows):.4f}")
    if out:
        out.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(prog="sandover-capture", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="capture every shot of a manifest")
    r.add_argument("manifest", type=Path)
    r.add_argument("--only", nargs="+", help="shot names to capture (manifest order is kept)")
    r.add_argument("--out-dir", type=Path, help="default: $SANDOVER_DATA/shots/<set>")
    r.add_argument("--settle-frames", type=int, help="override the manifest's settle_frames")
    r.add_argument("--slot", type=int, help="use only this game instance slot")
    r.add_argument("--parallel", type=int, default=4, help="concurrent game instances (default 4)")
    r.add_argument("--ab-mask", nargs=2, metavar=("A", "B"), type=lambda v: int(v, 0),
                   help="paused-frame A/B: each shot with perf-opt mask A, then the same paused "
                        "frame with mask B, into <out>/A and <out>/B, then pixdiff")
    r.add_argument("--ab-water-clock", type=float, default=100.0, metavar="SEC",
                   help="with --ab-mask: pin the wave clock here (negative: live; default 100)")
    o = sub.add_parser("one", help="capture a single shot")
    o.add_argument("--continue-point", default="village1-hut")
    o.add_argument("--hour", type=float, default=12.0)
    o.add_argument("--name")
    o.add_argument("--settle-frames", type=int, default=60)
    o.add_argument("--hide-jak", action="store_true")
    o.add_argument("--wind", action="store_true")
    o.add_argument("--effects", action="store_true")
    o.add_argument("--particles", action="store_true")
    o.add_argument("--out", type=Path, required=True)
    o.add_argument("--slot", type=int, help="game instance slot (default: first free)")
    fs = sub.add_parser("from-snapshot", help="re-shoot a play-session snapshot (F9 sidecar) with the rig")
    fs.add_argument("snapshot", type=Path, help="<run dir>/NNNN.json")
    fs.add_argument("--name", help="shot name (default <run>-NNNN)")
    fs.add_argument("--out-dir", type=Path, help="default: $SANDOVER_DATA/shots/from-snapshot")
    fs.add_argument("--no-toggles", action="store_true", help="skip the session's toggles (vanilla look)")
    fs.add_argument("--slot", type=int, help="game instance slot (default: first free)")
    for sp in (r, o, fs):
        sp.add_argument("--preset", action="append", default=[], metavar="PHASE=NAME",
                        help="apply a preset from sandover/presets/<phase>.toml (repeatable)")
        sp.add_argument("--goal", action="append", default=[], metavar="FORM",
                        help="extra GOAL form sent once per instance before the first shot "
                             "(a level load can reset effect state; see --goal-after-load)")
        sp.add_argument("--goal-after-load", action="append", default=[], metavar="FORM",
                        help="GOAL form sent in every shot once its level is loaded (repeatable)")
        sp.add_argument("--wait-log", metavar="REGEX",
                        help="before the settle frames, wait until a gk log line matches REGEX")
        sp.add_argument("--wait-vista", action="store_true",
                        help="before the settle frames, wait until no vista neighbour is still loading")
        sp.add_argument("--wait-log-timeout", type=float, metavar="SEC",
                        help="limit for --wait-log/--wait-vista, then shoot anyway (default 120)")
        sp.add_argument("--allow-camera-drift", action="store_true",
                        help="count a shot whose captured camera is off the requested one "
                             f"(> {CAMERA_TOL_M} m or {CAMERA_TOL_DEG} deg) as done; it is still reported")
        sp.add_argument("--bin", metavar="NAME",
                        help="staged binary copy under $SANDOVER_DATA/bin to run (default: current build)")
        sp.add_argument("--actors", choices=ACTORS,
                        help="hold enemies, platforms, crates and ambients after their first settle "
                             "frames (freeze; Jak invulnerable) or let them run (live). Default: "
                             + ("the manifest's, else freeze" if sp is r else
                                "freeze" if sp is fs else "live"))
        sp.add_argument("--force-vanilla", action="store_true",
                        help="shoot a vanilla-look frame even when the vanilla library has one")
        sp.add_argument("--play-look", action="store_true",
                        help="after each load, the game's default Sandover look (reset, reapply, "
                             "update in one message) before the other after-load forms")
        sp.add_argument("--cutscenes", action="store_true",
                        help="let cutscenes and scripted cameras play (default: the rig ends each "
                             "one as it starts)")
        sp.add_argument("--max-seconds", type=float, metavar="SEC",
                        help="wall cap for the whole run: no shot starts after it; summary lists the rest")
        sp.add_argument("--while-playing", choices=playguard.MODES, default="wait",
                        help="a play session (gk with config/play) is running before the run, an "
                             "instance start or a shot: wait for it to end (default; checks every "
                             f"{playguard.POLL_S:.0f} s, bounded by --max-seconds), fail, or ignore it. "
                             "Every sidecar records play_session")
        sp.add_argument("--max-play-wait", type=float, metavar="SEC",
                        help="with --while-playing wait: give up after waiting this long for one play "
                             "session to end; the rest are skipped (default: no limit but --max-seconds)")
    c = sub.add_parser("compare", help="FLIP between two capture dirs")
    c.add_argument("a", type=Path)
    c.add_argument("b", type=Path)
    c.add_argument("--json", type=Path)
    sub.add_parser("continues", help="list continue points (name, level)")
    pr = sub.add_parser("prune", help="delete old staged binary copies under $SANDOVER_DATA/bin")
    pr.add_argument("--keep", type=int, default=engine.KEEP, help="newest copies to keep")
    args = ap.parse_args()

    if args.cmd == "prune":
        removed = engine.prune(args.keep)
        for d in removed:
            print(f"removed {d.name}")
        left = sorted(d.name for d in (data_dir() / "bin").glob("*") if d.is_dir())
        print(f"{len(removed)} removed; kept {left}")
        return 0
    if args.cmd == "continues":
        for name, level in continue_points():
            print(f"{name}\t{level}")
        return 0
    if args.cmd == "compare":
        return compare(args.a, args.b, args.json)
    setup, toggles = [], {}
    for spec in getattr(args, "preset", []):
        phase, _, name = spec.partition("=")
        setup += presets.goal_forms(phase, name)
        toggles[phase] = name
    setup += getattr(args, "goal", [])
    if getattr(args, "goal", []):
        toggles["goal"] = list(args.goal)
    if getattr(args, "wait_log", None):
        try:
            re.compile(args.wait_log)
        except re.error as exc:
            ap.error(f"--wait-log: {exc}")
    post_args = (getattr(args, "goal_after_load", []), getattr(args, "wait_log", None),
                 getattr(args, "wait_vista", False), getattr(args, "wait_log_timeout", None))
    strict = not getattr(args, "allow_camera_drift", False)
    cap = args.max_seconds
    guard = PlayGuard(args.while_playing, args.max_play_wait, say=stamp)
    if args.play_look:
        toggles["play_look"] = True
    if args.cutscenes:
        toggles["cutscenes"] = True
    bin_dir = data_dir() / "bin" / args.bin if getattr(args, "bin", None) else None
    if bin_dir is not None and not (bin_dir / f"gk{EXE}").exists():
        ap.error(f"no gk in {bin_dir}")
    if bin_dir is not None and not os.environ.get("SANDOVER_GK_PROJ_PATH"):
        proj = bin_root(bin_dir)
        if proj is not None:
            os.environ["SANDOVER_GK_PROJ_PATH"] = str(proj)
            stamp(f"gk runs from the bin's project root {proj.name}")
    no_reuse = args.force_vanilla or not vanilla.reuse_allowed(getattr(args, "bin", None), env_overrides())
    if args.cmd == "from-snapshot":
        from sandover.rig import snapshot

        shot, forms = snapshot.shot_from_snapshot(args.snapshot, args.name)
        shot.actors = args.actors or "freeze"
        if args.no_toggles:
            forms = []
        toggles = {"snapshot": str(args.snapshot.name), **toggles}
        if forms:
            toggles["forms"] = " ".join(forms)
        setup = forms + setup + ([snapshot.stage_hook(forms)] if forms else [])
        out_dir = args.out_dir or data_dir() / "shots" / "from-snapshot"
        if args.play_look:
            apply_play_look([shot])
        shot.cutscenes = shot.cutscenes or args.cutscenes
        apply_post_load_args([shot], *post_args)
        return run_shots("from-snapshot", [shot], out_dir, 1, args.slot, bin_dir, setup, toggles, strict, cap,
                         guard=guard, force_vanilla=no_reuse)
    if args.cmd == "one":
        name = args.name or f"{args.continue_point}-h{args.hour:g}"
        shot = Shot(name=name, continue_point=args.continue_point, hour=args.hour, jak=not args.hide_jak,
                    wind=args.wind, effects=args.effects, particles=args.particles, settle_frames=args.settle_frames,
                    actors=args.actors or "live")
        if args.play_look:
            apply_play_look([shot])
        shot.cutscenes = shot.cutscenes or args.cutscenes
        apply_post_load_args([shot], *post_args)
        out_dir = args.out.parent
        rc = run_shots("one", [shot], out_dir, 1, args.slot, bin_dir, setup, toggles, strict, cap, guard=guard,
                       force_vanilla=no_reuse)
        produced = out_dir / f"{name}.png"
        if rc == 0 and produced != args.out:
            produced.replace(args.out)
            (out_dir / f"{name}.json").replace(args.out.with_suffix(".json"))
            if (out_dir / f"{name}.exr").exists():
                (out_dir / f"{name}.exr").replace(args.out.with_suffix(".exr"))
        return rc
    set_name, shots = load_manifest(args.manifest, actors="freeze")
    if args.actors:
        for s in shots:
            s.actors = args.actors
    if args.play_look:
        apply_play_look(shots)
    for s in shots:
        s.cutscenes = s.cutscenes or args.cutscenes
    if args.only:
        missing = set(args.only) - {s.name for s in shots}
        if missing:
            ap.error(f"not in manifest: {sorted(missing)}")
        shots = [s for s in shots if s.name in args.only]
    if args.settle_frames is not None:
        for s in shots:
            s.settle_frames = args.settle_frames
    apply_post_load_args(shots, *post_args)
    out_dir = args.out_dir or data_dir() / "shots" / set_name
    ab = None
    if args.ab_mask:
        clock = args.ab_water_clock if args.ab_water_clock >= 0 else None
        ab = AbSpec(args.ab_mask[0], args.ab_mask[1], clock)
        toggles["ab_mask"] = [f"{m:#x}" for m in args.ab_mask]
    return run_shots(set_name, shots, out_dir, args.parallel, args.slot, bin_dir, setup, toggles, strict, cap,
                     ab, guard, no_reuse)


if __name__ == "__main__":
    sys.exit(main())
