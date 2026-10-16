"""Keep rig captures away from a live play session.

The play launcher (``sandover/local/play.py``) runs gk with ``--config-path
$SANDOVER_DATA/config/play``; rig instances use ``config/slot<n>``. A capture taken while the
user plays shares the GPU (and, before the rig opened no gamepads, the pad) with the session.
:class:`PlayGuard` finds such a gk by its command line before a run, an instance start and each
shot, and waits for it to end, fails, or only records it (``--while-playing``). It only lists
processes: it never kills or signals the play session.
"""

from __future__ import annotations

import os
import re
import subprocess
import threading
import time
from collections.abc import Callable
from pathlib import Path

MODES = ("wait", "fail", "ignore")
POLL_S = 30.0
#: a wait this long stops the booted rig instance until the session ends
IDLE_S = 60.0
#: one listing serves every check within this many seconds (a shot's after-check and the next
#: shot's before-check, parallel instances)
CACHE_S = 5.0
CONFIG_ARG = re.compile(r"""--config-path(?:=|\s+)(?:"([^"]*)"|'([^']*)'|(\S+))""", re.IGNORECASE)
PS_LIST = ("[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
           "Get-CimInstance Win32_Process -Filter \"Name = 'gk.exe'\" | "
           "ForEach-Object { \"$($_.ProcessId)`t$($_.CommandLine)\" }")


class PlaySessionActive(RuntimeError):
    """A play session is running and the guard will not capture (``fail``, or ``wait`` past
    ``--max-play-wait``)."""


def config_path(cmdline: str) -> str | None:
    """The ``--config-path`` value of a gk command line (quoted, ``=`` or space form), else None."""
    m = CONFIG_ARG.search(cmdline or "")
    return next((g for g in m.groups() if g is not None), None) if m else None


def is_play_config(path: str) -> bool:
    """True when ``path`` ends in ``config`` + separator + ``play`` (either slash, any case)."""
    parts = [p for p in re.split(r"[\\/]+", path.strip().strip("\"'")) if p]
    return len(parts) >= 2 and parts[-2].lower() == "config" and parts[-1].lower() == "play"


def is_play_session(cmdline: str) -> bool:
    path = config_path(cmdline)
    return path is not None and is_play_config(path)


def gk_processes() -> list[tuple[int, str]] | None:
    """(pid, command line) of every running gk, or None when the processes cannot be listed."""
    if os.name == "nt":
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", PS_LIST],
                               capture_output=True, timeout=60,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except (OSError, subprocess.TimeoutExpired):
            return None
        if r.returncode != 0:
            return None
        out = []
        for line in r.stdout.decode("utf-8", "replace").splitlines():
            pid, _, cmd = line.partition("\t")
            if pid.strip().isdigit():
                out.append((int(pid), cmd.strip()))
        return out
    proc = Path("/proc")
    if not proc.is_dir():
        return None
    out = []
    for d in proc.iterdir():
        if not d.name.isdigit():
            continue
        try:
            argv = [a.decode("utf-8", "replace") for a in (d / "cmdline").read_bytes().split(b"\0") if a]
        except OSError:
            continue
        if argv and Path(argv[0]).name == "gk":
            out.append((int(d.name), " ".join(f'"{a}"' if " " in a else a for a in argv)))
    return out


