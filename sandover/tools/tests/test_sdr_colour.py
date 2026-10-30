import numpy as np
from PIL import Image

from sandover import sdr_colour as sc


def _png(path, rgb):
    im = np.zeros((1440, 2560, 3), np.uint8)
    im[:] = rgb
    Image.fromarray(im).save(path)


def test_region_delta_identity_and_hue_shift():
    v = np.full((20, 20, 3), [0.4, 0.6, 0.9], np.float32)
    lv = sc.region_lab(v, (0, 0, 20, 20))
    d = sc.delta(lv, sc.region_lab(v.copy(), (0, 0, 20, 20)))
    assert d == {"hue_deg": 0.0, "chroma": 1.0, "L": 1.0, "dE": 0.0}
    # more red in a blue sky turns it toward violet: positive hue shift, as the old toon clip did
    c = np.full((20, 20, 3), [0.5, 0.6, 0.9], np.float32)
    assert sc.delta(lv, sc.region_lab(c, (0, 0, 20, 20)))["hue_deg"] > 1.0


def test_region_median_ignores_specks():
    v = np.full((10, 10, 3), 0.5, np.float32)
    c = v.copy()
    c[0, :3] = [1.0, 0.0, 0.0]
    assert sc.delta(sc.region_lab(v, (0, 0, 10, 10)), sc.region_lab(c, (0, 0, 10, 10)))["dE"] == 0.0
    assert sc.region_lab(v, (0, 0, 0, 5)) is None


def test_measure_matches_frames_by_shot_name(tmp_path):
    van, cand = tmp_path / "v", tmp_path / "c"
    van.mkdir()
    cand.mkdir()
    _png(van / "village1-hut-noon.png", (100, 150, 220))
    _png(cand / "village1-hut-noon.png", (100, 150, 220))
    _png(van / "beach-start-dusk.png", (100, 150, 220))
    res = sc.measure(van, cand)
    assert list(res) == ["village1-hut-noon"]
    assert set(res["village1-hut-noon"]) == set(sc.REGIONS["village1-hut-noon"])
    assert all(r["dE"] == 0.0 for r in res["village1-hut-noon"].values())
    assert sc.summary(res)["dE"] == 0.0
