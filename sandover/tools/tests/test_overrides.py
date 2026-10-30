from pathlib import Path

import pytest

from sandover.materials.overrides import (
    FIELDS,
    append_override,
    latest_verdicts,
    override_key,
    read_overrides,
)


def test_append_override_creates_file_with_header(tmp_path: Path):
    path = tmp_path / "emissive-overrides.csv"
    row = append_override(path, {"combo_id": "0x1", "tpage": "t", "name": "n",
                                  "verdict": "emissive", "note": "torch"})
    assert row["verdict"] == "emissive"
    assert row["timestamp"]
    text = path.read_text(encoding="utf-8")
    assert text.splitlines()[0] == ",".join(FIELDS)
    assert len(read_overrides(path)) == 1


def test_append_override_appends_without_rewriting_header(tmp_path: Path):
    path = tmp_path / "o.csv"
    append_override(path, {"name": "a", "verdict": "emissive"})
    append_override(path, {"name": "b", "verdict": "not-emissive"})
    rows = read_overrides(path)
    assert [r["name"] for r in rows] == ["a", "b"]


def test_append_override_rejects_bad_verdict(tmp_path: Path):
    with pytest.raises(ValueError, match="bad verdict"):
        append_override(tmp_path / "o.csv", {"name": "a", "verdict": "sort-of"})


def test_append_override_empty_verdict_is_a_clear(tmp_path: Path):
    row = append_override(tmp_path / "o.csv", {"name": "a", "verdict": ""})
    assert row["verdict"] == ""


def test_read_overrides_missing_file_is_empty(tmp_path: Path):
    assert read_overrides(tmp_path / "nope.csv") == []


def test_override_key_prefers_combo_id():
    assert override_key({"combo_id": "0x1", "name": "n"}) == "0x1"
    assert override_key({"combo_id": "", "name": "n"}) == "n"
    assert override_key({"name": "n"}) == "n"


def test_latest_verdicts_last_row_wins_and_clear_removes():
    rows = [
        {"combo_id": "0x1", "name": "a", "verdict": "emissive"},
        {"combo_id": "0x1", "name": "a", "verdict": "not-emissive"},
        {"combo_id": "", "name": "b", "verdict": "emissive"},
        {"combo_id": "", "name": "b", "verdict": ""},
    ]
    verdicts = latest_verdicts(rows)
    assert verdicts == {"0x1": "not-emissive"}
    assert "b" not in verdicts


def test_latest_verdicts_skips_rows_with_no_key():
    assert latest_verdicts([{"combo_id": "", "name": "", "verdict": "emissive"}]) == {}
