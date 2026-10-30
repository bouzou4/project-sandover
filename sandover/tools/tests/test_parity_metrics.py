import numpy as np

from sandover.parity import gate, metrics


def _img(seed=0, h=96, w=128):
    rng = np.random.default_rng(seed)
    base = np.linspace(20, 230, w, dtype=np.float32)[None, :, None]
    img = base + rng.normal(0, 12, (h, w, 3))
    img[..., 1] *= 0.8
    return np.clip(img, 0, 255).astype(np.uint8)


def test_flip_identical_is_zero():
    a = _img()
    mean, fmap = metrics.flip(a, a)
    assert mean == 0.0 and fmap.shape == a.shape[:2]


def test_precheck_passes_small_change():
    a = _img()
    assert metrics.precheck(a, metrics.offset(a, 1)).ok


def test_precheck_flags_black_size_and_swap():
    a = _img()
    assert metrics.precheck(a, np.zeros_like(a)).reason == "black frame"
    assert not metrics.precheck(a, a[:-1]).ok
    assert "swapped" in metrics.precheck(a, a[..., ::-1]).reason


def test_perturbations_register():
    a = _img()
    for fn in metrics.PERTURBATIONS.values():
        b = fn(a)
        assert b.shape == a.shape and b.dtype == np.uint8
        assert metrics.flip(a, b)[0] > 0


def test_gamma_nudge_moves_mid_grey_one_level():
    grey = np.full((2, 2, 3), 128, np.uint8)
    assert int(metrics.gamma_nudge(grey, 1.0)[0, 0, 0]) == 127


def test_shift_repeats_edge():
    a = np.arange(12, dtype=np.uint8).reshape(1, 4, 3)
    s = metrics.shift(a, 1, 0)
    assert (s[:, 0] == a[:, 0]).all() and (s[:, 1:] == a[:, :-1]).all()


def test_bands_halfway_between_floor_and_smallest_perturbation():
    pert = {"a": {"min": 0.01}, "b": {"min": 0.004}}
    b = gate.bands([0.0, 0.001, 0.002], pert)
    assert abs(b["F0"] - np.percentile([0.0, 0.001, 0.002], 99)) < 1e-12
    assert b["registers_kind"] == "b" and abs(b["threshold"] - (b["F0"] + 0.004) / 2) < 1e-12
    assert gate.bands([0.01], pert)["m"] == 0.0


def test_cal_key():
    assert gate.cal_key("auto-v1", (2560, 1440), 4, "abc") == "auto-v1|2560x1440|msaa4|abc"
