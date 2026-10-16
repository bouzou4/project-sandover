import re
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PRESET = REPO / "sandover" / "presets" / "pbr.toml"
MATERIALS = REPO / "game" / "graphics" / "opengl_renderer" / "Materials.cpp"
GFX = REPO / "game" / "graphics" / "gfx.h"


def _engine_keys() -> tuple[set[str], set[str]]:
    src = MATERIALS.read_text(encoding="utf-8")
    body = src[src.index("bool apply_preset_value("):src.index("void pbr_load_preset()")]
    glob = set(re.findall(r'\{"([a-z_]+)", &gs\.', body)) | set(re.findall(r'key == "([a-z_]+)"', body))
    cls = set(re.findall(r'\{"([a-z_]+)", &PbrClassParams::', body)) | {"tint"}
    return glob, cls


def _sections(data: dict, prefix: str = ""):
    for name, val in data.items():
        if isinstance(val, dict):
            yield from _sections(val, f"{prefix}{name}.")
        elif prefix:
            yield prefix[:-1], name


def test_every_preset_key_is_understood_by_the_engine():
    glob, cls = _engine_keys()
    data = tomllib.loads(PRESET.read_text(encoding="utf-8"))
    for section, key in _sections(data):
        parts = section.split(".")
        if parts[0] == "look":
            parts = parts[2:] or ["global"]
        if parts[0] == "class":
            assert key in cls, f"[{section}] {key}"
        elif parts[0] == "global":
            assert key in glob, f"[{section}] {key}"


def test_class_response_rules_on_in_the_preset_off_at_boot():
    data = tomllib.loads(PRESET.read_text(encoding="utf-8"))
    g = data["global"]
    assert g["transmit"] == 1.0 and g["wrap"] == 1.0 and g["wind_tie"] == 1
    assert data["class"]["foliage"]["transmit"] > 0 and data["class"]["snow"]["wrap"] > 0
    gfx = GFX.read_text(encoding="utf-8")
    assert "float pbr_transmit = 0.f;" in gfx and "float pbr_wrap = 0.f;" in gfx
    assert "bool pbr_wind_tie = false;" in gfx and "float pbr_bake_wrap = 0.f;" in gfx
    assert g["bake_wrap"] == 0.0 and data["class"]["snow"]["bake_wrap"] > 0


def test_snow_glints_on_in_the_preset_off_at_boot():
    data = tomllib.loads(PRESET.read_text(encoding="utf-8"))
    assert data["global"]["glint"] == 1.0 and data["class"]["snow"]["glint"] > 0
    for name, cls in data["class"].items():
        if name != "snow":
            assert cls.get("glint", 0) == 0, name
    gfx = GFX.read_text(encoding="utf-8")
    assert "float pbr_glint = 0.f;" in gfx and "GLINT = 32," in gfx
    src = MATERIALS.read_text(encoding="utf-8")
    for key in ["glint", "glint_cell_m", "glint_density", "glint_spread", "glint_width"]:
        assert f'{{"{key}", &gs.' in src, key
    assert '{"glint", &PbrClassParams::glint}' in src


def test_backlight_sun_occlusion_on_in_the_preset_off_at_boot():
    g = tomllib.loads(PRESET.read_text(encoding="utf-8"))["global"]
    assert g["sun_occlusion"] == 1 and g["leaf_density"] == 2.5 and g["transmit"] == 1.0
    gfx = GFX.read_text(encoding="utf-8")
    assert "bool pbr_transmit_occl = false;" in gfx and "SUN_OCCLUSION = 30," in gfx
    src = MATERIALS.read_text(encoding="utf-8")
    assert 'key == "sun_occlusion"' in src and 'key == "leaf_density"' in src


def test_ice_picked_moderate_in_the_preset_off_at_boot():
    data = tomllib.loads(PRESET.read_text(encoding="utf-8"))
    assert data["global"]["ice"] == 1.0
    assert set(data["ice"]) == {"depth_m", "absorb_m", "roughness", "transmit"}
    assert "ice" in data["class"] and data["class"]["ice"].get("glint", 0) == 0
    gfx = GFX.read_text(encoding="utf-8")
    assert "float pbr_ice = 0.f;" in gfx and "ICE = 40," in gfx
    src = MATERIALS.read_text(encoding="utf-8")
    assert '{"ice", &gs.pbr_ice}' in src and "PBR_ICE" in src
    for key in ["depth_m", "absorb_m", "roughness", "transmit"]:
        assert f'{{"{key}", &gs.pbr_ice_' in src, key


def test_per_class_shader_arrays_cover_every_class():
    count = int(re.search(r"PBR_CLASS_COUNT = (\d+)", (MATERIALS.parent / "Materials.h").read_text()).group(1))
    shaders = MATERIALS.parent / "shaders"
    for name in ["pbr_common_frag.glsl", "pbr_bg_frag.glsl"]:
        src = (shaders / name).read_text(encoding="utf-8")
        for arr, n in re.findall(r"\b(?:uniform|const)\s+\w+\s+(\w+)\[(\d+)\]", src):
            if re.search(rf"\b{arr}\[c\]", src):
                assert int(n) >= count, f"{name}: {arr}[{n}] is indexed by class ({count} classes)"
