import json
import os
import re
from pathlib import Path

import pytest

from sandover import presets

TOML = """
[presets.deep]
radius = 0.8
intensity = 0.9
bias = 0.02
blur_radius = 4
blur_sharpness = 1.0
merc_weight = 1.5
luma_floor = 4
cave_scale = 0.5
taps = 16

[round2.presets.deep-crease]
role = "candidate"
base = "deep"
crease_angle = 20.0
"""


def _load(monkeypatch, tmp_path):
    f = tmp_path / "ssao.toml"
    f.write_text(TOML, encoding="utf-8")
    monkeypatch.setattr(presets, "preset_file", lambda phase: f)


def test_round_candidate_merges_base(monkeypatch, tmp_path):
    _load(monkeypatch, tmp_path)
    p = presets.resolve("ssao", "round2/deep-crease")
    assert p["radius"] == 0.8 and p["crease_angle"] == 20.0 and p["taps"] == 16


def test_optional_params_reset_to_defaults(monkeypatch, tmp_path):
    _load(monkeypatch, tmp_path)
    plain = presets.goal_forms("ssao", "deep")
    assert "(pc-set-ssao-param 12 0)" in plain and "(pc-set-ssao-param 13 0)" in plain
    crease = presets.goal_forms("ssao", "round2/deep-crease")
    assert "(pc-set-ssao-param 12 20000000)" in crease
    assert crease[-1] == "(pc-set-ssao-param 0 1000000)"


VISTA_TOML = """
[presets.unified]
fog_start_m = 150
fog_density = 0.15
fog_height_m = 800

[presets.intent]
base = "unified"

[presets.intent.neighbour_fog.misty]
density_scale = 4.0
tint = [0.5, 1.0, 1.5]
max_fog = 0.8
haze_scale = 0.0

[presets.hazy]
base = "unified"
haze_density = 0.6
haze_start_m = 250
haze_tint = [1.25, 1.2, 1.0]
haze_max = 0.7
standin_levels = true
"""


def test_vista_neighbour_overrides(monkeypatch, tmp_path):
    f = tmp_path / "vista.toml"
    f.write_text(VISTA_TOML, encoding="utf-8")
    monkeypatch.setattr(presets, "preset_file", lambda phase: f)
    plain = presets.goal_forms("vista", "unified")
    assert plain[0] == '(pc-set-vista-fog "unified" 150 150 800)'
    assert not any('"misty"' in x for x in plain)
    forms = presets.goal_forms("vista", "intent")
    assert '(pc-set-vista-neighbour-fog "misty" 4000 #x4080c0 800)' in forms
    assert forms.index('(pc-set-vista-neighbour-fog "clear" -1 -1 -1)') < len(forms) - 3
    assert forms[-1] == '(pc-set-vista "neighbours")'
    assert presets.goal_forms("vista", "off") == ['(pc-set-vista "none")']
    assert '(pc-set-vista-haze 0 200 #x808080 1000)' in plain
    assert '(pc-set-vista-neighbour-haze "misty" 0)' in forms
    hazy = presets.goal_forms("vista", "hazy")
    assert '(pc-set-vista-haze 600 250 #xa09a80 700)' in hazy
    assert '(pc-set-vista-option 12 1)' in hazy
    assert '(pc-set-vista-option 12 0)' in plain


EMISSIVE_TOML = """
[emissive_lights]
intensity = 1.0
radius_m = 4.0
falloff = 2.0
threshold = 0.02

[emissive_lights.presets.strong]
intensity = 3.0
"""


def test_emissive_light_forms(monkeypatch, tmp_path):
    f = tmp_path / "emissive.toml"
    f.write_text(EMISSIVE_TOML, encoding="utf-8")
    monkeypatch.setattr(presets, "preset_file", lambda phase: f)
    forms = presets.goal_forms("emissive", "default")
    assert forms[0] == presets.EMISSIVE_LIGHTS_EXTERN
    assert forms[1:] == ["(pc-set-emissive-lights 1 1000000)", "(pc-set-emissive-lights 2 4000000)",
                         "(pc-set-emissive-lights 3 2000000)", "(pc-set-emissive-lights 4 20000)",
                         "(pc-set-emissive-lights 0 1000000)"]
    assert "(pc-set-emissive-lights 1 3000000)" in presets.goal_forms("emissive", "strong")
    assert presets.goal_forms("emissive", "off")[-1] == "(pc-set-emissive-lights 0 0)"


def test_emissive_toml_has_light_defaults():
    p = presets.emissive_light_params("default")
    assert set(p) == set(presets.EMISSIVE_LIGHT_PARAMS + presets.WORLD_LIGHT_PARAMS
                         + presets.TEMPORAL_LIGHT_PARAMS + list(presets.EXTRA_LIGHT_PARAMS))


def test_emissive_temporal_forms():
    forms = presets.goal_forms("emissive", "default")
    assert "(pc-set-emissive-lights 13 250000)" in forms
    assert "(pc-set-emissive-lights 14 3000000)" in forms
    assert "(pc-set-emissive-lights 16 1000000)" in forms
    off = presets.goal_forms("emissive", "unsmoothed")
    assert "(pc-set-emissive-lights 13 1000000)" in off and "(pc-set-emissive-lights 16 0)" in off


def test_emissive_highlight_forms():
    forms = presets.goal_forms("emissive", "default")
    assert "(pc-set-emissive-lights 21 2000000)" in forms and "(pc-set-emissive-lights 22 1500000)" in forms
    # the switch follows its parameters, then the tile lights go on
    assert forms.index("(pc-set-emissive-lights 20 1000000)") > forms.index("(pc-set-emissive-lights 21 2000000)")
    assert forms[-1] == "(pc-set-emissive-lights 0 1000000)"
    # the picked look only; `unsmoothed` is the flicker tool's rig baseline
    assert list(presets.load("emissive")["emissive_lights"]["presets"]) == ["unsmoothed"]


