import pytest

from sandover import vr_measure, vr_synth

VIEW_LOG = """\
[35:31] [info] [xr] view VIEW 0 flags 0xf pos -0.030500 0.000000 0.000000 quat 0.0000000 0.0000000 0.0000000 1.0000000 fov_rad -1.0733775 0.7582840 0.9257251 -0.9257251 rec 3400 3468
[35:31] [info] [xr] view VIEW 1 flags 0xf pos 0.030500 0.000000 0.000000 quat 0.0000000 0.0000000 0.0000000 1.0000000 fov_rad -0.7582840 1.0733775 0.9257251 -0.9257251 rec 3400 3468
[35:31] [info] [xr] view LOCAL 0 flags 0xf pos -0.069999 1.715805 0.043296 quat 0.0101650 0.7674026 -0.0468972 0.6393673 fov_rad -1.0733775 0.7582840 0.9257251 -0.9257251 rec 3400 3468
[48:42] [info] [xr] view VIEW 0 flags 0xf pos -0.030500 0.000000 0.000000 quat 0.0000000 0.0000001 -0.0000000 1.0000000 fov_rad -1.0733775 0.7582840 0.9257251 -0.9257251 rec 3400 3468
"""

PROBE_LOG = """\
[32:56] [info] bgprobe: frame 746 shot false eye -1 fb 105 3840x2160 | sky 20 tfrag 277 tie 401 shrub 42 merc 156 generic 0 sprite 254 ocean 22 tex 0 other 5 | off-target buckets: pre:75 stages actor-shadows:75 0:75 1:75 18:75
[36:30] [info] bgprobe: frame 515 shot true eye 0 fb 71 4491x3468 | sky 20 tfrag 91 tie 412 shrub 21 merc 105 generic 0 sprite 31 ocean 22 tex 0 other 5 | off-target buckets: none
[36:30] [info] bgprobe: frame 515 shot true eye 1 fb 71 4491x3468 | sky 20 tfrag 91 tie 412 shrub 21 merc 105 generic 0 sprite 31 ocean 22 tex 0 other 5 | off-target buckets: none
[36:30] [info] [xr] view VIEW 0 flags 0xf pos 0 0 0
"""

FAKE_LOG = """\
[35:45] [info] [xr] fake head pose on
[35:45] [info] [xr] fake head: yaw 0.000 pitch 0.000 roll 0.000 deg, pos 0.0000 0.0000 0.0000 m
[35:45] [info] [xr] fake eye 0: rot 1.000000 -0.000000 0.000000 / -0.000000 1.000000 -0.000000 / 0.000000 -0.000000 1.000000 pos_m 0.03050 0.00000 -0.00000 head_x_m -0.03050 tan -1.84177 0.94719 1.32898 -1.32898
[35:45] [info] [xr] fake eye 1: rot 1.000000 -0.000000 0.000000 / -0.000000 1.000000 -0.000000 / 0.000000 -0.000000 1.000000 pos_m -0.03050 0.00000 -0.00000 head_x_m 0.03050 tan -0.94719 1.84177 1.32898 -1.32898
[36:44] [info] [xr] fake head: yaw 10.000 pitch 0.000 roll 0.000 deg, pos 0.0000 0.0000 0.0000 m
[36:44] [info] [xr] fake eye 0: rot 0.984808 -0.000000 -0.173648 / -0.000000 1.000000 -0.000000 / 0.173648 -0.000000 0.984808 pos_m 0.03004 0.00000 -0.00530 head_x_m -0.03050 tan -1.84177 0.94719 1.32898 -1.32898
[36:44] [info] [xr] fake eye 1: rot 0.984808 -0.000000 -0.173648 / -0.000000 1.000000 -0.000000 / 0.173648 -0.000000 0.984808 pos_m -0.03004 0.00000 0.00530 head_x_m 0.03050 tan -0.94719 1.84177 1.32898 -1.32898
"""


def test_view_re_profile_takes_first_set():
    assert len(vr_synth.VIEW_RE.findall(VIEW_LOG)) == 4
    p = vr_synth.parse_profile(VIEW_LOG)
    assert p["recommended"] == [3400, 3468]
    assert p["ipd_m"] == pytest.approx(0.061)
    assert p["views"][0]["fov_rad"] == pytest.approx([-1.0733775, 0.7582840, 0.9257251, -0.9257251])
    assert p["views"][0]["quat"][1] == 0.0  # the later 0.0000001 line is ignored
    assert p["local"][0]["pos"][1] == pytest.approx(1.715805)
    assert p["local"][1] is None


def test_view_re_requires_both_eyes():
    with pytest.raises(ValueError):
        vr_synth.parse_profile(VIEW_LOG.splitlines()[0])


def test_probe_re_inventory():
    rows = vr_synth.parse_inventory(PROBE_LOG)
    assert len(rows) == 3
    leak, left, right = rows
    assert (leak["frame"], leak["shot"], leak["eye"], leak["fb"]) == (746, False, -1, 105)
    assert leak["size"] == "3840x2160" and leak["tie"] == 401 and leak["sprite"] == 254
    assert leak["off_target"].startswith("pre:75 stages actor-shadows:75")
    assert (left["shot"], left["eye"], left["size"], left["off_target"]) == (True, 0, "4491x3468", "none")
    assert right["eye"] == 1 and right["merc"] == 105


def test_fake_eye_re_by_pose():
    out = vr_measure.fake_eyes_by_pose(FAKE_LOG)
    assert set(out) == {(0.0, 0.0, 0.0), (10.0, 0.0, 0.0)}
    e0, e1 = out[(10.0, 0.0, 0.0)]
    assert e0["rot"][0, 2] == pytest.approx(-0.173648)
    assert e0["pos_m"] == pytest.approx([0.03004, 0.0, -0.0053])
    assert e1["head_x_m"] == pytest.approx(0.0305)
    assert e1["tan"] == pytest.approx([-0.94719, 1.84177, 1.32898, -1.32898])
    assert out[(0.0, 0.0, 0.0)][0]["rot"][1, 1] == 1.0
