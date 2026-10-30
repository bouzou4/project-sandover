"""The light shaft census and the GOAL rule ``sp-light-shaft?`` agree on the game's defparts."""

from pathlib import Path

import pytest

from sandover import light_shafts

REPO = Path(__file__).resolve().parents[3]
LAUNCHER = REPO / "goal_src" / "jak1" / "engine" / "gfx" / "sprite" / "sparticle" / "sparticle-launcher.gc"


def test_rule_on_bodies():
    shaft = "(:texture (flare effects)) (:scale-x (meters 15) (meters 5)) (:rot-x 4) (:timer (seconds 8))"
    assert light_shafts.is_shaft(shaft)
    assert not light_shafts.is_shaft(shaft.replace("(seconds 8)", "(seconds 1.5)"))  # a glow burst
    assert not light_shafts.is_shaft(shaft.replace("(meters 15)", "(meters 6)"))  # an orb's rays
    assert not light_shafts.is_shaft(shaft.replace("(:rot-x 4)", ""))  # a billboard
    assert not light_shafts.is_shaft(shaft + " (:vel-y (meters 0.01))")  # a moving sprite


def test_census_finds_the_two_shafts():
    if not (REPO / "goal_src" / "jak1" / "levels").is_dir():
        pytest.skip("no goal_src")
    rows = {r.part: r for r in light_shafts.census(REPO)}
    assert set(rows) == {666, 864}
    assert rows[666].direct == ["beach-part-grotto-1"]
    assert rows[864].groups == ["group-jungle-dapple-light-1"]


def test_goal_rule_uses_the_census_thresholds():
    src = LAUNCHER.read_text(encoding="utf-8")
    assert "(= (-> spec 0 initial-value) 4)" in src
    assert "(>= (-> spec 0 initial-valuef) (meters 10))" in src
    assert "(>= (-> spec 0 initial-value) (seconds 4))" in src
    assert "(if (not (and *sp-hide-light-shafts?* (sp-light-shaft? ,particle)))" in src