def test_highlight_engine_defaults_match_toml():
    import re
    from sandover.paths import root
    gfx = (root() / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")
    p = presets.emissive_light_params("default")
    for field, key in [("light_highlights_strength", "highlight_strength"),
                       ("light_highlights_max", "highlight_max"),
                       ("light_highlights_damp", "highlight_damp")]:
        m = re.search(rf"float {field} = ([0-9.]+)f;", gfx)
        assert m and float(m.group(1)) == float(p[key]), field
    assert "bool light_highlights = true;" in gfx and p["highlights"] == 1.0


def test_emissive_world_light_forms(monkeypatch, tmp_path):
    f = tmp_path / "emissive.toml"
    extra = "threshold = 0.02\nworld_count = 16\nworld_arc = 0.005\nworld_glow = 0.03\n"
    f.write_text(EMISSIVE_TOML.replace("threshold = 0.02\n", extra), encoding="utf-8")
    monkeypatch.setattr(presets, "preset_file", lambda phase: f)
    forms = presets.goal_forms("emissive", "default")
    assert "(pc-set-emissive-lights 5 16000000)" in forms
    assert "(pc-set-emissive-lights 8 5000)" in forms
    assert "(pc-set-emissive-lights 6" not in " ".join(forms)
    assert "(pc-set-emissive-lights 18 30000)" in forms
    assert forms[-1] == "(pc-set-emissive-lights 0 1000000)"


WATER_TOML = """
[presets.subtle]
wave = 0.5
glint = 1.5
glint_exp = 96.0
fresnel = 0.35
sparkle = 0.8

[presets.waves]
wave = 0.3
glint = 1.5
glint_exp = 96.0
fresnel = 0.35
sparkle = 0.4
displace = 1.0
shore_foam = 0.8

[[presets.waves.waves]]
dir_deg = 20.0
amp_m = 0.2
length_m = 12.0
speed = 2.5
steep = 0.6
"""


def test_water_forms(monkeypatch, tmp_path):
    f = tmp_path / "water.toml"
    f.write_text(WATER_TOML, encoding="utf-8")
    monkeypatch.setattr(presets, "preset_file", lambda phase: f)
    old = presets.goal_forms("water", "subtle")
    # a round-1 preset turns the round-2 terms off and flattens every wave
    assert "(pc-set-water 7 0)" in old and "(pc-set-water 12 0)" in old
    assert "(pc-set-water 21 0)" in old and "(pc-set-water 36 0)" in old
    new = presets.goal_forms("water", "waves")
    assert "(pc-set-water 7 1000000)" in new and "(pc-set-water 12 800000)" in new
    assert "(pc-set-water 20 20000000)" in new and "(pc-set-water 22 12000000)" in new
    assert "(pc-set-water 26 0)" in new  # second wave flat
    assert new[-1] == "(pc-set-water 0 1000000)"
    assert presets.goal_forms("water", "off") == ["(pc-set-water 0 0)"]


def test_water_toml_resolves():
    assert list(presets.presets("water")) == ["bold"]
    assert not [k for k in presets.load("water") if k.startswith("round")]
    assert presets.goal_forms("water", "bold")[-1] == "(pc-set-water 0 1000000)"


def test_water_round3_forms(monkeypatch, tmp_path):
    f = tmp_path / "water.toml"
    f.write_text(WATER_TOML + """
[[wave_sets.lake.waves]]
dir_deg = 10.0
amp_m = 0.05
length_m = 5.0
speed = 1.0
steep = 0.4
""", encoding="utf-8")
    monkeypatch.setattr(presets, "preset_file", lambda phase: f)
    old = presets.goal_forms("water", "subtle")
    # revision 3 terms off when a preset leaves them out; the swell is flat
    assert "(pc-set-water 45 0)" in old and "(pc-set-water 47 0)" in old
    assert "(pc-set-water 41 0)" in old
    # the lake set goes to 60 + 5 * wave + field for every preset
    assert "(pc-set-water 60 10000000)" in old and "(pc-set-water 61 50000)" in old
    assert "(pc-set-water 66 0)" in old


def test_water_bold_matches_engine_defaults():
    bold = presets.resolve("water", "bold")
    assert len(bold["waves"]) == presets.WATER_WAVES
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    engine = {k: float(v) for k, v in re.findall(r"float water_(\w+) = (-?[0-9.]+)f;", gfx)}
    for key in ("sky_refl", "sky_haze_m", "amp_noise", "phase_noise", "noise_m",
                "shallow_depth_m", "shore_fade_near_m", "shore_fade_far_m", "shoal_m",
                "shoal_occ", "depth_blur_m"):
        assert engine[key] == bold[key]


def test_water_tiling_forms(monkeypatch, tmp_path):
    bold = presets.goal_forms("water", "bold")
    assert "(pc-set-water 54 1000000)" in bold and "(pc-set-water 57 10000000)" in bold
    assert "(pc-set-water 48 0)" in bold
    # a preset without the group keeps the old look
    f = tmp_path / "water.toml"
    f.write_text(WATER_TOML, encoding="utf-8")
    monkeypatch.setattr(presets, "preset_file", lambda phase: f)
    old = presets.goal_forms("water", "subtle")
    assert "(pc-set-water 54 0)" in old and "(pc-set-water 57 0)" in old
    f.write_text(WATER_TOML + "\n[presets.subtle.tiling]\nhexx = 1.0\n", encoding="utf-8")
    with pytest.raises(ValueError, match="hexx"):
        presets.goal_forms("water", "subtle")


def test_water_tiling_matches_engine():
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    engine = {k: float(v) for k, v in re.findall(r"float water_tile_(\w+) = ([0-9.]+)f;", gfx)}
    bold = presets.resolve("water", "bold")
    assert engine == {k: float(v) for k, v in bold["tiling"].items()}
    opts = re.search(r"enum class WaterOption \{([^}]*)\}", gfx).group(1)
    for key, (index, _) in presets.WATER_TILING.items():
        assert f"TILE_{key.upper()} = {index}," in opts
    waves = re.search(r"WaterWave water_waves\[kWaterWaves\] = \{(.*?)\};", gfx, re.S).group(1)
    rows = [[float(x) for x in re.findall(r"(-?[0-9.]+)f", r)]
            for r in re.findall(r"\{([^{}]*)\}", waves)]
    assert rows == [[w[k] for k in presets.WATER_WAVE_FIELDS] for w in bold["waves"]]


def test_water_aa_forms(monkeypatch, tmp_path):
    bold = presets.goal_forms("water", "bold")
    assert "(pc-set-water 85 -1500000)" in bold and "(pc-set-water 86 500000)" in bold
    assert "(pc-set-water 87 1000000)" in bold
    assert "(pc-set-water 88 8000000)" in bold and "(pc-set-water 89 4000000)" in bold
    # a preset without the group keeps full detail at every distance and a hard foam edge
    f = tmp_path / "water.toml"
    f.write_text(WATER_TOML, encoding="utf-8")
    monkeypatch.setattr(presets, "preset_file", lambda phase: f)
    old = presets.goal_forms("water", "subtle")
    assert "(pc-set-water 85 99000000)" in old and "(pc-set-water 86 100000000)" in old
    assert "(pc-set-water 87 0)" in old and "(pc-set-water 89 0)" in old
    f.write_text(WATER_TOML + "\n[presets.subtle.aa]\nlod = 1.0\n", encoding="utf-8")
    with pytest.raises(ValueError, match="lod"):
        presets.goal_forms("water", "subtle")


def test_water_aa_matches_engine():
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    engine = {k: float(v) for k, v in re.findall(r"float water_aa_(\w+) = (-?[0-9.]+)f;", gfx)}
    assert engine == {k: float(v) for k, v in presets.resolve("water", "bold")["aa"].items()}
    opts = re.search(r"enum class WaterOption \{([^}]*)\}", gfx).group(1)
    for key, (index, _) in presets.WATER_AA.items():
        assert f"AA_{key.upper()} = {index}," in opts
    # past the lake wave set (60 + 5 * 5 fields)
    assert min(i for i, _ in presets.WATER_AA.values()) >= 60 + 5 * presets.WATER_WAVES


def test_water_bold_wave_ratios_irrational_and_crests_unchanged():
    bold = presets.resolve("water", "bold")
    lengths = [w["length_m"] for w in bold["waves"]]
    # no two wave lengths within 2% of p/q, q <= 6 (the round-3 set had 9:4.5 and 48:16)
    for i, a in enumerate(lengths):
        for b in lengths[i + 1:]:
            r = max(a, b) / min(a, b)
            for q in range(1, 7):
                assert abs(r * q - round(r * q)) > 0.02 * q, (a, b, q)
    # steepness budget of water_setup_ocean (Materials.cpp): with the warp the chop's divisor
    # stays at its count, so bold's crests keep their round-5 sharpness
    t = bold["tiling"]
    warp_g = 3.0 * 1.866 * t["warp_m"] / t["warp_cell_m"]
    warp_k = ((1.0 + warp_g / 3.0) ** 2 + warp_g ** 2) ** 0.5
    noise_dk = (6.2831853 * bold["phase_noise"] + 2.0 * bold["amp_noise"]) * 2.13 / bold["noise_m"]
    steep = sum(min(w["steep"], 1.0) * (1.0 + noise_dk * w["length_m"] / 6.2831853) * warp_k
                for w in bold["waves"])
    assert steep <= 4.0


def test_water_bodies_match_engine():
    src = (presets.root() / "game/graphics/opengl_renderer/Materials.cpp").read_text(
        encoding="utf-8")
    table = src[src.index("kWaterBodies[] = {"):]
    table = table[:table.index("};")]
    engine = {lvl: presets.WATER_BODIES[int(b)]
              for lvl, b in re.findall(r'\{"(\w+)", (\d)\}', table)}
    assert engine == presets.water_bodies()
    assert presets.water_body("sunken") == "lake"
    assert presets.water_body("no-such-level") == "sea"


def test_tonemap_forms():
    toon = presets.goal_forms("tonemap", "toon")
    assert "(pc-set-hdr-target #t)" in toon
    assert "(pc-set-tonemap 0 2000000)" in toon and "(pc-set-tonemap 4 950000)" in toon
    assert "(pc-set-tonemap 5 1500000)" in toon
    assert "(pc-set-tonemap 6 2000000)" in toon and "(pc-set-tonemap 7 3500000)" in toon
    assert "(pc-set-tonemap 8 1200000)" in toon and "(pc-set-tonemap 9 1800000)" in toon
    assert "(pc-set-tonemap 0 1000000)" in presets.goal_forms("tonemap", "filmic")
    # the tonemap-mode setting's modes only; round arms (filmic 0.6, toon before #275) are gone
    assert list(presets.presets("tonemap")) == ["clamp", "filmic", "toon"]
    assert presets.goal_forms("tonemap", "off")[-1] == "(pc-set-hdr-target #f)"


def test_tonemap_toon_matches_engine_defaults():
    toon = presets.resolve("tonemap", "toon")
    assert presets.load("tonemap")["meta"]["recommended"] == "toon"
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    engine = {k: float(v) for k, v in re.findall(r"float tonemap_(\w+) = ([0-9.]+)f;", gfx)}
    assert engine["toon_knee"] == toon["toon_knee"]
    assert engine["toon_range"] == toon["toon_range"]
    assert engine["white_start"] == toon["white_start"]
    assert engine["white_full"] == toon["white_full"]
    assert engine["halo_start"] == toon["halo_start"]
    assert engine["halo_full"] == toon["halo_full"]
    opts = re.search(r"enum class TonemapOption \{([^}]*)\}", gfx).group(1)
    for key, index in presets.TONEMAP_PARAMS.items():
        assert f"{key.upper()} = {index}," in opts
    mode = re.search(r"enum class TonemapMode \{([^}]*)\}", gfx).group(1)
    assert f"TOON = {toon['mode']}" in mode


def test_vista_outdoor_levels_match_engine():
    src = (presets.root() / "game/graphics/opengl_renderer/Neighbours.cpp").read_text(
        encoding="utf-8")
    table = src[src.index("kOutdoor[] = {"):]
    table = table[:table.index("};")]
    assert re.findall(r'"(\w+)"', table) == presets.vista_outdoor_levels()
    outdoor = set(presets.vista_outdoor_levels())
    # the snapshot run's intrusion (ogre) and the rest of the indoor class stay off
    for level in ("ogre", "finalboss", "darkcave", "maincave", "robocave", "sunken", "sunkenb",
                  "citadel", "jungleb", "lavatube"):
        assert level not in outdoor
    for level in ("village1", "beach", "jungle", "misty", "rolling", "village2", "snow"):
        assert level in outdoor


def test_vista_neighbour_sets():
    src = (presets.root() / "game/graphics/opengl_renderer/Neighbours.cpp").read_text(
        encoding="utf-8")
    table = src[src.index("kAdjacency = {{"):]
    adjacency = re.findall(r'\{"(\w+)", "(\w+)"\}', table[:table.index("}};")])
    assert len(adjacency) == 20
    # the engine applies the same filter
    assert "if (other && is_outdoor(other)" in src
    assert presets.vista_neighbours("village3", adjacency) == {"snow"}
    assert presets.vista_neighbours("village2", adjacency) == {"firecanyon", "rolling", "swamp"}
    assert presets.vista_neighbours("jungle", adjacency) == {"village1"}
    assert presets.vista_neighbours("village1", adjacency) == {
        "firecanyon", "jungle", "training", "beach", "misty"}
    assert presets.vista_neighbours("ogre", adjacency) == set()


def test_vista_keep_out_matches_engine():
    src = (presets.root() / "game/graphics/opengl_renderer/Neighbours.cpp").read_text(
        encoding="utf-8")
    table = src[src.index("kKeepOut[] = {"):]
    rows = re.findall(r'\{"(\w+)", "(\w+)", \{([^}]*)\}, \{([^}]*)\}\}',
                      table[:table.index("};")])
    floats = lambda s: [float(x.strip().rstrip("f")) for x in s.split(",")]
    assert [(a, b, floats(lo), floats(hi)) for a, b, lo, hi in rows] == [
        (k["level"], k["inside"], k["box_min"], k["box_max"]) for k in presets.vista_keep_out()]
    assert "!neighbours::kept_out(l, cam_m)" in (
        presets.root() / "game/graphics/opengl_renderer/VistaRenderer.cpp").read_text(
            encoding="utf-8")
    # runs/20260927-0137-play 0002 camera (beach cliff top): firecanyon is not drawn
    assert presets.vista_kept_out("firecanyon", (-63.2, 45.9, -316.6))
    # village1-hut continue point: firecanyon is still drawn from village1
    assert not presets.vista_kept_out("firecanyon", (-156.0, 34.0, 188.0))
    assert not presets.vista_kept_out("jungle", (-63.2, 45.9, -316.6))


def test_swamp_forms_combined_look():
    off = presets.goal_forms("swamp", "off")
    assert off == [presets.SWAMP_EXTERN, "(pc-set-swamp 0 0)", "(pc-set-swamp 5 0)",
                   "(pc-set-swamp 9 0)"]
    assert list(presets.presets("swamp")) == ["swamp"]
    forms = presets.goal_forms("swamp", "swamp")
    assert forms[0] == presets.SWAMP_EXTERN
    assert all(f"(pc-set-swamp {t} {presets.SCALE})" in forms for t in (0, 5, 9))


def test_swamp_options_match_engine():
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class SwampOption \{([^}]*)\}", gfx).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    for direction, (toggle, keys) in presets.SWAMP_DIRECTIONS.items():
        assert engine[direction.upper()] == toggle
        for key, i in keys.items():
            name = f"{direction}_{key}".upper()
            assert engine[name] == i, name


