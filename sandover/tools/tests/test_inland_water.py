"""Inland water table generator (``python -m sandover.inland_water``) and its presets."""

from __future__ import annotations

import re

import pytest
import yaml

from sandover import inland_water as W
from sandover import presets

GOAL = W.checkout() / "goal_src" / "jak1"
GFX = (W.checkout() / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")


def _goal() -> dict[str, str]:
    return W.read_goal(W.Sources(*(None,) * 4, goal_src=GOAL))


def _have_data() -> bool:
    try:
        src = W.default_sources()
    except FileNotFoundError:
        return False
    return (src.raw_obj / "water-anim-training-ag.go").exists() and src.rip.is_dir()


def test_look_table_and_skelgroups():
    text = (GOAL / "engine/common-obs/water-anim.gc").read_text(encoding="utf-8")
    looks = W.parse_look_table(text)
    groups = W.parse_skelgroups(text)
    assert looks[36] == "*water-anim-training-lake-sg*"
    assert groups[looks[37]].model == "water-anim-darkcave-water-with-crystal-lod0"
    assert groups[looks[44]].art_group == "water-anim-village1"


def test_type_info_from_source():
    goal = _goal()
    types, parent = W.subtypes(goal, W.BASE_TYPE)
    assert {"cave-water", "sunken-water", "mud", "dark-eco-pool", "helix-dark-eco"} <= types
    sunken = W.parse_type_info(goal, parent, "sunken-water")
    assert sunken.query and not sunken.mud
    assert sunken.tint[None] == (0.125, 0.225, 0.22, 0.5)
    cave = W.parse_type_info(goal, parent, "cave-water")
    assert not cave.query
    assert cave.tint[37] == cave.tint[14] == (0.2, 0.1, 0.3, 0.75)
    assert W.parse_type_info(goal, parent, "jungle-water").tint[None] == (0.01, 0.45, 0.5, 0.75)
    assert W.parse_type_info(goal, parent, "mud").mud


@pytest.mark.skipif(not _have_data(), reason="needs decompiler_out and the rip")
def test_surfaces_and_table_are_current():
    t = W.build(W.default_sources())
    by = {(s["level"], s["name"]): s for s in t["surfaces"]}
    kinds = {}
    for s in t["surfaces"]:
        kinds[s["kind"]] = kinds.get(s["kind"], 0) + 1
    assert kinds == {"clear": 24, "mud": 11, "dark-eco": 10, "other": 3}
    # the purple cave pools are clear water drawn by Merc2 after SSAO
    for key in (("darkcave", "cave-water-3"), ("maincave", "cave-water-2")):
        s = by[key]
        assert (s["kind"], s["renderer"], s["bucket"]) == ("clear", "merc2", "MERC_WATER_LEVEL0/1")
        assert not s["sky"]
    # sunken pools keep their ripple query: Generic2, water bucket
    assert by[("sunken", "sunken-water-3")]["bucket"] == "GENERIC_WATER_LEVEL0/1"
    assert all(s["sink"] == "water" for s in t["surfaces"] if s["kind"] == "clear")
    assert all(s["texture"] == "mis-quicksand-temp" for s in t["surfaces"] if s["kind"] == "mud")
    textures = {c["texture"] for c in t["textures"]}
    assert textures == {"cv-water", "sun-water", "mis-quicksand-temp"}
    inc = (W.checkout() / W.INC_REL).read_text(encoding="utf-8")
    assert inc == W.cpp_table(t), "InlandWater.inc is stale: run python -m sandover.inland_water"


def test_inland_forms():
    off = presets.goal_forms("inland-water", "off")
    assert off == [presets.INLAND_EXTERN, "(pc-set-inland-water 11 0)", "(pc-set-inland-water 0 0)",
                   "(pc-set-inland-water 5 0)"]
    assert list(presets.presets("inland-water")) == ["inland"]
    on = presets.goal_forms("inland-water", "inland")
    assert on[0] == presets.INLAND_EXTERN
    assert "(pc-set-inland-water 1 1500000)" in on
    assert on.index("(pc-set-inland-water 1 1500000)") < on.index(f"(pc-set-inland-water 0 {presets.SCALE})")


def test_inland_options_match_engine():
    opts = re.search(r"enum class InlandWaterOption \{([^}]*)\}", GFX).group(1)
    index = {n: int(v) for n, v in re.findall(r"(\w+) = (\d+),", opts)}
    assert index["PICKED"] == presets.INLAND_PICKED
    for part, (toggle, keys) in presets.INLAND_PARTS.items():
        assert index[part.upper()] == toggle
        for key, i in keys.items():
            name = key.upper() if part == "water" else f"MUD_{key.upper()}"
            assert index[name] == i, (part, key)


def test_inland_preset_matches_engine_defaults():
    values = presets.resolve("inland-water", "inland")
    assert re.search(r"bool inland_water = true;", GFX)
    assert re.search(r"bool inland_mud = true;", GFX)
    assert re.search(r"bool inland_water_picked = false;", GFX)  # the play look is `inland`
    for part, (_, keys) in presets.INLAND_PARTS.items():
        for key in keys:
            field = f"inland_water_{key}" if part == "water" else f"inland_mud_{key}"
            m = re.search(rf"float {field} = ([0-9.]+)f;", GFX)
            assert m and float(m.group(1)) == pytest.approx(values[part][key]), field


def test_manifest_looks_match_presets():
    doc = yaml.safe_load((W.checkout() / "sandover/shots/inland-water.yaml").read_text(encoding="utf-8"))
    # P and B2 were losing arms (revision 1's B, clearer water), no longer kept as presets
    arms = {"A": "off", "B1": "inland"}
    seen = set()
    for shot in doc["shots"]:
        forms = shot.get("goals_after_load", [])
        arm = shot["name"].rsplit("-", 1)[1]
        seen.add(arm)
        if arm == "C":
            assert not any("pc-set-inland-water" in f for f in forms), shot["name"]
            continue
        if arm not in arms:
            continue
        joined = " ".join(forms)
        assert all(f in joined for f in presets.goal_forms("inland-water", arms[arm])), shot["name"]
    assert seen == {"C", "A", "P", "B1", "B2"}
