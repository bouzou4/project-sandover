import json
from pathlib import Path

import pytest
from PIL import Image

from sandover import census
from sandover.materials import CLASS_KEYS, CLASSES, TAG_KEYS, TAGS, judge, load_taxonomy, sheets, table
from sandover.materials.sheets import MANIFEST_FIELDS, read_csv, write_csv
from sandover.materials.table import AMBIGUOUS_BELOW, _levels_map, combine


def _j(cls, conf, note=""):
    return {"class": cls, "confidence": conf, "note": note}


# ---- taxonomy (materials.toml) ----------------------------------------------------------

def test_taxonomy_loads_the_committed_preset():
    classes, tags = load_taxonomy()
    assert [c.name for c in classes] == list(CLASSES)
    assert [t.name for t in tags] == list(TAGS)
    assert "snow" in CLASSES and "lava" in CLASSES and "ice" in CLASSES
    assert CLASS_KEYS["snow"] == "I" and CLASS_KEYS["lava"] == "L"
    assert TAG_KEYS == {"fur": "7", "precursor": "8", "baked-tint": "T", "shaft": "H"}


def test_taxonomy_rejects_duplicate_key_across_classes_and_tags(tmp_path: Path):
    bad = tmp_path / "materials.toml"
    bad.write_text(
        '[[classes]]\nname = "stone"\nkey = "1"\ndescription = "rock"\n\n'
        '[[tags]]\nname = "fur"\nkey = "1"\ndescription = "fuzzy"\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="duplicate key"):
        load_taxonomy(bad)


def test_taxonomy_rejects_duplicate_name(tmp_path: Path):
    bad = tmp_path / "materials.toml"
    bad.write_text(
        '[[classes]]\nname = "stone"\nkey = "1"\ndescription = "rock"\n\n'
        '[[classes]]\nname = "stone"\nkey = "2"\ndescription = "rock again"\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="duplicate class name"):
        load_taxonomy(bad)


def test_taxonomy_rejects_non_single_character_key(tmp_path: Path):
    bad = tmp_path / "materials.toml"
    bad.write_text('[[classes]]\nname = "stone"\nkey = "10"\ndescription = "rock"\n',
                    encoding="utf-8")
    with pytest.raises(ValueError, match="invalid key"):
        load_taxonomy(bad)


def test_judge_prompt_lists_every_class_including_snow_and_lava():
    text = judge.prompt("a", 0, 20)
    assert "- snow: snow (packed, powder" in text
    assert "- ice: clear or blue ice" in text
    assert "- lava: molten rock, emissive" in text
    assert f"`class` must be one of: {', '.join(CLASSES)}." in text


def test_combine_agree_averages():
    cls, conf, agree, _ = combine(_j("stone", 0.9), _j("stone", 0.7))
    assert (cls, agree) == ("stone", "agree")
    assert abs(conf - 0.8) < 1e-9


def test_combine_disagree_is_ambiguous():
    cls, conf, agree, note = combine(_j("metal", 0.9), _j("stone", 0.6))
    assert (cls, agree) == ("metal", "disagree")
    assert conf < AMBIGUOUS_BELOW
    assert "a: metal" in note and "b: stone" in note
    assert combine(_j("metal", 1.0), _j("stone", 0.6))[1] < AMBIGUOUS_BELOW


def test_combine_single_and_none():
    assert combine(_j("wood", 0.95), None)[1] < AMBIGUOUS_BELOW
    assert combine(None, None)[2] == "none"


def test_every_sheet_judged_by_two_different_batches():
    n = 201
    a = {s: b for b, ss in enumerate(judge.batches("a", n)) for s in ss}
    b = {s: k for k, ss in enumerate(judge.batches("b", n)) for s in ss}
    assert sorted(a) == sorted(b) == list(range(1, n + 1))
    # The two batches holding a sheet never contain exactly the same sheets.
    groups_a = [set(g) for g in judge.batches("a", n)]
    groups_b = [set(g) for g in judge.batches("b", n)]
    assert all(groups_a[a[s]] != groups_b[b[s]] for s in range(1, n + 1))


def test_levels_map_joins_with_semicolon(tmp_path, monkeypatch):
    path = tmp_path / "texture-levels.json"
    path.write_text(json.dumps({"common/gen-metal-01": ["village1", "citadel"],
                                 "effects/fx-spark": []}), encoding="utf-8")
    monkeypatch.setattr(census, "texture_levels_path", lambda: path)

    levels = _levels_map()

    assert levels[("common", "gen-metal-01")] == "village1;citadel"
    assert levels[("effects", "fx-spark")] == ""


def test_envmap_swatch_heuristic():
    assert sheets.is_envmap_swatch(Image.new("RGB", (8, 8), (200, 200, 210)))
    noisy = Image.effect_noise((16, 16), 80).convert("RGB")
    assert not sheets.is_envmap_swatch(noisy)
    assert not sheets.is_envmap_swatch(Image.new("RGB", (32, 32), (200, 200, 210)))


# ---- rejudge ----------------------------------------------------------------------------

def _materials_row(combo, tpage, name, cls, conf, agree, reviewed=False):
    return {"combo_id": combo, "tpage": tpage, "name": name, "class": cls, "confidence": f"{conf:.3f}",
            "judge_agreement": agree, "source": "judge", "envmap_swatch": "False", "has_alpha": "False",
            "levels": "", "reviewed": reviewed, "tags": "", "note": ""}


def _manifest_row(idx, combo, tpage, name):
    return {"index": idx, "combo_id": combo, "tpage": tpage, "name": name, "width": 16, "height": 16,
            "has_alpha": "False", "envmap_swatch": "False", "png": f"{tpage}/{name}.png"}


@pytest.fixture
def materials_env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path / "data"))
    write_csv(table.csv_path(), [
        _materials_row("0x1", "t", "low-conf", "stone", 0.2, "disagree"),
        _materials_row("0x2", "t", "mid-conf-agree", "wood", 0.5, "agree"),
        _materials_row("0x3", "t", "high-conf", "metal", 0.95, "agree"),
        _materials_row("0x4", "t", "already-reviewed", "organic", 0.1, "disagree", reviewed=True),
    ], table.FIELDS)
    write_csv(table.manifest_path(), [
        _manifest_row(0, "0x1", "t", "low-conf"),
        _manifest_row(1, "0x2", "t", "mid-conf-agree"),
        _manifest_row(2, "0x3", "t", "high-conf"),
        _manifest_row(3, "0x4", "t", "already-reviewed"),
    ], MANIFEST_FIELDS)
    return tmp_path


