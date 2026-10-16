import json
import urllib.parse
from pathlib import Path

from sandover.compare import (
    COMPARE_JS_ON_LOAD,
    apply_compare_event,
    assign_side,
    compare_html,
    default_sides,
    file_url,
    key_to_side,
    keymap_html,
    legend_html,
    side_caption,
    side_choices,
    side_display,
    side_image_path,
    side_label,
    side_options,
    step_shot,
    toggle_active,
)
from sandover.rounds import RoundManifest


def _make_manifest(tmp_path: Path, *, with_vanilla: bool = True) -> RoundManifest:
    round_dir = tmp_path / "round-001"
    round_dir.mkdir()
    manifest = {
        "schema": 1, "phase": "p", "round": 1, "pick_set": "s",
        "shots": ["shot-a", "shot-b"],
        "candidates": {
            "A": {"preset": "f", "role": "anchor", "label": "F"},
            "B": {"preset": "c", "role": "candidate", "label": "C"},
            "C": {"preset": "d", "role": "candidate", "label": "D"},
        },
        "images": {
            "A": {"shot-a": "a1.png", "shot-b": "a2.png"},
            "B": {"shot-a": "b1.png", "shot-b": "b2.png"},
            "C": {"shot-a": "c1.png", "shot-b": "c2.png"},
        },
    }
    if with_vanilla:
        manifest["vanilla"] = {"shot-a": "v1.png", "shot-b": "v2.png"}
    (round_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return RoundManifest.load(round_dir)


def test_side_options_includes_vanilla_first(tmp_path: Path):
    m = _make_manifest(tmp_path)
    assert side_options(m) == ["vanilla", "A", "B", "C"]


def test_side_options_without_vanilla(tmp_path: Path):
    m = _make_manifest(tmp_path, with_vanilla=False)
    assert side_options(m) == ["A", "B", "C"]


def test_default_sides_is_vanilla_vs_anchor(tmp_path: Path):
    m = _make_manifest(tmp_path)
    assert default_sides(m) == ("vanilla", "A")


def test_default_sides_without_vanilla_falls_back_to_first_two(tmp_path: Path):
    m = _make_manifest(tmp_path, with_vanilla=False)
    # A is both the first option and the anchor, so the right side is the next letter.
    assert default_sides(m) == ("A", "B")


def test_key_to_side_maps_v_and_candidate_letters():
    opts = ["vanilla", "A", "B", "C"]
    assert key_to_side("v", opts) == "vanilla"
    assert key_to_side("V", opts) == "vanilla"
    assert key_to_side("b", opts) == "B"
    assert key_to_side("C", opts) == "C"


def test_key_to_side_ignores_other_keys():
    opts = ["vanilla", "A", "B", "C"]
    assert key_to_side("D", opts) is None  # not a candidate in this round
    assert key_to_side("Tab", opts) is None
    assert key_to_side("]", opts) is None
    assert key_to_side("v", ["A", "B"]) is None  # no vanilla in the round


def test_toggle_active_flips_between_panes():
    assert toggle_active("right") == "left"
    assert toggle_active("left") == "right"


def test_assign_side_only_touches_the_active_pane():
    assert assign_side("vanilla", "A", "right", "D") == ("vanilla", "D")
    assert assign_side("vanilla", "A", "left", "C") == ("C", "A")


def test_step_shot_wraps_both_ways():
    assert step_shot(0, 4, -1) == 3
    assert step_shot(3, 4, 1) == 0
    assert step_shot(1, 4, 1) == 2


def test_apply_compare_event_takes_the_page_state():
    opts = ["vanilla", "A", "B"]
    assert apply_compare_event(opts, 4, 0, "vanilla", "A", 0,
                               {"idx": 2, "left": "B", "right": "vanilla", "seq": 1}) == (2, "B", "vanilla", 1)


def test_apply_compare_event_drops_stale_events():
    opts = ["vanilla", "A", "B"]
    assert apply_compare_event(opts, 4, 0, "vanilla", "A", 5, {"idx": 1, "seq": 5}) is None
    assert apply_compare_event(opts, 4, 0, "vanilla", "A", 5, {"idx": 1, "seq": 3}) is None


def test_apply_compare_event_rejects_unknown_sides_and_bad_index():
    opts = ["vanilla", "A", "B"]
    got = apply_compare_event(opts, 4, 1, "vanilla", "A", 0,
                              {"idx": "x", "left": "Z", "right": "../etc", "seq": 2})
    assert got == (1, "vanilla", "A", 2)
    assert apply_compare_event(opts, 4, 1, "vanilla", "A", 0, {"idx": 9, "seq": 1})[0] == 1
    assert apply_compare_event(opts, 4, 1, "vanilla", "A", 3, None) == (1, "vanilla", "A", 3)


def test_side_caption_and_display(tmp_path: Path):
    m = _make_manifest(tmp_path)
    m.candidates["B"].label = "softer corners"
    assert side_caption(m, "vanilla") == ""
    assert side_caption(m, "B") == "softer corners"
    assert side_display(m, "B") == "B — softer corners"
    assert side_display(m, "vanilla") == "vanilla"
    assert side_choices(m)[0] == ("vanilla", "vanilla")
    assert [v for _, v in side_choices(m)] == ["vanilla", "A", "B", "C"]


def test_side_caption_never_shows_a_preset_name_or_bare_faithful(tmp_path: Path):
    m = _make_manifest(tmp_path)
    m.candidates["B"].label = "c"  # equals the preset name
    m.candidates["A"].label = "Faithful"
    assert side_caption(m, "B") == ""
    assert side_caption(m, "A") == ""
    assert side_display(m, "A") == "A (faithful)"


def test_legend_and_keymap_list_every_side_and_key(tmp_path: Path):
    m = _make_manifest(tmp_path)
    legend = legend_html(m)
    for side in ("vanilla", "A", "B", "C"):
        assert f'data-side="{side}"' in legend
    assert "(faithful)" in legend
    keys = keymap_html(m)
    for token in ("<kbd>V</kbd>", "<kbd>A</kbd>–<kbd>C</kbd>", "Tab", "<kbd>[</kbd>", "<kbd>]</kbd>",
                  "Space", "Enter", "zoom"):
        assert token in keys


def test_side_image_path_resolves_vanilla_and_letters(tmp_path: Path):
    m = _make_manifest(tmp_path)
    assert side_image_path(m, "vanilla", "shot-a") == m.dir / "v1.png"
    assert side_image_path(m, "B", "shot-b") == m.dir / "b2.png"


def test_side_label_marks_the_anchor(tmp_path: Path):
    m = _make_manifest(tmp_path)
    assert side_label(m, "vanilla") == "vanilla"
    assert side_label(m, "A") == "A (faithful)"
    assert side_label(m, "B") == "B"


def test_file_url_is_same_origin_and_encodes_the_path(tmp_path: Path):
    path = tmp_path / "a dir" / "shot.png"
    url = file_url(path)
    assert url.startswith("/gradio_api/file=")
    assert "http://" not in url and "https://" not in url
    assert "%20" in url  # the space in "a dir"


def test_compare_html_has_no_external_references(tmp_path: Path):
    m = _make_manifest(tmp_path)
    out = compare_html(m, "shot-a", "vanilla", "A")
    for banned in ("http://", "https://", "fonts.googleapis", "fonts.gstatic"):
        assert banned not in out
    assert "/gradio_api/file=" in out
    assert "vanilla" in out
    assert "A (faithful)" in out


def test_compare_html_has_no_script_tag(tmp_path: Path):
    """gr.HTML never executes a <script> from innerHTML; the wiring lives in js_on_load."""
    m = _make_manifest(tmp_path)
    out = compare_html(m, "shot-a", "vanilla", "A")
    assert "<script" not in out.lower()


def test_compare_html_documents_the_shortcuts(tmp_path: Path):
    m = _make_manifest(tmp_path)
    out = compare_html(m, "shot-a", "vanilla", "A")
    assert "shot" in out.lower()
    assert "flicker" in out.lower()
    assert "zoom" in out.lower() or "+/-" in out


def test_compare_js_on_load_has_no_external_references():
    for banned in ("http://", "https://", "fonts.googleapis", "fonts.gstatic"):
        assert banned not in COMPARE_JS_ON_LOAD


def test_compare_js_on_load_wires_all_documented_shortcuts():
    for key in ('"ArrowLeft"', '"ArrowRight"', '"["', '"]"', '"Tab"', '"Enter"', '" "', '"+"',
                '"-"', '"0"', '"1"', '"Escape"', '"wheel"', '"v"', "toUpperCase"):
        assert key in COMPARE_JS_ON_LOAD


def test_compare_js_on_load_actual_size_uses_natural_pixels_not_the_stretched_frame():
    """The 1:1 jump must be computed from the image's own naturalWidth/devicePixelRatio,
    not from zoomBy's 1-4x range, which can't reach below an already-oversized fit frame."""
    assert "naturalWidth" in COMPARE_JS_ON_LOAD
    assert "devicePixelRatio" in COMPARE_JS_ON_LOAD
    assert "function actualSize" in COMPARE_JS_ON_LOAD


def test_compare_html_documents_and_wires_the_actual_size_button(tmp_path: Path):
    m = _make_manifest(tmp_path)
    out = compare_html(m, "shot-a", "vanilla", "A")
    assert "cmp-actual-btn" in out
    assert "1:1" in out
    assert "actual size" in keymap_html(m).lower()


def _url_to_path(url: str) -> Path:
    assert url.startswith("/gradio_api/file=")
    return Path(urllib.parse.unquote(url[len("/gradio_api/file="):]))


def _extract_attr(out: str, name: str) -> str:
    import html as html_mod

    marker = f'{name}="'
    start = out.index(marker) + len(marker)
    end = out.index('"', start)
    return html_mod.unescape(out[start:end])


def test_main_view_img_tags_serve_the_source_png_bytes_unmodified(tmp_path: Path):
    """No thumbnail/resize/re-encode step for the initially-shown left/right panes -- the
    src each <img> tag gets must resolve to the manifest's own file, byte for byte."""
    m = _make_manifest(tmp_path)
    vanilla_bytes = b"\x89PNG\r\n\x1a\n-VANILLA-SHOT-A-NATIVE-RES-NOT-A-COPY"
    a_bytes = b"\x89PNG\r\n\x1a\n-CANDIDATE-A-SHOT-A-NATIVE-RES-NOT-A-COPY"
    m.vanilla_path("shot-a").write_bytes(vanilla_bytes)
    m.image_path("A", "shot-a").write_bytes(a_bytes)

    out = compare_html(m, "shot-a", "vanilla", "A")
    left_src = out.split('class="left" src="', 1)[1].split('"', 1)[0]
    right_src = out.split('class="right" src="', 1)[1].split('"', 1)[0]

    assert _url_to_path(left_src) == m.vanilla_path("shot-a")
    assert _url_to_path(left_src).read_bytes() == vanilla_bytes
    assert _url_to_path(right_src) == m.image_path("A", "shot-a")
    assert _url_to_path(right_src).read_bytes() == a_bytes


def test_compare_html_data_shots_serves_every_side_and_shot_unmodified(tmp_path: Path):
    """Same guarantee for every shot/side reachable via key presses (the data-shots blob
    the client switches between with no server round trip) -- always the source PNG."""
    m = _make_manifest(tmp_path)
    written = {}
    for letter in m.candidates:
        for shot in m.shots:
            p = m.image_path(letter, shot)
            data = f"PNGDATA-{letter}-{shot}".encode()
            p.write_bytes(data)
            written[p] = data
    for shot in m.vanilla:
        p = m.vanilla_path(shot)
        data = f"PNGDATA-vanilla-{shot}".encode()
        p.write_bytes(data)
        written[p] = data

    out = compare_html(m, "shot-a", "vanilla", "A")
    shots_json = json.loads(_extract_attr(out, "data-shots"))
    assert shots_json  # sanity: the round actually has shots
    for entry in shots_json:
        for url in entry["urls"].values():
            path = _url_to_path(url)
            assert path.read_bytes() == written[path]


def test_compare_html_carries_every_shot_and_side(tmp_path: Path):
    m = _make_manifest(tmp_path)
    out = compare_html(m, "shot-b", "vanilla", "B", "right", nonce=7)
    assert 'data-idx="1"' in out and 'data-render="7"' in out
    assert 'data-active="right"' in out
    for name in ("v1.png", "v2.png", "a1.png", "b2.png", "c2.png"):
        assert name in out
    assert "shot 2 of 2: shot-b" in out
