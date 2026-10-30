import http.server
import json
import threading
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from sandover.materials.review import (
    CLASS_KEYS,
    KEY_ACTION,
    TAG_KEYS,
    agreement_by_class,
    agreement_markdown,
    alpha_thumbnail,
    answer,
    apply_action,
    clamp,
    compose_over_checker,
    fetch_models_using,
    first_unanswered,
    fit_for_review,
    load_queue,
    merge_needs_review,
    ordered_queue,
    progress,
    record,
    render_context_html,
    row_tags,
    save_note,
    sort_queue_by_confidence,
    toggle_tags,
    viewer_url,
    write_queue,
)


def _row(slot, combo, cls="stone", human="", conf="0.800", agree="agree", tags=""):
    return {"slot": str(slot), "index": str(slot), "combo_id": combo, "tpage": "t", "name": f"n{slot}",
            "judged_class": cls, "confidence": conf, "judge_agreement": agree,
            "human_class": human, "tags": tags, "human_note": "", "timestamp": ""}


def test_load_write_queue_roundtrip(tmp_path: Path):
    rows = [_row(0, "0x1"), _row(1, "0x2")]
    csv_path = tmp_path / "spotcheck.csv"
    write_queue(csv_path, rows)
    back = load_queue(csv_path)
    assert [r["combo_id"] for r in back] == ["0x1", "0x2"]
    assert back[0]["human_class"] == ""


def test_answer_writes_the_csv_at_once(tmp_path: Path):
    csv_path = tmp_path / "spotcheck.csv"
    write_queue(csv_path, [_row(0, "0x1"), _row(1, "0x2")])
    rows = ordered_queue(csv_path, "file")
    rows, pos = answer(csv_path, rows, 0, "a note", "wood")
    back = load_queue(csv_path)
    assert back[0]["human_class"] == "wood" and back[0]["human_note"] == "a note"
    assert back[1]["human_class"] == ""
    assert pos == 1 and progress(rows) == "1/2"


def test_answer_from_a_stale_page_keeps_answers_saved_since(tmp_path: Path):
    csv_path = tmp_path / "spotcheck.csv"
    write_queue(csv_path, [_row(i, f"0x{i}") for i in range(4)])
    stale = ordered_queue(csv_path, "file")  # a page opened before the answers below
    rows, pos = answer(csv_path, stale, 0, "", "wood")
    rows, pos = answer(csv_path, rows, pos, "", "metal")
    assert progress(ordered_queue(csv_path, "file")) == "2/4"  # what a reloaded page starts from
    rows, _ = answer(csv_path, stale, 2, "", "stone")
    assert [r["human_class"] for r in load_queue(csv_path)] == ["wood", "metal", "stone", ""]
    assert progress(rows) == "3/4"


def test_answer_skip_does_not_touch_the_csv(tmp_path: Path):
    csv_path = tmp_path / "spotcheck.csv"
    write_queue(csv_path, [_row(0, "0x1"), _row(1, "0x2")])
    before = csv_path.read_bytes()
    rows = ordered_queue(csv_path, "file")
    new_rows, pos = answer(csv_path, rows, 0, "", "skip")
    assert new_rows is rows and pos == 1 and csv_path.read_bytes() == before


def test_commit_renumbers_a_new_row_whose_slot_is_taken(tmp_path: Path):
    from sandover.materials.queue import commit

    csv_path = tmp_path / "spotcheck.csv"
    write_queue(csv_path, [_row(0, "0x1"), _row(1, "0xother")])
    rows = [_row(0, "0x1")]
    mine = {**_row(1, "0xmine"), "human_class": "wood"}
    out = commit(csv_path, [*rows, mine], [mine])
    back = {r["combo_id"]: r for r in load_queue(csv_path)}
    assert back["0xother"]["slot"] == "1" and back["0xmine"]["slot"] == "2"
    assert back["0xmine"]["human_class"] == "wood"
    assert [r["combo_id"] for r in out] == ["0x1", "0xmine", "0xother"]


def test_record_sets_human_class_note_and_timestamp():
    rows = [_row(0, "0x1")]
    new_rows = record(rows, 0, "wood", "correcting")
    assert new_rows[0]["human_class"] == "wood"
    assert new_rows[0]["human_note"] == "correcting"
    assert new_rows[0]["timestamp"]
    # pure: original untouched
    assert rows[0]["human_class"] == ""


def test_apply_action_class_sets_and_advances():
    rows = [_row(0, "0x1"), _row(1, "0x2")]
    new_rows, new_pos = apply_action(rows, 0, "note", "wood")
    assert new_rows[0]["human_class"] == "wood"
    assert new_pos == 1
    assert new_rows is not rows


def test_apply_action_accept_uses_judged_class():
    rows = [_row(0, "0x1", cls="metal")]
    new_rows, new_pos = apply_action(rows, 0, "", "accept")
    assert new_rows[0]["human_class"] == "metal"
    assert new_pos == 0  # clamped, only one row


def test_apply_action_skip_does_not_write():
    rows = [_row(0, "0x1"), _row(1, "0x2")]
    new_rows, new_pos = apply_action(rows, 0, "", "skip")
    assert new_rows is rows
    assert new_pos == 1
    assert new_rows[0]["human_class"] == ""


