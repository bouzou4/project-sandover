"""Rig camera: re-fixed after the hold and after the settle frames, and checked at the shot."""

import json
import math
import re
from types import SimpleNamespace

from sandover.rig import capture as C

# 0011-dbg on 6bfbfdd87, no after-load forms: RIG:CAMERA in the slot log (9.7 m off the snapshot)
DRIFTED = ("ffffffffc744a623 46d203e4 ffffffffc8840d14 ffffffffbf41d221 ffffffffbb32a1f5 3f273d5d "
           "3b8d4ea0 3f7ffca3 3c163c5e ffffffffbf273cce 3c1fe70d ffffffffbf41cecf")
# runs/20260926-1011-play/0011.json, the camera the shot asked for
SNAP_TRANS = [-48743.2421875, 28409.5, -310308.96875]
SNAP_ROT = [-0.17649364471435547, -1.8998980522155762e-07, 0.9843018054962158,
            -0.11662255227565765, 0.9929561614990234, -0.020911216735839844,
            -0.9773684740066528, -0.11848248541355133, -0.17525053024291992]
# recheck-p297c/0011-off (held, after-load form): position kept, rotation re-aimed by 0.4 deg
HELD_ROT = [-0.18329644203186035, 1.080334186553955e-06, 0.9830576777458191,
            -0.11743348836898804, 0.9928393363952637, -0.021897196769714355,
            -0.9760183691978455, -0.11945757269859314, -0.18198370933532715]


