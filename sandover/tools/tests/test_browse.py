from sandover.materials import CLASSES
from sandover.materials.browse import (
    EMISSIVE,
    NOT_EMISSIVE_OVERRIDE,
    RECEIVER,
    browse_classes,
    class_label,
    class_members,
    is_receiver,
    jump_to_texture,
    paginate,
    ships_emissive,
    tile_caption,
)


def _row(name, combo="", cls="stone", conf="0.5"):
    return {"combo_id": combo, "tpage": "t", "name": name, "class": cls, "confidence": conf,
            "judge_agreement": "agree"}


def _scores(threshold=0.5, **textures):
    return {"threshold": threshold, "textures": textures}


def test_browse_classes_appends_derived_classes_after_taxonomy():
    classes = browse_classes()
    assert classes[:len(CLASSES)] == CLASSES
    assert classes[-3:] == (EMISSIVE, RECEIVER, NOT_EMISSIVE_OVERRIDE)


def test_class_label_spells_out_the_two_derived_classes_only():
    assert class_label(EMISSIVE) == "emissive (ships)"
    assert class_label(RECEIVER) == "receiver (E scaled to 0 at export)"
    assert class_label("stone") == "stone"
    assert class_label(NOT_EMISSIVE_OVERRIDE) == NOT_EMISSIVE_OVERRIDE


def test_ships_emissive_falls_back_to_raw_table_when_unscored():
    row = _row("torch")
    assert ships_emissive(row, {}, {"torch": 0.11}, {}, threshold=0.1) is True
    assert ships_emissive(row, {}, {"torch": 0.1}, {}, threshold=0.1) is False


def test_ships_emissive_uses_emitter_score_over_the_table_when_scored():
    row = _row("wall")
    # raw E is high (bake lit it), but the emitter score says it is a receiver
    below = _scores(wall={"score": 0.2})
    assert ships_emissive(row, {}, {"wall": 0.9}, below) is False
    above = _scores(wall={"score": 0.8})
    assert ships_emissive(row, {}, {"wall": 0.9}, above) is True


def test_ships_emissive_anchor_or_human_emitter_wins_below_threshold():
    row = _row("lamp")
    anchor = _scores(lamp={"score": 0.1, "anchor": "lamp glass"})
    assert ships_emissive(row, {}, {}, anchor) is True
    human = _scores(lamp={"score": 0.1, "human": "emitter"})
    assert ships_emissive(row, {}, {}, human) is True


def test_ships_emissive_override_wins_over_score_both_ways():
    row = _row("torch", combo="0x1")
    scored_high = _scores(torch={"score": 0.9})
    assert ships_emissive(row, {"0x1": "not-emissive"}, {}, scored_high) is False
    scored_low = _scores(torch={"score": 0.0})
    assert ships_emissive(row, {"0x1": "emissive"}, {}, scored_low) is True


def test_is_receiver_raw_e_over_threshold_but_scored_below():
    row = _row("wall")
    scores = _scores(wall={"score": 0.2})
    assert is_receiver(row, {}, {"wall": 0.9}, scores, threshold=0.1) is True
    assert is_receiver(row, {}, {"wall": 0.05}, scores, threshold=0.1) is False


def test_is_receiver_false_once_the_texture_ships():
    row = _row("wall")
    scores = _scores(wall={"score": 0.8})
    assert is_receiver(row, {}, {"wall": 0.9}, scores, threshold=0.1) is False


def test_is_receiver_excludes_overridden_textures():
    row = _row("wall", combo="0x2")
    scores = _scores(wall={"score": 0.2})
    assert is_receiver(row, {"0x2": "not-emissive"}, {"wall": 0.9}, scores, threshold=0.1) is False
    assert is_receiver(row, {"0x2": "emissive"}, {"wall": 0.9}, scores, threshold=0.1) is False


def test_class_members_taxonomy_class():
    rows = [_row("a", cls="stone"), _row("b", cls="wood")]
    assert [r["name"] for r in class_members(rows, "stone", {}, {})] == ["a"]


def test_class_members_emissive_receiver_and_not_emissive_override():
    rows = [_row("a", combo="0x1"), _row("b", combo="0x2"), _row("c"), _row("d")]
    verdicts = {"0x1": "emissive", "0x2": "not-emissive"}
    e_table = {"c": 0.5, "d": 0.9}
    scores = _scores(d={"score": 0.1})  # over the raw threshold, scored a receiver
    emissive = class_members(rows, EMISSIVE, verdicts, e_table, scores)
    assert {r["name"] for r in emissive} == {"a", "c"}
    receiver = class_members(rows, RECEIVER, verdicts, e_table, scores)
    assert [r["name"] for r in receiver] == ["d"]
    not_emissive = class_members(rows, NOT_EMISSIVE_OVERRIDE, verdicts, e_table, scores)
    assert [r["name"] for r in not_emissive] == ["b"]


def test_paginate_clamps_and_counts_pages():
    rows = [_row(str(i)) for i in range(25)]
    page_rows, page, n_pages = paginate(rows, page=0, page_size=10)
    assert len(page_rows) == 10 and page == 0 and n_pages == 3
    page_rows, page, n_pages = paginate(rows, page=99, page_size=10)
    assert page == 2 and len(page_rows) == 5
    page_rows, page, n_pages = paginate([], page=0, page_size=10)
    assert page_rows == [] and page == 0 and n_pages == 1


def test_jump_to_texture_reuses_existing_slot():
    rows = [{"slot": "0", "combo_id": "0x1", "tpage": "t", "name": "a", "judged_class": "stone",
             "confidence": "0.9", "judge_agreement": "agree", "human_class": "", "tags": "",
             "human_note": "", "timestamp": "", "index": "0"}]
    new_rows, pos = jump_to_texture(rows, _row("a", combo="0x1"))
    assert new_rows is rows and pos == 0


def test_jump_to_texture_appends_when_not_queued():
    rows = []
    new_rows, pos = jump_to_texture(rows, _row("z", combo="0x9"))
    assert pos == 0
    assert new_rows[0]["combo_id"] == "0x9"
    assert new_rows[0]["name"] == "z"


def test_tile_caption_shows_name_and_confidence():
    assert tile_caption(_row("torch", conf="0.42")) == "torch  (0.42)"
