"""Rig instances next to an interactive goalc.

The integration test needs built binaries and game data; it runs when SANDOVER_TEST_BIN
names a directory holding goalc (e.g. a staged copy under $SANDOVER_DATA/bin).
"""

import os
import subprocess
import time
from pathlib import Path

import pytest

from sandover import locks
from sandover.rig.capture import EXE, engine_env, goalc_env_for


def test_each_slot_logs_to_its_own_dir():
    a, b = engine_env(locks.Slot(1)), engine_env(locks.Slot(7))
    assert Path(a["OPENGOAL_LOG_DIR"]).parent == locks.Slot(1).logs_dir
    assert a["OPENGOAL_LOG_DIR"] != b["OPENGOAL_LOG_DIR"]
    g = goalc_env_for(locks.Slot(1))
    assert g["OPENGOAL_LOG_DIR"] == a["OPENGOAL_LOG_DIR"]
    assert g["OPENGOAL_MAKE_NO_WRITE"] == "1"
    assert "OPENGOAL_MAKE_NO_WRITE" not in goalc_env_for(locks.Slot(1), write=True)


def _start(bin_dir: Path, slot: locks.Slot, env: dict, out: Path, stdin) -> subprocess.Popen:
    return subprocess.Popen(
        [bin_dir / f"goalc{EXE}", "--user-auto", "--game", "jak1", "--port", str(slot.nrepl_port),
         "--debug-port", str(slot.debug_port)],
        env=env, stdin=stdin, stdout=open(out, "wb"), stderr=subprocess.STDOUT)


def _listening(p: subprocess.Popen, out: Path, timeout: float = 60) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if b"Listening on" in out.read_bytes():
            return True
        if p.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def _stop(p: subprocess.Popen) -> None:
    p.kill()
    p.wait(30)


@pytest.mark.skipif(not os.environ.get("SANDOVER_TEST_BIN"), reason="SANDOVER_TEST_BIN not set")
def test_rig_goalc_starts_while_interactive_goalc_holds_oldest_log(tmp_path):
    bin_dir = Path(os.environ["SANDOVER_TEST_BIN"])
    shared = tmp_path / "log"
    empty = tmp_path / "empty-stdin.txt"
    empty.write_bytes(b"")
    with locks.slot(purpose="test: interactive goalc", timeout=0) as play, \
            locks.slot(purpose="test: rig goalc", timeout=0) as rig:
        # an interactive session (stdin stays open) and the rig's way (stdin at EOF, as in Game)
        env = dict(goalc_env_for(play), OPENGOAL_LOG_DIR=str(shared))
        held = _start(bin_dir, play, env, tmp_path / "play.out", subprocess.PIPE)
        try:
            assert _listening(held, tmp_path / "play.out")
            # enough starts in the same dir that the held log becomes the oldest to rotate out
            for k in range(11):
                env = dict(goalc_env_for(rig), OPENGOAL_LOG_DIR=str(shared))
                out = tmp_path / f"rig{k}.out"
                p = _start(bin_dir, rig, env, out, open(empty, "rb"))
                ok = _listening(p, out)
                _stop(p)
                assert ok, out.read_text(errors="replace")[-400:]
            out = tmp_path / "rig-own.out"
            p = _start(bin_dir, rig, goalc_env_for(rig), out, open(empty, "rb"))
            ok = _listening(p, out)
            _stop(p)
            assert ok, out.read_text(errors="replace")[-400:]
            assert held.poll() is None
            assert list(shared.glob("compiler.*.log"))
        finally:
            _stop(held)