def test_apply_action_back_moves_without_writing():
    rows = [_row(0, "0x1"), _row(1, "0x2")]
    new_rows, new_pos = apply_action(rows, 1, "", "back")
    assert new_rows is rows
    assert new_pos == 0


def test_apply_action_back_at_start_stays():
    rows = [_row(0, "0x1")]
    _, new_pos = apply_action(rows, 0, "", "back")
    assert new_pos == 0


def test_apply_action_invalid_class_is_noop():
    rows = [_row(0, "0x1")]
    new_rows, new_pos = apply_action(rows, 0, "", "not-a-class")
    assert new_rows is rows
    assert new_pos == 0


def test_apply_action_empty_queue():
    assert apply_action([], 0, "", "wood") == ([], 0)


def test_clamp():
    assert clamp(-1, 5) == 0
    assert clamp(10, 5) == 4
    assert clamp(2, 0) == 0


def test_first_unanswered():
    rows = [_row(0, "0x1", human="stone"), _row(1, "0x2"), _row(2, "0x3")]
    assert first_unanswered(rows) == 1
    all_done = [_row(0, "0x1", human="stone")]
    assert first_unanswered(all_done) == 0


def test_progress_counts_answered():
    rows = [_row(0, "0x1", human="stone"), _row(1, "0x2")]
    assert progress(rows) == "1/2"


def test_merge_needs_review_skips_duplicates_and_numbers_slots():
    rows = [_row(0, "0x1"), _row(1, "0x2")]
    candidates = [
        {"combo_id": "0x2", "tpage": "t", "name": "dup", "judged_class": "wood",
         "confidence": "0.5", "judge_agreement": "disagree"},
        {"combo_id": "0x3", "tpage": "t", "name": "new", "judged_class": "metal",
         "confidence": "0.4", "judge_agreement": "disagree"},
    ]
    merged = merge_needs_review(rows, candidates)
    assert len(merged) == 3
    assert merged[-1]["combo_id"] == "0x3"
    assert merged[-1]["slot"] == "2"
    assert merged[-1]["human_class"] == ""


def test_agreement_by_class_and_markdown():
    rows = [
        _row(0, "0x1", cls="stone", human="stone"),
        _row(1, "0x2", cls="stone", human="wood"),
        _row(2, "0x3", cls="wood", human=""),  # unanswered, excluded
    ]
    counts = agreement_by_class(rows)
    assert counts["stone"] == (1, 2)
    assert counts["wood"] == (0, 0)
    md = agreement_markdown(rows)
    assert "| stone | 50% | 2 |" in md
    assert "| wood | - | 0 |" in md


def test_fit_for_review_upscales_small_and_leaves_big():
    small = Image.new("RGBA", (8, 8), (1, 2, 3, 255))
    big = fit_for_review(small, min_box=512)
    assert max(big.size) >= 512
    assert big.size == (512, 512)

    large = Image.new("RGBA", (600, 300), (1, 2, 3, 255))
    same = fit_for_review(large, min_box=512)
    assert same.size == (600, 300)


def test_compose_over_checker_is_rgb_same_size():
    im = Image.new("RGBA", (16, 32), (10, 20, 30, 255))
    out = compose_over_checker(im)
    assert out.mode == "RGB"
    assert out.size == (16, 32)


def test_alpha_thumbnail_matches_alpha_channel():
    im = Image.new("RGBA", (4, 4), (0, 0, 0, 200))
    thumb = alpha_thumbnail(im)
    assert thumb.mode == "L"
    assert np.asarray(thumb).flatten().tolist() == [200] * 16


# ---- tags -----------------------------------------------------------------------------

def test_row_tags_parses_semicolons_and_handles_empty():
    assert row_tags({"tags": "fur;precursor"}) == {"fur", "precursor"}
    assert row_tags({"tags": ""}) == set()
    assert row_tags({}) == set()


def test_toggle_tags_adds_and_removes_in_stable_order():
    rows = [_row(0, "0x1")]
    rows = toggle_tags(rows, 0, "precursor")
    assert rows[0]["tags"] == "precursor"
    rows = toggle_tags(rows, 0, "fur")
    assert rows[0]["tags"] == "fur;precursor"  # TAGS order, not insertion order
    rows = toggle_tags(rows, 0, "precursor")
    assert rows[0]["tags"] == "fur"


def test_toggle_tags_ignores_unknown_tag():
    rows = [_row(0, "0x1")]
    assert toggle_tags(rows, 0, "not-a-tag") is rows


def test_apply_action_toggle_tag_writes_and_does_not_advance():
    rows = [_row(0, "0x1"), _row(1, "0x2")]
    new_rows, new_pos = apply_action(rows, 0, "", "toggle:fur")
    assert new_rows[0]["tags"] == "fur"
    assert new_pos == 0
    assert new_rows is not rows


# ---- context-for-judges note ------------------------------------------------------------