def test_rejudge_candidates_excludes_reviewed_and_confident_agree(materials_env):
    names = [c["name"] for c in table.rejudge_candidates()]
    assert names == ["low-conf", "mid-conf-agree"]


def test_rejudge_candidates_respects_limit(materials_env):
    cands = table.rejudge_candidates(limit=1)
    assert len(cands) == 1 and cands[0]["name"] == "low-conf"


def test_guidance_lines_and_block_from_human_notes():
    rows = [
        {"tpage": "t", "name": "a", "human_note": "looks like precursor bronze"},
        {"tpage": "t", "name": "b", "human_note": ""},
        {"tpage": "u", "name": "c", "human_note": "  has fur shell  "},
    ]
    assert table.guidance_lines(rows) == [
        "t/a: looks like precursor bronze", "u/c: has fur shell",
    ]
    block = table.guidance_block(rows)
    assert block.startswith("Human guidance from the spot-check:\n")
    assert "- t/a: looks like precursor bronze" in block
    assert table.guidance_block([{"tpage": "t", "name": "a", "human_note": ""}]) == ""


def test_rejudge_prepares_then_reports_awaiting_judgement(materials_env, monkeypatch):
    monkeypatch.setattr(table, "render_all", lambda *a, **k: [])
    msg = table.rejudge(with_notes=False)
    assert "prepared 2 row(s)" in msg
    pending = table.latest_rejudge_dir()
    assert pending is not None
    assert (pending / "manifest.csv").is_file()
    prompt_text = (pending / "prompt.txt").read_text(encoding="utf-8")
    assert "Human guidance" not in prompt_text

    assert "awaiting judgement" in table.rejudge()


