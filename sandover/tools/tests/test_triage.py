import csv
import json
import threading
import urllib.error
import urllib.request
from pathlib import Path

import pytest
from PIL import Image

try:
    from sandover import hub
except ImportError:
    hub = None
from sandover.triage import server
from sandover.triage.store import FACETS, Run, TriageError, caption, crop_box, normalize


def _sidecar(i: int, level: str) -> dict:
    return {
        "index": i, "time_utc": "2026-09-25T12:00:00Z", "level": level, "continue": f"{level}-start",
        "jak_pos": [1.0, 2.0, 3.0], "cam_pos": [4.0, 5.0, 6.0],
        "cam_rows": [[1, 0, 0], [0, 1, 0], [0, 0, 1]], "fov": 64.0, "hour": 12.5,
        "mood_weights": {"sun": 0.75, "fog": 0.25}, "toggles": {"ssao": True, "fur": False},
        "build_sha": "0123456789abcdef", "status": "kept", "note": None, "tags": [], "annotations": [],
    }


def make_run(root: Path, n: int = 3) -> Path:
    run = root / "runs" / "20260925-1200-test"
    run.mkdir(parents=True)
    for i in range(1, n + 1):
        Image.new("RGB", (200, 100), (40 * i, 80, 120)).save(run / f"{i:04d}.png")
        (run / f"{i:04d}.json").write_text(json.dumps(_sidecar(i, ["beach", "jungle", "village1"][i % 3])))
    return run


@pytest.fixture
def run(tmp_path: Path) -> Run:
    return Run(make_run(tmp_path))


def _raw(run: Run, i: int, folder: str = "") -> dict:
    return json.loads((run.root / folder / f"{i:04d}.json").read_text())


def test_normalize_fills_triage_fields_and_keeps_others():
    s = normalize({"level": "beach", "tags": ["performance", "bogus"], "note": "  "})
    assert s == {"level": "beach", "status": "kept", "note": None, "tags": ["performance"],
                 "annotations": []}


def test_json_round_trip_keeps_capture_fields_and_backs_up_once(run: Run):
    original = (run.root / "0001.json").read_text()
    run.update(1, {"note": " fog breaks up ", "tags": ["game-developer", "performance"]})
    run.update(1, {"status": "not-for-review"})
    raw = _raw(run, 1)
    assert raw["note"] == "fog breaks up"
    assert raw["tags"] == ["performance", "game-developer"]
    assert raw["status"] == "not-for-review"
    assert raw["cam_rows"] == [[1, 0, 0], [0, 1, 0], [0, 0, 1]] and raw["build_sha"] == "0123456789abcdef"
    assert (run.root / "0001.json.bak").read_text() == original
    assert not list(run.root.glob("*.tmp"))
    assert Run(run.root).snapshot(1)["note"] == "fog breaks up"


def test_update_rejects_bad_values(run: Run):
    with pytest.raises(TriageError):
        run.update(1, {"status": "deleted"})
    with pytest.raises(TriageError):
        run.update(1, {"tags": ["fog"]})
    with pytest.raises(TriageError):
        run.update(9, {"note": "x"})


def test_delete_moves_files_and_undo_restores_previous_status(run: Run):
    run.update(2, {"status": "not-for-review"})
    snap = run.delete(2)
    assert snap["status"] == "deleted"
    assert not (run.root / "0002.png").exists() and not (run.root / "0002.json").exists()
    assert (run.root / "deleted" / "0002.png").is_file()
    assert _raw(run, 2, "deleted")["status"] == "deleted"
    assert [s["status"] for s in run.snapshots()] == ["kept", "deleted", "kept"]
    with pytest.raises(TriageError):
        run.update(2, {"note": "x"})
    restored = run.undo()
    assert restored["index"] == 2 and restored["status"] == "not-for-review"
    assert (run.root / "0002.png").is_file() and not (run.root / "deleted" / "0002.png").exists()
    assert run.undo() is None


def test_restore_without_undo_defaults_to_kept(run: Run):
    run.delete(3)
    assert run.restore(3)["status"] == "kept"
    assert _raw(run, 3)["status"] == "kept"


def test_crop_box_adds_margin_and_clamps():
    assert crop_box([50, 20, 100, 40], (200, 100)) == (40, 16, 160, 64)
    assert crop_box([0, 0, 200, 100], (200, 100)) == (0, 0, 200, 100)


def test_annotations_write_crops_and_delete_undo(run: Run):
    snap = run.add_annotation(1, [50, 20, 100, 40], "black pixels", "game-developer")
    ann = snap["annotations"][0]
    assert ann["id"] == "a1" and ann["rect"] == [50, 20, 100, 40] and ann["created_utc"]
    crop = run.root / "crops" / "0001-a1.png"
    with Image.open(crop) as im:
        assert im.size == (120, 48)
    snap = run.update_annotation(1, "a1", {"rect": [-10, -10, 30, 30], "facet": None, "text": "seam"})
    assert snap["annotations"][0]["rect"] == [0, 0, 20, 20]
    assert snap["annotations"][0]["facet"] is None
    with pytest.raises(TriageError):
        run.update_annotation(1, "a1", {"facet": "fog"})
    run.delete_annotation(1, "a1")
    assert not crop.exists() and len(list((run.root / "crops" / "removed").iterdir())) == 1
    assert run.undo()["annotations"][0]["text"] == "seam"
    assert crop.is_file()