def test_swamp_presets_match_engine_defaults():
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    values = presets.resolve("swamp", "swamp")
    for direction, (_, keys) in presets.SWAMP_DIRECTIONS.items():
        assert re.search(rf"bool swamp_{direction} = true;", gfx), direction
        for key in keys:
            m = re.search(rf"float swamp_{direction}_{key} = ([0-9.]+)f;", gfx)
            assert m and float(m.group(1)) == pytest.approx(values[direction][key]), key


def test_swamp_manifest_looks_match_presets():
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load((presets.root() / "sandover/shots/swamp-directions.yaml").read_text())
    assert set(doc["looks"]) == {"A", "B"}
    for key, look in {"A": "off", "B": "swamp"}.items():
        forms = presets.goal_forms("swamp", look)
        assert doc["looks"][key][-len(forms):] == forms, key
    names = [s["name"] for s in doc["shots"]]
    assert all(n.endswith("-vanilla") for n in names[:3]) and len(names) == 9


def test_shore_forms():
    off = presets.goal_forms("shore", "off")
    assert off == [presets.SWAMP_EXTERN, "(pc-set-swamp 16 0)"]
    assert list(presets.presets("shore")) == ["shore"]
    forms = presets.goal_forms("shore", "shore")
    assert forms[0] == presets.SWAMP_EXTERN and forms[-1] == f"(pc-set-swamp 16 {presets.SCALE})"
    assert len(forms) == 6


