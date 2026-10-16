import json
import os
from pathlib import Path

import pytest

from sandover import vanilla
from sandover.rig.capture import Shot

TRANS = [-681604.375, 33955.08, -1312343.75]
ROT = [-0.99778, 0.0, -0.06651, 0.00799, 0.99276, -0.11983, 0.06603, -0.12010, -0.99056]


def sidecar(**kw) -> dict:
    m = {"name": "x", "continue": "beach-start", "hour": 17.5024, "camera": "explicit",
         "camera_trans": TRANS, "camera_rot": ROT, "effects": False, "wind": False, "jak": True,
         "particles": False, "resolution": [64, 36], "msaa": 4, "toggles": {}, "env": {},
         "timestamp": "2026-10-08T22:47:09+00:00", "sha": "abc"}
    m.update(kw)
    return m


def put(path: Path, meta: dict | None, exr: bool = False) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    # minimal PNG header: signature, IHDR length+type, width, height
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\x00\x00\x00\rIHDR" + (64).to_bytes(4, "big") + (36).to_bytes(4, "big"))
    if meta is not None:
        path.with_suffix(".json").write_text(json.dumps(meta), encoding="utf-8")
    if exr:
        path.with_suffix(".exr").write_bytes(b"exr")
    return path


@pytest.fixture
def data(tmp_path, monkeypatch):
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path))
    return tmp_path


def test_fingerprint_tolerance():
    a = sidecar()
    assert vanilla.fingerprint(a) == vanilla.fingerprint(sidecar(hour=17.5049))
    assert vanilla.fingerprint(a) != vanilla.fingerprint(sidecar(hour=12.0))
    assert vanilla.fingerprint(a) != vanilla.fingerprint(sidecar(resolution=[128, 72]))
    assert vanilla.fingerprint(a) != vanilla.fingerprint(sidecar(camera_trans=[TRANS[0] + 4096, *TRANS[1:]]))
    assert vanilla.fingerprint(a) != vanilla.fingerprint(sidecar(wind=True))


def test_camera_error_matches_rig():
    from sandover.rig.capture import camera_error

    b = ([TRANS[0] + 100, *TRANS[1:]], ROT)
    assert vanilla.camera_error((TRANS, ROT), b) == pytest.approx(camera_error(TRANS, ROT, *b))


def test_vanilla_detection():
    assert vanilla.is_vanilla_sidecar(sidecar())
    assert vanilla.is_vanilla_sidecar(sidecar(toggles={"snapshot": "0002.json"}))
    assert not vanilla.is_vanilla_sidecar(sidecar(toggles={"goal_after_load": ["(x)"]}, vanilla=True))
    assert not vanilla.is_vanilla_sidecar(sidecar(toggles={"forms": "(x)"}))
    assert not vanilla.is_vanilla_sidecar(sidecar(post_load={"applied": ["(x)"]}))
    assert not vanilla.is_vanilla_sidecar(sidecar(toggles={"goal_after_load": ["(x)"]}))
    assert not vanilla.is_vanilla_sidecar(sidecar(toggles={"play_look": True}))
    assert not vanilla.is_vanilla_sidecar(sidecar(toggles={"aa": "smaa"}))
    assert not vanilla.is_vanilla_sidecar(sidecar(render_scale=0.5))
    assert not vanilla.is_vanilla_sidecar(sidecar(env={"SANDOVER_PBR_PRESET": "x"}))


