"""Resource locks shared by sandover tools.

- ``build``: one ninja build at a time (a running game does not block a build).
- ``extract``: one extraction at a time.
- ``compile``: one goalc ``(mi)`` at a time; it rewrites ``out/jak1`` for every instance.
- game instance slots ``slot0..slot<N-1>``: several games may run at once, each on its own
  ports and config dir (:class:`Slot`). ``game`` is the legacy lock a hand-run instance on
  the default ports takes; it blocks slot 0.

A lock is the file ``$SANDOVER_DATA/locks/<name>.lock``; it exists exactly while someone
holds it. Tools take it through :func:`hold`, which uses ``filelock.SoftFileLock`` (the file
holds the owner's PID record) and writes a human-readable ``<name>.purpose`` next to it.
Anyone working by hand creates ``<name>.lock`` with a purpose and time inside and deletes
it when done; :func:`hold` waits for such a file to disappear before taking the lock.

Usage::

    from sandover import locks
    with locks.hold(locks.BUILD, "rebuild gk"):
        ...
    with locks.slot(purpose="capture auto-v0") as s:   # auto-picks a free slot
        print(s.game_port, s.nrepl_port, s.config_dir)
"""

from __future__ import annotations

import os
import re
import signal
import socket
import subprocess
import sys
import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

from filelock import SoftFileLock

from sandover.paths import data_dir

BUILD = "build"
EXTRACT = "extract"
COMPILE = "compile"
GAME = "game"
SLOTS = 8
PORT_STRIDE = 10
GAME_PORT, NREPL_PORT, DEBUG_PORT = 8112, 8181, 8128


def lock_path(name: str) -> Path:
    return data_dir() / "locks" / f"{name}.lock"


def holder(name: str) -> str | None:
    """Describe the current holder of lock ``name``, or None if it is free."""
    path = lock_path(name)
    if not path.exists():
        return None
    for p in (path.with_suffix(".purpose"), path):
        try:
            text = p.read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            continue
        if text:
            return " | ".join(text.splitlines())
    return "(no purpose recorded)"


def try_acquire(name: str) -> SoftFileLock | None:
    path = lock_path(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    # a hand-made lock has no PID record, and filelock may treat it as stale: never touch it
    if path.exists() and not _pid_record(path):
        return None
    _break_dead(name, path)
    lock = SoftFileLock(path, timeout=0)
    # a failed attempt breaks a crashed owner's record, so the second one can succeed
    for _ in range(2):
        try:
            lock.acquire()
            return lock
        except TimeoutError:
            pass
    return None


def _pid_record(path: Path) -> bool:
    """True if ``path`` holds a filelock PID record (pid, host), not a hand-made lock."""
    try:
        lines = path.read_text(encoding="utf-8").strip().splitlines()
    except (OSError, UnicodeDecodeError):
        return False
    return len(lines) in (2, 3) and lines[0].isdigit()


def pid_running(pid: int) -> bool:
    """Is process ``pid`` running. On Windows an exited process stays openable while anyone holds
    a handle to it, and filelock's own check counts that as alive; this reads its exit code."""
    if os.name == "nt":
        import ctypes

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.OpenProcess.restype = ctypes.c_void_p
        h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return ctypes.get_last_error() != 87  # ERROR_INVALID_PARAMETER: no such process
        try:
            code = ctypes.c_ulong()
            if not k32.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code)):
                return True
            return code.value == 259  # STILL_ACTIVE
        finally:
            k32.CloseHandle(ctypes.c_void_p(h))
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _host() -> str:
    try:
        from filelock._identity import host_name
    except ImportError:  # pragma: no cover
        return socket.gethostname()
    return host_name()


def _break_dead(name: str, path: Path) -> None:
    """Remove ``path`` when its PID record names a process of this host that is not running."""
    try:
        st = path.stat()
        lines = path.read_text(encoding="utf-8").strip().splitlines()
    except (OSError, UnicodeDecodeError):
        return
    if len(lines) not in (2, 3) or not lines[0].isdigit() or lines[1] != _host() or pid_running(int(lines[0])):
        return
    try:
        from filelock._util import break_lock_file
    except ImportError:  # older filelock: leave it to filelock's own stale check
        return
    try:
        break_lock_file(str(path), st.st_mtime, st.st_ino)
    except OSError:
        return
    print(f"reclaimed {name} lock: holder pid {lines[0]} is not running", file=sys.stderr, flush=True)


def purpose_pid(name: str) -> int | None:
    """The ``pid N`` line of ``<name>.purpose`` (written by :func:`held`), else None."""
    try:
        text = lock_path(name).with_suffix(".purpose").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    m = re.search(r"^pid (\d+)\s*$", text, re.MULTILINE)
    return int(m.group(1)) if m else None


