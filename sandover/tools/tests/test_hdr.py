import numpy as np
import pytest

from sandover import hdr


def _synthetic(h=9, w=13):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.stack([x * 0.25, y * 0.4, (x + y) * 0.02 - 0.05], axis=2)


def test_round_trip(tmp_path):
    rgb = _synthetic()
    p = tmp_path / "a.exr"
    hdr.write_exr(p, rgb, {"sandoverTonemapMode": "filmic"})
    back, attrs = hdr.read_exr(p)
    assert back.shape == rgb.shape
    np.testing.assert_allclose(back, rgb.astype(np.float16).astype(np.float32))
    assert attrs == {"sandoverTonemapMode": "filmic"}


def test_stats():
    rgb = np.zeros((2, 2, 3), np.float32)
    rgb[0, 0] = [3.0, 0.5, 0.0]
    rgb[0, 1] = [1.5, 0.0, 0.0]
    s = hdr.stats(rgb)
    assert s["R"]["peak"] == 3.0
    assert s["R"]["over1"] == 50.0
    assert s["R"]["over2"] == 25.0
    assert s["G"]["over1"] == 0.0
    assert s["max"]["mean"] == pytest.approx(4.5 / 4)
    assert "peak 3.00" in hdr.stats_line(s)


def test_curves():
    rgb = np.array([[[0.5, 0.5, 0.5], [2.0, 1.0, 0.5], [4.0, 0.2, 0.1]]], np.float32)
    clamp = hdr.tonemap(rgb, "clamp")
    np.testing.assert_allclose(clamp[0, 1], [1.0, 1.0, 0.5])
    film = hdr.tonemap(rgb, "filmic", knee=0.8)
    assert film[0, 0, 0] == pytest.approx(0.5)  # identity below the knee
    assert 0.8 < film[0, 1, 0] < 1.0
    toon = hdr.tonemap(rgb, "toon")
    np.testing.assert_allclose(toon[0, 0], [0.5, 0.5, 0.5], rtol=1e-6)
    assert toon.max() <= 1.0


def test_toon_matches_shader_table():
    # the #240 table (tonemap_toon at knee 0.95, range 1.5, core whitening and halo clip off)
    rgb = np.array([[[1.0, 1.0, 1.0], [1.0, 0.5, 0.2], [0.8, 0.6, 0.3],
                     [3.0, 1.0, 0.2], [0.1, 0.3, 1.5]]], np.float32)
    toon = hdr.tonemap(rgb, "toon", white_start=1e9, white_full=2e9, halo_start=1e9, halo_full=2e9)
    np.testing.assert_allclose(toon[0, 0], [0.975] * 3, atol=1e-3)
    np.testing.assert_allclose(toon[0, 1], [1.0, 0.5, 0.2], atol=1e-6)  # below the knee
    np.testing.assert_allclose(toon[0, 2], [0.8, 0.6, 0.3], atol=1e-6)
    np.testing.assert_allclose(toon[0, 3], [1.0, 0.421, 0.190], atol=1e-3)  # lava
    np.testing.assert_allclose(toon[0, 4], [0.133, 0.257, 1.0], atol=1e-3)  # blue vent


def test_summary_line(tmp_path):
    png = tmp_path / "shot.png"
    png.write_bytes(b"")
    assert hdr.summary_line(png) is None
    hdr.write_exr(tmp_path / "shot.exr", _synthetic())
    assert hdr.summary_line(png).startswith("HDR peak")


def test_cli_preview(tmp_path, capsys):
    p = tmp_path / "a.exr"
    hdr.write_exr(p, _synthetic(), {"sandoverTonemapMode": "clamp"})
    out = tmp_path / "a-toon.png"
    assert hdr.main(["stats", str(p), "--preview", str(out), "--curve", "toon",
                     "--toon-knee", "0.9", "--toon-range", "2"]) == 0
    assert out.exists()
    printed = capsys.readouterr().out
    assert "TonemapMode=clamp" in printed and ">1.0 %" in printed


def test_opencv_reads_our_exr(tmp_path, monkeypatch):
    monkeypatch.setenv("OPENCV_IO_ENABLE_OPENEXR", "1")
    cv2 = pytest.importorskip("cv2")
    rgb = _synthetic()
    p = tmp_path / "a.exr"
    hdr.write_exr(p, rgb, {"sandoverTonemapMode": "clamp"})
    im = cv2.imread(str(p), cv2.IMREAD_UNCHANGED)
    assert im is not None and im.shape == rgb.shape
    np.testing.assert_array_equal(im[..., ::-1], rgb.astype(np.float16).astype(np.float32))


def test_toon_below_white_start_is_hue_kept():
    # the #240 table is unchanged with the core whitening off, and every row with a max channel
    # under white_start (2.0) is unchanged with the defaults when the halo clip is off
    rgb = np.array([[[1.0, 1.0, 1.0], [1.0, 0.5, 0.2], [0.8, 0.6, 0.3],
                     [3.0, 1.0, 0.2], [0.1, 0.3, 1.5], [0.3, 0.6, 1.99]]], np.float32)
    off = hdr.tonemap(rgb, "toon", white_start=1e9, white_full=2e9, halo_start=1e9, halo_full=2e9)
    np.testing.assert_allclose(off[0, 3], [1.0, 0.421, 0.190], atol=1e-3)
    on = hdr.tonemap(rgb, "toon", halo_start=1e9, halo_full=2e9)
    keep = [0, 1, 2, 4, 5]
    np.testing.assert_array_equal(on[0, keep], off[0, keep])