def test_index_build_on_fixture(data):
    put(data / "runs/r1/vanilla/0002.png", sidecar(), exr=True)
    put(data / "runs/r2/vanilla/0002.png", sidecar(timestamp="2026-10-09T01:00:00+00:00"))
    put(data / "runs/r2/0003.png", sidecar(toggles={"play_look": True}))  # not vanilla
    put(data / "rounds/p/p-001/A/s.png", sidecar(toggles={"goal_after_load": ["(x)"]}))  # not vanilla
    # a copy of the first capture (same timestamp) in a round's vanilla dir
    put(data / "rounds/p/p-001/vanilla/s.png", sidecar())
    # a "vanilla" folder or an Original game arm whose sidecar has forms is not indexed
    put(data / "runs/r3/vanilla/0009.png", sidecar(hour=3.0, toggles={"play_look": True}))
    rd = data / "rounds/q/q-001"
    put(rd / "A/s.png", sidecar(camera_trans=[1.0, 2.0, 3.0], toggles={"goal_after_load": ["(x)"]}))
    put(rd / "C/s.png", None)
    (rd / "manifest.json").write_text(json.dumps({
        "candidates": {"A": {"preset": "a", "role": "anchor", "label": "Today"},
                       "C": {"preset": "c", "role": "candidate", "label": "Original game"}},
        "images": {"A": {"s": "A/s.png"}, "C": {"s": "C/s.png"}}, "shots": ["s"]}), encoding="utf-8")
    log = []
    assert len(vanilla.scan(say=log.append)) == 3
    assert any("runs/r3/vanilla/0009.png" in m and "MISMATCH" in m for m in log)
    idx = vanilla.build()
    s = vanilla.stats(idx)
    assert s["unique"] == 1
    assert s["pngs"] == 3 and s["duplicate_pngs"] == 2
    assert s["duplicate_captures"] == 1 and s["refired"] == 1
    assert len(s["top_offenders"]) == 1 and s["top_offenders"][0][1] == 1
    first = next(f for f in idx["frames"].values() if f["captures"] == 2)
    assert first["json"] and first["bin"]["sha"] == "abc"
    assert first["resolution"] == [64, 36]


def test_find_cli(data, capsys, monkeypatch):
    put(data / "runs/r1/vanilla/0002.png", sidecar(), exr=True)
    vanilla.write_index(vanilla.build())
    q = data / "q.json"
    q.write_text(json.dumps(sidecar(camera_trans=[TRANS[0] + 10, *TRANS[1:]])), encoding="utf-8")
    monkeypatch.setattr("sys.argv", ["sandover-vanilla", "find", str(q)])
    assert vanilla.main() == 0
    assert "runs/r1/vanilla/0002.png" in capsys.readouterr().out
    q.write_text(json.dumps(sidecar(hour=3.0)), encoding="utf-8")
    assert vanilla.main() == 1


def shot(**kw) -> Shot:
    kw.setdefault("resolution", (64, 36))
    kw.setdefault("actors", "freeze")
    return Shot(name="s1", continue_point="beach-start", hour=17.5024, camera={"pos": TRANS, "rot": ROT}, **kw)


def test_reuse_path_without_gk(data, monkeypatch):
    from sandover.rig import capture

    put(data / "runs/r1/vanilla/0002.png", sidecar(), exr=True)
    vanilla.write_index(vanilla.build())

    def boom(*a, **k):
        raise AssertionError("gk must not start")

    monkeypatch.setattr(capture, "run_worker", boom)
    monkeypatch.setattr(capture.engine, "staged_bin", boom)
    out = data / "shots/new"
    rc = capture.run_shots("t", [shot()], out, toggles={}, setup=[])
    assert rc == 0
    assert (out / "s1.png").exists() and (out / "s1.exr").exists()
    meta = json.loads((out / "s1.json").read_text(encoding="utf-8"))
    assert meta["name"] == "s1" and meta["vanilla_reuse"] == "runs/r1/vanilla/0002.png"


def test_force_and_non_vanilla_do_not_reuse(data):
    from sandover.rig import capture

    put(data / "runs/r1/vanilla/0002.png", sidecar())
    vanilla.write_index(vanilla.build())
    s = shot()
    assert capture.reuse_vanilla([s], data / "o1", {}, [], force=True) == [s]
    s2 = shot(goals_after_load=("(x)",))
    assert capture.reuse_vanilla([s2], data / "o2", {}, [], force=False) == [s2]
    assert capture.reuse_vanilla([s2], data / "o3", {"play_look": True}, [], force=False) == [s2]
    s4 = shot(resolution=(128, 72))
    assert capture.reuse_vanilla([s4], data / "o4", {}, [], force=False) == [s4]
    assert not any((data / d).exists() for d in ("o1", "o2", "o3", "o4"))


