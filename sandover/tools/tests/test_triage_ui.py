"""Browser walk-through of ``sandover-triage``: open, tag, note, crop, 1:1, flag, delete, undo, reload.

Needs Playwright and its Chromium (skipped otherwise). ``SANDOVER_TRIAGE_RUN`` copies a real
run instead of the synthetic one; ``SANDOVER_TRIAGE_SHOTS`` keeps the screenshots.
"""

import json
import os
import shutil
import threading
from pathlib import Path

import pytest
from PIL import Image

from sandover.triage import server
from sandover.triage.store import Run

sync_api = pytest.importorskip("playwright.sync_api")


def _run_dir(tmp_path: Path) -> Path:
    src = os.environ.get("SANDOVER_TRIAGE_RUN")
    dst = tmp_path / "runs" / "20260925-1200-walk"
    if src:
        shutil.copytree(src, dst, ignore=shutil.ignore_patterns("deleted", "crops", "*.bak", "triage.csv"))
        for p in dst.glob("*.json"):
            d = json.loads(p.read_text(encoding="utf-8"))
            d.update(status="kept", note=None, tags=[], annotations=[])
            p.write_text(json.dumps(d), encoding="utf-8")
        return dst
    dst.mkdir(parents=True)
    for i, level in enumerate(["beach", "jungle", "village1"], 1):
        im = Image.linear_gradient("L").resize((1920, 1080)).convert("RGB")
        im.paste((60 * i, 90, 140), (200, 200, 600, 500))
        im.save(dst / f"{i:04d}.png")
        (dst / f"{i:04d}.json").write_text(json.dumps({
            "index": i, "level": level, "hour": 12.0, "status": "kept", "note": None,
            "tags": [], "annotations": [], "toggles": {"ssao": True}}), encoding="utf-8")
    return dst


def _side(run: Path, i: int, folder: str = "") -> dict:
    return json.loads((run / folder / f"{i:04d}.json").read_text(encoding="utf-8"))


def test_walkthrough(tmp_path: Path):
    run_dir = _run_dir(tmp_path)
    shots = Path(os.environ.get("SANDOVER_TRIAGE_SHOTS") or tmp_path / "shots")
    shots.mkdir(parents=True, exist_ok=True)
    srv = server.make_server(Run(run_dir), 0, hub=False)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{srv.server_address[1]}/"

    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:  # noqa: BLE001
            pytest.skip(f"no Chromium for Playwright: {exc}")
        page = browser.new_page(viewport={"width": 1600, "height": 1000}, color_scheme="dark")

        def settle():
            page.wait_for_function("() => !document.getElementById('save').classList.contains('pending')")
            page.wait_for_timeout(150)

        def snap(name):
            page.wait_for_timeout(250)
            page.screenshot(path=str(shots / f"{name}.png"))

        page.goto(url)
        page.wait_for_selector(".tile")
        page.wait_for_function("() => document.getElementById('img').naturalWidth > 0")
        assert page.locator(".tile").count() == 3
        assert page.locator("#snapIndex").inner_text() == "#0001"
        snap("01-open")

        page.keyboard.press("4")
        page.keyboard.press("n")
        page.keyboard.type("Fog breaks up into bands over the water")
        page.keyboard.press("Enter")
        settle()
        side = _side(run_dir, 1)
        assert side["tags"] == ["game-developer"]
        assert side["note"] == "Fog breaks up into bands over the water"
        snap("02-tagged-and-noted")

        page.keyboard.press("c")
        box = page.locator("#stage").bounding_box()
        x0, y0 = box["x"] + box["width"] * 0.30, box["y"] + box["height"] * 0.30
        page.mouse.move(x0, y0)
        page.mouse.down()
        page.mouse.move(x0 + 220, y0 + 140, steps=6)
        page.mouse.up()
        page.keyboard.type("Too gaudy: this bronze wants to be 20% weaker")
        page.keyboard.press("Enter")
        settle()
        page.keyboard.press("2")
        settle()
        ann = _side(run_dir, 1)["annotations"]
        assert len(ann) == 1 and ann[0]["facet"] == "artistic-integrity"
        assert ann[0]["rect"][2] > 50 and (run_dir / "crops" / "0001-a1.png").is_file()
        assert page.locator("#overlay .ann").count() == 1
        snap("03-crop-annotated")

        page.mouse.move(x0 + 100, y0 + 60)
        page.keyboard.press("z")
        assert page.locator("#zoomTag").inner_text() == "1:1"
        page.mouse.move(box["x"] + 400, box["y"] + 400)
        page.mouse.down()
        page.mouse.move(box["x"] + 300, box["y"] + 330, steps=4)
        page.mouse.up()
        snap("04-one-to-one-panned")
        page.keyboard.press("z")
        page.keyboard.press("Escape")

        page.keyboard.press("j")
        assert page.locator("#snapIndex").inner_text() == "#0002"
        page.keyboard.press("x")
        settle()
        assert _side(run_dir, 2)["status"] == "not-for-review"
        assert page.locator("#snapIndex").inner_text() == "#0003"

        page.keyboard.press("d")
        settle()
        assert not (run_dir / "0003.png").exists()
        assert _side(run_dir, 3, "deleted")["status"] == "deleted"
        snap("05-flagged-and-deleted")

        page.keyboard.press("u")
        settle()
        page.wait_for_function("() => document.getElementById('snapIndex').textContent === '#0003'")
        assert (run_dir / "0003.png").is_file() and _side(run_dir, 3)["status"] == "kept"
        snap("06-undo-delete")

        page.reload()
        page.wait_for_selector(".tile")
        page.wait_for_function("() => document.getElementById('img').naturalWidth > 0")
        assert page.locator("#note").input_value() == "Fog breaks up into bands over the water"
        assert page.locator(".tile.not-for-review").count() == 1
        assert page.locator("#facetCounts").inner_text().count("1") >= 2
        snap("07-after-reload")

        page.keyboard.press("f")
        assert "Kept" in page.locator("#filterLabel").inner_text()
        assert page.locator(".tile:not(.filtered)").count() == 2
        snap("08-filter-kept")
        browser.close()
    srv.shutdown()

    rows = (run_dir / "triage.csv").read_text(encoding="utf-8").splitlines()
    assert len(rows) == 1 + 3  # header, #1, its crop, #3
