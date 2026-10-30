"""engine.ensure_fresh: rebuild only when a tracked engine file is newer than gk."""

import os
from contextlib import contextmanager

import pytest

from sandover import engine


@pytest.fixture
def tree(tmp_path, monkeypatch):
    """A fake checkout: game/gfx.h tracked, gk in out/build/Release/bin. git and the lock are stubbed."""
    (tmp_path / "game").mkdir()
    src = tmp_path / "game" / "gfx.h"
    src.write_text("x", encoding="utf-8")
    bin_dir = tmp_path / "out" / "build" / "Release" / "bin"
    bin_dir.mkdir(parents=True)
    gk = bin_dir / f"gk{engine.EXE}"
    gk.write_bytes(b"x")
    state = {"dirty": "", "builds": [], "rc": 0}

    def git(*args):
        if args[0] == "ls-files":
            return "game/gfx.h"
        if args[0] == "status":
            return state["dirty"]
        return "abc1234"

    @contextmanager
    def hold(name, purpose, *a, **k):
        state["lock"] = (name, purpose)
        yield tmp_path

    def run_build(cmd):
        state["builds"].append(cmd)
        if state["rc"] == 0:
            os.utime(gk, (src.stat().st_mtime + 10,) * 2)
        return state["rc"], ["[1/2] compiling", "error: boom"]

    monkeypatch.setattr(engine, "root", lambda: tmp_path)
    monkeypatch.setattr(engine, "_git", git)
    monkeypatch.setattr(engine.locks, "hold", hold)
    monkeypatch.setattr(engine, "_run_build", run_build)
    state.update(src=src, gk=gk)
    return state


def _age(path, newer_than, by):
    t = newer_than.stat().st_mtime + by
    os.utime(path, (t, t))


def test_stale_rebuilds_under_build_lock(tree):
    _age(tree["src"], tree["gk"], 5)
    said = []
    reason = engine.ensure_fresh(say=said.append)
    assert reason and "game/gfx.h" in reason
    assert len(tree["builds"]) == 1
    cmd = tree["builds"][0]
    assert cmd[:2] == ["cmake", "--build"] and cmd[-2:] == ["-j", str(engine.build_jobs())]
    assert {"gk", "goalc"} <= set(cmd)
    assert tree["lock"][0] == engine.locks.BUILD and tree["lock"][1]
    assert said[-1] == "rebuild done"


def test_fresh_does_not_rebuild(tree):
    _age(tree["gk"], tree["src"], 5)
    assert engine.ensure_fresh(say=lambda m: None) is None
    assert tree["builds"] == []


def test_failed_rebuild_refuses(tree):
    _age(tree["src"], tree["gk"], 5)
    tree["rc"] = 1
    with pytest.raises(engine.StaleBuild) as exc:
        engine.ensure_fresh(say=lambda m: None)
    assert "error: boom" in str(exc.value) and "game/gfx.h" in str(exc.value)


def test_dirty_but_fresh_launches_without_rebuild(tree):
    tree["dirty"] = " M game/gfx.h"
    _age(tree["gk"], tree["src"], 5)
    assert engine.source_state(tree["gk"])["engine_sha"] is None  # still recorded as untrusted
    assert engine.ensure_fresh(say=lambda m: None) is None
    assert tree["builds"] == []


def test_dirty_and_stale_rebuilds(tree):
    tree["dirty"] = " M game/gfx.h"
    _age(tree["src"], tree["gk"], 5)
    assert engine.ensure_fresh(say=lambda m: None)
    assert len(tree["builds"]) == 1


def test_no_rebuild_warns_and_skips_build(tree):
    _age(tree["src"], tree["gk"], 5)
    said = []
    assert engine.ensure_fresh(rebuild=False, say=said.append)
    assert tree["builds"] == [] and "STALE BUILD" in said[0]


def test_missing_gk_is_stale(tree):
    tree["gk"].unlink()
    assert "not been built" in engine.stale_reason(tree["gk"])


def test_build_jobs_from_cpu_count(monkeypatch):
    monkeypatch.setattr(engine.os, "cpu_count", lambda: 24)
    assert engine.build_jobs() == 22
    monkeypatch.setattr(engine.os, "cpu_count", lambda: 2)
    assert engine.build_jobs() == 1
