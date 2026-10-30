"""sandover.vr_mv: the geometric prediction of a head step's motion vectors."""

import math

import numpy as np

from sandover.vr_mv import eye_poses, predict


def _yaw(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, 0.0, -s], [0.0, 1.0, 0.0], [s, 0.0, c]])


def test_no_motion_predicts_zero():
    pose = (np.eye(3), np.array([125.0, 0.0, 0.0]))
    v = predict(64, 48, 1.8, 1.3, np.full((48, 64), 4096.0), pose, pose)
    assert np.abs(v).max() < 1e-9


def test_pure_rotation_on_the_sky_moves_the_centre_by_its_tangent():
    w, h, rtx = 400, 300, 1.5
    before = (np.eye(3), np.zeros(3))
    after = (_yaw(5.0), np.zeros(3))
    v = predict(w, h, rtx, 1.0, np.zeros((h, w)), before, after)
    # the centre pixel's direction was 5 degrees to one side in the previous frame
    expect = math.tan(math.radians(5.0)) / rtx * w / 2
    assert abs(abs(v[h // 2, w // 2, 0]) - expect) < 0.6
    assert abs(v[h // 2, w // 2, 1]) < 0.01


def test_translation_parallax_shrinks_with_depth():
    before = (np.eye(3), np.zeros(3))
    after = (np.eye(3), np.array([100.0, 0.0, 0.0]))
    near = predict(100, 100, 1.0, 1.0, np.full((100, 100), 1000.0), before, after)
    far = predict(100, 100, 1.0, 1.0, np.full((100, 100), 100000.0), before, after)
    assert abs(near[50, 50, 0]) > 50 * abs(far[50, 50, 0]) > 0


def test_eye_poses_from_log_lines():
    log = ("[xr] fake head: yaw 0.000 pitch 0.000 roll 0.000 deg, pos 0 0 0 m\n"
           "[xr] fake eye 0: rot 1 0 0 / 0 1 0 / 0 0 1 pos_m 0.03050 0.00000 0.00000 head_x_m -0.03050 tan -1 1 1 -1\n"
           "[xr] fake eye 1: rot 1 0 0 / 0 1 0 / 0 0 1 pos_m -0.03050 0.00000 0.00000 head_x_m 0.03050 tan -1 1 1 -1\n"
           "[xr] fake head: yaw 5.000 pitch 0.000 roll 0.000 deg, pos 0 0 0 m\n")
    p = eye_poses(log)
    assert list(p) == [(0.0, 0.0, 0.0), (5.0, 0.0, 0.0)]
    assert abs(p[(0.0, 0.0, 0.0)][0][1][0] - 0.0305 * 4096) < 1e-6