def test_toon_halo_clips_like_vanilla_sky_keeps_hue():
    # EXR halo values: blue eco (runs/20260926-1011-play emitter-cores/0009), a green vent, and
    # the overbright sky (0004, 0001), which stays under halo_start
    halo = np.array([[[0.60, 0.91, 1.95], [0.9, 1.85, 0.4]]], np.float32)
    np.testing.assert_allclose(hdr.tonemap(halo, "toon")[0], np.minimum(halo[0], 1.0), atol=1e-6)
    old = hdr.tonemap(halo, "toon", halo_start=1e9, halo_full=2e9)[0]
    assert old[0, 1] < 0.6 and old[1, 0] < 0.6  # before: blue to violet, lime to pure green
    sky = np.array([[[0.73, 0.67, 1.06], [0.52, 0.72, 1.12]]], np.float32)
    off = hdr.tonemap(sky, "toon", halo_start=1e9, halo_full=2e9)
    np.testing.assert_array_equal(hdr.tonemap(sky, "toon"), off)


def test_toon_halo_ramp_is_smooth_and_monotonic():
    s = np.linspace(1.0, 2.2, 97, dtype=np.float32)
    rgb = np.stack([0.3 * s, 0.45 * s, s], axis=-1)[None]
    out = hdr.tonemap(rgb, "toon")[0]
    # red leaves the 25% desaturation for the clip: flat to within 0.5/255
    assert (np.maximum.accumulate(out, axis=0) - out).max() < 0.5 / 255
    np.testing.assert_allclose(out[s >= 1.8], np.minimum(rgb[0][s >= 1.8], 1.0), atol=1e-6)



def test_toon_hot_core_whitens_like_vanilla():
    # EXR core values of #263: blue eco (0002) and wall torch (0020)
    eco = np.array([[[1.32, 1.82, 4.15]]], np.float32)
    torch = np.array([[[6.31, 1.30, 0.32]]], np.float32)
    np.testing.assert_allclose(hdr.tonemap(eco, "toon")[0, 0], [1.0, 1.0, 1.0], atol=1e-6)
    t = hdr.tonemap(torch, "toon")[0, 0]
    np.testing.assert_allclose(t, [1.0, 1.0, 0.32], atol=1e-6)  # yellow, as the clamp gives
    old = hdr.tonemap(eco, "toon", white_start=1e9, white_full=2e9, halo_start=1e9,
                      halo_full=2e9)[0, 0]
    assert old[2] == pytest.approx(1.0) and old[0] < 0.8  # before: blue-violet, never white


def test_toon_white_ramp_is_smooth_and_monotonic():
    # a blue emitter brightening: blue stays 1, red and green rise from the hue-kept value to 1
    s = np.linspace(1.0, 5.0, 81, dtype=np.float32)
    rgb = np.stack([0.3 * s, 0.45 * s, s], axis=-1)[None]
    out = hdr.tonemap(rgb, "toon", white_start=2.0, white_full=3.5, halo_start=1e9,
                      halo_full=2e9)[0]
    below, above = s < 2.0, s >= 3.5
    np.testing.assert_array_equal(out[below], hdr.tonemap(rgb, "toon", white_start=1e9,
                                                          white_full=2e9, halo_start=1e9,
                                                          halo_full=2e9)[0][below])
    np.testing.assert_allclose(out[above], np.minimum(rgb[0][above], 1.0), atol=1e-6)
    ramp = out[(s >= 2.0) & (s < 3.5)]
    assert np.all(np.diff(ramp[:, 0]) >= -1e-6) and np.all(np.diff(ramp[:, 1]) >= -1e-6)
    assert np.all(out <= 1.0)


def test_core_rgb():
    img = np.zeros((10, 10, 3), np.uint8)
    img[2, 3] = [255, 255, 255]
    assert hdr.core_rgb(img, (0, 0, 10, 10)) == [255, 255, 255]
    assert hdr.core_rgb(img, (5, 5, 5, 5)) == [0, 0, 0]
    assert hdr.core_rgb(np.full((4, 4, 3), 0.5, np.float32), (0, 0, 4, 4)) == [128, 128, 128]


def test_cli_core(tmp_path, capsys):
    rgb = np.full((20, 20, 3), 0.2, np.float32)
    rgb[5:8, 5:8] = [1.32, 1.82, 4.15]
    p = tmp_path / "a.exr"
    hdr.write_exr(p, rgb)
    assert hdr.main(["core", str(p), "--curve", "toon", "--rect", "0,0,20,20"]) == 0
    assert "toon [255, 255, 255]" in capsys.readouterr().out
    assert hdr.main(["core", str(p), "--curve", "toon", "--white-start", "100",
                     "--white-full", "200", "--halo-start", "100", "--halo-full", "200",
                     "--rect", "0,0,20,20"]) == 0
    assert "toon [255, 255, 255]" not in capsys.readouterr().out
