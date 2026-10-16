"""Slot locks whose holder died: the lock and the orphans on the slot's ports are reclaimed at once."""

import subprocess
import sys
from pathlib import Path

import pytest

from sandover import locks
from sandover.rig import capture as C


@pytest.fixture
def data(tmp_path, monkeypatch):
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path))
    (tmp_path / "locks").mkdir()
    return tmp_path


@pytest.fixture
def dead_pid():
    """The pid of a process that has exited while this test still holds its handle (on Windows
    the process object, and so the pid, stays openable)."""
    p = subprocess.Popen([sys.executable, "-c", "pass"])
    p.wait()
    yield p.pid
    del p


def test_exited_process_is_not_running(dead_pid):
    import os

    assert locks.pid_running(os.getpid())
    assert not locks.pid_running(dead_pid)


def _write_lock(data: Path, name: str, pid: int, purpose_pid: int | None = None) -> None:
    (data / "locks" / f"{name}.lock").write_text(f"{pid}\n{locks._host()}\n", encoding="utf-8")
    (data / "locks" / f"{name}.purpose").write_text(
        f"sandover-capture t (1 shots) bin b\n2026-10-02T05:23:21+00:00\npid {purpose_pid or pid}\n",
        encoding="utf-8")


def test_dead_holder_lock_is_taken_at_once(data, dead_pid, monkeypatch, capsys):
    monkeypatch.setattr(locks.Slot, "busy_ports", lambda self: [])
    _write_lock(data, "slot0", dead_pid)
    with locks.slot(0, "new run", timeout=0) as s:
        assert s.index == 0
        assert f"\npid {__import__('os').getpid()}\n" in (data / "locks" / "slot0.purpose").read_text()
    assert f"reclaimed slot0 lock: holder pid {dead_pid} is not running" in capsys.readouterr().err


def test_live_holder_lock_is_kept(data, monkeypatch):
    import os

    monkeypatch.setattr(locks.Slot, "busy_ports", lambda self: [])
    _write_lock(data, "slot0", os.getppid())
    with pytest.raises(TimeoutError, match="slot0"):
        with locks.slot(0, "new run", timeout=0, quiet=True):
            pass


def test_dead_holders_orphans_on_the_ports_are_stopped(data, dead_pid, monkeypatch, capsys):
    """The force-killed run's goalc kept 8181/8128: only that orphan is stopped."""
    _write_lock(data, "slot0", dead_pid)
    busy = {"ports": [8181, 8128]}
    monkeypatch.setattr(locks.Slot, "busy_ports", lambda self: list(busy["ports"]))
    monkeypatch.setattr(locks, "port_listeners", lambda ports: {8181: 500, 8128: 500, 8112: 600})
    procs = {500: ("goalc.exe", dead_pid, "goalc.exe --user-auto --game jak1 --port 8181 --debug-port 8128"),
             600: ("python.exe", dead_pid, "python.exe something")}
    monkeypatch.setattr(locks, "process_info", lambda pid: procs.get(pid))
    killed = []

    def kill(pid):
        killed.append(pid)
        busy["ports"] = []

    real = locks.reclaim_orphans
    monkeypatch.setattr(locks, "reclaim_orphans", lambda s: real(s, kill=kill))
    with locks.slot(0, "new run", timeout=0) as s:
        assert s.index == 0
    assert killed == [500]
    err = capsys.readouterr().err
    assert f"reclaimed slot0: holder pid {dead_pid} is not running; stopping its goalc.exe pid 500" in err


def test_orphans_are_left_alone_unless_provably_the_dead_holders(data, dead_pid, monkeypatch):
    import os

    monkeypatch.setattr(locks, "port_listeners", lambda ports: {8112: 1, 8181: 2, 8128: 3})
    procs = {1: ("gk.exe", 4321, "gk.exe --config-path data\\config\\slot0"),  # another parent
             2: ("gk.exe", dead_pid, "gk.exe --config-path data\\config\\play"),  # a play session
             3: ("notepad.exe", dead_pid, "notepad.exe")}
    monkeypatch.setattr(locks, "process_info", lambda pid: procs.get(pid))
    killed = []
    _write_lock(data, "slot0", dead_pid)
    assert locks.reclaim_orphans(locks.Slot(0), kill=killed.append) == [] and killed == []
    # a live holder: nothing is touched whatever listens
    _write_lock(data, "slot0", os.getpid())
    procs[1] = ("gk.exe", os.getpid(), "gk.exe --config-path data\\config\\slot0")
    assert locks.reclaim_orphans(locks.Slot(0), kill=killed.append) == [] and killed == []


def test_parse_netstat():
    out = ("  Proto  Local Address          Foreign Address        State           PID\n"
           "  TCP    0.0.0.0:8181           0.0.0.0:0              LISTENING       21964\n"
           "  TCP    127.0.0.1:8128         0.0.0.0:0              LISTENING       21964\n"
           "  TCP    127.0.0.1:8112         127.0.0.1:50000        ESTABLISHED     77\n"
           "  TCP    [::]:8112              [::]:0                 LISTENING       88\n")
    assert locks.parse_netstat(out, [8112, 8181, 8128]) == {8181: 21964, 8128: 21964, 8112: 88}


