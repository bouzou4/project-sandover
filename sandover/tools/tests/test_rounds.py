import json
from pathlib import Path

import pytest

from sandover.rounds import RoundManifest, read_result, write_result


def _make_round(tmp_path: Path) -> Path:
    round_dir = tmp_path / "round-001"
    round_dir.mkdir()
    manifest = {
        "schema": 1,
        "phase": "test-phase",
        "round": 3,
        "pick_set": "pick-v1",
        "shots": ["shot-a", "shot-b"],
        "candidates": {
            "A": {"preset": "faithful", "role": "anchor", "label": "Faithful"},
            "B": {"preset": "contact", "role": "candidate", "label": "Contact"},
        },
        "images": {
            "A": {"shot-a": "candidates/A/shot-a.png", "shot-b": "candidates/A/shot-b.png"},
            "B": {"shot-a": "candidates/B/shot-a.png", "shot-b": "candidates/B/shot-b.png"},
        },
        "vanilla": {"shot-a": "vanilla/shot-a.png", "shot-b": "vanilla/shot-b.png"},
    }
    (round_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return round_dir


def test_load_manifest_and_paths(tmp_path: Path):
    round_dir = _make_round(tmp_path)
    m = RoundManifest.load(round_dir)
    assert m.phase == "test-phase"
    assert m.anchor_letter == "A"
    assert m.image_path("B", "shot-a") == round_dir / "candidates/B/shot-a.png"
    assert m.vanilla_path("shot-b") == round_dir / "vanilla/shot-b.png"


def test_load_rejects_unknown_schema(tmp_path: Path):
    round_dir = tmp_path / "bad"
    round_dir.mkdir()
    (round_dir / "manifest.json").write_text(json.dumps({"schema": 99}), encoding="utf-8")
    with pytest.raises(ValueError):
        RoundManifest.load(round_dir)


def test_write_result_valid(tmp_path: Path):
    round_dir = _make_round(tmp_path)
    m = RoundManifest.load(round_dir)
    out = write_result(m, "B", [], {"shot-a": "banding"}, generator="human-pick")
    assert out == round_dir / "result.json"
    data = json.loads(out.read_text(encoding="utf-8"))
    assert data["picked"] == "B"
    assert data["broken"] == {"shot-a": "banding"}
    assert data["round"] == 3

    assert read_result(round_dir) == data
    assert read_result(tmp_path / "no-such-round") is None


def test_write_result_rejects_unknown_pick(tmp_path: Path):
    m = RoundManifest.load(_make_round(tmp_path))
    with pytest.raises(ValueError):
        write_result(m, "Z", [], {})


def test_write_result_rejects_more_like_this_equal_to_pick(tmp_path: Path):
    m = RoundManifest.load(_make_round(tmp_path))
    with pytest.raises(ValueError):
        write_result(m, "B", ["B"], {})


def test_write_result_rejects_unknown_broken_shot(tmp_path: Path):
    m = RoundManifest.load(_make_round(tmp_path))
    with pytest.raises(ValueError):
        write_result(m, "B", [], {"no-such-shot": "note"})


def test_write_result_sorts_more_like_this(tmp_path: Path):
    round_dir = _make_round(tmp_path)
    manifest = json.loads((round_dir / "manifest.json").read_text(encoding="utf-8"))
    manifest["candidates"]["C"] = {"preset": "soft", "role": "candidate", "label": "Soft"}
    manifest["images"]["C"] = manifest["images"]["B"]
    (round_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    m = RoundManifest.load(round_dir)

    out = write_result(m, "A", ["C", "B"], {})
    data = json.loads(out.read_text(encoding="utf-8"))
    assert data["more_like_this"] == ["B", "C"]


@pytest.mark.parametrize("mutate", [
    lambda m: m.pop("shots"),
    lambda m: m["candidates"].update(F={"preset": "x", "role": "candidate", "label": "X"}),
    lambda m: m["candidates"]["B"].update(role="anchor"),
    lambda m: m["candidates"]["A"].update(role="candidate"),
    lambda m: m["candidates"]["B"].pop("label"),
    lambda m: m["images"].pop("B"),
    lambda m: m["images"]["A"].pop("shot-b"),
    lambda m: m["images"]["A"].update({"shot-a": "../outside.png"}),
    lambda m: m["vanilla"].update({"no-such-shot": "vanilla/x.png"}),
    lambda m: m.update(shots=["shot-a", "shot-a"]),
])
def test_load_rejects_malformed_manifest(tmp_path: Path, mutate):
    round_dir = _make_round(tmp_path)
    path = round_dir / "manifest.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    mutate(data)
    path.write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(ValueError):
        RoundManifest.load(round_dir)