def port_listeners(ports: list[int]) -> dict[int, int]:
    """{port: pid} of local TCP listeners on ``ports`` (Windows ``netstat``; empty elsewhere)."""
    if os.name != "nt":
        return {}
    try:
        out = subprocess.run(["netstat", "-ano", "-p", "TCP"], capture_output=True, text=True,
                             timeout=30, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {}
    return parse_netstat(out, ports)


def parse_netstat(out: str, ports: list[int]) -> dict[int, int]:
    found = {}
    for m in re.finditer(r"^\s*TCP\s+\S+:(\d+)\s+\S+\s+LISTENING\s+(\d+)\s*$", out, re.MULTILINE):
        if int(m.group(1)) in ports:
            found[int(m.group(1))] = int(m.group(2))
    return found


def process_info(pid: int) -> tuple[str, int, str] | None:
    """(image name, parent pid, command line) of a running process, else None."""
    if os.name == "nt":
        cmd = (f"Get-CimInstance Win32_Process -Filter 'ProcessId = {int(pid)}' | "
               "ForEach-Object { \"$($_.Name)`t$($_.ParentProcessId)`t$($_.CommandLine)\" }")
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd],
                                 capture_output=True, text=True, timeout=60,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            return None
        name, ppid, cmdline = (out.split("\t", 2) + ["", ""])[:3]
        return (name, int(ppid), cmdline) if ppid.strip().isdigit() else None
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
        cmdline = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode("utf-8", "replace")
    except OSError:
        return None
    return stat[stat.index("(") + 1:stat.rindex(")")], int(stat[stat.rindex(")") + 1:].split()[1]), cmdline


#: what a slot's holder starts; only these are stopped when their holder is dead
RIG_PROCESSES = ("gk", "gk.exe", "goalc", "goalc.exe")


def _arg(cmdline: str, flag: str) -> str | None:
    m = re.search(rf"(?<![\w-]){re.escape(flag)}(?:=|\s+)(?:\"([^\"]*)\"|(\S+))", cmdline)
    return (m.group(1) if m.group(1) is not None else m.group(2)) if m else None


def _same_path(a: str, b: Path | str) -> bool:
    def norm(p) -> str:
        return os.path.normcase(os.path.normpath(str(p).strip().strip("\"'")))
    return norm(a) == norm(b)


def rig_launch_line(s: Slot, name: str, cmdline: str) -> bool:
    """True when ``cmdline`` is how the rig starts ``name`` on slot ``s``: gk with that slot's
    ``--config-path``, goalc with its ``--port`` and ``--debug-port``. Empty or unreadable: False."""
    if not cmdline or not cmdline.strip():
        return False
    stem = name.lower().removesuffix(".exe")
    if stem == "gk":
        cfg = _arg(cmdline, "--config-path")
        return cfg is not None and _same_path(cfg, s.config_dir)
    if stem == "goalc":
        return (_arg(cmdline, "--port") == str(s.nrepl_port)
                and _arg(cmdline, "--debug-port") == str(s.debug_port))
    return False


def reclaim_orphans(s: Slot, kill=None) -> list[int]:
    """Stop the ``gk``/``goalc`` processes a dead slot holder left listening on ``s``'s ports:
    the holder named in ``slot<n>.purpose`` is not running, is their parent, and their command
    line is the rig's launch line for this slot (:func:`rig_launch_line`). Anything else on the
    ports (another holder's processes, a hand-run or build goalc, a play session's gk whose
    launcher died, a process whose command line cannot be read) is left alone. Returns the pids."""
    from sandover.rig.playguard import is_play_session

    dead = purpose_pid(f"slot{s.index}")
    if dead is None or pid_running(dead):
        return []
    kill = kill or (lambda pid: os.kill(pid, signal.SIGTERM))
    stopped: list[int] = []
    for port, pid in sorted(port_listeners(s.ports).items()):
        info = process_info(pid)
        if (pid in stopped or not info or info[0].lower() not in RIG_PROCESSES or info[1] != dead
                or is_play_session(info[2]) or not rig_launch_line(s, info[0], info[2])):
            continue
        print(f"reclaimed slot{s.index}: holder pid {dead} is not running; stopping its {info[0]} "
              f"pid {pid} on port {port}", file=sys.stderr, flush=True)
        try:
            kill(pid)
        except OSError:
            continue
        stopped.append(pid)
    return stopped


#: one tab-separated line per finished hold: start, lock, seconds held, pid, purpose
HOLD_LOG = "holds.log"
#: holds longer than this print a warning on release
LONG_HOLD_S = 10 * 60


def _log_hold(name: str, start: datetime, seconds: float, purpose: str) -> None:
    # runs in held()'s finally: a logging failure must never fail the holder
    try:
        line = "\t".join([start.isoformat(timespec="seconds"), name, f"{seconds:.0f}", str(os.getpid()),
                          purpose.splitlines()[0].replace("\t", " ") if purpose else ""])
        with open(data_dir() / "locks" / HOLD_LOG, "a", encoding="utf-8", errors="replace") as f:
            f.write(line + "\n")
    except Exception:
        pass
    if seconds > LONG_HOLD_S:
        print(f"{name} lock held {seconds / 60:.0f} min: {purpose}", file=sys.stderr, flush=True)


