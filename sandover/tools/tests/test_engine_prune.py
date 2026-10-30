"""engine.prune: only auto-staged copies go; hand-named copies (perf-base, other bins) stay."""

from sandover import engine


def _copy(root, name, done=True):
    d = root / "bin" / name
    d.mkdir(parents=True)
    (d / "gk.exe").write_bytes(b"x")
    if done:
        (d / engine.DONE).write_text("x", encoding="utf-8")
    return d


def test_prune_keeps_hand_named_copies(tmp_path, monkeypatch):
    monkeypatch.setattr(engine, "data_dir", lambda: tmp_path)
    monkeypatch.setattr(engine, "in_use", lambda: set())
    monkeypatch.setattr(engine, "_busy", lambda d: False)
    stamped = [_copy(tmp_path, f"2026092{i}-120000-abcdef{i}") for i in range(5)]
    hand = [_copy(tmp_path, "perf-base"), _copy(tmp_path, "tl-3"), _copy(tmp_path, "perf-opt-ssao3", False)]
    removed = engine.prune(keep=3)
    assert sorted(d.name for d in removed) == sorted(d.name for d in stamped[:2])
    assert all(d.exists() for d in hand + stamped[2:])
    assert engine.prune(keep=3) == []