def test_slot_wait_counts_towards_the_wall_cap(data, monkeypatch, capsys):
    monkeypatch.setattr(C, "_free_slot", lambda *a, **k: None)
    seen = {}

    def busy_slot(index=None, purpose="", timeout=None, **k):
        seen["timeout"] = timeout
        raise TimeoutError("no free game slot among [0]: slot0 ports [8181] in use")

    monkeypatch.setattr(C.locks, "slot", busy_slot)
    monkeypatch.setattr(C.engine, "staged_bin", lambda: data / "bin" / "b")
    monkeypatch.setattr(C.engine, "prune", lambda also_keep=None: [])
    monkeypatch.setattr(C, "outputs_fresh", lambda: True)
    guard = C.PlayGuard("ignore", lister=lambda: [])
    shots = [C.Shot(name="a", continue_point="jungle-start", hour=12)]
    assert C.run_shots("t", shots, data / "out", slot=0, max_seconds=700, guard=guard) == 1
    assert 0 < seen["timeout"] <= 700
    out = capsys.readouterr().out
    assert "RIG:SLOT-BUSY no free game slot among [0]: slot0 ports [8181] in use" in out
    assert "0/1 shots" in out and "skipped (slot busy): ['a']" in out


def test_only_the_rigs_own_launch_line_is_stopped(data, dead_pid, monkeypatch):
    """Dead parent alone is not enough (a reused pid could have launched anything): the command
    line must be the rig's launch line for this slot, and an empty one is never touched."""
    s0 = locks.Slot(0)
    rig_gk = f'"x/gk.exe" -v --game jak1 --config-path "{s0.config_dir}" --port 8112 -- -boot'
    monkeypatch.setattr(locks, "port_listeners", lambda ports: {8112: 1, 8181: 2, 8128: 3})
    procs = {1: ("gk.exe", dead_pid, ""),  # elevated: command line unreadable
             2: ("goalc.exe", dead_pid, "goalc.exe --user-auto --game jak1"),  # hand-run, default ports
             3: ("goalc.exe", dead_pid, "goalc.exe --user-auto --game jak1 --port 8191 --debug-port 8138")}
    monkeypatch.setattr(locks, "process_info", lambda pid: procs.get(pid))
    killed = []
    _write_lock(data, "slot0", dead_pid)
    assert locks.reclaim_orphans(s0, kill=killed.append) == [] and killed == []
    procs[1] = ("gk.exe", dead_pid, rig_gk)
    procs[2] = ("goalc.exe", dead_pid, "goalc.exe --user-auto --game jak1 --port 8181 --debug-port 8128")
    assert locks.reclaim_orphans(s0, kill=killed.append) == [1, 2] and killed == [1, 2]


def test_rig_launch_line():
    s0, s1 = locks.Slot(0), locks.Slot(1)
    gk = f"gk.exe -v --game jak1 --config-path {s0.config_dir} --port 8112 -- -boot"
    assert locks.rig_launch_line(s0, "gk.exe", gk)
    assert locks.rig_launch_line(s0, "GK.EXE", gk.replace("--config-path ", "--config-path="))
    assert not locks.rig_launch_line(s1, "gk.exe", gk)
    assert not locks.rig_launch_line(s0, "gk.exe", "gk.exe -v --game jak1 -- -boot")
    goalc = "goalc.exe --user-auto --game jak1 --port 8181 --debug-port 8128"
    assert locks.rig_launch_line(s0, "goalc.exe", goalc)
    assert not locks.rig_launch_line(s1, "goalc.exe", goalc)
    assert not locks.rig_launch_line(s0, "goalc.exe", "goalc.exe --debug-port 8128")
    assert not locks.rig_launch_line(s0, "goalc.exe", "goalc.exe --port 8181")
    for empty in ("", "   "):
        assert not locks.rig_launch_line(s0, "gk.exe", empty)
        assert not locks.rig_launch_line(s0, "goalc.exe", empty)


@pytest.mark.skipif(__import__("os").name != "nt", reason="Windows OpenProcess path")
def test_pid_running_access_denied_is_running(monkeypatch):
    import ctypes

    class K32:
        class OpenProcess:
            restype = None

            def __new__(cls, *a):
                return 0

    monkeypatch.setattr(ctypes, "WinDLL", lambda *a, **k: K32)
    monkeypatch.setattr(ctypes, "get_last_error", lambda: 5)  # ERROR_ACCESS_DENIED
    assert locks.pid_running(4)
    monkeypatch.setattr(ctypes, "get_last_error", lambda: 87)  # ERROR_INVALID_PARAMETER
    assert not locks.pid_running(4)


@pytest.mark.skipif(__import__("os").name == "nt", reason="POSIX kill path")
def test_pid_running_permission_error_is_running(monkeypatch):  # pragma: no cover
    def deny(pid, sig):
        raise PermissionError

    monkeypatch.setattr(locks.os, "kill", deny)
    assert locks.pid_running(1)


def test_old_filelock_without_break_lock_file_breaks_nothing(data, dead_pid, monkeypatch):
    import builtins

    real_import = builtins.__import__

    def no_util(name, *a, **k):
        if name == "filelock._util":
            raise ImportError(name)
        return real_import(name, *a, **k)

    monkeypatch.setattr(builtins, "__import__", no_util)
    _write_lock(data, "slot3", dead_pid)
    locks._break_dead("slot3", data / "locks" / "slot3.lock")
    assert (data / "locks" / "slot3.lock").exists()
