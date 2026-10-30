import re
from pathlib import Path

import pytest

from sandover.mood_tables import LEVELS, QUICK_LEVELS
from sandover.vista import fog_model

SHADERS = Path(__file__).resolve().parents[3] / "game/graphics/opengl_renderer/shaders"


def test_village1_noon_matches_the_tables():
    v = fog_model.vanilla("village1", 12)
    assert (round(v.start_m), round(v.end_m)) == (64, 1640)
    assert v.color == (128.0, 180.0, 243.0)
    assert v.amount(64) == pytest.approx(0.0)
    assert v.amount(1640) == pytest.approx(0.9)
    assert v.amount(10000) == pytest.approx(0.9)


def test_finalboss_fight_slot():
    v = fog_model.vanilla("finalboss", 6, slot=3)
    assert (round(v.start_m), round(v.end_m)) == (222, 467)
    assert v.color == (45.0, 0.0, 96.0)


@pytest.mark.parametrize("level", [lv for lv in LEVELS if lv not in QUICK_LEVELS])
def test_combine_is_the_larger_of_the_two(level):
    for hour in range(24):
        for r in fog_model.curves(level, hour + 0.5):
            te = 1 - (1 - r["unified"]) * (1 - r["haze"])
            assert r["combine"] == pytest.approx(max(r["vanilla"], te), abs=1e-6), (level, hour, r)
            if te <= r["vanilla"]:
                assert r["combine_de"] < 0.5, (level, hour, r)


def test_master_was_thinner_and_add_overshot():
    rows = {r["m"]: r for r in fog_model.curves("village1", 12)}
    assert rows[800]["master"] < rows[800]["vanilla"] < rows[800]["add"]
    fb = {r["m"]: r for r in fog_model.curves("finalboss", 6, 450, slot=3)}
    assert fb[3200]["combine"] == pytest.approx(fb[3200]["vanilla"])


def test_neighbour_tint_only_in_the_excess():
    for r in fog_model.curves("jungle", 12, 200, nb="training"):
        assert r["combine_de"] < 0.5, r


def test_every_unified_fog_shader_combines():
    for f in SHADERS.glob("*.vert"):
        src = f.read_text()
        if re.search(r"float atmo_fog\(", src):
            assert "atmo_combine(" in src, f.name