def hold_log(min_s: float = 0) -> list[dict]:
    """Finished holds from the hold log, oldest first, those of at least ``min_s`` seconds."""
    try:
        lines = (data_dir() / "locks" / HOLD_LOG).read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    out = []
    for line in lines:
        f = line.split("\t")
        try:
            e = {"start": f[0], "lock": f[1], "seconds": float(f[2]), "pid": int(f[3]), "purpose": f[4]}
        except (IndexError, ValueError):
            continue  # a line cut short by a crash mid-write
        if len(f) == 5 and e["seconds"] >= min_s:
            out.append(e)
    return out


@contextmanager
def held(name: str, lock: SoftFileLock, purpose: str) -> Iterator[Path]:
    path = lock_path(name)
    info = path.with_suffix(".purpose")
    start = datetime.now(UTC)
    info.write_text(f"{purpose}\n{start.isoformat(timespec='seconds')}\npid {os.getpid()}\n", encoding="utf-8")
    t0 = time.monotonic()
    try:
        yield path
    finally:
        info.unlink(missing_ok=True)
        lock.release()
        _log_hold(name, start, time.monotonic() - t0, purpose)


@contextmanager
def hold(name: str, purpose: str, timeout: float | None = None, poll: float = 2.0,
         repeat: float | None = None) -> Iterator[Path]:
    """Hold lock ``name`` for the duration of the block, waiting up to ``timeout`` seconds
    (forever if None). Prints the holder once while waiting, or every ``repeat`` seconds with
    the elapsed time."""
    t0 = time.monotonic()
    deadline = None if timeout is None else t0 + timeout
    announced: float | None = None
    while (lock := try_acquire(name)) is None:
        now = time.monotonic()
        if announced is None or (repeat is not None and now - announced >= repeat):
            e = int(now - t0)
            waited = f" (waited {e // 60}m{e % 60:02d}s)" if repeat is not None else ""
            print(f"waiting for {name} lock{waited}: {holder(name)}", file=sys.stderr, flush=True)
            announced = now
        if deadline is not None and time.monotonic() > deadline:
            raise TimeoutError(f"{name} lock still held: {holder(name)}")
        time.sleep(poll)
    with held(name, lock, purpose) as path:
        yield path


@dataclass(frozen=True)
class Slot:
    """Ports and directories of game instance slot ``index``."""

    index: int

    @property
    def game_port(self) -> int:
        return GAME_PORT + PORT_STRIDE * self.index

    @property
    def nrepl_port(self) -> int:
        return NREPL_PORT + PORT_STRIDE * self.index

    @property
    def debug_port(self) -> int:
        return DEBUG_PORT + PORT_STRIDE * self.index

    @property
    def config_dir(self) -> Path:
        return data_dir() / "config" / f"slot{self.index}"

    @property
    def logs_dir(self) -> Path:
        return data_dir() / "logs" / f"slot{self.index}"

    @property
    def ports(self) -> list[int]:
        return [self.game_port, self.nrepl_port, self.debug_port]

    def busy_ports(self) -> list[int]:
        busy = []
        for port in self.ports:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                if s.connect_ex(("127.0.0.1", port)) == 0:
                    busy.append(port)
        return busy

    def ports_free(self) -> bool:
        return not self.busy_ports()


@contextmanager
def slot(index: int | None = None, purpose: str = "", count: int = SLOTS,
         timeout: float | None = None, poll: float = 5.0, quiet: bool = False) -> Iterator[Slot]:
    """Hold a game instance slot: ``index`` if given, else the first free of ``count``.
    A slot is free when its lock is free and nothing listens on its ports; slot 0 is also
    busy while the legacy ``game`` lock exists. A lock whose holder is not running is taken
    over at once, and the gk/goalc that holder left on the slot's ports are stopped
    (:func:`reclaim_orphans`)."""
    candidates = [index] if index is not None else list(range(count))
    deadline = None if timeout is None else time.monotonic() + timeout
    announced = False
    why: dict[int, str | None] = {}
    while True:
        for i in candidates:
            s = Slot(i)
            if i == 0 and lock_path(GAME).exists():
                why[i] = "legacy game lock"
                continue
            lock = try_acquire(f"slot{i}")
            if lock is None:
                why[i] = holder(f"slot{i}") or "lock taken"
                continue
            busy = s.busy_ports()
            if busy and reclaim_orphans(s):
                for _ in range(20):
                    busy = s.busy_ports()
                    if not busy:
                        break
                    time.sleep(0.5)
            if busy:
                lock.release()
                why[i] = f"ports {busy} in use"
                continue
            with held(f"slot{i}", lock, purpose):
                yield s
            return
        if not announced and not quiet:
            print(f"waiting for a free game slot: { {f'slot{i}': why.get(i) for i in candidates} }",
                  file=sys.stderr, flush=True)
            announced = True
        if deadline is not None and time.monotonic() > deadline:
            raise TimeoutError(f"no free game slot among {candidates}: "
                               + ", ".join(f"slot{i} {why.get(i)}" for i in candidates))
        time.sleep(poll)
