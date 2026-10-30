import numpy as np

from sandover.assets.alpha import normalize


def test_flat_vanilla_snaps_noise_to_flat_value():
    alpha = np.array([113, 129, 120, 128], dtype=np.uint8)
    out, action, _ = normalize(alpha, (128, 128))
    assert action == "flattened"
    assert (out == 128).all()


def test_flat_nonopaque_vanilla_snaps_to_its_own_value():
    alpha = np.array([49, 63, 55], dtype=np.uint8)
    out, action, _ = normalize(alpha, (64, 64))
    assert action == "flattened"
    assert (out == 64).all()


def test_real_transparency_rescales_peak_to_128():
    alpha = np.array([0, 67, 134], dtype=np.uint8)
    out, action, _ = normalize(alpha, (0, 128))
    assert action == "rescaled"
    assert out[2] == 128
    assert out[0] == 0
    assert (out <= 128).all()


def test_no_vanilla_match_clamps():
    alpha = np.array([100, 146, 200], dtype=np.uint8)
    out, action, _ = normalize(alpha, None)
    assert action == "clamped_no_vanilla"
    assert (out <= 128).all()


def test_translucent_gradient_rescales_to_vanilla_peak():
    alpha = np.array([0, 40, 80], dtype=np.uint8)
    out, action, _ = normalize(alpha, (0, 64))
    assert action == "rescaled"
    assert out.max() == 64
