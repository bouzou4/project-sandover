import re

import numpy as np
import pytest

from sandover import caustics, presets
from sandover.rig import perf as P

GFX = "game/graphics/gfx.h"
FRAG = "game/graphics/opengl_renderer/shaders/pbr_bg_frag.glsl"
VERT = "game/graphics/opengl_renderer/shaders/pbr_bg_vert.glsl"


def _read(rel: str) -> str:
    return (presets.root() / rel).read_text(encoding="utf-8")


def test_caustic_levels_come_from_the_moods():
    assert caustics.caustic_moods() == [("sunken", 4), ("sunkenb", 4)]
    cells = caustics.read_cpp_cells()
    assert list(cells) == ["sunken", "sunkenb"]
    assert all(0.5 < c < 20.0 for v in cells.values() for c in v.values())
    assert _read(str(caustics.CPP_TABLE)) == caustics.cpp_table(cells)


def test_caustic_cells_match_the_rips():
    if not caustics._rip("sunken").exists():
        pytest.skip("no 8-slot rips under SANDOVER_DATA")
    table = caustics.read_cpp_cells()
    for level in ("sunken", "sunkenb"):
        measured = caustics.level_cells(level, n=150000)
        for kind, cell in measured.items():
            assert table[level][kind] == pytest.approx(cell, rel=0.1), (level, kind)


def test_caustic_forms():
    assert presets.goal_forms("caustics", "off") == [presets.CAUSTIC_EXTERN, "(pc-set-caustics 0 0)"]
    forms = presets.goal_forms("caustics", "web")
    assert forms[0] == presets.CAUSTIC_EXTERN and forms[-1] == f"(pc-set-caustics 0 {presets.SCALE})"
    assert "(pc-set-caustics 5 900000)" in forms
    assert list(presets.presets("caustics")) == ["web", "off"]


def test_caustic_options_match_engine():
    opts = re.search(r"enum class CausticOption \{([^}]*)\}", _read(GFX)).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    for key, i in presets.CAUSTIC_PARAMS.items():
        assert engine[key.upper()] == i, key
    assert engine["CLOCK_S"] == 7
    assert "kStrengthRange = 2.f" in _read("game/graphics/opengl_renderer/background/Caustics.cpp")
    assert caustics.STRENGTH_RANGE == 2.0


def test_web_preset_is_the_engine_default():
    gfx = _read(GFX)
    values = presets.resolve("caustics", "web")
    assert re.search(r"int caustic_mode = 1;", gfx)
    for key in presets.CAUSTIC_PARAMS:
        if key == "mode":
            continue
        m = re.search(rf"float caustic_{key} = ([0-9.]+)f;", gfx)
        assert m and float(m.group(1)) == pytest.approx(values[key]), key


def test_shader_constants_match_the_mirror():
    frag, vert = _read(FRAG), _read(VERT)
    assert f"* {caustics.NOISE_INV_STD};" in frag
    assert "idx + 8192" in vert
    assert f"2.0 * e.a / {caustics.MASK_STRENGTH}, 1.0)" in vert
    assert "max(cau_a.z * c * fade, -1.0)" in frag
    k = caustics.LIMIT_KNEE
    assert f"if (p1 <= {k} || p1 <= p0)" in frag and f"{k} + {1 - k:.1f} * (1.0 - exp(-(p1 - {k}) / {1 - k:.1f}))" in frag
    for f in ("tfrag3.frag", "shrub.frag", "etie_base.frag"):
        assert "pbr_caustic_lit(fragment_color.rgb, T0.rgb)" in _read(f"game/graphics/opengl_renderer/shaders/{f}")


def test_soft_limit_eases_into_white_and_keeps_the_rest():
    p0 = np.array([0.3, 0.3, 0.3, 0.99, 0.5])
    p1 = np.array([0.7, 1.4, 3.0, 1.2, 0.4])
    out = caustics.soft_limit(p0, p1)
    assert out[0] == p1[0] and out[4] == p1[4]  # below the knee, or the term darkens
    assert np.all(out[1:3] < 1.0) and out[1] < out[2] and out[1] > 0.8
    assert out[3] == pytest.approx(0.99)  # already past the curve: the term adds nothing
    assert "kCausticEnvOffset = 8192" in _read("game/graphics/opengl_renderer/background/Caustics.h")


def test_strength_is_the_relative_loop_rms():
    flat = np.full((4, 3, 3), 60.0)
    assert np.allclose(caustics.strength(flat), 0.0)
    slots = np.zeros((4, 1, 3))
    slots[0] = 120.0  # all the light in one phase
    lum = np.einsum("snc,c->sn", slots, caustics.LUM)[:, 0]
    van = caustics.vanilla_weights() @ lum
    assert caustics.strength(slots)[0] == pytest.approx(van.std() / van.mean(), abs=2.0 / 255)


def test_noise_half_correlation():
    assert caustics.noise_half_corr(n=40000) == pytest.approx(caustics.NOISE_HALF_CORR, abs=0.02)


