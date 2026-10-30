from pathlib import Path

import numpy as np
from PIL import Image

from sandover.report import SLIDER_CSS, WIPE_JS_HELPERS, band_for, collect_shots, render_report


def test_band_for():
    assert band_for(0.01, threshold=0.02, near_multiplier=1.5) == "pass"
    assert band_for(0.02, threshold=0.02, near_multiplier=1.5) == "pass"
    assert band_for(0.025, threshold=0.02, near_multiplier=1.5) == "near"
    assert band_for(0.05, threshold=0.02, near_multiplier=1.5) == "fail"
    assert band_for(0.9, threshold=None, near_multiplier=1.5) == "uncalibrated"


def _write_png(path: Path, arr: np.ndarray) -> None:
    Image.fromarray(arr, mode="RGB").save(path)


def _make_shots(tmp_path: Path) -> tuple[Path, Path]:
    rng = np.random.default_rng(0)
    vanilla_dir, candidate_dir = tmp_path / "vanilla", tmp_path / "candidate"
    vanilla_dir.mkdir()
    candidate_dir.mkdir()

    base = (rng.random((24, 24, 3)) * 255).astype("uint8")
    _write_png(vanilla_dir / "quiet-shot.png", base)
    _write_png(candidate_dir / "quiet-shot.png", base)  # identical -> near-zero FLIP

    changed = base.copy()
    changed[:12, :12] = 255 - changed[:12, :12]
    _write_png(vanilla_dir / "loud-shot.png", base)
    _write_png(candidate_dir / "loud-shot.png", changed)  # big local change -> high FLIP

    # a candidate PNG with no matching vanilla is ignored
    _write_png(candidate_dir / "orphan-shot.png", base)

    return vanilla_dir, candidate_dir


def test_collect_shots_matches_by_stem_and_sorts_worst_first(tmp_path: Path):
    vanilla_dir, candidate_dir = _make_shots(tmp_path)
    shots = collect_shots(vanilla_dir, candidate_dir, threshold=0.05, near_multiplier=1.5)

    assert [s.name for s in shots] == ["loud-shot", "quiet-shot"]
    assert shots[0].flip_mean > shots[1].flip_mean
    assert shots[1].band == "pass"
    assert shots[0].band in ("near", "fail")


def test_render_report_is_self_contained(tmp_path: Path):
    vanilla_dir, candidate_dir = _make_shots(tmp_path)
    shots = collect_shots(vanilla_dir, candidate_dir, threshold=0.05, near_multiplier=1.5)

    html = render_report(shots, preset="Test Preset", commit="deadbeef",
                          threshold=0.05, noise_floor=0.001)

    assert "data:image/jpeg;base64," in html
    assert "Test Preset" in html
    assert "loud-shot" in html and "quiet-shot" in html
    # no external network references anywhere in the page
    assert "http://" not in html and "https://" not in html


def test_render_report_uncalibrated_note_without_threshold(tmp_path: Path):
    vanilla_dir, candidate_dir = _make_shots(tmp_path)
    shots = collect_shots(vanilla_dir, candidate_dir, threshold=None, near_multiplier=1.5)
    assert all(s.band == "uncalibrated" for s in shots)

    html = render_report(shots, preset=None, commit="unknown", threshold=None, noise_floor=None)
    assert "not calibrated" in html


def test_render_report_reuses_the_shared_slider_component(tmp_path: Path):
    """The FLIP slider and the compare view (sandover.compare) share one CSS/JS."""
    vanilla_dir, candidate_dir = _make_shots(tmp_path)
    shots = collect_shots(vanilla_dir, candidate_dir, threshold=0.05, near_multiplier=1.5)
    html = render_report(shots, preset=None, commit="unknown", threshold=None, noise_floor=None)

    assert SLIDER_CSS.strip() in html
    assert "function setupWipe(sl)" in WIPE_JS_HELPERS
    assert WIPE_JS_HELPERS.strip() in html