def yaw(rot, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    out = []
    for i in range(3):
        x, y, z = rot[3 * i:3 * i + 3]
        out += [x * c + z * s, y, -x * s + z * c]
    return out


def test_camera_bits_reads_the_logged_camera():
    trans, rot = C.camera_bits(DRIFTED)
    assert [round(v) for v in trans] == [-50342, 26882, -270441]
    assert round(rot[0], 4) == -0.7571 and round(rot[2], 4) == 0.6533


def test_camera_error():
    assert C.camera_error(SNAP_TRANS, SNAP_ROT, SNAP_TRANS, SNAP_ROT) == (0.0, 0.0)
    moved = [SNAP_TRANS[0] + 4096.0, SNAP_TRANS[1], SNAP_TRANS[2]]
    dist, deg = C.camera_error(SNAP_TRANS, SNAP_ROT, moved, yaw(SNAP_ROT, 3.0))
    assert math.isclose(dist, 1.0) and math.isclose(deg, 3.0, abs_tol=1e-3)


def test_check_camera_flags_the_0011_captures():
    bad = C.check_camera((SNAP_TRANS, SNAP_ROT), C.camera_bits(DRIFTED))
    assert not bad["ok"] and 9.7 < bad["dist_m"] < 9.8 and 39 < bad["angle_deg"] < 40.5
    held = C.check_camera((SNAP_TRANS, SNAP_ROT), (SNAP_TRANS, HELD_ROT))
    assert not held["ok"] and held["dist_m"] == 0 and 0.35 < held["angle_deg"] < 0.45
    tiny = C.check_camera((SNAP_TRANS, SNAP_ROT), ([SNAP_TRANS[0] + 40.0, *SNAP_TRANS[1:]], yaw(SNAP_ROT, 0.1)))
    assert tiny["ok"]
    assert "RIG:CAMERA-MISMATCH 0011" in C.camera_mismatch_message("0011", bad)


def test_parse_camfix_and_sidecar_fields():
    line = f'RIG:CAMFIX "village1-hut" region="camera-212" state=cam-string slaves=2 {DRIFTED}'
    fix = C.parse_camfix(line)
    assert fix["region"] == "camera-212" and fix["state"] == "cam-string" and fix["slaves"] == 2
    none = C.parse_camfix(f"RIG:CAMFIX village3-farside region=none state=cam-fixed slaves=1 {DRIFTED}")
    assert none["region"] is None
    assert C.parse_camfix(f"RIG:CAMERA {DRIFTED}") is None
    out = C.camera_fields((SNAP_TRANS, SNAP_ROT), (SNAP_TRANS, SNAP_ROT), fix)
    assert out["camera_check"]["ok"] and out["camera_requested"]["trans"] == SNAP_TRANS
    assert 9.7 < out["camera_refixed"]["dist_m"] < 9.8
    assert "camera_refixed" not in C.camera_fields((SNAP_TRANS, SNAP_ROT), (SNAP_TRANS, SNAP_ROT), None)
    json.dumps(out)


def _sequence() -> str:
    src = (C.HERE / "rig.gc").read_text(encoding="utf-8")
    start = src.index("(defun rig-sequence ()")
    return src[start:src.index("(defun rig-set-options", start)]


def _balanced(src: str) -> bool:
    depth, in_str, i = 0, False, 0
    while i < len(src):
        ch = src[i]
        if in_str:
            if ch == "\\":
                i += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch == ";":
            i = src.find("\n", i)
            if i < 0:
                break
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth < 0:
                return False
        i += 1
    return depth == 0 and not in_str


def test_rig_gc_parens_balance():
    assert _balanced((C.HERE / "rig.gc").read_text(encoding="utf-8"))


def test_camera_is_fixed_again_after_the_hold_and_after_the_settle():
    seq = _sequence()
    at = seq.index
    fixes = [m.start() for m in re.finditer(r"\(rig-camera\)", seq)]
    assert len(fixes) == 3
    # READY, fix, hold (after-load forms and waits), release, fix again, actor reset
    assert at("RIG:READY") < fixes[0] < at("RIG:HOLD") < at("RIG:RELEASED") < fixes[1] < at("(reset-actors 'life)")
    # settle frames, stage hook, then the drift check and third fix, before the pan and the pause
    settle = at("(dotimes (i (-> o settle))")
    assert settle < at("(*rig-stage-hook*)") < at("(rig-camera-off?)") < fixes[2]
    assert fixes[2] < at("(dotimes (i RIG_CAM_SETTLE)") < at("(rig-pan-camera 0)") < at("(set-master-mode 'pause)")
    assert at("(set-master-mode 'pause)") < at("(rig-print-camera)") < at("(pc-screen-shot)")


class FakeGame:
    def __init__(self, meta):
        self.meta = meta

    def __call__(self, *a, **k):
        return self

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def alive(self):
        return True

    def send(self, form):
        pass

    def shoot(self, shot, out_png):
        return dict(self.meta)


def _worker(monkeypatch, tmp_path, check, strict):
    meta = {"load_ticks": 60, "camera_check": check}
    monkeypatch.setattr(C, "Game", FakeGame(meta))
    shot = C.Shot(name="0011", continue_point="village1-hut", hour=22.5)
    failed, crashed = C.run_worker(SimpleNamespace(index=0), tmp_path, [shot], tmp_path, False, None,
                                   camera_strict=strict)
    return failed, crashed, json.loads((tmp_path / "0011.json").read_text(encoding="utf-8"))


def test_mismatch_fails_the_shot_but_keeps_the_sidecar(monkeypatch, tmp_path, capsys):
    bad = C.check_camera((SNAP_TRANS, SNAP_ROT), C.camera_bits(DRIFTED))
    failed, crashed, side = _worker(monkeypatch, tmp_path, bad, True)
    assert [s.name for s in failed] == ["0011"] and not crashed
    assert side["camera_check"]["ok"] is False
    assert "RIG:CAMERA-MISMATCH 0011" in capsys.readouterr().out


def test_mismatch_only_reported_with_allow_camera_drift(monkeypatch, tmp_path, capsys):
    bad = C.check_camera((SNAP_TRANS, SNAP_ROT), C.camera_bits(DRIFTED))
    failed, _, _ = _worker(monkeypatch, tmp_path, bad, False)
    out = capsys.readouterr().out
    assert failed == [] and "RIG:CAMERA-MISMATCH" in out and "RIG:DONE 0011" in out


def test_matching_camera_is_done(monkeypatch, tmp_path, capsys):
    ok = C.check_camera((SNAP_TRANS, SNAP_ROT), (SNAP_TRANS, SNAP_ROT))
    failed, _, _ = _worker(monkeypatch, tmp_path, ok, True)
    out = capsys.readouterr().out
    assert failed == [] and "MISMATCH" not in out and "RIG:DONE 0011" in out


def test_a_cut_camera_line_is_reported_not_raised():
    # render-thread output landing inside a RIG:CAMERA line: HEX12 still matches by splitting
    # words, so the captured text holds fewer than 12 words
    cut = "RIG:CAMERA " + " ".join(DRIFTED.split()[:7]) + " [09:44] [info] present: 300 frames"
    m = re.search(r"RIG:CAMERA " + C.HEX12, cut)
    assert m is not None and len(m.group(1).split()) < 12
    assert C.camera_bits(m.group(1)) is None
    assert C.camera_bits(DRIFTED) is not None
    bad = C.check_camera((SNAP_TRANS, SNAP_ROT), None)
    assert bad["ok"] is None and "malformed" in bad["error"]
    out = C.camera_fields(None, (SNAP_TRANS, SNAP_ROT), {"trans": SNAP_TRANS, "rot": SNAP_ROT})
    assert out["camera_requested"] is None and out["camera_check"]["ok"] is None
    assert "camera_refixed" not in out
    try:
        C.camera_error(SNAP_TRANS, SNAP_ROT, SNAP_TRANS, SNAP_ROT[:8])
        raise AssertionError("short rotation accepted")
    except ValueError as exc:
        assert "malformed camera" in str(exc)


def test_identical_unnormalized_continue_camera_is_no_mismatch():
    rot = [-0.8947, 0.0, -0.4464, 0.1082, 0.9701, -0.2169, 0.4331, -0.2424, -0.868]
    assert C.check_camera(([0, 0, 0], rot), ([0, 0, 0], rot))["angle_deg"] < 0.01