def test_rejudge_with_notes_injects_guidance_from_spotcheck(materials_env, monkeypatch):
    monkeypatch.setattr(table, "render_all", lambda *a, **k: [])
    write_csv(table.materials_dir() / "spotcheck" / "spotcheck.csv", [
        {"slot": "0", "index": "0", "combo_id": "0x9", "tpage": "t", "name": "n9",
         "judged_class": "stone", "confidence": "0.5", "judge_agreement": "agree",
         "human_class": "stone", "tags": "", "human_note": "shiny like precursor metal"},
    ], ["slot", "index", "combo_id", "tpage", "name", "judged_class", "confidence",
        "judge_agreement", "human_class", "tags", "human_note"])
    table.rejudge(with_notes=True)
    prompt_text = (table.latest_rejudge_dir() / "prompt.txt").read_text(encoding="utf-8")
    assert "Human guidance from the spot-check:" in prompt_text
    assert "t/n9: shiny like precursor metal" in prompt_text


def test_rejudge_merges_judgement_and_reports_changed_count(materials_env, monkeypatch):
    monkeypatch.setattr(table, "render_all", lambda *a, **k: [])
    table.rejudge()
    pending = table.latest_rejudge_dir()
    by_name = {r["name"]: r for r in read_csv(pending / "manifest.csv")}
    judged = [
        {"index": int(by_name["low-conf"]["index"]), "class": "wood", "confidence": 0.7, "note": "bark pattern"},
        {"index": int(by_name["mid-conf-agree"]["index"]), "class": "wood", "confidence": 0.9, "note": "confirmed"},
    ]
    (pending / "judgement.json").write_text(json.dumps(judged), encoding="utf-8")

    msg = table.rejudge()
    assert "1 class(es) changed" in msg
    assert (pending / "merged.json").is_file()
    updated = {r["name"]: r for r in read_csv(table.csv_path())}
    assert updated["low-conf"]["class"] == "wood"
    assert updated["low-conf"]["confidence"] == "0.700"
    assert updated["mid-conf-agree"]["note"] == "rejudge: confirmed"


def test_rejudge_no_candidates_reports_nothing_to_do(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path / "data"))
    write_csv(table.csv_path(), [_materials_row("0x1", "t", "a", "stone", 0.95, "agree")], table.FIELDS)
    write_csv(table.manifest_path(), [_manifest_row(0, "0x1", "t", "a")], MANIFEST_FIELDS)
    assert table.rejudge() == "no needs-review rows to rejudge"


def test_rejudge_prompt_includes_guidance_and_local_index_range(tmp_path: Path):
    text = judge.rejudge_prompt(tmp_path / "batch", 3, guidance="Human guidance from the spot-check:\n- t/a: note")
    assert "Human guidance from the spot-check:\n- t/a: note" in text
    assert "indices 0-2" in text
    assert (tmp_path / "batch" / "judgement.json").as_posix() in text


def test_rejudge_prompt_no_items_no_guidance():
    text = judge.rejudge_prompt(Path("out"), 0, guidance="")
    assert "indices (none)" in text
    assert "Human guidance" not in text


# ---- tag rules ([tag_rules] in materials.toml) ---------------------------------------------