def test_shore_options_match_engine():
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class SwampOption \{([^}]*)\}", gfx).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    (toggle, keys), = presets.SHORE_DIRECTIONS.values()
    assert engine["SHORE"] == toggle
    for key, i in keys.items():
        assert engine[f"SHORE_{key.upper()}"] == i, key


def test_shore_preset_matches_engine_defaults():
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    values = presets.resolve("shore", "shore")
    for direction, (_, keys) in presets.SHORE_DIRECTIONS.items():
        assert re.search(rf"bool shore_{direction} = true;", gfx), direction
        for key in keys:
            m = re.search(rf"float shore_{direction}_{key} = ([0-9.]+)f;", gfx)
            assert m and float(m.group(1)) == pytest.approx(values[direction][key]), key


def test_shore_manifest_looks_match_presets():
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load((presets.root() / "sandover/shots/shore-wet.yaml").read_text())
    assert set(doc["looks"]) == {"A", "B", "C"}
    # every arm pins the game clock in the stage hook, so the waves match
    assert all("rig-set-clock" in doc["looks"][k][0] for k in "ABC") and len(doc["looks"]["C"]) == 1
    for key, look in {"A": "off", "B": "shore"}.items():
        forms = presets.goal_forms("shore", look)
        assert doc["looks"][key][-len(forms):] == forms, key
    # A and B differ only in the shore forms
    assert doc["looks"]["A"][:-2] == doc["looks"]["B"][:-6]
    names = [s["name"] for s in doc["shots"]]
    assert all(n.endswith("-vanilla") for n in names[:5]) and len(names) == 15
    assert all("(pc-set-water 90 120000000)" in doc["looks"][k] for k in "AB")


