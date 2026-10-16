import json

from sandover.census import load_texture_levels


def test_load_texture_levels_parses_tpage_name_keys(tmp_path):
    path = tmp_path / "texture-levels.json"
    path.write_text(json.dumps({
        "common/gen-metal-01": ["village1", "village2", "citadel"],
        "village1-vis-tfrag/vil1-wood-01": ["village1"],
        "effects/fx-spark": [],
    }), encoding="utf-8")

    levels = load_texture_levels(path)

    assert levels[("common", "gen-metal-01")] == ["village1", "village2", "citadel"]
    assert levels[("village1-vis-tfrag", "vil1-wood-01")] == ["village1"]
    assert levels[("effects", "fx-spark")] == []


def test_load_texture_levels_missing_file_returns_empty(tmp_path):
    assert load_texture_levels(tmp_path / "missing.json") == {}


def test_load_texture_levels_name_with_slash_splits_once(tmp_path):
    # Only the tpage/name boundary matters; a name containing "/" keeps the rest intact.
    path = tmp_path / "texture-levels.json"
    path.write_text(json.dumps({"tpage/weird/name": ["level"]}), encoding="utf-8")

    levels = load_texture_levels(path)

    assert levels[("tpage", "weird/name")] == ["level"]
