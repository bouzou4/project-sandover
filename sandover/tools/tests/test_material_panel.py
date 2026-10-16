import json
from pathlib import Path

from sandover.materials import emissive_strengths as es
from sandover.materials.emissive_review import TAB_KEYS, build_queue, ladder_action
from sandover.materials.emissive_maps import override_names
from sandover.materials.material_panel import (
    FLAG_KEYS,
    LADDER_KEYS,
    captures_for,
    emissive_reason,
    find_captures,
    flag,
    key_bindings,
    ladder_step,
    legend_html,
    save_edit,
    target_edit_state,
    undo_last,
    verdict_for,
    why_markdown,
)
from sandover.materials.overrides import FIELDS, read_overrides

INDEX = {"textures": {
    "lamp": {"tpage": "t", "source": "bake", "coverage": 0.2, "default_strength": 0.8, "role": "light"},
    "sand": {"tpage": "t", "source": "bake", "coverage": 1.0, "default_strength": 0.4, "role": "night"},
}}
MATS = [{"combo_id": "0x1", "tpage": "t", "name": "lamp", "class": "glass"},
        {"combo_id": "0x2", "tpage": "t", "name": "sand", "class": "stone", "tags": ""},
        {"combo_id": "0x3", "tpage": "t", "name": "sign", "class": "metal"}]


def queue_names(path: Path) -> list[str]:
    return [r["name"] for r in build_queue(INDEX, override_names(read_overrides(path)), MATS)]


def test_n_drops_from_ladder_and_undo_restores(tmp_path: Path):
    path = tmp_path / "emissive-overrides.csv"
    assert queue_names(path) == ["lamp", "sand"]
    row, undo = flag(path, MATS[1], "not-emissive", "lit by the beach lamps", [])
    assert row["combo_id"] == "0x2" and row["note"] == "lit by the beach lamps"
    assert queue_names(path) == ["lamp"]
    assert verdict_for(MATS[1], read_overrides(path)) == "not-emissive"
    back, undo = undo_last(path, undo)
    assert back["verdict"] == "" and undo == []
    assert queue_names(path) == ["lamp", "sand"]
    assert verdict_for(MATS[1], read_overrides(path)) == ""
    assert undo_last(path, undo) == (None, [])


def test_e_force_adds_a_texture_with_no_map(tmp_path: Path):
    path = tmp_path / "o.csv"
    flag(path, MATS[2], "emissive", "", [])
    q = build_queue(INDEX, override_names(read_overrides(path)), MATS)
    sign = next(r for r in q if r["name"] == "sign")
    assert sign["source"] == "override" and sign["combo_id"] == "0x3"


def test_undo_restores_the_previous_verdict_not_a_clear(tmp_path: Path):
    path = tmp_path / "o.csv"
    _, undo = flag(path, MATS[1], "emissive", "", [])
    _, undo = flag(path, MATS[1], "not-emissive", "", undo)
    undo_last(path, undo)
    assert verdict_for(MATS[1], read_overrides(path)) == "emissive"


def test_flags_share_the_viewer_mark_format(tmp_path: Path):
    """Reviewer flags are ordinary emissive-overrides.csv rows: same header as /api/mark writes."""
    path = tmp_path / "o.csv"
    flag(path, MATS[0], "not-emissive", "n", [])
    assert path.read_text(encoding="utf-8").splitlines()[0] == ",".join(FIELDS)


def test_legend_lists_every_bound_key():
    for entries in (TAB_KEYS, FLAG_KEYS):
        legend = legend_html(entries)
        for key, _, desc in entries:
            assert f"<kbd>{key}</kbd>" in legend
            assert desc.split(":")[0] in legend
    bound = key_bindings(TAB_KEYS)
    assert {bound[k] for k in ("n", "N", "e", "E", "u", "U")} == {"not-emissive", "emissive", "undo"}
    assert bound["ArrowUp"] == "up" and bound["Enter"] == "accept"
    assert set(bound.values()) == {a for _, a, _ in LADDER_KEYS + FLAG_KEYS}