def test_apply_action_note_saves_without_class_and_advances():
    rows = [_row(0, "0x1"), _row(1, "0x2")]
    new_rows, new_pos = apply_action(rows, 0, "look closer at the seam", "note")
    assert new_rows[0]["human_note"] == "look closer at the seam"
    assert new_rows[0]["human_class"] == ""
    assert new_pos == 1


def test_save_note_is_pure_and_sets_timestamp():
    rows = [_row(0, "0x1")]
    new_rows = save_note(rows, 0, "ctx")
    assert new_rows[0]["human_note"] == "ctx"
    assert new_rows[0]["timestamp"]
    assert rows[0]["human_note"] == ""


# ---- queue ordering ---------------------------------------------------------------------

def test_sort_queue_by_confidence_lowest_first_disagree_before_agree():
    rows = [
        _row(0, "0x1", conf="0.900", agree="agree"),
        _row(1, "0x2", conf="0.300", agree="agree"),
        _row(2, "0x3", conf="0.300", agree="disagree"),
        _row(3, "0x4", conf="0.100", agree="single"),
    ]
    ordered = [r["combo_id"] for r in sort_queue_by_confidence(rows)]
    assert ordered == ["0x4", "0x3", "0x2", "0x1"]


# ---- key map derived from materials.toml -----------------------------------------------

def test_class_keys_include_snow_and_lava():
    assert CLASS_KEYS["snow"] == "I"
    assert CLASS_KEYS["lava"] == "L"


def test_key_action_maps_every_class_key_case_insensitively():
    for cls, key in CLASS_KEYS.items():
        assert KEY_ACTION[key.upper()] == cls
        assert KEY_ACTION[key.lower()] == cls
    assert KEY_ACTION["I"] == "snow" and KEY_ACTION["i"] == "snow"
    assert KEY_ACTION["L"] == "lava" and KEY_ACTION["l"] == "lava"


def test_key_action_maps_every_tag_key_case_insensitively_as_toggle():
    for tag, key in TAG_KEYS.items():
        assert KEY_ACTION[key.upper()] == f"toggle:{tag}"
        assert KEY_ACTION[key.lower()] == f"toggle:{tag}"


def test_key_action_reserved_keys_still_win():
    assert KEY_ACTION["s"] == "skip" and KEY_ACTION["S"] == "skip"
    assert KEY_ACTION["b"] == "back" and KEY_ACTION["B"] == "back"
    assert KEY_ACTION["Enter"] == "accept"


def test_apply_action_accepts_snow_and_lava_classes():
    rows = [_row(0, "0x1")]
    new_rows, _ = apply_action(rows, 0, "", "snow")
    assert new_rows[0]["human_class"] == "snow"
    new_rows, _ = apply_action(rows, 0, "", "lava")
    assert new_rows[0]["human_class"] == "lava"


# ---- 3D context panel -------------------------------------------------------------------

class _StubViewerHandler(http.server.BaseHTTPRequestHandler):
    response_body = b"[]"

    def do_GET(self):
        body = self.response_body
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):  # quiet
        pass


@pytest.fixture
def stub_viewer():
    servers = []

    def factory(payload) -> int:
        handler = type("Handler", (_StubViewerHandler,), {"response_body": json.dumps(payload).encode()})
        srv = http.server.HTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        servers.append(srv)
        return srv.server_address[1]

    yield factory
    for srv in servers:
        srv.shutdown()


def _free_port() -> int:
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_viewer_url_quotes_model_and_stem():
    url = viewer_url(9999, "beach/mayor-lod0.glb", "mayor flesh")
    assert url == "http://127.0.0.1:9999/?model=beach%2Fmayor-lod0.glb&highlight=mayor%20flesh"


def test_fetch_models_using_parses_json(stub_viewer):
    port = stub_viewer([{"id": "beach/mayor-lod0.glb", "kind": "skinned", "triangles": 10}])
    assert fetch_models_using(port, "mayor-flesh") == [
        {"id": "beach/mayor-lod0.glb", "kind": "skinned", "triangles": 10}
    ]


def test_fetch_models_using_unreachable_returns_none():
    assert fetch_models_using(_free_port(), "stem", timeout=0.5) is None


def test_render_context_html_reports_empty_list():
    html = render_context_html(_free_port(), {"name": "stem"})
    assert "could not reach the viewer" in html


def test_render_context_html_no_model_in_rip(stub_viewer):
    port = stub_viewer([])
    html = render_context_html(port, {"name": "unused-stem"})
    assert "no model uses this texture in the rip" in html


def test_render_context_html_builds_iframe_and_other_model_links(stub_viewer):
    port = stub_viewer([
        {"id": "beach/mayor-lod0.glb", "kind": "skinned", "triangles": 1},
        {"id": "beach/barrel-lod0.glb", "kind": "prop", "triangles": 2},
    ])
    html = render_context_html(port, {"name": "mayor-flesh"})
    # html_escape turns the attribute's "&" into "&amp;"; the onclick JS string keeps a raw "&".
    assert (f'id="contextFrame" src="http://127.0.0.1:{port}/?model=beach%2Fmayor-lod0.glb'
            '&amp;highlight=mayor-flesh"') in html
    assert "beach/barrel-lod0.glb" in html
