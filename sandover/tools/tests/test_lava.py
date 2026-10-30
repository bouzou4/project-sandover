"""Lava rules: presets against the engine, the round manifest, the generated volume table."""

from __future__ import annotations

import re

import pytest

from sandover import presets
from sandover.bridge_sources import bridge_text
from sandover.lights import lava_volumes as LV


def _gfx() -> str:
    return (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")


def test_lava_forms():
    off = presets.goal_forms("lava", "off")
    assert off == [presets.LAVA_EXTERN, "(pc-set-lava 0 0)", "(pc-set-lava 5 0)"]
    assert list(presets.presets("lava")) == ["strong"]
    strong = presets.goal_forms("lava", "strong")
    assert strong[0] == presets.LAVA_EXTERN
    assert all(f"(pc-set-lava {t} {presets.SCALE})" in strong for t in (0, 5))
    assert strong[-1] == f"(pc-set-lava 5 {presets.SCALE})"


def test_lava_options_match_engine():
    opts = re.search(r"enum class LavaOption \{([^}]*)\}", _gfx()).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    for direction, (toggle, keys) in presets.LAVA_DIRECTIONS.items():
        assert engine[direction.upper()] == toggle
        for key, i in keys.items():
            name = f"{direction}_{key}".upper()
            assert engine[name] == i, name


def test_lava_preset_matches_engine_defaults():
    gfx = _gfx()
    values = presets.resolve("lava", "strong")
    for direction, (_, keys) in presets.LAVA_DIRECTIONS.items():
        assert re.search(rf"bool lava_{direction} = true;", gfx), direction
        for key in keys:
            m = re.search(rf"float lava_{direction}_{key} = ([0-9.]+)f;", gfx)
            assert m and float(m.group(1)) == pytest.approx(values[direction][key]), key


def test_lava_manifest_looks_match_presets():
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load((presets.root() / "sandover/shots/lava-rules.yaml").read_text())
    assert set(doc["looks"]) == {"V", "A", "B", "C"}
    pin, noview = "(pc-set-lava 14 1000000)", "(pc-set-lava 10 0)"
    # B was the moderate look, a losing arm no longer kept as a preset
    for key, (look, extra) in {"A": ("off", [noview]), "C": ("strong", [pin, noview])}.items():
        forms = presets.goal_forms("lava", look) + extra
        assert doc["looks"][key][-len(forms):] == forms, key
    assert all("rig-set-clock" in doc["looks"][k][0] for k in doc["looks"])
    assert doc["defaults"]["particles"] is False
    names = [s["name"] for s in doc["shots"]]
    assert len(names) == 11 and all(n.endswith("-vanilla") for n in names[:3])
    assert names[-2:] == ["firecanyon-diag-A-racer-death", "firecanyon-diag-B-racer-death"]


def test_lava_types_from_goal():
    types = LV.lava_types(LV.checkout() / "goal_src" / "jak1" / "levels")
    assert types == {"ogre-lava", "villagec-lava", "lavatube-lava"}


def test_footprint_from_vol_planes():
    m = LV.METER
    planes = [[-1.0, 0.0, 0.0, -10 * m], [1.0, 0.0, 0.0, 20 * m], [0.0, -1.0, 0.0, 0.0],
              [0.0, 0.0, -1.0, 40 * m], [0.0, 1.0, 0.0, 5 * m], [0.0, 0.0, 1.0, -30 * m]]
    assert LV.footprint(planes) == (10.0, 20.0, -40.0, -30.0)


def test_volume_table_is_current():
    try:
        text = LV.generate()
    except FileNotFoundError:
        pytest.skip("no decompiler_out")
    inc = LV.checkout() / LV.INC_REL
    assert inc.read_text(encoding="utf-8") == text
    assert '"ogre", 20313, "ogre-lava-1"' in text


def test_reapply_resets_lava_options():
    """sandover-settings-reapply sends pc-set-lava RESET, which restores every lava option."""
    root = presets.root()
    gc = (root / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    body = gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    opts = re.search(r"enum class LavaOption \{([^}]*)\}", _gfx()).group(1)
    reset = int(re.search(r"RESET = (\d+)", opts).group(1))
    assert f"(pc-set-lava {reset} " in body
    km = bridge_text(root)
    case = km[km.index("case LavaOption::RESET"):]
    case = case[:case.index("break;")]
    fields = set(re.findall(r"\b(lava_\w+) = ", _gfx()))
    assert fields and all(f"s.{f} = d.{f};" in case for f in fields), fields
