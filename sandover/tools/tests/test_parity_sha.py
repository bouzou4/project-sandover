import os
import subprocess

import pytest

from sandover import engine
from sandover.parity import gate


def _git(repo, *args):
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)


@pytest.fixture
def checkout(tmp_path, monkeypatch):
    repo = tmp_path / "repo"
    (repo / "game").mkdir(parents=True)
    (repo / "game" / "a.cpp").write_text("int a;\n")
    _git(repo, "init", "-q")
    _git(repo, "add", ".")
    _git(repo, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "init")
    monkeypatch.setenv("SANDOVER_ROOT", str(repo))
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path / "data"))
    bin_dir = tmp_path / "data" / "bin" / "20260101-000000-abc"
    bin_dir.mkdir(parents=True)
    gk = bin_dir / f"gk{engine.EXE}"
    gk.write_bytes(b"gk")
    return repo, bin_dir, gk


def _age(path, seconds):
    t = path.stat().st_mtime - seconds
    os.utime(path, (t, t))


def test_source_state_clean_build(checkout):
    repo, _, gk = checkout
    _age(repo / "game" / "a.cpp", 60)
    info = engine.source_state(gk)
    assert info["engine_sha"] and info["reason"] == ""


def test_source_state_rejects_source_newer_than_build(checkout):
    _, _, gk = checkout
    _age(gk, 60)
    info = engine.source_state(gk)
    assert info["engine_sha"] is None and "newer than the build" in info["reason"]


def test_source_state_rejects_dirty_engine(checkout):
    repo, _, gk = checkout
    (repo / "game" / "a.cpp").write_text("int b;\n")
    _age(repo / "game" / "a.cpp", 60)
    info = engine.source_state(gk)
    assert info["engine_sha"] is None and "uncommitted" in info["reason"]


def test_require_sha_fails_without_build_info(checkout):
    _, bin_dir, _ = checkout
    with pytest.raises(SystemExit, match="unknown"):
        gate.require_sha(bin_dir)
    assert gate.require_sha(bin_dir, "abc1234") == "abc1234"


def test_pin_uses_recorded_sha_and_rejects_contradiction(checkout):
    repo, bin_dir, _ = checkout
    _age(repo / "game" / "a.cpp", 60)
    engine._write_info(bin_dir, bin_dir)
    sha = engine.build_info(bin_dir)["engine_sha"]
    pinned = gate.resolve_build(str(bin_dir), pin=True)
    assert pinned.name == f"{sha}-{bin_dir.name}" and gate.sha_of_bin(pinned) == (sha, "")
    with pytest.raises(SystemExit, match="contradicts"):
        gate.require_sha(bin_dir, "0000000")