def test_noise_is_unit_variance_and_web_means_match():
    p = np.random.default_rng(3).random((200000, 3)) * 300.0 - 150.0
    n = caustics.noise(p, 0) * caustics.NOISE_INV_STD
    assert abs(n.mean()) < 0.01 and n.std() == pytest.approx(1.0, abs=0.01)
    assert abs(np.corrcoef(n, caustics.noise(p, 1))[0, 1]) < 0.02
    for name, look in caustics.looks().items():
        assert look["web"] > 0, name
        assert caustics.web_mean(look["web"], n=200000) == pytest.approx(look["web_mean"], abs=0.003)
    # amp = the share of the caustic light gathered into the lines / web_mean
    for name, look, gather in (("web", caustics.looks()["web"], 0.75), ("web-sharp", WEB_SHARP, 0.5)):
        assert look["amp"] * look["web_mean"] == pytest.approx(gather, abs=0.01), name


# the first web (#318), the look the maintainer found too sharp and too fast; no longer a preset
WEB_SHARP = {"mode": 1, "cell_scale": 0.25, "loop_hz": 1.875, "drift_mps": 0.25, "amp": 2.44, "web": 0.3,
             "web_mean": 0.2047}


def test_web_is_softer_and_slower_than_web_sharp():
    """The default after the maintainer's 'too sharp, too fast': about 30 % of web-sharp's gradient (the
    term's RMS slope per metre on a floor) and tempo (Jak 2's 2 s caustic loop)."""
    soft, sharp = caustics.looks()["web"], WEB_SHARP
    assert soft["loop_hz"] / sharp["loop_hz"] == pytest.approx(0.27, abs=0.02)
    assert soft["drift_mps"] / sharp["drift_mps"] == pytest.approx(0.27, abs=0.02)
    assert soft["web"] == pytest.approx(3 * sharp["web"])
    g = np.linspace(0, 15, 600)  # 2.5 cm: resolves web-sharp's 0.3 line half-width
    X, Z = np.meshgrid(g, g)
    P = np.stack([X.ravel(), np.full(X.size, 0.37), Z.ravel()], 1)

    def grad(look):
        f = np.maximum(caustics.field(P, 0.0, look, 7.6), -1.0).reshape(X.shape)
        gy, gx = np.gradient(f, g[1] - g[0])
        return np.sqrt((gx ** 2 + gy ** 2).mean())

    assert grad(soft) / grad(sharp) == pytest.approx(0.30, abs=0.02)


def test_field_is_zero_mean_and_loops_at_the_rate():
    look = dict(caustics.looks()["web"], web=0.0, amp=1.0, drift_mps=0.0)
    p = np.random.default_rng(5).random((50000, 3)) * 200.0
    f0 = caustics.field(p, 0.0, look, 6.0)
    assert abs(f0.mean()) < 0.02
    half = 0.5 / look["loop_hz"]
    assert np.allclose(caustics.field(p, half, look, 6.0), -f0, atol=1e-9)
    web = dict(caustics.looks()["web"], drift_mps=0.0)
    cells = np.full(len(p), 6.0)
    assert np.allclose(caustics.field(p, 1.0 / web["loop_hz"], web, cells), caustics.field(p, 0.0, web, cells),
                       atol=1e-9)


def test_vanilla_weights():
    w = caustics.vanilla_weights()
    assert w.shape == (32, 4) and np.allclose(w.sum(1), 1.25) and np.allclose(w.mean(0), 0.3125)


def test_manifest_looks_match_presets():
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load((presets.root() / "sandover/shots/sunken-caustics.yaml").read_text())
    hooks = set()
    off = presets.goal_forms("caustics", "off")
    assert doc["looks"]["A"][-1 - len(off):-1] == off
    # B1 and B2 were web-sharp and web-strong, losing arms no longer kept as presets
    for arm in ("A", "B1", "B2"):
        got = doc["looks"][arm]
        assert "(the-as uint 116)" in got[-1] and "(pc-set-caustics 7 1933333)" in got[-1]
        assert "(rig-set-clock" in got[-1] and "36000 13700" in got[-1]
        hooks.add(got[-1])
    hooks.add(doc["looks"]["C"][-1])
    assert len(hooks) == 1  # every arm pins the same phase
    names = [s["name"] for s in doc["shots"]]
    assert len(names) == 12 and all("-C" in n for n in names[:3])
    assert all(isinstance(s["camera"], dict) for s in doc["shots"])


def test_safe_clock_keeps_every_synced_water_safe():
    # sync periods (ticks), phase offsets and deadly-time from the sunken-water entities
    waters = [(1800, 0.0, 0.5), (2400, 0.0, 0.3), (2400, 0.0, 0.25), (1500, 0.5, 0.5)]
    for t in range(13700, 13700 + 451, 5):
        for period, offset, deadly in waters:
            phase = ((t % period) + offset * period) % period / period
            assert phase >= deadly, (t, period)


def test_perf_toggle():
    assert P.toggle_form("caustics", False) == "(pc-set-caustics 0 0)"
    assert "caustics" in P.OPT_IN