class PlayGuard:
    """``--while-playing``: ``wait`` (poll every ``poll`` s until no play session runs, at most
    ``max_wait`` s per wait and never past the run's wall cap), ``fail`` (raise
    :class:`PlaySessionActive`) or ``ignore`` (only record it)."""

    def __init__(self, mode: str = "wait", max_wait: float | None = None, poll: float = POLL_S,
                 lister: Callable[[], list[tuple[int, str]] | None] = gk_processes,
                 clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep,
                 say: Callable[[str], None] = lambda m: print(m, flush=True),
                 cache_s: float = CACHE_S):
        if mode not in MODES:
            raise ValueError(f"--while-playing must be one of {MODES}, not {mode!r}")
        self.mode, self.max_wait, self.poll = mode, max_wait, poll
        self.lister, self.clock, self.sleep, self.say, self.cache_s = lister, clock, sleep, say, cache_s
        self._lock = threading.Lock()
        self._cached: tuple[float, list[int] | None] | None = None
        self._warned = False

    def sessions(self, fresh: bool = False) -> list[int] | None:
        """PIDs of running play-session gk processes; None when they cannot be listed, or when
        none matches but a gk's command line is unreadable (an elevated gk hides it)."""
        with self._lock:
            now = self.clock()
            if not fresh and self._cached and now - self._cached[0] < self.cache_s:
                return self._cached[1]
            procs = self.lister()
            pids = None if procs is None else [pid for pid, cmd in procs if is_play_session(cmd)]
            if pids == [] and any(not cmd.strip() for _, cmd in procs):
                pids = None
            self._cached = (now, pids)
            if pids is None and not self._warned:
                self._warned = True
                self.say("RIG:PLAY-GUARD cannot list gk processes; a play session would go unnoticed")
            return pids

    def check(self) -> bool | None:
        """Is a play session running now (None: unknown)."""
        pids = self.sessions()
        return None if pids is None else bool(pids)

    def wait(self, what: str, deadline: float | None = None,
             idle: Callable[[], None] | None = None, idle_after: float = IDLE_S) -> dict:
        """Before ``what`` (the run, an instance start, a shot). Returns ``detected`` (a play
        session was running when this returned; None: unknown) and ``waited_s``. ``wait`` gives
        up at ``deadline`` (the run's wall cap, ``clock()`` time) and returns detected, so the
        caller's wall-cap path skips the rest. A listing that fails during a wait counts as still
        playing. ``idle`` is called once when a wait has lasted ``idle_after`` s (the caller stops
        its booted instance)."""
        pids = self.sessions()
        if not pids:
            return {"detected": None if pids is None else False, "waited_s": 0.0}
        if self.mode == "ignore":
            return {"detected": True, "waited_s": 0.0}
        if self.mode == "fail":
            raise PlaySessionActive(f"a play session is running (gk pid {', '.join(map(str, pids))}) "
                                    f"before {what}; --while-playing fail")
        t0 = self.clock()
        limit = None if self.max_wait is None else t0 + self.max_wait
        self.say(f"RIG:PLAY-WAIT {what}: a play session is running (gk pid "
                 f"{', '.join(map(str, pids))}); checking every {self.poll:.0f}s")
        while pids is None or pids:
            now = self.clock()
            if idle is not None and now - t0 >= idle_after:
                idle()
                idle = None
            if limit is not None and now >= limit and (deadline is None or limit <= deadline):
                raise PlaySessionActive(f"a play session was still running after {now - t0:.0f}s "
                                        f"(--max-play-wait) before {what}")
            if deadline is not None and now >= deadline:
                self.say(f"RIG:PLAY-WAIT {what}: wall cap reached after {now - t0:.0f}s, still playing")
                return {"detected": True, "waited_s": round(now - t0, 1)}
            ends = [t - now for t in (deadline, limit) if t is not None]
            self.sleep(max(0.0, min([self.poll, *ends])))
            pids = self.sessions(fresh=True)
        waited = self.clock() - t0
        self.say(f"RIG:PLAY-RESUME {what} after {waited:.0f}s")
        return {"detected": False, "waited_s": round(waited, 1)}


def sidecar(before: dict, after: bool | None) -> dict:
    """``play_session`` sidecar field: ``detected`` when a play session ran right before or right
    after the shot (None: the processes could not be listed, False: none seen)."""
    seen = (before["detected"], after)
    detected = True if True in seen else None if None in seen else False
    return {"detected": detected, "before": before["detected"], "after": after,
            "waited_s": before["waited_s"]}