def test_reason_names_the_source_and_the_override():
    r = emissive_reason("sand", "", INDEX, {"sand": 0.45})
    assert r["emissive"] and "night light" in r["because"]
    r = emissive_reason("sand", "not-emissive", INDEX, {"sand": 0.45})
    assert not r["emissive"] and "human" in r["because"] and "night light" in r["source"]
    r = emissive_reason("rock", "", INDEX, {})
    assert not r["emissive"]
    md = why_markdown(r, None, None, mask_exists=False)
    assert "No emissive mask" in md and "Emitter score: none" in md


def test_why_shows_ladder_step_and_score():
    scores = {"threshold": 0.5, "textures": {"lamp": {"score": 0.9, "components": {"correlation": 0.8}}}}
    r = emissive_reason("lamp", "", INDEX, {"lamp": 0.5}, scores)
    md = why_markdown(r, {"strength": "0.5", "source": "human"}, (1, 2), True)
    assert "step -4" in md and "placed by human" in md and "0.90" in md and "correlation 0.80" in md
    assert "emitter" in md
    assert ladder_step(1.0) == 0 and ladder_step(2.0) == 4


def test_scored_receiver_accepted_at_the_bottom_writes_an_explicit_row():
    scores = {"threshold": 0.5, "textures": {
        "lamp": {"score": 0.9}, "sand": {"score": 0.07, "ladder_default": es.LADDER_MIN}}}
    q = es.scored_queue(build_queue(INDEX, {}, MATS), scores)
    assert [r["name"] for r in q] == ["lamp", "sand"]
    sand = q[1]
    assert sand["emitter"] is False and sand["default"] == es.LADDER_MIN
    assert ladder_action(sand, es.LADDER_MIN, "accept") == (es.LADDER_MIN, "human", 1)
    assert ladder_action(q[0], q[0]["default"], "accept")[1] == "default"


def test_captures_by_level_night_first(tmp_path: Path):
    d = tmp_path / "goldens" / "auto-v1" / "abc"
    d.mkdir(parents=True)
    for hour, tag in ((0.0, "night"), (12.0, "noon"), (18.0, "dusk")):
        (d / f"beach-start-{tag}.png").write_bytes(b"x")
        (d / f"beach-start-{tag}.json").write_text(json.dumps({"continue": "beach-start", "hour": hour}))
    caps = find_captures(tmp_path)
    got = captures_for(["training", "beach"], caps)
    assert [c["png"].name for c in got] == ["beach-start-night.png", "beach-start-noon.png"]
    assert captures_for(["sunkenb"], caps) == []


def test_save_edit_queues_and_round_trips():
    rows, pos = save_edit([], MATS[1], "stone", ["fur"], "sand under a lamp")
    assert rows[pos]["human_class"] == "stone" and rows[pos]["human_note"] == "sand under a lamp"
    cls, tags, note, judged = target_edit_state(MATS[1], rows)
    assert (cls, tags, note, judged) == ("stone", ["fur"], "sand under a lamp", "stone")


def test_do_save_writes_through_and_keeps_answers_saved_since(tmp_path):
    from types import SimpleNamespace

    from sandover.materials.material_panel import MaterialPanel
    from sandover.materials.queue import load_queue, write_queue

    csv_path = tmp_path / "spotcheck.csv"
    q = {"slot": "0", "combo_id": MATS[0]["combo_id"], "tpage": MATS[0]["tpage"], "name": MATS[0]["name"],
         "judged_class": "stone", "human_class": ""}
    write_queue(csv_path, [q])
    stale = load_queue(csv_path)
    write_queue(csv_path, [{**q, "human_class": "wood"}])  # answered in another tab since
    fake = SimpleNamespace(queue_csv=csv_path, data=SimpleNamespace(material_row=lambda t: t))
    rows, _ = MaterialPanel.do_save(fake, MATS[1], stale, "metal", [], "")
    back = {r["combo_id"]: r["human_class"] for r in load_queue(csv_path)}
    assert back == {MATS[0]["combo_id"]: "wood", MATS[1]["combo_id"]: "metal"}
    assert {r["combo_id"]: r["human_class"] for r in rows} == back