def test_register_after_capture(data):
    p = put(data / "shots/new/s1.png", sidecar())
    assert vanilla.register(p, say=lambda m: None)
    assert vanilla.register(put(data / "shots/new2/s1.png", sidecar(timestamp="t2")), say=lambda m: None)
    idx = vanilla.load_index()
    (e,) = idx["frames"].values()
    assert e["captures"] == 2 and e["extra_rounds"] == ["shots/new"]
    assert vanilla.lookup(sidecar(), idx)["png"] == "shots/new2/s1.png"
    assert not vanilla.register(put(data / "shots/x/s.png", sidecar(vanilla_reuse="a")))


def test_newest_capture_is_kept(data):
    put(data / "runs/old/s.png", sidecar())
    put(data / "runs/new/s.png", sidecar(timestamp="2026-10-09T01:00:00+00:00"))
    (e,) = vanilla.build()["frames"].values()
    assert e["png"] == "runs/new/s.png" and e["extra_rounds"] == ["runs/old"]


def test_cutoff_excludes_older_captures(data):
    put(data / "runs/old/s.png", sidecar())
    put(data / "runs/new/s.png", sidecar(hour=3.0, timestamp="2026-10-09T01:00:00+00:00"))
    (data / "vanilla").mkdir()
    (data / "vanilla/since.txt").write_text("2026-10-09T00:00:00+00:00\n", encoding="utf-8")
    (e,) = vanilla.build()["frames"].values()
    assert e["png"] == "runs/new/s.png"


def test_register_refuses_non_vanilla(data):
    log = []
    p = put(data / "shots/n/s1.png", sidecar(toggles={"goal_after_load": ["(x)"]}))
    assert not vanilla.register(p, say=log.append)
    assert "MISMATCH" in log[0] and not vanilla.index_path().exists()
    p = put(data / "shots/n/s2.png", sidecar(env={"SANDOVER_PBR_MAPS": "x"}))
    assert not vanilla.register(p, say=log.append)


def test_reuse_into_source_folder_keeps_original(data):
    src = put(data / "runs/r1/vanilla/0002.png", sidecar(), exr=True)
    vanilla.write_index(vanilla.build())
    (e,) = vanilla.load_index()["frames"].values()
    before = src.with_suffix(".json").read_text(encoding="utf-8")
    assert vanilla.reuse(e, src, "0002", say=lambda m: None)
    assert src.exists() and src.with_suffix(".json").read_text(encoding="utf-8") == before
    out = data / "runs/r1/vanilla2/0002.png"
    assert vanilla.reuse(e, out, "0002", say=lambda m: None)
    assert not os.path.samefile(src, out)  # a copy: editing it leaves the library alone
    assert json.loads(src.with_suffix(".json").read_text(encoding="utf-8")).get("vanilla_reuse") is None


def test_reuse_off_for_bin_or_env():
    assert vanilla.reuse_allowed(None, {})
    assert not vanilla.reuse_allowed("my-build", {})
    assert not vanilla.reuse_allowed(None, {"SANDOVER_PBR_MAPS": "x"})
    assert vanilla.reuse_allowed(None, {"SANDOVER_GK_PROJ_PATH": "x"})


def test_lookup_half_tolerance(data):
    put(data / "runs/r1/s.png", sidecar())
    idx = vanilla.build()
    far = sidecar(camera_trans=[TRANS[0] + 0.04 * vanilla.METER, *TRANS[1:]])
    assert vanilla.lookup(far, idx) is None
    near = sidecar(camera_trans=[TRANS[0] + 0.02 * vanilla.METER, *TRANS[1:]])
    assert vanilla.lookup(near, idx)


def test_actor_mode_and_settle_frames_are_in_the_request(data):
    put(data / "runs/r1/s.png", sidecar(actors="freeze", settle_frames=60))
    idx = vanilla.build()
    assert vanilla.lookup(vanilla.shot_meta(shot(actors="freeze", settle_frames=60)), idx)
    assert vanilla.lookup(vanilla.shot_meta(shot(actors="live")), idx) is None
    assert vanilla.lookup(vanilla.shot_meta(shot(actors="freeze", settle_frames=90)), idx) is None
