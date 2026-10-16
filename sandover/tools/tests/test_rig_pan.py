import math

import pytest

from sandover.lights import flicker as F
from sandover.rig import capture as C

CAM = {"pos": [4096.0, 0.0, 0.0], "rot": [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]}


def test_pan_cameras_turn_about_world_up_and_move():
    cams = C.pan_cameras(CAM, {"frames": 2, "yaw_deg": 90.0, "move_m": [0.0, 1.0, 0.0]})
    assert len(cams) == 7  # frames + 5: the approach frame and slack for the burst
    pos, rows = cams[1]
    assert pos == [4096.0, 4096.0, 0.0]
    assert rows[:3] == pytest.approx([0.0, 0.0, -1.0], abs=1e-9)
    assert rows[3:6] == [0.0, 1.0, 0.0]
    assert rows[6:9] == pytest.approx([1.0, 0.0, 0.0], abs=1e-9)
    for k in range(3):  # rows stay orthonormal
        assert math.hypot(*cams[4][1][3 * k:3 * k + 3]) == pytest.approx(1.0)


def test_pan_needs_explicit_camera():
    with pytest.raises(ValueError):
        C._pan({"name": "x", "camera": "from_continue", "pan": {"frames": 8}})
    with pytest.raises(ValueError):
        C._pan({"name": "x", "camera": CAM, "pan": {"frames": 99}})
    assert C._pan({"name": "x", "camera": CAM, "pan": {"frames": 4, "yaw_deg": 0.5}}) == {
        "frames": 4, "yaw_deg": 0.5, "move_m": [0.0, 0.0, 0.0]}


def test_parse_emlights():
    line = ("[12:00] [info] EMLIGHTS f=3 raw=1.000,2.000,3.000,0.5,0.40;4.000,5.000,6.000,0.25,0.90; "
            "out=7,1.000,2.000,3.000,0.5; world=1234,9.000,8.000,7.000,0.01;")
    p = C.parse_emlights(line)
    assert p["frame"] == 3
    assert p["raw"] == [[1.0, 2.0, 3.0, 0.5, 0.4], [4.0, 5.0, 6.0, 0.25, 0.9]]
    assert p["out"] == [[7, 1.0, 2.0, 3.0, 0.5]] and p["world"] == [[1234, 9.0, 8.0, 7.0, 0.01]]
    assert C.parse_emlights("[info] EMLIGHTS f=0 raw= out= world=")["raw"] == []
    assert C.parse_emlights("RIG:SHOT x") is None


def test_flicker_steady_vs_jumping():
    steady = [{"frame": i, "raw": [[0.0, 0.0, 0.0, 1.0, 1.0]], "out": [[0, 0.0, 0.0, 0.0, 1.0]],
               "world": []} for i in range(4)]
    r = F.analyse(steady)
    assert set(r["mean"].values()) == {0.0}
    assert r["pos_var_m2"] == 0.0
    # raw hops 0.5 m and loses half its power every other frame; the slot moves once, 0.1 m
    jump = [{"frame": i, "raw": [[0.5 * (i % 2), 0.0, 0.0, 1.0 - 0.5 * (i % 2), 1.0]],
             "out": [[0, 0.1 * (i > 1), 0.0, 0.0, 1.0]], "world": []} for i in range(4)]
    r = F.analyse(jump)
    assert r["mean"]["raw_move_m"] == pytest.approx(0.5)
    assert r["mean"]["raw_churn"] == pytest.approx(0.5 / 1.5)
    assert r["mean"]["out_move_m"] == pytest.approx(0.1 / 3)
    assert r["mean"]["out_churn"] == 0.0
    assert r["pos_var_m2"] == pytest.approx(0.0025)
    # a slot that vanishes is all churn
    gone = [{"frame": 0, "raw": [], "out": [[0, 0.0, 0.0, 0.0, 1.0]], "world": []},
            {"frame": 1, "raw": [], "out": [], "world": []}]
    assert F.analyse(gone)["mean"]["out_churn"] == 1.0


def test_flicker_world_churn_and_drawn_step():
    fr = [{"frame": 0, "raw": [], "out": [[0, 0.0, 0.0, 0.0, 1.0]],
           "world": [[5, 1.0, 0.0, 0.0, 1.0]]},
          {"frame": 1, "raw": [], "out": [[0, 0.0, 0.0, 0.0, 1.0]], "world": []}]
    r = F.analyse(fr)
    assert r["pairs"][0]["world_churn"] == 1.0
    assert r["pairs"][0]["drawn_step"] == pytest.approx(0.5)
    assert r["pairs"][0]["drawn_luma"] == 1.0


def test_flicker_run_with_baseline(tmp_path, capsys):
    import json

    for d, luma in (("a", 1.0), ("b", 2.0)):
        (tmp_path / d).mkdir()
        lights = [{"frame": i, "raw": [], "out": [[0, 0.0, 0.0, 0.0, luma if i % 2 else 1.0]],
                   "world": []} for i in range(3)]
        (tmp_path / d / "s.json").write_text(
            json.dumps({"name": "s", "pan": {"frames": 3, "lights": lights}}), encoding="utf-8")
    assert F.run(tmp_path / "a", None, 2.0, False, tmp_path / "b") == 0
    res = json.loads((tmp_path / "a" / "s-flicker.json").read_text(encoding="utf-8"))
    assert res["mean"]["out_churn"] == 0.0 and res["baseline"]["out_churn"] > 0.0
    assert "basel." in capsys.readouterr().out


def test_emlights_off_line_and_presets_dir(monkeypatch, tmp_path):
    m = C.EMLIGHTS_OFF.search("[01:02] [info] EMLIGHTS f=3 off ssao=false lights=true")
    assert m.group(1) == "ssao=false lights=true"
    assert C.parse_emlights("[info] EMLIGHTS f=3 off ssao=false lights=true") is None
    from sandover import presets

    monkeypatch.setenv("SANDOVER_PRESETS", str(tmp_path))
    assert presets.preset_file("emissive") == tmp_path / "emissive.toml"


def test_box_steps_follow_a_pan_and_catch_a_pop():
    import numpy as np

    rng = np.random.default_rng(1)
    base = rng.uniform(40, 120, (200, 400, 3)).astype(np.float32)
    frames = []
    for i in range(6):
        f = np.roll(base, 6 * i, axis=1).copy()  # the pan scrolls the content
        if i >= 4:
            f[..., 2] += 30.0  # a blue light switches on
        frames.append(f)
    bx = F.box_steps(frames, grid=(2, 1), search=20)
    assert bx and all(b["frames"] == 6 for b in bx)
    s = F.box_summary(bx)
    assert s["box_hue"] == pytest.approx(30.0, abs=0.5)
    assert s["box_step"] == pytest.approx(30.0 * 0.0722, abs=0.2)
    quiet = F.box_summary(F.box_steps(frames[:4], grid=(2, 1), search=20))
    assert quiet["box_step"] < 0.2 and quiet["box_hue"] < 0.2


def test_parse_emlights_with_colour():
    p = C.parse_emlights("EMLIGHTS f=1 raw=1,2,3,0.5,0.4,0.1,0.6,0.9; out=4,1,2,3,0.5,0.1,0.6,0.9; world=")
    assert p["raw"][0][5:] == [0.1, 0.6, 0.9] and p["out"][0][5:] == [0.1, 0.6, 0.9]