def test_misty_mist_matches_engine():
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class SwampOption \{([^}]*)\}", gfx).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    (direction, (toggle, keys)), = presets.MISTY_DIRECTIONS.items()
    assert engine["MISTY_MIST"] == toggle
    for key, i in keys.items():
        assert engine[f"MISTY_MIST_{key.upper()}"] == i, key
    assert presets.goal_forms("misty", "off") == [presets.SWAMP_EXTERN, f"(pc-set-swamp {toggle} 0)"]
    forms = presets.goal_forms("misty", "moderate")
    assert forms[-1] == f"(pc-set-swamp {toggle} {presets.SCALE})" and len(forms) == len(keys) + 2
    assert list(presets.presets("misty")) == ["moderate"]


def test_misty_mist_defaults_are_the_pick():
    """The engine defaults are the chosen preset (moderate), so play needs
    no forms; the values are the ones the round shot for B."""
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    chosen = presets.load("misty")["meta"]["chosen"]
    assert chosen == "moderate"
    values = presets.resolve("misty", chosen)["mist"]
    assert values == {"density": 0.03, "height_m": 6.0, "start_m": 10.0, "max": 0.7, "bright": 0.4,
                      "strength": 1.0}
    assert re.search(r"bool misty_mist = true;", gfx)
    for key in presets.MISTY_DIRECTIONS["mist"][1]:
        m = re.search(rf"float misty_mist_{key} = ([0-9.]+)f;", gfx)
        assert m and float(m.group(1)) == pytest.approx(values[key]), key


def test_misty_mist_manifest():
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load((presets.root() / "sandover/shots/misty-mist.yaml").read_text())
    forms = {"A": presets.goal_forms("misty", "off"), "B1": presets.goal_forms("misty", "moderate")}
    assert set(doc["looks"]) == {"A", "B1", "B2"}  # B2 was the strong mist, no longer a preset
    play = doc["looks"]["A"][:-len(forms["A"])]
    for k, f in forms.items():
        # the looks differ only in this effect's forms
        assert doc["looks"][k] == play + f, k
    shots, cams = doc["shots"], 3
    assert len(shots) == 4 * cams
    assert all(s["name"].endswith("-vanilla") and "goals_after_load" not in s for s in shots[:cams])
    for n, k in enumerate(("A", "B1", "B2"), 1):
        assert all(s["goals_after_load"] == doc["looks"][k] for s in shots[n * cams:(n + 1) * cams])


def test_sky_hue_forms_and_engine_defaults():
    assert list(presets.presets("sky-hue")) == ["moderate"]
    assert presets.goal_forms("sky-hue", "off") == ["(pc-set-shadow-option 30 0)"]
    assert presets.goal_forms("sky-hue", "moderate") == [
        "(pc-set-shadow-option 31 500000)", f"(pc-set-shadow-option 30 {presets.SCALE})"]
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class ShadowOption \{([^}]*)\}", gfx).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    assert engine["SKY_HUE"] == presets.SKY_HUE_TOGGLE
    assert engine["SKY_HUE_STRENGTH"] == presets.SKY_HUE_PARAMS["strength"]
    # the engine default is the chosen moderate look
    assert re.search(r"bool shadow_sky_hue = true;", gfx)
    m = re.search(r"float shadow_sky_hue_strength = ([0-9.]+)f;", gfx)
    assert float(m.group(1)) == pytest.approx(presets.resolve("sky-hue", "moderate")["strength"])


def test_sky_hue_manifest_arms_match_presets():
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load((presets.root() / "sandover/shots/sky-hued-shadows.yaml").read_text())
    for arm, look in {"A": "off", "B1": "moderate"}.items():  # B2 `full` is no longer a preset
        forms = presets.goal_forms("sky-hue", look)
        assert all(f in doc["variants"][arm] for f in forms), arm


def test_wetpbr_forms_and_engine():
    assert presets.goal_forms("wetpbr", "off") == [presets.SWAMP_EXTERN, "(pc-set-swamp 40 0)"]
    forms = presets.goal_forms("wetpbr", "l2")
    assert forms[0] == presets.SWAMP_EXTERN and forms[-1] == f"(pc-set-swamp 40 {presets.SCALE})"
    assert "(pc-set-swamp 45 700000)" in forms and "(pc-set-swamp 46 1300000)" in forms
    assert len(forms) == 8
    assert list(presets.presets("wetpbr")) == ["l2"]
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class SwampOption \{([^}]*)\}", gfx).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    for name, (_, keys) in presets.WETPBR_DIRECTIONS.items():
        assert engine["WET_PBR"] == 40
        assert all(engine[f"WET_PBR_{k.upper()}"] == i for k, i in keys.items())
    # on by default with the chosen l2, like shore_wet
    assert re.search(r"bool wet_pbr = true;", gfx)
    for key in presets.WETPBR_DIRECTIONS["wet"][1]:
        m = re.search(rf"float wet_pbr_{key} = ([0-9.]+)f;", gfx)
        assert m and float(m.group(1)) == pytest.approx(presets.resolve("wetpbr", "l2")["wet"][key])