def test_tag_rules_tag_every_tpage_copy_and_clear_the_rest():
    from sandover.materials import tag_rules

    rows = [{"combo_id": "0x1", "tpage": "beach-vis-pris", "name": "muse-white", "tags": ""},
            {"combo_id": "0x2", "tpage": "misty-vis-pris", "name": "muse-white", "tags": "precursor"},
            {"combo_id": "0x3", "tpage": "misty-vis-pris", "name": "muse-eye", "tags": "fur"}]
    changes = tag_rules.apply(rows, {"fur": ("muse-white",)})
    assert [r["tags"] for r in rows] == ["fur", "precursor;fur", ""]
    added, removed = changes["fur"]
    assert [r["combo_id"] for r in added] == ["0x1", "0x2"] and [r["combo_id"] for r in removed] == ["0x3"]


def test_tag_rules_committed_preset_loads_and_matches_known_fur():
    from sandover.materials import tag_rules

    globs = tag_rules.load_rules()["fur"]
    for name in ("sk-bodyfur", "bab-longfur", "puppy-fur-01", "muse-white"):
        assert tag_rules.matches(name, globs), name
    for name in ("muse-eye", "sk-eye-lid", "sk-ear", "bab-eye"):
        assert not tag_rules.matches(name, globs), name


def test_tag_rules_reject_unknown_tag(tmp_path: Path):
    from sandover.materials import tag_rules

    bad = tmp_path / "materials.toml"
    bad.write_text('[tag_rules.fluffy]\nnames = ["x"]\n', encoding="utf-8")
    with pytest.raises(ValueError, match="no tag"):
        tag_rules.load_rules(bad)


# names that carried `fur` in the hand-tagged table before the rule (2026-10-09); `ogre-nose`
# was dropped on purpose (noses are outside the criterion)
PRE_RULE_FUR = (
    "bab-allfur", "bab-fur", "bab-furskin-trans", "bab-furtrans", "bab-longfur", "muse-ear2",
    "puppy-fur-01", "puppy-fur-02", "puppy-fur-03", "puppy-fur-04", "puppy-fur-05",
    "puppy-furlong-01", "puppy-furlong-02", "puppy-ltfur2skin", "puppy-medfur2skin",
    "evilbro-beard", "ogre-bluefur", "ogre-furlong", "ogre-furtrans", "ogre-furtrans2",
    "ogre-hair", "drill-furlight", "sk-armfur", "sk-bodyfur", "sk-finger",
    "sk-orange2yellowfur", "sk-solidorangefur", "sk-yellowfurnew", "flut-creamfeathers",
)


def test_tag_rules_keep_every_pre_rule_fur_name_but_the_nose():
    from sandover.materials import tag_rules

    globs = tag_rules.load_rules()["fur"]
    assert len(PRE_RULE_FUR) == 29
    assert [n for n in PRE_RULE_FUR if not tag_rules.matches(n, globs)] == []
    assert not tag_rules.matches("ogre-nose", globs)


def test_tag_rules_unmatched_globs():
    from sandover.materials import tag_rules

    rows = [{"name": "muse-white"}, {"name": "sk-bodyfur"}]
    assert tag_rules.unmatched_globs(rows, {"fur": ("muse-*", "sk-*fur*", "yeti-wool")}) == [
        ("fur", "yeti-wool")]


def test_merge_reapplies_the_fur_rule_after_keeping_reviewed_rows(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path / "data"))
    monkeypatch.setattr(table, "load_pass", lambda pass_id: {})
    monkeypatch.setattr(table, "_levels_map", lambda: {})
    reviewed = _materials_row("0x2", "t", "sk-ear", "organic", 1.0, "agree", reviewed=True)
    reviewed["tags"] = "fur"
    write_csv(table.csv_path(), [_materials_row("0x1", "t", "muse-white", "organic", 0.9, "agree"),
                                 reviewed], table.FIELDS)
    write_csv(table.manifest_path(), [_manifest_row(0, "0x1", "t", "muse-white"),
                                      _manifest_row(1, "0x2", "t", "sk-ear")], MANIFEST_FIELDS)
    table.merge()
    tags = {r["name"]: r["tags"] for r in read_csv(table.csv_path())}
    assert tags == {"muse-white": "fur", "sk-ear": ""}