def test_csv_export_lists_kept_snapshots_and_annotations(run: Run):
    run.update(1, {"note": "too gaudy", "tags": ["artistic-integrity", "remaster-fanatic"]})
    run.add_annotation(1, [10, 10, 20, 20], "bronze, 20% weaker", "artistic-integrity")
    run.update(2, {"status": "not-for-review", "note": "accident"})
    run.delete(3)
    with open(run.root / "triage.csv", newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    assert rows == [
        {"index": "0001", "annotation": "", "level": "jungle",
         "tags": "artistic-integrity;remaster-fanatic", "note": "too gaudy", "png": "0001.png", "crop": ""},
        {"index": "0001", "annotation": "a1", "level": "jungle", "tags": "artistic-integrity",
         "note": "bronze, 20% weaker", "png": "0001.png", "crop": "crops/0001-a1.png"},
    ]


def test_summary_counts_open_items_by_facet(run: Run):
    run.update(1, {"tags": ["performance"]})
    run.add_annotation(1, [0, 0, 10, 10], "", "performance")
    run.add_annotation(2, [0, 0, 10, 10], "", "game-developer")
    run.update(3, {"status": "not-for-review", "tags": ["performance"]})
    s = run.summary()
    assert s["counts"] == {"kept": 2, "not-for-review": 1, "deleted": 0}
    assert s["facets"] == {"performance": 1, "artistic-integrity": 0, "remaster-fanatic": 0,
                           "game-developer": 1}
    assert caption(s).startswith("Open: Performance 1 · Artistic integrity 0")


@pytest.mark.skipif(hub is None, reason="review dashboard not in this tree")
def test_dashboard_caption_follows_the_run(tmp_path: Path):
    r = Run(make_run(tmp_path))
    manifest = tmp_path / "hub" / "features.toml"
    manifest.parent.mkdir()
    feats = []
    hub.add_round(feats, "critic-run", hub.Round("http://127.0.0.1:7895/", "old", "needs-look",
                                                 "runs/20260925-1200-test"))
    hub.save(manifest, feats)
    assert server.sync_hub_caption(r, "Open: x", tmp_path)
    assert hub.load(manifest)[0].now.caption == "Open: x"
    assert not server.sync_hub_caption(r, "Open: x", tmp_path)


@pytest.mark.skipif(hub is None, reason="review dashboard not in this tree")
def test_dashboard_up_starts_triage_for_runs(tmp_path: Path, monkeypatch):
    make_run(tmp_path)
    manifest = tmp_path / "hub" / "features.toml"
    manifest.parent.mkdir()
    feats = []
    hub.add_round(feats, "critic-run", hub.Round("http://127.0.0.1:7895/", "", "needs-look",
                                                 "runs/20260925-1200-test"))
    hub.save(manifest, feats)
    spawned = []
    monkeypatch.setattr(hub, "is_listening", lambda port, host="127.0.0.1": False)
    monkeypatch.setattr(hub, "_spawn", lambda args, log: spawned.append(args))
    assert hub.up(tmp_path) == ["critic-run :7895"]
    assert spawned[0][1:] == ["-m", "sandover.triage", str(tmp_path / "runs/20260925-1200-test"),
                              "--port", "7895"]
    assert "0001.png" in hub.render(tmp_path, hub.load(manifest))


def _serve(run: Run):
    srv = server.make_server(run, 0, hub=False)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://127.0.0.1:{srv.server_address[1]}"


def _post(url: str, body: dict | None = None):
    req = urllib.request.Request(url, data=json.dumps(body or {}).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read())


def test_server_api(run: Run):
    srv, base = _serve(run)
    try:
        with urllib.request.urlopen(base + "/api/run") as r:
            data = json.loads(r.read())
        assert data["name"] == "20260925-1200-test" and data["facets"] == list(FACETS)
        assert [s["index"] for s in data["snapshots"]] == [1, 2, 3]
        with urllib.request.urlopen(base + "/thumb/0001.jpg") as r:
            assert r.headers["Content-Type"] == "image/jpeg"
        with urllib.request.urlopen(base + "/") as r:
            assert b"Critic triage" in r.read()
        out = _post(base + "/api/snap/1", {"tags": ["performance"]})
        assert out["snapshot"]["tags"] == ["performance"] and out["summary"]["facets"]["performance"] == 1
        out = _post(base + "/api/snap/1/annotations", {"rect": [1, 1, 30, 30], "text": "t"})
        assert out["snapshot"]["annotations"][0]["id"] == "a1"
        _post(base + "/api/snap/1/annotations/a1/delete")
        assert _post(base + "/api/undo")["snapshot"]["annotations"][0]["id"] == "a1"
        assert _post(base + "/api/snap/2/delete")["summary"]["counts"]["deleted"] == 1
        with urllib.request.urlopen(base + "/img/0002.png") as r:
            assert r.read(4) == b"\x89PNG"
        with pytest.raises(urllib.error.HTTPError) as err:
            _post(base + "/api/snap/1", {"status": "maybe"})
        assert err.value.code == 400
        with pytest.raises(urllib.error.HTTPError) as err:
            urllib.request.urlopen(base + "/static/../store.py")
        assert err.value.code == 404
    finally:
        srv.shutdown()
