import json
import struct

from sandover.rig import snapshot


def _hex(x: float) -> str:
    return f"{struct.unpack('<I', struct.pack('<f', x))[0]:X}"


def test_parse_state_and_sidecar(tmp_path):
    vals = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 1, 0, 0, 0, 1, 0, 0, 0, 1,
            64.0 * snapshot.DEGREES, 12.5] + [0.25] * 8
    line = "\x1b[0mSNAP:STATE 7 village1 \"village1-hut\" " + " ".join(_hex(v) for v in vals)
    st = snapshot.parse_state(line)
    assert st["index"] == 7 and st["level"] == "village1" and st["continue"] == "village1-hut"
    assert st["cam_rows"] == [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    assert abs(st["fov"] - 64.0) < 1e-3 and st["hour"] == 12.5 and st["mood_weights"] == [0.25] * 8
    data = snapshot.sidecar(st, ["(pc-set-fur 0 1000000)"], "abc123")
    assert list(data) == ["index", "time_utc", "level", "continue", "jak_pos", "cam_pos", "cam_rows",
                          "fov", "hour", "mood_weights", "toggles", "build_sha", "status", "note",
                          "tags", "annotations"]
    assert data["status"] == "kept" and data["note"] is None and data["tags"] == [] == data["annotations"]
    (tmp_path / "0007.png").write_bytes(b"png")
    out = snapshot.write_sidecar(tmp_path, data, png_timeout=5)
    assert json.loads(out.read_text())["index"] == 7


def test_parse_state_none_fields():
    line = "SNAP:STATE 1 none none" + " 0" * 25
    st = snapshot.parse_state(line)
    assert st["level"] is None and st["continue"] is None
    assert snapshot.parse_state("SNAP:WARP 1") is None


def test_goal_float_round_trips_without_exponent():
    from sandover.rig.capture import goal_float

    for v in (2.4e-07, -1.5e-12, 0.1, -560712.0625, 1e20, 1.0, -0.0, 0.12345678):
        s = goal_float(v)
        assert "e" not in s.lower() and "." in s and float(s) == v
