"""Per-build copies of the engine binaries, so a rebuild never fights a running game.

:func:`staged_bin` copies ``gk``, ``goalc`` and the shared libraries next to them from
``$SANDOVER_ROOT/out/build/Release/bin`` to ``$SANDOVER_DATA/bin/<build time>-<hash>/`` and
returns that directory. Game data, shaders and ``goal_src`` are still found through
``OPENGOAL_PROJ_PATH``. Each copy carries ``build.json`` with the engine commit the checkout
held at staging time (:func:`source_state`). :func:`prune` keeps the newest :data:`KEEP` copies plus any copy a
live slot lock names (slot holders put ``bin <dir name>`` in their purpose).
:func:`ensure_fresh` rebuilds ``gk``/``goalc`` before staging when a source file is newer
than ``gk`` (the play launcher uses it; rig captures do not).
``python -m sandover.engine build <worktree>`` configures (seeded, ccache) and builds a worktree
in a free build lane (:func:`build_worktree`).
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import deque
from datetime import UTC, datetime
from pathlib import Path

from sandover import locks
from sandover.paths import data_dir, root

EXE = ".exe" if os.name == "nt" else ""
LIB = ".dll" if os.name == "nt" else ".so"
DONE = ".complete"
BUILD_INFO = "build.json"
KEEP = 3
# checkout paths compiled into gk/goalc; goal_src is compiled by goalc at run time
ENGINE_SRC = ("CMakeLists.txt", "common", "decompiler", "game", "goalc", "third-party")


def build_bin() -> Path:
    return root() / "out" / "build" / "Release" / "bin"


def _files(src: Path) -> list[Path]:
    return sorted([src / f"gk{EXE}", src / f"goalc{EXE}", *src.glob(f"*{LIB}")])


def _git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(root()), *args], capture_output=True, text=True,
                          check=True).stdout.strip()


def source_state(gk: Path) -> dict:
    """Engine commit of the checkout, trusted as the source of ``gk`` only if no engine file
    is modified or newer than ``gk``. ``engine_sha`` is None (with ``reason``) otherwise."""
    try:
        head = _git("rev-parse", "--short", "HEAD")
        sha = _git("log", "-1", "--format=%h", "HEAD", "--", ".", ":(exclude)sandover")
        dirty = _git("status", "--porcelain", "--untracked-files=no", "--", *ENGINE_SRC)
        tracked = _git("ls-files", "--", *ENGINE_SRC).splitlines()
    except (OSError, subprocess.CalledProcessError):
        return {"engine_sha": None, "head": None, "reason": "git unavailable"}
    info = {"engine_sha": sha, "head": head, "reason": ""}
    if dirty:
        info.update(engine_sha=None, reason=f"uncommitted engine changes ({dirty.splitlines()[0].strip()})")
        return info
    newer = _newer_than(gk, tracked)
    if newer:
        info.update(engine_sha=None, reason=f"{newer} is newer than the build; rebuild first")
    return info


def _newer_than(gk: Path, tracked: list[str]) -> str | None:
    built = gk.stat().st_mtime
    for f in tracked:
        try:
            if (root() / f).stat().st_mtime > built:
                return f
        except OSError:
            continue
    return None


def stale_reason(gk: Path) -> str | None:
    """Why ``gk`` is older than its sources, or None if it is not. Only the mtime test counts,
    so uncommitted engine edits that are already built in (``gk`` newer) are not stale."""
    if not gk.exists():
        return f"{gk.name} has not been built"
    try:
        tracked = _git("ls-files", "--", *ENGINE_SRC).splitlines()
    except (OSError, subprocess.CalledProcessError):
        return None
    newer = _newer_than(gk, tracked)
    return f"{newer} is newer than {gk.name}" if newer else None


class StaleBuild(RuntimeError):
    """The build is older than its sources and could not be rebuilt."""


def build_jobs() -> int:
    """Build parallelism: logical threads - 2, at least 1."""
    return max(1, (os.cpu_count() or 1) - 2)


def real_exe(name: str) -> str | None:
    """``name`` on PATH, with a scoop shim resolved to the exe it launches: each shim call
    costs a second process spawn (~0.1 s here), paid per try-compile and per compile."""
    found = shutil.which(name)
    if found is None:
        return None
    shim = Path(found).with_suffix(".shim")
    if shim.exists():
        text = shim.read_text(encoding="utf-8", errors="replace")
        m = re.search(r'^path\s*=\s*"?([^"\r\n]+)"?', text, re.M)
        # a shim that adds arguments must stay in the call
        if m and Path(m.group(1)).exists() and not re.search(r"^args\s*=", text, re.M):
            return m.group(1)
    return found


def configure_cmd(src: Path, launcher: bool = True) -> list[str]:
    """Configure ``src`` with the project preset, the configure seed (cached try-compile
    results, :mod:`sandover.buildseed`) and ccache as the compiler launcher when installed.
    Tool paths are resolved at run time, never stored."""
    from sandover import buildseed
    cmd = ["cmake"]
    seed = buildseed.seed_path(src)
    # the seed holds Release clang results; CACHE INTERNAL would overwrite an existing tree's own
    if seed.exists() and not (src / "out" / "build" / "Release" / "CMakeCache.txt").exists():
        cmd += ["-C", str(seed)]
    cmd.append(f"--preset={buildseed.PRESET}")
    ninja = real_exe("ninja")
    if ninja:
        cmd.append(f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}")
    ccache = real_exe("ccache") if launcher else None
    if ccache:
        cc = Path(ccache).as_posix()
        cmd += [f"-DCMAKE_C_COMPILER_LAUNCHER={cc}", f"-DCMAKE_CXX_COMPILER_LAUNCHER={cc}"]
    return cmd


def build_cmd() -> list[str]:
    return ["cmake", "--build", str(root() / "out" / "build" / "Release"),
            "--target", "gk", "goalc", "-j", str(build_jobs())]


def _run_build(cmd: list[str]) -> tuple[int, list[str]]:
    """Run ``cmd`` echoing its output; returns the exit code and the last 40 lines."""
    tail: deque[str] = deque(maxlen=40)
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             encoding="utf-8", errors="replace")
    except OSError as exc:
        return 127, [f"{cmd[0]}: {exc}"]
    for line in p.stdout:
        tail.append(line.rstrip())
        print(tail[-1], flush=True)
    return p.wait(), list(tail)


def ensure_fresh(rebuild: bool = True, say=lambda m: print(m, file=sys.stderr, flush=True),
                 purpose: str = "rebuild stale gk/goalc") -> str | None:
    """Rebuild ``gk`` and ``goalc`` under the build lock if a tracked engine file is newer
    than ``gk`` (:func:`stale_reason`). Returns the stale reason it handled, or None if the
    build was fresh. Raises :class:`StaleBuild` if the rebuild fails. With ``rebuild=False``
    it only warns. Shaders load from the checkout at run time, so a stale ``gk`` can run new
    shaders that expect uniforms only the new C++ sets."""
    gk = build_bin() / f"gk{EXE}"
    reason = stale_reason(gk)
    if reason is None:
        return None
    if not rebuild:
        say(f"WARNING: STALE BUILD ({reason}); launching it anyway. Shaders load from the "
            "checkout and may expect uniforms this binary never sets (e.g. black actors).")
        return reason
    cmd = build_cmd()
    say(f"build is stale ({reason}); rebuilding gk and goalc first: {' '.join(cmd)}")
    with locks.hold(locks.BUILD, purpose):
        rc, tail = _run_build(cmd)
    if rc != 0:
        raise StaleBuild(f"rebuild failed (exit {rc}); not launching a stale build ({reason}).\n"
                         "Last build output:\n" + "\n".join(tail))
    after = stale_reason(gk)
    say(f"rebuild done; ninja is up to date although {after}" if after else "rebuild done")
    return reason


def _write_info(dest: Path, src: Path) -> None:
    info = {"schema": 1, **source_state(src / f"gk{EXE}"),
            "staged": datetime.now(UTC).isoformat(timespec="seconds")}
    (dest / BUILD_INFO).write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")


def build_info(bin_dir: Path) -> dict | None:
    """``build.json`` of a staged copy, or None for copies staged before it existed."""
    p = bin_dir / BUILD_INFO
    return json.loads(p.read_text(encoding="utf-8")) if p.exists() else None


def _stage(src: Path) -> Path:
    files = _files(src)
    h = hashlib.blake2b(digest_size=5)
    for f in files:
        h.update(f.name.encode())
        h.update(f.read_bytes())
    built = datetime.fromtimestamp((src / f"gk{EXE}").stat().st_mtime)
    dest = data_dir() / "bin" / f"{built:%Y%m%d-%H%M%S}-{h.hexdigest()}"
    if (dest / DONE).exists():
        if not (dest / BUILD_INFO).exists():
            _write_info(dest, src)  # same bytes and mtime as the current build
        return dest
    tmp = dest.with_name(dest.name + f".tmp{os.getpid()}")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    for f in files:
        shutil.copy2(f, tmp / f.name)
    _write_info(tmp, src)
    (tmp / DONE).write_text(str(src) + "\n", encoding="utf-8")
    try:
        tmp.rename(dest)
    except OSError:
        shutil.rmtree(tmp, ignore_errors=True)
        if not (dest / DONE).exists():
            raise
    return dest


_STAMPED = re.compile(r"^\d{8}-\d{6}-[0-9a-f]+$")


def latest_staged() -> Path | None:
    """Newest auto-staged build; hand-named stagings (other branches) never count."""
    dirs = [d for d in (data_dir() / "bin").glob("*") if (d / DONE).exists() and _STAMPED.match(d.name)]
    return max(dirs, key=lambda d: d.name, default=None)


def staged_bin(wait_for_build: bool = False) -> Path:
    """Directory holding a copy of the current build's gk/goalc. While a build holds the
    build lock, returns the newest existing copy instead (or waits if there is none or
    ``wait_for_build``)."""
    src = build_bin()
    lock = None if wait_for_build else locks.try_acquire(locks.BUILD)
    if lock is None:
        prev = latest_staged()
        if prev is not None and not wait_for_build:
            print(f"build in progress ({locks.holder(locks.BUILD)}); using {prev.name}", file=sys.stderr)
            return prev
        with locks.hold(locks.BUILD, "stage engine binaries"):
            return _stage(src)
    with locks.held(locks.BUILD, lock, "stage engine binaries"):
        return _stage(src)


def in_use() -> set[str]:
    """Staged dir names referenced by live slot locks."""
    names = set()
    for purpose in (data_dir() / "locks").glob("slot*.purpose"):
        if not purpose.with_suffix(".lock").exists():
            continue
        text = purpose.read_text(encoding="utf-8", errors="replace")
        names.update(re.findall(r"\bbin (\S+)", text))
    return names


def _busy(d: Path) -> bool:
    """True if a file in ``d`` cannot be opened for writing (a running exe or loaded library on Windows)."""
    for f in d.iterdir():
        try:
            with open(f, "r+b"):
                pass
        except PermissionError:
            return True
    return False


def prune(keep: int = KEEP, also_keep: Path | None = None) -> list[Path]:
    """Delete auto-staged copies (``<build time>-<hash>``) other than the newest ``keep``,
    ``also_keep`` and those in use. Hand-named copies (``perf-base``, other branches) are never
    touched. Returns the removed dirs; a copy that cannot be removed (still running) is skipped."""
    root_dir = data_dir() / "bin"
    dirs = sorted((d for d in root_dir.glob("*") if d.is_dir() and _STAMPED.match(d.name)),
                  key=lambda d: d.name, reverse=True)
    staged = [d for d in dirs if (d / DONE).exists()]
    protected = {d.name for d in staged[:keep]} | in_use()
    if also_keep is not None:
        protected.add(also_keep.name)
    removed = []
    for d in staged:
        if d.name in protected:
            continue
        # rmtree would delete .complete and stop at the running exe, leaving a broken copy
        if _busy(d):
            print(f"prune: kept {d.name} (in use)", file=sys.stderr)
            continue
        try:
            shutil.rmtree(d)
        except OSError as exc:
            print(f"prune: kept {d.name} ({exc.strerror})", file=sys.stderr)
            continue
        removed.append(d)
    return removed


# --- worktree builds -------------------------------------------------------------------

#: build lanes; ``build-b`` only with :data:`LANE_B_FREE_GB` of free memory (two builds at once)
LANES = ("build", "build-b")
LANE_B_FREE_GB = 10


def free_gb() -> float:
    if os.name != "nt":
        return float(os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")) / 2**30
    import ctypes

    class MEM(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    *((f"x{i}", ctypes.c_ulonglong) for i in range(5))]
    m = MEM()
    m.dwLength = ctypes.sizeof(MEM)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ullAvailPhys / 2**30


def take_lane(say=lambda m: print(m, file=sys.stderr, flush=True), poll: float = 5.0):
    """Wait for a free build lane; returns (name, lock)."""
    announced = False
    while True:
        for name in LANES:
            if name != LANES[0] and free_gb() < LANE_B_FREE_GB:
                continue
            lock = locks.try_acquire(name)
            if lock is not None:
                return name, lock
        if not announced:
            say("waiting for a build lane: " + " / ".join(str(locks.holder(n)) for n in LANES))
            announced = True
        time.sleep(poll)


def build_worktree(wt: Path, targets: list[str], say=lambda m: print(m, file=sys.stderr, flush=True)) -> int:
    """Configure ``wt`` if it has no build tree yet (:func:`configure_cmd`: seed + ccache), then
    ninja ``targets`` in a free lane. Returns the exit code."""
    out = wt / "out" / "build" / "Release"
    lane, lock = take_lane(say)
    with locks.held(lane, lock, f"{wt.name} [{lane}]: configure if new + ninja {' '.join(targets)}"):
        if not (out / "build.ninja").exists():
            cmd = configure_cmd(wt)
            say(" ".join(cmd))
            t0 = time.monotonic()
            rc = subprocess.call(cmd, cwd=wt)
            say(f"configure: {time.monotonic() - t0:.0f} s (exit {rc})")
            if rc:
                return rc
        jobs = build_jobs()
        ninja = real_exe("ninja") or "ninja"
        say(f"[{lane}] ninja -j {jobs} {' '.join(targets)}")
        t0 = time.monotonic()
        rc = subprocess.call([ninja, "-C", str(out), "-j", str(jobs), *targets], cwd=wt)
        say(f"build: {time.monotonic() - t0:.0f} s (exit {rc})")
        return rc


def main(argv: list[str] | None = None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="python -m sandover.engine")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="configure (seeded, ccache) if new, then build in a free lane")
    b.add_argument("worktree", type=Path)
    b.add_argument("targets", nargs="*", default=["gk"])
    c = sub.add_parser("configure-cmd", help="print the configure command for a checkout")
    c.add_argument("worktree", type=Path)
    h = sub.add_parser("holds", help="finished lock holds from the hold log, longest first")
    h.add_argument("--min", type=float, default=10, help="minutes (default 10)")
    a = ap.parse_args(argv)
    if a.cmd == "holds":
        for e in sorted(locks.hold_log(a.min * 60), key=lambda e: -e["seconds"]):
            print(f"{e['start']}  {e['lock']:8s} {e['seconds'] / 60:6.1f} min  {e['purpose']}")
        return 0
    if a.cmd == "configure-cmd":
        print(subprocess.list2cmdline(configure_cmd(a.worktree.resolve())))
        return 0
    return build_worktree(a.worktree.resolve(), a.targets)


if __name__ == "__main__":
    sys.exit(main())
