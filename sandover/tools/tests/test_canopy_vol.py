from pathlib import Path

import numpy as np

from sandover.unbake import canopy as cp2
from sandover.unbake import canopy_vol as cv
from test_canopy import _alpha, _quad, _scene


def test_classify_keeps_shade_cards_and_crown_body():
    meshes, alphas, classes = _scene()
    keep, rep, body = cv.classify(meshes, alphas, classes, cv.Params())
    keep2, _ = cp2.classify(meshes, alphas, classes, cp2.Params())
    assert set(keep) == set(keep2) == {"crown"}
    assert (keep["crown"] == keep2["crown"]).all()  # the shade cards are the v2 ones
    assert (body["crown"] >= keep["crown"]).all()


def test_volume_counts_steep_cards_and_top_height():
    flat, uvf = _quad(0, 4, 0, 4, 10.0)
    a, b, c, d = (2, 8, 0), (2, 8, 4), (2.3, 12, 4), (2.3, 12, 0)  # steep card above the flat one
    steep = np.array([[a, b, c], [a, c, d]], np.float64)
    tri = np.concatenate([flat, steep]); uv = np.concatenate([uvf, uvf])
    shade = np.array([True, True, False, False])
    cm = cv.rasterize([(tri, uv, _alpha("solid"), shade)], cv.Params(blur=0.0, height_blur=0.0, opacity=0.5))
    j, i = int((2 - cm.z0) / cm.texel), int((2.1 - cm.x0) / cm.texel)
    assert abs(cm.coverage[j, i] - 0.5) < 1e-6  # shade: the flat card only
    assert abs(cm.volume[j, i] - 0.75) < 1e-6  # volume: both
    assert cm.height[j, i] < 9.0 and cm.top[j, i] > 10.0


def test_write_read_roundtrip(tmp_path):
    rng = np.random.default_rng(3)
    cov = rng.uniform(0, 1, (7, 9))
    hgt = rng.uniform(20, 60, (7, 9))
    top = hgt + rng.uniform(0, 5, (7, 9))
    vol = rng.uniform(0, 1, (7, 9))
    cm = cv.CanopyMap(-12.5, 340.0, 0.5, float(hgt.min()), float(top.max()), cov, hgt, top, vol, sky=False)
    path = tmp_path / "x.canopy"
    cv.write(path, cm)
    b = path.read_bytes()
    assert b[:4] == b"SOVC" and len(b) == cv.HEADER.size + 7 * 9 * 8
    back = cv.read(path)
    tol = (top.max() - hgt.min()) / 65535 + 1e-4
    assert np.allclose(back.height, hgt, atol=tol) and np.allclose(back.top, top, atol=tol)
    assert np.allclose(back.volume, vol, atol=1 / 65535) and back.sky is False


def test_level_skies(tmp_path):
    f = tmp_path / "level-info.gc"
    f.write_text("(define a\n  (new 'static 'level-load-info\n       :name 'jungle\n       :sky #t\n))\n"
                 "(define b\n       :name 'robocave\n       :sky #f\n)\n")
    assert cv.level_skies(f) == {"jungle": True, "robocave": False}


def test_v2_and_v3_readers_reject_each_other(tmp_path):
    rng = np.random.default_rng(1)
    h = rng.uniform(5, 9, (3, 4))
    p2, p3 = tmp_path / "a.canopy", tmp_path / "b.canopy"
    cp2.write(p2, cp2.CanopyMap(0.0, 0.0, 0.25, float(h.min()), float(h.max()), rng.uniform(0, 1, (3, 4)), h))
    cv.write(p3, cv.CanopyMap(0.0, 0.0, 0.25, float(h.min()), float(h.max()), h * 0, h, h, h * 0))
    for f, other in ((cp2.read, p3), (cv.read, p2)):
        try:
            f(other)
            raise AssertionError("read the other version")
        except ValueError:
            pass


def test_header_matches_engine_reader():
    repo = Path(__file__).resolve().parents[3]
    cpp = (repo / "game/graphics/opengl_renderer/background/CanopyVol.cpp").read_text(encoding="utf-8")
    assert cv.HEADER.size == 48 and "static_assert(sizeof(hdr) == 48);" in cpp
    assert f"constexpr u32 kVersion = {cv.VERSION};" in cpp
    assert f"constexpr u32 kFlagSky = {cv.FLAG_SKY};" in cpp