def test_wetpbr_manifest_arms():
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load((presets.root() / "sandover/shots/wet-pbr.yaml").read_text())
    A, C = doc["looks"]["A"], doc["looks"]["C"]
    l2 = presets.goal_forms("wetpbr", "l2")
    n = len(l2)
    assert doc["looks"]["L2"][-n:] == l2 and doc["looks"]["L2"][:-n] == A[:-2]
    # B1 and L1 were losing arms, no longer presets: they differ from L2 only in the splash zone
    splash = ("(pc-set-swamp 45 ", "(pc-set-swamp 46 ")
    for arm in ("B1", "L1"):
        look = doc["looks"][arm]
        assert look[:-n] == A[:-2] and len(look) == len(doc["looks"]["L2"]), arm
        assert all(f.startswith(splash) for f in look[-n:] if f not in l2), arm
    assert A[-2:] == presets.goal_forms("wetpbr", "off")[-2:]
    assert "rig-set-clock" in A[0] and C == [A[0]]
    assert all(f in A for f in presets.goal_forms("shore", "shore")[1:])
    names = [s["name"] for s in doc["shots"]]
    assert len(names) == 20 and all(n.endswith("-vanilla") for n in names[:4])


def test_reapply_resets_glow_prop_options():
    """sandover-settings-reapply sends pc-set-emissive-lights 37, which restores the glow prop
    ranking options (35 fade distance, 36 slot cap) and the eco shot lights (27, 28) to their
    engine defaults."""
    import re
    from sandover.paths import root
    gc = (root() / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    assert "(pc-set-emissive-lights 37 " in gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    km = (root() / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    fn = km[km.index("void pc_set_emissive_lights("):]
    case = fn[fn.index("    case 37: {"):]
    case = case[:case.index("break;")]
    gfx = (root() / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")
    fields = set(re.findall(r"\b(world_lights_glow_(?:fade_m|slots)|eco_lights(?:_strength)?) = ", gfx))
    assert len(fields) == 4 and all(f"s.{f} = d.{f};" in case for f in fields), fields
    for n, f in [(35, "world_lights_glow_fade_m"), (36, "world_lights_glow_slots"),
                 (27, "eco_lights"), (28, "eco_lights_strength")]:
        body = fn[fn.index(f"    case {n}:"):]
        assert f"s.{f} =" in body[:body.index("break;")], n


def test_fur_size_rule_presets_match_engine_default():
    import re
    from sandover.paths import root as _root
    gfx = (_root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    exp = float(re.search(r"float fur_size_exp = ([0-9.]+)f;", gfx).group(1))
    ref = float(re.search(r"float fur_size_ref_m = ([0-9.]+)f;", gfx).group(1))
    assert presets.FUR_OPTIONAL["size_exp"][1] == exp
    assert presets.FUR_OPTIONAL["size_ref_m"][1] == ref
    assert f"(pc-set-fur 8 {round(exp * 1_000_000)})" in presets.goal_forms("fur", "medium")
    assert list(presets.presets("fur")) == ["medium"]


def test_reapply_resets_smaa_options():
    """sandover-settings-reapply sends pc-set-smaa 4, which restores the SMAA sub-options
    (1 threshold, 2 luma edges, 3 debug view) to their engine defaults."""
    import re
    from sandover.paths import root
    gc = (root() / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    assert "(pc-set-smaa 4 " in gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    km = (root() / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    fn = km[km.index("void pc_set_smaa("):]
    case = fn[fn.index("    case 4: {"):]
    case = case[:case.index("break;")]
    gfx = (root() / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")
    fields = set(re.findall(r"\b(smaa_\w+) = ", gfx))
    assert len(fields) == 3 and all(f"s.{f} = d.{f};" in case for f in fields), fields
    for n, f in [(1, "smaa_threshold"), (2, "smaa_luma"), (3, "smaa_debug")]:
        body = fn[fn.index(f"    case {n}:"):]
        assert f"s.{f} =" in body[:body.index("break;")], n


def test_reapply_resets_render_scale_sharpness():
    """sandover-settings-reapply sends pc-set-render-scale 3, which restores the RCAS sharpness
    (option 2, not a settings field) to its engine default."""
    from sandover.paths import root
    gc = (root() / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    assert "(pc-set-render-scale 3 " in gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    km = (root() / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    fn = km[km.index("void pc_set_render_scale("):]
    case = fn[fn.index("    case 3: {"):]
    assert "s.fsr_sharpness = d.fsr_sharpness;" in case[:case.index("break;")]
    body = fn[fn.index("    case 2:"):]
    assert "s.fsr_sharpness =" in body[:body.index("break;")]


def test_reapply_resets_terrain_sun_cover():
    """sandover-settings-reapply sends shadow option 20 at the engine default (1), so a rig shot
    that sets the terrain sun cover cannot leak it into the next."""
    from sandover.paths import root
    gc = (root() / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    assert "(pc-set-shadow-option 20 1000000)" in gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    gfx = (root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    assert "TERRAIN_SUN = 20," in gfx and "float shadow_terrain_sun = 1.f;" in gfx
    km = (root() / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    case = km[km.index("case ShadowOption::TERRAIN_SUN:"):]
    assert "s.shadow_terrain_sun = std::clamp(v, 0.f, 1.f);" in case[:case.index("break;")]


def test_water_glint_forms_and_engine_defaults():
    assert list(presets.presets("water-glint")) == ["glint"]
    assert presets.goal_forms("water-glint", "off") == ["(pc-set-water 94 0)"]
    b1 = presets.resolve("water-glint", "glint")
    assert b1["strength"] == pytest.approx(6.0)  # the chosen default
    assert presets.goal_forms("water-glint", "glint") == [
        f"(pc-set-water 95 {round(b1['strength'] * presets.SCALE)})",
        f"(pc-set-water 96 {round(b1['rough'] * presets.SCALE)})",
        f"(pc-set-water 97 {round(b1['peak'] * presets.SCALE)})",
        f"(pc-set-water 94 {presets.SCALE})"]
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class WaterOption \{([^}]*)\}", gfx).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    assert engine["SUN_GLINT"] == presets.WATER_GLINT_TOGGLE
    assert engine["SUN_GLINT_RESET"] == presets.WATER_GLINT_RESET
    assert {k: engine[f"SUN_GLINT_{n}"] for k, n in
            (("strength", "STRENGTH"), ("rough", "ROUGH"), ("peak", "PEAK"))} == presets.WATER_GLINT_PARAMS
    # off at boot (the settings row turns it on); the parameters' defaults are `glint`
    assert re.search(r"bool water_sun_glint = false;", gfx)
    assert "water_sun_glint_land" not in gfx
    for k, field in (("strength", "strength"), ("rough", "rough"), ("peak", "peak")):
        m = re.search(rf"float water_sun_glint_{field} = ([0-9.]+)f;", gfx)
        assert float(m.group(1)) == pytest.approx(b1[k]), k


def test_reapply_resets_water_glint():
    """sandover-settings-reapply sends pc-set-water SUN_GLINT_RESET, which restores 95-97, and
    forgets the glint row (glint?), whose update sends the switch (94)."""
    root = presets.root()
    gc = (root / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    body = gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    assert f"(pc-set-water {presets.WATER_GLINT_RESET} " in body
    assert "(set! (-> *sandover-applied* glint?) #f)" in body
    update = gc[gc.index("(defun sandover-settings-update"):]
    assert f"(pc-set-water {presets.WATER_GLINT_TOGGLE} (if (-> want glint?) 1000000 0))" in update
    km = (root / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    case = km[km.index("case WaterOption::SUN_GLINT_RESET"):]
    case = case[:case.index("break;")]
    for field in ("water_sun_glint_strength", "water_sun_glint_rough", "water_sun_glint_peak"):
        assert f"s.{field} = d.{field};" in case, field


def test_reapply_resets_world_sun_map_options():
    """sandover-settings-reapply sends shadow option 39, which restores every world sun map
    option (32-38) to its engine default, so a rig A/B arm cannot leak them into the next shot."""
    from sandover.paths import root
    gc = (root() / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    assert "(pc-set-shadow-option 39 1)" in gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    gfx = (root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class ShadowOption \{([^}]*)\}", gfx).group(1)
    engine = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}
    names = ["WORLD", "WORLD_ACTORS", "WORLD_WATER", "WORLD_SIZE", "WORLD_CASCADES",
             "WORLD_PENUMBRA", "WORLD_RANGE_M", "WORLD_RESET"]
    assert [engine[n] for n in names] == list(range(32, 40))
    km = (root() / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    case = km[km.index("case ShadowOption::WORLD_RESET:"):]
    case = case[:case.index("break;")]
    fields = re.findall(r"(shadow_world\w*) = ", gfx)
    assert len(fields) == 7
    tiered = {"shadow_world_size": 0, "shadow_world_cascades": 1}
    for f in fields:
        if f in tiered:
            assert f"s.{f} = kWorldShadowTier[std::clamp(s.shadow_quality, 0, 2)][{tiered[f]}];" in case, f
        else:
            assert f"s.{f} = d.{f};" in case, f


def test_shadow_quality_tier_sets_world_map_size_and_cascades():
    """Shadow option 40 (the settings page's shadow quality) sets the world sun map's size (35)
    and cascades (36) per the design note's low/medium/high table; option 39 resets them to the
    current tier's, and the engine defaults are the high tier (shadow_quality 2)."""
    from sandover.paths import root
    km = (root() / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    m = re.search(r"kWorldShadowTier\[3\]\[2\] = \{\{(\d+), (\d+)\}, \{(\d+), (\d+)\}, \{(\d+), (\d+)\}\};", km)
    tiers = [tuple(map(int, m.groups()[i:i + 2])) for i in (0, 2, 4)]
    note = (root() / "sandover/kb/findings/2026-10-08-world-sun-shadows.md").read_text(encoding="utf-8")
    table = {r.group(1): (int(r.group(2)), int(r.group(3)))
             for r in re.finditer(r"^\| (low|medium|high) \| (\d+) \| (\d) \|", note, flags=re.M)}
    assert tiers == [table["low"], table["medium"], table["high"]]
    case = km[km.index("case ShadowOption::QUALITY:"):]
    case = case[:case.index("break;")]
    assert "s.shadow_world_size = kWorldShadowTier[s.shadow_quality][0];" in case
    assert "s.shadow_world_cascades = kWorldShadowTier[s.shadow_quality][1];" in case
    gfx = (root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    assert "int shadow_quality = 2;" in gfx
    size = int(re.search(r"int shadow_world_size = (\d+);", gfx).group(1))
    casc = int(re.search(r"int shadow_world_cascades = (\d+);", gfx).group(1))
    assert (size, casc) == tiers[2]


def _sun_disc_engine():
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class SunOption \{([^}]*)\}", gfx).group(1)
    return gfx, {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) = (\d+)", opts)}


# what the selected sun-disc variant sent (its sidecars' post_load forms), the chosen default
SUN_DISC_PICK = {"size_deg": 2.2, "glare": 0.3, "core": 2000.0, "rim": 2.0, "sdr_core": 12.0,
                 "sdr_rim": 1.0, "fade": 0.35}


def test_sun_disc_forms_and_engine_defaults():
    assert list(presets.presets("sun-disc")) == ["sun"]
    assert presets.load("sun-disc")["meta"]["chosen"] == "sun"
    assert presets.goal_forms("sun-disc", "off") == ["(pc-set-sun-option 10 0)"]
    forms = presets.goal_forms("sun-disc", "sun")
    assert forms[0] == "(pc-set-sun-option 19 1)" and forms[-1] == f"(pc-set-sun-option 10 {presets.SCALE})"
    assert "(pc-set-sun-option 11 2200000)" in forms
    gfx, engine = _sun_disc_engine()
    assert engine["DISC"] == presets.SUN_DISC_TOGGLE and engine["DISC_RESET"] == presets.SUN_DISC_RESET
    names = {"size_deg": "DISC_SIZE_DEG", "glare": "DISC_GLARE", "core": "DISC_CORE",
             "rim": "DISC_RIM", "sdr_core": "DISC_SDR_CORE", "sdr_rim": "DISC_SDR_RIM",
             "fade": "DISC_FADE"}
    assert {k: engine[v] for k, v in names.items()} == presets.SUN_DISC_PARAMS


def test_sun_disc_defaults_are_the_pick():
    """The engine defaults and the one preset are the picked arm's values, so play needs no forms;
    off at boot (the vista switch turns it on)."""
    gfx, _ = _sun_disc_engine()
    assert re.search(r"bool sun_disc = false;", gfx)
    params = presets.resolve("sun-disc", "sun")
    assert {k: float(params[k]) for k in presets.SUN_DISC_PARAMS} == pytest.approx(SUN_DISC_PICK)
    for k, v in SUN_DISC_PICK.items():
        m = re.search(rf"float sun_disc_{k} = ([0-9.]+)f;", gfx)
        assert m and float(m.group(1)) == pytest.approx(v), k


def test_sun_disc_pick_matches_round_sidecars():
    data = os.environ.get("SANDOVER_DATA")
    sidecars = sorted(Path(data, "rounds/sky/sun-disc-001/B2").glob("*.json")) if data else []
    if not sidecars:
        pytest.skip("no sun-disc-001 B2 sidecars under SANDOVER_DATA")
    for f in sidecars:
        sent = " ".join(json.loads(f.read_text(encoding="utf-8"))["post_load"]["applied"])
        got = {int(i): int(v) for i, v in re.findall(r"\(pc-set-sun-option (\d+) (-?\d+)\)", sent)}
        for k, i in presets.SUN_DISC_PARAMS.items():
            assert got[i] == round(SUN_DISC_PICK[k] * presets.SCALE), (f.name, k)


def test_reapply_resets_sun_disc_options():
    """sandover-settings-reapply sends pc-set-sun-option 19, which restores every sun disc
    sub-option (11-18) to its engine default; the sun switch (sun?) turns the disc on and off."""
    gc = (presets.root() / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    assert "(pc-set-sun-option 19 1)" in gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    update = gc[gc.index("(defun sandover-settings-update"):]
    assert "(pc-set-sun-option 10 (if (-> want sun?) 1000000 0))" in update
    km = (presets.root() / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    fn = km[km.index("void pc_set_sun_option("):]
    case = fn[fn.index("    case SunOption::DISC_RESET: {"):]
    case = case[:case.index("break;")]
    gfx, engine = _sun_disc_engine()
    fields = set(re.findall(r"\b(sun_disc_\w+) = ", gfx))
    assert len(fields) == 8 and all(f"s.{f} = d.{f};" in case for f in fields), fields
    assert "s.sun_disc = " not in case
    for name, n in engine.items():
        if name.startswith("DISC_") and name != "DISC_RESET":
            assert 11 <= n <= 18, name
            body = fn[fn.index(f"    case SunOption::{name}:"):]
            assert "s.sun_disc_" in body[:body.index("break;")], name


def test_sun_disc_manifest_arms_match_presets():
    yaml = pytest.importorskip("yaml")
    doc = yaml.safe_load((presets.root() / "sandover/shots/sun-disc.yaml").read_text())
    for arm, look in {"A": "off", "B": "sun"}.items():
        assert doc["variants"][arm].split() == " ".join(presets.goal_forms("sun-disc", look)).split(), arm


def test_sky_bodies_forms_and_engine_defaults():
    """The sky bodies options (sun options 20-28, reset 29) match gfx.h; the jak3 preset is the
    engine default, so play needs no forms; disc is the #363 disc."""
    gfx, engine = _sun_disc_engine()
    names = {"style": "SKY_STYLE", "burst": "SKY_BURST", "wash": "SKY_WASH", "glow": "SKY_GLOW",
             "moon": "SKY_MOON", "moon_sdr": "SKY_MOON_SDR", "stars": "SKY_STARS",
             "moon_deg": "SKY_MOON_DEG", "glow_sdr": "SKY_GLOW_SDR"}
    assert {k: engine[v] for k, v in names.items()} == presets.SKY_BODIES_PARAMS
    assert engine["SKY_RESET"] == presets.SKY_BODIES_RESET
    jak3 = presets.resolve("sky-bodies", "jak3")
    for k in presets.SKY_BODIES_PARAMS:
        m = re.search(rf"(?:float|int) sky_{k} = ([0-9.]+)f?;", gfx)
        assert m and float(m.group(1)) == pytest.approx(float(jak3[k])), k
    forms = presets.goal_forms("sky-bodies", "jak3")
    assert forms[0] == "(pc-set-sun-option 29 1)" and forms[-1] == f"(pc-set-sun-option 10 {presets.SCALE})"
    assert presets.goal_forms("sky-bodies", "disc") == [
        "(pc-set-sun-option 29 1)", "(pc-set-sun-option 20 0)", f"(pc-set-sun-option 10 {presets.SCALE})"]


def test_reapply_resets_sky_bodies_options():
    """sandover-settings-reapply sends pc-set-sun-option 29, which restores every sky bodies
    sub-option (20-28) to its engine default; one sun row covers the sun and the moon."""
    gc = (presets.root() / "goal_src/jak1/pc/pckernel.gc").read_text(encoding="utf-8")
    start = gc.index("(defun sandover-settings-reapply")
    assert "(pc-set-sun-option 29 1)" in gc[start:gc.index("(sandover-settings-reapply)\n", start)]
    km = (presets.root() / "game/kernel/common/kmachine.cpp").read_text(encoding="utf-8")
    fn = km[km.index("void pc_set_sun_option("):]
    case = fn[fn.index("    case SunOption::SKY_RESET: {"):]
    case = case[:case.index("break;")]
    gfx, engine = _sun_disc_engine()
    fields = set(re.findall(r"\b(sky_\w+) = ", gfx))
    assert len(fields) == 9 and all(f"s.{f} = d.{f};" in case for f in fields), fields
    for name, n in engine.items():
        if name.startswith("SKY_") and name != "SKY_RESET":
            assert 20 <= n <= 28, name
            assert f"    case SunOption::{name}:" in fn, name
